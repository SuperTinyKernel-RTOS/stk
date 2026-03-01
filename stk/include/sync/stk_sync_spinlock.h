/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_SPINLOCK_H_
#define STK_SYNC_SPINLOCK_H_

#include "stk_common.h"

/*! \file  stk_sync_spinlock.h
    \brief Implementation of synchronization primitive: SpinLock.
*/

namespace stk {
namespace sync {

/*! \class SpinLock
    \brief Recursive spinlock with configurable spin count and automatic cooperative yielding.

    SpinLock is a high-performance synchronization primitive intended for extremely
    short critical sections where the overhead of a kernel context switch (as seen in Mutex)
    is undesirable.

    This implementation provides:
    - **Nesting (Recursion)**: Allows the owning thread to acquire the lock multiple times
      without deadlocking. Max recursion depth is \c 0xFFFE.
    - **Spin Count**: Limits the duration of busy-waiting. If the lock is not acquired
      within \a spin_count iterations, the task calls \c stk::Yield() to allow other
      tasks to run, preventing priority-based livelocks.

    \code
    // Example: Protecting high-frequency parameter updates
    stk::sync::SpinLock g_ParamLock(2000); // spin 2000 times before yielding

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
class SpinLock : public IMutex
{
public:
    /*! \brief     Construct a Spinlock.
        \param[in] spin_count: Number of iterations to spin before giving up/yielding.
        \note      Max value of spin_count is 0xFFFF.
    */
    explicit SpinLock(uint16_t spin_count = 4000)
        : m_owner_tid(0), m_spin_count(spin_count), m_recursion_count(0)
    {}

    /*! \brief    Acquire the lock, spinning up to m_spin_count times.
        \note     If spin count is exhausted, it performs a Yield to allow other tasks to run.
        \warning  ISR-unsafe.
    */
    void Lock();

    /*! \brief    Attempt to acquire lock immediately.
        \warning  ISR-unsafe.
        \return   True if acquired (or already owned), false if owned by another thread.
    */
    bool TryLock();

    /*! \brief    Release lock or decrement recursion count.
        \warning  ISR-unsafe.
    */
    void Unlock();

private:
    bool LockRecursively(TId locking_tid);
    void MakeLocked(TId locking_tid);

    hw::SpinLock m_lock;            //!< low-level spin lock
    TId          m_owner_tid;       //!< thread id of the current owner
    uint16_t     m_spin_count;      //!< max spins before yielding
    uint16_t     m_recursion_count; //!< nesting depth
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
    STK_ASSERT(!hw::IsInsideISR());

    // must be in a locked state
    STK_ASSERT(m_owner_tid == GetTid());
    STK_ASSERT(m_recursion_count != 0);

    if (--m_recursion_count == 0)
    {
        m_owner_tid = 0;
        __stk_full_memfence();

        m_lock.Unlock();
    }
}

// ---------------------------------------------------------------------------
// LockRecursively
// ---------------------------------------------------------------------------

inline bool SpinLock::LockRecursively(TId locking_tid)
{
    if ((m_owner_tid == locking_tid) && (m_recursion_count != 0))
    {
        ++m_recursion_count;
        STK_ASSERT(m_recursion_count < 0xFFFF);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// MakeLocked
// ---------------------------------------------------------------------------

inline void SpinLock::MakeLocked(TId locking_tid)
{
    // must not be in a locked state
    STK_ASSERT(m_owner_tid == 0);
    STK_ASSERT(m_recursion_count == 0);

    m_owner_tid       = locking_tid;
    m_recursion_count = 1;
    __stk_full_memfence();
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_SPINLOCK_H_ */
