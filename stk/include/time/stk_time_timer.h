/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_TIME_TIMER_H_
#define STK_TIME_TIMER_H_

#include "sync/stk_sync_event.h"
#include "sync/stk_sync_pipe.h"

/*! \file  stk_time_timer.h
    \brief Implementation of stk::time::TimerHost, a software timer multiplexer that manages multiple
           stk::time::TimerHost::Timer instances.
*/

namespace stk {
namespace time {

/*! \def   STK_TIMER_THREADS_COUNT
    \brief Number of threads handling timers in TimerHost (default: 1).
*/
#ifndef STK_TIMER_THREADS_COUNT
    #define STK_TIMER_THREADS_COUNT 1U
#endif

/*! \def   STK_TIMER_HANDLER_STACK_SIZE
    \brief Stack size of the timer handler, increase if your timers consume more (default: 256).
*/
#ifndef STK_TIMER_HANDLER_STACK_SIZE
    #define STK_TIMER_HANDLER_STACK_SIZE 256U
#endif

/*! \def   STK_TIMER_COUNT_MAX
    \brief Max number of timers handled by TimerHost (default: 32).
*/
#ifndef STK_TIMER_COUNT_MAX
    #define STK_TIMER_COUNT_MAX 32U
#endif

/*! \class TimerHost
    \brief Software timer multiplexer that manages multiple Timer instances on top of a small
           fixed set of kernel tasks.

    TimerHost has two operating modes:
    - <b>Task mode</b> (Initialize()): TimerHost registers its own kernel tasks and runs on its own.
    - <b>Polled mode</b> (InitializePolled()): TimerHost registers no kernel tasks, the application calls
      Process() periodically and expired timers are handled inline (see "Polled mode and HRT kernels").

    In task mode TimerHost internally runs two categories of tasks:
    - One \e tick task that maintains the active timer list, evaluates deadlines every wake
      cycle, and queues expired timers for dispatch.
    - One or more \e handler tasks (see \a STK_TIMER_THREADS_COUNT) that dequeue expired
      timers and invoke their OnExpired() callbacks.

    All timers share the same tick and handler tasks, so the total kernel task overhead is
    constant regardless of how many timers are active.

    Two timer modes are supported:
    - <b>One-shot</b>: fires once after \a delay ticks and becomes inactive automatically.
    - <b>Periodic</b>: fires every \a period ticks until explicitly stopped.

    \note  TimerHost must be initialized before use by calling Initialize() (task mode) or InitializePolled() (polled mode).
           The maximum number of concurrently active timers is STK_TIMER_COUNT_MAX (default: 32).
           The maximum timer period is bounded by \c uint32_t (~49 days at 1 ms resolution).
    \note  Requires a kernel built with stk::KERNEL_SYNC: the command and ready queues are stk::sync::Pipe based.
    \note  In task mode TimerHost occupies TASK_COUNT (1 + \a STK_TIMER_THREADS_COUNT) kernel task slots, make sure the
           kernel's task budget (e.g. Kernel::TASKS_MAX) accounts for them. Initialize() reports a failure
           to add a task (e.g. \a ADD_FAIL_POOL_FULL), in KERNEL_DYNAMIC mode such a failure is never asserted.
           Polled mode occupies no task slots.
    \note  Task mode is a soft real-time component: its tasks block on internal queues and are registered with
           IKernel::AddTask(ITask *). It cannot be used with a kernel running in stk::KERNEL_HRT mode
           (the kernel rejects the task with \a ADD_FAIL_INVALID and asserts), use polled mode there.

    \par Scheduling weights
    The tick task and the handler tasks are registered with the weights supplied via Config
    (see Initialize()), by default DEFAULT_WEIGHT is used for all of them. A weight is
    \e strategy-specific and is passed to the kernel unchanged, TimerHost does not interpret it:
    - SwitchStrategyRoundRobin, SwitchStrategyEDF, SwitchStrategyMonotonic: weights are ignored.
    - SwitchStrategyFixedPriority: the weight is the priority level, [0, MAX_PRIORITIES - 1].
    - SwitchStrategySmoothWeightedRoundRobin: the weight is a CPU share, (0, 0x7FFFFF], a larger value
      gives more slices but does not preempt.
    - SwitchStrategyPreemptionThreshold: the weight is encoded with MakeWeight(priority, threshold).
      DEFAULT_WEIGHT (1) decodes as priority 0 / threshold 1, i.e. every timer task would be \e held while
      it runs, therefore the weights \b must be set explicitly. Use threshold == priority
      (MakeWeight(p, p)) unless holding is intended.

    It is recommended that the tick task is at least as urgent as the handler tasks, otherwise a long
    OnExpired() callback delays evaluation of all other deadlines.

    Configuration examples (pick the one matching the strategy the kernel is built with, a kernel
    using SwitchStrategyRoundRobin, SwitchStrategyEDF or SwitchStrategyMonotonic needs no Config):
    \code
    stk::time::TimerHost::Config cfg;

    // SwitchStrategyFP32: real preemption priority
    cfg.tick_weight    = stk::SwitchStrategyFP32::PRIORITY_HIGH;
    cfg.handler_weight = stk::SwitchStrategyFP32::PRIORITY_ABOVE_NORMAL;

    // SwitchStrategyPT32: keep threshold == priority (no hold)
    cfg.tick_weight    = stk::SwitchStrategyPT32::MakeWeight(5, 5);
    cfg.handler_weight = stk::SwitchStrategyPT32::MakeWeight(4, 4);

    // SwitchStrategySWRR: no preemption, only a larger share. Tick latency is bounded by rotation, not priority.
    cfg.tick_weight    = 4;
    cfg.handler_weight = 1;

    g_TimerHost.Initialize(kernel, stk::ACCESS_USER, cfg);
    \endcode

    \par Polled mode and HRT kernels
    In polled mode TimerHost creates no kernel tasks, therefore the weights and stack buffers of Config do
    not apply. The application calls Process() periodically from a single context of its own and the
    OnExpired() callbacks of expired timers run inline from Process(). This is the way to use TimerHost
    with stk::KERNEL_HRT, where tasks are periodic jobs (IKernel::AddTask() with periodicity and deadline)
    and blocking on queues is not a job. Things to keep in mind:
    - the period of the driving job is the timer resolution: a timer fires on the first Process() call at or
      after its deadline, i.e. it may be late by up to one period;
    - callbacks run in the driving job, so the worst-case execution time of all callbacks which can expire
      in the same call is part of that job's WCET and of the schedulability analysis;
    - Start(), Stop() and the other commands may be issued from any task (callbacks included), they
      take effect at the next Process() call;
    - Process() never blocks, it uses non-blocking queue operations only.

    \code
    // HRT kernel: a periodic job drives the timers
    class TimerJob : public stk::Task<256, stk::ACCESS_USER>
    {
    public:
        void Run()
        {
            for (;;)
            {
                g_TimerHost.Process(); // runs expired callbacks inline
                stk::Yield();          // job completed, sleep until the next period
            }
        }
    };

    TimerJob g_TimerJob;

    void SetupTimers(stk::IKernel *kernel)
    {
        g_TimerHost.InitializePolled();

        // 5 ms timer resolution, 2 ms budget (callbacks included) per job
        kernel->AddTask(&g_TimerJob, stk::GetTicksFromMsClampedToTimeout(5), stk::GetTicksFromMsClampedToTimeout(2), 0);
    }
    \endcode

    Process() returns the number of ticks until the earliest deadline, a non-HRT application can use it to
    sleep only as long as needed (commands issued meanwhile are picked up when the sleep ends, hence the cap):
    \code
    for (;;)
    {
        const stk::Timeout next = g_TimerHost.Process();

        if (next != stk::NO_WAIT)
        {
            stk::Sleep((next < 100) ? next : 100);
        }
    }
    \endcode

    \code
    // define a concrete timer by overriding OnExpired
    class HeartbeatTimer : public stk::time::TimerHost::Timer
    {
    public:
        void OnExpired(stk::time::TimerHost *host)
        {
            // called every 500 ms
            ToggleLed();
        }
    };

    // declare host and timer instances (static storage, no heap)
    stk::time::TimerHost  g_TimerHost;
    HeartbeatTimer        g_Heartbeat;

    // one-shot example: send a delayed notification
    class NotifyTimer : public stk::time::TimerHost::Timer
    {
    public:
        void OnExpired(stk::time::TimerHost *host)
        {
            g_Event.Signal();
        }
    };

    NotifyTimer g_Notify;

    void SetupTimers(stk::IKernel *kernel)
    {
        // initialize the host, must be called before Start() (default weights and internal stacks)
        if (g_TimerHost.Initialize(kernel, stk::ACCESS_USER) != stk::ADD_OK)
        {
            // e.g. no free task slots in the kernel
            return;
        }

        // start 500 ms periodic heartbeat
        uint32_t period = static_cast<uint32_t>(stk::GetTicksFromMs(500));
        g_TimerHost.Start(g_Heartbeat, period, period);

        // start a one-shot notification after 1 second
        uint32_t delay = static_cast<uint32_t>(stk::GetTicksFromMs(1000));
        g_TimerHost.Start(g_Notify, delay);
    }
    \endcode

    \see TimerHost::Timer, TimerHost::Config, TimerHost::Process, TimerHost::InitializePolled, STK_TIMER_THREADS_COUNT, STK_TIMER_HANDLER_STACK_SIZE, STK_TIMER_COUNT_MAX
*/
class TimerHost
{
public:
    enum EConsts : size_t
    {
        //!< total number of tasks serving this instance
        TASK_COUNT = (1U + STK_TIMER_THREADS_COUNT),

        //!< stack memory size of the tick task
        TASK_TICK_MEMORY_SIZE = Max<size_t>(256U, STK_STACK_SIZE_MIN),

        //!< stack memory size of the timer handler task
        TASK_HANDLER_STACK_SIZE = Max<size_t>(STK_TIMER_HANDLER_STACK_SIZE, STK_STACK_SIZE_MIN)
    };

    /*! \class Timer
        \brief Abstract base class for a timer managed by TimerHost.

        To implement a concrete timer, inherit from this class and override OnExpired().
        The timer instance must outlive the TimerHost or be explicitly stopped before
        destruction, as TimerHost holds a non-owning pointer to each active timer.

        Each Timer instance may be registered with at most one TimerHost at a time.

        \see TimerHost::Start, TimerHost::Stop, TimerHost::Reset, TimerHost::Restart
    */
    class Timer : private util::DListEntry<Timer, false>
    {
        friend class TimerHost;
        friend class util::DListCast; // allow casts because DListEntry is private

    public:
        /*! \brief     Default constructor.
        */
        explicit Timer() 
            : m_deadline(0), m_timestamp(0), m_period(0U), m_active(false), 
              m_pending(false), m_rearming(false)
        {}

        /*! \brief     Destructor.
            \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
        */
        STK_VIRT_DTOR ~Timer() = default;
        
        /*! \brief A tag for type-safe casts done by CastListEntryToParent.
            \see   CastListEntryToParent.
        */
        enum { DLEntryTag = 1 };

        /*! \brief     Callback invoked when this timer expires.
            \param[in] host: Pointer to the TimerHost that fired this timer.
            \note      Task mode: executes in the context of a TimerHost handler task.
                       Polled mode: executes inline in the context which calls TimerHost::Process().
            \note      Implementations must be safe to call from that context and
                       should complete quickly, long-running work should be delegated
                       (e.g. via an event or queue) to avoid blocking other expired timers.
                       In polled mode (e.g. an HRT job) the execution time counts against the
                       deadline of the job which calls Process(), and the callback must not
                       call Process() itself.
        */
        virtual void OnExpired(TimerHost *host) = 0;

        /*! \brief     Check whether the timer is currently active.
            \return    True if the timer has been started and not yet stopped or expired (one-shot).
            \note      The value is advisory, it is written by the tick task and read without
                       synchronization, so it may be transiently stale on multi-core targets.
        */
        bool IsActive() const { return m_active; }

        /*! \brief     Get the absolute time in ticks at which the timer will expire.
            \return    Absolute expiration deadline in ticks.
            \note      Meaningful only when IsActive() returns true.
        */
        Ticks GetDeadline() const { return m_deadline; }

        /*! \brief     Get the tick count at which the timer last expired.
            \return    Absolute tick count recorded by the tick task at expiration time,
                       or 0 if the timer has never fired.
        */
        Ticks GetTimestamp() const { return m_timestamp; }

        /*! \brief     Get the reload period of the timer.
            \return    Period in ticks, or 0 for a one-shot timer.
        */
        uint32_t GetPeriod() const { return m_period; }

        /*! \brief     Get remaining ticks until the timer next expires.
            \return    Ticks remaining, or 0 if already expired / not active.
            \note      The value is advisory: it is computed from m_now (the last value written
                       by the tick task) and may be up to one tick task wake cycle stale.
                       If the deadline has already passed but not yet been processed, 0 is returned.
        */
        uint32_t GetRemainingTicks() const;

    private:
        STK_NONCOPYABLE_CLASS(Timer);

        Ticks         m_deadline;  //!< absolute expiration time (ticks)
        Ticks         m_timestamp; //!< absolute time at which timer expired (ticks), updated by TimerHost
        uint32_t      m_period;    //!< reload period in ticks (0 = one-shot)
        volatile bool m_active;    //!< true if active
        volatile bool m_pending;   //!< true if pending to be handled
        volatile bool m_rearming;  //!< true while a CMD_RESTART / CMD_START_OR_RESET is already in the command queue
    };

    /*! \struct Config
        \brief  Optional configuration of the timer tasks, see Initialize().

        A default-constructed Config reproduces the fixed behavior of TimerHost: DEFAULT_WEIGHT for all
        tasks and the internal stack buffers.

        \note  Weights are strategy-specific and forwarded to the kernel as is (see "Scheduling weights"
               in TimerHost). Only NO_WEIGHT is rejected here, all other validation is performed by the
               kernel and its task-switching strategy when the task is added.
        \note  Externally supplied stack buffers must outlive the host, be distinct from each other (the
               kernel rejects a duplicate stack with \a ADD_FAIL_DUPLICATE), and satisfy the stack alignment
               and memory protection requirements of the platform (the internal buffers, see
               StackMemoryDef, already do).
    */
    struct Config
    {
        Weight tick_weight;                            //!< Weight of the tick task (default: DEFAULT_WEIGHT).
        Weight handler_weight;                         //!< Weight of every handler task (default: DEFAULT_WEIGHT).
        Word  *tick_stack;                             //!< Stack of the tick task, or \c nullptr to use the internal buffer (default).
        size_t tick_stack_size;                        //!< Size of \a tick_stack in elements of Word (>= STK_STACK_SIZE_MIN), ignored if \a tick_stack is \c nullptr.
        Word  *handler_stack[STK_TIMER_THREADS_COUNT]; //!< Stack of each handler task, a \c nullptr entry selects the internal buffer (default).
        size_t handler_stack_size;                     //!< Size of every non-null \a handler_stack entry in elements of Word (>= STK_STACK_SIZE_MIN).

        /*! \brief Default constructor: default weights and internal stack buffers.
        */
        explicit Config()
            : tick_weight(DEFAULT_WEIGHT), handler_weight(DEFAULT_WEIGHT), tick_stack(nullptr),
              tick_stack_size(0U), handler_stack(), handler_stack_size(0U)
        {}
    };

    /*! \brief Default constructor. Zero-initializes all internal state.
        \note  Call Initialize() before using any other member function.
    */
    explicit TimerHost() 
      : m_task_tick_memory(), m_task_handler_memory(), m_task_tick(), m_task_process(), 
        m_active(), m_now(0), m_polled(false)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~TimerHost() = default;

    /*! \brief     Initialize timer host instance.
        \param[in] kernel: Kernel to which instance will be bound.
        \param[in] mode: Access mode for the timer handling tasks which call expired timers. The tick task
                   always runs with ACCESS_USER.
        \param[in] cfg: Weights and (optionally) stack buffers of the tick and handler tasks, by default
                   DEFAULT_WEIGHT and the internal stacks are used (see Config).
        \return    ADD_OK on success, otherwise the first failure reported by IKernel::AddTask(ITask *), e.g.
                   \a ADD_FAIL_POOL_FULL if the kernel has no free task slots, \a ADD_FAIL_DUPLICATE if a stack
                   is shared, or \a ADD_FAIL_INVALID (e.g. KERNEL_HRT kernel or kernel not initialized).
        \note      Must be called before Start() of a KERNEL_STATIC kernel and after the kernel was initialized.
        \note      On failure the host is unusable. Tasks added before the failure remain registered with the
                   kernel (blocked on the empty queue), in KERNEL_DYNAMIC mode they can be released with
                   IKernel::ScheduleTaskRemoval().
        \warning   With SwitchStrategyPreemptionThreshold the weights in \a cfg must be set explicitly,
                   the default weight is not neutral for this strategy (see "Scheduling weights" in TimerHost).
    */
    EAddResult Initialize(IKernel *kernel, EAccessMode mode, const Config &cfg = Config());

    /*! \brief     Initialize timer host instance in polled mode (no kernel tasks are created).
        \details   The application drives the host by calling Process() periodically. Expired timers are
                   handled inline, from the context which calls Process(), see "Polled mode and HRT kernels"
                   in TimerHost. This is the mode to use with a stk::KERNEL_HRT kernel.
        \note      Mutually exclusive with Initialize(), call one of them before any other member function.
                   After Shutdown() the host can be initialized again in either mode.
                   No kernel is involved, therefore it can not fail and does not depend on kernel mode,
                   task slots, weights or stack buffers (see Config).
    */
    void InitializePolled();

    /*! \brief     Run the host once (polled mode only): execute pending commands, evaluate deadlines and
                   invoke OnExpired() of every expired timer inline.
        \return    Ticks until the earliest active deadline: NO_WAIT if a timer is already overdue (call again
                   without delay), WAIT_INFINITE if no timer is active.
        \note      Never blocks. Call it periodically from one context only, not concurrently and not
                   from OnExpired(). Start(), Stop() and the other commands remain callable from any task.
        \note      Resolution: a timer fires on the first call at or after its deadline. A periodic timer which
                   is overdue by more than its period fires once per call until its schedule has caught up
                   (no drift is accumulated).
        \note      Callbacks run one after another with the time sampled at the start of the call, a slow callback
                   delays the following ones. Commands issued by a callback take effect at the next call.
        \note      Shutdown() stops and removes all timers, the host stays usable afterwards (there are no tasks to exit).
        \warning   Only valid after InitializePolled().
    */
    Timeout Process();

    /*! \brief     Start timer.
        \param[in] tmr: Timer instance. Must not already be active.
        \param[in] delay: Initial delay in ticks before first expiration.
        \param[in] period: Reload period in ticks (0 is one-shot timer).
        \return    True on success, false if timer is already active or command queue is full.
    */
    bool Start(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Stop running timer.
        \param[in] tmr: Timer instance. Must be active.
        \return    True on success, false if timer is not active or command queue is full.
    */
    bool Stop(Timer &tmr);

    /*! \brief     Reset periodic timer's deadline.
        \param[in] tmr: Timer instance. Must be active and periodic.
        \return    True on success, false if timer is not active, not periodic, or command queue is full.
    */
    bool Reset(Timer &tmr);

    /*! \brief     Atomically stop and re-start timer.
        \param[in] tmr: Timer instance (active or inactive).
        \param[in] delay: Initial delay in ticks before first expiration.
        \param[in] period: Reload period in ticks (0 is one-shot timer).
        \return    True on success, false if command queue is full.
        \note      Unlike calling Stop() followed by Start(), this operation is atomic
                   with respect to the tick task: the timer cannot fire between the implicit
                   stop and re-start, and only one command queue slot is consumed.
                   Useful for watchdog refresh and debounce reset patterns.
                   Safe to call regardless of whether the timer is currently active.
    */
    bool Restart(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Start timer if inactive, or reset its deadline if already active and periodic.
        \param[in] tmr: Timer instance (active or inactive).
        \param[in] delay: Initial delay in ticks (used only when starting).
        \param[in] period: Reload period in ticks (used only when starting, 0 is one-shot).
        \return    True on success, false if command queue is full.
        \note      Collapses the common pattern:
                     if (timer.IsActive()) host.Reset(timer);
                     else                  host.Start(timer, delay, period);
                   into a single atomic operation, eliminating the TOCTOU race between
                   the IsActive() check and the subsequent call.
                   If the timer is active but one-shot, no action is taken (a one-shot
                   timer mid-flight cannot be reset; use Restart() instead).
    */
    bool StartOrReset(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Change the period of a running periodic timer without affecting its current deadline.
        \param[in] tmr: Timer instance. Must be active and periodic.
        \param[in] period: New reload period in ticks. Must be non-zero.
        \return    True on success, false if timer is not active, not periodic,
                   period is zero, or command queue is full.
        \note      The new period takes effect on the next reload after the current deadline
                   fires. To apply the new period immediately (restart from now), call
                   Reset() after SetPeriod().
    */
    bool SetPeriod(Timer &tmr, uint32_t period);

    /*! \brief    Return true if no timers are currently active.
        \return   True if active timer count is zero.
        \note     The value is advisory and may change immediately after the call.
    */
    bool IsEmpty() const { return m_active.IsEmpty(); }

    /*! \brief    Return number of currently active timers.
        \return   Active timer count.
        \note     The value is advisory and may change immediately after the call.
    */
    size_t GetSize() const { return m_active.GetSize(); }

    /*! \brief    Shutdown host instance. All timers are stopped and removed from the host.
        \return   True on success, false if command queue is full.
        \note     Task mode: the tick and handler tasks exit. Polled mode: the host stays usable.
    */
    bool Shutdown();

    /*! \brief  Get current time.
        \return Current time (ticks).
    */
    Ticks GetTimeNow() const { return hw::ReadVolatile64(&m_now); }

private:
    STK_NONCOPYABLE_CLASS(TimerHost);

    /*! \typedef TimerFuncType
        \brief   Timer task function prototype.
    */
    typedef void (*TimerFuncType) (TimerHost *host);

    /*! \class TimerWorkerTask
        \brief Internal kernel task used by TimerHost for both the tick task and handler tasks.

        Wraps a free function pointer (\a TimerFuncType) so that the same ITask-derived
        class can serve either role (tick or handler) with different stack buffers, access
        modes, weights, and entry points supplied at Initialize() time.

        \note  This class is an implementation detail of TimerHost and is not part of the
               public API. Do not instantiate or inherit from it directly.
    */
    class TimerWorkerTask : public ITask
    {
    public:
        /*! \brief Default constructor. All members are zero/null-initialized. */
        explicit TimerWorkerTask()
            : m_func(nullptr), m_host(nullptr), m_stack(nullptr), m_stack_size(0U), 
              m_mode(ACCESS_USER), m_weight(DEFAULT_WEIGHT)
        {}

        // ITask
        EAccessMode GetAccessMode() const override { return m_mode; }
        void OnDeadlineMissed(uint32_t)   override {}
        void OnExit()                     override {}
        Weight GetWeight()          const override { return m_weight; }
        const char *GetTraceName()  const override { return nullptr; }

        // IStackMemory
        const Word *GetStack()      const override { return m_stack; }
        size_t GetStackSize()       const override { return m_stack_size; }

        /*! \brief     Bind this task to a host, stack buffer, access mode, and entry function.
            \param[in] host: TimerHost instance this task belongs to.
            \param[in] stack: Pointer to the pre-allocated stack memory.
            \param[in] stack_size: Stack size in words.
            \param[in] mode: Kernel access mode (user or privileged).
            \param[in] weight: Scheduling weight, strategy-specific and forwarded to the kernel unchanged.
            \param[in] func: Entry function called in the task's context.
            \note  Must be called before the task is added to the kernel. The weight must not be changed
                   afterwards (the kernel's strategy indexes the task by the weight seen at IKernel::AddTask()),
                   use IKernelService::SetWeight() to change it at run-time.
        */
        void Initialize(TimerHost *host, Word *stack, size_t stack_size, EAccessMode mode,
                        Weight weight, TimerFuncType const func)
        {
            m_host       = host;
            m_func       = func;
            m_stack      = stack;
            m_stack_size = stack_size;
            m_mode       = mode;
            m_weight     = weight;
        }

    private:
        /*! \brief Timer task entry point. */
        void Run() override { m_func(m_host); }

        TimerFuncType m_func;       //!< entry function (tick loop or handler loop)
        TimerHost    *m_host;       //!< owning TimerHost instance
        Word         *m_stack;      //!< pointer to stack buffer
        size_t        m_stack_size; //!< stack size in words
        EAccessMode   m_mode;       //!< kernel access mode
        Weight        m_weight;     //!< scheduling weight (strategy-specific, see TimerHost)
    };

    /*! \struct TimerCommand
        \brief  POD command record passed from the public API methods to the tick task via the command queue.

        Each command encodes a single operation together with the timer pointer, a call-site
        timestamp, and optional delay/period arguments. Commands are produced by the public
        API (Start, Stop, Reset, …) and consumed exclusively by the tick task inside
        ProcessCommands().
    */
    struct TimerCommand
    {
        /*! \enum  EId
            \brief Identifies the operation to perform on the target timer.
        */
        enum EId : uint8_t
        {
            CMD_SHUTDOWN = 0,   //!< shutdown timer host
            CMD_START,          //!< start timer
            CMD_STOP,           //!< stop timer
            CMD_RESET,          //!< reset timer
            CMD_RESTART,        //!< atomic stop + re-start
            CMD_START_OR_RESET, //!< start if inactive, reset deadline if active and periodic
            CMD_SET_PERIOD      //!< change period of a running periodic timer
        };

        EId      cmd;       //!< command id
        Timer   *timer;     //!< timer instance
        Ticks    timestamp; //!< hw tick count captured at call site
        uint32_t delay;     //!< initial delay ticks (CMD_START / CMD_RESTART / CMD_START_OR_RESET)
        uint32_t period;    //!< reload period ticks (CMD_START / CMD_RESTART / CMD_START_OR_RESET / CMD_SET_PERIOD)
    };

    /*! \brief  Tick task body: drives the timer list and dispatches expired timers.
        \note   Runs exclusively in the tick task context. Loops until CMD_SHUTDOWN is received.
    */
    void UpdateTime();

    /*! \brief     Evaluate all active timers against the given time.
        \param[in] now: Current time (ticks).
        \return    Ticks until the earliest active deadline (NO_WAIT if a timer is overdue, WAIT_INFINITE if none).
        \note      Task mode: queues expired timers for the handler tasks. Polled mode: invokes OnExpired()
                   of expired timers inline. Runs exclusively in the tick task (task mode) or in Process() (polled mode).
    */
    Timeout ScanTimers(Ticks now);

    /*! \brief  Handler task body: dequeues expired timers and invokes their OnExpired() callbacks.
        \note   Runs in each handler task context. Loops until a nullptr sentinel is dequeued.
    */
    void ProcessTimers();

    /*! \brief     Drain the command queue and execute each pending command.
        \param[in] next_sleep: Maximum ticks to block waiting for the first command when the
                   active list is non-empty. Overridden to WAIT_INFINITE when empty (task mode only,
                   polled mode never blocks).
        \return    True to continue the tick loop; false when CMD_SHUTDOWN is processed.
        \note      Called exclusively from the tick task (UpdateTime()).
    */
    bool ProcessCommands(Timeout next_sleep);

    /*! \brief     Enqueue a command for the tick task.
        \param[in] cmd: Fully initialized TimerCommand to push.
        \return    True on success, false if the command queue is full.
        \note      May be called from any task context. On queue-full the assertion fires in
                   debug builds; in release the caller receives false and may retry or escalate.
    */
    bool PushCommand(TimerCommand cmd);

    /*! \typedef TaskTickMemory
        \brief   Stack memory type for the single tick task.
    */
    typedef StackMemoryDef<TASK_TICK_MEMORY_SIZE>::Type    TaskTickMemory;

    /*! \typedef TimerHostMemory
        \brief   Stack memory type for each handler task.
    */
    typedef StackMemoryDef<TASK_HANDLER_STACK_SIZE>::Type  TimerHostMemory;

    /*! \typedef ReadyQueue
        \brief   Lock-free pipe used to transfer expired timer pointers from the tick task to handler tasks.
    */
    typedef sync::PipeT<Timer *, STK_TIMER_COUNT_MAX>      ReadyQueue;

    /*! \typedef CommandQueue
        \brief   Lock-free pipe used to send TimerCommand records from API callers to the tick task.
    */
    typedef sync::PipeT<TimerCommand, STK_TIMER_COUNT_MAX> CommandQueue;

    TaskTickMemory                m_task_tick_memory;                             //!< tick task memory
    TimerHostMemory               m_task_handler_memory[STK_TIMER_THREADS_COUNT]; //!< handler task memory
    TimerWorkerTask               m_task_tick;                                    //!< timer task
    TimerWorkerTask               m_task_process[STK_TIMER_THREADS_COUNT];        //!< handler tasks
    util::DListHead<Timer, false> m_active;                                       //!< active timers (tick task only)
    ReadyQueue                    m_queue;                                        //!< queue of timers ready for handling
    CommandQueue                  m_commands;                                     //!< command queue
    Ticks                         m_now;                                          //!< last known current time (ticks)
    bool                          m_polled;                                       //!< true in polled mode (InitializePolled()), no tasks, callbacks run inline from Process()
};

// ---------------------------------------------------------------------------
// Timer::GetRemainingTicks
// ---------------------------------------------------------------------------

inline uint32_t TimerHost::Timer::GetRemainingTicks() const
{
    uint32_t remaining_ticks = 0U;

    if (m_active)
    {
        // if deadline has passed but not yet been processed by the tick task,
        // remaining would be negative, result fits in uint32_t because delay and
        // period are uint32_t, so remaining time is bounded by uint32_t max
        const Ticks remaining = m_deadline - GetTicks();
        
        if (remaining > 0)
        {
            remaining_ticks = static_cast<uint32_t>(remaining);
        }
    }

    return remaining_ticks;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

inline EAddResult TimerHost::Initialize(IKernel *kernel, EAccessMode mode, const Config &cfg)
{
    STK_ASSERT(kernel != nullptr);
    STK_ASSERT(cfg.tick_weight    != NO_WEIGHT);
    STK_ASSERT(cfg.handler_weight != NO_WEIGHT);

    m_polled = false; // task mode (selects the mode again if the host was used in polled mode before)

    EAddResult result = ADD_OK;

    // handler tasks first: they only wait on the (empty) ready queue until the tick task is added
    for (size_t i = 0U; (i < STK_TIMER_THREADS_COUNT) && (result == ADD_OK); ++i)
    {
        Word  *stack = m_task_handler_memory[i];
        size_t size  = TASK_HANDLER_STACK_SIZE;

        if (cfg.handler_stack[i] != nullptr)
        {
            stack = cfg.handler_stack[i];
            size  = cfg.handler_stack_size;
        }

        STK_ASSERT(size >= STK_STACK_SIZE_MIN);

        m_task_process[i].Initialize(this, stack, size, mode, cfg.handler_weight, [](TimerHost *host)
        {
            host->ProcessTimers();
        });
        result = kernel->AddTask(&m_task_process[i]);
    }

    if (result == ADD_OK)
    {
        Word  *stack = m_task_tick_memory;
        size_t size  = TASK_TICK_MEMORY_SIZE;

        if (cfg.tick_stack != nullptr)
        {
            stack = cfg.tick_stack;
            size  = cfg.tick_stack_size;
        }

        STK_ASSERT(size >= STK_STACK_SIZE_MIN);

        m_task_tick.Initialize(this, stack, size, ACCESS_USER, cfg.tick_weight, [](TimerHost *host)
        {
            host->UpdateTime();
        });
        result = kernel->AddTask(&m_task_tick);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

inline bool TimerHost::Start(Timer &tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(delay <= static_cast<uint32_t>(WAIT_INFINITE));
    STK_ASSERT((period == 0U) || (period <= static_cast<uint32_t>(WAIT_INFINITE)));
    
    bool success;

    // timer must not already be active
    if (!tmr.m_active)
    {
        success = PushCommand({
            .cmd       = TimerCommand::CMD_START,
            .timer     = &tmr,
            .timestamp = GetTicks(),
            .delay     = delay,
            .period    = period
        });
    }
    else
    {
        // duplicate attempt to start already started timer is not an error (ignore)
        success = true;
    }

    return success;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

inline bool TimerHost::Stop(Timer &tmr)
{
    bool success;
  
    // timer must be active
    if (tmr.m_active)
    {
        success = PushCommand({
            .cmd       = TimerCommand::CMD_STOP,
            .timer     = &tmr,
            .timestamp = 0,
            .delay     = 0U,
            .period    = 0U
        });
    }
    else
    {
        // duplicate attempt to stop already stopped timer is not an error (ignore)
        success = true;
    }

    return success;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

inline bool TimerHost::Reset(Timer &tmr)
{
    bool success;
  
    // timer must be active and periodic
    if (tmr.m_active && (tmr.m_period != 0U))
    {
        success = PushCommand({
            .cmd       = TimerCommand::CMD_RESET,
            .timer     = &tmr,
            .timestamp = GetTicks(),
            .delay     = 0U,
            .period    = 0U
        });
    }
    else
    {
        success = false;
    }

    return success;
}

// ---------------------------------------------------------------------------
// Restart
// ---------------------------------------------------------------------------

inline bool TimerHost::Restart(Timer &tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(delay <= static_cast<uint32_t>(WAIT_INFINITE));
    STK_ASSERT((period == 0U) || (period <= static_cast<uint32_t>(WAIT_INFINITE)));

    return PushCommand({
        .cmd       = TimerCommand::CMD_RESTART,
        .timer     = &tmr,
        .timestamp = GetTicks(),
        .delay     = delay,
        .period    = period
    });
}

// ---------------------------------------------------------------------------
// StartOrReset
// ---------------------------------------------------------------------------

inline bool TimerHost::StartOrReset(Timer &tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(delay <= static_cast<uint32_t>(WAIT_INFINITE));
    STK_ASSERT((period == 0U) || (period <= static_cast<uint32_t>(WAIT_INFINITE)));

    return PushCommand({
        .cmd       = TimerCommand::CMD_START_OR_RESET,
        .timer     = &tmr,
        .timestamp = GetTicks(),
        .delay     = delay,
        .period    = period
    });
}

// ---------------------------------------------------------------------------
// SetPeriod
// ---------------------------------------------------------------------------

inline bool TimerHost::SetPeriod(Timer &tmr, uint32_t period)
{
    bool success;
  
    // period == 0 is rejected: it would silently convert a periodic timer
    // to one-shot semantics, which is better expressed via Stop() + Start()
    if (tmr.m_active && (tmr.m_period != 0U) && (period != 0U) &&
        (period <= static_cast<uint32_t>(WAIT_INFINITE)))
    {
        success = PushCommand({
            .cmd       = TimerCommand::CMD_SET_PERIOD,
            .timer     = &tmr,
            .timestamp = 0,
            .delay     = 0U,
            .period    = period
        });
    }
    else
    {
        success = false;
    }

    return success;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

inline bool TimerHost::Shutdown()
{
    return PushCommand({
        .cmd       = TimerCommand::CMD_SHUTDOWN,
        .timer     = nullptr,
        .timestamp = 0,
        .delay     = 0U,
        .period    = 0U
    });
}

// ---------------------------------------------------------------------------
// UpdateTime  (tick task body)
// ---------------------------------------------------------------------------

inline void TimerHost::UpdateTime()
{
    Timeout next_sleep = NO_WAIT;

    while (ProcessCommands(next_sleep))
    {
        next_sleep = ScanTimers(GetTicks());
    }

    // unlink all timers on shutdown
    while (Timer::DLEntryType *const tmr = m_active.GetFirst())
    {
        m_active.Unlink(tmr);
    }
}

// ---------------------------------------------------------------------------
// ScanTimers  (tick task body / Process())
// ---------------------------------------------------------------------------

inline Timeout TimerHost::ScanTimers(Ticks now)
{
    Timeout next_sleep = WAIT_INFINITE;

    // using WriteVolatile64() to guarantee correct lockless reading order by ReadVolatile64
    hw::WriteVolatile64(&m_now, now);

    Timer *tmr = util::DListCast::ListEntryToParent<Timer>(m_active.GetFirst());
    while (tmr != nullptr)
    {
        Timer *const next = util::DListCast::ListEntryToParent<Timer>(tmr->GetNext());

        if (tmr->m_active)
        {
            // check if still pending to be handled
            if (!tmr->m_pending)
            {
                bool one_shot = false;
                const Ticks diff = now - tmr->m_deadline;

                if (diff >= 0)
                {
                    // set timestamp at which timer expired
                    tmr->m_timestamp = now;

                    // avoid updating timer again before it was handled
                    tmr->m_pending = true;

                    // periodic
                    if (tmr->m_period != 0U)
                    {
                        // reload (use now to avoid drift accumulation)
                        tmr->m_deadline = now + static_cast<Ticks>(tmr->m_period) - diff;
                    }
                    // one-shot
                    else
                    {
                        one_shot = true;

                        // remove from active timers
                        m_active.Unlink(tmr);

                        // mark as inactive (must follow Unlink)
                        tmr->m_active = false;
                    }

                    __stk_full_memfence();

                    if (m_polled)
                    {
                        // polled mode: no handler tasks, handle inline (the timer is already
                        // re-armed or unlinked, commands issued by the callback are deferred)
                        tmr->m_pending = false;
                        tmr->OnExpired(this);
                    }
                    else
                    {
                        // push to the handling queue
                        STK_UNUSED(m_queue.Write(tmr));
                    }
                }

                // one-shot timer does not affect next_sleep
                if (!one_shot)
                {
                    Timeout next_deadline = static_cast<Timeout>(tmr->m_deadline - now);

                    // task mode: a (re)loaded deadline always lies in the future, polled mode: the caller
                    // may poll slower than the period, an overdue timer is served by the next call
                    STK_ASSERT(m_polled || (next_deadline > 0));

                    if (next_deadline <= 0)
                    {
                        next_deadline = NO_WAIT;
                    }

                    if (next_deadline < next_sleep)
                    {
                        next_sleep = next_deadline;
                    }
                }
            }
        }
        else
        {
            // could be stopped externally, remove from active timers
            m_active.Unlink(tmr);
        }

        tmr = next;
    }

    return next_sleep;
}

// ---------------------------------------------------------------------------
// InitializePolled
// ---------------------------------------------------------------------------

inline void TimerHost::InitializePolled()
{
    m_polled = true;
}

// ---------------------------------------------------------------------------
// Process  (polled mode)
// ---------------------------------------------------------------------------

inline Timeout TimerHost::Process()
{
    STK_ASSERT(m_polled);

    // execute pending commands, never blocks in polled mode
    STK_UNUSED(ProcessCommands(NO_WAIT));

    return ScanTimers(GetTicks());
}

// ---------------------------------------------------------------------------
// ProcessTimers  (handler task body)
// ---------------------------------------------------------------------------

inline void TimerHost::ProcessTimers()
{
    Timer *tmr = nullptr;
    bool keep_running = true;

    while (keep_running)
    {
        if (!m_queue.Read(tmr))
        {
            break; // no pending timers available
        }
      
        // nullptr is the shutdown sentinel pushed by CMD_SHUTDOWN
        if (tmr == nullptr)
        {
            keep_running = false;
        }
        else if (tmr->m_pending)
        {
            tmr->m_pending = false;
            tmr->OnExpired(this);
        }
        else
        {
            // noop
        }
    }
}

// ---------------------------------------------------------------------------
// ProcessCommands  (tick task only)
// ---------------------------------------------------------------------------

inline bool TimerHost::ProcessCommands(Timeout next_sleep)
{
    TimerCommand cmd = {};
    bool working = true;

    // if nothing is active, sleep indefinitely until a command arrives (task mode only,
    // polled mode never blocks)
    if (m_active.IsEmpty() && !m_polled)
    {
        next_sleep = WAIT_INFINITE;
    }

    while (working)
    {
        if (!m_commands.Read(cmd, next_sleep))
        {
            break;
        }
      
        switch (cmd.cmd)
        {
        case TimerCommand::CMD_START: 
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            // reject if already active or linked (double-start)
            if (!tmr->m_active)
            {
                STK_ASSERT(!tmr->IsLinked());

                tmr->m_deadline = cmd.timestamp + static_cast<Ticks>(cmd.delay);
                tmr->m_period   = cmd.period;
                tmr->m_active   = true;
                tmr->m_pending  = false;

                m_active.LinkBack(tmr);
                next_sleep = NO_WAIT;
            }
            
        break; }

        case TimerCommand::CMD_STOP:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            tmr->m_active  = false;
            tmr->m_pending = false;

            // allow possibly duplicate CMD_STOP as it is harmless for the logic
            if (tmr->IsLinked())
            {
                m_active.Unlink(tmr);
            }
            
        break; }

        case TimerCommand::CMD_RESET:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            // only reset if still active and periodic
            if (tmr->m_active && (tmr->m_period != 0U))
            {
                STK_ASSERT(tmr->GetHead() == &m_active);

                tmr->m_deadline = cmd.timestamp + static_cast<Ticks>(tmr->m_period);
                tmr->m_pending  = false;

                next_sleep = NO_WAIT;
            }
            
        break; }

        case TimerCommand::CMD_RESTART:
        {
            // atomic stop + re-start: no precondition on current timer state
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            // unlink if currently in the active list
            if (tmr->IsLinked())
            {
                m_active.Unlink(tmr);
            }

            // re-arm with fresh parameters
            tmr->m_deadline = cmd.timestamp + static_cast<Ticks>(cmd.delay);
            tmr->m_period   = cmd.period;
            tmr->m_active   = true;
            tmr->m_pending  = false;
            tmr->m_rearming = false;

            // re-link to the back of the list
            m_active.LinkBack(tmr);
            next_sleep = NO_WAIT;
            
        break; }

        case TimerCommand::CMD_START_OR_RESET:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            // not currently active: start with supplied parameters
            if (!tmr->m_active)
            {
                STK_ASSERT(!tmr->IsLinked());

                tmr->m_deadline = cmd.timestamp + static_cast<Ticks>(cmd.delay);
                tmr->m_period   = cmd.period;
                tmr->m_active   = true;
                tmr->m_pending  = false;
                tmr->m_rearming = false;

                m_active.LinkBack(tmr);
            }
            // active and periodic: reset deadline anchored to call-site timestamp
            else if (tmr->m_period != 0U)
            {
                STK_ASSERT(tmr->GetHead() == &m_active);

                tmr->m_deadline = cmd.timestamp + static_cast<Ticks>(tmr->m_period);
                tmr->m_pending  = false;
            }
            else
            {
                // noop
            }

            // active one-shot: no action - cannot reset a one-shot mid-flight,
            // caller should use Restart() if unconditional re-arm is needed

            next_sleep = NO_WAIT;
            
        break; }

        case TimerCommand::CMD_SET_PERIOD:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);
            STK_ASSERT(cmd.period != 0U);

            // guard: only apply if still active and periodic
            if (tmr->m_active && (tmr->m_period != 0U))
            {
                STK_ASSERT(tmr->GetHead() == &m_active);

                // new period takes effect on the next reload, current deadline is
                // intentionally left unchanged so the in-flight interval is not
                // truncated or extended, caller can follow up with Reset() if
                // immediate application is required
                tmr->m_period = cmd.period;
            }
            
        break; }

        case TimerCommand::CMD_SHUTDOWN:
        {
            if (m_polled)
            {
                // no tasks to stop: stop and release all timers, the host stays usable
                Timer *tmr = util::DListCast::ListEntryToParent<Timer>(m_active.GetFirst());
                while (tmr != nullptr)
                {
                    m_active.Unlink(tmr);

                    tmr->m_active   = false;
                    tmr->m_pending  = false;
                    tmr->m_rearming = false;

                    tmr = util::DListCast::ListEntryToParent<Timer>(m_active.GetFirst());
                }
            }
            else
            {
                // wake all handler tasks with shutdown sentinels
                for (size_t i = 0U; i < STK_TIMER_THREADS_COUNT; ++i)
                {
                    STK_UNUSED(m_queue.Write(nullptr, NO_WAIT));
                }

                // signal UpdateTime() to exit its loop
                working = false;
            }
            
        break; }

        default:
        {
            STK_ASSERT(false);
            
            break; }
        }
    }

    return working;
}

// ---------------------------------------------------------------------------
// PushCommand
// ---------------------------------------------------------------------------

inline bool TimerHost::PushCommand(TimerCommand cmd)
{
    bool success = true;
    bool proceed_to_write = true;

    const bool is_rearm = (cmd.cmd == TimerCommand::CMD_RESTART) ||
                          (cmd.cmd == TimerCommand::CMD_START_OR_RESET);

    // rearm coalescing (CMD_RESTART and CMD_START_OR_RESET only)
    if (is_rearm && (cmd.timer != nullptr))
    {
        if (cmd.timer->m_rearming)
        {
            // already rearming - successfully coalesced, no need to write to queue
            proceed_to_write = false;
        }
        else
        {
            cmd.timer->m_rearming = true;

            // prevent compiler from sinking the flag write past Write()
            __stk_full_memfence();
        }
    }

    // write to command queue
    if (proceed_to_write)
    {
        if (!m_commands.Write(cmd, NO_WAIT))
        {
            // revert the flag if this was a failed rearm attempt
            if (is_rearm && (cmd.timer != nullptr))
            {
                cmd.timer->m_rearming = false;
            }

            // queue full: this indicates a usage error - more commands are being
            // issued than the tick task can drain (recoverable in
            // release, caller receives false and can retry or escalate)
            STK_ASSERT(false);
            success = false;
        }
    }
    
    return success;
}

} // namespace time
} // namespace stk

#endif /* STK_TIME_TIMER_H_ */
