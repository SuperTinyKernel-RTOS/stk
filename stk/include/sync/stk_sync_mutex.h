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
class Mutex final : private ISyncObject, public IMutex, public ITraceable
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
    STK_VIRT_DTOR ~Mutex()
    {
        STK_ASSERT(m_wait_list.IsEmpty()); // API contract: must not be destroyed with waiting tasks
    }

    /*! \brief     Acquire lock.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \note      Maximum number of recursive locks must not exceed 0xFFFEU.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
        \return    True if lock acquired, false if timeout occurred.
    */
    bool TimedLock(Timeout timeout_ticks);

    /*! \brief     Acquire lock.
        \warning   ISR-unsafe.
    */
    void Lock() override { STK_UNUSED(TimedLock(WAIT_INFINITE)); }

    /*! \brief     Acquire the lock.
        \warning   ISR-safe.
        \return    True if lock acquired, false if lock is already acquired by another task.
    */
    bool TryLock() { return TimedLock(NO_WAIT); }

    /*! \brief     Release lock.
        \warning   ISR-safe.
    */
    void Unlock() override;

    /*! \brief     Get owner of the mutex.
        \warning   ISR-safe.
    */
    TId GetOwner() const { return m_owner_tid; }

private:
    STK_NONCOPYABLE_CLASS(Mutex);

    static const uint16_t RECURSION_MAX = 0xFFFEU; //!< maximum nesting depth

    TId      m_owner_tid;       //!< thread id of the current owner
    uint16_t m_recursion_count; //!< recursion depth
};

// ---------------------------------------------------------------------------
// TimedLock
// ---------------------------------------------------------------------------

inline bool Mutex::TimedLock(Timeout timeout_ticks)
{
    IKernelService *const svc = IKernelService::GetInstance();
    const TId current_tid = svc->GetTid();

    ScopedCriticalSection cs_;

    const TId owner_tid = m_owner_tid;
    bool success = false;

    // recursive path: already owned by the calling thread
    if ((m_recursion_count != 0U) && (owner_tid == current_tid))
    {
        STK_ASSERT(m_recursion_count < RECURSION_MAX); // API contract: caller must not exceed max recursion depth

        m_recursion_count = static_cast<uint16_t>(m_recursion_count + 1U);
        success = true;
    }
    // fast path: mutex is free
    else if (m_recursion_count == 0U)
    {
        // kernel invariant: counter is zero so owner must be TID_NONE
        if (owner_tid != TID_NONE)
        {
            STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
        }

        m_recursion_count = 1U;
        m_owner_tid       = current_tid;
        __stk_full_memfence();

        success = true;
    }
    // slow path: block until available or timeout expires
    else if (timeout_ticks != NO_WAIT)
    {
        STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR for a blocking call

        // boost priority of the owner to avoid priority inversion (in case of SwitchStrategyFixedPriority,
        // otherwise ignored by the kernel), noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
        svc->InheritWeight(owner_tid, GetUserTaskFromTid(current_tid)->GetWeight());

        // mutex owned by another thread (slow path/blocking)
        if (svc->Wait(this, &cs_, timeout_ticks)->IsTimeout())
        {
            // if owner did not change, undo priority boost to avoid stuck elevated priority: lookup for a
            // higher weight within existing wait objects, noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
            if (owner_tid == m_owner_tid)
            {
                svc->RestoreWeight(owner_tid, this);
            }

            success = false;
        }
        else
        {
            // kernel invariant: if either condition is false, the low-level lock and the
            // recursion counter are out of sync, this is an internal defect, not a caller error
            if ((m_owner_tid != current_tid) || (m_recursion_count != 1U))
            {
                STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
            }

            success = true;
        }
    }
    // try-lock variant: owned by someone else, but no-wait requested
    else
    {
        // success is false already, noop
    }

    return success;
}    

// ---------------------------------------------------------------------------
// Unlock
// ---------------------------------------------------------------------------

inline void Mutex::Unlock()
{
    const ScopedCriticalSection cs_;

    STK_ASSERT(m_owner_tid == GetTid()); // API contract: caller must own the lock
    STK_ASSERT(m_recursion_count != 0U); // API contract: must have matching Lock()

    m_recursion_count = static_cast<uint16_t>(m_recursion_count - 1U);

    if (m_recursion_count == 0U)
    {
        IKernelService *const svc = IKernelService::GetInstance();

        // restore priority of the owner, noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
        svc->RestoreWeight(m_owner_tid);

        if (!m_wait_list.IsEmpty())
        {
            // pass ownership directly to the first waiter (FIFO order)
            IWaitObject *const waiter = static_cast<IWaitObject *>(m_wait_list.GetFirst());

            // transfer ownership to the waiter
            m_recursion_count = 1U;
            m_owner_tid       = waiter->GetTid();
            __stk_full_memfence();

            // wake up
            waiter->Wake(false);

            // boost priority from the highest-priority task currently in wait list,
            // noop if the list is empty or if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
            svc->InheritWeight(m_owner_tid,
                FindWeightHigherThan(GetUserTaskFromTid(m_owner_tid)->GetWeight()));
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
