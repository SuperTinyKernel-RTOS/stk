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
    /*! \enum  EResult
        \brief Outcome of a Barrier::WaitEx() call.
    */
    enum EResult : uint8_t
    {
        BARRIER_RELEASED,     //!< Released because the barrier tripped; this task was not the last arrival.
        BARRIER_LAST_ARRIVAL, //!< This task was the last arrival and tripped the barrier.

        BARRIER_CANCELED,     //!< Wait was cancelled via IKernel::CancelTaskWait() before the barrier
                              //!< tripped. This task's arrival is rolled back (does not count toward
                              //!< the generation it was waiting on) so bookkeeping stays consistent for
                              //!< the remaining parties - it does not, and cannot, make the barrier trip
                              //!< without a replacement arrival.

        BARRIER_DELETED,      //!< The barrier was destroyed while this task was waiting (WAIT_RESULT_DELETED).
                              //!< The barrier must not be accessed afterwards; no bookkeeping is touched.

        BARRIER_FAILED        //!< The kernel rejected the wait (WAIT_RESULT_FAIL, e.g. the scheduler is locked,
                              //!< KERNEL_SYNC is not enabled or the kernel is not running). The arrival is rolled
                              //!< back the same way as for BARRIER_CANCELED.
    };

    /*! \brief     Constructor.
        \param[in] count: Number of tasks that must call \c Wait() before any of them is released.
        \note      \a count must not be 0.
    */
    explicit Barrier(uint32_t count) : m_threshold(count), m_count(count), m_generation(0U)
    {
        STK_ASSERT(count != 0U); // API contract: number of parties cannot be 0
    }

    /*! \brief     Destructor.
        \note      Tasks still waiting on the barrier at destruction time are woken and \c WaitEx() returns
                   \c BARRIER_DELETED (\c Wait() returns \c false); they must not access the barrier afterwards.
    */
    STK_VIRT_DTOR ~Barrier() = default;

    /*! \brief     Block the calling task until \a count tasks have called \c Wait().
        \details   Once the last task arrives, all currently waiting tasks are woken up and the
                   barrier resets itself so it can be reused for the next round.
        \note      Cannot be interrupted by a timeout (there is none), but can be interrupted by
                   \c IKernel::CancelTaskWait(), or fail in the kernel. This overload cannot report
                   that, it returns \c false the same as a normal (non-last) release. Use \c WaitEx()
                   to tell them apart.
        \warning   ISR-unsafe.
        \return    True if the calling task was the last one to arrive (and thus released the
                   others), false if the calling task was one of the released waiters.
    */
    bool Wait() { return (WaitEx() == BARRIER_LAST_ARRIVAL); }

    /*! \brief     Block the calling task until \a count tasks have called \c Wait(), preserving
                   the full outcome.
        \details   Identical to \c Wait(), except the caller can distinguish a normal release,
                   being the last arrival, and a cancellation via \c IKernel::CancelTaskWait().
        \warning   ISR-unsafe.
        \return    \c BARRIER_LAST_ARRIVAL, \c BARRIER_RELEASED, \c BARRIER_CANCELED, or \c BARRIER_FAILED.
        \note      Blocks, therefore it must not be called while the scheduler is locked
                   (see IKernelService::LockScheduler): it asserts in debug builds and returns \c BARRIER_FAILED
                   in release builds (unless the caller is the last arrival, which does not block).
    */
    EResult WaitEx();

    /*! \brief     Get the number of tasks required to trip the barrier.
        \warning   ISR-safe.
    */
    uint32_t GetThreshold() const { return m_threshold; }

private:
    STK_NONCOPYABLE_CLASS(Barrier);

    ConditionVariable m_cond;       //!< signals waiting tasks when the current generation completes
    const uint32_t    m_threshold;  //!< number of tasks required to trip the barrier
    uint32_t          m_count;      //!< number of tasks still to arrive in the current generation
    uint32_t          m_generation; //!< generation counter, incremented every time the barrier trips
};

// ---------------------------------------------------------------------------
// WaitEx
// ---------------------------------------------------------------------------

inline Barrier::EResult Barrier::WaitEx()
{
    EResult result = BARRIER_RELEASED;

    STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR

    // a critical section guards the state: the kernel re-locks it in the waiting task
    // after the wake, which must stay valid when the barrier is destroyed while the task waits
    ScopedCriticalSection cs_;

    const uint32_t gen = m_generation;

    if (--m_count == 0U)
    {
        // last task to arrive: start a new generation and release everyone else
        ++m_generation;
        m_count = m_threshold;

        m_cond.NotifyAll_CS();

        result = BARRIER_LAST_ARRIVAL;
    }
    else
    {
        // wait in a loop to guard against spurious wakeups: keep waiting while
        // we are still in the same generation we entered with
        while (gen == m_generation)
        {
            const EWaitResult wr = m_cond.WaitEx(cs_, WAIT_INFINITE);

            if (wr == WAIT_RESULT_DELETED)
            {
                // the barrier was destroyed while waiting: no member may be touched
                result = BARRIER_DELETED;
                break;
            }

            if (wr != WAIT_RESULT_SIGNAL)
            {
                // cancellation or kernel failure (e.g. scheduler locked): the wait did not block, so the loop
                // must not retry (it would spin forever). The cancel wakes this task before the critical section is
                // re-acquired, so the barrier may have tripped in between (the trip has then already consumed
                // our arrival and reset m_count): signal wins, report a normal release and leave the count alone
                if (gen == m_generation)
                {
                    // bail out before the barrier tripped: undo our own arrival so the count
                    // stays consistent for whichever tasks remain in this generation - do NOT
                    // let the barrier trip short-handed on our behalf
                    ++m_count;
                    result = ((wr == WAIT_RESULT_CANCELED) ? BARRIER_CANCELED : BARRIER_FAILED);
                }

                break;
            }
            // else: WAIT_RESULT_SIGNAL - NotifyAll() always pairs with ++m_generation, so a
            // genuine signal here means gen != m_generation already and the loop exits next check
        }
    }

    return result;
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_BARRIER_H_ */
