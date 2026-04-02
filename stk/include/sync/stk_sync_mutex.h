/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_MUTEX_H_
#define STK_SYNC_MUTEX_H_

#include "stk_sync_cs.h"

/*! \file  stk_sync_mutex.h
    \brief Implementation of synchronization primitive: stk::sync::Mutex.
*/

namespace stk {
namespace sync {

/*! \class Mutex
    \brief Recursive mutex primitive that allows the same thread to acquire the lock multiple times.

    Recursive mutex tracks ownership and a recursion count. If the owning thread
    calls \c Lock() again, the count is incremented and the call returns immediately
    without blocking. The lock is only fully released when \c Unlock() has been
    called an equal number of times.

    \code
    // Example: Recursive locking in nested methods
    stk::sync::Mutex g_ResourceMtx;

    void Method_Internal() {
        // second acquisition (recursion count = 2)
        g_ResourceMtx.Lock();
        // ... perform internal logic ...
        g_ResourceMtx.Unlock();
    }

    void Method_Public() {
        // first acquisition (recursion count = 1)
        if (g_ResourceMtx.TimedLock(100)) {
            // safe to call: same thread already owns the lock
            Method_Internal();
            g_ResourceMtx.Unlock();
        }
    }
    \endcode

    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
    \see  ISyncObject, IWaitObject, IKernelService::Wait
*/
class Mutex : public IMutex, public ITraceable, private ISyncObject
{
public:
    /*! \brief     Constructor.
    */
    explicit Mutex() : m_owner_tid(TID_NONE), m_recursion_count(0U)
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error (dangling waiters).
                   An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~Mutex()
    {
        STK_ASSERT(m_wait_list.IsEmpty()); // API contract: must not be destroyed with waiting tasks
    }

    /*! \brief     Acquire lock.
        \param[in] timeout: Maximum time to wait (ticks).
        \note      Maximum number of recursive locks must not exceed 0xFFFEU.
        \warning   ISR-unsafe.
        \return    True if lock acquired, false if timeout occurred.
    */
    bool TimedLock(Timeout timeout);

    /*! \brief     Acquire lock.
        \warning   ISR-unsafe.
    */
    void Lock() { (void)TimedLock(WAIT_INFINITE); }

    /*! \brief     Acquire the lock.
        \warning   ISR-unsafe.
        \return    True if lock acquired, false if lock is already acquired by another task.
    */
    bool TryLock() { return TimedLock(NO_WAIT); }

    /*! \brief     Release lock.
        \warning   ISR-unsafe.
    */
    void Unlock();

private:
    STK_NONCOPYABLE_CLASS(Mutex);

    static const uint16_t RECURSION_MAX = 0xFFFEu; //!< maximum nesting depth

    TId      m_owner_tid;       //!< thread id of the current owner
    uint16_t m_recursion_count; //!< recursion depth
};

// ---------------------------------------------------------------------------
// TimedLock
// ---------------------------------------------------------------------------

inline bool Mutex::TimedLock(Timeout timeout)
{
    STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR

    IKernelService *svc = IKernelService::GetInstance();
    TId current_tid = svc->GetTid();

    ScopedCriticalSection cs_;

    // already owned by the calling thread (recursive path)
    if ((m_recursion_count != 0U) && (m_owner_tid == current_tid))
    {
        STK_ASSERT(m_recursion_count < RECURSION_MAX); // API contract: caller must not exceed max recursion depth

        m_recursion_count = static_cast<uint16_t>(m_recursion_count + 1U);
        return true;
    }

    // mutex is free (fast path)
    if (m_recursion_count == 0U)
    {
        // kernel invariant: counter is zero so owner must be TID_NONE
        if (m_owner_tid != TID_NONE)
            STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);

        m_recursion_count = 1U;
        m_owner_tid       = current_tid;
        __stk_full_memfence();

        return true;
    }

    // try lock behavior
    if (timeout == NO_WAIT)
        return false;

    // mutex owned by another thread (slow path/blocking)
    if (svc->Wait(this, &cs_, timeout)->IsTimeout())
        return false;

    // kernel invariant: if either condition is false, the low-level lock and the
    // recursion counter are out of sync, this is an internal defect, not a caller error
    if ((m_owner_tid != current_tid) || (m_recursion_count != 1U))
        STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);

    return true;
}

// ---------------------------------------------------------------------------
// Unlock
// ---------------------------------------------------------------------------

inline void Mutex::Unlock()
{
    STK_ASSERT(!hw::IsInsideISR());      // API contract: caller must not be in ISR
    STK_ASSERT(m_owner_tid == GetTid()); // API contract: caller must own the lock
    STK_ASSERT(m_recursion_count != 0U); // API contract: must have matching Lock()

    ScopedCriticalSection cs_;

    m_recursion_count = static_cast<uint16_t>(m_recursion_count - 1U);

    if (m_recursion_count == 0U)
    {
        if (!m_wait_list.IsEmpty())
        {
            // pass ownership directly to the first waiter (FIFO order)
            IWaitObject *waiter = static_cast<IWaitObject *>(m_wait_list.GetFirst());

            // transfer ownership to the waiter
            m_recursion_count = 1U;
            m_owner_tid       = waiter->GetTid();
            __stk_full_memfence();

            waiter->Wake(false);
        }
        else
        {
            // free completely if there are no waiters
            m_owner_tid = TID_NONE;
            __stk_full_memfence();
        }
    }
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_MUTEX_H_ */
