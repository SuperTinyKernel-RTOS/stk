/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_FASTMUTEX_H_
#define STK_SYNC_FASTMUTEX_H_

#include "stk_sync_cs.h"

/*! \file  stk_sync_fastmutex.h
    \brief Implementation of synchronization primitive: stk::sync::FastMutex.
*/

namespace stk {
namespace sync {

/*! \class FastMutex
    \brief Non-recursive (binary) mutex primitive with ownership tracking.

    Unlike stk::sync::Mutex, FastMutex does not keep a recursion counter: the only state is
    the owner thread id (\c TID_NONE means the mutex is free). This makes the lock/unlock paths
    shorter and reduces the object size.

    A thread that already owns the mutex must not lock it again. Doing so is an unrecoverable
    contract violation: the kernel panics with \c KERNEL_PANIC_SYNC_DEADLOCK (in all build
    configurations, from \c Lock(), \c TryLock() and \c TimedLock()) instead of dead-locking
    on itself or failing silently.

    Ownership is handed over directly to the first waiter (FIFO) on \c Unlock(), and priority
    inheritance is supported in the same way as in stk::sync::Mutex.

    \code
    stk::sync::FastMutex g_ResourceMtx;

    void Task_Func() {
        if (g_ResourceMtx.TimedLock(100)) {
            // ... access shared resource, do NOT call Lock() again here ...
            g_ResourceMtx.Unlock();
        }
    }
    \endcode

    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
    \see  Mutex, ISyncObject, IWaitObject, IKernelService::Wait
*/
class FastMutex final : private SyncObjectBase, public IMutex, public ITraceable
{
public:
    /*! \brief     Constructor.
    */
    explicit FastMutex() : m_owner_tid(TID_NONE)
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error (dangling waiters).
                   An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~FastMutex()
    {
        STK_ASSERT(m_wait_list.IsEmpty()); // API contract: must not be destroyed with waiting tasks
    }

    /*! \brief     Acquire lock.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \note      Not recursive: locking a mutex already owned by the caller is a contract violation
                   that triggers STK_KERNEL_PANIC(KERNEL_PANIC_SYNC_DEADLOCK) in all build configurations.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
        \return    True if lock acquired, false if timeout occurred.
    */
    bool TimedLock(Timeout timeout_ticks);

    /*! \brief     Acquire lock.
        \warning   ISR-unsafe.
    */
    void Lock() override { STK_UNUSED(TimedLock(WAIT_INFINITE)); }

    /*! \brief     Try to acquire the lock without blocking.
        \warning   ISR-safe.
        \note      Panics (KERNEL_PANIC_SYNC_DEADLOCK) if the caller already owns the lock.
        \return    True if lock acquired, false if the lock is owned by another thread.
    */
    bool TryLock() { return TimedLock(NO_WAIT); }

    /*! \brief     Release lock.
        \warning   ISR-safe.
    */
    void Unlock() override;

    /*! \brief     Get owner of the mutex.
        \warning   ISR-safe.
        \return    Thread id of the owner, or TID_NONE if the mutex is free.
    */
    TId GetOwner() const { return m_owner_tid; }

    /*! \brief     Check if the mutex is currently locked.
        \note      Snapshot only, may be stale immediately for a non-owner.
        \warning   ISR-safe.
    */
    bool IsLocked() const { return (m_owner_tid != TID_NONE); }

private:
    STK_NONCOPYABLE_CLASS(FastMutex);

    TId m_owner_tid; //!< thread id of the current owner (TID_NONE if free)
};

// ---------------------------------------------------------------------------
// TimedLock
// ---------------------------------------------------------------------------

inline bool FastMutex::TimedLock(Timeout timeout_ticks)
{
    IKernelService *const svc = IKernelService::GetInstance();
    const TId current_tid = svc->GetTid();

    ScopedCriticalSection cs_;

    const TId owner_tid = m_owner_tid;
    bool success = false;

    STK_ASSERT(current_tid != TID_NONE); // API contract: must be called inside STK task

    // fast path: mutex is free
    if (owner_tid == TID_NONE)
    {
        m_owner_tid = current_tid;
        __stk_full_memfence();

        success = true;
    }
    // self-lock: non-recursive mutex, the caller would dead-lock on itself (or silently continue without
    // owning a fresh lock): unrecoverable API contract violation, FastMutex is not recursive
    else if (owner_tid == current_tid)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_SYNC_DEADLOCK);
    }
    // slow path: block until available or timeout expires
    else if (timeout_ticks != NO_WAIT)
    {
        STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR for a blocking call

        // boost priority of the owner to avoid priority inversion (in case of SwitchStrategyFixedPriority,
        // otherwise ignored by the kernel), noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
        svc->InheritWeight(owner_tid, GetUserTaskFromTid(current_tid)->GetWeight());

        // mutex owned by another thread (slow path/blocking)
        if (svc->Wait(this, &cs_, timeout_ticks) != WAIT_RESULT_SIGNAL)
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
            // kernel invariant: ownership must have been transferred to this thread by Unlock(),
            // otherwise this is an internal defect, not a caller error
            if (m_owner_tid != current_tid)
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

inline void FastMutex::Unlock()
{
    const ScopedCriticalSection cs_;

    STK_ASSERT((m_owner_tid == IKernelService::GetInstance()->GetTid()) &&
               (m_owner_tid != TID_NONE)); // API contract: caller must own the lock

    if (!m_wait_list.IsEmpty())
    {
        // slow path: contended unlock, hand over to the first waiter
        IKernelService *const svc = IKernelService::GetInstance();

        // restore priority of the owner (it could have been boosted by waiters of this mutex),
        // noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0; not needed if there are no waiters
        // because a boost is undone by the waiter itself on timeout/cancellation
        if (m_owner_tid != TID_NONE)
        {
            svc->RestoreWeight(m_owner_tid);
        }

        // pass ownership directly to the first waiter (FIFO order)
        IWaitObject *const waiter = util::DListCast::ListEntryToParent<IWaitObject>(m_wait_list.GetFirst());

        // transfer ownership to the waiter
        m_owner_tid = waiter->GetTid();
        __stk_full_memfence();

        // wake up
        waiter->Wake(false);

        // boost priority from the highest-priority task still in wait list, skip the lookup if none left,
        // noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
        if (!m_wait_list.IsEmpty())
        {
            svc->InheritWeight(m_owner_tid,
                FindWeightHigherThan(GetUserTaskFromTid(m_owner_tid)->GetWeight()));
        }
    }
    else
    {
        // fast path: free completely if there are no waiters
        m_owner_tid = TID_NONE;
        __stk_full_memfence();
    }
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_FASTMUTEX_H_ */
