/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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

    Maintains a running elapsed-tick accumulator. Each call to Poll() adds the ticks
    elapsed since the previous call. When the accumulator reaches \c m_period, Poll()
    returns \c true and subtracts \c m_period from the accumulator rather than resetting
    it to zero. This preserves any over-run into the next period and keeps the long-term
    firing rate accurate even if individual Poll() calls are slightly late.

    Usage example:
    \code
    // Trigger every 500 ticks (actual wall-clock duration depends on tick resolution).
    stk::time::PeriodicTrigger trigger(500);

    // Inside a task loop:
    if (trigger.Poll())
    {
        // executed once per 500-tick period
    }
    \endcode

    \note  Not thread-safe. Intended for use within a single task or ISR context.
    \note  The first Poll() call fires no earlier than \c m_period ticks after construction,
           because \c m_prev is initialized to the current tick count in the constructor.
*/
class PeriodicTrigger
{
public:
    /*! \brief     Construct a PeriodicTrigger and start the period from the current tick count.
        \param[in] period: Trigger period in ticks. Must be > 0. The wall-clock duration of one
                   tick is determined by the resolution passed to \c IKernel::Initialize()
                   (see \c IKernel::GetTickResolution()).
        \note      \c m_prev is initialised by calling GetTicks() (the kernel tick counter),
                   so the first Poll() firing occurs no earlier than \a period ticks after
                   the moment of construction.
    */
    PeriodicTrigger(Ticks period) : m_period(period), m_prev(GetTicks()), m_elapsed(0)
    {}

    /*! \brief     Change the trigger period without resetting the elapsed accumulator.
        \param[in] period: New trigger period in ticks. Must be > 0.
        \note      Takes effect on the next Poll() call. The current value of \c m_elapsed
                   is preserved, so if the new period is shorter than the accumulated elapsed
                   time, the very next Poll() call may fire immediately.
    */
    void SetPeriod(Ticks period)
    {
        m_period = period;
    }

    /*! \brief  Reset the trigger as if it were just constructed at the current tick count.
        \note   Sets \c m_prev to the current tick count and clears \c m_elapsed to zero.
                The next Poll() firing will occur no earlier than \c m_period ticks after
                this call. Does not change \c m_period.
    */
    void Restart()
    {
        m_prev    = GetTicks();
        m_elapsed = 0;
    }

    /*! \brief  Advance the accumulator and check whether a period has elapsed.
        \return \c true once per \c m_period ticks, \c false otherwise.
        \note   Should be called regularly (e.g. every task iteration). Infrequent calls do
                not lose the trigger: if more than \c m_period ticks have elapsed since the
                last call, Poll() returns \c true for that single call and the surplus is
                carried into the next period via \c m_elapsed.
        \note   At most one \c true is returned per call regardless of how many full periods
                have passed since the previous call. Applications requiring exact counts of
                every elapsed period (including missed ones) should inspect \c m_elapsed
                directly after Poll() returns \c false.
    */
    bool Poll()
    {
        Ticks now = GetTicks();

        m_elapsed += (now - m_prev);
        m_prev = now;

        if (m_elapsed >= m_period)
        {
            m_elapsed -= m_period;
            return true;
        }

        return false;
    }

protected:
    Ticks m_period;  //!< Trigger period in ticks. Modified only by SetPeriod(). Must be > 0.
    Ticks m_prev;    //!< Tick count captured at the last Poll() call or at construction. Used to compute the elapsed delta on the next Poll() call.
    Ticks m_elapsed; //!< Accumulated ticks since the last trigger fired. Carries over the remainder after each firing to maintain long-term accuracy.
};

} // namespace time
} // namespace stk

#endif /* STK_TIME_UTIL_H_ */
