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
           stk::time::Timer instances.
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

/*! \def   STK_TIMER_COMMAND_QUEUE_SIZE
    \brief Capacity of the command queue of TimerHost: the number of commands (Start(), Stop(), StartSync(), ...)
           which can wait for the tick task at once (default: 32).
    \note  It does not limit the number of timers: Timer instances are intrusive and owned by the application.
           When the queue is full a caller which can wait blocks until a slot is free, otherwise it gets
           TimerHost::RES_QUEUE_FULL, see "Command queue" in TimerHost.
    \note  For backward compatibility, if this macro is not defined but the deprecated STK_TIMER_COUNT_MAX is,
           the value of STK_TIMER_COUNT_MAX is used.
*/
#ifndef STK_TIMER_COMMAND_QUEUE_SIZE
    #ifdef STK_TIMER_COUNT_MAX
        #define STK_TIMER_COMMAND_QUEUE_SIZE STK_TIMER_COUNT_MAX
    #else
        #define STK_TIMER_COMMAND_QUEUE_SIZE 32U
    #endif
#endif

/*! \def        STK_TIMER_COUNT_MAX
    \deprecated Use STK_TIMER_COMMAND_QUEUE_SIZE instead. The old name suggested a limit on the number of timers,
                but it has always been the capacity of the command queue only (timers are intrusive and not limited
                by TimerHost). It is kept so existing build configurations keep compiling: defining it (without
                defining STK_TIMER_COMMAND_QUEUE_SIZE) sets the queue capacity, and when not defined it reads
                as STK_TIMER_COMMAND_QUEUE_SIZE. TimerHost itself does not use it.
*/
#ifndef STK_TIMER_COUNT_MAX
    #define STK_TIMER_COUNT_MAX STK_TIMER_COMMAND_QUEUE_SIZE
#endif

class TimerHost; // forward declaration (used by Timer::OnExpired)

/*! \class Timer
    \brief Abstract base class for a timer managed by TimerHost.

    To implement a concrete timer, inherit from this class and override OnExpired().
    The timer instance must outlive the TimerHost or be explicitly stopped before
    destruction, as TimerHost holds a non-owning pointer to each active timer.

    Each Timer instance may be registered with at most one TimerHost at a time.

    \note  Formerly the nested class TimerHost::Timer, which is kept as a type alias for backward compatibility.
           Being a namespace-level class, Timer can be forward-declared without the definition of TimerHost.

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
        : m_deadline(0), m_timestamp(0), m_period(0U), m_delay(0U), m_active(false),
          m_pending(false), m_rearming(false), m_ready_node(this)
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

    /*! \brief     Get the initial delay stored by the last start/restart/change command.
        \return    Delay in ticks, 0 if none was stored yet.
        \note      Used by TimerHost::ActivateSync() to (re)start the timer with its stored parameters.
    */
    uint32_t GetDelay() const { return m_delay; }

    /*! \brief     Get remaining ticks until the timer next expires.
        \return    Ticks remaining, or 0 if already expired / not active.
        \note      The value is advisory: it is computed from m_now (the last value written
                   by the tick task) and may be up to one tick task wake cycle stale.
                   If the deadline has already passed but not yet been processed, 0 is returned.
    */
    uint32_t GetRemainingTicks() const;

private:
    /*! \struct ReadyNode
        \brief  Hook of a Timer in the ready list (expired timers waiting for a handler task).

        It is a second, independent intrusive list hook of the Timer: the timer itself can stay in
        the list of active timers (periodic timer) while its node is in the ready list. A timer has
        exactly one node, so it can be queued for handling at most once and a command which cancels
        an expiration (stop, restart, ...) can unlink it in O(1).

        \note  Linked/unlinked only inside a critical section. Invariant (task mode): the node is
               linked if and only if m_pending is true.
    */
    struct ReadyNode : public util::DListEntry<ReadyNode, false>
    {
        explicit ReadyNode(Timer *owner) : timer(owner)
        {}

        Timer *const timer; //!< the timer owning this node
    };

    STK_NONCOPYABLE_CLASS(Timer);

    Ticks         m_deadline;  //!< absolute expiration time (ticks)
    Ticks         m_timestamp; //!< absolute time at which timer expired (ticks), updated by TimerHost
    uint32_t      m_period;    //!< reload period in ticks (0 = one-shot)
    uint32_t      m_delay;     //!< initial delay in ticks of the last start/restart/change (reused by CMD_ACTIVATE)
    volatile bool m_active;    //!< true if active
    volatile bool m_pending;   //!< true if pending to be handled (task mode: m_ready_node is linked)
    volatile bool m_rearming;  //!< true while a CMD_RESTART / CMD_START_OR_RESET is already in the command queue
    ReadyNode     m_ready_node;//!< hook in the ready list of the host (task mode), see ReadyNode
};

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
           The number of timers is not limited by TimerHost (STK_TIMER_COMMAND_QUEUE_SIZE is the capacity of the command queue).
           The maximum timer period is bounded by \c uint32_t (~49 days at 1 ms resolution).
    \note  Requires a kernel built with stk::KERNEL_SYNC: the command queue is stk::sync::Pipe based, the handler
           tasks are woken by a stk::sync::Event. The list of expired timers is intrusive (no capacity limit
           of its own, a Timer is queued at most once) and is protected by a critical section.
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
    class HeartbeatTimer : public stk::time::Timer
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
    class NotifyTimer : public stk::time::Timer
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
        if (g_TimerHost.Start(g_Heartbeat, period, period) != stk::time::TimerHost::RES_OK)
        {
            // e.g. RES_QUEUE_FULL (called from an ISR / before the kernel started), RES_ACTIVE, RES_NOT_READY
        }

        // start a one-shot notification after 1 second
        uint32_t delay = static_cast<uint32_t>(stk::GetTicksFromMs(1000));
        g_TimerHost.Start(g_Notify, delay);
    }
    \endcode

    \par Command queue
    Commands of both APIs reach the tick task through a queue of STK_TIMER_COMMAND_QUEUE_SIZE entries (default 32). The macro
    is the capacity of this queue only, it does not limit the number of timers, which are intrusive and owned by the
    application: size it for the burst of commands which can be issued before the tick task runs, not for the number
    of timers. (STK_TIMER_COUNT_MAX is the deprecated name of this macro.) When the queue is full:
    - a caller which can wait (task context, task mode, kernel running, not the tick task) blocks until the tick task
      has drained a slot, no command is lost and nothing is asserted. An asynchronous call is therefore non-blocking
      only while the queue has room;
    - a caller which can not wait (ISR, kernel not started, polled mode, the tick task) gets RES_QUEUE_FULL, nothing
      is queued.
    A blocking call must not be made from a critical section or with interrupts disabled. The wait ends when a slot is
    free, or fails with RES_CANCELLED only if it is cancelled (IKernel::CancelTaskWait()).

    \par Result codes
    Every command (asynchronous and synchronous) returns an EResult. Negative values are errors: the command was not
    queued and not executed. Positive values report the state of the timer (the command was refused because its
    precondition does not hold) or that the outcome is unknown:
    - RES_OK: synchronous API - the command was executed. Asynchronous API - the command was accepted into the
      command queue (or coalesced with an identical one already waiting, see Restart()); it is executed later by the
      tick task and its outcome is not reported;
    - RES_ACTIVE, RES_INACTIVE, RES_NOT_PERIODIC: the timer is not in the state the command requires. The asynchronous
      API evaluates this from the unsynchronized state of the timer, so it is advisory: nothing is queued when it
      reports a mismatch, but a command which was accepted can still be dropped by the tick task if the state
      has changed meanwhile. Use the synchronous API for an exact outcome;
    - RES_QUEUED: synchronous API only, the calling context can not wait, the command was queued asynchronously;
    - RES_QUEUE_FULL, RES_CANCELLED: the command queue is full and the caller can not wait, or the wait was cancelled;
    - RES_NOT_READY: the host is not initialized, its initialization failed, or it has been shut down;
    - RES_INVALID_PARAM: an argument is out of range (e.g. a zero or too large period of SetPeriod());
    - RES_FAIL: unspecified failure.
    \warning RES_OK is 0. A check like \c if(host.Start(...)) which was written for the former \c bool return value means
             "failed" now, compare with RES_OK explicitly.

    \par Synchronous API
    The asynchronous commands (Start(), Stop(), ...) only queue a request: the result is evaluated later by the
    tick task, the caller gets RES_OK ("queued") or an error code (e.g. RES_QUEUE_FULL if it can not wait for a
    free slot), see "Result codes". The synchronous variants (StartSync(), StopSync(),
    RestartSync(), ResetSync(), SetPeriodSync(), ActivateSync(), ChangeSync(), QuerySync()) queue the same command together with a
    completion point and block the caller until the tick task has executed it, so the returned EResult is the exact
    outcome of the command (e.g. EResult::RES_ACTIVE for a timer which was already active). They also block while
    the command queue is full instead of failing, so the number of concurrent callers is not limited by
    STK_TIMER_COMMAND_QUEUE_SIZE (see "Command queue"). If the host is not initialized, every call
    returns RES_NOT_READY.
    - A sync call waits only where waiting is possible: task context, kernel running, task mode. From an ISR, before
      the kernel is started, or in polled mode, the command is queued
      asynchronously and RES_QUEUED is returned (RES_QUEUE_FULL if the queue is full), QuerySync() returns an advisory
      snapshot instead. Callbacks may use the sync API in task mode (they run in handler tasks, the tick task never
      waits for them), in polled mode they get RES_QUEUED.
    - The tick task must be at least as urgent as every task which uses the sync API, otherwise a more urgent
      caller is blocked by a less urgent tick task (priority inversion). Configure the weights via Config.
    - A command with a completion is never coalesced (see Restart()), every caller gets its own result.
    - The wait is not abortable: if it is cancelled (IKernel::CancelTaskWait()) the call keeps waiting, the
      completion lives on the caller's stack and must outlive the tick task's access to it.
    - StopSync() (and every other command which cancels or re-arms a timer) removes an expiration which was already
      handed over to the handlers: expired timers wait in an intrusive ready list, one node per Timer, and the command
      unlinks the node before it completes. When StopSync() returns the host holds no queued reference to the Timer and
      no callback of that expiration will start. A callback which a handler task has already dequeued is not
      interrupted, StopSync() waits for it (polling every tick) before it returns, so after StopSync() the Timer
      is neither queued nor in use by the host and its storage can be released.
    - The wait for a running callback is skipped when StopSync() is called from a callback (a handler task): a
      callback which stops its own timer would wait for itself, and two callbacks stopping each other's timers
      would wait for each other. From a callback StopSync() guarantees only that no further callback of the timer
      starts. The asynchronous Stop() and the ISR / not-started / polled paths (RES_QUEUED) never wait either.
    - For ThreadX-like ordering of callbacks keep STK_TIMER_THREADS_COUNT at 1 (a single handler task runs the
      callbacks in expiration order, as the ThreadX timer thread does).

    \see Timer, TimerHost::Config, TimerHost::Process, TimerHost::InitializePolled, TimerHost::StartSync, STK_TIMER_THREADS_COUNT, STK_TIMER_HANDLER_STACK_SIZE, STK_TIMER_COMMAND_QUEUE_SIZE
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

    /*! \enum  EResult
        \brief Outcome of a command: returned by the asynchronous API (Start(), Stop(), ...) and by the synchronous
               API (StartSync() and the other *Sync() functions), see "Result codes" in TimerHost.
        \note  Negative values are errors (nothing was queued or executed), positive values report the state of the
               timer or an unknown outcome. The values of the existing codes are stable.
    */
    enum EResult : int8_t
    {
        RES_CANCELLED     = -5, //!< The wait for a free command queue slot was cancelled (IKernel::CancelTaskWait()), nothing is queued.
        RES_INVALID_PARAM = -4, //!< An argument is out of range (e.g. zero period for SetPeriod()), nothing is queued.
        RES_QUEUE_FULL    = -3, //!< The command queue is full and the caller can not wait (ISR, kernel not started, polled mode, tick task), nothing is queued.
        RES_NOT_READY     = -2, //!< Host is not initialized (or its initialization failed), or it has been shut down.
        RES_FAIL          = -1, //!< Unspecified failure.
        RES_OK            = 0,  //!< Sync: the command was executed. Async: the command was accepted into the command queue.
        RES_ACTIVE        = 1,  //!< The timer is active, but the command requires it to be inactive (start, activate, change).
        RES_INACTIVE      = 2,  //!< The timer is not active, but the command requires it to be active (stop, reset).
        RES_NOT_PERIODIC  = 3,  //!< The timer is not periodic, but the command requires it to be.
        RES_QUEUED        = 4   //!< The calling context can not wait: the command was queued asynchronously, its outcome is unknown.
    };

    /*! \struct TimerInfo
        \brief  Consistent snapshot of a timer, see QuerySync().
    */
    struct TimerInfo
    {
        bool     active;    //!< True if the timer is active.
        uint32_t remaining; //!< Ticks until the next expiration, 0 if not active or overdue.
        uint32_t delay;     //!< Initial delay stored by Start(), Restart(), ChangeSync().
        uint32_t period;    //!< Reload period, 0 for a one-shot timer.
    };

    /*! \typedef Timer
        \brief   Alias of stk::time::Timer, keeps the former nested name TimerHost::Timer valid.
        \note    Qualified on purpose and declared before any other use of the name Timer in this class:
                 an unqualified "typedef Timer Timer;" changes the meaning of Timer inside the class
                 (ill-formed, rejected by GCC).
    */
    typedef ::stk::time::Timer Timer;

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
        Weight      tick_weight;        //!< Weight of the tick task (default: DEFAULT_WEIGHT).
        Weight      handler_weight;     //!< Weight of every handler task (default: DEFAULT_WEIGHT).
        Word       *tick_stack;         //!< Stack of the tick task, or \c nullptr to use the internal buffer (default).
        size_t      tick_stack_size;    //!< Size of \a tick_stack in elements of Word (>= STK_STACK_SIZE_MIN), ignored if \a tick_stack is \c nullptr.
        Word       *handler_stack[STK_TIMER_THREADS_COUNT]; //!< Stack of each handler task, a \c nullptr entry selects the internal buffer (default).
        size_t      handler_stack_size; //!< Size of every non-null \a handler_stack entry in elements of Word (>= STK_STACK_SIZE_MIN).
        EAccessMode tick_mode;          //!< Access mode of the tick task (default: ACCESS_USER). The tick task only touches host state, but a host used from privileged code (e.g. an RTOS compatibility layer) may need ACCESS_PRIVILEGED.

        /*! \brief Default constructor: default weights, internal stack buffers and ACCESS_USER tick task.
        */
        explicit Config()
            : tick_weight(DEFAULT_WEIGHT), handler_weight(DEFAULT_WEIGHT), tick_stack(nullptr),
              tick_stack_size(0U), handler_stack(), handler_stack_size(0U), tick_mode(ACCESS_USER)
        {}
    };

    /*! \brief Default constructor. Zero-initializes all internal state.
        \note  Call Initialize() before using any other member function.
    */
    explicit TimerHost() 
      : m_task_tick_memory(), m_task_handler_memory(), m_task_tick(), m_task_process(),
        m_active(), m_ready_list(), m_ready_event(false, false), m_now(0), m_polled(false), m_ready(false),
        m_shutdown(false), m_running(), m_kernel(nullptr), m_tick_tid(TID_NONE)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~TimerHost() = default;

    /*! \brief     Initialize timer host instance.
        \param[in] kernel: Kernel to which instance will be bound.
        \param[in] mode: Access mode for the timer handling tasks which call expired timers. The access mode
                   of the tick task is Config::tick_mode (ACCESS_USER by default).
        \param[in] cfg: Weights, access mode of the tick task and (optionally) stack buffers of the tick and
                   handler tasks, by default DEFAULT_WEIGHT, ACCESS_USER and the internal stacks are used (see Config).
        \return    ADD_OK on success, otherwise the first failure reported by IKernel::AddTask(ITask *), e.g.
                   \a ADD_FAIL_POOL_FULL if the kernel has no free task slots, \a ADD_FAIL_DUPLICATE if a stack
                   is shared, or \a ADD_FAIL_INVALID (e.g. KERNEL_HRT kernel or kernel not initialized).
        \note      Must be called before Start() of a KERNEL_STATIC kernel and after the kernel was initialized.
        \note      On failure the host is unusable. Tasks added before the failure remain registered with the
                   kernel (blocked on the empty ready list), in KERNEL_DYNAMIC mode they can be released with
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
        \return    RES_OK if the command was queued, RES_ACTIVE if the timer is already active (nothing is queued),
                   RES_QUEUE_FULL, RES_CANCELLED, RES_NOT_READY (see "Result codes" in TimerHost).
    */
    EResult Start(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Stop running timer.
        \param[in] tmr: Timer instance. Must be active.
        \return    RES_OK if the command was queued, RES_INACTIVE if the timer is not active (nothing is queued),
                   RES_QUEUE_FULL, RES_CANCELLED, RES_NOT_READY.
    */
    EResult Stop(Timer &tmr);

    /*! \brief     Reset periodic timer's deadline.
        \param[in] tmr: Timer instance. Must be active and periodic.
        \return    RES_OK if the command was queued, RES_INACTIVE if the timer is not active, RES_NOT_PERIODIC if it
                   is a one-shot timer (nothing is queued in both cases), RES_QUEUE_FULL, RES_CANCELLED, RES_NOT_READY.
    */
    EResult Reset(Timer &tmr);

    /*! \brief     Atomically stop and re-start timer.
        \param[in] tmr: Timer instance (active or inactive).
        \param[in] delay: Initial delay in ticks before first expiration.
        \param[in] period: Reload period in ticks (0 is one-shot timer).
        \return    RES_OK if the command was queued (or coalesced with one already waiting), RES_QUEUE_FULL,
                   RES_CANCELLED, RES_NOT_READY.
        \note      Unlike calling Stop() followed by Start(), this operation is atomic
                   with respect to the tick task: the timer cannot fire between the implicit
                   stop and re-start, and only one command queue slot is consumed.
                   Useful for watchdog refresh and debounce reset patterns.
                   Safe to call regardless of whether the timer is currently active.
    */
    EResult Restart(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Start timer if inactive, or reset its deadline if already active and periodic.
        \param[in] tmr: Timer instance (active or inactive).
        \param[in] delay: Initial delay in ticks (used only when starting).
        \param[in] period: Reload period in ticks (used only when starting, 0 is one-shot).
        \return    RES_OK if the command was queued (or coalesced with one already waiting), RES_QUEUE_FULL,
                   RES_CANCELLED, RES_NOT_READY.
        \note      Collapses the common pattern:
                     if (timer.IsActive()) host.Reset(timer);
                     else                  host.Start(timer, delay, period);
                   into a single atomic operation, eliminating the TOCTOU race between
                   the IsActive() check and the subsequent call.
                   If the timer is active but one-shot, no action is taken (a one-shot
                   timer mid-flight cannot be reset; use Restart() instead).
    */
    EResult StartOrReset(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Change the period of a running periodic timer without affecting its current deadline.
        \param[in] tmr: Timer instance. Must be active and periodic.
        \param[in] period: New reload period in ticks. Must be non-zero.
        \return    RES_OK if the command was queued, RES_INVALID_PARAM if \a period is zero or larger than
                   WAIT_INFINITE, RES_INACTIVE if the timer is not active, RES_NOT_PERIODIC if it is a one-shot timer
                   (nothing is queued in these cases), RES_QUEUE_FULL, RES_CANCELLED, RES_NOT_READY.
        \note      The new period takes effect on the next reload after the current deadline
                   fires. To apply the new period immediately (restart from now), call
                   Reset() after SetPeriod().
    */
    EResult SetPeriod(Timer &tmr, uint32_t period);

    /*! \brief     Start timer and wait for the result (see "Synchronous API" in TimerHost).
        \param[in] tmr: Timer instance. Must not be active.
        \param[in] delay: Initial delay in ticks before first expiration, stored in the timer.
        \param[in] period: Reload period in ticks (0 is one-shot timer), stored in the timer.
        \return    RES_OK, RES_ACTIVE if the timer is already active (nothing is changed), RES_QUEUED, or an error code
                   (RES_NOT_READY, RES_QUEUE_FULL, RES_CANCELLED).
    */
    EResult StartSync(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Start timer with the delay and period stored in it and wait for the result.
        \param[in] tmr: Timer instance. Must not be active.
        \return    RES_OK, RES_ACTIVE if the timer is already active, RES_QUEUED, or an error code
                   (RES_NOT_READY, RES_QUEUE_FULL, RES_CANCELLED).
        \note      The parameters are those of the last StartSync(), Start(), Restart() or ChangeSync() (the
                   expiration is relative to the moment of the call).
    */
    EResult ActivateSync(Timer &tmr);

    /*! \brief     Stop timer and wait for the result.
        \param[in] tmr: Timer instance.
        \return    RES_OK if it was active, RES_INACTIVE if it was not active (a one-shot which already expired,
                   its queued callback is cancelled if it has not started yet), RES_QUEUED, or an error code
                   (RES_NOT_READY, RES_QUEUE_FULL, RES_CANCELLED).
        \note      Also waits until a callback of this timer which a handler task has already started has returned,
                   after RES_OK / RES_INACTIVE the Timer storage may be released.
        \warning   Called from an OnExpired() callback it does not wait for running callbacks (deadlock guard), see
                   "Synchronous API" in TimerHost. Do not release the Timer from a callback of another timer.
    */
    EResult StopSync(Timer &tmr);

    /*! \brief     Atomically stop and re-start timer and wait for the result.
        \param[in] tmr: Timer instance (active or inactive).
        \param[in] delay: Initial delay in ticks before first expiration, stored in the timer.
        \param[in] period: Reload period in ticks (0 is one-shot timer), stored in the timer.
        \return    RES_OK, RES_QUEUED, or an error code (RES_NOT_READY, RES_QUEUE_FULL, RES_CANCELLED).
    */
    EResult RestartSync(Timer &tmr, uint32_t delay, uint32_t period = 0);

    /*! \brief     Reset periodic timer's deadline and wait for the result.
        \param[in] tmr: Timer instance. Must be active and periodic.
        \return    RES_OK, RES_INACTIVE, RES_NOT_PERIODIC, RES_QUEUED, or an error code (RES_NOT_READY, RES_QUEUE_FULL,
                   RES_CANCELLED).
    */
    EResult ResetSync(Timer &tmr);

    /*! \brief     Change the period of a running periodic timer without affecting its current deadline and
                   wait for the result.
        \param[in] tmr: Timer instance. Must be active and periodic.
        \param[in] period: New reload period in ticks. Must be non-zero.
        \return    RES_OK, RES_INACTIVE if the timer is not active, RES_NOT_PERIODIC if it is a one-shot timer
                   (nothing is changed in both cases), RES_QUEUED, RES_INVALID_PARAM if \a period is zero or larger
                   than WAIT_INFINITE (nothing is queued), or an error code (RES_NOT_READY, RES_QUEUE_FULL, RES_CANCELLED).
        \note      The new period takes effect on the next reload after the current deadline fires. To apply it
                   immediately (restart from now), call ResetSync() after SetPeriodSync().
        \note      Unlike SetPeriod(), the state of the timer is evaluated by the tick task, so the outcome is exact
                   and a timer which is stopped or re-armed by a command queued earlier is judged correctly.
    */
    EResult SetPeriodSync(Timer &tmr, uint32_t period);

    /*! \brief     Change the delay and period of an inactive timer and wait for the result.
        \param[in] tmr: Timer instance. Must not be active.
        \param[in] delay: New initial delay in ticks.
        \param[in] period: New reload period in ticks (0 is one-shot timer).
        \return    RES_OK, RES_ACTIVE if the timer is active (nothing is changed), RES_QUEUED, or an error code
                   (RES_NOT_READY, RES_QUEUE_FULL, RES_CANCELLED).
        \note      The timer is not started, the values are used by the next ActivateSync().
    */
    EResult ChangeSync(Timer &tmr, uint32_t delay, uint32_t period);

    /*! \brief      Get a consistent snapshot of the timer (evaluated by the tick task).
        \param[in]  tmr: Timer instance.
        \param[out] info: Snapshot, valid if the result is RES_OK.
        \return     RES_OK, RES_NOT_READY if the host is not initialized or has been shut down.
        \note       From a context which can not wait (see "Synchronous API" in TimerHost) the snapshot is
                    read without synchronization and is advisory (RES_OK is returned).
    */
    EResult QuerySync(Timer &tmr, TimerInfo &info);

    /*! \brief     Check whether a task id belongs to a task of this host (tick or handler task).
        \param[in] tid: Task id, e.g. from GetTid().
        \return    True in task mode if \a tid is the tick task or one of the handler tasks, i.e. the caller is
                   running TimerHost code or an OnExpired() callback. Always false in polled mode.
        \note      Lets an application layer tell its own tasks from the host's tasks, e.g. to map GetTid()
                   to its own thread object.
    */
    bool IsHostTid(TId tid)
    {
        bool result = false;

        if (!m_polled && (tid != TID_NONE))
        {
            result = (tid == GetTidFromUserTask(&m_task_tick));

            for (size_t i = 0U; (i < STK_TIMER_THREADS_COUNT) && !result; ++i)
            {
                result = (tid == GetTidFromUserTask(&m_task_process[i]));
            }
        }

        return result;
    }

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
        \return   RES_OK if the command was queued, RES_QUEUE_FULL, RES_CANCELLED, RES_NOT_READY (already shut down
                  or not initialized).
        \note     Task mode: the tick and handler tasks exit. Polled mode: the host stays usable.
    */
    EResult Shutdown();

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

    /*! \struct Completion
        \brief  Completion point of a synchronous command, lives on the stack of the waiting caller.

        The tick task writes \a result and \a info, then calls \a done.Set() as its very last access:
        after that the caller may return and destroy the object.
    */
    struct Completion
    {
        explicit Completion() : done(false, false), result(RES_FAIL), info()
        {}

        sync::Event      done;   //!< auto-reset event, signaled when the command has been executed
        volatile EResult result; //!< outcome of the command
        TimerInfo        info;   //!< snapshot (CMD_QUERY only)
    };

    /*! \struct TimerCommand
        \brief  POD command record passed from the public API methods to the tick task via the command queue.

        Each command encodes a single operation together with the timer pointer, a call-site
        timestamp, and optional delay/period arguments. Commands are produced by the public
        API (Start, Stop, Reset, …) and consumed exclusively by the tick task inside
        ProcessCommands(). A command issued by a synchronous API function also carries the
        completion point of the waiting caller.
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
            CMD_SET_PERIOD,     //!< change period of a running periodic timer
            CMD_ACTIVATE,       //!< start with the delay and period stored in the timer
            CMD_CHANGE,         //!< store delay and period of an inactive timer (does not start it)
            CMD_QUERY           //!< fill TimerInfo of the timer (synchronous only)
        };

        EId         cmd;        //!< command id
        Timer      *timer;      //!< timer instance
        Ticks       timestamp;  //!< hw tick count captured at call site
        uint32_t    delay;      //!< initial delay ticks (CMD_START / CMD_RESTART / CMD_START_OR_RESET / CMD_CHANGE)
        uint32_t    period;     //!< reload period ticks (CMD_START / CMD_RESTART / CMD_START_OR_RESET / CMD_SET_PERIOD / CMD_CHANGE)
        Completion *completion; //!< completion point of the synchronous caller, \c nullptr for an asynchronous command
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

    /*! \brief  Handler task body: dequeues expired timers from the ready list and invokes their OnExpired() callbacks.
        \note   Runs in each handler task context. Blocks on the ready event while the list is empty and exits
                once the list is drained after CMD_SHUTDOWN (m_shutdown).
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
        \param[in] wait: Maximum ticks to wait for a free queue slot: NO_WAIT (default) does not wait unless the
                   queue is full and the caller can wait (see CanWait()), WAIT_INFINITE blocks until the tick task
                   has drained at least one command.
        \return    RES_OK if the command was queued (or coalesced), RES_NOT_READY if the host has no consumer of
                   the queue, RES_QUEUE_FULL if the queue is full and the caller can not wait, RES_CANCELLED if the
                   wait for a free slot was cancelled.
        \note      May be called from any context. When the queue is full a caller which can wait blocks until a
                   slot is free (see "Command queue" in TimerHost), otherwise it receives RES_QUEUE_FULL.
        \warning   A blocking push must not be issued from the context which drains the queue (the tick task,
                   Process() and callbacks run by it), see CanWait().
    */
    EResult PushCommand(TimerCommand cmd, Timeout wait = NO_WAIT);

    /*! \brief     Check whether the calling context can wait for the completion of a command.
        \return    True in task mode, from a task (not ISR, not the tick task), with the kernel running and
                   the host initialized.
    */
    bool CanWait() const;

    /*! \brief     Cancel a pending (expired, not yet handled) expiration of the timer.
        \param[in] tmr: Timer instance.
        \details   Clears Timer::m_pending and, in task mode, unlinks the timer's node from the ready list, so no
                   handler task can dequeue it afterwards and the host holds no queued reference to \a tmr.
                   Atomic with the dequeue done by a handler task: if the handler got there first, the callback
                   is already claimed and runs (it is not interrupted).
        \note      Tick task only (commands) and in polled mode. Takes a critical section in task mode.
    */
    void CancelPending(Timer &tmr);

    /*! \brief     Wait until no handler task is executing the callback of the timer.
        \param[in] tmr: Timer instance, already stopped (CancelPending() done by the tick task), so no new
                   callback of it can start and the wait is bounded by the callback which is running now.
        \details   Polls the running slots every tick (Sleep(1)), lock-free: it takes no critical section.
        \note      Does nothing when called from a handler task of this host (an OnExpired() callback): a callback
                   which stops its own timer would wait for itself, and two callbacks stopping each other's timers
                   would wait for each other. The guarantee therefore does not apply to a stop issued from a callback.
        \warning   Task context with the kernel running only (see CanWait()).
    */
    void WaitCallbackDone(Timer &tmr);

    /*! \brief      Execute a command synchronously, or queue it if the caller can not wait.
        \param[in]  id: Command id.
        \param[in]  tmr: Target timer.
        \param[in]  delay: Delay argument of the command.
        \param[in]  period: Period argument of the command.
        \param[out] info: Snapshot for CMD_QUERY, otherwise \c nullptr.
        \return     Outcome of the command, RES_QUEUED or an error code.
    */
    EResult Execute(TimerCommand::EId id, Timer &tmr, uint32_t delay, uint32_t period, TimerInfo *info);

    /*! \typedef TaskTickMemory
        \brief   Stack memory type for the single tick task.
    */
    typedef StackMemoryDef<TASK_TICK_MEMORY_SIZE>::Type    TaskTickMemory;

    /*! \typedef TimerHostMemory
        \brief   Stack memory type for each handler task.
    */
    typedef StackMemoryDef<TASK_HANDLER_STACK_SIZE>::Type  TimerHostMemory;

    /*! \typedef ReadyList
        \brief   Intrusive list of the expired timers (via Timer::m_ready_node) handed over from the tick task
                 to the handler tasks. Protected by a critical section.
    */
    typedef util::DListHead<Timer::ReadyNode, false>       ReadyList;

    /*! \typedef CommandQueue
        \brief   Lock-free pipe used to send TimerCommand records from API callers to the tick task.
    */
    typedef sync::PipeT<TimerCommand, STK_TIMER_COMMAND_QUEUE_SIZE> CommandQueue;

    TaskTickMemory                m_task_tick_memory;                             //!< tick task memory
    TimerHostMemory               m_task_handler_memory[STK_TIMER_THREADS_COUNT]; //!< handler task memory
    TimerWorkerTask               m_task_tick;                                    //!< timer task
    TimerWorkerTask               m_task_process[STK_TIMER_THREADS_COUNT];        //!< handler tasks
    util::DListHead<Timer, false> m_active;                                       //!< active timers (tick task only)
    ReadyList                     m_ready_list;                                   //!< expired timers waiting for a handler task (access only inside a critical section)
    sync::Event                   m_ready_event;                                  //!< auto-reset event which wakes a handler task when m_ready_list gets an entry or on shutdown
    CommandQueue                  m_commands;                                     //!< command queue
    Ticks                         m_now;                                          //!< last known current time (ticks)
    bool                          m_polled;                                       //!< true in polled mode (InitializePolled()), no tasks, callbacks run inline from Process()
    volatile bool                 m_ready;                                        //!< true while the host has a consumer of the command queue (initialized and not shut down)
    volatile bool                 m_shutdown;                                     //!< true once CMD_SHUTDOWN was received (task mode): handlers exit after draining m_ready_list
    Timer *volatile               m_running[STK_TIMER_THREADS_COUNT];             //!< timers whose callback is being executed by a handler task (one slot per handler: set inside the claim critical section, cleared and polled lock-free), see WaitCallbackDone()
    IKernel                      *m_kernel;                                       //!< kernel of the task mode (nullptr in polled mode)
    volatile TId                  m_tick_tid;                                     //!< id of the tick task (TID_NONE until it runs), commands must not be waited for from it
};

// ---------------------------------------------------------------------------
// Timer::GetRemainingTicks
// ---------------------------------------------------------------------------

inline uint32_t Timer::GetRemainingTicks() const
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

    m_polled   = false; // task mode (selects the mode again if the host was used in polled mode before)
    m_ready    = false; // accepts commands only once both tasks have been added
    m_shutdown = false; // handlers keep running (a re-initialization after Shutdown())
    m_kernel = kernel;

    EAddResult result = ADD_OK;

    // handler tasks first: they only wait on the (empty) ready list until the tick task is added
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

        m_task_tick.Initialize(this, stack, size, cfg.tick_mode, cfg.tick_weight, [](TimerHost *host)
        {
            host->UpdateTime();
        });
        result = kernel->AddTask(&m_task_tick);
    }

    // commands are accepted from now on (the tick task is added, it drains the queue once it is scheduled)
    m_ready = (result == ADD_OK);

    return result;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::Start(Timer &tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(delay <= static_cast<uint32_t>(WAIT_INFINITE));
    STK_ASSERT((period == 0U) || (period <= static_cast<uint32_t>(WAIT_INFINITE)));

    EResult result;

    // timer must not already be active (advisory check, the tick task evaluates the exact state)
    if (!tmr.m_active)
    {
        result = PushCommand({
            .cmd        = TimerCommand::CMD_START,
            .timer      = &tmr,
            .timestamp  = GetTicks(),
            .delay      = delay,
            .period     = period,
            .completion = nullptr
        });
    }
    else
    {
        result = RES_ACTIVE;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::Stop(Timer &tmr)
{
    EResult result;

    // timer must be active (advisory check, the tick task evaluates the exact state)
    if (tmr.m_active)
    {
        result = PushCommand({
            .cmd        = TimerCommand::CMD_STOP,
            .timer      = &tmr,
            .timestamp  = 0LL,
            .delay      = 0U,
            .period     = 0U,
            .completion = nullptr
        });
    }
    else
    {
        result = RES_INACTIVE;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::Reset(Timer &tmr)
{
    EResult result;

    // timer must be active and periodic
    if (!tmr.m_active)
    {
        result = RES_INACTIVE;
    }
    else if (tmr.m_period == 0U)
    {
        result = RES_NOT_PERIODIC;
    }
    else
    {
        result = PushCommand({
            .cmd        = TimerCommand::CMD_RESET,
            .timer      = &tmr,
            .timestamp  = GetTicks(),
            .delay      = 0U,
            .period     = 0U,
            .completion = nullptr
        });
    }

    return result;
}

// ---------------------------------------------------------------------------
// Restart
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::Restart(Timer &tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(delay <= static_cast<uint32_t>(WAIT_INFINITE));
    STK_ASSERT((period == 0U) || (period <= static_cast<uint32_t>(WAIT_INFINITE)));

    return PushCommand({
            .cmd        = TimerCommand::CMD_RESTART,
            .timer      = &tmr,
            .timestamp  = GetTicks(),
            .delay      = delay,
            .period     = period,
            .completion = nullptr
        });
}

// ---------------------------------------------------------------------------
// StartOrReset
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::StartOrReset(Timer &tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(delay <= static_cast<uint32_t>(WAIT_INFINITE));
    STK_ASSERT((period == 0U) || (period <= static_cast<uint32_t>(WAIT_INFINITE)));

    return PushCommand({
            .cmd        = TimerCommand::CMD_START_OR_RESET,
            .timer      = &tmr,
            .timestamp  = GetTicks(),
            .delay      = delay,
            .period     = period,
            .completion = nullptr
        });
}

// ---------------------------------------------------------------------------
// SetPeriod
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::SetPeriod(Timer &tmr, uint32_t period)
{
    EResult result;

    // period == 0 is rejected: it would silently convert a periodic timer
    // to one-shot semantics, which is better expressed via Stop() + Start()
    if ((period == 0U) || (period > static_cast<uint32_t>(WAIT_INFINITE)))
    {
        result = RES_INVALID_PARAM;
    }
    else if (!tmr.m_active)
    {
        result = RES_INACTIVE;
    }
    else if (tmr.m_period == 0U)
    {
        result = RES_NOT_PERIODIC;
    }
    else
    {
        result = PushCommand({
            .cmd        = TimerCommand::CMD_SET_PERIOD,
            .timer      = &tmr,
            .timestamp  = 0LL,
            .delay      = 0U,
            .period     = period,
            .completion = nullptr
        });
    }

    return result;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::Shutdown()
{
    return PushCommand({
        .cmd        = TimerCommand::CMD_SHUTDOWN,
        .timer      = nullptr,
        .timestamp  = 0LL,
        .delay      = 0U,
        .period     = 0U,
        .completion = nullptr
    });
}

// ---------------------------------------------------------------------------
// UpdateTime  (tick task body)
// ---------------------------------------------------------------------------

inline void TimerHost::UpdateTime()
{
    Timeout next_sleep = NO_WAIT;

    // synchronous callers must never wait from this task (it is the one which completes their commands)
    m_tick_tid = GetTid();

    while (ProcessCommands(next_sleep))
    {
        next_sleep = ScanTimers(GetTicks());
    }

    // no consumer of the command queue any more: synchronous calls fail instead of waiting forever
    m_ready = false;

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
                    // hand over to a handler task, must never block: a blocked tick task would stop completing
                    // commands, while a handler (callback) may itself be waiting for a synchronous command.
                    // The timer owns its node, so this can not fail (no queue to overflow): m_pending was false,
                    // therefore the node is not linked (see Timer::ReadyNode)
                    else
                    {
                        {
                            hw::CriticalSection::ScopedLock cs;

                            STK_ASSERT(!tmr->m_ready_node.IsLinked());
                            m_ready_list.LinkBack(&tmr->m_ready_node);
                        }

                        STK_UNUSED(m_ready_event.Set());
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
    m_kernel = nullptr;
    m_ready  = true;
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
    bool keep_running = true;

    while (keep_running)
    {
        Timer *tmr  = nullptr;
        bool   wait = false;
        size_t slot = STK_TIMER_THREADS_COUNT; // running slot of the claimed timer

        // dequeue and claim the expiration in one critical section: a command which cancels it
        // (CancelPending) either runs before (nothing to dequeue) or after (too late, the callback runs)
        {
            hw::CriticalSection::ScopedLock cs;

            if (!m_ready_list.IsEmpty())
            {
                Timer *const queued = util::DListCast::ListEntryToParent<Timer::ReadyNode>(m_ready_list.PopFront())->timer;

                // the node is linked only while pending, kept as a guard for the invariant
                if (queued->m_pending)
                {
                    queued->m_pending = false;
                    tmr = queued;

                    // publish "callback running" in the same critical section as the claim: a stop executed
                    // afterwards (CancelPending) is guaranteed to see it in WaitCallbackDone(). Each handler
                    // owns at most one slot, so a free one always exists.
                    for (size_t i = 0U; (i < STK_TIMER_THREADS_COUNT) && (slot == STK_TIMER_THREADS_COUNT); ++i)
                    {
                        if (m_running[i] == nullptr)
                        {
                            slot = i;
                        }
                    }

                    STK_ASSERT(slot < STK_TIMER_THREADS_COUNT);

                    if (slot < STK_TIMER_THREADS_COUNT)
                    {
                        m_running[slot] = tmr;
                    }
                }
            }
            else if (m_shutdown)
            {
                keep_running = false;
            }
            else
            {
                wait = true;
            }
        }

        if (tmr != nullptr)
        {
            // the timer must not be touched after the callback: it may be stopped and released by it
            // (or by a task waiting in StopSync() which is released by the slot cleared below)
            tmr->OnExpired(this);

            if (slot < STK_TIMER_THREADS_COUNT)
            {
                // no critical section: an aligned pointer store is atomic, the slot is owned by this handler
                // (a concurrent free-slot search skips it or finds it free, either is correct)
                __stk_full_memfence();
                m_running[slot] = nullptr;
                __stk_full_memfence();
            }
        }
        else if (!keep_running)
        {
            // shutdown: wake the next handler (the event is auto-reset), it drains the list and exits too
            STK_UNUSED(m_ready_event.Set());
        }
        else if (wait)
        {
            // nothing to handle: sleep until the tick task hands over an expiration or shuts down. The event
            // keeps its signaled state if Set() happened before this call, so no wake-up is lost. A spurious
            // wake-up only repeats the loop.
            if (m_ready_event.WaitEx(WAIT_INFINITE) == WAIT_RESULT_FAIL)
            {
                Sleep(1); // not expected, do not spin
            }
        }
        else
        {
            // dequeued a cancelled entry, look at the list again
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

        // outcome reported to a synchronous caller (ignored for an asynchronous command)
        EResult   res  = RES_OK;
        TimerInfo info = {};

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
                tmr->m_delay    = cmd.delay;
                tmr->m_period   = cmd.period;
                tmr->m_active   = true;
                CancelPending(*tmr); // drops an expiration of a previous run which was not handled yet

                m_active.LinkBack(tmr);
                next_sleep = NO_WAIT;
            }
            else
            {
                res = RES_ACTIVE;
            }
            
        break; }

        case TimerCommand::CMD_ACTIVATE:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            // start with the delay and period stored in the timer
            if (!tmr->m_active)
            {
                STK_ASSERT(!tmr->IsLinked());

                tmr->m_deadline = cmd.timestamp + static_cast<Ticks>(tmr->m_delay);
                tmr->m_active   = true;
                CancelPending(*tmr);

                m_active.LinkBack(tmr);
                next_sleep = NO_WAIT;
            }
            else
            {
                res = RES_ACTIVE;
            }

        break; }

        case TimerCommand::CMD_STOP:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            res = (tmr->m_active ? RES_OK : RES_INACTIVE);

            tmr->m_active = false;

            // unlinks an expiration which is queued for a handler but was not started yet: once this command
            // completed no queued reference to the timer is left (a callback already running is not affected)
            CancelPending(*tmr);

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
                CancelPending(*tmr);

                next_sleep = NO_WAIT;
            }
            else
            {
                res = (tmr->m_active ? RES_NOT_PERIODIC : RES_INACTIVE);
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
            tmr->m_delay    = cmd.delay;
            tmr->m_period   = cmd.period;
            tmr->m_active   = true;
            tmr->m_rearming = false;
            CancelPending(*tmr);

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
                tmr->m_delay    = cmd.delay;
                tmr->m_period   = cmd.period;
                tmr->m_active   = true;
                tmr->m_rearming = false;
                CancelPending(*tmr);

                m_active.LinkBack(tmr);
            }
            // active and periodic: reset deadline anchored to call-site timestamp
            else if (tmr->m_period != 0U)
            {
                STK_ASSERT(tmr->GetHead() == &m_active);

                tmr->m_deadline = cmd.timestamp + static_cast<Ticks>(tmr->m_period);
                CancelPending(*tmr);
            }
            else
            {
                res = RES_NOT_PERIODIC;
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
            else
            {
                res = (tmr->m_active ? RES_NOT_PERIODIC : RES_INACTIVE);
            }
            
        break; }

        case TimerCommand::CMD_CHANGE:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            // parameters of an active timer can not be changed (stop it first), the timer is not started here
            if (!tmr->m_active)
            {
                tmr->m_delay  = cmd.delay;
                tmr->m_period = cmd.period;
            }
            else
            {
                res = RES_ACTIVE;
            }

        break; }

        case TimerCommand::CMD_QUERY:
        {
            Timer *const tmr = cmd.timer;
            STK_ASSERT(tmr != nullptr);

            // evaluated by the tick task, so all fields belong to the same moment
            info.active    = tmr->m_active;
            info.delay     = tmr->m_delay;
            info.period    = tmr->m_period;
            info.remaining = 0U;

            if (tmr->m_active)
            {
                const Ticks left = tmr->m_deadline - GetTicks();

                if (left > 0)
                {
                    info.remaining = static_cast<uint32_t>(left);
                }
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
                // handler tasks drain the ready list and exit: the flag is published before the wake-up,
                // every exiting handler wakes the next one (the event is auto-reset)
                m_shutdown = true;
                __stk_full_memfence();

                STK_UNUSED(m_ready_event.Set());

                // signal UpdateTime() to exit its loop
                working = false;
            }
            
        break; }

        default:
        {
            STK_ASSERT(false);
            res = RES_FAIL;
            
            break; }
        }

        // complete a synchronous command: the result is published first, Set() is the last access to the
        // completion because the waiting caller owns it (stack) and may return right after it is woken
        if (cmd.completion != nullptr)
        {
            Completion *const completion = cmd.completion;

            completion->info   = info;
            completion->result = res;
            __stk_full_memfence();

            STK_UNUSED(completion->done.Set());
        }
    }

    return working;
}

// ---------------------------------------------------------------------------
// PushCommand
// ---------------------------------------------------------------------------

inline TimerHost::EResult TimerHost::PushCommand(TimerCommand cmd, Timeout wait)
{
    EResult result = RES_OK;

    if (!m_ready)
    {
        // not initialized, initialization failed or shut down: nobody consumes the command queue
        result = RES_NOT_READY;
    }
    else
    {
        bool proceed_to_write = true;

        const bool is_rearm = (cmd.cmd == TimerCommand::CMD_RESTART) ||
                              (cmd.cmd == TimerCommand::CMD_START_OR_RESET);

        // rearm coalescing (CMD_RESTART and CMD_START_OR_RESET of asynchronous callers only: a synchronous
        // caller needs its own result, its command is never merged into another one)
        if (is_rearm && (cmd.timer != nullptr) && (cmd.completion == nullptr))
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
            bool blocking = (wait == WAIT_INFINITE);
            bool written  = m_commands.Write(cmd, wait);

            // queue full: a caller which can wait blocks until the tick task has drained a slot instead of failing,
            // the tick task never waits for a caller, so the wait ends. Checked only now to keep the fast path short.
            if (!written && (wait == NO_WAIT) && CanWait())
            {
                blocking = true;
                written  = m_commands.Write(cmd, WAIT_INFINITE);
            }

            if (!written)
            {
                // revert the flag if this was a failed rearm attempt
                if (is_rearm && (cmd.timer != nullptr) && (cmd.completion == nullptr))
                {
                    cmd.timer->m_rearming = false;
                }

                // an unbounded wait fails only if it was cancelled, otherwise the queue is full and the caller
                // can not wait (ISR, kernel not running, polled mode, tick task): it may retry or escalate
                result = (blocking ? RES_CANCELLED : RES_QUEUE_FULL);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// CanWait / Execute  (synchronous commands)
// ---------------------------------------------------------------------------

inline bool TimerHost::CanWait() const
{
    // Task mode only (polled mode has no tick task to complete the command), from a task which is not the tick
    // task (the tick task completes the commands), not from an ISR, with the kernel scheduling (otherwise
    // nobody runs the tick task).
    return (m_ready && !m_polled && (m_kernel != nullptr) && !hw::IsInsideISR() &&
            (m_kernel->GetState() == IKernel::KSTATE_RUNNING) && (GetTid() != m_tick_tid));
}

inline void TimerHost::CancelPending(Timer &tmr)
{
    if (m_polled)
    {
        // callbacks run inline from Process(), nothing is ever queued
        tmr.m_pending = false;
    }
    else
    {
        // atomic with the claim done by a handler task (ProcessTimers): either the handler has dequeued the
        // timer already (it runs the callback, too late to cancel) or it finds nothing and never sees the timer
        hw::CriticalSection::ScopedLock cs;

        if (tmr.m_ready_node.IsLinked())
        {
            m_ready_list.Unlink(&tmr.m_ready_node);
        }

        tmr.m_pending = false;
    }
}

inline void TimerHost::WaitCallbackDone(Timer &tmr)
{
    // a callback must not wait (self-stop and mutual-stop deadlock), the caller of a stop from a callback
    // gets the "no new callback starts" guarantee only
    if (!IsHostTid(GetTid()))
    {
        bool running = true;

        while (running)
        {
            running = false;

            // no critical section: the slots are aligned volatile pointers (atomic reads), the claim which
            // set a slot happened before the stop completed, and the timer can not be claimed again
            __stk_full_memfence();

            for (size_t i = 0U; i < STK_TIMER_THREADS_COUNT; ++i)
            {
                if (m_running[i] == &tmr)
                {
                    running = true;
                }
            }

            if (running)
            {
                Sleep(1);
            }
        }
    }
}

inline TimerHost::EResult TimerHost::Execute(TimerCommand::EId id, Timer &tmr, uint32_t delay,
    uint32_t period, TimerInfo *info)
{
    STK_ASSERT(delay <= static_cast<uint32_t>(WAIT_INFINITE));
    STK_ASSERT((period == 0U) || (period <= static_cast<uint32_t>(WAIT_INFINITE)));

    EResult result = RES_NOT_READY;

    if (!m_ready)
    {
        // not initialized, initialization failed or shut down: nobody consumes the command queue
    }
    else if (CanWait())
    {
        // the completion is owned by this frame: the tick task makes its last access with Set()
        Completion completion;

        const EResult queued = PushCommand({
            .cmd        = id,
            .timer      = &tmr,
            .timestamp  = GetTicks(),
            .delay      = delay,
            .period     = period,
            .completion = &completion
        }, WAIT_INFINITE);

        if (queued == RES_OK)
        {
            // keep waiting if the wait is cancelled (IKernel::CancelTaskWait()): returning would leave
            // the tick task with a pointer to a dead stack frame
            EWaitResult wr;
            do
            {
                wr = completion.done.WaitEx(WAIT_INFINITE);

                if (wr == WAIT_RESULT_FAIL)
                {
                    Sleep(1); // not expected, do not spin
                }
            }
            while (wr != WAIT_RESULT_SIGNAL);

            result = completion.result;

            if (info != nullptr)
            {
                *info = completion.info;
            }

            // a stop completes only when the callback which a handler already started (if any) has returned,
            // the timer was unlinked from the ready list by the tick task so no other callback can start
            if ((id == TimerCommand::CMD_STOP) && ((result == RES_OK) || (result == RES_INACTIVE)))
            {
                WaitCallbackDone(tmr);
            }
        }
        else
        {
            // nothing was queued (e.g. the wait for a free slot was cancelled)
            result = queued;
        }
    }
    else if (id == TimerCommand::CMD_QUERY)
    {
        // can not wait (ISR, kernel not started, polled mode, ...): advisory snapshot
        if (info != nullptr)
        {
            info->active    = tmr.m_active;
            info->delay     = tmr.m_delay;
            info->period    = tmr.m_period;
            info->remaining = tmr.GetRemainingTicks();
        }

        result = RES_OK;
    }
    else
    {
        // can not wait: queue it asynchronously, the outcome is unknown to the caller
        const EResult queued = PushCommand({
            .cmd        = id,
            .timer      = &tmr,
            .timestamp  = GetTicks(),
            .delay      = delay,
            .period     = period,
            .completion = nullptr
        });

        result = ((queued == RES_OK) ? RES_QUEUED : queued);
    }

    return result;
}

inline TimerHost::EResult TimerHost::StartSync(Timer &tmr, uint32_t delay, uint32_t period)
{
    return Execute(TimerCommand::CMD_START, tmr, delay, period, nullptr);
}

inline TimerHost::EResult TimerHost::ActivateSync(Timer &tmr)
{
    return Execute(TimerCommand::CMD_ACTIVATE, tmr, 0U, 0U, nullptr);
}

inline TimerHost::EResult TimerHost::StopSync(Timer &tmr)
{
    return Execute(TimerCommand::CMD_STOP, tmr, 0U, 0U, nullptr);
}

inline TimerHost::EResult TimerHost::RestartSync(Timer &tmr, uint32_t delay, uint32_t period)
{
    return Execute(TimerCommand::CMD_RESTART, tmr, delay, period, nullptr);
}

inline TimerHost::EResult TimerHost::ResetSync(Timer &tmr)
{
    return Execute(TimerCommand::CMD_RESET, tmr, 0U, 0U, nullptr);
}

inline TimerHost::EResult TimerHost::SetPeriodSync(Timer &tmr, uint32_t period)
{
    // period == 0 is rejected as in SetPeriod(): it would silently convert a periodic timer to one-shot
    // semantics (and CMD_SET_PERIOD asserts on it), the activity of the timer is judged by the tick task
    if ((period == 0U) || (period > static_cast<uint32_t>(WAIT_INFINITE)))
    {
        return RES_INVALID_PARAM;
    }

    return Execute(TimerCommand::CMD_SET_PERIOD, tmr, 0U, period, nullptr);
}

inline TimerHost::EResult TimerHost::ChangeSync(Timer &tmr, uint32_t delay, uint32_t period)
{
    return Execute(TimerCommand::CMD_CHANGE, tmr, delay, period, nullptr);
}

inline TimerHost::EResult TimerHost::QuerySync(Timer &tmr, TimerInfo &info)
{
    return Execute(TimerCommand::CMD_QUERY, tmr, 0U, 0U, &info);
}

} // namespace time
} // namespace stk

#endif /* STK_TIME_TIMER_H_ */
