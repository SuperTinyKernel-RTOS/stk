/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_SEMAPHORE_H_
#define STK_SYNC_SEMAPHORE_H_

#include "stk_sync_cs.h"

/*! \file  stk_sync_semaphore.h
    \brief Implementation of synchronization primitive: stk::sync::Semaphore.
*/

namespace stk {
namespace sync {

/*! \class Semaphore
    \brief Counting semaphore primitive for resource management and signaling.

    Counting semaphore maintains an internal counter to manage access to a limited
    number of resources. Unlike a Condition Variable, a Semaphore is stateful: if
    \c Signal() is called when no tasks are waiting, the signal is "remembered" by
    incrementing the internal counter.

    \note  This implementation uses a Direct Handover policy: when a task is waiting,
           \c Signal() gives the resource "token" directly to the first task in the
           wait list without incrementing the counter. The waking task is then
           guaranteed ownership of that token upon returning from \c Wait().

    \code
    // Example: Resource throttling
    // Initialize with 3 permits (e.g., max 3 concurrent tasks accessing the same resource)
    stk::sync::Semaphore g_Limiter(3);

    void Worker() {
        // attempt to acquire a permit with a 1000 tick timeout
        if (g_Limiter.Wait(1000)) {

            // ... access limited resource ...

            // release the permit back to the semaphore
            g_Limiter.Signal();
        }
    }
    \endcode

    \see  ISyncObject, IWaitObject, IKernelService::Wait
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
class Semaphore final : private ISyncObject, public ITraceable
{
public:
    /*! \brief     Max count value supported.
    */
    static const uint16_t COUNT_MAX = 0xFFFEU;

    /*! \brief     Constructor.
        \details   Initializes the semaphore with a specific count and maximum limit.
                   Setting \a initial_count to a non-zero value allows for immediate
                   resource acquisition.
        \param[in] initial_count: Starting value of the internal counter (available permits).
        \param[in] max_count: Maximum value the counter is allowed to reach.
        \pre       \a initial_count <= \a max_count.
        \note      Triggers an assertion in a debug build if \a initial_count exceeds \a max_count.
    */
    explicit Semaphore(uint16_t initial_count = 0U, uint16_t max_count = COUNT_MAX)
        : m_count(initial_count), m_count_max(max_count)
    {
        STK_ASSERT(initial_count < max_count); // API contract: initial count must not exceed maximum
    }

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical
                   error (dangling waiters).
                   An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~Semaphore()
    {
        STK_ASSERT(m_wait_list.IsEmpty()); // API contract: must not be destroyed with waiting tasks
    }

    /*! \brief     Wait for a signal (decrement counter).
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \warning   ISR-safe only with \a timeout = \c NO_WAIT, ISR-unsafe otherwise.
        \return    True if acquired, false if timeout occurred.
    */
    bool Wait(Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Poll the semaphore without blocking (decrement counter if available).
        \details   Checks if a resource token is available. If so, decrements the counter
                   and returns immediately without yielding the CPU or touching the kernel
                   wait list. If the counter is zero, returns immediately with \c false.
        \note      ISR-safe.
        \return    \c true if a resource token was acquired, \c false if count was zero.
    */
    bool TryWait() { return Wait(NO_WAIT); }

    /*! \brief     Post a signal (increment counter).
        \note      Gives "token" directly to the waking task. The count is not incremented, and
                   the waking task does not decrement it.
        \note      ISR-safe.
    */
    void Signal();

    /*! \brief     Get current counter value.
        \return    Advisory snapshot of the counter. May be stale by the time the
                   caller acts on it. Atomic on 32-bit aligned targets.
        \note      ISR-safe on targets where a 32-bit aligned read is a single instruction
                   (all supported STK architectures). Not safe for read-modify-write use.
    */
    uint16_t GetCount() const { return m_count; }

private:
    STK_NONCOPYABLE_CLASS(Semaphore);

    uint16_t m_count;     //!< Internal resource counter
    uint16_t m_count_max; //!< Counter max limit
};

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

inline bool Semaphore::Wait(Timeout timeout_ticks)
{
    ScopedCriticalSection cs_;
    bool success = false;

    // fast path: resource is available
    if (m_count != 0U)
    {
        m_count = static_cast<uint16_t>(m_count - 1U);
        __stk_full_memfence();
        success = true;
    }
    // slow path: block until Signal() or timeout
    else if (timeout_ticks != NO_WAIT)
    {
        STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR if timeout_ticks!=NO_WAIT

        // note: after waking, if not a timeout, we effectively own the resource that Signal() produced
        // but didn't put into m_count (see logic of if (m_wait_list.IsEmpty()) in Signal())
        success = (IKernelService::GetInstance()->Wait(this, &cs_, timeout_ticks) == WAIT_RESULT_SIGNAL);
    }
    // try lock behavior (timeout_ticks=NO_WAIT)
    else
    {
        // success is false already, noop
    }

    return success;
}

// ---------------------------------------------------------------------------
// Signal
// ---------------------------------------------------------------------------

inline void Semaphore::Signal()
{
    const ScopedCriticalSection cs_;

    if (m_wait_list.IsEmpty())
    {
        STK_ASSERT(m_count < m_count_max); // API contract: the count must not exceed maximum

        // no one is waiting, save signal for later
        m_count = static_cast<uint16_t>(m_count + 1U);
        __stk_full_memfence();
    }
    else
    {
        // give signal directly to the first waiting task
        WakeOne();
    }
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_SEMAPHORE_H_ */
