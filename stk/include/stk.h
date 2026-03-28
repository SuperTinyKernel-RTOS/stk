/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_H_
#define STK_H_

#include "stk_helper.h"
#include "strategy/stk_strategy_rrobin.h"
#include "strategy/stk_strategy_swrrobin.h"
#include "strategy/stk_strategy_monotonic.h"
#include "strategy/stk_strategy_edf.h"
#include "strategy/stk_strategy_fpriority.h"

/*! \file  stk.h
    \brief Top-level STK include. Provides the Kernel class template and all built-in
           task-switching strategies.

    Include this single header in user application code. It transitively pulls in:
     - stk_helper.h             — Task, TaskW, StackMemoryWrapper, and time/synchronization utilities.
     - stk_strategy_rrobin.h    — SwitchStrategyRoundRobin.
     - stk_strategy_swrrobin.h  — SwitchStrategySmoothWeightedRoundRobin.
     - stk_strategy_monotonic.h — SwitchStrategyMonotonic (SRT rate-monotonic).
     - stk_strategy_edf.h       — SwitchStrategyEdf (Earliest Deadline First).
     - stk_strategy_fpriority.h — SwitchStrategyFixedPriority.
*/

namespace stk {

/*! \class Kernel
    \brief Concrete implementation of IKernel.

    All configuration is expressed as template parameters. No virtual dispatch, no heap
    allocation - the entire kernel, tasks, and traps live in statically reserved storage.

    \tparam _Mode:       Bitmask of EKernelMode flags that configures kernel features:
                         - stk::KERNEL_STATIC  — fixed task list, no add/remove after Start().
                         - stk::KERNEL_DYNAMIC — tasks may be added or removed at runtime.
                         - stk::KERNEL_HRT     — Hard Real-Time mode (must combine with STATIC or DYNAMIC).
                         - stk::KERNEL_SYNC    — enables synchronization primitives (Mutex, Event, etc.).
                         KERNEL_STATIC and KERNEL_DYNAMIC are mutually exclusive.
    \tparam _Size:       Maximum number of concurrent tasks. Must be > 0.
    \tparam TStrategy: Task-switching strategy type (e.g. SwitchStrategyRoundRobin).
                         Must inherit ITaskSwitchStrategy.
    \tparam TPlatform: Platform driver type (e.g. PlatformArmCortexM, or PlatformDefault).
                         Must inherit IPlatform.

    \note  At least 1 task is required: _Size must be > 0 (enforced by compile-time assertion).
    \note  KERNEL_HRT is incompatible with weighted scheduling strategies (WEIGHT_API == true),
           also enforced by a compile-time assertion.

    Usage example:
    \code
    static stk::Kernel<KERNEL_STATIC, 3,
                       SwitchStrategyRoundRobin,
                       PlatformDefault> kernel;

    static MyTask1<256, ACCESS_PRIVILEGED> task1;
    static MyTask2<512, ACCESS_USER>       task2;
    static MyTask3<512, ACCESS_USER>       task3;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.Start();
    \endcode
*/
template <uint8_t TMode, uint32_t TSize, class TStrategy, class TPlatform>
class Kernel : public IKernel, private IPlatform::IEventHandler
{
protected:
    /*! \typedef SleepTrapStackMemory
        \brief   Stack memory wrapper type for the sleep trap.
        \see     SleepTrapStack, STK_SLEEP_TRAP_STACK_SIZE
    */
    typedef StackMemoryWrapper<STK_SLEEP_TRAP_STACK_SIZE> SleepTrapStackMemory;

    /*! \typedef ExitTrapStackMemory
        \brief   Stack memory wrapper type for the exit trap.
        \see     ExitTrapStack, STACK_SIZE_MIN
    */
    typedef StackMemoryWrapper<STACK_SIZE_MIN> ExitTrapStackMemory;

    /*! \enum  ERequest
        \brief Bitmask flags for pending inter-task requests that must be processed
               by the kernel on the next tick (in UpdateTaskRequest()).
    */
    enum ERequest : uint8_t
    {
        REQUEST_NONE     = 0,       //!< No pending requests.
        REQUEST_ADD_TASK = (1 << 0) //!< An AddTask() request is pending from a running task (KERNEL_DYNAMIC only).
    };

    /*! \class KernelTask
        \brief Internal per-slot kernel descriptor that wraps a user ITask instance.

        Holds the kernel-side state for one task slot: the Stack descriptor, sleep timer,
        HRT or SRT scheduling metadata, optional wait object (KERNEL_SYNC), and optional
        weight (weighted strategies). The task-switching strategy operates on KernelTask
        pointers rather than ITask pointers directly.

        A slot is "free" when m_user == NULL (IsBusy() == false). The Kernel pre-allocates
        _Size slots in m_task_storage; AddTask() finds a free slot and calls Bind().
    */
    class KernelTask : public IKernelTask
    {
        friend class Kernel;

        /*! \enum  EStateFlags
            \brief Bitmask of transient state flags. Set by the task or the kernel and
                   consumed (cleared) during UpdateTaskState() on the next tick.
        */
        enum EStateFlags
        {
            STATE_NONE           = 0,        //!< No pending state flags.
            STATE_REMOVE_PENDING = (1 << 0), //!< Task returned from its Run function; slot will be freed on the next tick (KERNEL_DYNAMIC only).
            STATE_SLEEP_PENDING  = (1 << 1)  //!< Task called Sleep/Yield; strategy's OnTaskSleep() will be invoked on the next tick (sleep-aware strategies only).
        };

    public:
        /*! \class AddTaskRequest
            \brief Payload for an in-flight AddTask() request issued by a running task.
            \note  Used in KERNEL_DYNAMIC mode only, when AddTask() is called after Start().
                   The requesting task stores this struct on its own stack and yields, the
                   kernel reads it on the next tick in UpdateTaskRequest() then clears the
                   pointer to signal completion. The struct must remain valid until then.
        */
        struct AddTaskRequest
        {
            ITask *user_task; //!< User task to add. Must remain valid for the lifetime of its kernel slot.
        };

        /*! \brief Construct a free (unbound) task slot. All fields set to zero/null.
            \note  In KERNEL_SYNC mode the embedded WaitObject back-pointer is wired to this
                   KernelTask at construction so the wait object can wake its owning task.
        */
        explicit KernelTask() : m_user(nullptr), m_stack(), m_state(STATE_NONE), m_time_sleep(0),
            m_srt(), m_hrt(), m_rt_weight()
        {
            // bind to wait object
            if (IsSyncMode())
                m_wait_obj->m_task = this;
        }

        /*! \brief  Get bound user task.
            \return Pointer to the ITask, or \c NULL if the slot is free (IsBusy() == false).
        */
        ITask *GetUserTask() { return m_user; }

        /*! \brief  Get stack descriptor for this task slot.
            \return Pointer to the Stack (SP register value and access mode flags).
        */
        Stack *GetUserStack() { return &m_stack; }

        /*! \brief  Check whether this slot is bound to a user task.
            \return \c true if a user task is assigned (m_user != NULL); \c false if the slot is free.
        */
        bool IsBusy() const { return (m_user != nullptr); }

        /*! \brief  Check whether this task is currently sleeping (waiting for a tick or a wake event).
            \return \c true if m_time_sleep < 0 (negative value encodes remaining sleep ticks).
        */
        bool IsSleeping() const { return (m_time_sleep < 0); }

        /*! \brief  Get task identifier.
            \return TId derived from the bound ITask pointer address (unique per task instance).
        */
        TId GetTid() const { return hw::PtrToWord(m_user); }

        /*! \brief  Wake this task on the next scheduling tick.
            \note   Sets m_time_sleep to -1 (one tick remaining) so the task exits sleep state
                    on the next UpdateTaskState() pass. Asserts that the task is currently sleeping.
        */
        void Wake()
        {
            STK_ASSERT(IsSleeping());

            // wakeup on a next cycle
            m_time_sleep = -1;
        }

        /*! \brief     Update the run-time scheduling weight (weighted strategies only).
            \param[in] weight: New current weight. Ignored unless TStrategy::WEIGHT_API is true.
        */
        void SetCurrentWeight(int32_t weight)
        {
            if (TStrategy::WEIGHT_API)
                m_rt_weight[0] = weight;
        }

        /*! \brief  Get static scheduling weight from the user task.
            \return ITask::GetWeight() if WEIGHT_API is true; 1 otherwise.
        */
        int32_t GetWeight() const { return (TStrategy::WEIGHT_API ? m_user->GetWeight() : 1); }

        /*! \brief  Get current (run-time) scheduling weight.
            \return m_rt_weight[0] if WEIGHT_API is true; 1 otherwise.
            \note   The run-time weight is decremented each tick by the weighted strategy and
                    reset to GetWeight() when exhausted.
        */
        int32_t GetCurrentWeight() const { return (TStrategy::WEIGHT_API ? m_rt_weight[0] : 1); }

        /*! \brief  Get HRT scheduling periodicity.
            \return Period in ticks between successive activations of this task.
            \note   KERNEL_HRT mode only. Asserts if called outside HRT mode.
        */
        Timeout GetHrtPeriodicity() const
        {
            STK_ASSERT(IsHrtMode());

            return (IsHrtMode() ? m_hrt[0].periodicity : 0);
        }

        /*! \brief  Get absolute HRT deadline (ticks elapsed since task was activated).
            \return Deadline in ticks. The task must complete its work within this many ticks
                    of being switched in, or OnDeadlineMissed() is invoked.
            \note   KERNEL_HRT mode only. Asserts if called outside HRT mode.
        */
        Timeout GetHrtDeadline() const
        {
            STK_ASSERT(IsHrtMode());

            return (IsHrtMode() ? m_hrt[0].deadline : 0);
        }

        /*! \brief  Get remaining HRT deadline (ticks left before the deadline expires).
            \return deadline - duration: ticks remaining before the task must call Yield().
                    A negative or zero value means the deadline has been missed.
            \note   KERNEL_HRT mode only. Asserts if called outside HRT mode or while sleeping.
        */
        Timeout GetHrtRelativeDeadline() const
        {
            STK_ASSERT(IsHrtMode());
            STK_ASSERT(!IsSleeping());

            return (IsHrtMode() ? (m_hrt[0].deadline - m_hrt[0].duration) : 0);
        }

    protected:
        /*! \brief Destructor.
            \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
        */
        ~KernelTask()
        {}

        /*! \class SrtInfo
            \brief Per-task soft real-time (SRT) metadata.
            \note  Allocated only when _Mode does not include KERNEL_HRT. Zero-size in HRT mode
                   (STK_ALLOCATE_COUNT resolves to 0 on GCC/Clang).
        */
        struct SrtInfo
        {
            SrtInfo() : add_task_req(nullptr)
            {}

            /*! \brief Clear all fields, ready for slot re-use.
            */
            void Clear()
            {
                add_task_req = nullptr;
            }

            /*! Pointer to a pending AddTaskRequest stored on the requesting task's stack.
                Non-null while the request is in flight, cleared to null by UpdateTaskRequest()
                once the new task has been added, signalling completion to the requesting task.
                \see AddTaskRequest, RequestAddTask, UpdateTaskRequest
            */
            AddTaskRequest *add_task_req;
        };

        /*! \class HrtInfo
            \brief Per-task Hard Real-Time (HRT) scheduling metadata.
            \note  Allocated only when _Mode includes KERNEL_HRT. Zero-size in SRT mode.
        */
        struct HrtInfo
        {
            HrtInfo() : periodicity(0), deadline(0), duration(0), done(false)
            {}

            /*! \brief Clear all fields, ready for slot re-use or re-activation.
            */
            void Clear()
            {
                periodicity = 0;
                deadline    = 0;
                duration    = 0;
                done        = false;
            }

            Timeout periodicity; //!< Activation period in ticks: the task is re-activated every this many ticks.
            Timeout deadline;    //!< Maximum allowed active duration in ticks (relative to switch-in). Exceeding this triggers OnDeadlineMissed().
            Timeout duration;    //!< Ticks spent in the active (non-sleeping) state in the current period. Incremented by UpdateTaskState(); reset to 0 on switch-out.
            volatile bool done;  //!< Set to true when the task signals work completion (via Yield() or on exit). Triggers HrtOnSwitchedOut() at the next context switch.
        };

        /*! \class WaitObject
            \brief Concrete implementation of IWaitObject, embedded in each KernelTask slot.
            \note  Allocated only when _Mode includes KERNEL_SYNC. Zero-size otherwise.
            \note  One WaitObject per KernelTask, the back-pointer m_task is wired at
                   KernelTask construction and never changes.
        */
        struct WaitObject : public IWaitObject
        {
            explicit WaitObject() : m_task(nullptr), m_sync_obj(nullptr), m_timeout(false), m_time_wait(0)
            {}

            /*! \brief Destructor.
                \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
            */
            ~WaitObject()
            {}

            /*! \class WaitRequest
                \brief Payload stored in the sync object's kernel-side list entry while a task is waiting.
                \note  KERNEL_SYNC mode only. Holds the sync object to register with the kernel's
                       m_sync_list so it receives per-tick Tick() calls for timeout tracking.
            */
            struct WaitRequest
            {
                ISyncObject *sync_obj; //!< Sync object whose Tick() will be called each kernel tick.
            };

            /*! \brief  Get the TId of the task that owns this wait object.
                \return TId of m_task.
            */
            TId GetTid() const { return m_task->GetTid(); }

            /*! \brief  Check whether the wait expired due to timeout.
                \return \c true if the wait timed out before being signalled, \c false if woken by Wake().
            */
            bool IsTimeout() const { return m_timeout; }

            /*! \brief     Wake the waiting task (called by ISyncObject when it signals).
                \param[in] timeout: \c true if woken because the timeout expired, \c false if signalled.
                \note      Clears m_time_wait, records the timeout flag, removes this object from
                           m_sync_obj's wait list, nulls m_sync_obj, then calls m_task->Wake().
            */
            void Wake(bool timeout)
            {
                STK_ASSERT(m_sync_obj != nullptr);

                m_timeout   = timeout;
                m_time_wait = 0;

                m_sync_obj->RemoveWaitObject(this);
                m_sync_obj = nullptr;

                return m_task->Wake();
            }

            /*! \brief  Advance the timeout countdown by one tick.
                \return \c true if the wait is still active (not yet timed out),
                        \c false if the timeout just expired (caller should stop ticking this object).
                \note   Called by UpdateSyncObjects() each kernel tick.
                        WAIT_INFINITE waits never time out and always return \c true.
            */
            bool Tick(Timeout elapsed_ticks)
            {
                if ((m_time_wait != WAIT_INFINITE) && !m_timeout)
                {
                    m_time_wait -= elapsed_ticks;

                    if (m_time_wait <= 0)
                        m_timeout = true;
                }

                return !m_timeout;
            }

            /*! \brief     Configure and arm this wait object for a new wait operation.
                \param[in] sync_obj: The synchronization object to wait on. Must not already
                           have this wait object registered (asserted).
                \param[in] timeout: Maximum ticks to wait, or WAIT_INFINITE for no timeout.
                \note      Registers this object with sync_obj's wait list via AddWaitObject().
                           Must be paired with a matching Wake() or timeout expiry.
            */
            void SetupWait(ISyncObject *sync_obj, Timeout timeout)
            {
                STK_ASSERT(m_sync_obj == nullptr);

                m_sync_obj  = sync_obj;
                m_time_wait = timeout;
                m_timeout   = false;

                sync_obj->AddWaitObject(this);
            }

            KernelTask   *m_task;      //!< Back-pointer to the owning KernelTask. Set once at construction; never changes.
            ISyncObject  *m_sync_obj;  //!< Sync object this wait is registered with, or \c NULL when not waiting.
            volatile bool m_timeout;   //!< \c true if the wait expired due to timeout rather than a Wake() signal.
            Timeout       m_time_wait; //!< Ticks remaining until timeout. Decremented each tick; WAIT_INFINITE means no timeout.
        };

        /*! \brief     Bind this slot to a user task: set access mode, task ID, and initialise the stack.
            \param[in] platform: Platform driver used to initialise the stack frame.
            \param[in] user_task: User task to bind. Asserts that the stack is successfully initialised.
        */
        void Bind(TPlatform *platform, ITask *user_task)
        {
            // set access mode for this stack
            m_stack.mode = user_task->GetAccessMode();

            // set task id for tracking purpose
        #if STK_NEED_TASK_ID
            m_stack.tid = user_task->GetId();
        #endif

            // init stack of the user task
            if (!platform->InitStack(STACK_USER_TASK, &m_stack, user_task, user_task))
            {
                STK_ASSERT(false);
            }

            // bind user task
            m_user = user_task;
        }

        /*! \brief Reset this slot to the free (unbound) state, clearing all scheduling metadata.
            \note  Called by RemoveTask(). After Unbind() the slot is available for the next AddTask().
        */
        void Unbind()
        {
            m_user       = nullptr;
            m_stack      = {};
            m_state      = STATE_NONE;
            m_time_sleep = 0;

            if (IsHrtMode())
                m_hrt[0].Clear();
            else
                m_srt->Clear();
        }

        /*! \brief     Schedule the removal of the task from the kernel on next tick.
        */
        void ScheduleRemoval()
        {
            // make this task sleeping to switch it out from scheduling process
            ScheduleSleep(INT32_MAX);

            // mark it as done HRT task
            if (IsHrtMode())
                HrtOnWorkCompleted();

            // mark it as pending for removal
            m_state |= STATE_REMOVE_PENDING;
        }

        /*! \brief     Check if task is pending removal.
        */
        bool IsPendingRemoval() const { return ((m_state & STATE_REMOVE_PENDING) != 0U); }

        /*! \brief     Check if Stack Pointer (SP) belongs to this task.
            \param[in] SP: Stack Pointer.
        */
        bool IsMemoryOfSP(Word SP) const
        {
            Word *start = m_user->GetStack();
            Word *end   = start + m_user->GetStackSize();

            return (SP >= hw::PtrToWord(start)) && (SP <= hw::PtrToWord(end));
        }

        /*! \brief     Initialize task with HRT info.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] periodicity_tc: Periodicity time at which task is scheduled (ticks).
            \param[in] deadline_tc: Deadline time within which a task must complete its work (ticks).
            \param[in] start_delay_tc: Initial start delay for the task (ticks).
        */
        void HrtInit(Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc)
        {
            STK_ASSERT(periodicity_tc > 0);
            STK_ASSERT(deadline_tc > 0);
            STK_ASSERT(start_delay_tc >= 0);
            STK_ASSERT(periodicity_tc < INT32_MAX);
            STK_ASSERT(deadline_tc < INT32_MAX);

            m_hrt[0].periodicity = periodicity_tc;
            m_hrt[0].deadline    = deadline_tc;

            if (start_delay_tc > 0)
                ScheduleSleep(start_delay_tc);
        }

        /*! \brief     Called when task is switched into the scheduling process.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] ticks: Current ticks of the Kernel.
        */
        void HrtOnSwitchedIn() {}

        /*! \brief     Called when task is switched out from the scheduling process.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] platform: Platform driver instance.
            \param[in] ticks: Current ticks of the Kernel.
        */
        void HrtOnSwitchedOut(IPlatform */*platform*/)
        {
            const Timeout duration = m_hrt[0].duration;

            STK_ASSERT(duration >= 0);

            Timeout sleep = m_hrt[0].periodicity - duration;
            if (sleep > 0)
                ScheduleSleep(sleep);

            m_hrt[0].duration = 0;
            m_hrt[0].done     = false;
        }

        /*! \brief     Hard-fail HRT task when it missed its deadline.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] platform: Platform driver instance.
        */
        void HrtHardFailDeadline(IPlatform *platform)
        {
            const Timeout duration = m_hrt[0].duration;

            STK_ASSERT(duration >= 0);
            STK_ASSERT(HrtIsDeadlineMissed(duration));

            m_user->OnDeadlineMissed(duration);
            platform->ProcessHardFault();
        }

        /*! \brief     Called when task process called IKernelService::SwitchToNext to inform Kernel that work is completed.
            \note      Related to stk::KERNEL_HRT mode only.
        */
        void HrtOnWorkCompleted()
        {
            m_hrt[0].done = true;
            __stk_full_memfence();
        }

        /*! \brief     Check if deadline missed.
            \note      Related to stk::KERNEL_HRT mode only.
        */
        bool HrtIsDeadlineMissed(Timeout duration) const { return (duration > m_hrt[0].deadline); }

        /*! \brief     Put the task into a sleeping state for the specified number of ticks.
            \param[in] ticks: Number of ticks to sleep. Must be > 0.
            \note      Stores \c -ticks in m_time_sleep (negative values indicate sleeping;
                       UpdateTaskState() increments toward 0 each tick until the task wakes).
            \note      If the strategy uses SLEEP_EVENT_API and the task is not already sleeping,
                       sets STATE_SLEEP_PENDING so OnTaskSleep() is delivered on the next tick.
            \note      A full memory fence is emitted after the assignment so that the ISR-side
                       scheduler sees the updated value without delay.
        */
        void ScheduleSleep(Timeout ticks)
        {
            STK_ASSERT(ticks > 0);

            // set state first as kernel checks it when task IsSleeping
            if (TStrategy::SLEEP_EVENT_API)
            {
                if (m_time_sleep >= 0)
                    m_state |= STATE_SLEEP_PENDING;
            }

            m_time_sleep = -ticks;
            __stk_full_memfence();
        }

        ITask            *m_user;       //!< Bound user task, or \c NULL when slot is free.
        Stack             m_stack;      //!< Stack descriptor (SP register value + access mode + optional tid).
        volatile uint32_t m_state;      //!< Bitmask of EStateFlags. Written by task thread, read/cleared by kernel tick.
        volatile Timeout  m_time_sleep; //!< Sleep countdown: negative while sleeping (absolute value = ticks remaining), zero when awake.
        SrtInfo           m_srt[STK_ALLOCATE_COUNT(TMode, KERNEL_HRT, 0, 1)];       //!< SRT metadata. Zero-size (no memory) in KERNEL_HRT mode.
        HrtInfo           m_hrt[STK_ALLOCATE_COUNT(TMode, KERNEL_HRT, 1, 0)];       //!< HRT metadata. Zero-size (no memory) in non-HRT mode.
        int32_t           m_rt_weight[STK_ALLOCATE_COUNT(TStrategy::WEIGHT_API, 1, 1, 0)]; //!< Run-time weight for weighted-round-robin scheduling. Zero-size for unweighted strategies.
        WaitObject        m_wait_obj[STK_ALLOCATE_COUNT(TMode, KERNEL_SYNC, 1, 0)]; //!< Embedded wait object for synchronization. Zero-size (no memory) if KERNEL_SYNC is not set.
    };

    /*! \class KernelService
        \brief Concrete implementation of IKernelService exposed to running tasks.

        Holds the global tick counter (m_ticks, updated atomically by IncrementTick() each
        SysTick) and a typed pointer to the platform driver. Tasks access this object via
        IKernelService::GetInstance() which returns the singleton registered at Initialize().
    */
    class KernelService : public IKernelService
    {
        friend class Kernel;

    public:
        /*! \brief  Get the TId of the calling task.
            \return TId via platform->GetTid(), which resolves by stack pointer.
            \warning ISR-unsafe. Asserted in the helper-layer wrapper stk::GetTid().
        */
        TId GetTid() const { return m_platform->GetTid(); }

        /*! \brief  Get the current tick count since kernel start.
            \return Atomically-read 64-bit tick counter (via hw::ReadVolatile64).
            \note   ISR-safe.
        */
        Ticks GetTicks() const { return hw::ReadVolatile64(&m_ticks); }

        /*! \brief  Get the tick resolution.
            \return Microseconds per tick, as configured by Initialize(resolution_us).
            \note   ISR-safe.
        */
        int32_t GetTickResolution() const  { return m_platform->GetTickResolution(); }

        /*! \brief     Busy-wait until \a msec milliseconds have elapsed.
            \param[in] msec: Delay in milliseconds.
            \note      Spins calling __stk_relax_cpu() in a loop. Does not yield the CPU.
            \warning   ISR-unsafe. In HRT mode, long busy-waits will cause deadline misses.
        */
        __stk_attr_noinline void Delay(Timeout msec)
        {
            Ticks deadline = GetTicks() + GetTicksFromMsec(msec, GetTickResolution());
            while (GetTicks() < deadline)
            {
                __stk_relax_cpu();
            }
        }

        /*! \brief     Yield the CPU for \a msec milliseconds, allowing the scheduler to run other tasks.
            \param[in] msec: Sleep time in milliseconds.
            \note      Converts msec to ticks and calls platform->Sleep() which schedules the calling
                       task to sleep and spins until the kernel switches it back in.
            \warning   ISR-unsafe. Asserts (never returns) in KERNEL_HRT mode — HRT tasks sleep
                       automatically according to their periodicity, not via explicit Sleep() calls.
        */
        __stk_attr_noinline void Sleep(Timeout msec)
        {
            if (!IsHrtMode())
            {
                m_platform->Sleep((Timeout)GetTicksFromMsec(msec, GetTickResolution()));
            }
            else
            {
                // sleeping is not supported in HRT mode, task will sleep according to its periodicity and workload
                STK_ASSERT(false);
            }
        }

        /*! \brief Yield the CPU to the next runnable task.
            \note  Internally calls OnTaskSleep with a 2-tick sleep so the scheduler selects
                   another task on the very next tick. The calling task is rescheduled promptly.
            \warning ISR-unsafe.
        */
        void SwitchToNext() { m_platform->SwitchToNext(); }

        /*! \brief     Block the calling task until a synchronization object signals or the timeout expires.
            \param[in] sobj: Sync object to wait on. Must belong to this kernel's m_sync_list or be unregistered.
            \param[in] mutex: Mutex currently held by the caller. Unlocked before waiting, re-locked on return.
            \param[in] ticks: Maximum ticks to wait, or WAIT_INFINITE.
            \return    Pointer to the WaitObject on wakeup (check IsTimeout() to distinguish signal vs. timeout).
            \note      KERNEL_SYNC mode only. Asserts (returns NULL) if KERNEL_SYNC is not set.
        */
        IWaitObject *Wait(ISyncObject *sobj, IMutex *mutex, Timeout ticks)
        {
            if (IsSyncMode())
            {
                return m_platform->Wait(sobj, mutex, ticks);
            }
            else
            {
                STK_ASSERT(false);
                return nullptr;
            }
        }

    private:
        /*! \brief Construct an uninitialised service instance (m_platform = null, m_ticks = 0).
            \note  Fully initialised by Initialize(). Private; constructed only as a member of Kernel.
        */
        explicit KernelService() : m_platform(nullptr), m_ticks(0)
        {}

        /*! \brief Destructor.
            \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
        */
        ~KernelService()
        {}

        /*! \brief     Initialize instance.
            \note      When call completes Singleton<IKernelService *> will start referencing this
                       instance (see g_KernelService).
            \param[in] platform: IPlatform instance.
        */
        void Initialize(IPlatform *platform)
        {
            m_platform = static_cast<TPlatform *>(platform);
        }


        /*! \brief     Increment tick by value.
        */
        void IncrementTicks(Ticks advance)
        {
            // using WriteVolatile64() to guarantee correct lockless reading order by ReadVolatile64
            hw::WriteVolatile64(&m_ticks, m_ticks + advance);
        }

        TPlatform     *m_platform; //!< Typed platform driver pointer, set at Initialize().
        volatile Ticks m_ticks;    //!< Global tick counter. Written via hw::WriteVolatile64() by IncrementTick() (ISR context); read via hw::ReadVolatile64() by GetTicks() (task context) for a lock-free consistent 64-bit read on 32-bit CPUs.
    };

public:
    /*! \brief Constants.
    */
    enum EConsts
    {
        TASKS_MAX = TSize //!< Maximum number of concurrently registered tasks. Fixed at compile time. Exceeding this limit in AddTask() triggers a compile-time assert (TASKS_MAX > 0) and a runtime STK_ASSERT.
    };

    /*! \brief Construct the kernel with all storage zero-initialised and the request flag set to ~0
               (indicating uninitialized state; cleared to REQUEST_NONE by Initialize()).
        \note  In debug builds also verifies that TPlatform derives from IPlatform and TStrategy
               from ITaskSwitchStrategy.
    */
    explicit Kernel() : m_platform(), m_strategy(), m_task_now(nullptr), m_task_storage(), m_sleep_trap(),
        m_exit_trap(), m_fsm_state(FSM_STATE_NONE), m_request(REQUEST_NONE), m_state(STATE_INACTIVE)
    {
    #ifdef _DEBUG
        // TPlatform must inherit IPlatform
        IPlatform *platform = &m_platform;
        (void)platform;

        // TStrategy must inherit ITaskSwitchStrategy
        ITaskSwitchStrategy *strategy = &m_strategy;
        (void)strategy;
    #endif

    #if !STK_TICKLESS_IDLE
        STK_STATIC_ASSERT_DESC(((TMode & KERNEL_TICKLESS) == 0U),
            "STK_TICKLESS_IDLE must be defined to 1 for KERNEL_TICKLESS");
    #endif
    }

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~Kernel()
    {}

    /*! \brief     Prepare kernel for use: reset state, configure the platform, and register the service singleton.
        \param[in] resolution_us: System tick resolution in microseconds per tick (default: PERIODICITY_DEFAULT).
                   Valid range: 1 .. PERIODICITY_MAX. Asserts if 0 or out of range.
        \note      Must be called before AddTask() or Start(). Calling twice without an intervening
                   Stop() asserts (IsInitialized() is false until this call completes).
    */
    __stk_attr_noinline void Initialize(uint32_t resolution_us = PERIODICITY_DEFAULT)
    {
        STK_ASSERT(resolution_us != 0);
        STK_ASSERT(resolution_us <= PERIODICITY_MAX);
        STK_ASSERT(!IsInitialized());

        // reinitialize key state variables
        m_task_now  = nullptr;
        m_fsm_state = FSM_STATE_NONE;
        m_request   = REQUEST_NONE;

        m_service.Initialize(&m_platform);

        m_platform.Initialize(this, &m_service, resolution_us, (IsDynamicMode() ? &m_exit_trap[0].stack : nullptr));

        // now ready to Start()
        m_state = STATE_READY;
    }

    /*! \brief     Register task for a soft real-time (SRT) scheduling.
        \param[in] user_task: User task to add. Must not already be registered. Must not be \c nullptr.
        \note      Before Start(): allocates a free KernelTask slot and adds it to the strategy immediately.
        \note      After Start() (KERNEL_DYNAMIC only): serialises the request via RequestAddTask() —
                   the calling task yields and the kernel processes the request on the next tick.
        \warning   Asserts if called in KERNEL_HRT mode (use the HRT overload instead),
                   if called after Start() without KERNEL_DYNAMIC, or if TASKS_MAX is exceeded.
    */
    __stk_attr_noinline void AddTask(ITask *user_task)
    {
        if (!IsHrtMode())
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(IsInitialized());

            // when started the operation must be serialized by switching out from processing until
            // kernel processes this request
            if (IsStarted())
            {
                if (IsDynamicMode())
                {
                    RequestAddTask(user_task);
                }
                else
                {
                    STK_ASSERT(false);
                }
            }
            else
            {
                AllocateAndAddNewTask(user_task);
            }
        }
        else
        {
            STK_ASSERT(false);
        }
    }

    /*! \brief     Register a task for hard real-time (HRT) scheduling.
        \param[in] user_task:  User task to add. Must not already be registered. Must not be \c nullptr.
        \param[in] periodicity_tc: Activation period in ticks. Must be > 0 and < INT32_MAX.
        \param[in] deadline_tc: Maximum allowed active duration in ticks. Must be > 0 and < INT32_MAX.
        \param[in] start_delay_tc: Initial sleep delay in ticks before the first activation. 0 means activate immediately.
        \note      Must be called before Start(). Dynamic (post-Start) HRT task addition is not supported.
        \warning   Asserts if called outside KERNEL_HRT mode (use the SRT overload instead) or after Start().
    */
    __stk_attr_noinline void AddTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc)
    {
        if (IsHrtMode())
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(IsInitialized());
            STK_ASSERT(!IsStarted());

            HrtAllocateAndAddNewTask(user_task, periodicity_tc, deadline_tc, start_delay_tc);
        }
        else
        {
            STK_ASSERT(false);
        }
    }

    /*! \brief     Remove a previously added task from the kernel before Start().
        \param[in] user_task: User task to remove. Must not be \c nullptr.
        \note      Only valid before Start() (i.e. while the kernel is not running).
                   To remove tasks after Start() the task should return from its Run function
                   (in KERNEL_DYNAMIC mode the slot is freed automatically on the next tick).
        \warning   KERNEL_DYNAMIC mode only. Asserts if called in KERNEL_STATIC or KERNEL_HRT mode,
                   or if called after Start().
    */
    __stk_attr_noinline void RemoveTask(ITask *user_task)
    {
        if (IsDynamicMode())
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(!IsStarted());

            KernelTask *task = FindTaskByUserTask(user_task);
            if (task != nullptr)
                RemoveTask(task);
        }
        else
        {
            // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to be removed
            STK_ASSERT(false);
        }
    }

    /*! \brief     Start the scheduler. This call does not return until all tasks have exited
                   (KERNEL_DYNAMIC mode) or indefinitely (KERNEL_STATIC mode).
        \note      Re-initialises trap stacks on every call so Start() can be called again
                   after a previous scheduling session ended.
        \note      If STK_SEGGER_SYSVIEW is enabled, starts tracing and registers all pre-added tasks.
        \warning   At least one task must have been added via AddTask() before calling Start().
                   Asserts if called before Initialize().
    */
    __stk_attr_noinline void Start()
    {
        STK_ASSERT(IsInitialized());

         // stacks of the traps must be re-initilized on every subsequent Start
        InitTraps();

        // start tracing
    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_Start();
        for (int32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->IsBusy())
                SendTaskTraceInfo(task);
        }
    #endif

        m_platform.Start();
    }

    /*! \brief  Check whether scheduler is currently running.
        \return \c true if Start() has been called and the first task switch has occurred
                (m_task_now != nullptr), \c false before Start() or after all tasks exit.
    */
    bool IsStarted() const
    {
        return (m_task_now != nullptr);
    }

    /*! \brief  Get platform driver instance owned by this kernel.
        \return Pointer to the internal TPlatform cast to IPlatform*.
    */
    IPlatform *GetPlatform() { return &m_platform; }

    /*! \brief  Get task-switching strategy instance owned by this kernel.
        \return Pointer to the internal TStrategy cast to ITaskSwitchStrategy*.
    */
    ITaskSwitchStrategy *GetSwitchStrategy() { return &m_strategy; }

    /*! \brief  Get kernel state.
    */
    EState GetState() const { return m_state; }

protected:
    /*! \enum  EFsmState
        \brief Finite-state machine (FSM) state. Encodes what the kernel is currently doing
               between two consecutive tick events.
    */
    enum EFsmState : int8_t
    {
        FSM_STATE_NONE      = -1, //!< Sentinel / uninitialized value. Set by the constructor, replaced by FSM_STATE_SWITCHING on the first tick.
        FSM_STATE_SWITCHING,      //!< Normal operation: switching between runnable tasks each tick.
        FSM_STATE_SLEEPING,       //!< All tasks are sleeping, the sleep trap is executing (CPU in low-power state).
        FSM_STATE_WAKING,         //!< At least one task woke up, transitioning from sleep trap back to a user task.
        FSM_STATE_EXITING,        //!< All tasks exited (KERNEL_DYNAMIC only), executing the exit trap to return from Start().
        FSM_STATE_MAX             //!< Sentinel: number of valid states (used to size the FSM table), denotes uninitialized state
    };

    /*! \enum  EFsmEvent
        \brief Finite-state machine (FSM) event. Computed by FetchNextEvent() each tick based
               on strategy output and current kernel state.
    */
    enum EFsmEvent : int8_t
    {
        FSM_EVENT_SWITCH = 0, //!< Strategy returned a runnable task, perform a context switch.
        FSM_EVENT_SLEEP,      //!< No runnable tasks, enter sleep trap.
        FSM_EVENT_WAKE,       //!< A task became runnable while the kernel was sleeping, wake from sleep trap.
        FSM_EVENT_EXIT,       //!< No tasks remain (KERNEL_DYNAMIC), exit scheduling and return from Start().
        FSM_EVENT_MAX         //!< Sentinel: number of valid events (used to size the FSM table).
    };

    /*! \brief     Check if FSM state is valid.
    */
    static __stk_forceinline bool IsValidFsmState(EFsmState state)
    {
        return (state > FSM_STATE_NONE) &&
               (state < FSM_STATE_MAX);
    }

    /*! \brief     Initialize stack of the traps.
    */
    __stk_attr_noinline void InitTraps()
    {
        // init stack for a Sleep trap
        {
            SleepTrapStack &sleep = m_sleep_trap[0];

            SleepTrapStackMemory wrapper(&sleep.memory);
            sleep.stack.mode = ACCESS_PRIVILEGED;
        #if STK_NEED_TASK_ID
            sleep.stack.tid  = SYS_TASK_ID_SLEEP;
        #endif

            m_platform.InitStack(STACK_SLEEP_TRAP, &sleep.stack, &wrapper, nullptr);
        }

        // init stack for an Exit trap
        if (IsDynamicMode())
        {
            ExitTrapStack &exit = m_exit_trap[0];

            ExitTrapStackMemory wrapper(&exit.memory);
            exit.stack.mode = ACCESS_PRIVILEGED;
        #if STK_NEED_TASK_ID
            exit.stack.tid  = SYS_TASK_ID_EXIT;
        #endif

            m_platform.InitStack(STACK_EXIT_TRAP, &exit.stack, &wrapper, nullptr);
        }
    }

    /*! \brief     Allocate new instance of KernelTask.
        \param[in] user_task: User task for which kernel task object is allocated.
        \return    Kernel task.
    */
    KernelTask *AllocateNewTask(ITask *user_task)
    {
        // look for a free kernel task
        KernelTask *new_task = nullptr;
        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->IsBusy())
            {
                // avoid task collision
                STK_ASSERT(task->m_user != user_task);

                // avoid stack collision
                STK_ASSERT(task->m_user->GetStack() != user_task->GetStack());
            }
            else
            if (new_task == nullptr)
            {
                new_task = task;
            #if defined(NDEBUG) && !defined(_STK_ASSERT_REDIRECT)
                break; // break if assertions are inactive and do not try to validate collision with existing tasks
            #endif
            }
        }

        // if nullptr - exceeded max supported kernel task count, application design failure
        STK_ASSERT(new_task != nullptr);

        new_task->Bind(&m_platform, user_task);

        return new_task;
    }

    /*! \brief     Add kernel task to the scheduling strategy.
        \param[in] task: Pointer to the kernel task.
    */
    void AddKernelTask(KernelTask *task)
    {
    #if STK_SEGGER_SYSVIEW
        // start tracing new task
        SEGGER_SYSVIEW_OnTaskCreate(task->GetUserStack()->tid);
        if (IsStarted())
            SendTaskTraceInfo(task);
    #endif

        m_strategy.AddTask(task);
    }

    /*! \brief     Allocate new instance of KernelTask and add it into the scheduling process.
        \param[in] user_task: User task for which kernel task object is allocated.
        \return    Kernel task.
    */
    void AllocateAndAddNewTask(ITask *user_task)
    {
        KernelTask *task = AllocateNewTask(user_task);
        STK_ASSERT(task != nullptr);

        AddKernelTask(task);
    }

    /*! \brief     Allocate new instance of KernelTask and add it into the HRT scheduling process.
        \note      Related to stk::KERNEL_HRT mode only.
        \param[in] user_task: User task for which kernel task object is allocated.
        \param[in] periodicity_tc: Periodicity time at which task is scheduled (ticks).
        \param[in] deadline_tc: Deadline time within which a task must complete its work (ticks).
        \param[in] start_delay_tc: Initial start delay for the task (ticks).
        \return    Kernel task.
    */
    void HrtAllocateAndAddNewTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc)
    {
        KernelTask *task = AllocateNewTask(user_task);
        STK_ASSERT(task != nullptr);

        task->HrtInit(periodicity_tc, deadline_tc, start_delay_tc);

        AddKernelTask(task);
    }

    /*! \brief     Request to add new task.
        \note      Must be called by the task process only!
        \param[in] user_task: User task to add.
    */
    __stk_attr_noinline void RequestAddTask(ITask *user_task)
    {
        STK_ASSERT(IsDynamicMode());

        KernelTask *caller = FindTaskBySP(m_platform.GetCallerSP());
        STK_ASSERT(caller != nullptr);

        typename KernelTask::AddTaskRequest req = { .user_task = user_task };
        caller->m_srt[0].add_task_req = &req;

        // notify kernel
        ScheduleAddTask();

        // switch out and wait for completion (due to context switch request could be processed here)
        if (caller->m_srt[0].add_task_req != nullptr)
            m_service.SwitchToNext();

        STK_ASSERT(caller->m_srt[0].add_task_req == nullptr);
    }

    /*! \brief     Find kernel task by the bound ITask instance.
        \param[in] user_task: User task.
        \return    Kernel task.
    */
    __stk_attr_noinline KernelTask *FindTaskByUserTask(const ITask *user_task)
    {
        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->GetUserTask() == user_task)
                return task;
        }

        return nullptr;
    }

    /*! \brief     Find kernel task by the bound Stack instance.
        \param[in] stack: Stack.
        \return    Kernel task.
    */
    KernelTask *FindTaskByStack(const Stack *stack)
    {
        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->GetUserStack() == stack)
                return task;
        }

        return nullptr;
    }

    /*! \brief     Find kernel task for a Stack Pointer (SP).
        \param[in] SP: Stack pointer.
        \return    Kernel task.
    */
    __stk_attr_noinline KernelTask *FindTaskBySP(Word SP)
    {
        STK_ASSERT(m_task_now != nullptr);

        if (m_task_now->IsMemoryOfSP(SP))
            return m_task_now;

        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];

            // skip finished tasks (applicable only for KERNEL_DYNAMIC mode)
            if (IsDynamicMode() && !task->IsBusy())
                continue;

            if (task->IsMemoryOfSP(SP))
                return task;
        }

        return nullptr;
    }

    /*! \brief     Remove kernel task.
        \note      Removal of the kernel task means releasing it from the user task details.
        \param[in] task: Kernel task.
    */
    void RemoveTask(KernelTask *task)
    {
        STK_ASSERT(task != nullptr);

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskTerminate(task->GetUserStack()->tid);
    #endif

        m_strategy.RemoveTask(task);
        task->Unbind();
    }

    __stk_attr_noinline void OnStart(Stack *&active)
    {
        STK_ASSERT(m_strategy.GetSize() != 0);

        // iterate tasks and generate OnTaskSleep for a strategy for all initially sleeping tasks
        if (TStrategy::SLEEP_EVENT_API)
        {
            for (uint32_t i = 0; i < TASKS_MAX; ++i)
            {
                KernelTask *task = &m_task_storage[i];

                if (task->IsSleeping())
                {
                    if ((task->m_state & KernelTask::STATE_SLEEP_PENDING) != 0U)
                    {
                        task->m_state &= ~KernelTask::STATE_SLEEP_PENDING;

                        // notify strategy that task is sleeping
                        m_strategy.OnTaskSleep(task);
                    }
                }
            }
        }

        // get initial state and first task
        {
            m_fsm_state = FSM_STATE_SWITCHING;

            KernelTask *next = nullptr;
            m_fsm_state = GetNewFsmState(next);

            // expecting only SLEEPING or SWITCHING states
            STK_ASSERT((m_fsm_state == FSM_STATE_SLEEPING) || (m_fsm_state == FSM_STATE_SWITCHING));

            if (m_fsm_state == FSM_STATE_SWITCHING)
            {
                m_task_now = next;

                active = next->GetUserStack();

                if (IsHrtMode())
                    next->HrtOnSwitchedIn();
            }
            else
            if (m_fsm_state == FSM_STATE_SLEEPING)
            {
                // MISRA 5-2-3 deviation: GetNext/GetFirst returns IKernelTask*, all objects in
                // the strategy pool are KernelTask instances - downcast is guaranteed safe.
                m_task_now = static_cast<KernelTask *>(m_strategy.GetFirst());

                active = &m_sleep_trap[0].stack;
            }
        }

        // is in running state
        m_state = STATE_RUNNING;

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStartExec(m_task_now->tid);
    #endif
    }

    __stk_attr_noinline void OnStop()
    {
        if (IsDynamicMode())
        {
            m_fsm_state = FSM_STATE_NONE;

            // is in stopped state, i.e. is ready to Start() again
            m_state = STATE_READY;
        }
    }

#if STK_TICKLESS_IDLE
    bool OnTick(Stack *&idle, Stack *&active, Timeout &ticks)
#else
    bool OnTick(Stack *&idle, Stack *&active)
#endif
    {
    #if !STK_TICKLESS_IDLE
        enum { ticks = 1 };
    #endif

        // advance internal timestamp
        m_service.IncrementTicks(ticks);

        // consume elapsed and update to ticks to sleep
    #if STK_TICKLESS_IDLE
        ticks =
    #endif
        UpdateTasks(ticks);

        // decide on a context switch
        return UpdateFsmState(idle, active);
    }

    void OnTaskSwitch(Word caller_SP)
    {
        // yield with 2 ticks: 1 will be incremented on the next OnTick call by UpdateTasks
        // and remaining 1 will cause a context switch by UpdateFsmState when strategy detects
        // it as a sleeping test
        OnTaskSleep(caller_SP, 2);
    }

    void OnTaskSleep(Word caller_SP, Timeout ticks)
    {
        KernelTask *task = FindTaskBySP(caller_SP);
        STK_ASSERT(task != nullptr);

        // make change to HRT state and sleep time atomic
        {
            hw::CriticalSection::ScopedLock cs_;

            if (IsHrtMode())
                task->HrtOnWorkCompleted();

            task->ScheduleSleep(ticks);
        }

        // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
        while (task->IsSleeping())
        {
            __stk_relax_cpu();
        }
    }

    void OnTaskExit(Stack *stack)
    {
        if (IsDynamicMode())
        {
            KernelTask *task = FindTaskByStack(stack);
            STK_ASSERT(task != nullptr);

            task->ScheduleRemoval();
        }
        else
        {
            // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to exit
            STK_KERNEL_PANIC(KERNEL_PANIC_BAD_MODE);
        }
    }

    IWaitObject *OnTaskWait(Word caller_SP, ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
    {
        if (IsSyncMode())
        {
            STK_ASSERT(timeout != 0);        // API contract: caller must not be in ISR
            STK_ASSERT(sync_obj != nullptr); // API contract: ISyncObject instance must be provided
            STK_ASSERT(mutex != nullptr);    // API contract: IMutex instance must be provided
            STK_ASSERT((sync_obj->GetHead() == nullptr) || (sync_obj->GetHead() == &m_sync_list[0]));

            KernelTask *task = FindTaskBySP(caller_SP);
            STK_ASSERT(task != nullptr);

            // configure waiting
            task->m_wait_obj->SetupWait(sync_obj, timeout);

            // register ISyncObject if not yet
            if (sync_obj->GetHead() == nullptr)
                m_sync_list->LinkBack(sync_obj);

            // start sleeping infinitely, we rely on a Wake call via WaitObject
            task->ScheduleSleep(WAIT_INFINITE);

            // unlock mutex locked externally, so that we could wait in a busy-waiting loop
            mutex->Unlock();

            // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
            while (task->IsSleeping())
            {
                __stk_relax_cpu();
            }

            // re-lock mutex when returning to the task's execution space
            mutex->Lock();

            return task->m_wait_obj;
        }
        else
        {
            STK_ASSERT(false);
            return nullptr;
        }
    }

    TId OnGetTid(Word caller_SP)
    {
        KernelTask *task = FindTaskBySP(caller_SP);
        STK_ASSERT(task != nullptr);

        return task->GetTid();
    }

    /*! \brief     Update tasks (sleep, requests).
    */
#if STK_TICKLESS_IDLE
    Timeout
#else
    void
#endif
    UpdateTasks(Timeout elapsed_ticks)
    {
        // sync objects are updated before UpdateTaskRequest which may add a new object (newly added object must become 1 tick older)
        if (IsSyncMode())
            UpdateSyncObjects(elapsed_ticks);

        UpdateTaskRequest();

    #if STK_TICKLESS_IDLE
        return
    #endif
            UpdateTaskState(elapsed_ticks);
    }

    /*! \brief     Update task state (removal, sleep, duration of HRT task).
    */
#if STK_TICKLESS_IDLE
    Timeout
#else
    void
#endif
    UpdateTaskState(Timeout elapsed_ticks)
    {
        Timeout sleep_ticks = STK_TICKLESS_TICKS_MAX;

        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];

            if (task->IsSleeping())
            {
                if (IsDynamicMode())
                {
                    // task is pending removal, wait until it is switched out
                    if (task->IsPendingRemoval())
                    {
                        if ((task != m_task_now) ||
                            ((m_strategy.GetSize() == 1) && (m_fsm_state == FSM_STATE_SLEEPING)))
                        {
                            RemoveTask(task);
                            continue;
                        }
                    }
                }

                // deliver sleep event to strategy
                // note: only currently scheduled task can be pending to sleep
                if (TStrategy::SLEEP_EVENT_API)
                {
                    if ((task->m_state & KernelTask::STATE_SLEEP_PENDING) != 0U)
                    {
                        task->m_state &= ~KernelTask::STATE_SLEEP_PENDING;

                        // notify strategy that task is sleeping
                        m_strategy.OnTaskSleep(task);
                    }
                }

                // advance sleep time by a tick
                task->m_time_sleep += elapsed_ticks;

                // deliver sleep event to strategy
                if (TStrategy::SLEEP_EVENT_API)
                {
                    // notify strategy that task woke up
                    if (task->m_time_sleep >= 0)
                        m_strategy.OnTaskWake(task);
                }
            }
            else
            if (IsHrtMode())
            {
                // in HRT mode we trace how long task spent in active state (doing some work)
                if (task->IsBusy())
                {
                    task->m_hrt[0].duration += elapsed_ticks;

                    // check if deadline is missed (HRT failure)
                    if (task->HrtIsDeadlineMissed(task->m_hrt[0].duration))
                    {
                        bool can_recover = false;

                        // report deadline overrun to a strategy which supports overrun recovery
                        if (TStrategy::DEADLINE_MISSED_API)
                            can_recover = m_strategy.OnTaskDeadlineMissed(task);

                        // report failure if it could not be recovered by a scheduling strategy
                        if (!can_recover)
                            task->HrtHardFailDeadline(&m_platform);
                    }
                }
            }

            // get the number ticks the driver has to keep CPU in Idle
            if (IsTicklessMode() && task->IsBusy())
            {
                // note: task sleep time is negative
                Timeout task_sleep = -task->m_time_sleep;

                if (IsSyncMode())
                {
                    // likely task is sleeping during sync operation (see Wait)
                    if (task_sleep == WAIT_INFINITE)
                    {
                        // note: sync wait time is positive
                        task_sleep = task->m_wait_obj->m_time_wait;

                        // we shall account for only valid time (when task is waiting during sync operation)
                        if (task_sleep > 0)
                            sleep_ticks = stk::Min(sleep_ticks, task_sleep);
                    }
                    else
                    {
                        sleep_ticks = stk::Min(sleep_ticks, task_sleep);
                    }
                }
                else
                {
                    sleep_ticks = stk::Min(sleep_ticks, task_sleep);
                }

                // clamp to [1, STK_TICKLESS_TICKS_MAX] range
                sleep_ticks = stk::Max<Timeout>(sleep_ticks, 1);
            }
        }

    #if STK_TICKLESS_IDLE
        return sleep_ticks;
    #endif
    }

    /*! \brief     Update synchronization objects.
    */
    void UpdateSyncObjects(Timeout elapsed_ticks)
    {
        STK_ASSERT(IsSyncMode());

        ISyncObject::ListEntryType *itr = m_sync_list->GetFirst();

        while (itr != nullptr)
        {
            ISyncObject::ListEntryType *next = itr->GetNext();

            // MISRA 5-2-3 deviation: GetNext/GetFirst returns ISyncObject*, all objects in
            // m_sync_list are ISyncObject instances - downcast is guaranteed safe.
            if (!static_cast<ISyncObject *>(itr)->Tick(elapsed_ticks))
                m_sync_list->Unlink(itr);

            itr = next;
        }
    }

    /*! \brief     Update pending task requests.
    */
    void UpdateTaskRequest()
    {
        if (m_request == REQUEST_NONE)
            return;

        // process AddTask requests coming from tasks (KERNEL_DYNAMIC mode only, KERNEL_HRT is
        // excluded as we assume that HRT tasks must be known to the kernel before a Start())
        if (IsDynamicMode() && !IsHrtMode())
        {
            // process serialized AddTask request made from another active task, requesting process
            // is currently waiting due to SwitchToNext()
            if ((m_request & REQUEST_ADD_TASK) != 0U)
            {
                m_request &= ~REQUEST_ADD_TASK;

                for (uint32_t i = 0; i < TASKS_MAX; ++i)
                {
                    KernelTask *task = &m_task_storage[i];

                    if (task->m_srt[0].add_task_req != nullptr)
                    {
                        AllocateAndAddNewTask(task->m_srt[0].add_task_req->user_task);

                        task->m_srt[0].add_task_req = nullptr;
                        __stk_full_memfence();
                    }
                }
            }
        }
    }

    /*! \brief      Fetch next event for the FSM.
        \param[out] next: Next kernel task to which Kernel can switch.
        \return     FSM event.
    */
    EFsmEvent FetchNextEvent(KernelTask *&next)
    {
        EFsmEvent type = FSM_EVENT_EXIT;
        KernelTask *itr = nullptr;

        // check if no tasks left in KERNEL_DYNAMIC mode and exit, if KERNEL_DYNAMIC is not
        // set then 'is_empty' will always be false
        bool is_empty = IsDynamicMode() && (m_strategy.GetSize() == 0U);

        if (!is_empty)
        {
            // MISRA 5-2-3 deviation: GetNext/GetFirst returns IKernelTask*, all objects in
            // the strategy pool are KernelTask instances - downcast is guaranteed safe.
            itr = static_cast<KernelTask *>(m_strategy.GetNext());

            // sleep-aware strategy returns nullptr if no active tasks available, start sleeping
            if (itr == nullptr)
            {
                type = FSM_EVENT_SLEEP;
            }
            else
            {
                // strategy must provide active-only task
                STK_ASSERT(!itr->IsSleeping());

                // if was sleeping, process wake event first
                type = (m_fsm_state == FSM_STATE_SLEEPING ? FSM_EVENT_WAKE : FSM_EVENT_SWITCH);
            }
        }

        next = itr;
        return type;
    }

    /*! \brief      Get new FSM state.
        \param[out] next: Next kernel task to which Kernel can switch.
        \return     FSM state.
    */
#ifdef _STK_UNDER_TEST
    virtual
#endif
    EFsmState GetNewFsmState(KernelTask *&next)
    {
        STK_ASSERT(IsValidFsmState(m_fsm_state));
        return m_fsm[m_fsm_state][FetchNextEvent(next)];
    }

    /*! \brief      Update FSM state.
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
        \return     FSM state.
    */
    bool UpdateFsmState(Stack *&idle, Stack *&active)
    {
        KernelTask *now = m_task_now, *next = nullptr;
        bool switch_context = false;

        EFsmState new_state = GetNewFsmState(next);

        switch (new_state)
        {
        case FSM_STATE_SWITCHING:
            switch_context = StateSwitch(now, next, idle, active);
            break;
        case FSM_STATE_SLEEPING:
            switch_context = StateSleep(now, next, idle, active);
            break;
        case FSM_STATE_WAKING:
            switch_context = StateWake(now, next, idle, active);
            break;
        case FSM_STATE_EXITING:
            switch_context = StateExit(now, next, idle, active);
            break;
        case FSM_STATE_NONE:
            return switch_context; // valid intermittent non-persisting state: no-transition
        case FSM_STATE_MAX:
        default:                   // invalid state value
            STK_KERNEL_PANIC(KERNEL_PANIC_BAD_STATE);
            break;
        }

        m_fsm_state = new_state;
        return switch_context;
    }

    /*! \brief      Switches contexts.
        \note       FSM state: stk::FSM_STATE_SWITCHING.
        \param[in]  now: Currently active kernel task.
        \param[in]  next: Next kernel task.
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateSwitch(KernelTask *now, KernelTask *next, Stack *&idle, Stack *&active)
    {
        STK_ASSERT(now != nullptr);
        STK_ASSERT(next != nullptr);

        // do not switch context because task did not change
        if (next == now)
            return false;

        idle   = now->GetUserStack();
        active = next->GetUserStack();

        // if stack memory is exceeded these assertions will be hit
        if (now->IsBusy())
        {
            // current task could exit, thus we check it with IsBusy to avoid referencing nullptr returned by GetUserTask()
            STK_ASSERT(now->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);
        }
        STK_ASSERT(next->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);

        m_task_now = next;

        if ((IsHrtMode()))
        {
            if (now->m_hrt[0].done)
            {
                now->HrtOnSwitchedOut(&m_platform);
                next->HrtOnSwitchedIn();
            }
        }

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStopReady(now->GetUserStack()->tid, TRACE_EVENT_SWITCH);
        SEGGER_SYSVIEW_OnTaskStartReady(next->GetUserStack()->tid);
    #endif

        return true; // switch context
    }

    /*! \brief      Wakes up after sleeping.
        \note       FSM state: stk::FSM_STATE_AWAKENING.
        \param[in]  now: Currently active kernel task (ignored).
        \param[in]  next: Next kernel task.
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateWake(KernelTask *now, KernelTask *next, Stack *&idle, Stack *&active)
    {
        (void)now;

        STK_ASSERT(next != nullptr);

        idle   = &m_sleep_trap[0].stack;
        active = next->GetUserStack();

        // if stack memory is exceeded these assertions will be hit
        STK_ASSERT(m_sleep_trap[0].memory[0] == STK_STACK_MEMORY_FILLER);
        STK_ASSERT(next->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);

        m_task_now = next;

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStartReady(next->GetUserStack()->tid);
    #endif

        if ((IsHrtMode()))
            next->HrtOnSwitchedIn();

        return true; // switch context
    }

    /*! \brief      Enters into a sleeping mode.
        \note       FSM state: stk::FSM_STATE_SLEEPING.
        \param[in]  now: Currently active kernel task.
        \param[in]  next: Next kernel task (ignored).
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateSleep(KernelTask *now, KernelTask *next, Stack *&idle, Stack *&active)
    {
        (void)next;

        STK_ASSERT(now != nullptr);
        STK_ASSERT(m_sleep_trap[0].stack.SP != 0);

        idle   = now->GetUserStack();
        active = &m_sleep_trap[0].stack;

        m_task_now = static_cast<KernelTask *>(m_strategy.GetFirst());

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStopReady(now->GetUserStack()->tid, TRACE_EVENT_SLEEP);
    #endif

        if (IsHrtMode())
        {
            if (!now->IsPendingRemoval())
                now->HrtOnSwitchedOut(&m_platform);
        }

        return true; // switch context
    }

    /*! \brief      Exits from scheduling.
        \note       FSM state: stk::FSM_STATE_EXITING.
        \note       Exits only if stk::KERNEL_DYNAMIC mode is specified, otherwise ignored.
        \param[in]  now: Currently active kernel task (ignored).
        \param[in]  next: Next kernel task (ignored).
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateExit(KernelTask *now, KernelTask *next, Stack *&idle, Stack *&active)
    {
        (void)now;
        (void)next;

        if (IsDynamicMode())
        {
            // dynamic tasks are not supported if main processes's stack memory is not provided in Start()
            STK_ASSERT(m_exit_trap[0].stack.SP != 0);

            idle   = nullptr;
            active = &m_exit_trap[0].stack;

            m_task_now = nullptr;

            m_platform.Stop();
        }
        else
        {
            (void)idle;
            (void)active;
        }

        return false;
    }

    /*! \brief     Check whether Initialize() has been called and completed successfully.
        \return    \c true if Initialize() was called, \c false otherwise.
    */
    bool IsInitialized() const { return (m_state != STATE_INACTIVE); }

    /*! \brief     Signal the kernel to process a pending AddTask request on the next tick.
        \note      Sets the REQUEST_ADD_TASK bit in m_request and emits a full memory fence
                   so the ISR-side tick handler observes the flag without delay.
    */
    void ScheduleAddTask()
    {
        hw::CriticalSection::ScopedLock cs_;
        m_request |= REQUEST_ADD_TASK;
    }

#if STK_SEGGER_SYSVIEW
    /*! \brief      Emit SEGGER SystemView task registration info for a kernel task.
        \param[in]  task: Kernel task to register. Must be bound (IsBusy() == true).
        \note       Only compiled when STK_SEGGER_SYSVIEW is enabled.
    */
    void SendTaskTraceInfo(KernelTask *task)
    {
        STK_ASSERT(task->IsBusy());

        SEGGER_SYSVIEW_TASKINFO info =
        {
            .TaskID    = task->GetUserStack()->tid,
            .sName     = task->GetUserTask()->GetTraceName(),
            .Prio      = 0,
            .StackBase = hw::PtrToWord(task->GetUserTask()->GetStack()),
            .StackSize = task->GetUserTask()->GetStackSizeBytes()
        };
        SEGGER_SYSVIEW_SendTaskInfo(&info);
    }
#endif

    // Kernel modes:
    static __stk_forceinline bool IsStaticMode() { return ((TMode & KERNEL_STATIC) != 0U); }
    static __stk_forceinline bool IsDynamicMode() { return ((TMode & KERNEL_DYNAMIC) != 0U); }
    static __stk_forceinline bool IsHrtMode() { return ((TMode & KERNEL_HRT) != 0U); }
    static __stk_forceinline bool IsSyncMode() { return ((TMode & KERNEL_SYNC) != 0U); }
    static __stk_forceinline bool IsTicklessMode() { return ((TMode & KERNEL_TICKLESS) != 0U); }

    // If hit here: Kernel<N> expects at least 1 task, e.g. N > 0
    STK_STATIC_ASSERT_N(TASKS_MAX, TASKS_MAX > 0);

    // If hit here: Kernel mode must be assigned.
    STK_STATIC_ASSERT_N(KERNEL_MODE_MUST_BE_SET, (TMode != 0U));

    // If hit here: KERNEL_STATIC and KERNEL_DYNAMIC can not be mixed, either one of these is possible.
    STK_STATIC_ASSERT_N(KERNEL_MODE_MIX_NOT_ALLOWED,
        (((TMode & KERNEL_STATIC) & (TMode & KERNEL_DYNAMIC)) == 0U));

    // If hit here: KERNEL_HRT must accompany KERNEL_STATIC or KERNEL_DYNAMIC.
    STK_STATIC_ASSERT_N(KERNEL_MODE_HRT_ALONE, (((TMode & KERNEL_HRT) == 0U) ||
        ((((TMode & KERNEL_HRT) != 0U)) && (((TMode & KERNEL_STATIC) != 0U) || ((TMode & KERNEL_DYNAMIC) != 0U)))));

    // if hit here: KERNEL_TICKLESS is incompatible with KERNEL_HRT. Tickless suppresses the timer,
    // which destroys the precise periodicity HRT depends on.
    STK_STATIC_ASSERT_N(TICKLESS_HRT_CONFLICT,
        (((TMode & KERNEL_TICKLESS) == 0U) || ((TMode & KERNEL_HRT) == 0U)));

    /*! \typedef TaskStorageType
        \brief   KernelTask array type used as a storage for the KernelTask instances.
    */
    typedef KernelTask TaskStorageType[TASKS_MAX];

    /*! \class SleepTrapStack
        \brief Storage bundle for the sleep trap: a Stack descriptor paired with its backing memory.

        \note  The sleep trap executes when all user tasks are simultaneously sleeping, putting the
               CPU into a low-power WFI state until the next tick wakes a task.
        \note  Exactly one sleep trap is always allocated regardless of kernel mode.
    */
    struct SleepTrapStack
    {
        typedef SleepTrapStackMemory::MemoryType Memory;

        Stack  stack;  //!< Stack descriptor (SP register value + access mode). Initialised by InitTraps() on every Start().
        Memory memory; //!< Backing stack memory array. Size: STK_SLEEP_TRAP_STACK_SIZE elements of Word.
    };

    /*! \class ExitTrapStack
        \brief Storage bundle for the exit trap: a Stack descriptor paired with its backing memory.

        \note  The exit trap executes when all user tasks have exited (KERNEL_DYNAMIC only),
               restoring the CPU context to the point immediately after IKernel::Start() was called
               so the application can continue after scheduling ends.
        \note  Allocated only in KERNEL_DYNAMIC mode (zero-size otherwise, via STK_ALLOCATE_COUNT).
    */
    struct ExitTrapStack
    {
        typedef ExitTrapStackMemory::MemoryType Memory;

        Stack  stack;  //!< Stack descriptor (SP register value + access mode). Initialised by InitTraps() on every Start().
        Memory memory; //!< Backing stack memory array. Size: STACK_SIZE_MIN elements of Word.
    };

    /*! \typedef SyncObjectList
        \brief   Intrusive list of active ISyncObject instances registered with this kernel.
                 Each sync object in this list receives a Tick() call every kernel tick for
                 timeout tracking. Allocated only when KERNEL_SYNC is set (zero-size otherwise).
    */
    typedef ISyncObject::ListHeadType SyncObjectList;

    KernelService    m_service;         //!< Kernel service singleton exposed to running tasks via IKernelService::GetInstance().
    TPlatform        m_platform;        //!< Platform driver (SysTick, PendSV, context switch implementation).
    TStrategy        m_strategy;        //!< Task-switching strategy (determines which task runs next).
    KernelTask      *m_task_now;        //!< Currently executing task, or \c nullptr before Start() or after all tasks exit.
    TaskStorageType  m_task_storage;    //!< Static pool of _Size KernelTask slots (free slots have m_user == nullptr).
    SleepTrapStack   m_sleep_trap[1];   //!< Sleep trap (always present): executed when all tasks are sleeping.
    ExitTrapStack    m_exit_trap[STK_ALLOCATE_COUNT(TMode, KERNEL_DYNAMIC, 1, 0)]; //!< Exit trap: zero-size in KERNEL_STATIC mode; one entry in KERNEL_DYNAMIC mode.
    EFsmState        m_fsm_state;       //!< Current FSM state. Drives context-switch decision on every tick.
    volatile uint8_t m_request;         //!< Bitmask of pending ERequest flags from running tasks. Written by tasks, read/cleared by UpdateTaskRequest() in tick context.
    volatile EState  m_state;           //!< Current kernel state.
    SyncObjectList   m_sync_list[STK_ALLOCATE_COUNT(TMode, KERNEL_SYNC, 1, 0)]; //!< List of active sync objects. Zero-size (no memory) if KERNEL_SYNC is not set.

    const EFsmState  m_fsm[FSM_STATE_MAX][FSM_EVENT_MAX] = {
    //    FSM_EVENT_SWITCH     FSM_EVENT_SLEEP     FSM_EVENT_WAKE    FSM_EVENT_EXIT
        { FSM_STATE_SWITCHING, FSM_STATE_SLEEPING, FSM_STATE_NONE,   FSM_STATE_EXITING }, // FSM_STATE_SWITCHING
        { FSM_STATE_NONE,      FSM_STATE_NONE,     FSM_STATE_WAKING, FSM_STATE_EXITING }, // FSM_STATE_SLEEPING
        { FSM_STATE_SWITCHING, FSM_STATE_SLEEPING, FSM_STATE_NONE,   FSM_STATE_EXITING }, // FSM_STATE_WAKING
        { FSM_STATE_NONE,      FSM_STATE_NONE,     FSM_STATE_NONE,   FSM_STATE_NONE    }  // FSM_STATE_EXITING
    }; //!< Compile-time FSM transition table. Indexed as m_fsm[current_state][event] -> next_state.
       //!< FSM_STATE_NONE as a next-state means "no transition": the FSM stays in the current state.
       //!< Updated by UpdateFsmState() each tick via GetNewFsmState() -> FetchNextEvent().
};

} // namespace stk

#endif /* STK_H_ */
