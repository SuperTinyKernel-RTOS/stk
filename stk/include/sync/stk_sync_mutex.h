/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
    \brief Implementation of synchronization primitive: Mutex.
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
    \see  ISyncObject, IWaitObject, IKernelService::StartWaiting
*/
class Mutex : public IMutex, public ITraceable, private ISyncObject
{
public:
    /*! \brief     Constructor.
    */
    explicit Mutex() : m_owner_tid(0), m_count(0)
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error (dangling waiters).
                   An assertion is triggered in debug builds.
    */
    ~Mutex() { STK_ASSERT(m_wait_list.IsEmpty()); }

    /*! \brief     Acquire lock.
        \param[in] timeout: Maximum time to wait (ticks).
        \note      Maximum number of recursive locks must not exceed 65534 (UINT16_MAX - 1).
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
    bool Tick();

    TId      m_owner_tid; //!< thread id of the current owner
    uint16_t m_count;     //!< recursion depth
};

inline bool Mutex::TimedLock(Timeout timeout)
{
    // not supported inside ISR, may call StartWaiting
    STK_ASSERT(!hw::IsInsideISR());

    IKernelService *svc = IKernelService::GetInstance();
    TId current_tid = svc->GetTid();

    ScopedCriticalSection __cs;

    // already owned by the calling thread (recursive path)
    if ((m_count != 0) && (m_owner_tid == current_tid))
    {
        STK_ASSERT(m_count <= (UINT16_MAX - 1));

        ++m_count;
        return true;
    }

    // mutex is free (fast path)
    if (m_count == 0)
    {
        m_count     = 1;
        m_owner_tid = current_tid;
        __stk_full_memfence();

        return true;
    }

    // try lock behavior
    if (timeout == 0)
        return false;

    // mutex owned by another thread (slow path/blocking)
    IWaitObject *wo = svc->StartWaiting(this, &__cs, timeout);
    STK_ASSERT(wo != nullptr);

    if (wo->IsTimeout())
        return false;

    // mutex has been taken via Mutex::Unlock()
    STK_ASSERT(m_count == 1);
    STK_ASSERT(m_owner_tid == current_tid);

    return true;
}

inline void Mutex::Unlock()
{
    // can't be locked by ISR
    STK_ASSERT(!hw::IsInsideISR());

    ScopedCriticalSection __cs;

    // ensure the caller actually owns the mutex
    STK_ASSERT((m_count != 0) && (m_owner_tid == GetTid()));

    if (--m_count == 0)
    {
        if (!m_wait_list.IsEmpty())
        {
            // pass ownership directly to the first waiter (FIFO order)
            IWaitObject *waiter = static_cast<IWaitObject *>(m_wait_list.GetFirst());

            // transfer ownership to the waiter
            m_count     = 1;
            m_owner_tid = waiter->GetTid();
            __stk_full_memfence();

            waiter->Wake(false);
        }
        else
        {
            // free completely if there are no waiters
            m_owner_tid = 0;
            __stk_full_memfence();
        }
    }
}

inline bool Mutex::Tick()
{
    // required for multi-core CPU and multiple instances of STK (one per core)
#if (_STK_ARCH_CPU_COUNT > 1)
    ScopedCriticalSection __cs;
#endif

    return ISyncObject::Tick();
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_MUTEX_H_ */
