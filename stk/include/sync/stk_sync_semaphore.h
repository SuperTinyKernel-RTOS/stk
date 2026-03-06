/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
    \brief Implementation of synchronization primitive: Semaphore.
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
class Semaphore : public ITraceable, private ISyncObject
{
public:
    /*! \brief     Constructor.
        \param[in] initial_count: Starting value of the semaphore.
    */
    explicit Semaphore(uint32_t initial_count = 0) : m_count(initial_count)
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical
                   error (dangling waiters).
                   An assertion is triggered in debug builds.
    */
    virtual ~Semaphore() { STK_ASSERT(m_wait_list.IsEmpty()); }

    /*! \brief     Wait for a signal (decrement counter).
        \param[in] timeout: Maximum time to wait (ticks).
        \warning   ISR-unsafe.
        \return    True if acquired, false if timeout occurred.
    */
    bool Wait(Timeout timeout = WAIT_INFINITE);

    /*! \brief     Post a signal (increment counter).
        \note      Gives "token" directly to the waking task. The count is not incremented, and
                   the waking task does not decrement it.
        \note      ISR-safe.
    */
    void Signal();

    /*! \brief     Get current counter value.
        \note      ISR-safe.
    */
    uint32_t GetCount() const { return m_count; }

private:
    bool Tick();

    uint32_t m_count; //!< Internal resource counter
};

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

inline bool Semaphore::Wait(Timeout timeout)
{
    // not supported inside ISR, may call Wait
    STK_ASSERT(!hw::IsInsideISR());

    ScopedCriticalSection __cs;

    // fast path: resource is available
    if (m_count != 0)
    {
        --m_count;
        __stk_full_memfence();
        return true;
    }

    // try lock behavior (timeout 0)
    if (timeout == 0)
        return false;

    // slow path: block until Signal() or timeout
    // note: after waking, if not a timeout, we effectively own the resource that Signal() produced
    // but didn't put into m_count (see logic of if (m_wait_list.IsEmpty()) in Signal())
    return !IKernelService::GetInstance()->Wait(this, &__cs, timeout)->IsTimeout();
}

// ---------------------------------------------------------------------------
// Signal
// ---------------------------------------------------------------------------

inline void Semaphore::Signal()
{
    ScopedCriticalSection __cs;

    if (m_wait_list.IsEmpty())
    {
        // no one is waiting, save signal for later
        ++m_count;
        __stk_full_memfence();
    }
    else
    {
        // give signal directly to the first waiting task
        WakeOne();
    }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

inline bool Semaphore::Tick()
{
    // required for multi-core CPU and multiple instances of STK (one per core)
#if (STK_ARCH_CPU_COUNT > 1)
    ScopedCriticalSection __cs;
#endif

    return ISyncObject::Tick();
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_SEMAPHORE_H_ */
