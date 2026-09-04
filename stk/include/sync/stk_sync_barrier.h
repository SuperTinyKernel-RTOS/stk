/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_BARRIER_H_
#define STK_SYNC_BARRIER_H_

#include "stk_sync_mutex.h"
#include "stk_sync_cv.h"

/*! \file  stk_sync_barrier.h
    \brief Implementation of synchronization primitive: stk::sync::Barrier.
*/

namespace stk {
namespace sync {

/*! \class Barrier
    \brief Cyclic barrier that blocks a fixed-size group of tasks until all of them have arrived.

    A Barrier lets a set of \a count tasks rendezvous at a common point: each task calls
    \c Wait() and blocks until the last member of the group also calls \c Wait(). Once the last
    task arrives, all waiting tasks are released simultaneously and the barrier automatically
    resets (cyclic behavior), ready to be reused for the next round.

    \note  Implemented on top of \a Mutex and \a ConditionVariable. A generation counter is used
           to distinguish successive rounds and to guard the wait loop against spurious wakeups.

    \code
    // Example: synchronizing 3 worker tasks at the end of each processing round
    stk::sync::Barrier g_Barrier(3U);

    void Task_Worker() {
        for (;;) {
            // ... do per-round work ...

            // block here until all 3 tasks reach this point
            if (g_Barrier.Wait()) {
                // only the last task to arrive executes this branch:
                // safe to do once-per-round bookkeeping here
            }
        }
    }
    \endcode

    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
    \see  Mutex, ConditionVariable
*/
class Barrier final : public ITraceable
{
public:
    /*! \brief     Constructor.
        \param[in] count: Number of tasks that must call \c Wait() before any of them is released.
        \note      \a count must not be 0.
    */
    explicit Barrier(uint32_t count) : m_threshold(count), m_count(count), m_generation(0U)
    {
        STK_ASSERT(count != 0U); // API contract: number of parties cannot be 0
    }

    /*! \brief     Destructor.
    */
    STK_VIRT_DTOR ~Barrier()
    {}

    /*! \brief     Block the calling task until \a count tasks have called \c Wait().
        \details   Once the last task arrives, all currently waiting tasks are woken up and the
                   barrier resets itself so it can be reused for the next round.
        \warning   ISR-unsafe.
        \return    True if the calling task was the last one to arrive (and thus released the
                   others), false if the calling task was one of the released waiters.
    */
    bool Wait();

    /*! \brief     Get the number of tasks required to trip the barrier.
        \warning   ISR-safe.
    */
    uint32_t GetThreshold() const { return m_threshold; }

private:
    STK_NONCOPYABLE_CLASS(Barrier);

    Mutex             m_mutex;      //!< protects m_count / m_generation and serializes calls to m_cond
    ConditionVariable m_cond;       //!< signals waiting tasks when the current generation completes
    const uint32_t    m_threshold;  //!< number of tasks required to trip the barrier
    uint32_t          m_count;      //!< number of tasks still to arrive in the current generation
    uint32_t          m_generation; //!< generation counter, incremented every time the barrier trips
};

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

inline bool Barrier::Wait()
{
    bool is_last = false;

    STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR

    m_mutex.Lock();

    const uint32_t gen = m_generation;

    if (--m_count == 0U)
    {
        // last task to arrive: start a new generation and release everyone else
        ++m_generation;
        m_count = m_threshold;

        m_cond.NotifyAll();

        is_last = true;
    }
    else
    {
        // wait in a loop to guard against spurious wakeups: keep waiting while
        // we are still in the same generation we entered with
        while (gen == m_generation)
        {
            STK_UNUSED(m_cond.Wait(m_mutex, WAIT_INFINITE));
        }
    }

    m_mutex.Unlock();

    return is_last;
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_BARRIER_H_ */
