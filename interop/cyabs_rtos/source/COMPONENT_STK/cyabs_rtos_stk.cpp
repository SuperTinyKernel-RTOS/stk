/***********************************************************************************************//**
 * \file cyabs_rtos_stk.cpp
 *
 * \brief
 * SuperTinyKernel (STK) port of the Infineon RTOS abstraction layer (cyabs_rtos.h).
 * See README_STK.md for the design notes and the list of differences from the other ports.
 *
 ***************************************************************************************************
 * \copyright
 * Licensed under the Apache License, Version 2.0 (same license as the abstraction-rtos library).
 * SPDX-License-Identifier: Apache-2.0
 **************************************************************************************************/

#include <new>
#include <stdint.h>

#include "stk.h"
#include "sync/stk_sync.h"
#include "time/stk_time.h"

#include "cyabs_rtos.h"
#include "cyabs_rtos_stk.h"

// Intrusive list of the thread control blocks (see cy_stk_thread below).
struct cy_stk_thread;

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

STK_STATIC_ASSERT_DESC(!STK_MPU_STACK_GUARD,
    "STK_MPU_STACK_GUARD is not supported: user supplied stacks can not be given the required alignment");
STK_STATIC_ASSERT_DESC(CY_RTOS_STK_MIN_STACK_BYTES >= (STK_STACK_SIZE_MIN * sizeof(stk::Word)),
    "CY_RTOS_STK_MIN_STACK_BYTES is below the minimum stack size of STK for this target");

// ----------------------------------------------------------------------------
// Kernel configuration
//
// Fixed priority preemptive scheduling with round-robin inside a priority level, priority inheritance
// for mutexes (stk::sync::Mutex requests it from the strategy).
// ----------------------------------------------------------------------------

#if CY_RTOS_STK_USE_TIMERS
static constexpr uint32_t TIMER_TASKS = static_cast<uint32_t>(stk::time::TimerHost::TASK_COUNT);
#else
static constexpr uint32_t TIMER_TASKS = 0U;
#endif

typedef stk::sched::SwitchStrategyFixedPriority<CY_RTOS_STK_PRIORITY_LEVELS> Strategy;

typedef stk::Kernel<
    (stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC | stk::KERNEL_TELEMETRY
#if STK_TICKLESS_IDLE
    | stk::KERNEL_TICKLESS
#endif
    ), (CY_RTOS_STK_MAX_THREADS + TIMER_TASKS), Strategy, stk::PlatformDefault> Kernel;

Kernel g_Kernel;

#if CY_RTOS_STK_USE_TIMERS
stk::time::TimerHost g_TimerHost;
#endif

volatile cy_rtos_error_t g_LastError   = CY_RTOS_STK_ERR_NONE;
volatile uint32_t        g_SuspendCount = 0U;

// Validity markers stored in the "id" member of the cy_*_t types.
static constexpr uint32_t ID_MUTEX     = 0x4D545831U;
static constexpr uint32_t ID_SEMAPHORE = 0x53454D31U;
static constexpr uint32_t ID_EVENT     = 0x45564E31U;
static constexpr uint32_t ID_QUEUE     = 0x51554531U;
static constexpr uint32_t ID_TIMER     = 0x544D5231U;
static constexpr uint32_t ID_THREAD    = 0x54485231U;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

cy_rslt_t Fail(cy_rtos_error_t error)
{
    g_LastError = error;
    return CY_RTOS_GENERAL_ERROR;
}

__stk_forceinline bool InIsr()
{
    return stk::hw::IsInsideISR();
}

__stk_forceinline stk::IKernel::EKernelState KState()
{
    return g_Kernel.GetState();
}

// Threads are scheduled (not before Start(), not while the scheduler is locked).
__stk_forceinline bool SchedulerRunning()
{
    return (KState() == stk::IKernel::KSTATE_RUNNING);
}

// Milliseconds -> ticks, rounded up so that a wait is never shorter than requested.
uint32_t MsToTicks(cy_time_t ms)
{
    uint64_t ticks = ((static_cast<uint64_t>(ms) * 1000U) + (CY_RTOS_STK_TICK_PERIOD_US - 1U)) /
                     CY_RTOS_STK_TICK_PERIOD_US;

    if ((ticks == 0U) && (ms != 0U))
    {
        ticks = 1U;
    }
    if (ticks >= static_cast<uint64_t>(stk::WAIT_INFINITE))
    {
        ticks = static_cast<uint64_t>(stk::WAIT_INFINITE) - 1U; // WAIT_INFINITE is reserved
    }

    return static_cast<uint32_t>(ticks);
}

stk::Timeout MsToTimeout(cy_time_t ms)
{
    stk::Timeout result;

    if (ms == CY_RTOS_NEVER_TIMEOUT)
    {
        result = stk::WAIT_INFINITE;
    }
    else if (ms == 0U)
    {
        result = stk::NO_WAIT;
    }
    else
    {
        result = static_cast<stk::Timeout>(MsToTicks(ms));
    }

    return result;
}

// Converts the timeout of a call that may block, checking that blocking is possible. While the
// scheduler is locked a call can not block (it would assert in STK), it degrades to NO_WAIT.
// Returns false and the status to report if the call is not allowed.
bool PrepareTimeout(cy_time_t timeout_ms, stk::Timeout &out, cy_rslt_t &error)
{
    bool ok = true;

    out = stk::NO_WAIT;

    if (timeout_ms != 0U)
    {
        if (InIsr())
        {
            error = CY_RTOS_BAD_PARAM; // timeout must be zero in an ISR
            ok    = false;
        }
        else
        {
            const stk::IKernel::EKernelState st = KState();

            if (st == stk::IKernel::KSTATE_RUNNING)
            {
                out = MsToTimeout(timeout_ms);
            }
            else if (st != stk::IKernel::KSTATE_LOCKED)
            {
                error = Fail(CY_RTOS_STK_ERR_CONTEXT); // kernel is not running
                ok    = false;
            }
            else
            {
                // locked: NO_WAIT
            }
        }
    }

    return ok;
}

// Maps the outcome of a wait. \a timeout_status is reported for NO_WAIT and for an expired timeout.
cy_rslt_t WaitResultToCy(stk::EWaitResult wr, cy_rslt_t timeout_status)
{
    cy_rslt_t result;

    switch (wr)
    {
    case stk::WAIT_RESULT_SIGNAL:
        result = CY_RSLT_SUCCESS;
        break;

    case stk::WAIT_RESULT_TIMEOUT:
        result = timeout_status;
        break;

    case stk::WAIT_RESULT_CANCELED:
        result = Fail(CY_RTOS_STK_ERR_WAIT_CANCELED);
        break;

    case stk::WAIT_RESULT_FAIL:
    default:
        result = Fail(CY_RTOS_STK_ERR_WAIT_FAILED);
        break;
    }

    return result;
}

} // namespace
// ----------------------------------------------------------------------------

// ============================================================================
// ==== Thread control block ====
//
// Allocated by cy_rtos_thread_create() (together with the stack if the caller gave none), released by
// cy_rtos_thread_join(). The cy_thread_t handle is a pointer to it. Only threads created through the
// abstraction layer are known to it: they are kept in a registry to resolve the calling thread.
// ============================================================================

struct cy_stk_thread final : public stk::ITask
{
    STK_NONCOPYABLE_CLASS(cy_stk_thread);

    cy_stk_thread(cy_thread_entry_fn_t entry, cy_thread_arg_t arg, const char *name, stk::Word *stack,
                  size_t stack_words, void *raw, uint32_t priority)
        : m_magic(ID_THREAD), m_entry(entry), m_arg(arg), m_name(name), m_stack(stack),
          m_stack_words(stack_words), m_raw(raw), m_priority(static_cast<stk::Weight>(priority)),
          m_finished(false), m_removed(false), m_next(nullptr), m_notify(0U, 1U)
    {}

    virtual ~cy_stk_thread()
    {}

    void Run() override
    {
        m_entry(m_arg);
        m_finished = true; // the kernel removes the task and calls OnExit()
    }

    // Called by the kernel from the tick handler once the task is removed.
    void OnExit() override
    {
        m_removed = true;
    }

    const stk::Word *GetStack() const override { return m_stack; }
    size_t GetStackSize() const override { return m_stack_words; }
    stk::EAccessMode GetAccessMode() const override { return stk::ACCESS_PRIVILEGED; }
    stk::Weight GetWeight() const override { return m_priority; }
    const char *GetTraceName() const override { return m_name; }

    uint32_t                                m_magic;
    cy_thread_entry_fn_t                    m_entry;
    cy_thread_arg_t                         m_arg;
    const char                             *m_name;
    stk::Word                              *m_stack;
    size_t                                  m_stack_words;
    void                                   *m_raw;      // block to free (control block [+ stack])
    stk::Weight                             m_priority;
    volatile bool                           m_finished;  // Run() returned, or exit/terminate requested
    volatile bool                           m_removed;   // the kernel does not reference the task
    cy_stk_thread                          *m_next;      // registry
    stk::sync::SemaphoreT<uint16_t>         m_notify;    // binary semaphore of the thread notification
};

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

cy_stk_thread *g_Threads = nullptr; // registry, guarded by a critical section

void RegistryAdd(cy_stk_thread *th)
{
    const stk::sync::ScopedCriticalSection cs_;

    th->m_next = g_Threads;
    g_Threads  = th;
}

void RegistryRemove(cy_stk_thread *th)
{
    const stk::sync::ScopedCriticalSection cs_;

    for (cy_stk_thread **pp = &g_Threads; (*pp != nullptr); pp = &(*pp)->m_next)
    {
        if (*pp == th)
        {
            *pp = th->m_next;
            break;
        }
    }
}

cy_stk_thread *RegistryFind(const stk::ITask *task)
{
    const stk::sync::ScopedCriticalSection cs_;

    cy_stk_thread *th = g_Threads;

    while ((th != nullptr) && (static_cast<const stk::ITask *>(th) != task))
    {
        th = th->m_next;
    }

    return th;
}

// Calling thread, nullptr in an ISR, before the kernel runs, or for a task which was not created by
// cy_rtos_thread_create() (for example the tasks of the timer host).
cy_stk_thread *CurrentThread()
{
    cy_stk_thread *th = nullptr;

    if (!InIsr())
    {
        const stk::IKernel::EKernelState st = KState();

        if ((st == stk::IKernel::KSTATE_RUNNING) || (st == stk::IKernel::KSTATE_LOCKED))
        {
            const stk::TId tid = stk::GetTid();

            if ((tid != stk::TID_NONE) && !stk::IsIsrTid(tid))
            {
                th = RegistryFind(stk::GetUserTaskFromTid(tid));
            }
        }
    }

    return th;
}

__stk_forceinline cy_stk_thread *ValidThread(const cy_thread_t *handle)
{
    cy_stk_thread *th = ((handle != nullptr) ? *handle : nullptr);

    return (((th != nullptr) && (th->m_magic == ID_THREAD)) ? th : nullptr);
}

__stk_forceinline uintptr_t AlignUp(uintptr_t value, uintptr_t alignment)
{
    return ((value + (alignment - 1U)) & ~(alignment - 1U));
}

// ----------------------------------------------------------------------------
} // namespace
// ----------------------------------------------------------------------------

// ============================================================================
// ==== Kernel control (cyabs_rtos_stk.h) ====
// ============================================================================

extern "C" {

cy_rslt_t cy_rtos_stk_kernel_init(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    if (KState() == stk::IKernel::KSTATE_INACTIVE)
    {
        g_Kernel.Initialize(CY_RTOS_STK_TICK_PERIOD_US);

#if CY_RTOS_STK_USE_TIMERS
        stk::time::TimerHost::Config cfg;

        // the tick task and the handler task run at the same level, preemption of callers is not wanted
        cfg.tick_weight    = static_cast<stk::Weight>(CY_RTOS_STK_TIMER_PRIORITY);
        cfg.handler_weight = static_cast<stk::Weight>(CY_RTOS_STK_TIMER_PRIORITY);
        cfg.tick_mode      = stk::ACCESS_PRIVILEGED;

        if (g_TimerHost.Initialize(&g_Kernel, stk::ACCESS_PRIVILEGED, cfg) != stk::ADD_OK)
        {
            result = CY_RTOS_NO_MEMORY;
        }
#endif
    }

    return result;
}

void cy_rtos_stk_kernel_start(void)
{
    g_Kernel.Start();
}

cy_rslt_t cy_rtos_stk_kernel_enter(void (*app_init)(void))
{
    cy_rslt_t result = cy_rtos_stk_kernel_init();

    if (result == CY_RSLT_SUCCESS)
    {
        if (app_init != nullptr)
        {
            app_init();
        }

        g_Kernel.Start(); // returns when all threads have exited
    }

    return result;
}

} // extern "C"

stk::IKernel *cy_rtos_stk_get_kernel(void)
{
    return &g_Kernel;
}

// ============================================================================
// ==== Last error ====
// ============================================================================

extern "C" {

cy_rtos_error_t cy_rtos_last_error(void)
{
    return g_LastError;
}

// ============================================================================
// ==== Threads ====
// ============================================================================

cy_rslt_t cy_rtos_thread_create(cy_thread_t *thread, cy_thread_entry_fn_t entry_function,
                                const char *name, void *stack, uint32_t stack_size,
                                cy_thread_priority_t priority, cy_thread_arg_t arg)
{
    const uintptr_t stack_align = STK_STACK_MEMORY_ALIGN;

    if (0U != (stack_size & CY_RTOS_ALIGNMENT_MASK))
    {
        return CY_RTOS_ALIGNMENT_ERROR;
    }
    if ((thread == nullptr) || (entry_function == nullptr) || (stack_size < CY_RTOS_MIN_STACK_SIZE) ||
        (static_cast<uint32_t>(priority) >= CY_RTOS_STK_PRIORITY_LEVELS))
    {
        return CY_RTOS_BAD_PARAM;
    }
    if ((stack != nullptr) && (0U != (reinterpret_cast<uintptr_t>(stack) & (stack_align - 1U))))
    {
        return CY_RTOS_ALIGNMENT_ERROR;
    }
    if (InIsr())
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }

    // one block: control block, and the stack unless the caller supplied it
    const size_t hdr   = static_cast<size_t>(AlignUp(sizeof(cy_stk_thread), stack_align));
    size_t       total = hdr;
    if (stack == nullptr)
    {
        total += (static_cast<size_t>(stack_size) + stack_align); // + room for aligning the stack start
    }

    void *const raw = CY_RTOS_STK_MALLOC(total);
    if (raw == nullptr)
    {
        return CY_RTOS_NO_MEMORY;
    }

    if (stack == nullptr)
    {
        stack = reinterpret_cast<void *>(AlignUp(reinterpret_cast<uintptr_t>(raw) + hdr, stack_align));
    }

    cy_stk_thread *const th = new (raw) cy_stk_thread(entry_function, arg, name,
        static_cast<stk::Word *>(stack), (stack_size / sizeof(stk::Word)), raw,
        static_cast<uint32_t>(priority));

    RegistryAdd(th);
    *thread = th; // before the thread can run

    cy_rslt_t result;

    switch (g_Kernel.AddTask(th))
    {
    case stk::ADD_OK:
        result = CY_RSLT_SUCCESS;
        break;
    case stk::ADD_FAIL_POOL_FULL: // increase CY_RTOS_STK_MAX_THREADS
        g_LastError = CY_RTOS_STK_ERR_NONE;
        result      = CY_RTOS_NO_MEMORY;
        break;
    case stk::ADD_FAIL_DUPLICATE:
        result = Fail(CY_RTOS_STK_ERR_DUPLICATE);
        break;
    default: // ADD_FAIL_INVALID: cy_rtos_stk_kernel_init() was not called
        result = Fail(CY_RTOS_STK_ERR_KERNEL_STATE);
        break;
    }

    if (result != CY_RSLT_SUCCESS)
    {
        *thread = nullptr;
        RegistryRemove(th);
        th->m_magic = 0U;
        th->~cy_stk_thread();
        CY_RTOS_STK_FREE(raw);
    }

    return result;
}

cy_rslt_t cy_rtos_thread_exit(void)
{
    cy_stk_thread *const th = CurrentThread();

    if (th == nullptr)
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }

    // Like RTX/FreeRTOS: does not return. (Returning from the entry function is equivalent.)
    th->m_finished = true;
    stk::ExitTask();

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_thread_terminate(const cy_thread_t *thread)
{
    cy_stk_thread *const th = ValidThread(thread);

    if (th == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }
    if (th->m_finished || th->m_removed)
    {
        return CY_RSLT_SUCCESS; // already done, join it
    }
    if (InIsr())
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }

    const bool is_self = (CurrentThread() == th);

    {
        const stk::sync::ScopedCriticalSection cs_;
        th->m_finished = true;
    }

    if (is_self)
    {
        stk::ExitTask(); // switched out and removed by the kernel, does not return
    }
    else if (KState() == stk::IKernel::KSTATE_INACTIVE)
    {
        return Fail(CY_RTOS_STK_ERR_KERNEL_STATE);
    }
    else if (KState() == stk::IKernel::KSTATE_READY)
    {
        g_Kernel.RemoveTask(th); // the kernel has not started: nothing will call OnExit()
        th->m_removed = true;
    }
    else
    {
        g_Kernel.ScheduleTaskRemoval(th); // OnExit() fires from the kernel tick
    }

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_thread_join(cy_thread_t *thread)
{
    cy_stk_thread *const th = ValidThread(thread);

    if (th == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }
    if (th == CurrentThread())
    {
        return CY_RTOS_BAD_PARAM; // joining itself never completes
    }

    // The kernel releases the task a tick after it has finished. Polling keeps the kernel free of
    // callbacks from its tick handler (the same approach as the ThreadX port).
    while (!th->m_removed)
    {
        if (InIsr() || !SchedulerRunning())
        {
            return Fail(CY_RTOS_STK_ERR_CONTEXT);
        }
        if (!stk::Sleep(1))
        {
            return Fail(CY_RTOS_STK_ERR_WAIT_FAILED);
        }
    }

    void *const raw = th->m_raw;

    RegistryRemove(th);
    th->m_magic = 0U;
    th->~cy_stk_thread();
    CY_RTOS_STK_FREE(raw);

    *thread = nullptr;

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_thread_is_running(const cy_thread_t *thread, bool *running)
{
    cy_stk_thread *const th = ValidThread(thread);

    if ((th == nullptr) || (running == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    // Only the calling thread is running (single core)
    *running = (th == CurrentThread());

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_thread_get_state(const cy_thread_t *thread, cy_thread_state_t *state)
{
    cy_stk_thread *const th = ValidThread(thread);

    if ((th == nullptr) || (state == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    if (th == CurrentThread())
    {
        *state = CY_THREAD_STATE_RUNNING;
    }
    else if (th->m_removed || th->m_finished)
    {
        *state = CY_THREAD_STATE_TERMINATED;
    }
    else
    {
        stk::TaskStateInfo info = { stk::TASK_STATE_REMOVED, nullptr, stk::WAIT_INFINITE };

        if (!g_Kernel.GetTaskState(th, info))
        {
            *state = CY_THREAD_STATE_UNKNOWN;
        }
        else
        {
            switch (info.state)
            {
            case stk::TASK_STATE_SLEEPING:
            case stk::TASK_STATE_WAITING:
            case stk::TASK_STATE_SUSPENDED:
                *state = CY_THREAD_STATE_BLOCKED;
                break;
            case stk::TASK_STATE_EXITING:
                *state = CY_THREAD_STATE_TERMINATED;
                break;
            case stk::TASK_STATE_READY:
            case stk::TASK_STATE_REMOVED: // added, not bound by a running kernel yet
            default:
                *state = CY_THREAD_STATE_READY;
                break;
            }
        }
    }

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_thread_get_handle(cy_thread_t *thread)
{
    if (thread == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }

    cy_stk_thread *const th = CurrentThread();

    if (th == nullptr)
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT); // ISR, or not a thread of the abstraction layer
    }

    *thread = th;

    return CY_RSLT_SUCCESS;
}

// The notification is a binary semaphore owned by the thread: a notification sent while the
// thread is not waiting is kept and satisfies its next wait (like FreeRTOS/RTX, unlike the ThreadX port).
cy_rslt_t cy_rtos_thread_wait_notification(cy_time_t timeout_ms)
{
    cy_stk_thread *const th = CurrentThread();

    if (th == nullptr)
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }

    stk::Timeout timeout;
    cy_rslt_t    error = CY_RSLT_SUCCESS;

    if (!PrepareTimeout(timeout_ms, timeout, error))
    {
        return error;
    }

    return WaitResultToCy(th->m_notify.WaitEx(timeout), CY_RTOS_TIMEOUT);
}

cy_rslt_t cy_rtos_thread_set_notification(const cy_thread_t *thread)
{
    cy_stk_thread *const th = ValidThread(thread);

    if (th == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }

    // ISR-safe. A notification which is already pending is not counted (binary).
    (void)th->m_notify.TrySignal();

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_thread_get_name(const cy_thread_t *thread, const char **thread_name)
{
    cy_stk_thread *const th = ValidThread(thread);

    if ((th == nullptr) || (thread_name == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    *thread_name = th->m_name;

    return CY_RSLT_SUCCESS;
}

// ============================================================================
// ==== Scheduler ====
//
// Backed by stk::LockScheduler(): task switching stops while ticks, timeouts and ISRs keep running.
// STK does not nest the lock, so the nesting is counted here.
// ============================================================================

cy_rslt_t cy_rtos_scheduler_suspend(void)
{
    if (InIsr())
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }

    const stk::IKernel::EKernelState st = KState();

    // before Start() nothing is switched: only the nesting is recorded
    if ((st == stk::IKernel::KSTATE_RUNNING) || (st == stk::IKernel::KSTATE_LOCKED))
    {
        const stk::ELockResult lr = stk::LockScheduler();

        if ((lr != stk::LOCK_WAS_UNLOCKED) && (lr != stk::LOCK_WAS_LOCKED))
        {
            return Fail(CY_RTOS_STK_ERR_CONTEXT);
        }
    }

    ++g_SuspendCount;

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_scheduler_resume(void)
{
    if (g_SuspendCount == 0U)
    {
        return CY_RTOS_BAD_PARAM;
    }

    if ((--g_SuspendCount == 0U) && (KState() == stk::IKernel::KSTATE_LOCKED))
    {
        (void)stk::UnlockScheduler();
    }

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_scheduler_get_state(cy_scheduler_state_t *state)
{
    if (state == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }

    switch (KState())
    {
    case stk::IKernel::KSTATE_INACTIVE:
    case stk::IKernel::KSTATE_READY:
        *state = ((g_SuspendCount != 0U) ? CY_SCHEDULER_STATE_SUSPENDED : CY_SCHEDULER_STATE_NOT_STARTED);
        break;
    case stk::IKernel::KSTATE_SUSPENDED:
    case stk::IKernel::KSTATE_LOCKED:
        *state = CY_SCHEDULER_STATE_SUSPENDED;
        break;
    case stk::IKernel::KSTATE_RUNNING:
    default:
        *state = CY_SCHEDULER_STATE_RUNNING;
        break;
    }

    return CY_RSLT_SUCCESS;
}

} // extern "C"

// ============================================================================
// ==== Mutexes ====
//
// stk::sync::Mutex: always recursive, with priority inheritance. A non-recursive mutex is not
// available in STK, the "recursive" argument is accepted and ignored (like the ThreadX port).
// ============================================================================

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

STK_STATIC_ASSERT_DESC(sizeof(stk::sync::Mutex) <= sizeof(static_cast<cy_mutex_t *>(nullptr)->storage),
    "CY_RTOS_STK_MUTEX_WORDS is too small for stk::sync::Mutex");
STK_STATIC_ASSERT_DESC(alignof(stk::sync::Mutex) <= alignof(uint64_t), "stk::sync::Mutex alignment");

__stk_forceinline stk::sync::Mutex *MutexOf(const cy_mutex_t *m)
{
    return static_cast<stk::sync::Mutex *>(static_cast<void *>(const_cast<uint64_t *>(m->storage)));
}

__stk_forceinline bool IsValid(const cy_mutex_t *m)
{
    return ((m != nullptr) && (m->id == ID_MUTEX));
}

} // namespace
// ----------------------------------------------------------------------------

extern "C" {

cy_rslt_t cy_rtos_mutex_init(cy_mutex_t *mutex, bool recursive)
{
    (void)recursive;

    if (mutex == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }

    (void)new (static_cast<void *>(mutex->storage)) stk::sync::Mutex();
    mutex->id = ID_MUTEX;

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_mutex_get(const cy_mutex_t *mutex, cy_time_t timeout_ms)
{
    if (!IsValid(mutex) || InIsr())
    {
        return CY_RTOS_BAD_PARAM;
    }

    const stk::IKernel::EKernelState st = KState();
    if ((st != stk::IKernel::KSTATE_RUNNING) && (st != stk::IKernel::KSTATE_LOCKED))
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }

    stk::Timeout timeout;
    cy_rslt_t    error = CY_RSLT_SUCCESS;

    if (!PrepareTimeout(timeout_ms, timeout, error))
    {
        return error;
    }

    return WaitResultToCy(MutexOf(mutex)->TimedLockEx(timeout), CY_RTOS_TIMEOUT);
}

cy_rslt_t cy_rtos_mutex_set(const cy_mutex_t *mutex)
{
    if (!IsValid(mutex))
    {
        return CY_RTOS_BAD_PARAM;
    }
    if (InIsr() || (KState() == stk::IKernel::KSTATE_INACTIVE) || (KState() == stk::IKernel::KSTATE_READY))
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }
    if (MutexOf(mutex)->GetOwner() != stk::GetTid())
    {
        return Fail(CY_RTOS_STK_ERR_NOT_OWNER);
    }

    MutexOf(mutex)->Unlock();

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_mutex_deinit(cy_mutex_t *mutex)
{
    if (!IsValid(mutex))
    {
        return CY_RTOS_BAD_PARAM;
    }

    mutex->id = 0U;
    MutexOf(mutex)->~Mutex(); // STK contract: no thread may be waiting on it

    return CY_RSLT_SUCCESS;
}

} // extern "C"

// ============================================================================
// ==== Semaphores ====
// ============================================================================

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

typedef stk::sync::SemaphoreT<uint32_t> Semaphore;

STK_STATIC_ASSERT_DESC(sizeof(Semaphore) <= sizeof(static_cast<cy_semaphore_t *>(nullptr)->storage),
    "CY_RTOS_STK_SEMAPHORE_WORDS is too small for stk::sync::SemaphoreT<uint32_t>");
STK_STATIC_ASSERT_DESC(alignof(Semaphore) <= alignof(uint64_t), "semaphore alignment");

__stk_forceinline Semaphore *SemOf(const cy_semaphore_t *s)
{
    return static_cast<Semaphore *>(static_cast<void *>(const_cast<uint64_t *>(s->storage)));
}

__stk_forceinline bool IsValid(const cy_semaphore_t *s)
{
    return ((s != nullptr) && (s->id == ID_SEMAPHORE));
}

} // namespace
// ----------------------------------------------------------------------------

extern "C" {

cy_rslt_t cy_rtos_semaphore_init(cy_semaphore_t *semaphore, uint32_t maxcount, uint32_t initcount)
{
    if ((semaphore == nullptr) || (maxcount == 0U) || (initcount > maxcount))
    {
        return CY_RTOS_BAD_PARAM;
    }

    (void)new (static_cast<void *>(semaphore->storage)) Semaphore(initcount, maxcount);
    semaphore->id = ID_SEMAPHORE;

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_semaphore_get(const cy_semaphore_t *semaphore, cy_time_t timeout_ms)
{
    if (!IsValid(semaphore))
    {
        return CY_RTOS_BAD_PARAM;
    }

    stk::Timeout timeout;
    cy_rslt_t    error = CY_RSLT_SUCCESS;

    if (!PrepareTimeout(timeout_ms, timeout, error))
    {
        return error;
    }

    return WaitResultToCy(SemOf(semaphore)->WaitEx(timeout), CY_RTOS_TIMEOUT);
}

cy_rslt_t cy_rtos_semaphore_set(const cy_semaphore_t *semaphore)
{
    if (!IsValid(semaphore))
    {
        return CY_RTOS_BAD_PARAM;
    }

    // ISR-safe; the count is limited by the maximum given to cy_rtos_semaphore_init()
    return (SemOf(semaphore)->TrySignal() ? CY_RSLT_SUCCESS : Fail(CY_RTOS_STK_ERR_LIMIT));
}

cy_rslt_t cy_rtos_semaphore_get_count(const cy_semaphore_t *semaphore, size_t *count)
{
    if (!IsValid(semaphore) || (count == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    *count = static_cast<size_t>(SemOf(semaphore)->GetCount());

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_semaphore_deinit(cy_semaphore_t *semaphore)
{
    if (!IsValid(semaphore))
    {
        return CY_RTOS_BAD_PARAM;
    }

    semaphore->id = 0U;
    SemOf(semaphore)->~Semaphore(); // STK contract: no thread may be waiting on it

    return CY_RSLT_SUCCESS;
}

} // extern "C"

// ============================================================================
// ==== Events ====
//
// stk::sync::EventFlagsT<uint64_t>: the 32 flags live in the low half of the word, the reserved
// error bit of STK is bit 63, so all 32 bits are usable.
// ============================================================================

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

typedef stk::sync::EventFlagsT<uint64_t> EventFlags;

STK_STATIC_ASSERT_DESC(sizeof(EventFlags) <= sizeof(static_cast<cy_event_t *>(nullptr)->storage),
    "CY_RTOS_STK_EVENT_WORDS is too small for stk::sync::EventFlagsT<uint64_t>");
STK_STATIC_ASSERT_DESC(alignof(EventFlags) <= alignof(uint64_t), "event flags alignment");

__stk_forceinline EventFlags *EventOf(const cy_event_t *e)
{
    return static_cast<EventFlags *>(static_cast<void *>(const_cast<uint64_t *>(e->storage)));
}

__stk_forceinline bool IsValid(const cy_event_t *e)
{
    return ((e != nullptr) && (e->id == ID_EVENT));
}

} // namespace
// ----------------------------------------------------------------------------

extern "C" {

cy_rslt_t cy_rtos_event_init(cy_event_t *event)
{
    if (event == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }

    (void)new (static_cast<void *>(event->storage)) EventFlags(0U);
    event->id = ID_EVENT;

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_event_setbits(const cy_event_t *event, uint32_t bits)
{
    if (!IsValid(event))
    {
        return CY_RTOS_BAD_PARAM;
    }

    if (bits != 0U)
    {
        (void)EventOf(event)->Set(static_cast<uint64_t>(bits)); // ISR-safe, wakes the waiters
    }

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_event_clearbits(const cy_event_t *event, uint32_t bits)
{
    if (!IsValid(event))
    {
        return CY_RTOS_BAD_PARAM;
    }

    if (bits != 0U)
    {
        (void)EventOf(event)->Clear(static_cast<uint64_t>(bits));
    }

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_event_getbits(const cy_event_t *event, uint32_t *bits)
{
    if (!IsValid(event) || (bits == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    *bits = static_cast<uint32_t>(EventOf(event)->Get());

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_event_waitbits(const cy_event_t *event, uint32_t *bits, bool clear, bool all,
                                 cy_time_t timeout_ms)
{
    if (!IsValid(event) || (bits == nullptr) || (*bits == 0U))
    {
        return CY_RTOS_BAD_PARAM;
    }

    stk::Timeout timeout;
    cy_rslt_t    error = CY_RSLT_SUCCESS;

    if (!PrepareTimeout(timeout_ms, timeout, error))
    {
        return error;
    }

    uint32_t options = (all ? EventFlags::OPT_WAIT_ALL : EventFlags::OPT_WAIT_ANY);
    if (!clear)
    {
        options |= EventFlags::OPT_NO_CLEAR;
    }

    uint64_t actual = 0U;

    const cy_rslt_t result = WaitResultToCy(
        EventOf(event)->WaitEx(static_cast<uint64_t>(*bits), options, timeout, actual), CY_RTOS_TIMEOUT);

    if (result == CY_RSLT_SUCCESS)
    {
        // the whole flags word at the moment the request was satisfied (before auto-clear)
        *bits = static_cast<uint32_t>(actual);
    }

    return result;
}

cy_rslt_t cy_rtos_event_deinit(cy_event_t *event)
{
    if (!IsValid(event))
    {
        return CY_RTOS_BAD_PARAM;
    }

    event->id = 0U;
    EventOf(event)->~EventFlags(); // STK contract: no thread may be waiting on it

    return CY_RSLT_SUCCESS;
}

} // extern "C"

// ============================================================================
// ==== Queues ====
//
// stk::sync::MessageQueue over a heap block of length * itemsize bytes. Unlike ThreadX any item size
// is supported, items are copied as they are.
// ============================================================================

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

STK_STATIC_ASSERT_DESC(sizeof(stk::sync::MessageQueue) <= sizeof(static_cast<cy_queue_t *>(nullptr)->storage),
    "CY_RTOS_STK_QUEUE_WORDS is too small for stk::sync::MessageQueue");
STK_STATIC_ASSERT_DESC(alignof(stk::sync::MessageQueue) <= alignof(uint64_t), "queue alignment");

__stk_forceinline stk::sync::MessageQueue *QueueOf(const cy_queue_t *q)
{
    return static_cast<stk::sync::MessageQueue *>(static_cast<void *>(const_cast<uint64_t *>(q->storage)));
}

__stk_forceinline bool IsValid(const cy_queue_t *q)
{
    return ((q != nullptr) && (q->id == ID_QUEUE));
}

} // namespace
// ----------------------------------------------------------------------------

extern "C" {

cy_rslt_t cy_rtos_queue_init(cy_queue_t *queue, size_t length, size_t itemsize)
{
    if ((queue == nullptr) || (length == 0U) || (itemsize == 0U) ||
        (length > stk::sync::MessageQueue::CAPACITY_MAX) || (itemsize > (SIZE_MAX / length)))
    {
        return CY_RTOS_BAD_PARAM;
    }

    uint8_t *const buffer = static_cast<uint8_t *>(CY_RTOS_STK_MALLOC(length * itemsize));
    if (buffer == nullptr)
    {
        return CY_RTOS_NO_MEMORY;
    }

    (void)new (static_cast<void *>(queue->storage)) stk::sync::MessageQueue(buffer, length, itemsize);
    queue->buffer = buffer;
    queue->id     = ID_QUEUE;

    return CY_RSLT_SUCCESS;
}

// A full queue is reported as CY_RTOS_QUEUE_FULL (the ThreadX port returns CY_RTOS_NO_MEMORY).
cy_rslt_t cy_rtos_queue_put(const cy_queue_t *queue, const void *item_ptr, cy_time_t timeout_ms)
{
    if (!IsValid(queue) || (item_ptr == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    stk::Timeout timeout;
    cy_rslt_t    error = CY_RSLT_SUCCESS;

    if (!PrepareTimeout(timeout_ms, timeout, error))
    {
        return error;
    }

    return WaitResultToCy(QueueOf(queue)->PutEx(item_ptr, timeout), CY_RTOS_QUEUE_FULL);
}

cy_rslt_t cy_rtos_queue_get(const cy_queue_t *queue, void *item_ptr, cy_time_t timeout_ms)
{
    if (!IsValid(queue) || (item_ptr == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    stk::Timeout timeout;
    cy_rslt_t    error = CY_RSLT_SUCCESS;

    if (!PrepareTimeout(timeout_ms, timeout, error))
    {
        return error;
    }

    return WaitResultToCy(QueueOf(queue)->GetEx(item_ptr, timeout), CY_RTOS_TIMEOUT);
}

cy_rslt_t cy_rtos_queue_count(const cy_queue_t *queue, size_t *num_waiting)
{
    if (!IsValid(queue) || (num_waiting == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    *num_waiting = QueueOf(queue)->GetCount();

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_queue_space(const cy_queue_t *queue, size_t *num_spaces)
{
    if (!IsValid(queue) || (num_spaces == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    *num_spaces = QueueOf(queue)->GetSpace();

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_queue_reset(const cy_queue_t *queue)
{
    if (!IsValid(queue))
    {
        return CY_RTOS_BAD_PARAM;
    }

    QueueOf(queue)->Reset();

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_queue_deinit(cy_queue_t *queue)
{
    if (!IsValid(queue))
    {
        return CY_RTOS_BAD_PARAM;
    }

    queue->id = 0U;
    QueueOf(queue)->~MessageQueue(); // STK contract: no thread may be waiting on it
    CY_RTOS_STK_FREE(queue->buffer);
    queue->buffer = nullptr;

    return CY_RSLT_SUCCESS;
}

} // extern "C"

// ============================================================================
// ==== Timers ====
//
// stk::time::TimerHost in task mode (see CY_RTOS_STK_USE_TIMERS). The callbacks run in the handler
// task of the host, at CY_RTOS_STK_TIMER_PRIORITY, never in an ISR. The synchronous TimerHost API is
// used: when cy_rtos_timer_stop() returns the timer will not fire any more, and when
// cy_rtos_timer_deinit() returns the storage can be reused.
// ============================================================================

#if CY_RTOS_STK_USE_TIMERS

// ----------------------------------------------------------------------------
namespace {
// ----------------------------------------------------------------------------

class CyTimer final : public stk::time::Timer
{
    STK_NONCOPYABLE_CLASS(CyTimer);

public:
    CyTimer(bool oneshot, cy_timer_callback_t fn, cy_timer_callback_arg_t arg)
        : m_oneshot(oneshot), m_fn(fn), m_arg(arg)
    {}

    virtual ~CyTimer()
    {}

    // runs in the handler task of the timer host
    void OnExpired(stk::time::TimerHost *) override
    {
        m_fn(m_arg);
    }

    bool                      m_oneshot;
    cy_timer_callback_t       m_fn;
    cy_timer_callback_arg_t   m_arg;
};

STK_STATIC_ASSERT_DESC(sizeof(CyTimer) <= sizeof(static_cast<cy_timer_t *>(nullptr)->storage),
    "CY_RTOS_STK_TIMER_WORDS is too small for the timer object");
STK_STATIC_ASSERT_DESC(alignof(CyTimer) <= alignof(uint64_t), "timer alignment");

__stk_forceinline CyTimer *TimerOf(const cy_timer_t *t)
{
    return static_cast<CyTimer *>(static_cast<void *>(const_cast<uint64_t *>(t->storage)));
}

__stk_forceinline bool IsValid(const cy_timer_t *t)
{
    return ((t != nullptr) && (t->id == ID_TIMER));
}

// RES_QUEUED: called from an ISR or before the kernel runs, the command is executed later.
cy_rslt_t TimerResultToCy(stk::time::TimerHost::EResult res)
{
    cy_rslt_t result;

    switch (res)
    {
    case stk::time::TimerHost::RES_OK:
    case stk::time::TimerHost::RES_INACTIVE: // already stopped
    case stk::time::TimerHost::RES_QUEUED:
        result = CY_RSLT_SUCCESS;
        break;
    default:
        result = Fail(CY_RTOS_STK_ERR_TIMER_HOST);
        break;
    }

    return result;
}

} // namespace
// ----------------------------------------------------------------------------

extern "C" {

cy_rslt_t cy_rtos_timer_init(cy_timer_t *timer, cy_timer_trigger_type_t type,
                             cy_timer_callback_t fun, cy_timer_callback_arg_t arg)
{
    if ((timer == nullptr) || (fun == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }
    if (KState() == stk::IKernel::KSTATE_INACTIVE)
    {
        return Fail(CY_RTOS_STK_ERR_KERNEL_STATE); // the timer host starts with cy_rtos_stk_kernel_init()
    }

    (void)new (static_cast<void *>(timer->storage)) CyTimer((type == CY_TIMER_TYPE_ONCE), fun, arg);
    timer->id = ID_TIMER;

    return CY_RSLT_SUCCESS;
}

// Starts the timer, or restarts it if it is running.
cy_rslt_t cy_rtos_timer_start(const cy_timer_t *timer, cy_time_t num_ms)
{
    if (!IsValid(timer) || (num_ms == 0U) || (num_ms == CY_RTOS_NEVER_TIMEOUT))
    {
        return CY_RTOS_BAD_PARAM;
    }

    CyTimer *const t     = TimerOf(timer);
    const uint32_t ticks = MsToTicks(num_ms);

    return TimerResultToCy(g_TimerHost.RestartSync(*t, ticks, (t->m_oneshot ? 0U : ticks)));
}

cy_rslt_t cy_rtos_timer_stop(const cy_timer_t *timer)
{
    if (!IsValid(timer))
    {
        return CY_RTOS_BAD_PARAM;
    }

    return TimerResultToCy(g_TimerHost.StopSync(*TimerOf(timer)));
}

cy_rslt_t cy_rtos_timer_is_running(const cy_timer_t *timer, bool *state)
{
    if (!IsValid(timer) || (state == nullptr))
    {
        return CY_RTOS_BAD_PARAM;
    }

    stk::time::TimerHost::TimerInfo info = {};
    const stk::time::TimerHost::EResult res = g_TimerHost.QuerySync(*TimerOf(timer), info);

    if (res == stk::time::TimerHost::RES_OK)
    {
        *state = info.active;
    }
    else if (res == stk::time::TimerHost::RES_QUEUED)
    {
        *state = TimerOf(timer)->IsActive(); // advisory (ISR, or the kernel is not running)
    }
    else
    {
        return Fail(CY_RTOS_STK_ERR_TIMER_HOST);
    }

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_timer_deinit(cy_timer_t *timer)
{
    if (!IsValid(timer))
    {
        return CY_RTOS_BAD_PARAM;
    }

    CyTimer *const t = TimerOf(timer);

    timer->id = 0U;
    (void)g_TimerHost.StopSync(*t); // afterwards the host holds no reference to the timer
    t->~CyTimer();

    return CY_RSLT_SUCCESS;
}

} // extern "C"

#else // CY_RTOS_STK_USE_TIMERS

extern "C" {

cy_rslt_t cy_rtos_timer_init(cy_timer_t *timer, cy_timer_trigger_type_t type,
                             cy_timer_callback_t fun, cy_timer_callback_arg_t arg)
{
    (void)timer; (void)type; (void)fun; (void)arg;
    return CY_RTOS_UNSUPPORTED;
}

cy_rslt_t cy_rtos_timer_start(const cy_timer_t *timer, cy_time_t num_ms)
{
    (void)timer; (void)num_ms;
    return CY_RTOS_UNSUPPORTED;
}

cy_rslt_t cy_rtos_timer_stop(const cy_timer_t *timer)
{
    (void)timer;
    return CY_RTOS_UNSUPPORTED;
}

cy_rslt_t cy_rtos_timer_is_running(const cy_timer_t *timer, bool *state)
{
    (void)timer; (void)state;
    return CY_RTOS_UNSUPPORTED;
}

cy_rslt_t cy_rtos_timer_deinit(cy_timer_t *timer)
{
    (void)timer;
    return CY_RTOS_UNSUPPORTED;
}

} // extern "C"

#endif // CY_RTOS_STK_USE_TIMERS

// ============================================================================
// ==== Time ====
// ============================================================================

extern "C" {

cy_rslt_t cy_rtos_time_get(cy_time_t *tval)
{
    if (tval == nullptr)
    {
        return CY_RTOS_BAD_PARAM;
    }

    // milliseconds since the kernel started, wraps after 49 days like cy_time_t
    *tval = ((KState() == stk::IKernel::KSTATE_INACTIVE) ? 0U :
             static_cast<cy_time_t>(stk::GetTimeNowMs()));

    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_rtos_delay_milliseconds(cy_time_t num_ms)
{
    if (num_ms == 0U)
    {
        return CY_RSLT_SUCCESS;
    }
    if (InIsr() || !SchedulerRunning())
    {
        return Fail(CY_RTOS_STK_ERR_CONTEXT);
    }

    return (stk::Sleep(MsToTimeout(num_ms)) ? CY_RSLT_SUCCESS : Fail(CY_RTOS_STK_ERR_WAIT_FAILED));
}

} // extern "C"
