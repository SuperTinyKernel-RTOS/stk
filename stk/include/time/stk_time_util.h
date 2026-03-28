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
    \brief Time-related utilities (PeriodicTrigger).
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
    PeriodicTrigger(uint32_t period, bool start = false) : m_next(0U), m_period(period)
    {
        if (start)
            Restart();
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
        m_next = GetTicks() + m_period;
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

        Ticks diff = GetTicks() - m_next;
        if (diff >= 0)
        {
            m_next += m_period;
            return true;
        }

        return false;
    }

protected:
    Ticks    m_next;   //!< Next trigger time in ticks.
    uint32_t m_period; //!< Trigger period in ticks. Modified only by SetPeriod(). Must be > 0.
};

} // namespace time
} // namespace stk

#endif /* STK_TIME_UTIL_H_ */
