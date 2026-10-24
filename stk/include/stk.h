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
#include "strategy/stk_strategy_pthreshold.h"

/*! \file  stk.h
    \brief Top-level STK include. Provides the Kernel class template and all built-in
           task-switching strategies.

    Include this single header in user application code. It transitively pulls in:
     - stk_helper.h             - Task, TaskW, StackMemoryWrapper; and free-function helpers:
                                  GetTid, GetTicks, GetTickResolution, GetTicksFromMs,
                                  GetMsecFromTicks, GetTimeNowMsec, Delay, Sleep, SleepUntil, Yield.
     - stk_strategy_rrobin.h    - SwitchStrategyRoundRobin.
     - stk_strategy_swrrobin.h  - SwitchStrategySmoothWeightedRoundRobin.
     - stk_strategy_monotonic.h - SwitchStrategyMonotonic (SRT rate-monotonic).
     - stk_strategy_edf.h       - SwitchStrategyEdf (Earliest Deadline First).
     - stk_strategy_fpriority.h - SwitchStrategyFixedPriority.
     - stk_strategy_pthreshold  - SwitchStrategyPreemptionThreshold (Fixed-priority preemptive with Preemption-Threshold).
*/

namespace stk {

// Helper function for Kernel::UpdateTaskState.
template <bool TicklessMode> __stk_forceinline Timeout GetInitialSleepTicks();
template <> __stk_forceinline Timeout GetInitialSleepTicks<true>()  { return STK_TICKLESS_TICKS_MAX; }
template <> __stk_forceinline Timeout GetInitialSleepTicks<false>() { return 1; }

/*! \class Kernel
    \brief Concrete implementation of IKernel.

    All configuration is expressed as template parameters. No virtual dispatch, no heap
    allocation - the entire kernel, tasks, and traps live in statically reserved storage.

    \tparam TMode:     Bitmask of EKernelMode flags that configures kernel features:
                        - KERNEL_STATIC   - fixed task list, no add/remove after Start().
                        - KERNEL_DYNAMIC  - tasks may be added or removed at runtime.
                        - KERNEL_HRT      - Hard Real-Time mode (must combine with STATIC or DYNAMIC).
                        - KERNEL_SYNC     - enables synchronization primitives (Mutex, Event, etc.).
                        - KERNEL_TICKLESS - enables tickless low-power operation. Requires
                                            STK_TICKLESS_IDLE=1 in stk_config.h. Incompatible with
                                            KERNEL_HRT (tickless suppresses the timer, which destroys
                                            the precise periodicity HRT depends on - enforced by the
                                            compile-time assertion TICKLESS_HRT_CONFLICT).
                       - KERNEL_TIMESLICE - enables time-slicing: a running task is rotated behind its
                                            same-priority peers when its quantum (ITask::GetTimeSlice,
                                            IKernelService::SetTimeSlice) expires. Requires a strategy with
                                            TIMESLICE_API == 1 (e.g. SwitchStrategyPT32_TS), incompatible
                                            with KERNEL_HRT (TIMESLICE_HRT_CONFLICT).
                       - KERNEL_TELEMETRY - enables introspection functions for diagnostics / telemetry
                                            (IKernel::GetTaskState). Without it these functions are compiled
                                            out and report failure. Combines with any other mode.
                       KERNEL_STATIC and KERNEL_DYNAMIC are mutually exclusive.
    \tparam TSize:     Maximum number of concurrent tasks. Must be > 0.
    \tparam TStrategy: Task-switching strategy type (e.g. SwitchStrategyRoundRobin). Must inherit ITaskSwitchStrategy.
    \tparam TPlatform: Platform driver type (e.g. PlatformArmCortexM, or PlatformDefault). Must inherit IPlatform.

    \note  At least 1 task is required: TSize must be > 0 (enforced by compile-time assertion).
    \note  KERNEL_HRT is incompatible with weighted scheduling strategies (WEIGHT_API == true),
           also enforced by a compile-time assertion.
    \note  KERNEL_TICKLESS is incompatible with KERNEL_HRT, also enforced by a compile-time
           assertion (TICKLESS_HRT_CONFLICT).

    Usage example:
    \code
    static stk::Kernel<KERNEL_STATIC, 3, SwitchStrategyRoundRobin, PlatformDefault> kernel;

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
class Kernel
#ifndef _STK_UNDER_TEST
final
#endif
: public IKernel, private IPlatform::IEventHandler
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

    /*! \class KernelTask
        \brief Internal per-slot kernel descriptor that wraps a user ITask instance.

        Holds the kernel-side state for one task slot: the Stack descriptor, sleep timer,
        HRT or SRT scheduling metadata, optional wait object (KERNEL_SYNC), and optional
        weight (weighted strategies). The task-switching strategy operates on KernelTask
        pointers rather than ITask pointers directly.

        A slot is "free" when m_user == NULL (IsBusy() == false). The Kernel pre-allocates
        TSize slots in m_task_storage; AddTask() finds a free slot and calls Bind().
    */
    class KernelTask final : public IKernelTask
    {
        friend class Kernel;

        /*! \enum  EStateFlags
            \brief Bitmask of transient state flags. Set by the task or the kernel and
                   consumed (cleared) during UpdateTaskState() on the next tick.
        */
        enum EStateFlags : uint32_t
        {
            STATE_NONE            = 0U,        //!< No pending state flags.
            STATE_REMOVE_PENDING  = (1U << 0), //!< Task returned from its Run function; slot will be freed on the next tick (KERNEL_DYNAMIC only).
            STATE_SLEEP_PENDING   = (1U << 1), //!< Task called Sleep/SleepUntil/Yield; strategy's OnTaskSleep() will be invoked on the next tick (sleep-aware strategies only).
            STATE_WAKE_PENDING    = (1U << 2), //!< Task wake is pending (see Wake).
            STATE_SUSPENDED       = (1U << 3), //!< Task is suspended by SuspendTask.
            STATE_YIELD_PENDING   = (1U << 4), //!< Task called Yield() and the strategy handled it via OnTaskYield() (task stays runnable); cleared by the context switch request or by the next tick, whichever comes first.
            STATE_SUSPEND_PENDING = (1U << 5), //!< SuspendTask() was requested while the task was sleeping/waiting; converted into STATE_SUSPENDED when the sleep/wait ends (deferred suspension).
        };

    public:
        /*! \brief Construct a free (unbound) task slot. All fields set to zero/null.
            \note  In KERNEL_SYNC mode the embedded WaitObject back-pointer is wired to this
                   KernelTask at construction so the wait object can wake its owning task.
        */
        explicit KernelTask() : m_user(nullptr), m_stack(), m_state(STATE_NONE), m_time_sleep(NO_WAIT),
            m_hrt(), m_rt_weight(), m_base_weight(), m_wait_obj(), m_ts()
        {
            // bind to wait object
            if __stk_constexpr_cpp17 (IsSyncMode())
            {
                m_wait_obj[0].m_task = this;
            }
        }

        /*! \brief  Get bound user task.
            \return Pointer to the ITask.
        */
        ITask *GetUserTask() override { return m_user; }

        /*! \brief  Get bound user task (const).
            \return Pointer to the immutable ITask.
        */
        const ITask *GetUserTask() const { return m_user; }

        /*! \brief  Get stack descriptor for this task slot.
            \return Stack info (SP register value and access mode flags).
        */
        Stack GetUserStack() const override { return m_stack;}

        /*! \brief  Check whether this slot is bound to a user task.
            \return \c true if a user task is assigned (m_user != NULL); \c false if the slot is free.
        */
        bool IsBusy() const { return (m_user != nullptr); }

        /*! \brief  Check whether this task is currently sleeping (waiting for a tick or a wake event).
            \return \c true if m_time_sleep < 0 (negative value encodes remaining sleep ticks).
        */
        __stk_forceinline bool IsSleeping() const override { return (m_time_sleep < 0); }

        /*! \brief  Check whether this task is currently sleeping infinitely.
            \return \c true if m_time_sleep == -WAIT_INFINITE.
        */
        __stk_forceinline bool IsSleepInfinite() const { return (m_time_sleep == -WAIT_INFINITE); }

        /*! \brief  Get task identifier.
            \return TId derived from the bound ITask pointer address (unique per task instance).
        */
        TId GetTid() const { return GetTidFromUserTask(m_user); }

        /*! \brief  Wake this task on the next scheduling tick.
            \note   Sets m_time_sleep to -1 (one tick remaining) so the task exits sleep state
                    on the next UpdateTaskState() pass. Asserts that the task is currently sleeping.
        */
        void Wake() override
        {
            STK_ASSERT(IsSleeping());

            // wakeup on a next cycle
            m_time_sleep = -1;

            // notify kernel that this task, even if was going to sleep will be woken on a next tick
            if ((m_state & STATE_SLEEP_PENDING) != 0U)
            {
                m_state |= STATE_WAKE_PENDING;
            }
        }

        /*! \brief  Turn a deferred suspension (STATE_SUSPEND_PENDING) into an effective one.
            \note   Called by UpdateTaskState() at the moment the sleep/wait of the task ends. The task
                    stays asleep infinitely until ResumeTask(). The strategy has already removed the task
                    from its runnable set on OnTaskSleep(), therefore no OnTaskWake() is delivered.
            \note   Must be called with hw::CriticalSection held or from the tick handler.
        */
        void CommitSuspend()
        {
            m_state = ((m_state & ~STATE_SUSPEND_PENDING) | STATE_SUSPENDED);
            m_time_sleep = -WAIT_INFINITE;

            __stk_full_memfence();
        }

        /*! \brief     Update the run-time scheduling weight (weighted strategies only).
            \param[in] weight: New current weight. Ignored unless TStrategy::WEIGHT_API is true.
        */
        void SetCurrentWeight(Weight weight) override
        {
            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API)
            {
                m_rt_weight[0] = weight;
            }
        }

        /*! \brief  Get static scheduling weight from the user task.
            \return ITask::GetWeight() if WEIGHT_API is true; 1 otherwise.
        */
        Weight GetWeight() const override
        {
            Weight static_weight;

            if __stk_constexpr_cpp17 (TStrategy::PRIORITY_INHERITANCE_API)
            {
                if (m_rt_weight[0] != NO_WEIGHT)
                {
                    static_weight = m_rt_weight[0];
                }
                else
                {
                    static_weight = GetBaseWeight();
                }
            }
            else
            {
                static_weight = GetBaseWeight();
            }

            return static_weight;
        }

        /*! \brief  Get base scheduling weight from the user task, ignoring priority inheritance.
            \return ITask::GetWeight() if WEIGHT_API is true; DEFAULT_WEIGHT otherwise.
        */
        Weight GetBaseWeight() const override
        {
            Weight base_weight;

            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API)
            {
                // run-time override set via IKernelService::SetWeight() takes precedence
                base_weight = (m_base_weight[0] != NO_WEIGHT ? m_base_weight[0] : m_user->GetWeight());
            }
            else
            {
                base_weight = DEFAULT_WEIGHT;
            }

            return base_weight;
        }

        /*! \brief     Override the base weight at run-time (see IKernelService::SetWeight).
            \param[in] weight: New base weight. Ignored unless TStrategy::WEIGHT_API is true.
        */
        void SetBaseWeight(Weight weight)
        {
            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API)
            {
                m_base_weight[0] = weight;
            }
            else
            {
                STK_UNUSED(weight);
            }
        }

        /*! \brief  Get current (run-time) scheduling weight.
            \return m_rt_weight[0] if WEIGHT_API is true; 1 otherwise.
            \note   The run-time weight is decremented each tick by the weighted strategy and
                    reset to GetWeight() when exhausted.
        */
        Weight GetCurrentWeight() const override
        {
            Weight cur_weight;
          
            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API)
            {
                cur_weight = m_rt_weight[0];
            }
            else
            {
                cur_weight = DEFAULT_WEIGHT;
            }
            
            return cur_weight;
        }

        /*! \brief  Get HRT scheduling periodicity.
            \return Period in ticks between successive activations of this task.
            \note   KERNEL_HRT mode only. Asserts if called outside HRT mode.
        */
        Timeout GetHrtPeriodicity() const override
        {
            STK_ASSERT(IsHrtMode());
            
            Timeout to;
            
            if __stk_constexpr_cpp17 (IsHrtMode())
            {
                to = m_hrt[0].periodicity;
            }
            else
            {
                to = 0;
            }
            
            return to;
        }

        /*! \brief  Get absolute HRT deadline (ticks elapsed since task was activated).
            \return Deadline in ticks. The task must complete its work within this many ticks
                    of being switched in, or OnDeadlineMissed() is invoked.
            \note   KERNEL_HRT mode only. Asserts if called outside HRT mode.
        */
        Timeout GetHrtDeadline() const override
        {
            STK_ASSERT(IsHrtMode());
            
            Timeout deadline;

            if __stk_constexpr_cpp17 (IsHrtMode())
            {
                deadline = m_hrt[0].deadline;
            }
            else
            {
                deadline = 0;
            }
            
            return deadline;
        }

        /*! \brief  Get remaining HRT deadline (ticks left before the deadline expires).
            \return deadline - duration: ticks remaining before the task must call Yield().
                    Zero means this is the last tick still within the deadline; a negative value
                    means the deadline has already been missed (see HrtIsDeadlineMissed(), which
                    uses a strict duration > deadline comparison).
            \note   KERNEL_HRT mode only. Asserts if called outside HRT mode or while sleeping.
        */
        Timeout GetHrtRelativeDeadline() const override
        {
            STK_ASSERT(IsHrtMode());
            STK_ASSERT(!IsSleeping());
            
            Timeout relative_deadline;

            if __stk_constexpr_cpp17 (IsHrtMode())
            {
                const typename KernelTask::HrtInfo &hrt = m_hrt[0];

                relative_deadline = (hrt.deadline - hrt.duration);
            }
            else
            {
                relative_deadline = 0;
            }
            
            return relative_deadline;
        }

        Timeout GetSleepTicks(Timeout sleep_ticks)
        {
            // note: task sleep time is negative
            Timeout task_sleep = Max<Timeout>(NO_WAIT, -m_time_sleep);

            if __stk_constexpr_cpp17 (IsSyncMode())
            {
                typename KernelTask::WaitObject &wait_obj = m_wait_obj[0];

                // likely task is sleeping during sync operation (see Wait)
                if (wait_obj.IsWaiting())
                {
                    // note: sync wait time is positive
                    task_sleep = wait_obj.m_time_wait;

                    // we shall account for only valid time (when task is waiting during sync operation)
                    if (task_sleep > NO_WAIT)
                    {
                        sleep_ticks = Min(sleep_ticks, task_sleep);
                    }
                }
                else
                {
                    sleep_ticks = Min(sleep_ticks, task_sleep);
                }
            }
            else
            {
                sleep_ticks = Min(sleep_ticks, task_sleep);
            }

            // clamp to [1, STK_TICKLESS_TICKS_MAX] range
            return Max<Timeout>(1, sleep_ticks);
        }

    protected:
        /*! \brief Destructor.
            \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
        */
        STK_VIRT_DTOR ~KernelTask() = default;

        /*! \class HrtInfo
            \brief Per-task Hard Real-Time (HRT) scheduling metadata.
            \note  Allocated only when TMode includes KERNEL_HRT. Zero-size in SRT mode.
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

            Timeout       periodicity; //!< Activation period in ticks: the task is re-activated every this many ticks.
            Timeout       deadline;    //!< Maximum allowed active duration in ticks (relative to switch-in). Exceeding this triggers OnDeadlineMissed().
            Timeout       duration;    //!< Ticks spent in the active (non-sleeping) state in the current period. Incremented by UpdateTaskState(); reset to 0 on switch-out.
            volatile bool done;        //!< Set to true when the task signals work completion (via Yield() or on exit). Triggers HrtOnSwitchedOut() at the next context switch.
        };

        /*! \class TsInfo
            \brief Per-task time-slice (KERNEL_TIMESLICE) metadata.
            \note  Allocated only when TMode includes KERNEL_TIMESLICE. Zero-size otherwise.
        */
        struct TsInfo
        {
            TsInfo() : quantum(NO_TIME_SLICE), left(NO_TIME_SLICE)
            {}

            /*! \brief Clear all fields, ready for slot re-use.
            */
            void Clear()
            {
                quantum = NO_TIME_SLICE;
                left    = NO_TIME_SLICE;
            }

            /*! \brief Restart the current slice.
            */
            void Reload()
            {
                left = quantum;
            }

            Timeout          quantum; //!< Configured slice (ticks), NO_TIME_SLICE if time-slicing is disabled for the task.
            volatile Timeout left;    //!< Ticks left in the current slice. Written by the tick handler and, under the critical section, by task context.
        };

        /*! \class WaitObject
            \brief Concrete implementation of IWaitObject, embedded in each KernelTask slot.
            \note  Allocated only when TMode includes KERNEL_SYNC. Zero-size otherwise.
            \note  One WaitObject per KernelTask, the back-pointer m_task is wired at
                   KernelTask construction and never changes.
        */
        struct WaitObject final : public IWaitObject
        {
            explicit WaitObject() : m_task(nullptr), m_sync_obj(nullptr), m_time_wait(NO_WAIT),
                m_state(STATE_NONE)
            {}

            /*! \brief Destructor.
                \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
            */
            STK_VIRT_DTOR ~WaitObject() = default;

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
            TId GetTid() const override { return m_task->GetTid(); }

            /*! \brief  Get current state.
                \return State value.
                \see    EState
            */
            EState GetState() const override { return m_state; }

            /*! \brief  Check if busy with waiting.
                \return \c true if waiting, \c false if not.
            */
            bool IsWaiting() const { return (m_sync_obj != nullptr); }

            /*! \brief  Get wait result.
                \return Result value.
                \see    OnTaskWait, EWaitResult
            */
            EWaitResult GetWaitResult() const
            {
                STK_ASSERT(!IsWaiting());

                switch (GetState())
                {
                case STATE_TIMEOUT:
                    return WAIT_RESULT_TIMEOUT;
                case STATE_CANCELLED:
                    return WAIT_RESULT_CANCELED;
                default: {
                    STK_ASSERT(m_state != STATE_NONE);
                    STK_ASSERT(m_state != STATE_WAIT);
                    STK_ASSERT(m_state == STATE_SIGNALED);
                    return WAIT_RESULT_SIGNAL; }
                }
            }

            /*! \brief  Clear state.
                \see    OnTaskWait
            */
            void ClearState()
            {
                STK_ASSERT(!IsWaiting());

                m_state = STATE_NONE;
            }

            /*! \brief     Wake the waiting task (called by ISyncObject when it signals).
                \param[in] timeout: \c true if woken because the timeout expired, \c false if signalled.
                \note      Clears m_time_wait, records the timeout flag, removes this object from
                           m_sync_obj's wait list, nulls m_sync_obj, then calls m_task->Wake().
            */
            void Wake(bool timeout) override
            {
                STK_ASSERT(IsWaiting());

                m_state     = (timeout ? STATE_TIMEOUT : STATE_SIGNALED);
                m_time_wait = NO_WAIT;

                m_sync_obj->RemoveWaitObject(this);
                m_sync_obj = nullptr;

                return m_task->Wake();
            }

            /*! \brief  Advance the timeout countdown by a number of ticks.
                \return \c true if the wait is still active (not yet timed out),
                        \c false if the timeout just expired (caller should stop ticking this object).
                \note   Called by UpdateSyncObjects() each kernel tick.
                        WAIT_INFINITE waits never time out and always return \c true.
            */
            bool Tick(Timeout elapsed_ticks) override
            {
                if (m_time_wait != WAIT_INFINITE)
                {
                    if (m_state == STATE_WAIT)
                    {                  
                        m_time_wait -= elapsed_ticks;

                        if (m_time_wait <= NO_WAIT)
                        {
                            m_state = STATE_TIMEOUT;
                        }
                    }
                }

                return (m_state == STATE_WAIT);
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
                STK_ASSERT(!IsWaiting());

                m_sync_obj  = sync_obj;
                m_time_wait = timeout;
                m_state     = STATE_WAIT;

                sync_obj->AddWaitObject(this);
            }

            /*! \brief  Cancel an in-progress wait without waking the owning task.
                \note   Used when the owning task is being forcibly removed (see
                        KernelTask::ScheduleRemoval) while blocked on a synchronization object.
                        Unlike Wake(), this does not touch m_task's sleep state or set m_timeout -
                        the task is being torn down and will never observe the wait result.
                \note   No-op if not currently waiting.
            */
            void CancelNoWake()
            {
                if (IsWaiting())
                {
                    m_state = STATE_CANCELLED;

                    m_sync_obj->RemoveWaitObject(this);
                    m_sync_obj = nullptr;
                }
            }

            /*! \brief  Cancel an in-progress wait and wake the owning task early.
                \note   Used by IKernel::CancelTaskWait() to interrupt a task blocked in
                        OnTaskWait(), e.g. for cancellable I/O or shutdown sequences.
                        Unlike CancelNoWake(), this DOES wake the task - the task is not being
                        torn down, it needs to legitimately resume execution and observe the
                        result via IsCanceled().
                \note   Sets m_canceled instead of m_timeout, so the caller can distinguish a
                        cancelled wait (WAIT_RESULT_CANCELED) from a genuine timeout or signal.
                \note   Caller (IKernel::CancelTaskWait()) is responsible for checking IsWaiting()
                        before calling this - it asserts rather than no-op, mirroring Wake().
            */
            void Cancel()
            {
                STK_ASSERT(IsWaiting());

                m_state     = STATE_CANCELLED;
                m_time_wait = NO_WAIT;

                m_sync_obj->RemoveWaitObject(this);
                m_sync_obj = nullptr;

                m_task->Wake();
            }

            /*! \brief  Cancel an in-progress wait and wake the owning task early if was waiting.
                \return true if cancelled active waiting, false otherwise.
                \see    Cancel, IsWaiting
            */
            bool CancelIfWaiting()
            {
                const bool was_waiting = IsWaiting();
                if (was_waiting)
                {
                    Cancel();
                }

                return was_waiting;
            }

            KernelTask     *m_task;      //!< Back-pointer to the owning KernelTask. Set once at construction; never changes.
            ISyncObject    *m_sync_obj;  //!< Sync object this wait is registered with, or \c NULL when not waiting.
            Timeout         m_time_wait; //!< Ticks remaining until timeout. Decremented each tick; WAIT_INFINITE means no timeout.
            volatile EState m_state;     //!< State of the object, see WaitObject::EState.
        };

        /*! \brief     Bind this slot to a user task: set access mode, task ID, and initialize the stack.
            \param[in] platform: Platform driver used to initialize the stack frame.
            \param[in] user_task: User task to bind. Asserts that the stack is successfully initialized.
        */
        void Bind(TPlatform *platform, ITask *user_task)
        {
            // bind user task (GetTid depends on m_user)
            m_user = user_task;

            // set access mode for this stack
            m_stack.access_mode = user_task->GetAccessMode();

            // set task id for tracking purpose
        #if STK_STACK_NEEDS_TASK_ID
            m_stack.tid = GetTid();
            STK_ASSERT(m_stack.tid != TID_NONE);
        #endif

            // init stack of the user task
            platform->InitStack(STACK_USER_TASK, &m_stack, user_task, user_task);

            // initialize current weight to NO_WEIGHT for priority inheritance mechanism
            if __stk_constexpr_cpp17 (TStrategy::PRIORITY_INHERITANCE_API)
            {
                SetCurrentWeight(NO_WEIGHT);
            }

            // no run-time override of the base weight: ITask::GetWeight() is used
            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API)
            {
                m_base_weight[0] = NO_WEIGHT;
            }

            // time-slice quantum of the task, the first slice starts full
            if __stk_constexpr_cpp17 (IsTimesliceMode())
            {
                const Timeout quantum = user_task->GetTimeSlice();
                STK_ASSERT(quantum >= NO_TIME_SLICE);

                typename KernelTask::TsInfo &ts = m_ts[0];

                ts.quantum = quantum;
                ts.left    = quantum;
            }
        }

        /*! \brief Reset this slot to the free (unbound) state, clearing all scheduling metadata.
            \note  Called by RemoveTask(). After Unbind() the slot is available for the next AddTask().
        */
        void Unbind()
        {
            if __stk_constexpr_cpp17 (IsSyncMode())
            {
                // should be freed from waiting on task exit
                STK_ASSERT(!m_wait_obj[0].IsWaiting());
            }

            m_user       = nullptr;
            m_stack      = {};
            m_state      = STATE_NONE;
            m_time_sleep = 0;

            if __stk_constexpr_cpp17 (IsHrtMode())
            {
                m_hrt[0].Clear();
            }

            if __stk_constexpr_cpp17 (IsTimesliceMode())
            {
                m_ts[0].Clear();
            }
        }

        /*! \brief     Schedule the removal of the task from the kernel on next tick.
            \param[in] kernel: Owning kernel (used to deliver the sleep entry to the strategy immediately
                       if this is not the currently scheduled task).
            \note      Must be called with hw::CriticalSection held.
            \note      Makes the task sleep infinitely and marks it pending for removal. If the task is not
                       m_task_now (e.g. removed by another task via ScheduleTaskRemoval()) and it was not
                       sleeping yet, OnTaskSleep() is delivered to the strategy right away: only the
                       currently scheduled task may have STATE_SLEEP_PENDING consumed later by a tick or
                       a forced context switch, otherwise the strategy could still return this task
                       from GetNext() although it IsSleeping(). A task that is already sleeping has already
                       been reported to the strategy and is not reported again.
        */
        void ScheduleRemoval(Kernel *const kernel)
        {
            // if blocked on a sync object, detach first - otherwise the WaitObject stays linked
            // in the sync object's wait list after Unbind() frees this slot, and a later
            // Wake()/WakeAll() would corrupt whatever task the slot gets rebound to
            if __stk_constexpr_cpp17 (IsSyncMode())
            {
                m_wait_obj[0].CancelNoWake();
            }

            // make this task sleeping to switch it out from scheduling process
            ScheduleSleep(WAIT_INFINITE);

            // mark it as done HRT task
            if __stk_constexpr_cpp17 (IsHrtMode())
            {
                HrtOnWorkCompleted();
            }

            // mark it as pending for removal
            m_state |= STATE_REMOVE_PENDING;

            // the task may not be the current one (e.g. removed via ScheduleTaskRemoval() by another task):
            // deliver the sleep entry now, otherwise a forced context switch could pick this task from the
            // strategy although it IsSleeping(); skipped if the task was already sleeping (flag is not set
            // then, the strategy has already been notified)
            if ((this != kernel->m_task_now) && ((m_state & STATE_SLEEP_PENDING) != 0U))
            {
                kernel->ProcessTaskPendingSleep(this);
            }
        }

        /*! \brief     Check if task is pending removal.
        */
        bool IsPendingRemoval() const { return ((m_state & STATE_REMOVE_PENDING) != 0U); }

        /*! \brief     Check if Stack Pointer (SP) belongs to this task.
            \param[in] SP: Stack Pointer.
        */
        bool IsMemoryOfSP(Word SP) const
        {
            bool is_match = false;

            const Word start = hw::PtrToWord(m_user->GetStack());
            const Word end   = start + (m_user->GetStackSize() * sizeof(Word));

            if ((SP >= start) && (SP <= end))
            {
                is_match = true;
            }
        #if STK_TZ_SECURE // lookup Secure memory region too when on a Secure side
            else
            {
                IStackMemory *const secure_mem = m_user->GetSecureStackMemory();

                if (secure_mem != nullptr)
                {
                    const Word s_start = hw::PtrToWord(secure_mem->GetStack());
                    const Word s_end   = s_start + (secure_mem->GetStackSize() * sizeof(Word));

                    if ((SP >= s_start) && (SP <= s_end))
                    {
                        is_match = true;
                    }
                }
            }
        #endif

            return is_match;
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

            typename KernelTask::HrtInfo &hrt = m_hrt[0];

            hrt.periodicity = periodicity_tc;
            hrt.deadline    = deadline_tc;

            if (start_delay_tc > 0)
            {
                ScheduleSleep(start_delay_tc);
            }
        }

        /*! \brief     Called when task is switched into the scheduling process.
            \note      Related to stk::KERNEL_HRT mode only.
        */
        void HrtOnSwitchedIn() {}

        /*! \brief     Called when task is switched out from the scheduling process.
            \note      Related to stk::KERNEL_HRT mode only.
        */
        void HrtOnSwitchedOut()
        {
            typename KernelTask::HrtInfo &hrt = m_hrt[0];

            const Timeout duration = hrt.duration;

            STK_ASSERT(duration >= 0);

            const Timeout sleep = hrt.periodicity - duration;
            if (sleep > 0)
            {
                ScheduleSleep(sleep);
            }

            hrt.duration = 0;
            hrt.done     = false;
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
        bool HrtIsDeadlineMissed(Timeout duration) const
        {
            return (duration > m_hrt[0].deadline);
        }

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
            if (!IsSleeping())
            {
                m_state |= STATE_SLEEP_PENDING;
            }

            m_time_sleep = -ticks;

            // task leaves the CPU voluntarily: its next run starts with a full slice
            if __stk_constexpr_cpp17 (IsTimesliceMode())
            {
                m_ts[0].Reload();
            }

            __stk_full_memfence();
        }

        /*! \brief  Mark the task as yielding (strategy handled Yield() without sleeping the task).
        */
        void ScheduleYield()
        {
            if __stk_constexpr_cpp17 (TStrategy::NOSLEEP_YIELD_API)
            {
                m_state |= STATE_YIELD_PENDING;

                // yielding restarts the slice (tx_thread_relinquish semantics)
                if __stk_constexpr_cpp17 (IsTimesliceMode())
                {
                    m_ts[0].Reload();
                }

                __stk_full_memfence();
            }
        }

        /*! \brief  Request a context switch and wait until the kernel consumes the pending yield.
        */
        void BusyWaitWhileYielding(Kernel *const kernel) const
        {
            if __stk_constexpr_cpp17 (TStrategy::NOSLEEP_YIELD_API)
            {
                kernel->m_platform.ForceContextSwitch(GetTid());

                while ((m_state & STATE_YIELD_PENDING) != 0U)
                {
                    __stk_relax_cpu();
                }
            }
        }

        /*! \brief  Block further execution of the task's context while in sleeping state.
        */
        void BusyWaitWhileSleeping(Kernel *kernel) const
        {
            kernel->m_platform.ForceContextSwitch(GetTid());

            while (IsSleeping())
            {
                __stk_relax_cpu();
            }
        }
        
        /*! \brief  Get pointer to user Stack.
            \return Pointer to the Stack (SP register value and access mode flags).
        */
        Stack *GetUserStackPtr() { return &m_stack; }

        ITask            *m_user;       //!< Bound user task, or \c NULL when slot is free.
        Stack             m_stack;      //!< Stack descriptor (SP register value + access mode + optional tid).
        volatile uint32_t m_state;      //!< Bitmask of EStateFlags. Written by task thread, read/cleared by kernel tick.
        volatile Timeout  m_time_sleep; //!< Sleep countdown: negative while sleeping (absolute value = ticks remaining), zero when awake.
        HrtInfo           m_hrt        [STK_ALLOCATE_COUNT<TMode, KERNEL_HRT, 1U, 0U>::Value];         //!< HRT metadata. Zero-size (no memory) in non-HRT mode.
        Weight            m_rt_weight  [STK_ALLOCATE_COUNT<TStrategy::WEIGHT_API, 1U, 1U, 0U>::Value]; //!< Run-time weight for weighted-round-robin scheduling. Zero-size for unweighted strategies.
        Weight            m_base_weight[STK_ALLOCATE_COUNT<TStrategy::WEIGHT_API, 1U, 1U, 0U>::Value]; //!< Run-time override of ITask::GetWeight() (NO_WEIGHT = not overridden). Zero-size for unweighted strategies.
        WaitObject        m_wait_obj   [STK_ALLOCATE_COUNT<TMode, KERNEL_SYNC, 1U, 0U>::Value];        //!< Embedded wait object for synchronization. Zero-size (no memory) if KERNEL_SYNC is not set.
        TsInfo            m_ts         [STK_ALLOCATE_COUNT<TMode, KERNEL_TIMESLICE, 1U, 0U>::Value];   //!< Time-slice metadata. Zero-size (no memory) if KERNEL_TIMESLICE is not set.
    };

    /*! \class KernelService
        \brief Concrete implementation of IKernelService exposed to running tasks.

        Holds the global tick counter (m_ticks, updated atomically by IncrementTick() each
        SysTick) and a typed pointer to the platform driver. Tasks access this object via
        IKernelService::GetInstance() which returns the singleton registered at Initialize().
    */
    class KernelService final : public IKernelService
    {
        friend class Kernel;

    public:
        TId GetTid() const override { return m_kernel->m_platform.GetTid(); }

        Ticks GetTicks() const override { return hw::ReadVolatile64(&m_ticks); }

        uint32_t GetTickResolution() const override  { return m_kernel->m_platform.GetTickResolution(); }

        Cycles GetSysTimerCount() const override { return m_kernel->m_platform.GetSysTimerCount(); }

        uint32_t GetSysTimerFrequency() const override { return m_kernel->m_platform.GetSysTimerFrequency(); }

        void Delay(Timeout ticks) override
        {
            STK_ASSERT(!hw::IsInsideISR());
            STK_ASSERT(ticks >= 0);

            Ticks now = GetTicks();
            const Ticks deadline = now + ticks;
            STK_ASSERT(deadline >= now);

            for (; now < deadline; now = GetTicks())
            {
                __stk_relax_cpu();
            }
        }

        void Sleep(Timeout ticks) override
        {
            STK_ASSERT(!hw::IsInsideISR());
            STK_ASSERT(ticks >= 0);

            if __stk_constexpr_cpp17 (!IsHrtMode())
            {
                m_kernel->m_platform.Sleep(ticks);
            }
            else
            {
                // sleeping is not supported in HRT mode, task will sleep according to its periodicity and workload
                STK_ASSERT(false);
            }
        }

        bool SleepUntil(Ticks timestamp) override
        {
            STK_ASSERT(!hw::IsInsideISR());

            if __stk_constexpr_cpp17 (!IsHrtMode())
            {
                return m_kernel->m_platform.SleepUntil(timestamp);
            }
            else
            {
                // sleeping is not supported in HRT mode, task will sleep according to its periodicity and workload
                STK_ASSERT(false);
                return false;
            }
        }

        void SleepCancel(TId task_id) override
        {
            if __stk_constexpr_cpp17 (!IsHrtMode())
            {
                m_kernel->CancelTaskWait(GetUserTaskFromTid(task_id));
            }
        }

        void SwitchToNext() override
        {
            STK_ASSERT(!hw::IsInsideISR());

            m_kernel->m_platform.SwitchToNext();
        }

        void ExitTask() override
        {
            STK_ASSERT(!hw::IsInsideISR());

            if __stk_constexpr_cpp17 (IsDynamicMode())
            {
                KernelTask *const self = m_kernel->FindTaskBySP(m_kernel->m_platform.GetCallerSP());
                STK_ASSERT(self != nullptr);

                // same path as returning from ITask::Run() (exit trap): marks the task sleeping infinitely and
                // pending for removal, the kernel removes it on the next tick and calls ITask::OnExit()
                m_kernel->OnTaskExit(self->GetUserStackPtr());

                // switch out and never come back: the task is not scheduled anymore
                for (;;)
                {
                    self->BusyWaitWhileSleeping(m_kernel);
                }
            }
            else
            {
                // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to exit
                STK_KERNEL_PANIC(KERNEL_PANIC_BAD_MODE);
            }
        }

        EWaitResult Wait(ISyncObject *sobj, IMutex *mutex, Timeout ticks) override
        {
            if __stk_constexpr_cpp17 (IsSyncMode())
            {
                return m_kernel->m_platform.Wait(sobj, mutex, ticks);
            }
            else
            {
                STK_ASSERT(false);
                return WAIT_RESULT_FAIL;
            }
        }

        void Wake(ISyncObject *sobj, bool all) override
        {
            if __stk_constexpr_cpp17 (IsSyncMode())
            {
                if (all)
                {
                    ISyncObject::WakeAll(GetMutableWaitList(sobj));
                }
                else
                {
                    ISyncObject::WakeOne(GetMutableWaitList(sobj));
                }
            }
            else
            {
                STK_ASSERT(false);
            }
        }

        Timeout Suspend() override
        {
            if __stk_constexpr_cpp17 (IsTicklessMode())
            {
                return m_kernel->m_platform.Suspend();
            }
            else
            {
                STK_ASSERT(false);
                return 0;
            }
        }

        void Resume(Timeout elapsed_ticks) override
        {
            if __stk_constexpr_cpp17 (IsTicklessMode())
            {
                return m_kernel->m_platform.Resume(elapsed_ticks);
            }
            else
            {
                STK_ASSERT(false);
            }
        }

        void InheritWeight(TId tid, Weight weight) override
        {
            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API && TStrategy::PRIORITY_INHERITANCE_API)
            {
                m_kernel->OnInheritWeight(tid, weight);
            }
        }

        void RestoreWeight(TId tid, ISyncObject *sobj) override
        {
            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API && TStrategy::PRIORITY_INHERITANCE_API)
            {
                m_kernel->OnRestoreWeight(tid, sobj);
            }
        }

        Weight SetWeight(TId tid, Weight weight) override
        {
            Weight result;

            if __stk_constexpr_cpp17 (TStrategy::WEIGHT_API && TStrategy::PRIORITY_INHERITANCE_API)
            {
                // critical section: avoid race with OnTick

                const hw::CriticalSection::ScopedLock cs_;

                result = m_kernel->OnSetWeight(tid, weight);
            }
            else
            {
                // strategy must handle OnTaskWeightChange() events
                STK_UNUSED(tid);
                STK_UNUSED(weight);
                STK_ASSERT(false);

                result = NO_WEIGHT;
            }

            return result;
        }

        Timeout SetTimeSlice(TId tid, Timeout ticks) override
        {
            Timeout result;

            if __stk_constexpr_cpp17 (IsTimesliceMode())
            {
                // critical section: avoid race with OnTick

                const hw::CriticalSection::ScopedLock cs_;

                result = m_kernel->OnSetTimeSlice(tid, ticks);
            }
            else
            {
                STK_UNUSED(tid);
                STK_UNUSED(ticks);
                STK_ASSERT(false);

                result = NO_TIME_SLICE;
            }

            return result;
        }

    private:
        /*! \brief Construct an uninitialized service instance (m_platform = null, m_ticks = 0).
            \note  Fully initialized by Initialize(). Private; constructed only as a member of Kernel.
        */
        explicit KernelService() : m_kernel(nullptr), m_ticks(0)
        {}

        /*! \brief Destructor.
            \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
        */
        STK_VIRT_DTOR ~KernelService() = default;

        /*! \brief     Initialize instance.
            \note      When call completes Singleton<IKernelService *> will start referencing this
                       instance (see g_KernelService).
            \param[in] kernel: Kernel instance.
        */
        void Initialize(Kernel *kernel)
        {
            m_kernel = kernel;
        }

        /*! \brief     Increment counter by value.
            \param[in] advance: Number of ticks to add to the counter.
        */
        void IncrementTicks(Ticks advance)
        {
            // using WriteVolatile64() to guarantee correct lockless reading order by ReadVolatile64
            hw::WriteVolatile64(&m_ticks, m_ticks + advance);
        }

        Kernel        *m_kernel; //!< Pointer to the Kernel.
        volatile Ticks m_ticks;  //!< Global tick counter. Written via hw::WriteVolatile64() by IncrementTick() (ISR context); read via hw::ReadVolatile64() by GetTicks() (task context) for a lock-free consistent 64-bit read on 32-bit CPUs.
    };

public:
    /*! \brief Maximum number of concurrently registered tasks. Fixed at compile time. Exceeding this limit in AddTask() triggers a compile-time assert (TASKS_MAX > 0) and a runtime STK_ASSERT.
    */
    static constexpr size_t TASKS_MAX = TSize;

    /*! \brief Construct the kernel with all storage zero-initialized and m_kstate set to
               KSTATE_INACTIVE (indicating uninitialized state; advanced to
               KSTATE_READY by Initialize()).
        \note  In debug builds also verifies that TPlatform derives from IPlatform and TStrategy
               from ITaskSwitchStrategy.
        \note  If TMode includes KERNEL_TICKLESS, a compile-time assertion fires unless
               STK_TICKLESS_IDLE is defined to 1 in stk_config.h.
    */
    explicit Kernel() : m_platform(), m_strategy(), m_task_now(nullptr), m_task_storage(), m_sleep_trap(),
        m_exit_trap(), m_fsm_state(FSM_STATE_NONE), m_kstate(KSTATE_INACTIVE)
    {
    #ifdef _DEBUG
        // TPlatform must inherit IPlatform
        IPlatform *platform = &m_platform;
        STK_UNUSED(platform);

        // TStrategy must inherit ITaskSwitchStrategy
        ITaskSwitchStrategy *strategy = &m_strategy;
        STK_UNUSED(strategy);
    #endif

    #if !STK_TICKLESS_IDLE
        STK_STATIC_ASSERT_DESC(((TMode & KERNEL_TICKLESS) == 0U),
            "STK_TICKLESS_IDLE must be defined to 1 for KERNEL_TICKLESS");
    #endif
    }

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~Kernel() = default;

    /*! \brief     Initialize kernel.
        \param[in] resolution_us: Resolution of the system tick (SysTick) timer in microseconds.
                   Defaults to PERIODICITY_DEFAULT (1000 µs = 1 ms).
        \note      Must be called before AddTask() and Start().
        \note      If running on an STM32 device with HAL driver or on QEMU, do not change the default
                   resolution (PERIODICITY_DEFAULT). STM32's HAL expects 1 millisecond resolution and
                   QEMU does not have enough resolution on Windows to operate correctly at sub-millisecond resolution.
        \note      Kernel must be in \a KSTATE_INACTIVE state.
    */
    __stk_attr_noinline void Initialize(uint32_t resolution_us = PERIODICITY_DEFAULT) override
    {
        STK_ASSERT(resolution_us != 0);
        STK_ASSERT(resolution_us <= PERIODICITY_MAX);
        STK_ASSERT(!IsInitialized());

        // reinitialize key state variables
        m_task_now  = nullptr;
        m_fsm_state = FSM_STATE_NONE;
        
        // exit trap is required only for KERNEL_DYNAMIC mode
        Stack *exit_trap;
        if __stk_constexpr_cpp17 (IsDynamicMode())
        {
            exit_trap = &m_exit_trap[0].stack;
        }
        else
        {
            exit_trap = nullptr;
        }

        m_service.Initialize(this);
        m_platform.Initialize(this, &m_service, resolution_us, exit_trap);

        // now ready to Start()
        m_kstate = KSTATE_READY;
    }

    /*! \brief     Register task for a soft real-time (SRT) scheduling.
        \param[in] user_task: User task to add. Must not already be registered. Must not be \c nullptr.
        \return    See EAddResult. The kernel is not changed if the result is not ADD_OK.
        \note      Before Start(): allocates a free KernelTask slot and adds it to the strategy immediately.
        \note      After Start() (KERNEL_DYNAMIC only): the task is added immediately, inside a critical
                   section, and is visible to the scheduler on the next tick. The call does not block, does
                   not yield and does not defer any work to the tick handler.
        \note      ISR-safe (task or ISR context), with these restrictions:
                   - the whole operation runs with interrupts masked (stack initialization, MPU setup and the
                     ITask getters are executed inside the critical section), keep stacks of tasks added from
                     an ISR modest and call it from a low-priority ISR;
                   - the ISR priority must be numerically above STK_CORTEX_M_SVCALL_ISR_PRIORITY (lower urgency)
                     on Cortex-M, otherwise it is not masked by the BASEPRI critical section of an unprivileged task;
                   - in KERNEL_TICKLESS mode a task added from an ISR while all tasks are sleeping becomes runnable
                     only when the already armed timer expires (not one tick later), a task-context call is not
                     affected;
                   - kernels are expected to be per-core: the critical section excludes the tick of the local core only.
        \warning   KERNEL_DYNAMIC: a failure is only returned, never asserted, the caller must handle it.
                   Other modes: a failure asserts (KERNEL_HRT: always, use the HRT overload instead; KERNEL_STATIC:
                   after Start(), TASKS_MAX exceeded, task or stack already registered). In a release build such
                   a failure is not reported and the task is not added.
        \warning   The task and stack collision check is always performed, also in a release build.
    */
    __stk_attr_noinline EAddResult AddTask(ITask *user_task) override
    {
        EAddResult result = ADD_FAIL_INVALID;

        if __stk_constexpr_cpp17 (!IsHrtMode())
        {
            if (user_task != nullptr)
            {
                // critical section: avoid race with OnTick (and with a scheduler exit/restart when called from an ISR),
                // the kernel state is checked inside of it
                const hw::CriticalSection::ScopedLock cs_;

                // after Start() only KERNEL_DYNAMIC may add tasks
                if (IsInitialized() && (IsDynamicMode() || !IsStarted()))
                {
                    KernelTask *const task = TryAllocateNewTask(user_task, result, true);
                    if (task != nullptr)
                    {
                        AddKernelTask(task);
                    }
                }
            }
        }

        // KERNEL_DYNAMIC: the result is for the caller to handle (a task can be added at runtime and a failure
        // is a legit condition, e.g. the pool is full), otherwise this is an application design failure
        if __stk_constexpr_cpp17 (IsStaticMode() || IsHrtMode())
        {
            STK_ASSERT(result == ADD_OK);
        }

        return result;
    }

    /*! \brief     Register a task for hard real-time (HRT) scheduling.
        \param[in] user_task:  User task to add. Must not already be registered. Must not be \c nullptr.
        \param[in] periodicity_tc: Activation period in ticks. Must be > 0 and < INT32_MAX.
        \param[in] deadline_tc: Maximum allowed active duration in ticks. Must be > 0 and < INT32_MAX.
        \param[in] start_delay_tc: Initial sleep delay in ticks before the first activation. 0 means activate immediately.
        \warning   Tasks must be added before the scheduler is started (before Start()). Adding an HRT task
                   after Start() is not supported, not even from an ISR: the HRT schedule is built from the
                   complete task set and is not recomputed at runtime.
        \warning   Asserts if called outside KERNEL_HRT mode (use the SRT overload instead) or after Start().
                   The failure is not reported in a release build and the task is not added.
    */
    __stk_attr_noinline void AddTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc,
        Timeout start_delay_tc) override
    {
        if __stk_constexpr_cpp17 (IsHrtMode())
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

    /*! \brief     Remove a previously added task from the kernel when it is not started.
        \param[in] user_task: User task to remove. Must not be \c nullptr.
        \note      Only valid before Start() (i.e. while the kernel is not running).
                   To remove tasks after Start() the task should return from its Run function
                   (in KERNEL_DYNAMIC mode the slot is freed automatically on the next tick).
        \warning   KERNEL_DYNAMIC mode only. Asserts if called in KERNEL_STATIC or KERNEL_HRT mode,
                   or if called after Start().
    */
    __stk_attr_noinline void RemoveTask(ITask *user_task) override
    {
        if __stk_constexpr_cpp17 (IsDynamicMode())
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(!IsStarted());

            KernelTask *const task = FindTaskByUserTask(user_task);
            if (task != nullptr)
            {
                RemoveTask(task);
            }
        }
        else
        {
            // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to be removed
            STK_ASSERT(false);
        }
    }

    /*! \brief     Schedule task removal from scheduling (exit).
        \param[in] user_task: User task to remove. Must not be \c nullptr.
        \warning   KERNEL_DYNAMIC mode only. Asserts if called in KERNEL_STATIC or KERNEL_HRT mode,
                   or if called after Start().
    */
    __stk_attr_noinline void ScheduleTaskRemoval(ITask *user_task) override
    {
        if __stk_constexpr_cpp17 (IsDynamicMode())
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(IsStarted());

            // critical section: avoid race with OnTick

            const hw::CriticalSection::ScopedLock cs_;

            KernelTask *const task = FindTaskByUserTask(user_task);
            if (task != nullptr)
            {
                task->ScheduleRemoval(this);
            }
        }
        else
        {
            // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to be removed
            STK_ASSERT(false);
        }
    }

    using IKernel::SuspendTask; // keep the legacy SuspendTask(ITask *, bool &) overload visible

    /*! \brief      Suspend task.
        \param[in]  user_task: Pointer to the user task to suspend.
        \return     SUSPEND_DONE if the task was runnable and is suspended now; SUSPEND_DEFERRED if the
                    task is sleeping or blocked on a sync object (the suspension is applied when the
                    sleep/wait ends, the wait result is preserved); SUSPEND_ALREADY if the task is already
                    suspended or has a deferred suspension; SUSPEND_FAIL if the task is unknown or exiting.
        \note       ISR-safe. From an ISR the interrupted task is switched out by the next tick:
                    a forced context switch (SVC) is not legal in handler mode and is not requested.
        \note       hw::CriticalSection must not be active otherwise a deadlock will
                    happen if task is suspending self.
    */
    ESuspendResult SuspendTask(ITask *user_task) override
    {
        STK_ASSERT(user_task != nullptr);

        ESuspendResult result = SUSPEND_FAIL;
        KernelTask *self_task = nullptr;

        const bool in_isr = hw::IsInsideISR();

        // critical section: avoid race with OnTick
        {
            const hw::CriticalSection::ScopedLock cs_;

            KernelTask *const task = FindTaskByUserTask(user_task);

            if ((task != nullptr) && !task->IsPendingRemoval())
            {
                if ((task->m_state & (KernelTask::STATE_SUSPENDED | KernelTask::STATE_SUSPEND_PENDING)) != 0U)
                {
                    // nesting not supported
                    result = SUSPEND_ALREADY;
                }
                else if (task->IsSleeping())
                {
                    // sleeping or blocked on a sync object: do not overwrite m_time_sleep (that would
                    // corrupt the original sleep state and interfere with WaitObject::Tick()), only
                    // record the request: UpdateTaskState() turns it into a real suspension when the
                    // sleep/wait ends, with the wait result preserved
                    task->m_state |= KernelTask::STATE_SUSPEND_PENDING;
                    result = SUSPEND_DEFERRED;
                }
                else
                {
                    task->ScheduleSleep(WAIT_INFINITE);

                    // set state
                    task->m_state |= KernelTask::STATE_SUSPENDED;
                    result = SUSPEND_DONE;

                    if (task == m_task_now)
                    {
                        // trace blocked via Suspend
                    #if STK_SEGGER_SYSVIEW
                        SEGGER_SYSVIEW_OnTaskStopReady(task->GetUserStackPtr()->tid, TRACE_EVENT_SUSPEND);
                    #endif

                        // suspending self (task context): keep the pointer as m_task_now may change as soon
                        // as the critical section is released; from an ISR m_task_now is the interrupted
                        // task, it is switched out by the next tick and nobody must spin here
                        if (!in_isr)
                        {
                            self_task = task;
                        }
                    }
                    else if (IsStarted() && ((task->m_state & KernelTask::STATE_SLEEP_PENDING) != 0U))
                    {
                        // not the scheduled task: report the sleep to the strategy right away, otherwise a
                        // forced context switch could still pick it from GetNext() (see ScheduleRemoval)
                        ProcessTaskPendingSleep(task);
                    }
                    else
                    {
                        // noop
                    }
                }
            }
        }

        // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
        if (self_task != nullptr)
        {
            self_task->BusyWaitWhileSleeping(this);
        }

        return result;
    }

    /*! \brief     Resume task.
        \param[in] user_task: Pointer to the user task to resume.
        \return    RESUME_DONE if a suspended task was resumed; RESUME_LIFTED if a deferred suspension was
                   cancelled before it took effect; RESUME_NOT_SUSPENDED if the task was not suspended;
                   RESUME_FAIL if the task is unknown or exiting.
        \note      ISR-safe.
        \note      A task blocked in OnTaskWait() on a sync object is not woken by this call unless it is
                   suspended: use CancelTaskWait() to interrupt such a wait.
    */
    EResumeResult ResumeTask(ITask *user_task) override
    {
        STK_ASSERT(user_task != nullptr);

        EResumeResult result = RESUME_FAIL;

        // critical section: avoid race with OnTick

        const hw::CriticalSection::ScopedLock cs_;

        KernelTask *const task = FindTaskByUserTask(user_task);

        if ((task != nullptr) && !task->IsPendingRemoval())
        {
            if ((task->m_state & KernelTask::STATE_SUSPENDED) != 0U)
            {
                // clear state
                task->m_state &= ~KernelTask::STATE_SUSPENDED;

                // wake up
                if (task->IsSleeping())
                {
                    task->Wake();
                }

                result = RESUME_DONE;
            }
            else if ((task->m_state & KernelTask::STATE_SUSPEND_PENDING) != 0U)
            {
                // suspension was not applied yet: simply withdraw it
                task->m_state &= ~KernelTask::STATE_SUSPEND_PENDING;
                result = RESUME_LIFTED;
            }
            else
            {
                result = RESUME_NOT_SUSPENDED;
            }
        }

        return result;
    }

    /*! \brief     Cancel task's in-progress wait on a synchronization object.
        \param[in] user_task: Pointer to the user task whose wait should be cancelled.
    */
    void CancelTaskWait(ITask *user_task) override
    {
        STK_ASSERT(user_task != nullptr);

        KernelTask *const task = FindTaskByUserTask(user_task);

        // a suspended task must stay suspended: waking it would silently break SuspendTask()
        // (a deferred suspension, STATE_SUSPEND_PENDING, does not block the cancellation: the wait
        // ends with WAIT_RESULT_CANCELED and the task is parked afterwards by UpdateTaskState())
        if (task != nullptr)
        {
            // critical section: avoid race with OnTick

            const hw::CriticalSection::ScopedLock cs_;

            bool canceled_wait = false;

            if __stk_constexpr_cpp17 (IsSyncMode())
            {
                // a task blocked in OnTaskWait() also counts as sleeping (ScheduleSleep(WAIT_INFINITE)), but
                // its WaitObject is still linked into the sync object's wait list: waking only the task
                // would leave a stale entry behind (later signaled or ticked as a timeout while the task is
                // not waiting), so cancel the wait itself, this unlinks it, records WAIT_RESULT_CANCELED
                // and wakes the task
                canceled_wait = task->m_wait_obj[0].CancelIfWaiting();
            }

            if (!canceled_wait && task->IsSleeping() &&
                ((task->m_state & KernelTask::STATE_SUSPENDED) == 0U))
            {
                task->Wake();
            }
        }
    }

    /*! \brief      Get a snapshot of the kernel-side state of a task.
        \param[in]  user_task: Pointer to the user task to query. Must not be \c nullptr.
        \param[out] info: Receives the snapshot. Written only if the function returns \c true.
        \return     \c true on success, \c false if \a user_task is \c nullptr or the kernel is built without
                    KERNEL_TELEMETRY (\a info is left unchanged). See IKernel::GetTaskState().
        \note       The order of the checks matters: SuspendTask() and OnTaskWait() both put the task to an
                    infinite sleep (ScheduleSleep), so IsSleeping() is also true for suspended and blocked tasks.
        \note       KERNEL_TELEMETRY mode only.
    */
    bool GetTaskState(ITask *user_task, TaskStateInfo &info) const override
    {
        bool result;

        if (IsTelemetryMode())
        {
            if (user_task != nullptr)
            {
                info.state     = TASK_STATE_REMOVED;
                info.wait_obj  = nullptr;
                info.remaining = WAIT_INFINITE;

                // critical section: consistent view against OnTick
                const hw::CriticalSection::ScopedLock cs_;

                const KernelTask *const task = FindTaskByUserTask(user_task);
                if (task != nullptr)
                {
                    bool resolved = false;

                    if ((task->m_state & KernelTask::STATE_REMOVE_PENDING) != 0U)
                    {
                        info.state = TASK_STATE_EXITING;
                        resolved   = true;
                    }
                    else if ((task->m_state & KernelTask::STATE_SUSPENDED) != 0U)
                    {
                        info.state = TASK_STATE_SUSPENDED;
                        resolved   = true;
                    }
                    else
                    {
                        if __stk_constexpr_cpp17 (IsSyncMode())
                        {
                            const typename KernelTask::WaitObject &wait_obj = task->m_wait_obj[0];

                            if (wait_obj.IsWaiting())
                            {
                                info.state     = TASK_STATE_WAITING;
                                info.wait_obj  = wait_obj.m_sync_obj;
                                info.remaining = wait_obj.m_time_wait;
                                resolved       = true;
                            }
                        }
                    }

                    if (!resolved)
                    {
                        const Timeout time_sleep = task->m_time_sleep;

                        if (time_sleep < 0)
                        {
                            info.state     = TASK_STATE_SLEEPING;
                            info.remaining = -time_sleep; // -(-WAIT_INFINITE) == WAIT_INFINITE for an infinite sleep
                        }
                        else
                        {
                            info.state = TASK_STATE_READY;
                        }
                    }
                }

                result = true;
            }
            else
            {
                result = false;
            }
        }
        else
        {
            result = false;
        }

        return result;
    }

    /*! \brief     Enumerate kernel tasks.
        \param[in] tasks: Reference to the ArrayView of IKernelTask pointers.
        \return    Number of tasks in the array.
    */
   size_t EnumerateKernelTasks(ArrayView<IKernelTask *> tasks) override
   {
       size_t count = 0U;
       const size_t limit = Min(tasks.GetSize(), TASKS_MAX);

       // critical section: avoid race with OnTick
       {
           const hw::CriticalSection::ScopedLock cs_;

           for (size_t i = 0U; i < limit; ++i)
           {
               KernelTask &task = m_task_storage[i];
               if (task.IsBusy())
               {
                    tasks[count++] = &task;
               }
           }
       }

       return count;
   }
   
    /*! \brief     Enumerate user tasks.
        \param[in] user_tasks: Reference to the ArrayView of ITask pointers.
        \return    Number of tasks in the array.
    */
    size_t EnumerateTasks(ArrayView<ITask *> user_tasks) override
    {
        size_t count = 0U;
        const size_t limit = Min(user_tasks.GetSize(), TASKS_MAX);

        // critical section: avoid race with OnTick
        {
            const hw::CriticalSection::ScopedLock cs_;

            for (size_t i = 0U; i < limit; ++i)
            {
                KernelTask &task = m_task_storage[i];
                if (task.IsBusy())
                {
                    user_tasks[count++] = task.GetUserTask();
                }
            }
        }

        return count;
    }

    /*! \brief     Start the scheduler. This call does not return until all tasks have exited
                   (KERNEL_DYNAMIC mode) or indefinitely (KERNEL_STATIC mode).
        \note      Re-initializes trap stacks on every call so Start() can be called again
                   after a previous scheduling session ended.
        \note      If STK_SEGGER_SYSVIEW is enabled, starts tracing and registers all pre-added tasks.
        \warning   At least one task must have been added via AddTask() before calling Start().
                   Asserts if called before Initialize().
    */
    __stk_attr_noinline void Start() override
    {
        STK_ASSERT(IsInitialized());

         // stacks of the traps must be re-initilized on every subsequent Start
        InitTraps();

        // start tracing
    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_Start();
        for (size_t i = 0U; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->IsBusy())
            {
                SendTaskTraceInfo(task);
            }
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
    IPlatform *GetPlatform() override { return &m_platform; }

    /*! \brief  Get task-switching strategy instance owned by this kernel.
        \return Pointer to the internal TStrategy cast to ITaskSwitchStrategy*.
    */
    ITaskSwitchStrategy *GetSwitchStrategy() override { return &m_strategy; }

    /*! \brief  Get kernel state.
    */
    EKernelState GetState() const override { return m_kstate; }

    // Used by tests only, not Public API:
#ifdef _STK_UNDER_TEST
    void ScheduleSleepOnActiveTask(Timeout ticks)
    {
        m_task_now->ScheduleSleep(ticks);
    }
#endif

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

    /*! \brief Ticks to yield.

         Yield with 2 ticks: 1 will be incremented on the next OnTick call by UpdateTasks
         and remaining 1 will cause a context switch by UpdateFsmState when strategy detects
         it as a sleeping test.
    */
    static constexpr Timeout YIELD_TICKS = 1;

    /*! \brief     Check if FSM state is valid.
    */
    static __stk_forceinline bool IsValidFsmState(EFsmState state)
    {
        return ((state > FSM_STATE_NONE) && (state < FSM_STATE_MAX));
    }

    /*! \brief     Initialize stack of the traps.
    */
    __stk_attr_noinline void InitTraps()
    {
        // init stack for a Sleep trap
        {
            SleepTrapStack &sleep = m_sleep_trap[0];

            SleepTrapStackMemory wrapper(&sleep.memory);
            sleep.stack.access_mode = ACCESS_PRIVILEGED;
        #if STK_STACK_NEEDS_TASK_ID
            sleep.stack.tid  = SYS_TASK_ID_SLEEP;
        #endif

            STK_UNUSED(m_platform.InitStack(STACK_SLEEP_TRAP, &sleep.stack, &wrapper, nullptr));
        }

        // init stack for an Exit trap
        if __stk_constexpr_cpp17 (IsDynamicMode())
        {
            ExitTrapStack &exit = m_exit_trap[0];

            ExitTrapStackMemory wrapper(&exit.memory);
            exit.stack.access_mode = ACCESS_PRIVILEGED;
        #if STK_STACK_NEEDS_TASK_ID
            exit.stack.tid  = SYS_TASK_ID_EXIT;
        #endif

            STK_UNUSED(m_platform.InitStack(STACK_EXIT_TRAP, &exit.stack, &wrapper, nullptr));
        }
    }

    /*! \brief     Allocate new instance of KernelTask and bind the user task to it.
        \param[in]  user_task: User task for which kernel task object is allocated.
        \param[out] result: ADD_OK, ADD_FAIL_DUPLICATE or ADD_FAIL_POOL_FULL.
        \param[in]  validate: If \c false the search stops at the first free slot and the collision
                    with the already added tasks is not checked (assertions are inactive).
        \return     Bound kernel task, or \c nullptr (the kernel is not changed) if \a result is not ADD_OK.
        \note       Must be called inside a critical section when the kernel is started.
    */
    KernelTask *TryAllocateNewTask(ITask *user_task, EAddResult &result, const bool validate)
    {
        KernelTask *new_task = nullptr;
        bool collision = false;

        // look for a free kernel task
        for (size_t i = 0U; i < TASKS_MAX; ++i)
        {
            KernelTask *const task = &m_task_storage[i];
            if (task->IsBusy())
            {
                // avoid task collision and stack collision
                collision = collision || (task->m_user == user_task) ||
                    (task->m_user->GetStack() == user_task->GetStack());
            }
            else
            if (new_task == nullptr)
            {
                new_task = task;
                if (!validate)
                {
                    break;
                }
            }
            else
            {
                // noop, continue to the next slot
            }
        }

        if (collision)
        {
            result   = ADD_FAIL_DUPLICATE;
            new_task = nullptr;
        }
        else
        if (new_task == nullptr)
        {
            result = ADD_FAIL_POOL_FULL;
        }
        else
        {
            result = ADD_OK;
            new_task->Bind(&m_platform, user_task);
        }

        return new_task;
    }

    /*! \brief     Allocate new instance of KernelTask, asserts on failure.
        \param[in] user_task: User task for which kernel task object is allocated.
        \return    Kernel task.
    */
    KernelTask *AllocateNewTask(ITask *user_task)
    {
        // if assertions are inactive do not try to validate collision with existing tasks
    #if defined(NDEBUG) && !defined(_STK_ASSERT_REDIRECT)
        const bool validate = false;
    #else
        const bool validate = true;
    #endif

        EAddResult result;
        KernelTask *const new_task = TryAllocateNewTask(user_task, result, validate);

        // task or stack collision: application design failure
        STK_ASSERT(result != ADD_FAIL_DUPLICATE);

        // if nullptr - exceeded max supported kernel task count, application design failure
        STK_ASSERT(new_task != nullptr);

        return new_task;
    }

    /*! \brief     Add kernel task to the scheduling strategy.
        \param[in] task: Pointer to the kernel task.
    */
    void AddKernelTask(KernelTask *task)
    {
    #if STK_SEGGER_SYSVIEW
        // start tracing new task
        SEGGER_SYSVIEW_OnTaskCreate(task->GetUserStackPtr()->tid);
        if (IsStarted())
        {
            SendTaskTraceInfo(task);
        }
    #endif

        m_strategy.AddTask(task);
    }

    /*! \brief     Allocate new instance of KernelTask and add it into the HRT scheduling process.
        \note      Related to stk::KERNEL_HRT mode only.
        \param[in] user_task: User task for which kernel task object is allocated.
        \param[in] periodicity_tc: Periodicity time at which task is scheduled (ticks).
        \param[in] deadline_tc: Deadline time within which a task must complete its work (ticks).
        \param[in] start_delay_tc: Initial start delay for the task (ticks).
    */
    void HrtAllocateAndAddNewTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc)
    {
        KernelTask *const task = AllocateNewTask(user_task);
        STK_ASSERT(task != nullptr);

        task->HrtInit(periodicity_tc, deadline_tc, start_delay_tc);

        AddKernelTask(task);
    }

    template <typename TaskPtr, typename Storage>
    static inline TaskPtr FindTaskByUserTaskImpl(const ITask *user_task, Storage &task_storage)
    {
        TaskPtr found_task = nullptr;

        for (size_t i = 0U; i < TASKS_MAX; ++i)
        {
            TaskPtr const task = &task_storage[i];

            if (task->GetUserTask() == user_task)
            {
                found_task = task;
                break;
            }
        }

        return found_task;
    }

    /*! \brief     Find kernel task by the bound ITask instance (Non-const version).
        \param[in] user_task: User task.
        \return    Kernel task (mutable).
    */
    __stk_attr_noinline KernelTask *FindTaskByUserTask(const ITask *user_task)
    {
        return FindTaskByUserTaskImpl<KernelTask *>(user_task, m_task_storage);
    }

    /*! \brief     Find kernel task by the bound ITask instance (Const version).
        \param[in] user_task: User task.
        \return    Kernel task (const).
    */
    __stk_attr_noinline const KernelTask *FindTaskByUserTask(const ITask *user_task) const
    {
        return FindTaskByUserTaskImpl<const KernelTask *>(user_task, m_task_storage);
    }

    /*! \brief     Find kernel task by the bound Stack instance.
        \param[in] stack: Stack.
        \return    Kernel task.
    */
    KernelTask *FindTaskByStack(const Stack *stack)
    {
        KernelTask *found_task = nullptr;
      
        for (size_t i = 0U; i < TASKS_MAX; ++i)
        {
            KernelTask *const task = &m_task_storage[i];
            if (task->GetUserStackPtr() == stack)
            {
                found_task = task;
                break;
            }
        }

        return found_task;
    }

    /*! \brief     Find kernel task for a Stack Pointer (SP).
        \param[in] SP: Stack pointer.
        \return    Kernel task.
    */   
    __stk_attr_noinline KernelTask *FindTaskBySP(Word SP)
    {
        STK_ASSERT(m_task_now != nullptr);
        
        KernelTask *found_task = nullptr;

        if (m_task_now->IsMemoryOfSP(SP))
        {
            found_task = m_task_now;
        }
        else
        {
            for (size_t i = 0U; i < TASKS_MAX; ++i)
            {
                KernelTask *const task = &m_task_storage[i];

                // skip finished tasks (applicable only for KERNEL_DYNAMIC mode)
                if __stk_constexpr_cpp17 (IsDynamicMode())
                {
                    if (!task->IsBusy())
                    {
                        continue;
                    }
                }

                if (task->IsMemoryOfSP(SP))
                {
                    found_task = task;
                    break;
                }
            }
        }

        return found_task;
    }

    /*! \brief     Remove kernel task.
        \note      Removal of the kernel task means releasing it from the user task details.
        \param[in] task: Kernel task.
    */
    void RemoveTask(KernelTask *task)
    {
        STK_ASSERT(task != nullptr);

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskTerminate(task->GetUserStackPtr()->tid);
    #endif

        // notify task about pending exit
        task->GetUserTask()->OnExit();

        m_strategy.RemoveTask(task);
        task->Unbind();
    }

    /*! \brief      Called by platform driver immediately after a scheduler start (first tick).
        \param[out] active: Set to the stack of the first task to run, or to the sleep-trap stack if
                    all tasks are initially sleeping.
        \note       Delivers initial OnTaskSleep notifications to sleep-aware strategies for any tasks
                    that were added in a sleeping state before Start() was called.
        \note       Selects the first runnable task via GetNewFsmState() and transitions the kernel
                    to KSTATE_RUNNING.
        \note       If STK_SEGGER_SYSVIEW is enabled, emits a task-start trace event for the first task.
        \warning    At least one task must have been added via AddTask(); asserts if the strategy pool is empty.
    */
    __stk_attr_noinline void OnStart(Stack *&active) override
    {
        STK_ASSERT(m_strategy.GetSize() != 0);

        // iterate tasks and generate OnTaskSleep for a strategy for all initially sleeping tasks
        for (size_t i = 0U; i < TASKS_MAX; ++i)
        {
            KernelTask *const task = &m_task_storage[i];

            if ((task->m_state & KernelTask::STATE_SLEEP_PENDING) != 0U)
            {
                STK_ASSERT(task->IsSleeping());

                task->m_state &= ~KernelTask::STATE_SLEEP_PENDING;

                // notify strategy that task is sleeping
                if __stk_constexpr_cpp17 (TStrategy::SLEEP_EVENT_API)
                {
                    m_strategy.OnTaskSleep(task);
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
                active     = next->GetUserStackPtr();

                if __stk_constexpr_cpp17 (IsHrtMode())
                {
                    next->HrtOnSwitchedIn();
                }
            }
            else
            if (m_fsm_state == FSM_STATE_SLEEPING)
            {
                m_task_now = util::DListCast::ListEntryToParent<KernelTask>(m_strategy.GetFirst());
                active     = &m_sleep_trap[0].stack;
            }
            else
            {
                // unexpected state
                STK_KERNEL_PANIC(KERNEL_PANIC_BAD_STATE);
            }
        }

        // is in running state
        m_kstate = KSTATE_RUNNING;
    }

    /*! \brief  Called by the platform driver after a scheduler stop (all tasks have exited).
        \note   KERNEL_DYNAMIC mode only: resets FSM to FSM_STATE_NONE and transitions
                kernel back to KSTATE_READY so Start() may be called again.
        \note   Has no effect in KERNEL_STATIC mode (static kernels never stop).
    */
    __stk_attr_noinline void OnStop() override
    {
        if __stk_constexpr_cpp17 (IsDynamicMode())
        {
            m_fsm_state = FSM_STATE_NONE;

            // is in stopped state, i.e. is ready to Start() again
            m_kstate = KSTATE_READY;
        }
    }

    /*! \brief      Process one scheduler tick. Called from the platform timer/tick ISR.
        \param[out] idle: Stack descriptor to context-switch out (\c nullptr if no switch needed).
        \param[out] active: Stack descriptor to context-switch in (\c nullptr if no switch needed).
        \param[in,out] ticks: (KERNEL_TICKLESS builds only) On entry: actual number of ticks elapsed
                    since the last call, as measured by the platform driver. On return:
                    the number of ticks the hardware timer may suppress before the next
                    required wakeup, computed as the minimum remaining sleep across all
                    active tasks, clamped to [1, STK_TICKLESS_TICKS_MAX]. The platform
                    driver programs this value into the timer to avoid unnecessary wakeups.
                    This parameter is absent in non-tickless builds.
        \return     \c true if a context switch is required (\a idle and \a active are valid);
                    \c false if the current task continues running.
        \note       In non-tickless mode the internal tick counter always advances by exactly 1 per call.
        \note       In tickless mode (KERNEL_TICKLESS) the counter advances by the \a ticks value
                    supplied by the platform driver, which may be greater than 1 after a suppressed interval.
    */
    bool OnTick(Stack *&idle, Stack *&active
    #if STK_TICKLESS_IDLE
        , Timeout &ticks
    #endif
    ) override
    {
    #if !STK_TICKLESS_IDLE
        // in non-tickless mode kernel is advancing strictly by 1 tick on every OnTick call
        enum { ticks = 1 };
    #endif

        // elapsed ticks must be captured here: in tickless mode "ticks" is overwritten below
        const Timeout elapsed_ticks = ticks;

        // advance internal timestamp
        m_service.IncrementTicks(elapsed_ticks);

        // consume elapsed and update to ticks to sleep
    #if STK_TICKLESS_IDLE
        ticks = (
    #else
        // notify compiler that we ignore a return value of UpdateTasks
        STK_UNUSED(
    #endif
        UpdateTasks(elapsed_ticks));

        // time-slice accounting for the task that just ran (after sleep/wake updates, before the FSM picks next)
        if __stk_constexpr_cpp17 (IsTimesliceMode())
        {
            UpdateTimeslice(elapsed_ticks);
        }

        // decide on a context switch
        return UpdateFsmState(idle, active);
    }

    bool OnForceContextSwitch(TId id, Stack *&idle, Stack *&active)
    {
        bool switch_context = false;
        KernelTask *const task = m_task_now;

        STK_ASSERT(task != nullptr);

        // note: called from inside ISR, therefore protection by critical section is not needed

        if (task->GetTid() == id)
        {
            // current task is busy-waiting to be de-scheduled
            if (task->IsSleeping())
            {
                if ((task->m_state & KernelTask::STATE_SLEEP_PENDING) != 0U)
                {
                    ProcessTaskPendingSleep(task);

                    switch_context = UpdateFsmState(idle, active);
                }
            }
            else
            {
                if __stk_constexpr_cpp17 (TStrategy::NOSLEEP_YIELD_API)
                {
                    // current task yielded and stays runnable: let the strategy pick the next task
                    if ((task->m_state & KernelTask::STATE_YIELD_PENDING) != 0U)
                    {
                        ProcessTaskPendingYield(task);

                        switch_context = UpdateFsmState(idle, active);
                    }
                }
            }
        }

        return switch_context;
    }

    void OnTaskSwitch(Word caller_SP) override
    {
        // let the strategy handle the yield without putting the task to sleep (e.g. to hand
        // the CPU to same-priority peers only), not applicable in HRT mode where Yield()
        // means work completion
        if __stk_constexpr_cpp17 (TStrategy::NOSLEEP_YIELD_API && !IsHrtMode())
        {
            KernelTask *const task = FindTaskBySP(caller_SP);
            STK_ASSERT(task != nullptr);

            if (task != nullptr)
            {
                bool handled = false;

                // critical section: avoid race with OnTick
                {
                    const hw::CriticalSection::ScopedLock cs_;

                    if (!task->IsSleeping())
                    {
                        if (m_strategy.OnTaskYield(task))
                        {
                            task->ScheduleYield();
                            handled = true;
                        }
                    }
                }

                if (handled)
                {
                    task->BusyWaitWhileYielding(this);
                }
                else
                {
                    OnTaskSleep(caller_SP, YIELD_TICKS);
                }
            }
            else
            {
                // defensive: do nothing on null task
            }
        }
        else
        {
            OnTaskSleep(caller_SP, YIELD_TICKS);
        }
    }

    void OnTaskSleep(Word caller_SP, Timeout ticks) override
    {
        KernelTask *const task = FindTaskBySP(caller_SP);
        STK_ASSERT(task != nullptr);

        // // critical section: make change to HRT state and sleep time atomic
        {
            const hw::CriticalSection::ScopedLock cs_;

            if __stk_constexpr_cpp17 (IsHrtMode())
            {
                task->HrtOnWorkCompleted();
            }

            if (ticks > 0)
            {
                task->ScheduleSleep(ticks);

                // trace blocked on Sleep
            #if STK_SEGGER_SYSVIEW
                SEGGER_SYSVIEW_OnTaskStopReady(task->GetUserStackPtr()->tid, TRACE_EVENT_SLEEP);
            #endif
            }
        }

        // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
        task->BusyWaitWhileSleeping(this);
    }

    bool OnTaskSleepUntil(Word caller_SP, Ticks timestamp) override
    {
        KernelTask *const task = FindTaskBySP(caller_SP);
        STK_ASSERT(task != nullptr);

        bool result = true;

        // critical section: make change to HRT state and sleep time atomic
        {
            const hw::CriticalSection::ScopedLock cs_;

            // calculate signed delta (handles wrap-around correctly)
            const Ticks delta = timestamp - m_service.m_ticks;

            if (delta > 0)
            {
                const Ticks infinite_ticks = WAIT_INFINITE;              
                task->ScheduleSleep(static_cast<Timeout>(Min(delta, infinite_ticks)));

                // trace blocked on on SleepUntil
            #if STK_SEGGER_SYSVIEW
                SEGGER_SYSVIEW_OnTaskStopReady(task->GetUserStackPtr()->tid, TRACE_EVENT_SLEEP);
            #endif
            }
            else
            {
                result = false; // deadline already hit or passed
            }
        }

        // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
        if (result)
        {
            task->BusyWaitWhileSleeping(this);
        }

        return result;
    }

    void OnTaskExit(Stack *stack) override
    {
        if __stk_constexpr_cpp17 (IsDynamicMode())
        {
            KernelTask *const task = FindTaskByStack(stack);
            STK_ASSERT(task != nullptr);

            // task is being stopped and will terminate
        #if STK_SEGGER_SYSVIEW
            SEGGER_SYSVIEW_OnTaskStopExec();
        #endif

            // critical section: avoid race with OnTick
            {
                // make change to HRT/sleep/removal state atomic (same as OnTaskSleep/OnTaskSleepUntil)
                const hw::CriticalSection::ScopedLock cs_;

                // notify kernel to execute removal (asynchronous operation)
                task->ScheduleRemoval(this);
            }
        }
        else
        {
            // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to exit
            STK_KERNEL_PANIC(KERNEL_PANIC_BAD_MODE);
        }
    }

    EWaitResult OnTaskWait(Word caller_SP, ISyncObject *sync_obj, IMutex *mutex, Timeout timeout) override
    {
        if __stk_constexpr_cpp17 (IsSyncMode())
        {
            STK_ASSERT(timeout != 0);        // API contract: caller must not be in ISR
            STK_ASSERT(sync_obj != nullptr); // API contract: ISyncObject instance must be provided
            STK_ASSERT(mutex != nullptr);    // API contract: IMutex instance must be provided
            STK_ASSERT((sync_obj->GetHead() == nullptr) || (sync_obj->GetHead() == &m_sync_list[0]));

            KernelTask *const task = FindTaskBySP(caller_SP);
            STK_ASSERT(task != nullptr);

            typename KernelTask::WaitObject &wait_obj = task->m_wait_obj[0];

            // critical section: avoid race with OnTick
            {
                // keep the task from being de-scheduled between ScheduleSleep() and the end of mutex->Unlock():
                // otherwise a tick can switch it out (sleeping) while it still owns the mutex
                const hw::CriticalSection::ScopedLock cs_;

                // configure waiting
                wait_obj.SetupWait(sync_obj, timeout);

                // register ISyncObject if not yet
                if (sync_obj->GetHead() == nullptr)
                {
                    m_sync_list->LinkBack(sync_obj);
                }

                // start sleeping infinitely, we rely on a Wake call via WaitObject
                task->ScheduleSleep(WAIT_INFINITE);

                // trace blocked on sync object's Wait
            #if STK_SEGGER_SYSVIEW
                SEGGER_SYSVIEW_OnTaskStopReady(task->GetUserStackPtr()->tid, TRACE_EVENT_WAIT);
            #endif

                // unlock mutex locked externally, so that we could wait in a busy-waiting loop
                mutex->Unlock();
            }

            // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
            task->BusyWaitWhileSleeping(this);

            // extract & clear this wait's result BEFORE mutex->Lock(): a contended Lock() recurses
            // into OnTaskWait() and reuses this same per-task WaitObject, which would otherwise
            // silently overwrite/clear our still-unread result
            const EWaitResult result = wait_obj.GetWaitResult();
            wait_obj.ClearState();

            // re-lock mutex when returning to the task's execution space
            mutex->Lock();

            return result;
        }
        else
        {
            STK_ASSERT(false);
            return WAIT_RESULT_FAIL;
        }
    }

    TId OnGetTid(Word caller_SP) override
    {
        KernelTask *const task = FindTaskBySP(caller_SP);
        STK_ASSERT(task != nullptr);

        return task->GetTid();
    }

    void OnSuspend(bool suspended) override
    {
        // toggle kernel state
        if (suspended)
        {
            if (m_kstate == KSTATE_RUNNING) 
            {
                m_kstate = KSTATE_SUSPENDED;
            }
        }
        else
        {
            if (m_kstate == KSTATE_SUSPENDED) 
            {
                m_kstate = KSTATE_RUNNING;
            }
        }

        // force yield for a currently active task
        if (!m_task_now->IsSleeping())
        {
            m_task_now->ScheduleSleep(YIELD_TICKS);
        }
    }

    void OnInheritWeight(TId tid, Weight weight)
    {
        STK_ASSERT(tid != TID_NONE);
        STK_ASSERT(TStrategy::WEIGHT_API && TStrategy::PRIORITY_INHERITANCE_API);

        if (weight != NO_WEIGHT)
        {
            KernelTask *const task = FindTaskByUserTask(GetUserTaskFromTid(tid));
            STK_ASSERT(task != nullptr);

            const Weight prev_weight = task->GetWeight();

            if (prev_weight < weight)
            {
                task->SetCurrentWeight(weight);
                m_strategy.OnTaskWeightChange(task, prev_weight);
            }
        }
    }

    Weight OnSetWeight(TId tid, Weight weight)
    {
        STK_ASSERT(tid != TID_NONE);
        STK_ASSERT(TStrategy::WEIGHT_API && TStrategy::PRIORITY_INHERITANCE_API);
        STK_ASSERT(weight != NO_WEIGHT);

        KernelTask *const task = FindTaskByUserTask(GetUserTaskFromTid(tid));
        STK_ASSERT(task != nullptr);

        const Weight prev_weight = task->GetWeight();     // effective weight (base or inherited)
        const Weight prev_base   = task->GetBaseWeight();

        task->SetBaseWeight(weight);

        // an inherited weight that does not exceed the new base weight is no longer a boost
        if ((task->GetCurrentWeight() != NO_WEIGHT) && (task->GetCurrentWeight() <= weight))
        {
            task->SetCurrentWeight(NO_WEIGHT);
        }

        // always notify: the strategy may depend on parts of the weight that do not change
        // the effective weight while a boost is active (e.g. preemption-threshold)
        m_strategy.OnTaskWeightChange(task, prev_weight);

        return prev_base;
    }

    Timeout OnSetTimeSlice(TId tid, Timeout ticks)
    {
        STK_ASSERT(tid != TID_NONE);
        STK_ASSERT(ticks >= NO_TIME_SLICE);

        KernelTask *const task = FindTaskByUserTask(GetUserTaskFromTid(tid));
        STK_ASSERT(task != nullptr);

        typename KernelTask::TsInfo &ts = task->m_ts[0];

        const Timeout prev = ts.quantum;

        ts.quantum = ticks;
        ts.Reload();

        return prev;
    }

    /*! \brief     Charge the running task and rotate it behind its same-priority peers when its slice expires.
        \note      KERNEL_TIMESLICE only. Called from OnTick() after UpdateTasks() and before UpdateFsmState().
        \param[in] elapsed_ticks: Ticks since the previous OnTick() (always 1 in non-tickless mode).
    */
    void UpdateTimeslice(const Timeout elapsed_ticks)
    {
        KernelTask *const task = m_task_now;

        // only a real, runnable task is charged: not the sleep trap, not a task which is leaving
        if ((task != nullptr) &&
            ((m_fsm_state == FSM_STATE_SWITCHING) || (m_fsm_state == FSM_STATE_WAKING)) &&
            task->IsBusy() && !task->IsSleeping())
        {
            typename KernelTask::TsInfo &ts = task->m_ts[0];

            if (ts.quantum != NO_TIME_SLICE)
            {
                ts.left -= elapsed_ticks;

                if (ts.left <= 0)
                {
                    ts.Reload();

                    // strategy rotates to a same-priority peer if there is one, otherwise (or if the
                    // task is not eligible) the task keeps running with a fresh slice
                    STK_UNUSED(m_strategy.OnTaskTimeSlice(task));
                }
            }
        }
    }

    void OnRestoreWeight(TId tid, ISyncObject *sobj)
    {
        STK_ASSERT(tid != TID_NONE);
        STK_ASSERT(TStrategy::WEIGHT_API && TStrategy::PRIORITY_INHERITANCE_API);

        KernelTask *const task = FindTaskByUserTask(GetUserTaskFromTid(tid));
        STK_ASSERT(task != nullptr);

        const Weight prev_weight = task->GetWeight();

        // restore to original or boost from wait objects
        task->SetCurrentWeight(sobj != nullptr ? sobj->FindWeightHigherThan(task->GetWeight()) : NO_WEIGHT);

        m_strategy.OnTaskWeightChange(task, prev_weight);
    }

    /*! \brief     Update tasks (sync objects, sleep).
    */
    Timeout UpdateTasks(const Timeout elapsed_ticks)
    {
        if __stk_constexpr_cpp17 (IsSyncMode())
        {
            UpdateSyncObjects(elapsed_ticks);
        }

        return UpdateTaskState(elapsed_ticks);
    }

    void ProcessTaskPendingSleep(KernelTask *const task)
    {
        task->m_state &= ~(KernelTask::STATE_SLEEP_PENDING | KernelTask::STATE_WAKE_PENDING);

        // notify strategy that task is sleeping
        if __stk_constexpr_cpp17 (TStrategy::SLEEP_EVENT_API)
        {
            m_strategy.OnTaskSleep(task);
        }
    }

    void ProcessTaskPendingYield(KernelTask *const task)
    {
        task->m_state &= ~KernelTask::STATE_YIELD_PENDING;
    }

        
    /*! \brief     Update task state: process removals, deliver sleep/wake notifications, advance
                   sleep timers, and track HRT durations.
        \param[in] elapsed_ticks: Number of ticks elapsed since the previous call.
                   Always 1 in non-tickless mode, may be >1 in tickless mode.
        \return    In non-tickless mode: always 1.
                   In tickless mode (KERNEL_TICKLESS): the minimum remaining sleep ticks across all
                   active tasks, clamped to [1, STK_TICKLESS_TICKS_MAX]. The platform driver uses
                   this value to program the next timer wakeup interval, suppressing timer/tick ISR for
                   that many ticks when the system would otherwise be idle.
        \note      Sleep entry/exit protocol: ScheduleSleep() sets STATE_SLEEP_PENDING and stores
                   m_time_sleep = -ticks. This function's first pass over a newly-sleeping task (the
                   "entry tick") consumes STATE_SLEEP_PENDING and delivers OnTaskSleep() to the
                   strategy, but does NOT also advance m_time_sleep on that same pass - doing both in
                   one call would let a 1-tick sleep collapse to zero elapsed time (OnTaskSleep and
                   OnTaskWake firing back-to-back before the task was ever excluded from scheduling).
                   The entry tick is therefore required to be a single, uncoalesced tick (asserted
                   below); this holds because a task can only enter STATE_SLEEP_PENDING by putting
                   itself to sleep, which means it was actively running (not idle) up to that point,
                   so tickless coalescing, which only suppresses ticks while the CPU is idle, cannot
                   have applied yet.
                   If Wake()/SleepCancel() races in before the entry tick runs, STATE_WAKE_PENDING is
                   set alongside STATE_SLEEP_PENDING. The entry tick then delivers OnTaskSleep()
                   followed immediately by the normal advance (and OnTaskWake(), since m_time_sleep
                   was already primed to -1 by Wake()) in the same pass, so the task still resumes on
                   the next tick as promised, with the two notifications correctly paired instead of
                   an orphaned OnTaskWake().
                   Tasks sleeping via WAIT_INFINITE (IsSleepInfinite()) are excluded from the per-tick
                   advance entirely so they never spuriously time out; only an explicit Wake() (which
                   directly sets m_time_sleep = -1) ends an infinite sleep.
    */
    Timeout UpdateTaskState(const Timeout elapsed_ticks)
    {
        Timeout sleep_ticks = GetInitialSleepTicks<IsTicklessMode()>();

        for (size_t i = 0U; i < TASKS_MAX; ++i)
        {
            KernelTask *const task = &m_task_storage[i];

            if __stk_constexpr_cpp17 (TStrategy::NOSLEEP_YIELD_API)
            {
                // a pending yield is consumed by this tick (the strategy already moved its cursor)
                if ((task->m_state & KernelTask::STATE_YIELD_PENDING) != 0U)
                {
                    task->m_state &= ~KernelTask::STATE_YIELD_PENDING;
                }
            }

            if (task->IsSleeping())
            {
                if __stk_constexpr_cpp17 (IsDynamicMode())
                {
                    // task is pending removal, wait until it is switched out
                    if (task->IsPendingRemoval())
                    {
                        const size_t tasks_left = m_strategy.GetSize();
                      
                        if ((task != m_task_now) ||
                            ((tasks_left == 1U) && (m_fsm_state == FSM_STATE_SLEEPING)))
                        {
                            RemoveTask(task);
                            continue;
                        }
                    }
                }

                bool just_entered_sleep = false;

                // note: only currently scheduled task can be pending to sleep
                if ((task->m_state & KernelTask::STATE_SLEEP_PENDING) != 0U)
                {
                    STK_ASSERT(elapsed_ticks == 1); // entry tick must be a single, uncoalesced tick

                    // if Wake() raced in before this entry tick ran, STATE_WAKE_PENDING is set:
                    // deliver OnTaskSleep() below as usual, then fall through to the advance block
                    // in this same pass instead of waiting an extra tick, so the paired OnTaskWake()
                    // still fires on schedule.
                    const bool wake_pending = ((task->m_state & KernelTask::STATE_WAKE_PENDING) != 0U);

                    ProcessTaskPendingSleep(task);

                    just_entered_sleep = !wake_pending;
                }

                if (!just_entered_sleep && !task->IsSleepInfinite())
                {
                    if (((task->m_state & KernelTask::STATE_SUSPEND_PENDING) != 0U) &&
                        ((task->m_time_sleep + elapsed_ticks) >= 0))
                    {
                        // sleep/wait is over (timeout, signal, cancel or Wake) but a deferred suspension is
                        // pending: park the task instead of waking it. The strategy has already removed the
                        // task on OnTaskSleep(), therefore OnTaskWake() is not delivered. The check precedes
                        // the advance so the task is never observed awake (m_time_sleep >= 0) in between.
                        task->CommitSuspend();
                    }
                    else
                    {
                        // advance sleep time by number of elapsed ticks (always 1 if non-Tickless)
                        task->m_time_sleep += elapsed_ticks;

                        // deliver sleep event to the strategy
                        if __stk_constexpr_cpp17 (TStrategy::SLEEP_EVENT_API)
                        {
                            // notify strategy that the task woke up
                            if (!task->IsSleeping())
                            {
                                m_strategy.OnTaskWake(task);
                            }
                        }
                    }
                }
            }
            else
            {
                if __stk_constexpr_cpp17 (IsHrtMode())
                {
                    // in HRT mode we trace how long task spent in active state (doing some work)
                    if (task->IsBusy())
                    {
                        typename KernelTask::HrtInfo &hrt = task->m_hrt[0];

                        hrt.duration += elapsed_ticks;

                        // check if deadline is missed (HRT failure)
                        if (task->HrtIsDeadlineMissed(hrt.duration))
                        {
                            // report deadline overrun to the strategy which supports overrun recovery
                            if __stk_constexpr_cpp17 (TStrategy::DEADLINE_MISSED_API)
                            {                                
                                if (!m_strategy.OnTaskDeadlineMissed(task))
                                {
                                    // report failure if it could not be recovered by the scheduling strategy
                                    task->HrtHardFailDeadline(&m_platform);
                                }
                            }
                            else
                            {
                                task->HrtHardFailDeadline(&m_platform);
                            }
                        }
                    }
                }
            }

            // get the number of ticks the driver has to keep CPU in Idle
            if __stk_constexpr_cpp17 (IsTicklessMode())
            {
                if ((sleep_ticks > 1) && task->IsBusy())
                {
                    sleep_ticks = task->GetSleepTicks(sleep_ticks);
                }
            }
        }

        return sleep_ticks;
    }

    /*! \brief     Update synchronization objects.
    */
    void UpdateSyncObjects(const Timeout elapsed_ticks)
    {
        ISyncObject::ListEntryType *itr = m_sync_list->GetFirst();

        while (itr != nullptr)
        {
            ISyncObject::ListEntryType *const next = itr->GetNext();

            if (!util::DListCast::ListEntryToParent<ISyncObject>(itr)->Tick(elapsed_ticks))
            {
                m_sync_list->Unlink(itr);
            }

            itr = next;
        }
    }

    /*! \brief      Fetch next event for the FSM.
        \param[out] next: Next kernel task to which Kernel can switch.
        \return     FSM event.
    */
    EFsmEvent FetchNextEvent(KernelTask *&next)
    {
        EFsmEvent type = FSM_EVENT_SLEEP;

        // try getting next task for scheduling
        next = util::DListCast::ListEntryToParent<KernelTask>(m_strategy.GetNext());

        // sleep-aware strategy returns nullptr if no active tasks available
        if (next != nullptr)
        {
            // strategy must provide active-only task
            STK_ASSERT(!next->IsSleeping());

            // if was sleeping, process wake event first
            type = (m_fsm_state == FSM_STATE_SLEEPING ? FSM_EVENT_WAKE : FSM_EVENT_SWITCH);
        }
        // start sleeping
        else
        {
            if __stk_constexpr_cpp17 (IsDynamicMode())
            {
                // if nullptr is returned then either strategy has all tasks sleeping or none left,
                // if KERNEL_DYNAMIC mode and no tasks left then exit from scheduling
                if (m_strategy.GetSize() == 0U)
                {
                    next = nullptr;
                    type = FSM_EVENT_EXIT;
                }
            }
        }

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
        KernelTask *const now = m_task_now, *next = nullptr;
        bool switch_context = false;

        const EFsmState new_state = GetNewFsmState(next);

        switch (new_state)
        {
        case FSM_STATE_SWITCHING:
            switch_context = StateSwitch(now, next, idle, active);
            m_fsm_state = new_state;
            break;
        case FSM_STATE_SLEEPING:
            switch_context = StateSleep(now, next, idle, active);
            m_fsm_state = new_state;
            break;
        case FSM_STATE_WAKING:
            switch_context = StateWake(now, next, idle, active);
            m_fsm_state = new_state;
            break;
        case FSM_STATE_EXITING:
            switch_context = StateExit(now, next, idle, active);
            m_fsm_state = new_state;
            break;
        case FSM_STATE_NONE:
            break; // valid intermittent non-persisting state: no-transition
        case FSM_STATE_MAX:
        default:   // invalid state value
            STK_KERNEL_PANIC(KERNEL_PANIC_BAD_STATE);
            break;
        }

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
        
        bool switch_context = false;

        // if equal: do not switch context because task did not change
        if (next != now)
        {
            idle   = now->GetUserStackPtr();
            active = next->GetUserStackPtr();

            // if stack memory is exceeded these assertions will be hit
        #if STK_STACK_GUARD
            if (now->IsBusy())
            {
                // current task could exit, thus we check it with IsBusy to avoid referencing nullptr returned by GetUserTask()
                STK_ASSERT(now->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);
            }
            STK_ASSERT(next->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);
        #endif

            m_task_now = next;

            if __stk_constexpr_cpp17 (IsHrtMode())
            {
                if (now->m_hrt[0].done)
                {
                    now->HrtOnSwitchedOut();
                    next->HrtOnSwitchedIn();
                }
            }

            switch_context = true;
        }

        return switch_context;
    }

    /*! \brief      Wakes up after sleeping.
        \note       FSM state: stk::FSM_STATE_WAKING.
        \param[in]  now: Currently active kernel task (ignored).
        \param[in]  next: Next kernel task.
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateWake(KernelTask *now, KernelTask *next, Stack *&idle, Stack *&active)
    {
        STK_UNUSED(now);

        STK_ASSERT(next != nullptr);

        idle   = &m_sleep_trap[0].stack;
        active = next->GetUserStackPtr();

        // if stack memory is exceeded these assertions will be hit
        STK_ASSERT(m_sleep_trap[0].memory[0] == STK_STACK_MEMORY_FILLER);
    #if STK_STACK_GUARD
        STK_ASSERT(next->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);
    #endif

        m_task_now = next;

        if __stk_constexpr_cpp17 (IsHrtMode())
        {
            next->HrtOnSwitchedIn();
        }

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
        STK_UNUSED(next);

        STK_ASSERT(now != nullptr);
        STK_ASSERT(m_sleep_trap[0].stack.SP != 0);

        idle   = now->GetUserStackPtr();
        active = &m_sleep_trap[0].stack;

        m_task_now = util::DListCast::ListEntryToParent<KernelTask>(m_strategy.GetFirst());

        if __stk_constexpr_cpp17 (IsHrtMode())
        {
            if (!now->IsPendingRemoval())
            {
                now->HrtOnSwitchedOut();
            }
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
        STK_UNUSED(now);
        STK_UNUSED(next);

        if __stk_constexpr_cpp17 (IsDynamicMode())
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
            STK_UNUSED(idle);
            STK_UNUSED(active);
        }

        return false;
    }

    /*! \brief     Check whether Initialize() has been called and completed successfully.
        \return    \c true if Initialize() was called, \c false otherwise.
    */
    bool IsInitialized() const { return (m_kstate != KSTATE_INACTIVE); }

#if STK_SEGGER_SYSVIEW
    /*! \brief      Emit SEGGER SystemView task registration info for a kernel task.
        \param[in]  task: Kernel task to register. Must be bound (IsBusy() == true).
        \note       Only compiled when STK_SEGGER_SYSVIEW is enabled.
    */
    void SendTaskTraceInfo(KernelTask *task)
    {
        STK_ASSERT(task->IsBusy());

        const SEGGER_SYSVIEW_TASKINFO info =
        {
            .TaskID    = task->GetUserStackPtr()->tid,
            .sName     = task->GetUserTask()->GetTraceName(),
            .Prio      = static_cast<U32>(task->GetWeight()),
            .StackBase = hw::PtrToWord(task->GetUserTask()->GetStack()),
            .StackSize = (task->GetUserTask()->GetStackSize() * sizeof(Word))
        };
        SEGGER_SYSVIEW_SendTaskInfo(&info);
    }
#endif

    // Kernel modes:
    static constexpr bool IsStaticMode()    { return ((TMode & KERNEL_STATIC) != 0U); }
    static constexpr bool IsDynamicMode()   { return ((TMode & KERNEL_DYNAMIC) != 0U); }
    static constexpr bool IsHrtMode()       { return ((TMode & KERNEL_HRT) != 0U); }
    static constexpr bool IsSyncMode()      { return ((TMode & KERNEL_SYNC) != 0U); }
    static constexpr bool IsTicklessMode()  { return ((TMode & KERNEL_TICKLESS) != 0U); }
    static constexpr bool IsTimesliceMode() { return ((TMode & KERNEL_TIMESLICE) != 0U); }
    static constexpr bool IsTelemetryMode() { return ((TMode & KERNEL_TELEMETRY) != 0U); }

    // If hit here: Kernel<N> expects at least 1 task, e.g. N > 0
    STK_STATIC_ASSERT_N(TASKS_MAX, TASKS_MAX != 0U);

    // If hit here: Kernel mode must be assigned.
    STK_STATIC_ASSERT_N(KERNEL_MODE_MUST_BE_SET, (TMode != 0U));

    // If hit here: KERNEL_STATIC and KERNEL_DYNAMIC can not be mixed, either one of these is possible.
    STK_STATIC_ASSERT_N(KERNEL_MODE_MIX_NOT_ALLOWED,
        (((TMode & KERNEL_STATIC) & (TMode & KERNEL_DYNAMIC)) == 0U));

    // If hit here: KERNEL_HRT must accompany KERNEL_STATIC or KERNEL_DYNAMIC.
    STK_STATIC_ASSERT_N(KERNEL_MODE_HRT_ALONE, (((TMode & KERNEL_HRT) == 0U) ||
        ((((TMode & KERNEL_HRT) != 0U)) && (((TMode & KERNEL_STATIC) != 0U) || ((TMode & KERNEL_DYNAMIC) != 0U)))));

    // If hit here: KERNEL_TICKLESS is incompatible with KERNEL_HRT. Tickless suppresses the timer,
    // which destroys the precise periodicity HRT depends on.
    STK_STATIC_ASSERT_N(TICKLESS_HRT_CONFLICT,
        (((TMode & KERNEL_TICKLESS) == 0U) || ((TMode & KERNEL_HRT) == 0U)));

    // If hit here: KERNEL_TIMESLICE is incompatible with KERNEL_HRT, HRT tasks are scheduled by
    // periodicity and Yield() signals job completion.
    STK_STATIC_ASSERT_N(TIMESLICE_HRT_CONFLICT,
        (((TMode & KERNEL_TIMESLICE) == 0U) || ((TMode & KERNEL_HRT) == 0U)));

    // If hit here: KERNEL_TIMESLICE requires a strategy with a sticky GetNext() and OnTaskTimeSlice()
    // (TIMESLICE_API == 1), e.g. SwitchStrategyPT32_TS.
    STK_STATIC_ASSERT_N(TIMESLICE_STRATEGY_UNSUPPORTED,
        (((TMode & KERNEL_TIMESLICE) == 0U) || (TStrategy::TIMESLICE_API != 0)));

    // If hit here: Strategy which supports Priority Inheritance API must also support Weight API.
    STK_STATIC_ASSERT_N(KERNEL_MODE_MUST_BE_SET, (TStrategy::PRIORITY_INHERITANCE_API && TStrategy::WEIGHT_API) ||
        !TStrategy::PRIORITY_INHERITANCE_API);

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

        Stack  stack;  //!< Stack descriptor (SP register value + access mode). Initialized by InitTraps() on every Start().
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

        Stack  stack;  //!< Stack descriptor (SP register value + access mode). Initialized by InitTraps() on every Start().
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
    TaskStorageType  m_task_storage;    //!< Static pool of TSize KernelTask slots (free slots have m_user == nullptr).
    SleepTrapStack   m_sleep_trap[1];   //!< Sleep trap (always present): executed when all tasks are sleeping.
    ExitTrapStack    m_exit_trap[STK_ALLOCATE_COUNT<TMode, KERNEL_DYNAMIC, 1U, 0U>::Value]; //!< Exit trap: zero-size in KERNEL_STATIC mode; one entry in KERNEL_DYNAMIC mode.
    EFsmState        m_fsm_state;       //!< Current FSM state. Drives context-switch decision on every tick.
    volatile EKernelState m_kstate;     //!< Current kernel state.
    SyncObjectList   m_sync_list[STK_ALLOCATE_COUNT<TMode, KERNEL_SYNC, 1U, 0U>::Value]; //!< List of active sync objects. Zero-size (no memory) if KERNEL_SYNC is not set.

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
