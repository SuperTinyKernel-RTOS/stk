/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
    \brief Implementation of TimerHost, a software timer multiplexer that manages multiple Timer instances.
*/

namespace stk {
namespace time {

/*! \def   STK_TIMER_THREADS_COUNT
    \brief Number of threads handling timers in TimerHost (default: 1).
*/
#ifndef STK_TIMER_THREADS_COUNT
    #define STK_TIMER_THREADS_COUNT 1
#endif

/*! \def   STK_TIMER_HANDLER_STACK_SIZE
    \brief Stack size of the timer handler, increase if your timers consume more (default: 256).
*/
#ifndef STK_TIMER_HANDLER_STACK_SIZE
    #define STK_TIMER_HANDLER_STACK_SIZE 256
#endif

/*! \def   STK_TIMER_COUNT_MAX
    \brief Max number of timers handled by TimerHost (default: 32).
*/
#ifndef STK_TIMER_COUNT_MAX
    #define STK_TIMER_COUNT_MAX 32
#endif

/*! \class TimerHost
    \brief Software timer multiplexer that manages multiple Timer instances on top of a small
           fixed set of kernel tasks.

    TimerHost internally runs two categories of tasks:
    - One \e tick task that maintains the active timer list, evaluates deadlines every wake
      cycle, and queues expired timers for dispatch.
    - One or more \e handler tasks (see \a STK_TIMER_THREADS_COUNT) that dequeue expired
      timers and invoke their OnExpired() callbacks.

    All timers share the same tick and handler tasks, so the total kernel task overhead is
    constant regardless of how many timers are active.

    Two timer modes are supported:
    - <b>One-shot</b>: fires once after \a delay ticks and becomes inactive automatically.
    - <b>Periodic</b>: fires every \a period ticks until explicitly stopped.

    \note  TimerHost must be initialized before use by calling Initialize().
           The maximum number of concurrently active timers is STK_TIMER_COUNT_MAX (default: 32).
           The maximum timer period is bounded by \c uint32_t (~49 days at 1 ms resolution).

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
        // initialize the host, must be called before Start()
        g_TimerHost.Initialize(kernel, stk::ACCESS_USER);

        // start 500 ms periodic heartbeat
        uint32_t period = stk::GetTicksFromMsec(500);
        g_TimerHost.Start(g_Heartbeat, period, period);

        // start a one-shot notification after 1 second
        uint32_t delay = stk::GetTicksFromMsec(1000);
        g_TimerHost.Start(g_Notify, delay);
    }
    \endcode

    \see TimerHost::Timer, STK_TIMER_THREADS_COUNT, STK_TIMER_HANDLER_STACK_SIZE, STK_TIMER_COUNT_MAX
*/
class TimerHost
{
public:
    enum EConsts
    {
        //!< total number of tasks serving this instance
        TASK_COUNT = (1 + STK_TIMER_THREADS_COUNT),

        //!< stack memory size of the tick task
        TASK_TICK_MEMORY_SIZE = STK_MAX(256, STK_STACK_SIZE_MIN),

        //!< stack memory size of the timer handler task
        TASK_HANDLER_STACK_SIZE = STK_MAX(STK_TIMER_HANDLER_STACK_SIZE, STK_STACK_SIZE_MIN)
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

    public:
        Timer() : m_deadline(0), m_timestamp(0), m_period(0), m_active(false), m_pending(false)
        {}

        virtual void OnExpired(TimerHost *host) = 0;

        bool IsActive() const      { return m_active; }
        Ticks GetDeadline() const  { return m_deadline; }
        Ticks GetTimestamp() const { return m_timestamp; }
        uint32_t GetPeriod() const { return m_period; }

        /*! \brief  Get remaining ticks until the timer next expires.
            \return Ticks remaining, or 0 if already expired / not active.
            \note   The value is advisory: it is computed from m_now (the last value written
                    by the tick task) and may be up to one tick task wake cycle stale.
                    If the deadline has already passed but not yet been processed, 0 is returned.
        */
        uint32_t GetRemainingTime() const;

    private:
        Ticks         m_deadline;  //!< absolute expiration time (ticks)
        Ticks         m_timestamp; //!< time at which timer expired (ticks), updated by TimerHost
        uint32_t      m_period;    //!< reload period in ticks (0 = one-shot)
        volatile bool m_active;    //!< true if active
        volatile bool m_pending;   //!< true if pending to be handled
    };

    explicit TimerHost()
        : m_task_tick_memory(), m_task_handler_memory(), m_task_tick(), m_task_process(), m_active(), m_now(0)
    {}

    /*! \brief     Initialize timer host instance.
        \param[in] kernel: Kernel to which instance will be bound.
        \param[in] mode: Access mode for the timer handling tasks which call expired timers.
    */
    void Initialize(IKernel *kernel, EAccessMode mode);

    /*! \brief     Start timer.
        \param[in] timer: Timer instance. Must not already be active.
        \param[in] delay: Initial delay in ticks before first expiration.
        \param[in] period: Reload period in ticks (0 is one-shot timer).
        \return    True on success, false if timer is already active or command queue is full.
    */
    bool Start(Timer &timer, uint32_t delay, uint32_t period = 0);

    /*! \brief     Stop running timer.
        \param[in] timer: Timer instance. Must be active.
        \return    True on success, false if timer is not active or command queue is full.
    */
    bool Stop(Timer &timer);

    /*! \brief     Reset periodic timer's deadline.
        \param[in] timer: Timer instance. Must be active and periodic.
        \return    True on success, false if timer is not active, not periodic, or command queue is full.
    */
    bool Reset(Timer &timer);

    /*! \brief     Atomically stop and re-start timer.
        \param[in] timer: Timer instance (active or inactive).
        \param[in] delay: Initial delay in ticks before first expiration.
        \param[in] period: Reload period in ticks (0 is one-shot timer).
        \return    True on success, false if command queue is full.
        \note      Unlike calling Stop() followed by Start(), this operation is atomic
                   with respect to the tick task: the timer cannot fire between the implicit
                   stop and re-start, and only one command queue slot is consumed.
                   Useful for watchdog refresh and debounce reset patterns.
                   Safe to call regardless of whether the timer is currently active.
    */
    bool Restart(Timer &timer, uint32_t delay, uint32_t period = 0);

    /*! \brief     Start timer if inactive, or reset its deadline if already active and periodic.
        \param[in] timer: Timer instance (active or inactive).
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
    bool StartOrReset(Timer &timer, uint32_t delay, uint32_t period = 0);

    /*! \brief     Change the period of a running periodic timer without affecting its current deadline.
        \param[in] timer: Timer instance. Must be active and periodic.
        \param[in] period: New reload period in ticks. Must be non-zero.
        \return    True on success, false if timer is not active, not periodic,
                   period is zero, or command queue is full.
        \note      The new period takes effect on the next reload after the current deadline
                   fires. To apply the new period immediately (restart from now), call
                   Reset() after SetPeriod().
    */
    bool SetPeriod(Timer &timer, uint32_t period);

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
    */
    bool Shutdown();

    /*! \brief  Get current time.
        \return Current time (ticks).
    */
    Ticks GetTimeNow() const { return hw::ReadVolatile64(&m_now); }

private:
    /*! \typedef TimerFuncType
        \brief   Timer task function prototype.
    */
    typedef void (*TimerFuncType) (TimerHost *host);

    /*! \class TimerWorkerTask
        \brief The actual task that executes timer callback.
    */
    class TimerWorkerTask : public ITask
    {
    public:
        explicit TimerWorkerTask()
            : m_func(nullptr), m_host(nullptr), m_stack(nullptr), m_stack_size(0), m_mode(ACCESS_USER), m_weight(0)
        {}

        // ITask
        EAccessMode GetAccessMode() const { return m_mode; }
        void OnDeadlineMissed(uint32_t duration) { (void)duration; }
        int32_t GetWeight() const         { return m_weight; }
        TId GetId() const                 { return 0; }
        const char *GetTraceName() const  { return nullptr; }

        // IStackMemory
        TReg *GetStack() const           { return m_stack; }
        size_t GetStackSize() const      { return m_stack_size; }
        size_t GetStackSizeBytes() const { return m_stack_size * sizeof(size_t); }

        void Initialize(TimerHost *host, size_t *stack, size_t stack_size, EAccessMode mode, TimerFuncType func)
        {
            m_host       = host;
            m_func       = func;
            m_stack      = stack;
            m_stack_size = stack_size;
            m_mode       = mode;
            m_weight     = 1;
        }

        void SetWeight(int32_t weight) { m_weight = weight; }

    private:
        void Run() { m_func(m_host); }

        TimerFuncType m_func;
        TimerHost    *m_host;
        TReg         *m_stack;
        size_t        m_stack_size;
        EAccessMode   m_mode;
        int32_t       m_weight;
    };

    struct TimerCommand
    {
        enum EId
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

    void UpdateTime();
    void ProcessTimers();
    bool ProcessCommands(Timeout next_sleep);
    bool PushCommand(TimerCommand cmd);

    typedef StackMemoryDef<TASK_TICK_MEMORY_SIZE>::Type   TaskTickMemory;
    typedef StackMemoryDef<TASK_HANDLER_STACK_SIZE>::Type TimerHostMemory;
    typedef sync::Pipe<Timer *, STK_TIMER_COUNT_MAX>      ReadyQueue;
    typedef sync::Pipe<TimerCommand, STK_TIMER_COUNT_MAX> CommandQueue;

    TaskTickMemory                m_task_tick_memory;                             //!< tick task memory
    TimerHostMemory               m_task_handler_memory[STK_TIMER_THREADS_COUNT]; //!< handler task memory
    TimerWorkerTask               m_task_tick;                                    //!< timer task
    TimerWorkerTask               m_task_process[STK_TIMER_THREADS_COUNT];        //!< handler tasks
    util::DListHead<Timer, false> m_active;                                       //!< active timers (tick task only)
    ReadyQueue                    m_queue;                                        //!< queue of timers ready for handling
    CommandQueue                  m_commands;                                     //!< command queue
    Ticks                         m_now;                                          //!< last known current time (ticks)
};

// ---------------------------------------------------------------------------
// Timer::GetTimeRemaining
// ---------------------------------------------------------------------------

inline uint32_t TimerHost::Timer::GetRemainingTime() const
{
    if (!m_active)
        return 0;

    // if deadline has passed but not yet been processed by the tick task,
    // remaining would be negative, result fits in uint32_t because delay and
    // period are uint32_t, so remaining time is bounded by uint32_t max
    Ticks remaining = m_deadline - GetTicks();
    return (remaining > 0 ? (uint32_t)remaining : 0);
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

inline void TimerHost::Initialize(IKernel *kernel, EAccessMode mode)
{
    for (int32_t i = 0; i < STK_TIMER_THREADS_COUNT; ++i)
    {
        m_task_process[i].Initialize(this, m_task_handler_memory[i], TASK_HANDLER_STACK_SIZE, mode, [](TimerHost *host) {
            host->ProcessTimers();
        });
        kernel->AddTask(&m_task_process[i]);
    }

    m_task_tick.Initialize(this, m_task_tick_memory, TASK_TICK_MEMORY_SIZE, ACCESS_USER, [](TimerHost *host) {
        host->UpdateTime();
    });
    kernel->AddTask(&m_task_tick);
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

inline bool TimerHost::Start(Timer &timer, uint32_t delay, uint32_t period)
{
    // timer must not already be active
    if (timer.m_active)
        return false;

    return PushCommand({
        .cmd       = TimerCommand::CMD_START,
        .timer     = &timer,
        .timestamp = GetTicks(),
        .delay     = delay,
        .period    = period
    });
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

inline bool TimerHost::Stop(Timer &timer)
{
    // timer must be active
    if (!timer.m_active)
        return false;

    return PushCommand({
        .cmd       = TimerCommand::CMD_STOP,
        .timer     = &timer,
        .timestamp = 0,
        .delay     = 0,
        .period    = 0
    });
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

inline bool TimerHost::Reset(Timer &timer)
{
    // timer must be active and periodic
    if (!timer.m_active || (timer.m_period == 0))
        return false;

    return PushCommand({
        .cmd      = TimerCommand::CMD_RESET,
        .timer     = &timer,
        .timestamp = GetTicks(),
        .delay     = 0,
        .period    = 0
    });
}

// ---------------------------------------------------------------------------
// Restart
// ---------------------------------------------------------------------------

inline bool TimerHost::Restart(Timer &timer, uint32_t delay, uint32_t period)
{
    return PushCommand({
        .cmd       = TimerCommand::CMD_RESTART,
        .timer     = &timer,
        .timestamp = GetTicks(),
        .delay     = delay,
        .period    = period
    });
}

// ---------------------------------------------------------------------------
// StartOrReset
// ---------------------------------------------------------------------------

inline bool TimerHost::StartOrReset(Timer &timer, uint32_t delay, uint32_t period)
{
    return PushCommand({
        .cmd       = TimerCommand::CMD_START_OR_RESET,
        .timer     = &timer,
        .timestamp = GetTicks(),
        .delay     = delay,
        .period    = period
    });
}

// ---------------------------------------------------------------------------
// SetPeriod
// ---------------------------------------------------------------------------

inline bool TimerHost::SetPeriod(Timer &timer, uint32_t period)
{
    // period == 0 is rejected: it would silently convert a periodic timer
    // to one-shot semantics, which is better expressed via Stop() + Start()
    if (!timer.m_active || (timer.m_period == 0) || (period == 0))
        return false;

    return PushCommand({
        .cmd       = TimerCommand::CMD_SET_PERIOD,
        .timer     = &timer,
        .timestamp = 0,
        .delay     = 0,
        .period    = period
    });
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

inline bool TimerHost::Shutdown()
{
    return PushCommand({
        .cmd  = TimerCommand::CMD_SHUTDOWN,
        .timer = nullptr,
        .timestamp = 0,
        .delay     = 0,
        .period    = 0
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
        next_sleep = WAIT_INFINITE;

        Ticks now = GetTicks();

        // using WriteVolatile64() to guarantee correct lockless reading order by ReadVolatile64
        hw::WriteVolatile64(&m_now, now);

        Timer *timer = static_cast<Timer *>(m_active.GetFirst());
        while (timer != nullptr)
        {
            Timer *next = static_cast<Timer *>(timer->GetNext());

            if (timer->m_active)
            {
                // check if still pending to be handled
                if (timer->m_pending)
                {
                    timer = next;
                    continue;
                }

                bool one_shot = false;
                int32_t diff = (int32_t)(now - timer->m_deadline);

                if (diff >= 0)
                {
                    // set timestamp at which timer expired
                    timer->m_timestamp = now;

                    // avoid updating timer again before it was handled
                    timer->m_pending = true;

                    // periodic
                    if (timer->m_period != 0)
                    {
                        // reload (use now to avoid drift accumulation)
                        timer->m_deadline = now + timer->m_period - diff;
                    }
                    // one-shot
                    else
                    {
                        one_shot = true;

                        // remove from active timers
                        m_active.Unlink(timer);

                        // mark as inactive (must follow Unlink)
                        timer->m_active = false;
                    }

                    __stk_full_memfence();

                    // push to the handling queue
                    m_queue.Write(timer);
                }

                // one-shot timer does not affect next_sleep
                if (!one_shot)
                {
                    Timeout next_deadline = (Timeout)(timer->m_deadline - now);
                    if (next_deadline < next_sleep)
                        next_sleep = next_deadline;
                }
            }
            else
            {
                // could be stopped externally, remove from active timers
                m_active.Unlink(timer);
            }

            timer = next;
        }
    }

    // unlink all timers on shutdown
    while (Timer::DLEntryType *timer = m_active.GetFirst())
        m_active.Unlink(timer);
}

// ---------------------------------------------------------------------------
// ProcessTimers  (handler task body)
// ---------------------------------------------------------------------------

inline void TimerHost::ProcessTimers()
{
    Timer *timer;
    while (m_queue.Read(timer))
    {
        // nullptr is the shutdown sentinel pushed by CMD_SHUTDOWN
        if (timer == nullptr)
            break;

        if (timer->m_pending)
        {
            timer->m_pending = false;
            timer->OnExpired(this);
        }
    }
}

// ---------------------------------------------------------------------------
// ProcessCommands  (tick task only)
// ---------------------------------------------------------------------------

inline bool TimerHost::ProcessCommands(Timeout next_sleep)
{
    // if nothing is active, sleep indefinitely until a command arrives
    next_sleep = (m_active.IsEmpty() ? WAIT_INFINITE : next_sleep);

    TimerCommand cmd;
    while (m_commands.Read(cmd, next_sleep))
    {
        switch (cmd.cmd)
        {
        case TimerCommand::CMD_START: {
            Timer *timer = cmd.timer;
            STK_ASSERT(timer != nullptr);

            // reject if already active or linked (double-start)
            if (timer->m_active)
                continue;

            STK_ASSERT(!timer->IsLinked());

            timer->m_deadline = cmd.timestamp + cmd.delay;
            timer->m_period   = cmd.period;
            timer->m_active   = true;
            timer->m_pending  = false;

            m_active.LinkBack(timer);
            next_sleep = NO_WAIT;
            break; }

        case TimerCommand::CMD_STOP: {
            Timer *timer = cmd.timer;
            STK_ASSERT(timer != nullptr);

            timer->m_active  = false;
            timer->m_pending = false;

            // allow possibly duplicate CMD_STOP as it is harmless for the logic
            if (timer->IsLinked())
                m_active.Unlink(timer);
            break; }

        case TimerCommand::CMD_RESET: {
            Timer *timer = cmd.timer;
            STK_ASSERT(timer != nullptr);

            // only reset if still active and periodic
            if (!timer->m_active || (timer->m_period == 0))
                continue;

            STK_ASSERT(timer->GetHead() == &m_active);

            timer->m_deadline = cmd.timestamp + timer->m_period;
            timer->m_pending  = false;

            next_sleep = NO_WAIT;
            break; }

        case TimerCommand::CMD_RESTART: {
            // atomic stop + re-start: no precondition on current timer state
            Timer *timer = cmd.timer;
            STK_ASSERT(timer != nullptr);

            // unlink if currently in the active list
            if (timer->IsLinked())
                m_active.Unlink(timer);

            // re-arm with fresh parameters
            timer->m_deadline = cmd.timestamp + cmd.delay;
            timer->m_period   = cmd.period;
            timer->m_active   = true;
            timer->m_pending  = false;

            // re-link to the back of the list
            m_active.LinkBack(timer);
            next_sleep = NO_WAIT;
            break; }

        case TimerCommand::CMD_START_OR_RESET: {
            Timer *timer = cmd.timer;
            STK_ASSERT(timer != nullptr);

            // not currently active: start with supplied parameters
            if (!timer->m_active)
            {
                STK_ASSERT(!timer->IsLinked());

                timer->m_deadline = cmd.timestamp + cmd.delay;
                timer->m_period   = cmd.period;
                timer->m_active   = true;
                timer->m_pending  = false;

                m_active.LinkBack(timer);
            }
            else
            // active and periodic: reset deadline anchored to call-site timestamp
            if (timer->m_period != 0)
            {
                STK_ASSERT(timer->GetHead() == &m_active);

                timer->m_deadline = cmd.timestamp + timer->m_period;
                timer->m_pending  = false;
            }

            // active one-shot: no action — cannot reset a one-shot mid-flight,
            // caller should use Restart() if unconditional re-arm is needed.

            next_sleep = NO_WAIT;
            break; }

        case TimerCommand::CMD_SET_PERIOD: {
            Timer *timer = cmd.timer;
            STK_ASSERT(timer != nullptr);
            STK_ASSERT(cmd.period != 0);

            // guard: only apply if still active and periodic
            if (!timer->m_active || (timer->m_period == 0))
                continue;

            STK_ASSERT(timer->GetHead() == &m_active);

            // new period takes effect on the next reload, current deadline is
            // intentionally left unchanged so the in-flight interval is not
            // truncated or extended, caller can follow up with Reset() if
            // immediate application is required
            timer->m_period = cmd.period;
            break; }

        case TimerCommand::CMD_SHUTDOWN: {
            // wake all handler tasks with shutdown sentinels
            for (int32_t i = 0; i < STK_TIMER_THREADS_COUNT; ++i)
                m_queue.Write(nullptr, NO_WAIT);

            // signal UpdateTime() to exit its loop
            return false; }

        default: {
            STK_ASSERT(false);
            break; }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// PushCommand
// ---------------------------------------------------------------------------

inline bool TimerHost::PushCommand(TimerCommand cmd)
{
    if (!m_commands.Write(cmd, NO_WAIT))
    {
        // queue full: this indicates a usage error — more commands are being
        // issued than the tick task can drain. Loud in debug, recoverable in
        // release (caller receives false and can retry or escalate).
        STK_ASSERT(false);
        return false;
    }

    return true;
}

} // namespace time
} // namespace stk

#endif /* STK_TIME_TIMER_H_ */
