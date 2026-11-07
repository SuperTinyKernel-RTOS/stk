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
class Mutex final : public SyncObjectBase, public IMutex, public ITraceable
{
public:
    static const uint16_t RECURSION_MAX = 0xFFFEU; //!< maximum nesting depth

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

    /*! \brief     Acquire lock, preserving the full wait outcome.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \note      Maximum number of recursive locks must not exceed 0xFFFEU.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if the lock was acquired (note: here "signal" means
                   "lock acquired", including the immediate and recursive paths where no
                   notification was involved), \c WAIT_RESULT_TIMEOUT if the lock was busy and the
                   timeout expired (or \a NO_WAIT was passed), \c WAIT_RESULT_CANCELED if the wait
                   was cancelled via IKernel::CancelTaskWait(), \c WAIT_RESULT_FAIL if the kernel
                   rejected the wait.
    */
    EWaitResult TimedLockEx(Timeout timeout_ticks);

    /*! \brief     Acquire lock.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \note      Maximum number of recursive locks must not exceed 0xFFFEU.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
        \return    True if lock acquired, false otherwise (timeout, cancellation or NO_WAIT on a busy
                   mutex). Use \c TimedLockEx() to tell these apart.
    */
    bool TimedLock(Timeout timeout_ticks) { return (TimedLockEx(timeout_ticks) == WAIT_RESULT_SIGNAL); }

    /*! \brief     Acquire lock.
        \note      Not cancelable: a \c void Lock() cannot report failure and must return with the
                   lock held, so a cancellation (IKernel::CancelTaskWait()) is absorbed and the wait is
                   retried. This also keeps the mutex re-acquisition at the end of
                   ConditionVariable::Wait() reliable (the kernel re-locks via this method).
                   Use \c TimedLockEx() if cancellation must be observed. Panics
                   (KERNEL_PANIC_ASSERT) on any other failure.
        \warning   ISR-unsafe.
    */
    void Lock() override;

    /*! \brief     Try to acquire the lock without blocking.
        \warning   ISR-safe.
        \return    True if lock acquired (including a recursive acquisition by the owner), false if
                   the lock is owned by another thread.
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

    /*! \brief     Get current recursion depth.
        \note      Reads the counter without a critical section. The value is only
                   stable for the caller if the caller is the owner (nobody else
                   modifies the counter while the lock is held); for a non-owner it
                   is a snapshot that may be stale immediately.
        \warning   ISR-safe.
        \return    0 if the mutex is free, otherwise the number of nested Lock() calls
                   made by the owner (1..RECURSION_MAX).
    */
    uint16_t GetRecursionCount() const { return m_recursion_count; }

private:
    STK_NONCOPYABLE_CLASS(Mutex);

    TId      m_owner_tid;       //!< thread id of the current owner
    uint16_t m_recursion_count; //!< recursion depth
};

// ---------------------------------------------------------------------------
// TimedLock
// ---------------------------------------------------------------------------

inline EWaitResult Mutex::TimedLockEx(Timeout timeout_ticks)
{
    IKernelService *const svc = IKernelService::GetInstance();
    const TId current_tid = svc->GetTid();

    ScopedCriticalSection cs_;

    const TId owner_tid = m_owner_tid;

    // NO_WAIT on a busy mutex is reported as an immediately-expired timeout
    EWaitResult result = WAIT_RESULT_TIMEOUT;

    STK_ASSERT(current_tid != TID_NONE); // API contract: must be called inside STK task

    // recursive path: already owned by the calling thread
    if ((m_recursion_count != 0U) && (owner_tid == current_tid))
    {
        STK_ASSERT(m_recursion_count < RECURSION_MAX); // API contract: caller must not exceed max recursion depth

        m_recursion_count = static_cast<uint16_t>(m_recursion_count + 1U);
        result = WAIT_RESULT_SIGNAL;
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

        result = WAIT_RESULT_SIGNAL;
    }
    // slow path: block until available or timeout expires
    else if (timeout_ticks != NO_WAIT)
    {
        STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR for a blocking call

        // boost priority of the owner to avoid priority inversion (in case of sched::SwitchStrategyFixedPriority,
        // otherwise ignored by the kernel), noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
        svc->InheritWeight(owner_tid, GetUserTaskFromTid(current_tid)->GetWeight());

        // mutex owned by another thread (slow path/blocking)
        result = svc->Wait(this, &cs_, timeout_ticks);

        if (result != WAIT_RESULT_SIGNAL)
        {
            // kernel invariant: a wait that did not end in a signal must not leave this thread as the
            // owner (Unlock() hands over ownership only together with a signal), otherwise the caller
            // would be told it failed while owning a lock nobody will release
            STK_ASSERT(m_owner_tid != current_tid);

            // if owner did not change, undo priority boost to avoid stuck elevated priority: lookup for a
            // higher weight within existing wait objects, noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
            if (owner_tid == m_owner_tid)
            {
                svc->RestoreWeight(owner_tid, this);
            }
        }
        else
        {
            // kernel invariant: if either condition is false, the low-level lock and the
            // recursion counter are out of sync, this is an internal defect, not a caller error
            if ((m_owner_tid != current_tid) || (m_recursion_count != 1U))
            {
                STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
            }
        }
    }
    // try-lock variant: owned by someone else, but no-wait requested
    else
    {
        // result is WAIT_RESULT_TIMEOUT already, noop
    }

    return result;
}

// ---------------------------------------------------------------------------
// Lock
// ---------------------------------------------------------------------------

inline void Mutex::Lock()
{
    EWaitResult result;

    do
    {
        result = TimedLockEx(WAIT_INFINITE);
    }
    while (result == WAIT_RESULT_CANCELED);

    if (result != WAIT_RESULT_SIGNAL)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
    }
}

// ---------------------------------------------------------------------------
// Unlock
// ---------------------------------------------------------------------------

inline void Mutex::Unlock()
{
    const ScopedCriticalSection cs_;

    STK_ASSERT((m_owner_tid == IKernelService::GetInstance()->GetTid()) &&
               (m_owner_tid != TID_NONE)); // API contract: caller must own the lock
    STK_ASSERT(m_recursion_count != 0U);   // API contract: must have matching Lock()

    m_recursion_count = static_cast<uint16_t>(m_recursion_count - 1U);

    if (m_recursion_count == 0U)
    {
        IKernelService *const svc = IKernelService::GetInstance();

        // restore priority of the owner, noop if ISwitchStrategy::PRIORITY_INHERITANCE_API = 0
        if (m_owner_tid != TID_NONE)
        {
            svc->RestoreWeight(m_owner_tid);
        }

        if (!m_wait_list.IsEmpty())
        {
            // pass ownership directly to the first waiter (FIFO order)
            IWaitObject *const waiter = detail::DListCast::ListEntryToParent<IWaitObject>(m_wait_list.GetFirst());

            // transfer ownership to the waiter
            m_recursion_count = 1U;
            m_owner_tid       = waiter->GetTid();
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
            // free completely if there are no waiters
            m_owner_tid = TID_NONE;
            __stk_full_memfence();
        }
    }
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_MUTEX_H_ */
