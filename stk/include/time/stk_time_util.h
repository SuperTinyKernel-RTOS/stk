/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_TIME_UTIL_H_
#define STK_TIME_UTIL_H_

/*! \file  stk_time_util.h
    \brief Time-related utilities: stk::time::PeriodicTrigger.
*/

namespace stk {
namespace time {

/*! \struct PeriodicTrigger
    \brief  Lightweight periodic trigger: returns \c true once per configured period when polled.

    Implements an absolute-time based periodic trigger. Internally stores the
    tick value of the next scheduled firing (\c m_next). Each call to Poll()
    compares the current tick count against \c m_next. When the current time
    reaches or exceeds \c m_next, Poll() returns \c true and advances
    \c m_next by exactly one period.

    Because the next trigger time is incremented by \c m_period rather than
    reset to the current time, the long-term firing rate remains stable even
    if individual Poll() calls are delayed.

    Usage example:
    \code
    // Trigger every 500 ticks (actual wall-clock duration depends on tick resolution).
    stk::time::PeriodicTrigger trigger(500, true);

    // Inside a task loop:
    if (trigger.Poll())
    {
        // executed once per 500-tick period
    }
    \endcode

    \note  Not thread-safe. Intended for use within a single task or ISR context.
    \note  If constructed without start=true, Restart() must be called before Poll().
    \note  When started (either via constructor or Restart()), the first Poll()
           firing occurs no earlier than \c m_period ticks after the start moment.
*/
class PeriodicTrigger
{
public:
    /*! \brief     Construct a PeriodicTrigger.
        \param[in] period: Trigger period in ticks. Must be > 0. The wall-clock duration
                   of one tick is determined by the resolution passed to
                   \c IKernel::Initialize() (see \c IKernel::GetTickResolution()).
        \param[in] start: \c true to start immediately, \c false otherwise (default).
        \note      If start=true, equivalent to calling Restart() from the constructor.
                   The first Poll() firing will occur no earlier than \a period ticks
                   after construction.
    */
    PeriodicTrigger(uint32_t period, bool start = false) : m_next(0), m_period(period)
    {
        if (start)
        {
            Restart();
        }
    }

    /*! \brief  Get currently configured trigger period.
        \return Trigger period in ticks.
    */
    uint32_t GetPeriod() const
    {
        return m_period;
    }

    /*! \brief     Change the trigger period while preserving phase.
        \param[in] period: New trigger period in ticks. Must be > 0.
        \note      Adjusts \c m_next so that the relative progress toward the next
                   firing is preserved. Takes effect immediately.
    */
    void SetPeriod(uint32_t period)
    {
        m_next = (m_next - static_cast<Ticks>(m_period)) + static_cast<Ticks>(period);
        m_period = period;
    }

    /*! \brief  Reset the trigger and start.
        \note   Sets \c m_next to (current ticks + m_period).
                The next Poll() firing will occur no earlier than \c m_period ticks after this call.
    */
    void Restart()
    {
        m_next = GetTicks() + static_cast<Ticks>(m_period);
    }

    /*! \brief   Check whether the scheduled trigger time has been reached.
        \return  \c true once when the current tick count reaches or exceeds
                 the scheduled trigger time, \c false otherwise.
        \note    Should be called regularly (e.g. every task iteration).
                 If multiple full periods have elapsed since the previous call,
                 only a single \c true is returned and \c m_next is advanced by
                 exactly one period. Subsequent calls will continue to catch up
                 one period at a time until the schedule is realigned.
        \warning Must be started (constructor with start=true or Restart()).
    */
    bool Poll()
    {
        STK_ASSERT(m_next > 0);

        bool triggered = false;
        const Ticks diff = GetTicks() - m_next;

        if (diff >= 0)
        {
            m_next += static_cast<Ticks>(m_period);
            triggered = true;
        }

        return triggered;
    }

protected:
    Ticks    m_next;   //!< Next trigger time in ticks.
    uint32_t m_period; //!< Trigger period in ticks. Modified only by SetPeriod(). Must be > 0.
};

/*! \struct Stopwatch
    \brief  Lightweight elapsed-cycle measurement utility.

    Measures the number of CPU cycles (or other monotonic counter units) that
    have elapsed between successive calls to Update(). The time source is
    supplied by the caller as a callable (function pointer, lambda, or functor),
    allowing the stopwatch to remain independent of any particular hardware
    counter.

    Typical usage:
    \code
    stk::time::Stopwatch sw;
    sw.Start([]() { return stk::hw::HiResClock::GetCycles(); });

    // ... work to be measured ...

    Cycles elapsed = sw.Update([]() { return stk::hw::HiResClock::GetCycles(); });
    // elapsed holds the number of cycles since the last Start() or Update().
    \endcode

    \note  Not thread-safe. Intended for use within a single task or ISR context.
    \note  If Update() is called before Start(), the first call is treated as a
           start point and returns 0 cycles elapsed.
    \note  Wrap-around of the underlying counter is handled naturally by
           unsigned subtraction, provided the elapsed time does not exceed
           one full counter period.
*/
class Stopwatch
{
    static constexpr Cycles NOT_STARTED = 0ULL; //!< Value of m_prev meaning that stopwatch is not started.

public:
    /*! \brief  Construct a Stopwatch.
        \note   The stopwatch is initially stopped. Call Start() before the
                first meaningful Update() call; otherwise the first Update()
                call will implicitly act as the start point and return 0.
    */
    Stopwatch() : m_prev(NOT_STARTED)
    {}

    /*! \brief     Capture the start time.
        \tparam    Func: Callable type returning a \c Cycles value.
        \param[in] now_func: Callable invoked with no arguments that returns
                   the current time as a \c Cycles value (e.g. a CPU cycle counter
                   read function or a lambda wrapping one).
        \note      Resets the internal reference point to the current counter value.
                   The next Update() call will measure elapsed cycles from this moment.
    */
    template <typename Func>
    void Start(Func now_func)
    {
        m_prev = static_cast<Cycles>(now_func());
    }

    /*! \brief     Return the number of cycles elapsed since the last Start() or Update().
        \tparam    Func: Callable type returning a \c Cycles value.
        \param[in] now_func: Callable invoked with no arguments that returns
                   the current time as a \c Cycles value. Must be the same counter
                   source used in the preceding Start() call.
        \return    Elapsed \c Cycles since the previous reference point.
                   Returns \c 0 on the very first call when Start() was not called
                   beforehand (the call itself becomes the new reference point).
        \note      Updates the internal reference point to the current counter value,
                   so each successive call measures the interval since the previous one.
    */
    template <typename Func>
    Cycles Update(Func now_func)
    {
        Cycles now = static_cast<Cycles>(now_func());

        if (STK_UNLIKELY(m_prev == NOT_STARTED))
        {
            m_prev = now;
        }

        Cycles diff = now - m_prev;
        m_prev = now;

        return diff;
    }

protected:
    Cycles m_prev; //!< Counter value captured at the last Start() or Update() call.
};

} // namespace time
} // namespace stk

#endif /* STK_TIME_UTIL_H_ */
