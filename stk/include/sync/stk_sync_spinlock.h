/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_SPINLOCK_H_
#define STK_SYNC_SPINLOCK_H_

#include "stk_helper.h"

/*! \file  stk_sync_spinlock.h
    \brief Implementation of synchronization primitive: stk::sync::SpinLock.
*/

namespace stk {
namespace sync {

/*! \class SpinLock
    \brief Recursive spinlock.

    SpinLock is a high-performance synchronization primitive intended for extremely
    short critical sections where the overhead of a kernel context switch (as seen in Mutex)
    is undesirable.

    This implementation provides:
    - **Nesting (Recursion)**: Allows the owning thread to acquire the lock multiple times
      without deadlocking. Max recursion depth is \c 0xFFFE.

    \code
    // Example: Protecting high-frequency parameter updates
    stk::sync::SpinLock g_ParamLock;

    void Param_Process_Task() {
        // acquisition is extremely fast if the lock is free
        g_Lock.Lock();

        // ... update parameters ...

        g_Lock.Unlock();
    }
    \endcode

    \warning While faster than a Mutex for uncontended or very short locks, prolonged spinning
             wastes CPU cycles. If critical section involves I/O or complex logic,
             prefer \a stk::sync::Mutex.
    \warning ISR-unsafe, for guarding code accessible by ISR use hw::CriticalSection instead.
    \see     IMutex, Mutex, ScopedCriticalSection, Yield
*/
class SpinLock final : public IMutex
{
public:
    /*! \brief    Construct a SpinLock in the unlocked state.
    */
    explicit SpinLock() : m_owner_tid(TID_NONE), m_recursion_count(0U)
    {}

    /*! \brief    Destructor.
        \note     If tasks are still waiting at destruction time it is considered a logical error (dangling waiters).
                  An assertion is triggered in debug builds.
        \note     MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~SpinLock()
    {
        STK_ASSERT(m_owner_tid == TID_NONE); // API contract: lock must not be destroyed while held
    }

    /*! \brief    Acquire the lock.
        \details  Blocks until the low-level lock is available, then takes ownership.
                  If the calling task already owns the lock, increments the recursion
                  counter and returns immediately without re-acquiring.
        \warning  ISR-unsafe.
    */
    void Lock();

    /*! \brief    Attempt to acquire the lock without blocking.
        \return   \c true if acquired (or already owned by the calling task);
                  \c false if currently owned by another task.
        \warning  ISR-unsafe.
    */
    bool TryLock();

    /*! \brief   Release the lock or decrement the recursion counter.
        \details If the calling task holds the lock recursively, decrements the
                 recursion counter and returns. On the final Unlock() the lock
                 is fully released and becomes available to other tasks.
        \warning ISR-unsafe.
    */
    void Unlock();

private:
    STK_NONCOPYABLE_CLASS(SpinLock);

    bool LockRecursively(TId locking_tid);
    void MakeLocked(TId locking_tid);

    static const uint16_t RECURSION_MAX = 0xFFFEU; //!< maximum nesting depth

    TId          m_owner_tid;       //!< thread id of the current owner
    uint16_t     m_recursion_count; //!< nesting depth
    hw::SpinLock m_lock;            //!< low-level spin lock
};

// ---------------------------------------------------------------------------
// Lock
// ---------------------------------------------------------------------------

inline void SpinLock::Lock()
{
    TId current_tid = GetTid();

    // increase recursion if this thread already owns the lock
    if (!LockRecursively(current_tid))
    {
        m_lock.Lock();
        MakeLocked(current_tid);
    }
}

// ---------------------------------------------------------------------------
// TryLock
// ---------------------------------------------------------------------------

inline bool SpinLock::TryLock()
{
    TId current_tid = GetTid();

    // increase recursion if this thread already owns the lock
    if (!LockRecursively(current_tid))
    {
        if (!m_lock.TryLock())
            return false;

        MakeLocked(current_tid);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Unlock
// ---------------------------------------------------------------------------

inline void SpinLock::Unlock()
{
    STK_ASSERT(!hw::IsInsideISR());      // API contract: caller must not be in ISR
    STK_ASSERT(m_owner_tid == GetTid()); // API contract: caller must own the lock
    STK_ASSERT(m_recursion_count != 0U); // API contract: must have matching Lock()

    if (--m_recursion_count == 0U)
    {
        m_owner_tid = TID_NONE;
        __stk_full_memfence();

        m_lock.Unlock();
    }
}

// ---------------------------------------------------------------------------
// LockRecursively
// ---------------------------------------------------------------------------

inline bool SpinLock::LockRecursively(TId locking_tid)
{
    if ((m_owner_tid == locking_tid) && (m_recursion_count != 0U))
    {
        STK_ASSERT(m_recursion_count < RECURSION_MAX); // API contract: caller must not exceed max recursion depth

        ++m_recursion_count;
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// MakeLocked
// ---------------------------------------------------------------------------

inline void SpinLock::MakeLocked(TId locking_tid)
{
    // kernel invariant: if either condition is false, the low-level lock and the
    // recursion counter are out of sync, this is an internal defect, not a caller error
    if ((m_owner_tid != TID_NONE) || (m_recursion_count != 0U))
        STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);

    m_owner_tid       = locking_tid;
    m_recursion_count = 1U;
    __stk_full_memfence();
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_SPINLOCK_H_ */
