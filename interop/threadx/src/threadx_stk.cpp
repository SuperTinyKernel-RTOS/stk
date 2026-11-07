/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "stk.h"
#include "sync/stk_sync.h"
#include "memory/stk_memory.h"
#include "time/stk_time.h"

// See design notes, API coverage and the list of differences from native ThreadX in README.md.

#include "tx_api.h"

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// File-local variables
// ----------------------------------------------------------------------------

volatile bool g_IntDisabled = false; // posture emulated for tx_interrupt_control()
int64_t g_TimeOffset = 0; // tx_time_set() adjustment

// ----------------------------------------------------------------------------
// Kernel configuration
//
// sched::SwitchStrategyPreemptionThreshold<TX_MAX_PRIORITIES, true> = ThreadX scheduling model: fixed priority +
// preemption-threshold + per-thread time-slice, up to 1024 priority levels. STK numbers priorities in the opposite
// direction (higher number = more important), the conversion ((TX_MAX_PRIORITIES - 1) - tx_priority) is done by
// Strategy::FromThreadX().
// ----------------------------------------------------------------------------

STK_STATIC_ASSERT_DESC((TX_MAX_PRIORITIES >= 1U) && (TX_MAX_PRIORITIES <= 1024U),
    "TX_MAX_PRIORITIES must be in [1, 1024] (limit of the ready bitmap of sched::SwitchStrategyPreemptionThreshold)");

typedef stk::sched::SwitchStrategyPreemptionThreshold<TX_MAX_PRIORITIES, true> StkStrategy;

// KERNEL_TELEMETRY: IKernel::GetTaskState() is used for the exact thread states (tx_thread_info_get(),
// tx_thread_suspend(), tx_thread_wait_abort())
typedef stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC | stk::KERNEL_TIMESLICE | stk::KERNEL_TELEMETRY
#if STK_TICKLESS_IDLE
    | stk::KERNEL_TICKLESS
#endif
    , (TX_STK_MAX_THREADS + stk::time::TimerHost::TASK_COUNT), // + tick and handler tasks of the timer host
    StkStrategy, stk::PlatformDefault> StkKernel;

static StkKernel g_StkKernel;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

class StkThread;

// 64-bit flags word: the 32 ThreadX flags live in the low half, the most significant bit (63) is the error marker
// reserved by stk::sync::EventFlagsT, so bit 31 is a normal flag as in ThreadX (EventFlags with a 32-bit word has
// only 31 usable bits).
typedef stk::sync::EventFlagsT<uint64_t> StkEventFlags;

typedef stk::time::TimerHost StkTimerHost;

StkTimerHost g_TimerHost; // software timers (see "Application timers"), its tasks are not TX_THREADs

// True for the tick and handler tasks of the timer host.
__stk_forceinline bool TimerTask(stk::TId tid)
{
    return g_TimerHost.IsHostTid(tid);
}

__stk_forceinline bool InIsr()
{
    return stk::hw::IsInsideISR();
}

__stk_forceinline bool KernelRunning()
{
    return (g_StkKernel.GetState() == stk::IKernel::KSTATE_RUNNING);
}

__stk_forceinline bool KernelInitialized()
{
    return (g_StkKernel.GetState() != stk::IKernel::KSTATE_INACTIVE);
}

// ThreadX wait option (ticks, 0xFFFFFFFF = forever) -> STK timeout.
__stk_forceinline stk::Timeout ToStkTimeout(ULONG wait_option)
{
    stk::Timeout result;

    if (wait_option == TX_NO_WAIT)
    {
        result = stk::NO_WAIT;
    }
    else if (wait_option >= static_cast<ULONG>(stk::WAIT_INFINITE))
    {
        result = stk::WAIT_INFINITE;
    }
    else
    {
        result = static_cast<stk::Timeout>(wait_option);
    }

    return result;
}

// ThreadX (priority, threshold) -> STK weight (encoded priority + preemption-threshold).
__stk_forceinline stk::Weight ToStkWeight(UINT priority, UINT threshold)
{
    return StkStrategy::FromThreadX(static_cast<uint16_t>(priority), static_cast<uint16_t>(threshold));
}

// Map the outcome of an STK wait to a ThreadX status; \a timeout_status is object-specific
// (TX_NOT_AVAILABLE, TX_NO_INSTANCE, TX_QUEUE_EMPTY, ...), it is returned for both NO_WAIT and an expired timeout.
__stk_forceinline UINT WaitResultToTx(stk::EWaitResult wr, UINT timeout_status)
{
    UINT result;

    switch (wr)
    {
    case stk::WAIT_RESULT_SIGNAL:
        result = TX_SUCCESS;
        break;

    case stk::WAIT_RESULT_TIMEOUT:
        result = timeout_status;
        break;

    case stk::WAIT_RESULT_CANCELED:
        result = TX_WAIT_ABORTED;
        break;

    case stk::WAIT_RESULT_FAIL:
    default:
        result = TX_WAIT_ERROR;
        break;
    }

    return result;
}

// ----------------------------------------------------------------------------
// Thread control block
//
// StkThread lives in TX_THREAD::tx_thread_stk_storage (placement-new). It is created by tx_thread_create(),
// but it is registered in the kernel only when the thread is started (TX_AUTO_START at creation, or the first
// tx_thread_resume() for TX_DONT_START). Registration state is tracked by m_registered/m_removed.
// ----------------------------------------------------------------------------

class StkThread final : public stk::ITask
{
    STK_NONCOPYABLE_CLASS(StkThread);

public:
    explicit StkThread(TX_THREAD *tx)
        : m_tx(tx), m_weight(ToStkWeight(tx->tx_thread_priority, tx->tx_thread_preempt_threshold)),
          m_time_slice(static_cast<stk::Timeout>(tx->tx_thread_time_slice)),
          m_registered(false), m_removed(true), m_wait_kind(TX_READY), m_abort(false)
    {}

    virtual ~StkThread()
    {}

    void Run() override
    {
        TX_THREAD *const tx = m_tx;

        tx->tx_thread_run_count = (tx->tx_thread_run_count + 1U);

        if (tx->tx_thread_entry_exit_notify != nullptr)
        {
            tx->tx_thread_entry_exit_notify(tx, TX_THREAD_ENTRY);
        }

        tx->tx_thread_entry(tx->tx_thread_entry_parameter);

        // returning from the entry function completes the thread (KERNEL_DYNAMIC removes the task itself)
        if (tx->tx_thread_state != TX_TERMINATED)
        {
            tx->tx_thread_state = TX_COMPLETED;

            if (tx->tx_thread_entry_exit_notify != nullptr)
            {
                tx->tx_thread_entry_exit_notify(tx, TX_THREAD_EXIT);
            }
        }
    }

    // Called by the kernel from the tick handler once the task has been removed.
    void OnExit() override
    {
        m_registered = false;
        m_removed    = true;
    }

    // ---- IStackMemory ----
    const stk::Word *GetStack() const override { return static_cast<const stk::Word *>(m_tx->tx_thread_stack_start); }
    size_t GetStackSize()       const override { return (static_cast<size_t>(m_tx->tx_thread_stack_size) / sizeof(stk::Word)); }

    // ---- ITask ----
    stk::EAccessMode GetAccessMode() const override { return stk::ACCESS_PRIVILEGED; }
    void OnDeadlineMissed(uint32_t) override        {}
    stk::Weight GetWeight()         const override  { return m_weight; }
    const char *GetTraceName()      const override  { return m_tx->tx_thread_name; }
    stk::Timeout GetTimeSlice()     const override  { return m_time_slice; }

    // ---- Members ----
    TX_THREAD           *m_tx;
    volatile stk::Weight m_weight;     // encoded priority + threshold (see ToStkWeight)
    stk::Timeout         m_time_slice; // time-slice quantum used when the task is bound to the kernel
    volatile bool        m_registered; // the task has been passed to the kernel (AddTask)
    volatile bool        m_removed;    // the kernel does not reference the task (never added, or OnExit() fired)
    volatile UINT        m_wait_kind;  // TX_*_SUSP/TX_EVENT_FLAG/... hint: which kind of object the thread blocks on (TX_READY = none)
    volatile bool        m_abort;      // tx_thread_wait_abort() hit a plain sleep
};

STK_STATIC_ASSERT_DESC(sizeof(StkThread) <= sizeof(static_cast<TX_THREAD *>(nullptr)->tx_thread_stk_storage),
    "TX_STK_THREAD_STORAGE_WORDS is too small for StkThread");
STK_STATIC_ASSERT_DESC(alignof(StkThread) <= alignof(ALIGN_TYPE),
    "StkThread requires stricter alignment than ALIGN_TYPE");

__stk_forceinline StkThread *ThreadOf(TX_THREAD *tx)
{
    return static_cast<StkThread *>(static_cast<void *>(tx->tx_thread_stk_storage));
}

__stk_forceinline bool IsValidThread(const TX_THREAD *tx)
{
    return ((tx != nullptr) && (tx->tx_thread_id == TX_THREAD_ID));
}

// Current TX thread, nullptr in ISR, before the kernel is running or inside the timer host tasks (expiration functions).
StkThread *CurrentThread()
{
    StkThread *th = nullptr;

    if (!InIsr() && KernelRunning())
    {
        const stk::TId tid = stk::GetTid();

        if ((tid != stk::TID_NONE) && !TimerTask(tid))
        {
            th = static_cast<StkThread *>(stk::GetUserTaskFromTid(tid));
        }
    }

    return th;
}

TX_THREAD *ThreadFromTid(stk::TId tid)
{
    TX_THREAD *tx = nullptr;

    if ((tid != stk::TID_NONE) && !TimerTask(tid) && !stk::IsIsrTid(tid))
    {
        tx = static_cast<StkThread *>(stk::GetUserTaskFromTid(tid))->m_tx;
    }

    return tx;
}

// Fills the "first suspended thread / suspended count" outputs of a *_info_get() call from the wait list of an STK
// synchronization object (ISyncObject::GetWaitList().GetSize()/GetFirstWaiterTid()). Both values are read in one critical
// section so that they are consistent with each other.
static void FillSuspended(const stk::ISyncObject &sobj, TX_THREAD **first_suspended, ULONG *suspended_count)
{
    size_t   count = 0U;
    stk::TId first = stk::TID_NONE;

    {
        const stk::sync::ScopedCriticalSection cs_;

        count = sobj.GetWaitList().GetSize();
        first = sobj.GetFirstWaiterTid();
    }

    if (suspended_count != nullptr) { *suspended_count = static_cast<ULONG>(count); }
    if (first_suspended != nullptr) { *first_suspended = ThreadFromTid(first); } // TID_NONE -> nullptr
}

// Fills the "first suspended thread / suspended count" outputs of a *_info_get() call from the wait list of
// stk::EventFlags.
static void FillSuspended(const StkEventFlags &wf, TX_THREAD **first_suspended, ULONG *suspended_count)
{
    size_t   count = 0U;
    stk::TId first = stk::TID_NONE;

    {
        const stk::sync::ScopedCriticalSection cs_;

        count = wf.GetWaitList().GetSize();
        first = wf.GetFirstWaiterTid();
    }

    if (suspended_count != nullptr) { *suspended_count = static_cast<ULONG>(count); }
    if (first_suspended != nullptr) { *first_suspended = ThreadFromTid(first); } // TID_NONE -> nullptr
}

// A blocking call is only valid in thread context (not ISR, not the timer thread, kernel running).
__stk_forceinline bool CanBlock(ULONG wait_option)
{
    return ((wait_option == TX_NO_WAIT) || (CurrentThread() != nullptr));
}

// Tells which kind of object the calling thread is about to block on (TX_QUEUE_SUSP, TX_MUTEX_SUSP, ...).
// This is only the "kind" hint: whether the thread is actually blocked is decided by the kernel
// (IKernel::GetTaskState(), see ThreadStateOf()), so there is no state to restore and nothing to race with.
class ScopedWaitKind
{
    STK_NONCOPYABLE_CLASS(ScopedWaitKind);

public:
    explicit ScopedWaitKind(UINT kind, ULONG wait_option)
        : m_th(nullptr)
    {
        if (wait_option != TX_NO_WAIT)
        {
            m_th = CurrentThread();
            if (m_th != nullptr)
            {
                m_th->m_wait_kind = kind;
            }
        }
    }

    ~ScopedWaitKind()
    {
        if (m_th != nullptr)
        {
            m_th->m_wait_kind = TX_READY;
        }
    }

private:
    StkThread *m_th;
};

// Effective ThreadX state of a thread.
//
// TX_THREAD::tx_thread_state holds only the lifecycle (TX_READY, TX_SUSPENDED for a thread not started yet,
// TX_COMPLETED, TX_TERMINATED). The blocked states (TX_SLEEP, TX_SUSPENDED by tx_thread_suspend(), TX_QUEUE_SUSP, ...) are
// derived from the kernel's own view, which is exact: it is not written by the waiting thread before it blocks and
// cannot be overwritten by a concurrent suspend/terminate/abort.
UINT ThreadStateOf(TX_THREAD *tx)
{
    UINT state = tx->tx_thread_state;

    if (state == TX_READY)
    {
        StkThread *const th = ThreadOf(tx);

        if (th->m_registered && !th->m_removed)
        {
            stk::TaskStateInfo info = { stk::TASK_STATE_REMOVED, nullptr, stk::WAIT_INFINITE };

            // fails only for a null task or without KERNEL_TELEMETRY: keep the lifecycle state then
            if (!g_StkKernel.GetTaskState(th, info))
            {
                info.state = stk::TASK_STATE_READY;
            }

            switch (info.state)
            {
            case stk::TASK_STATE_SLEEPING:
                state = TX_SLEEP;
                break;

            case stk::TASK_STATE_WAITING:
                state = ((th->m_wait_kind != TX_READY) ? th->m_wait_kind : static_cast<UINT>(TX_SLEEP));
                break;

            case stk::TASK_STATE_SUSPENDED:
                state = TX_SUSPENDED;
                break;

            case stk::TASK_STATE_EXITING:
                state = TX_COMPLETED;
                break;

            case stk::TASK_STATE_READY:
            case stk::TASK_STATE_REMOVED:
            default:
                break; // TX_READY
            }
        }
    }

    return state;
}

// Passes the thread to the kernel (first start, or restart after tx_thread_reset()).
UINT RegisterThread(StkThread *th)
{
    UINT result;

    // the state is published before the kernel gets the thread: the thread may run right after AddTask()
    th->m_removed    = false;
    th->m_registered = true;
    th->m_tx->tx_thread_state = TX_READY;

    // AddTask() is ISR-safe in KERNEL_DYNAMIC mode: the thread is added at once (no yield, no wait) and the
    // failure is returned to the caller (never asserted in KERNEL_DYNAMIC mode)
    switch (g_StkKernel.AddTask(th))
    {
    case stk::ADD_OK:
        result = TX_SUCCESS;
        break;
    case stk::ADD_FAIL_DUPLICATE: // the thread (or its stack) is already known to the kernel
        result = TX_THREAD_ERROR;
        break;
    default:                      // ADD_FAIL_POOL_FULL: out of kernel task slots, increase TX_STK_MAX_THREADS
        result = TX_START_ERROR;  // ADD_FAIL_INVALID:   kernel is not initialized
        break;
    }

    if (result != TX_SUCCESS)
    {
        // the kernel is not changed: roll back, the thread stays not started
        th->m_removed    = true;
        th->m_registered = false;
        th->m_tx->tx_thread_state = TX_SUSPENDED;
    }

    return result;
}

// Waits until the kernel has finished removing a terminated/completed thread (OnExit() fired).
bool WaitThreadRemoved(const StkThread *th)
{
    // removal is processed by the kernel tick, a few ticks at most
    for (uint32_t attempts = 0U; (!th->m_removed && (attempts < 1000U)); ++attempts)
    {
        if (!KernelRunning() || InIsr())
        {
            break;
        }

        stk::Sleep(1);
    }

    return th->m_removed;
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

// ============================================================================
// ==== Kernel / system ====
// ============================================================================

VOID tx_kernel_enter(VOID)
{
    g_StkKernel.Initialize(static_cast<uint32_t>(1000000UL / TX_TIMER_TICKS_PER_SECOND));

    // ThreadX passes the first unused memory address here. STK works with caller-supplied memory only.
    tx_application_define(nullptr);

    // does not return for KERNEL_STATIC, for KERNEL_DYNAMIC returns when all threads have exited
    g_StkKernel.Start();
}

UINT tx_interrupt_control(UINT new_posture)
{
    // absolute posture semantics: disable once, enable once, return the previous posture
    const UINT old_posture = (g_IntDisabled ? TX_INT_DISABLE : TX_INT_ENABLE);

    if ((new_posture == TX_INT_DISABLE) && !g_IntDisabled)
    {
        stk::hw::CriticalSection::Enter();
        g_IntDisabled = true;
    }
    else if ((new_posture != TX_INT_DISABLE) && g_IntDisabled)
    {
        g_IntDisabled = false;
        stk::hw::CriticalSection::Exit();
    }
    else
    {
        // noop: already in the requested posture
    }

    return old_posture;
}

ULONG tx_time_get(VOID)
{
    ULONG result = 0U;

    if (KernelInitialized())
    {
        result = static_cast<ULONG>(static_cast<int64_t>(stk::GetTicks()) + g_TimeOffset);
    }

    return result;
}

VOID tx_time_set(ULONG new_time)
{
    if (KernelInitialized())
    {
        g_TimeOffset = (static_cast<int64_t>(new_time) - static_cast<int64_t>(stk::GetTicks()));
    }
}

// ============================================================================
// ==== Thread ====
// ============================================================================

UINT tx_thread_create(TX_THREAD *thread_ptr, TX_NAME_CONST CHAR *name_ptr,
                      VOID (*entry_function)(ULONG entry_input), ULONG entry_input,
                      VOID *stack_start, ULONG stack_size,
                      UINT priority, UINT preempt_threshold,
                      ULONG time_slice, UINT auto_start)
{
    UINT result;

    if (thread_ptr == nullptr)
    {
        result = TX_THREAD_ERROR;
    }
    else if (thread_ptr->tx_thread_id == TX_THREAD_ID)
    {
        result = TX_THREAD_ERROR; // already created
    }
    else if ((entry_function == nullptr) || (stack_start == nullptr) ||
             ((reinterpret_cast<uintptr_t>(stack_start) % alignof(stk::Word)) != 0U))
    {
        result = TX_PTR_ERROR;
    }
    else if (stack_size < static_cast<ULONG>(STK_STACK_SIZE_MIN * sizeof(stk::Word)))
    {
        result = TX_SIZE_ERROR;
    }
    else if (priority >= TX_MAX_PRIORITIES)
    {
        result = TX_PRIORITY_ERROR;
    }
    else if (preempt_threshold > priority) // numerically greater = lower than the thread's own priority
    {
        result = TX_THRESH_ERROR;
    }
    else if (auto_start > TX_AUTO_START)
    {
        result = TX_START_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        thread_ptr->tx_thread_run_count              = 0U;
        thread_ptr->tx_thread_stack_start            = stack_start;
        thread_ptr->tx_thread_stack_size             = stack_size;
        thread_ptr->tx_thread_time_slice             = time_slice;
        thread_ptr->tx_thread_name                   = name_ptr;
        thread_ptr->tx_thread_priority               = priority;
        thread_ptr->tx_thread_state                  = TX_SUSPENDED;
        thread_ptr->tx_thread_preempt_threshold      = preempt_threshold;
        thread_ptr->tx_thread_entry                  = entry_function;
        thread_ptr->tx_thread_entry_parameter        = entry_input;
        thread_ptr->tx_thread_entry_exit_notify      = nullptr;
        thread_ptr->tx_thread_extension_ptr          = nullptr;
        thread_ptr->tx_thread_stk_start_priority     = priority;
        thread_ptr->tx_thread_stk_start_threshold    = preempt_threshold;
        thread_ptr->tx_thread_stk_start_time_slice   = time_slice;

        StkThread *const th = new (static_cast<void *>(thread_ptr->tx_thread_stk_storage)) StkThread(thread_ptr);

        result = TX_SUCCESS;

        if (auto_start == TX_AUTO_START)
        {
            result = RegisterThread(th);
        }

        if (result == TX_SUCCESS)
        {
            thread_ptr->tx_thread_id = TX_THREAD_ID;
        }
        else
        {
            th->~StkThread();
        }
    }

    return result;
}

UINT tx_thread_delete(TX_THREAD *thread_ptr)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else if ((thread_ptr->tx_thread_state != TX_COMPLETED) && (thread_ptr->tx_thread_state != TX_TERMINATED))
    {
        result = TX_DELETE_ERROR;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);

        if (!WaitThreadRemoved(th))
        {
            result = TX_DELETE_ERROR; // the kernel still references the thread
        }
        else
        {
            thread_ptr->tx_thread_id = TX_CLEAR_ID;
            th->~StkThread();
            result = TX_SUCCESS;
        }
    }

    return result;
}

UINT tx_thread_entry_exit_notify(TX_THREAD *thread_ptr,
                                 VOID (*thread_entry_exit_notify)(TX_THREAD *notify_thread_ptr, UINT type))
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else
    {
        thread_ptr->tx_thread_entry_exit_notify = thread_entry_exit_notify;
        result = TX_SUCCESS;
    }

    return result;
}

TX_THREAD *tx_thread_identify(VOID)
{
    StkThread *const th = CurrentThread();

    return ((th != nullptr) ? th->m_tx : nullptr);
}

UINT tx_thread_info_get(TX_THREAD *thread_ptr, TX_NAME_CONST CHAR **name, UINT *state,
                        ULONG *run_count, UINT *priority, UINT *preemption_threshold,
                        ULONG *time_slice, TX_THREAD **next_thread, TX_THREAD **next_suspended_thread)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else
    {
        if (name != nullptr)                 { *name                 = thread_ptr->tx_thread_name; }
        if (state != nullptr)                { *state                = ThreadStateOf(thread_ptr); }
        if (run_count != nullptr)            { *run_count            = thread_ptr->tx_thread_run_count; }
        if (priority != nullptr)             { *priority             = thread_ptr->tx_thread_priority; }
        if (preemption_threshold != nullptr) { *preemption_threshold = thread_ptr->tx_thread_preempt_threshold; }
        if (time_slice != nullptr)           { *time_slice           = thread_ptr->tx_thread_time_slice; }

        // the kernel does not expose its internal created/suspended lists
        if (next_thread != nullptr)           { *next_thread           = nullptr; }
        if (next_suspended_thread != nullptr) { *next_suspended_thread = nullptr; }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold, UINT *old_threshold)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else if (old_threshold == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if (new_threshold > thread_ptr->tx_thread_priority)
    {
        result = TX_THRESH_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);

        *old_threshold = thread_ptr->tx_thread_preempt_threshold;

        thread_ptr->tx_thread_preempt_threshold = new_threshold;
        th->m_weight = ToStkWeight(thread_ptr->tx_thread_priority, new_threshold);

        if (th->m_registered && !th->m_removed)
        {
            // run-time threshold change, handled by the strategy (releases/creates the thread's hold)
            stk::SetWeight(stk::GetTidFromUserTask(th), th->m_weight);
        }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_thread_priority_change(TX_THREAD *thread_ptr, UINT new_priority, UINT *old_priority)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else if (new_priority >= TX_MAX_PRIORITIES)
    {
        result = TX_PRIORITY_ERROR;
    }
    else if (old_priority == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);

        *old_priority = thread_ptr->tx_thread_priority;

        // as in ThreadX: a threshold numerically smaller than the new priority is kept, otherwise
        // (the new priority is more urgent than the threshold) it is lowered to the new priority
        UINT new_threshold = thread_ptr->tx_thread_preempt_threshold;
        if (new_priority < new_threshold)
        {
            new_threshold = new_priority;
        }

        thread_ptr->tx_thread_priority          = new_priority;
        thread_ptr->tx_thread_preempt_threshold = new_threshold;
        th->m_weight = ToStkWeight(new_priority, new_threshold);

        if (th->m_registered && !th->m_removed)
        {
            stk::SetWeight(stk::GetTidFromUserTask(th), th->m_weight);
        }

        result = TX_SUCCESS;
    }

    return result;
}

VOID tx_thread_relinquish(VOID)
{
    if (!InIsr() && KernelRunning())
    {
        stk::Yield(); // tx_thread_relinquish semantics (sched::SwitchStrategyPreemptionThreshold handles it in OnTaskYield())
    }
}

UINT tx_thread_reset(TX_THREAD *thread_ptr)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else if ((thread_ptr->tx_thread_state != TX_COMPLETED) && (thread_ptr->tx_thread_state != TX_TERMINATED))
    {
        result = TX_NOT_DONE;
    }
    else
    {
        StkThread *th = ThreadOf(thread_ptr);

        if (!WaitThreadRemoved(th))
        {
            result = TX_NOT_DONE; // the kernel is still removing the thread, try again later
        }
        else
        {
            // restore creation parameters and re-create the STK task object
            thread_ptr->tx_thread_priority          = thread_ptr->tx_thread_stk_start_priority;
            thread_ptr->tx_thread_preempt_threshold = thread_ptr->tx_thread_stk_start_threshold;
            thread_ptr->tx_thread_time_slice        = thread_ptr->tx_thread_stk_start_time_slice;
            thread_ptr->tx_thread_state             = TX_SUSPENDED;

            th->~StkThread();
            new (static_cast<void *>(thread_ptr->tx_thread_stk_storage)) StkThread(thread_ptr);

            result = TX_SUCCESS; // resume it with tx_thread_resume()
        }
    }

    return result;
}

UINT tx_thread_sleep(ULONG timer_ticks)
{
    UINT result;
    StkThread *const th = CurrentThread();

    if (th == nullptr)
    {
        result = TX_CALLER_ERROR;
    }
    else if (timer_ticks == 0U)
    {
        result = TX_SUCCESS;
    }
    else
    {
        th->m_abort = false;

        // WAIT_INFINITE (INT32_MAX) is a valid very long sleep, ThreadX has no infinite sleep
        stk::Sleep(ToStkTimeout(timer_ticks)); // reported as TX_SLEEP by ThreadStateOf() (kernel state SLEEPING)

        if (th->m_abort)
        {
            th->m_abort = false;
            result = TX_WAIT_ABORTED;
        }
        else
        {
            result = TX_SUCCESS;
        }
    }

    return result;
}

UINT tx_thread_stack_error_notify(VOID (*stack_error_handler)(TX_THREAD *thread_ptr))
{
    // STK reports a stack overflow with KERNEL_PANIC_STACK_CORRUPT (see stk::IKernel panic handling),
    // there is no per-thread callback: accepted for source compatibility and ignored.
    (void)stack_error_handler;
    return TX_SUCCESS;
}

// Thread which was not passed to the kernel yet: created with TX_DONT_START or restarted by tx_thread_reset().
static bool IsNotStarted(const TX_THREAD *tx, const StkThread *th)
{
    return ((tx->tx_thread_state == TX_SUSPENDED) && !th->m_registered && th->m_removed);
}

UINT tx_thread_suspend(TX_THREAD *thread_ptr)
{
    UINT result = TX_SUSPEND_ERROR;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else if (!KernelRunning())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);
        bool eligible;

        {
            const stk::sync::ScopedCriticalSection cs_;

            // only a thread which is known to the kernel and has not completed/terminated
            eligible = (th->m_registered && !th->m_removed && (thread_ptr->tx_thread_state == TX_READY));
        }

        // tx_thread_state is intentionally not modified: TX_SUSPENDED is derived from the kernel by ThreadStateOf()
        // (IKernel::GetTaskState()). A thread blocked in a sleep/wait keeps reporting its wait state until the wait
        // ends (ThreadX "delayed suspension"), and a lifted suspension leaves nothing to restore.
        if (eligible)
        {
            // for the calling thread this returns after it has been resumed (SUSPEND_DONE)
            switch (g_StkKernel.SuspendTask(th))
            {
            case stk::SUSPEND_DONE:
            case stk::SUSPEND_DEFERRED: // sleeping/waiting: the kernel parks the thread when the sleep/wait ends,
                                        // its wait result is preserved
                result = TX_SUCCESS;
                break;
            default:                    // SUSPEND_ALREADY / SUSPEND_FAIL
                result = TX_SUSPEND_ERROR;
                break;
            }
        }
    }

    return result;
}

UINT tx_thread_resume(TX_THREAD *thread_ptr)
{
    UINT result = TX_RESUME_ERROR;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);
        bool start = false;

        {
            const stk::sync::ScopedCriticalSection cs_;

            // claim the start: a concurrent tx_thread_resume() (task or ISR) must not pass the thread to the kernel twice
            start = IsNotStarted(thread_ptr, th);
            if (start)
            {
                th->m_registered = true;
            }
        }

        if (start)
        {
            // first start (TX_DONT_START) or restart after tx_thread_reset(), allowed from an ISR too
            // (the kernel adds a task from an ISR immediately), on failure RegisterThread() rolls the claim back
            result = RegisterThread(th);
        }
        else
        {
            // tx_thread_state is not modified, see tx_thread_suspend()
            switch (g_StkKernel.ResumeTask(th))
            {
            case stk::RESUME_DONE:
                result = TX_SUCCESS;
                break;
            case stk::RESUME_LIFTED:    // deferred suspension withdrawn before it took effect
                result = TX_SUSPEND_LIFTED;
                break;
            default:                    // RESUME_NOT_SUSPENDED / RESUME_FAIL
                result = TX_RESUME_ERROR;
                break;
            }
        }
    }

    return result;
}

UINT tx_thread_terminate(TX_THREAD *thread_ptr)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else if ((thread_ptr->tx_thread_state == TX_COMPLETED) || (thread_ptr->tx_thread_state == TX_TERMINATED))
    {
        result = TX_SUCCESS;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);
        const bool is_self = (CurrentThread() == th);
        bool in_kernel;

        {
            const stk::sync::ScopedCriticalSection cs_;

            in_kernel = (th->m_registered && !th->m_removed);
            thread_ptr->tx_thread_state = TX_TERMINATED;
        }

        if (thread_ptr->tx_thread_entry_exit_notify != nullptr)
        {
            thread_ptr->tx_thread_entry_exit_notify(thread_ptr, TX_THREAD_EXIT);
        }

        if (in_kernel)
        {
            if (is_self)
            {
                // the calling thread is switched out and removed by the kernel, does not return
                stk::ExitTask();
            }
            else
            {
                // OnExit() fires from the kernel tick once the task is actually removed
                g_StkKernel.ScheduleTaskRemoval(th);
            }
        }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_thread_time_slice_change(TX_THREAD *thread_ptr, ULONG new_time_slice, ULONG *old_time_slice)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else if (old_time_slice == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);

        *old_time_slice = thread_ptr->tx_thread_time_slice;
        thread_ptr->tx_thread_time_slice = new_time_slice;

        const stk::Timeout ticks = ((new_time_slice == TX_NO_TIME_SLICE) ? stk::NO_TIME_SLICE :
            static_cast<stk::Timeout>(stk::Min<ULONG>(new_time_slice, static_cast<ULONG>(stk::WAIT_INFINITE) - 1U)));

        th->m_time_slice = ticks;

        if (th->m_registered && !th->m_removed)
        {
            stk::SetTimeSlice(stk::GetTidFromUserTask(th), ticks);
        }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_thread_wait_abort(TX_THREAD *thread_ptr)
{
    UINT result;

    if (!IsValidThread(thread_ptr))
    {
        result = TX_THREAD_ERROR;
    }
    else
    {
        StkThread *const th = ThreadOf(thread_ptr);

        stk::TaskStateInfo info = { stk::TASK_STATE_REMOVED, nullptr, stk::WAIT_INFINITE };

        if (th->m_registered && !th->m_removed)
        {
            if (!g_StkKernel.GetTaskState(th, info))
            {
                info.state = stk::TASK_STATE_REMOVED; // not queryable: not blocked as far as we can tell
            }
        }

        // exact: a thread suspended by tx_thread_suspend() is reported as SUSPENDED, not as sleeping
        const bool sleeping = (info.state == stk::TASK_STATE_SLEEPING);
        const bool waiting  = (info.state == stk::TASK_STATE_WAITING);

        if (!sleeping && !waiting)
        {
            result = TX_WAIT_ABORT_ERROR;
        }
        else
        {
            if (sleeping)
            {
                th->m_abort = true; // lets tx_thread_sleep() report TX_WAIT_ABORTED
            }

            // ends a plain sleep and cancels a sync-object wait (reported as WAIT_RESULT_CANCELED) identically
            stk::SleepCancel(stk::GetTidFromUserTask(th));
            result = TX_SUCCESS;
        }
    }

    return result;
}

// ============================================================================
// ==== Mutex ====
//
// Backed by stk::sync::Mutex: always recursive and with priority inheritance, the \a inherit argument
// of tx_mutex_create() is accepted but TX_NO_INHERIT is not honored.
// ============================================================================

STK_STATIC_ASSERT_DESC(sizeof(stk::sync::Mutex) <= sizeof(static_cast<TX_MUTEX *>(nullptr)->tx_mutex_stk_storage),
    "TX_STK_MUTEX_STORAGE_WORDS is too small for stk::sync::Mutex");

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

__stk_forceinline stk::sync::Mutex *MutexOf(TX_MUTEX *m)
{
    return static_cast<stk::sync::Mutex *>(static_cast<void *>(m->tx_mutex_stk_storage));
}

__stk_forceinline bool IsValidMutex(const TX_MUTEX *m)
{
    return ((m != nullptr) && (m->tx_mutex_id == TX_MUTEX_ID));
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_mutex_create(TX_MUTEX *mutex_ptr, TX_NAME_CONST CHAR *name_ptr, UINT inherit)
{
    UINT result;

    if (mutex_ptr == nullptr)
    {
        result = TX_MUTEX_ERROR;
    }
    else if (mutex_ptr->tx_mutex_id == TX_MUTEX_ID)
    {
        result = TX_MUTEX_ERROR; // already created
    }
    else if (inherit > TX_INHERIT)
    {
        result = TX_INHERIT_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        stk::sync::Mutex *const m = new (static_cast<void *>(mutex_ptr->tx_mutex_stk_storage)) stk::sync::Mutex();
        m->SetTraceName(name_ptr);

        mutex_ptr->tx_mutex_name = name_ptr;
        mutex_ptr->tx_mutex_id   = TX_MUTEX_ID;
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_mutex_delete(TX_MUTEX *mutex_ptr)
{
    UINT result;

    if (!IsValidMutex(mutex_ptr))
    {
        result = TX_MUTEX_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        // API contract of STK: no thread may be waiting on the mutex (waiters are not resumed with TX_DELETED)
        mutex_ptr->tx_mutex_id = TX_CLEAR_ID;
        MutexOf(mutex_ptr)->~Mutex();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    UINT result;

    if (!IsValidMutex(mutex_ptr))
    {
        result = TX_MUTEX_ERROR;
    }
    else if (InIsr() || !KernelRunning())
    {
        result = TX_CALLER_ERROR;
    }
    else if (!CanBlock(wait_option))
    {
        result = TX_WAIT_ERROR;
    }
    else
    {
        const ScopedWaitKind ws(TX_MUTEX_SUSP, wait_option);

        result = WaitResultToTx(MutexOf(mutex_ptr)->TimedLockEx(ToStkTimeout(wait_option)), TX_NOT_AVAILABLE);
    }

    return result;
}

UINT tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    UINT result;

    if (!IsValidMutex(mutex_ptr))
    {
        result = TX_MUTEX_ERROR;
    }
    else if (InIsr() || !KernelRunning())
    {
        result = TX_CALLER_ERROR;
    }
    else if (MutexOf(mutex_ptr)->GetOwner() != stk::GetTid())
    {
        result = TX_NOT_OWNED;
    }
    else
    {
        MutexOf(mutex_ptr)->Unlock();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_mutex_info_get(TX_MUTEX *mutex_ptr, TX_NAME_CONST CHAR **name, ULONG *count, TX_THREAD **owner,
                       TX_THREAD **first_suspended, ULONG *suspended_count, TX_MUTEX **next_mutex)
{
    UINT result;

    if (!IsValidMutex(mutex_ptr))
    {
        result = TX_MUTEX_ERROR;
    }
    else
    {
        stk::sync::Mutex *const m = MutexOf(mutex_ptr);

        if (name != nullptr)             { *name             = mutex_ptr->tx_mutex_name; }
        if (count != nullptr)            { *count            = m->GetRecursionCount(); }
        if (owner != nullptr)            { *owner            = ThreadFromTid(m->GetOwner()); }
        if (next_mutex != nullptr)       { *next_mutex       = nullptr; }

        FillSuspended(*m, first_suspended, suspended_count);

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_mutex_prioritize(TX_MUTEX *mutex_ptr)
{
    // waiters are served in FIFO order, priority ordering of the wait list is not supported (noop)
    return (IsValidMutex(mutex_ptr) ? TX_SUCCESS : TX_MUTEX_ERROR);
}

// ============================================================================
// ==== Semaphore ====
//
// Backed by stk::sync::SemaphoreT<ULONG>: the counter has the same width as the ThreadX counter (ULONG), so
// the full ThreadX count range is supported. tx_semaphore_ceiling_put() maps to SemaphoreT::TrySignal(ceiling),
// which checks the ceiling and signals atomically.
// ============================================================================

typedef stk::sync::SemaphoreT<ULONG> StkSemaphore;

STK_STATIC_ASSERT_DESC(sizeof(StkSemaphore) <= sizeof(static_cast<TX_SEMAPHORE *>(nullptr)->tx_semaphore_stk_storage),
    "TX_STK_SEMAPHORE_STORAGE_WORDS is too small for stk::sync::SemaphoreT<ULONG>");

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

__stk_forceinline StkSemaphore *SemOf(TX_SEMAPHORE *s)
{
    return static_cast<StkSemaphore *>(static_cast<void *>(s->tx_semaphore_stk_storage));
}

__stk_forceinline bool IsValidSem(const TX_SEMAPHORE *s)
{
    return ((s != nullptr) && (s->tx_semaphore_id == TX_SEMAPHORE_ID));
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_semaphore_create(TX_SEMAPHORE *semaphore_ptr, TX_NAME_CONST CHAR *name_ptr, ULONG initial_count)
{
    UINT result;

    if (semaphore_ptr == nullptr)
    {
        result = TX_SEMAPHORE_ERROR;
    }
    else if (semaphore_ptr->tx_semaphore_id == TX_SEMAPHORE_ID)
    {
        result = TX_SEMAPHORE_ERROR; // already created
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkSemaphore *const s = new (static_cast<void *>(semaphore_ptr->tx_semaphore_stk_storage))
            StkSemaphore(initial_count, StkSemaphore::COUNT_MAX);
        s->SetTraceName(name_ptr);

        semaphore_ptr->tx_semaphore_name       = name_ptr;
        semaphore_ptr->tx_semaphore_put_notify = nullptr;
        semaphore_ptr->tx_semaphore_id         = TX_SEMAPHORE_ID;
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_semaphore_delete(TX_SEMAPHORE *semaphore_ptr)
{
    UINT result;

    if (!IsValidSem(semaphore_ptr))
    {
        result = TX_SEMAPHORE_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        // API contract of STK: no thread may be waiting on the semaphore (waiters are not resumed with TX_DELETED)
        semaphore_ptr->tx_semaphore_id = TX_CLEAR_ID;
        SemOf(semaphore_ptr)->~StkSemaphore();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_semaphore_get(TX_SEMAPHORE *semaphore_ptr, ULONG wait_option)
{
    UINT result;

    if (!IsValidSem(semaphore_ptr))
    {
        result = TX_SEMAPHORE_ERROR;
    }
    else if (!CanBlock(wait_option))
    {
        result = TX_WAIT_ERROR;
    }
    else
    {
        const ScopedWaitKind ws(TX_SEMAPHORE_SUSP, wait_option);

        result = WaitResultToTx(SemOf(semaphore_ptr)->WaitEx(ToStkTimeout(wait_option)), TX_NO_INSTANCE);
    }

    return result;
}

UINT tx_semaphore_put(TX_SEMAPHORE *semaphore_ptr)
{
    UINT result;

    if (!IsValidSem(semaphore_ptr))
    {
        result = TX_SEMAPHORE_ERROR;
    }
    else if (!SemOf(semaphore_ptr)->TrySignal())
    {
        result = TX_CEILING_EXCEEDED; // counter saturated at the maximum of ULONG (ThreadX itself would wrap)
    }
    else
    {
        if (semaphore_ptr->tx_semaphore_put_notify != nullptr)
        {
            semaphore_ptr->tx_semaphore_put_notify(semaphore_ptr);
        }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_semaphore_ceiling_put(TX_SEMAPHORE *semaphore_ptr, ULONG ceiling)
{
    UINT result;

    if (!IsValidSem(semaphore_ptr))
    {
        result = TX_SEMAPHORE_ERROR;
    }
    else if (ceiling == 0U)
    {
        result = TX_OPTION_ERROR;
    }
    else
    {
        // the ceiling check and the signal are made atomically by the semaphore: with a waiter present the
        // signal is handed over directly and the counter does not grow, so the ceiling can be exceeded only
        // when nobody is waiting
        result = (SemOf(semaphore_ptr)->TrySignal(ceiling) ? TX_SUCCESS : TX_CEILING_EXCEEDED);
    }

    if ((result == TX_SUCCESS) && (semaphore_ptr->tx_semaphore_put_notify != nullptr))
    {
        semaphore_ptr->tx_semaphore_put_notify(semaphore_ptr);
    }

    return result;
}

UINT tx_semaphore_info_get(TX_SEMAPHORE *semaphore_ptr, TX_NAME_CONST CHAR **name, ULONG *current_value,
                           TX_THREAD **first_suspended, ULONG *suspended_count, TX_SEMAPHORE **next_semaphore)
{
    UINT result;

    if (!IsValidSem(semaphore_ptr))
    {
        result = TX_SEMAPHORE_ERROR;
    }
    else
    {
        if (name != nullptr)            { *name            = semaphore_ptr->tx_semaphore_name; }
        if (current_value != nullptr)   { *current_value   = SemOf(semaphore_ptr)->GetCount(); }
        if (next_semaphore != nullptr)  { *next_semaphore  = nullptr; }

        FillSuspended(*SemOf(semaphore_ptr), first_suspended, suspended_count);

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_semaphore_prioritize(TX_SEMAPHORE *semaphore_ptr)
{
    return (IsValidSem(semaphore_ptr) ? TX_SUCCESS : TX_SEMAPHORE_ERROR); // noop: FIFO wait list
}

UINT tx_semaphore_put_notify(TX_SEMAPHORE *semaphore_ptr,
                             VOID (*semaphore_put_notify)(TX_SEMAPHORE *notify_semaphore_ptr))
{
    UINT result;

    if (!IsValidSem(semaphore_ptr))
    {
        result = TX_SEMAPHORE_ERROR;
    }
    else
    {
        semaphore_ptr->tx_semaphore_put_notify = semaphore_put_notify;
        result = TX_SUCCESS;
    }

    return result;
}

// ============================================================================
// ==== Event flags ====
//
// Backed by stk::sync::EventFlagsT<uint64_t>: all 32 ThreadX flags are usable (flags are kept in the low half of
// the 64-bit word, the reserved error bit of STK is bit 63).
// ============================================================================

STK_STATIC_ASSERT_DESC(sizeof(StkEventFlags) <= sizeof(static_cast<TX_EVENT_FLAGS_GROUP *>(nullptr)->tx_event_flags_group_stk_storage),
    "TX_STK_EVENT_FLAGS_STORAGE_DWORDS is too small for StkEventFlags");
STK_STATIC_ASSERT_DESC(alignof(StkEventFlags) <= alignof(uint64_t),
    "StkEventFlags requires stricter alignment than the event flags storage (uint64_t)");

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

__stk_forceinline StkEventFlags *EfOf(TX_EVENT_FLAGS_GROUP *g)
{
    return static_cast<StkEventFlags *>(static_cast<void *>(g->tx_event_flags_group_stk_storage));
}

__stk_forceinline bool IsValidEf(const TX_EVENT_FLAGS_GROUP *g)
{
    return ((g != nullptr) && (g->tx_event_flags_group_id == TX_EVENT_FLAGS_ID));
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP *group_ptr, TX_NAME_CONST CHAR *name_ptr)
{
    UINT result;

    if (group_ptr == nullptr)
    {
        result = TX_GROUP_ERROR;
    }
    else if (group_ptr->tx_event_flags_group_id == TX_EVENT_FLAGS_ID)
    {
        result = TX_GROUP_ERROR; // already created
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkEventFlags *const g = new (static_cast<void *>(group_ptr->tx_event_flags_group_stk_storage))
            StkEventFlags(0U);
        g->SetTraceName(name_ptr);

        group_ptr->tx_event_flags_group_name       = name_ptr;
        group_ptr->tx_event_flags_group_set_notify = nullptr;
        group_ptr->tx_event_flags_group_id         = TX_EVENT_FLAGS_ID;
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_event_flags_delete(TX_EVENT_FLAGS_GROUP *group_ptr)
{
    UINT result;

    if (!IsValidEf(group_ptr))
    {
        result = TX_GROUP_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        // API contract of STK: no thread may be waiting on the group (waiters are not resumed with TX_DELETED)
        group_ptr->tx_event_flags_group_id = TX_CLEAR_ID;
        EfOf(group_ptr)->~EventFlagsT();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG requested_flags, UINT get_option,
                        ULONG *actual_flags_ptr, ULONG wait_option)
{
    UINT result;

    if (!IsValidEf(group_ptr))
    {
        result = TX_GROUP_ERROR;
    }
    else if (actual_flags_ptr == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if ((get_option > TX_AND_CLEAR) || (requested_flags == 0U))
    {
        result = TX_OPTION_ERROR;
    }
    else if (!CanBlock(wait_option))
    {
        result = TX_WAIT_ERROR;
    }
    else
    {
        // TX_AND -> wait for all requested flags, TX_OR -> any of them; ThreadX clears matched flags only
        // for the *_CLEAR options (bit 0), STK clears by default
        uint32_t opts = ((get_option & TX_AND) != 0U) ? StkEventFlags::OPT_WAIT_ALL : StkEventFlags::OPT_WAIT_ANY;
        if ((get_option & 1U) == 0U)
        {
            opts |= StkEventFlags::OPT_NO_CLEAR;
        }

        uint64_t actual = 0U;
        stk::EWaitResult wr;
        {
            const ScopedWaitKind ws(TX_EVENT_FLAG, wait_option);

            wr = EfOf(group_ptr)->WaitEx(static_cast<uint64_t>(requested_flags), opts, ToStkTimeout(wait_option), actual);
        }

        result = WaitResultToTx(wr, TX_NO_EVENTS);

        if (result == TX_SUCCESS)
        {
            // like ThreadX: the whole group's flags word as it was when the request was satisfied (before auto-clear)
            *actual_flags_ptr = static_cast<ULONG>(actual); // the flags are in the low 32 bits
        }
    }

    return result;
}

UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set, UINT set_option)
{
    UINT result;

    if (!IsValidEf(group_ptr))
    {
        result = TX_GROUP_ERROR;
    }
    else if ((set_option != TX_OR) && (set_option != TX_AND))
    {
        result = TX_OPTION_ERROR;
    }
    else
    {
        StkEventFlags *const g = EfOf(group_ptr);

        if (set_option == TX_OR)
        {
            if (flags_to_set != 0U)
            {
                (void)g->Set(static_cast<uint64_t>(flags_to_set)); // wakes all waiters, each re-evaluates its own condition
            }
        }
        else
        {
            // TX_AND: group = group & flags_to_set (cannot satisfy any waiter, so nobody is woken)
            (void)g->And(static_cast<uint64_t>(flags_to_set)); // the upper half of the group is always 0
        }

        if (group_ptr->tx_event_flags_group_set_notify != nullptr)
        {
            group_ptr->tx_event_flags_group_set_notify(group_ptr);
        }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_event_flags_info_get(TX_EVENT_FLAGS_GROUP *group_ptr, TX_NAME_CONST CHAR **name, ULONG *current_flags,
                             TX_THREAD **first_suspended, ULONG *suspended_count, TX_EVENT_FLAGS_GROUP **next_group)
{
    UINT result;

    if (!IsValidEf(group_ptr))
    {
        result = TX_GROUP_ERROR;
    }
    else
    {
        if (name != nullptr)            { *name            = group_ptr->tx_event_flags_group_name; }
        if (current_flags != nullptr)   { *current_flags   = static_cast<ULONG>(EfOf(group_ptr)->Get()); }
        if (next_group != nullptr)      { *next_group      = nullptr; }

        FillSuspended(*EfOf(group_ptr), first_suspended, suspended_count);

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_event_flags_set_notify(TX_EVENT_FLAGS_GROUP *group_ptr,
                               VOID (*events_set_notify)(TX_EVENT_FLAGS_GROUP *notify_group_ptr))
{
    UINT result;

    if (!IsValidEf(group_ptr))
    {
        result = TX_GROUP_ERROR;
    }
    else
    {
        group_ptr->tx_event_flags_group_set_notify = events_set_notify;
        result = TX_SUCCESS;
    }

    return result;
}

// ============================================================================
// ==== Message queue ====
//
// Backed by stk::sync::MessageQueue. ThreadX message size is counted in ULONGs, the queue area (queue_size) in
// bytes; the area is used as the ring buffer directly (no heap).
// ============================================================================

STK_STATIC_ASSERT_DESC(sizeof(stk::sync::MessageQueue) <= sizeof(static_cast<TX_QUEUE *>(nullptr)->tx_queue_stk_storage),
    "TX_STK_QUEUE_STORAGE_WORDS is too small for stk::sync::MessageQueue");

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

__stk_forceinline stk::sync::MessageQueue *QueueOf(TX_QUEUE *q)
{
    return static_cast<stk::sync::MessageQueue *>(static_cast<void *>(q->tx_queue_stk_storage));
}

__stk_forceinline bool IsValidQueue(const TX_QUEUE *q)
{
    return ((q != nullptr) && (q->tx_queue_id == TX_QUEUE_ID));
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_queue_create(TX_QUEUE *queue_ptr, TX_NAME_CONST CHAR *name_ptr, UINT message_size,
                     VOID *queue_start, ULONG queue_size)
{
    UINT result;

    if (queue_ptr == nullptr)
    {
        result = TX_QUEUE_ERROR;
    }
    else if (queue_ptr->tx_queue_id == TX_QUEUE_ID)
    {
        result = TX_QUEUE_ERROR; // already created
    }
    else if (queue_start == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if ((message_size < TX_1_ULONG) || (message_size > TX_16_ULONG))
    {
        result = TX_SIZE_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        const size_t msg_bytes = (static_cast<size_t>(message_size) * sizeof(ULONG));
        const size_t capacity  = (static_cast<size_t>(queue_size) / msg_bytes);

        if ((capacity < 1U) || (capacity > stk::sync::MessageQueue::CAPACITY_MAX))
        {
            result = TX_SIZE_ERROR;
        }
        else
        {
            stk::sync::MessageQueue *const q = new (static_cast<void *>(queue_ptr->tx_queue_stk_storage))
                stk::sync::MessageQueue(static_cast<uint8_t *>(queue_start), capacity, msg_bytes);
            q->SetTraceName(name_ptr);

            queue_ptr->tx_queue_name         = name_ptr;
            queue_ptr->tx_queue_message_size = message_size;
            queue_ptr->tx_queue_send_notify  = nullptr;
            queue_ptr->tx_queue_id           = TX_QUEUE_ID;
            result = TX_SUCCESS;
        }
    }

    return result;
}

UINT tx_queue_delete(TX_QUEUE *queue_ptr)
{
    UINT result;

    if (!IsValidQueue(queue_ptr))
    {
        result = TX_QUEUE_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        // API contract of STK: no thread may be waiting on the queue (waiters are not resumed with TX_DELETED)
        queue_ptr->tx_queue_id = TX_CLEAR_ID;
        QueueOf(queue_ptr)->~MessageQueue();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_queue_flush(TX_QUEUE *queue_ptr)
{
    UINT result;

    if (!IsValidQueue(queue_ptr))
    {
        result = TX_QUEUE_ERROR;
    }
    else
    {
        QueueOf(queue_ptr)->Reset();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_queue_info_get(TX_QUEUE *queue_ptr, TX_NAME_CONST CHAR **name, ULONG *enqueued, ULONG *available_storage,
                       TX_THREAD **first_suspended, ULONG *suspended_count, TX_QUEUE **next_queue)
{
    UINT result;

    if (!IsValidQueue(queue_ptr))
    {
        result = TX_QUEUE_ERROR;
    }
    else
    {
        stk::sync::MessageQueue *const q = QueueOf(queue_ptr);

        if (name != nullptr)              { *name              = queue_ptr->tx_queue_name; }
        if (enqueued != nullptr)          { *enqueued          = static_cast<ULONG>(q->GetCount()); }
        if (available_storage != nullptr) { *available_storage = static_cast<ULONG>(q->GetSpace()); }
        if (first_suspended != nullptr)   { *first_suspended   = nullptr; }
        if (suspended_count != nullptr)   { *suspended_count   = 0U; }
        if (next_queue != nullptr)        { *next_queue        = nullptr; }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_queue_prioritize(TX_QUEUE *queue_ptr)
{
    return (IsValidQueue(queue_ptr) ? TX_SUCCESS : TX_QUEUE_ERROR); // noop: FIFO wait list
}

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

UINT QueueSend(TX_QUEUE *queue_ptr, VOID *source_ptr, ULONG wait_option, bool front)
{
    UINT result;

    if (!IsValidQueue(queue_ptr))
    {
        result = TX_QUEUE_ERROR;
    }
    else if (source_ptr == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if (!CanBlock(wait_option))
    {
        result = TX_WAIT_ERROR;
    }
    else
    {
        stk::sync::MessageQueue *const q = QueueOf(queue_ptr);
        const stk::Timeout timeout       = ToStkTimeout(wait_option);

        {
            const ScopedWaitKind ws(TX_QUEUE_SUSP, wait_option);

            result = WaitResultToTx((front ? q->PutFrontEx(source_ptr, timeout) : q->PutEx(source_ptr, timeout)),
                                    TX_QUEUE_FULL);
        }

        if ((result == TX_SUCCESS) && (queue_ptr->tx_queue_send_notify != nullptr))
        {
            queue_ptr->tx_queue_send_notify(queue_ptr);
        }
    }

    return result;
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_queue_send(TX_QUEUE *queue_ptr, VOID *source_ptr, ULONG wait_option)
{
    return QueueSend(queue_ptr, source_ptr, wait_option, false);
}

UINT tx_queue_front_send(TX_QUEUE *queue_ptr, VOID *source_ptr, ULONG wait_option)
{
    return QueueSend(queue_ptr, source_ptr, wait_option, true);
}

UINT tx_queue_receive(TX_QUEUE *queue_ptr, VOID *destination_ptr, ULONG wait_option)
{
    UINT result;

    if (!IsValidQueue(queue_ptr))
    {
        result = TX_QUEUE_ERROR;
    }
    else if (destination_ptr == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if (!CanBlock(wait_option))
    {
        result = TX_WAIT_ERROR;
    }
    else
    {
        const ScopedWaitKind ws(TX_QUEUE_SUSP, wait_option);

        result = WaitResultToTx(QueueOf(queue_ptr)->GetEx(destination_ptr, ToStkTimeout(wait_option)), TX_QUEUE_EMPTY);
    }

    return result;
}

UINT tx_queue_send_notify(TX_QUEUE *queue_ptr, VOID (*queue_send_notify)(TX_QUEUE *notify_queue_ptr))
{
    UINT result;

    if (!IsValidQueue(queue_ptr))
    {
        result = TX_QUEUE_ERROR;
    }
    else
    {
        queue_ptr->tx_queue_send_notify = queue_send_notify;
        result = TX_SUCCESS;
    }

    return result;
}

// ============================================================================
// ==== Block memory pool ====
//
// Backed by stk::memory::BlockMemoryPool. tx_block_release() receives only the block address, so, as in
// ThreadX, every block carries a hidden pointer-sized header holding the owning pool.
// ============================================================================

STK_STATIC_ASSERT_DESC(sizeof(stk::memory::BlockMemoryPool) <= sizeof(static_cast<TX_BLOCK_POOL *>(nullptr)->tx_block_pool_stk_storage),
    "TX_STK_BLOCK_POOL_STORAGE_WORDS is too small for stk::memory::BlockMemoryPool");

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

constexpr size_t BLOCK_HDR_SIZE = sizeof(void *);

__stk_forceinline stk::memory::BlockMemoryPool *BlockPoolOf(TX_BLOCK_POOL *p)
{
    return static_cast<stk::memory::BlockMemoryPool *>(static_cast<void *>(p->tx_block_pool_stk_storage));
}

__stk_forceinline bool IsValidBlockPool(const TX_BLOCK_POOL *p)
{
    return ((p != nullptr) && (p->tx_block_pool_id == TX_BLOCK_POOL_ID));
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_block_pool_create(TX_BLOCK_POOL *pool_ptr, TX_NAME_CONST CHAR *name_ptr, ULONG block_size,
                          VOID *pool_start, ULONG pool_size)
{
    UINT result;

    if (pool_ptr == nullptr)
    {
        result = TX_POOL_ERROR;
    }
    else if (pool_ptr->tx_block_pool_id == TX_BLOCK_POOL_ID)
    {
        result = TX_POOL_ERROR; // already created
    }
    else if ((pool_start == nullptr) || ((reinterpret_cast<uintptr_t>(pool_start) % sizeof(void *)) != 0U))
    {
        result = TX_PTR_ERROR;
    }
    else if (block_size == 0U)
    {
        result = TX_SIZE_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        const size_t stride   = stk::memory::BlockMemoryPool::AlignBlockSize(static_cast<size_t>(block_size) + BLOCK_HDR_SIZE);
        size_t       capacity = (static_cast<size_t>(pool_size) / stride);

        if (capacity < 1U)
        {
            result = TX_SIZE_ERROR;
        }
        else
        {
            capacity = stk::Min<size_t>(capacity, stk::memory::BlockMemoryPool::CAPACITY_MAX);

            stk::memory::BlockMemoryPool *const p = new (static_cast<void *>(pool_ptr->tx_block_pool_stk_storage))
                stk::memory::BlockMemoryPool(capacity, static_cast<size_t>(block_size) + BLOCK_HDR_SIZE,
                    static_cast<uint8_t *>(pool_start), (capacity * stride), name_ptr);
            p->SetTraceName(name_ptr);

            pool_ptr->tx_block_pool_name = name_ptr;
            pool_ptr->tx_block_pool_id   = TX_BLOCK_POOL_ID;
            result = TX_SUCCESS;
        }
    }

    return result;
}

UINT tx_block_pool_delete(TX_BLOCK_POOL *pool_ptr)
{
    UINT result;

    if (!IsValidBlockPool(pool_ptr))
    {
        result = TX_POOL_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        // API contract of STK: no thread may be waiting on the pool (waiters are not resumed with TX_DELETED)
        pool_ptr->tx_block_pool_id = TX_CLEAR_ID;
        BlockPoolOf(pool_ptr)->~BlockMemoryPool();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_block_allocate(TX_BLOCK_POOL *pool_ptr, VOID **block_ptr, ULONG wait_option)
{
    UINT result;

    if (!IsValidBlockPool(pool_ptr))
    {
        result = TX_POOL_ERROR;
    }
    else if (block_ptr == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if (!CanBlock(wait_option))
    {
        result = TX_WAIT_ERROR;
    }
    else
    {
        void *blk = nullptr;
        stk::EWaitResult wr;

        {
            const ScopedWaitKind ws(TX_BLOCK_MEMORY, wait_option);

            wr = BlockPoolOf(pool_ptr)->TimedAllocEx(&blk, ToStkTimeout(wait_option));
        }

        result = WaitResultToTx(wr, TX_NO_MEMORY);

        if ((result == TX_SUCCESS) && (blk != nullptr))
        {
            // store the owner in the hidden header, hand out the area behind it
            *static_cast<TX_BLOCK_POOL **>(blk) = pool_ptr;
            *block_ptr = (static_cast<uint8_t *>(blk) + BLOCK_HDR_SIZE);
        }
        else if (result == TX_SUCCESS)
        {
            result = TX_NO_MEMORY;
        }
        else
        {
            // noop: error status is already set
        }
    }

    return result;
}

UINT tx_block_release(VOID *block_ptr)
{
    UINT result;

    if (block_ptr == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else
    {
        void *const blk = (static_cast<uint8_t *>(block_ptr) - BLOCK_HDR_SIZE);
        TX_BLOCK_POOL *const pool = *static_cast<TX_BLOCK_POOL **>(blk);

        if (!IsValidBlockPool(pool))
        {
            result = TX_PTR_ERROR; // not a block handed out by tx_block_allocate()
        }
        else
        {
            result = (BlockPoolOf(pool)->Free(blk) ? TX_SUCCESS : TX_PTR_ERROR);
        }
    }

    return result;
}

UINT tx_block_pool_info_get(TX_BLOCK_POOL *pool_ptr, TX_NAME_CONST CHAR **name, ULONG *available_blocks,
                            ULONG *total_blocks, TX_THREAD **first_suspended, ULONG *suspended_count,
                            TX_BLOCK_POOL **next_pool)
{
    UINT result;

    if (!IsValidBlockPool(pool_ptr))
    {
        result = TX_POOL_ERROR;
    }
    else
    {
        stk::memory::BlockMemoryPool *const p = BlockPoolOf(pool_ptr);

        if (name != nullptr)             { *name             = pool_ptr->tx_block_pool_name; }
        if (available_blocks != nullptr) { *available_blocks = static_cast<ULONG>(p->GetFreeCount()); }
        if (total_blocks != nullptr)     { *total_blocks     = static_cast<ULONG>(p->GetCapacity()); }
        if (first_suspended != nullptr)  { *first_suspended  = nullptr; }
        if (suspended_count != nullptr)  { *suspended_count  = 0U; }
        if (next_pool != nullptr)        { *next_pool        = nullptr; }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_block_pool_prioritize(TX_BLOCK_POOL *pool_ptr)
{
    return (IsValidBlockPool(pool_ptr) ? TX_SUCCESS : TX_POOL_ERROR); // noop: FIFO wait list
}

// ============================================================================
// ==== Byte memory pool ====
//
// Backed by stk::memory::BytePool: first-fit allocator with block splitting, coalescing of free neighbours
// and task-blocking semantics (stk::sync::ConditionVariable, all waiters are re-evaluated on every release,
// the timeout is a total budget, not per wake-up).
//
// tx_byte_release() receives only the block address and BytePool blocks carry no owner pointer, so the
// pools created by tx_byte_pool_create() are kept in a small intrusive list and the owner is found by the
// address range of the pool area. The list is touched only by create/delete/release (short, under a critical
// section), allocation and info paths do not use it.
// ============================================================================

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

class StkBytePool final
{
    STK_NONCOPYABLE_CLASS(StkBytePool);

public:
    explicit StkBytePool(uint8_t *start, size_t size, const char *name)
        : m_next(nullptr),
          m_pool(start, size, name),
          m_lo(reinterpret_cast<uintptr_t>(start)),
          m_hi(reinterpret_cast<uintptr_t>(start) + size)
    {}

    stk::memory::BytePool &Pool() { return m_pool; }

    bool IsValid() const { return m_pool.IsStorageValid(); }

    // True if \a mem lies inside the area handed over to tx_byte_pool_create().
    bool Contains(const void *mem) const
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(mem);
        return ((a >= m_lo) && (a < m_hi));
    }

    StkBytePool *m_next; // registry link, protected by a critical section

private:
    stk::memory::BytePool m_pool;
    uintptr_t             m_lo;
    uintptr_t             m_hi;
};

STK_STATIC_ASSERT_DESC(sizeof(StkBytePool) <= sizeof(static_cast<TX_BYTE_POOL *>(nullptr)->tx_byte_pool_stk_storage),
    "TX_STK_BYTE_POOL_STORAGE_WORDS is too small for StkBytePool (stk::memory::BytePool + registry link and range)");
STK_STATIC_ASSERT_DESC(alignof(StkBytePool) <= alignof(ALIGN_TYPE),
    "StkBytePool requires stricter alignment than ALIGN_TYPE");

StkBytePool *g_BytePools = nullptr; // registry head

__stk_forceinline StkBytePool *BytePoolOf(TX_BYTE_POOL *p)
{
    return static_cast<StkBytePool *>(static_cast<void *>(p->tx_byte_pool_stk_storage));
}
__stk_forceinline bool IsValidBytePool(const TX_BYTE_POOL *p)
{
    return ((p != nullptr) && (p->tx_byte_pool_id == TX_BYTE_POOL_ID));
}

// Critical section must be held.
void BytePoolLink(StkBytePool *p)
{
    p->m_next   = g_BytePools;
    g_BytePools = p;
}

// Critical section must be held.
void BytePoolUnlink(StkBytePool *p)
{
    StkBytePool **link = &g_BytePools;

    while ((*link != nullptr) && (*link != p))
    {
        link = &((*link)->m_next);
    }

    if (*link != nullptr)
    {
        *link = p->m_next;
    }

    p->m_next = nullptr;
}

// Critical section must be held. Returns the pool whose area contains \a mem, or nullptr.
StkBytePool *BytePoolFind(const void *mem)
{
    StkBytePool *p = g_BytePools;

    while ((p != nullptr) && !p->Contains(mem))
    {
        p = p->m_next;
    }

    return p;
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_byte_pool_create(TX_BYTE_POOL *pool_ptr, TX_NAME_CONST CHAR *name_ptr, VOID *pool_start, ULONG pool_size)
{
    UINT result;

    if (pool_ptr == nullptr)
    {
        result = TX_POOL_ERROR;
    }
    else if (pool_ptr->tx_byte_pool_id == TX_BYTE_POOL_ID)
    {
        result = TX_POOL_ERROR; // already created
    }
    else if (pool_start == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if (static_cast<size_t>(pool_size) < stk::memory::BytePool::RequiredSize(1U))
    {
        result = TX_SIZE_ERROR; // cannot hold even one block (BytePool asserts on this, so reject it here)
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkBytePool *const p = new (static_cast<void *>(pool_ptr->tx_byte_pool_stk_storage))
            StkBytePool(static_cast<uint8_t *>(pool_start), static_cast<size_t>(pool_size), name_ptr);

        if (!p->IsValid())
        {
            p->~StkBytePool(); // area is too small once aligned
            result = TX_SIZE_ERROR;
        }
        else
        {
            pool_ptr->tx_byte_pool_name = name_ptr;
            pool_ptr->tx_byte_pool_id   = TX_BYTE_POOL_ID;

            {
                const stk::sync::ScopedCriticalSection cs_;
                BytePoolLink(p);
            }

            result = TX_SUCCESS;
        }
    }

    return result;
}

UINT tx_byte_pool_delete(TX_BYTE_POOL *pool_ptr)
{
    UINT result;

    if (!IsValidBytePool(pool_ptr))
    {
        result = TX_POOL_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkBytePool *const p = BytePoolOf(pool_ptr);

        // API contract of STK: no thread may be waiting on the pool (waiters are not resumed with TX_DELETED)
        pool_ptr->tx_byte_pool_id = TX_CLEAR_ID;

        {
            const stk::sync::ScopedCriticalSection cs_;
            BytePoolUnlink(p);
        }

        p->~StkBytePool();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_byte_allocate(TX_BYTE_POOL *pool_ptr, VOID **memory_ptr, ULONG memory_size, ULONG wait_option)
{
    UINT result;

    if (!IsValidBytePool(pool_ptr))
    {
        result = TX_POOL_ERROR;
    }
    else if (memory_ptr == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else if ((memory_size == 0U) || (static_cast<size_t>(memory_size) > BytePoolOf(pool_ptr)->Pool().GetMaxRequest()))
    {
        result = TX_SIZE_ERROR; // can never succeed, do not block on it
    }
    else if (!CanBlock(wait_option))
    {
        result = TX_WAIT_ERROR;
    }
    else
    {
        void *mem = nullptr;

        const ScopedWaitKind ws(TX_BYTE_MEMORY, wait_option);

        result = WaitResultToTx(
            BytePoolOf(pool_ptr)->Pool().TimedAllocEx(&mem, static_cast<size_t>(memory_size), ToStkTimeout(wait_option)),
            TX_NO_MEMORY);

        if (result == TX_SUCCESS)
        {
            *memory_ptr = mem;
        }
    }

    return result;
}

UINT tx_byte_release(VOID *memory_ptr)
{
    UINT result;

    if (memory_ptr == nullptr)
    {
        result = TX_PTR_ERROR;
    }
    else
    {
        StkBytePool *owner;

        {
            const stk::sync::ScopedCriticalSection cs_;
            owner = BytePoolFind(memory_ptr);
        }

        // BytePool::Free() validates the block (range, alignment, state marker) and wakes blocked allocators
        if (owner == nullptr)
        {
            result = TX_PTR_ERROR; // not inside any byte pool
        }
        else
        {
            result = (owner->Pool().Free(memory_ptr) ? TX_SUCCESS : TX_PTR_ERROR); // error: foreign pointer or double release
        }
    }

    return result;
}

UINT tx_byte_pool_info_get(TX_BYTE_POOL *pool_ptr, TX_NAME_CONST CHAR **name, ULONG *available_bytes,
                           ULONG *fragments, TX_THREAD **first_suspended, ULONG *suspended_count,
                           TX_BYTE_POOL **next_pool)
{
    UINT result;

    if (!IsValidBytePool(pool_ptr))
    {
        result = TX_POOL_ERROR;
    }
    else
    {
        size_t available = 0U;
        size_t largest   = 0U;
        size_t frags     = 0U;

        BytePoolOf(pool_ptr)->Pool().GetInfo(available, largest, frags); // consistent snapshot, takes its own critical section

        if (name != nullptr)            { *name            = pool_ptr->tx_byte_pool_name; }
        if (available_bytes != nullptr) { *available_bytes = static_cast<ULONG>(available); }
        if (fragments != nullptr)       { *fragments       = static_cast<ULONG>(frags); }
        if (first_suspended != nullptr) { *first_suspended = nullptr; } // BytePool does not expose its wait list yet
        if (suspended_count != nullptr) { *suspended_count = 0U; }
        if (next_pool != nullptr)       { *next_pool       = nullptr; }

        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_byte_pool_prioritize(TX_BYTE_POOL *pool_ptr)
{
    return (IsValidBytePool(pool_ptr) ? TX_SUCCESS : TX_POOL_ERROR); // noop: FIFO wait list
}

// ============================================================================
// ==== Application timers ====
//
// Backed by stk::time::TimerHost in task mode: a tick task evaluates deadlines and a handler task runs the
// expiration functions, both at TX_TIMER_THREAD_PRIORITY (as the timer thread of ThreadX). The TX_TIMER control
// block embeds the stk::time::Timer (intrusive), so the number of timers is not limited.
//
// The ThreadX calls use the synchronous TimerHost API: tx_timer_activate/deactivate/change/delete/info_get return
// the exact outcome (TX_ACTIVATE_ERROR for an already active timer, ...) and take effect before they return, so
// deactivating a timer from the callback of another one prevents it from firing. Called from an ISR (or before
// the kernel is started) the command is queued and TX_SUCCESS is reported optimistically, tx_timer_info_get() then
// returns an advisory snapshot.
//
// The host is started by the first tx_timer_create() and occupies stk::time::TimerHost::TASK_COUNT kernel task
// slots (see the kernel configuration above). Tick values are limited to INT32_MAX (STK timeout range), larger
// values are clamped.
// ============================================================================

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

class StkTimer final : public stk::time::Timer
{
    STK_NONCOPYABLE_CLASS(StkTimer);

public:
    explicit StkTimer(TX_TIMER *tx) : m_tx(tx)
    {}

    virtual ~StkTimer()
    {}

    // runs in the handler task of the timer host
    void OnExpired(StkTimerHost *) override
    {
        // copied first: the function and the input are not touched after the call, the timer may be deleted by it
        VOID (*const function)(ULONG) = m_tx->tx_timer_expiration_function;
        const ULONG input             = m_tx->tx_timer_expiration_input;

        if (function != nullptr)
        {
            function(input);
        }
    }

private:
    TX_TIMER *m_tx;
};

STK_STATIC_ASSERT_DESC(sizeof(StkTimer) <= sizeof(static_cast<TX_TIMER *>(nullptr)->tx_timer_stk_storage),
    "TX_STK_TIMER_STORAGE_DWORDS is too small for StkTimer (stk::time::Timer + back pointer)");
STK_STATIC_ASSERT_DESC(alignof(StkTimer) <= alignof(uint64_t),
    "StkTimer requires stricter alignment than the timer storage (uint64_t)");

bool g_TimerHostStarted = false;

__stk_forceinline StkTimer *TimerOf(TX_TIMER *t)
{
    return static_cast<StkTimer *>(static_cast<void *>(t->tx_timer_stk_storage));
}

__stk_forceinline bool IsValidTimer(const TX_TIMER *t) { return ((t != nullptr) && (t->tx_timer_id == TX_TIMER_ID)); }

// ThreadX ticks (ULONG) -> STK ticks (uint32_t limited to the STK timeout range).
__stk_forceinline uint32_t ToStkTicks(ULONG ticks)
{
    return static_cast<uint32_t>(stk::Min<ULONG>(ticks, static_cast<ULONG>(stk::WAIT_INFINITE)));
}

// Starts the timer host on first use (task or initialization context).
UINT EnsureTimerHost()
{
    UINT result = TX_SUCCESS;

    if (!g_TimerHostStarted)
    {
        StkTimerHost::Config cfg;

        // preemption-threshold == priority (no hold). The tick task is as urgent as the handler and so
        // as urgent as the callers of tx_timer_*() which have a priority not higher than the timer thread.
        cfg.tick_weight    = ToStkWeight(TX_TIMER_THREAD_PRIORITY, TX_TIMER_THREAD_PRIORITY);
        cfg.handler_weight = ToStkWeight(TX_TIMER_THREAD_PRIORITY, TX_TIMER_THREAD_PRIORITY);
        cfg.tick_mode      = stk::ACCESS_PRIVILEGED;

        if (g_TimerHost.Initialize(&g_StkKernel, stk::ACCESS_PRIVILEGED, cfg) == stk::ADD_OK)
        {
            g_TimerHostStarted = true;
        }
        else
        {
            // out of kernel task slots (increase TX_STK_MAX_THREADS) or the kernel is not suitable
            result = TX_NO_MEMORY;
        }
    }

    return result;
}

// Maps the outcome of a synchronous TimerHost command. \a active_status is returned for RES_ACTIVE.
UINT TimerResultToTx(StkTimerHost::EResult res, UINT active_status)
{
    UINT result;

    switch (res)
    {
    case StkTimerHost::RES_OK:
    case StkTimerHost::RES_INACTIVE: // already in the requested (inactive) state
    case StkTimerHost::RES_QUEUED:   // ISR / kernel not started: executed later
        result = TX_SUCCESS;
        break;

    case StkTimerHost::RES_ACTIVE:
        result = active_status;
        break;

    case StkTimerHost::RES_NOT_PERIODIC:
    case StkTimerHost::RES_FAIL:
    default:
        result = TX_TIMER_ERROR;
        break;
    }

    return result;
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

UINT tx_timer_create(TX_TIMER *timer_ptr, TX_NAME_CONST CHAR *name_ptr,
                     VOID (*expiration_function)(ULONG input), ULONG expiration_input,
                     ULONG initial_ticks, ULONG reschedule_ticks, UINT auto_activate)
{
    UINT result;

    if (timer_ptr == nullptr)
    {
        result = TX_TIMER_ERROR;
    }
    else if (timer_ptr->tx_timer_id == TX_TIMER_ID)
    {
        result = TX_TIMER_ERROR; // already created
    }
    else if (expiration_function == nullptr)
    {
        result = TX_FEATURE_NOT_ENABLED; // ThreadX reports TX_FUNCTION_ERROR (0x20) here, not defined in the API subset
    }
    else if (initial_ticks == 0U)
    {
        result = TX_TICK_ERROR;
    }
    else if (auto_activate > TX_AUTO_ACTIVATE)
    {
        result = TX_ACTIVATE_ERROR;
    }
    else if (InIsr() || !KernelInitialized())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        result = EnsureTimerHost();

        if (result == TX_SUCCESS)
        {
            timer_ptr->tx_timer_name                = name_ptr;
            timer_ptr->tx_timer_expiration_function = expiration_function;
            timer_ptr->tx_timer_expiration_input    = expiration_input;
            timer_ptr->tx_timer_initial_ticks       = initial_ticks;
            timer_ptr->tx_timer_reschedule_ticks    = reschedule_ticks;

            StkTimer *const t = new (static_cast<void *>(timer_ptr->tx_timer_stk_storage)) StkTimer(timer_ptr);

            // store the parameters in the timer (it is not started yet, so this cannot fail on activity)
            result = TimerResultToTx(g_TimerHost.ChangeSync(*t, ToStkTicks(initial_ticks), ToStkTicks(reschedule_ticks)),
                                     TX_TIMER_ERROR);

            if (result == TX_SUCCESS)
            {
                timer_ptr->tx_timer_id = TX_TIMER_ID;

                if (auto_activate == TX_AUTO_ACTIVATE)
                {
                    result = tx_timer_activate(timer_ptr);

                    if (result != TX_SUCCESS)
                    {
                        timer_ptr->tx_timer_id = TX_CLEAR_ID;
                    }
                }
            }

            if (result != TX_SUCCESS)
            {
                t->~StkTimer();
            }
        }
    }

    return result;
}

UINT tx_timer_delete(TX_TIMER *timer_ptr)
{
    UINT result;

    if (!IsValidTimer(timer_ptr))
    {
        result = TX_TIMER_ERROR;
    }
    else if (InIsr())
    {
        result = TX_CALLER_ERROR;
    }
    else
    {
        StkTimer *const t = TimerOf(timer_ptr);

        timer_ptr->tx_timer_id = TX_CLEAR_ID;

        // when StopSync() returns the host holds no reference to the timer (a queued expiration is removed, a
        // running expiration function has returned), so the storage can be released by the caller right after
        // this function. Called from an expiration function the wait for running functions is skipped (see
        // TimerHost "Synchronous API"): do not delete a timer from the expiration function of another timer.
        (void)g_TimerHost.StopSync(*t);

        t->~StkTimer();
        result = TX_SUCCESS;
    }

    return result;
}

UINT tx_timer_activate(TX_TIMER *timer_ptr)
{
    UINT result;

    if (!IsValidTimer(timer_ptr))
    {
        result = TX_TIMER_ERROR;
    }
    else
    {
        // a new expiration is relative to the moment of the call, the delay is the one of tx_timer_create/change
        result = TimerResultToTx(g_TimerHost.ActivateSync(*TimerOf(timer_ptr)), TX_ACTIVATE_ERROR);
    }

    return result;
}

UINT tx_timer_deactivate(TX_TIMER *timer_ptr)
{
    UINT result;

    if (!IsValidTimer(timer_ptr))
    {
        result = TX_TIMER_ERROR;
    }
    else
    {
        result = TimerResultToTx(g_TimerHost.StopSync(*TimerOf(timer_ptr)), TX_SUCCESS);
    }

    return result;
}

UINT tx_timer_change(TX_TIMER *timer_ptr, ULONG initial_ticks, ULONG reschedule_ticks)
{
    UINT result;

    if (!IsValidTimer(timer_ptr))
    {
        result = TX_TIMER_ERROR;
    }
    else if (initial_ticks == 0U)
    {
        result = TX_TICK_ERROR;
    }
    else
    {
        // ThreadX requires the timer to be deactivated (TX_ACTIVATE_ERROR otherwise), the new values are used by
        // the next tx_timer_activate()
        result = TimerResultToTx(g_TimerHost.ChangeSync(*TimerOf(timer_ptr), ToStkTicks(initial_ticks),
                                                        ToStkTicks(reschedule_ticks)), TX_ACTIVATE_ERROR);

        if (result == TX_SUCCESS)
        {
            timer_ptr->tx_timer_initial_ticks    = initial_ticks;
            timer_ptr->tx_timer_reschedule_ticks = reschedule_ticks;
        }
    }

    return result;
}

UINT tx_timer_info_get(TX_TIMER *timer_ptr, TX_NAME_CONST CHAR **name, UINT *active, ULONG *remaining_ticks,
                       ULONG *reschedule_ticks, TX_TIMER **next_timer)
{
    UINT result;

    if (!IsValidTimer(timer_ptr))
    {
        result = TX_TIMER_ERROR;
    }
    else
    {
        StkTimerHost::TimerInfo info = {};

        // consistent snapshot made by the tick task (advisory when called from an ISR)
        result = TimerResultToTx(g_TimerHost.QuerySync(*TimerOf(timer_ptr), info), TX_TIMER_ERROR);

        if (result == TX_SUCCESS)
        {
            if (name != nullptr)             { *name             = timer_ptr->tx_timer_name; }
            if (active != nullptr)           { *active           = (info.active ? TX_TRUE : TX_FALSE); }
            if (remaining_ticks != nullptr)  { *remaining_ticks  = static_cast<ULONG>(info.remaining); }
            if (reschedule_ticks != nullptr) { *reschedule_ticks = static_cast<ULONG>(info.period); }
            if (next_timer != nullptr)       { *next_timer       = nullptr; }
        }
    }

    return result;
}
