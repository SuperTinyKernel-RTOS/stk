/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_RWMUTEX_H_
#define STK_SYNC_RWMUTEX_H_

#include "stk_common.h"
#include "stk_sync_cs.h"
#include "stk_sync_cv.h"

/*! \file  stk_sync_rwmutex.h
    \brief Implementation of synchronization primitive: Read-Write Mutex.
*/

namespace stk {
namespace sync {

/*! \class RWMutex
    \brief Reader-Writer Lock synchronization primitive for non-recursive shared and exclusive access.

    RWMutex allows multiple tasks to read a shared resource simultaneously (shared access)
    while ensuring that only one task can write to the resource at a time (exclusive access).
    This is particularly efficient for data structures that are read frequently but
    modified infrequently.

    \note  **Writer Preference Policy**: To prevent "writer starvation," this implementation
           prioritizes waiting writers. If a writer is waiting for the lock, new readers
           will be blocked until the writer has completed its operation.

    \note Maximum number of waiting tasks must not exceed 65534 (UINT16_MAX - 1).

    \note It is non-recursive implementation.

    \code
    // Example: Protecting app settings
    stk::sync::RWMutex g_SettingsLock;
    Settings           g_Settings;

    void Engine_Task() {
        // Multiple engine instances can read settings concurrently
        stk::sync::RWMutex::ScopedTimedReadMutex guard(g_SettingsLock);
        Apply(g_Settings);
    }

    void UI_Control_Task() {
        // Exclusive access to update settings
        g_SettingsLock.Lock();
        g_Settings.volume = new_volume;
        g_SettingsLock.Unlock();
    }
    \endcode

    \see   Mutex, ConditionVariable, ScopedReadMutex
*/
class RWMutex : public IMutex, public ITraceable
{
public:
    /*! \brief     Constructor.
    */
    explicit RWMutex() : m_readers(0), m_writers_waiting(0), m_writer_active(false)
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error (dangling waiters).
                   An assertion is triggered in debug builds.
    */
    ~RWMutex() { STK_ASSERT((m_readers == 0) && (m_writers_waiting == 0) && !m_writer_active); }

    /*! \class ScopedTimedLock
        \brief RAII wrapper for attempting exclusive write access with a timeout.
    */
    class ScopedTimedLock
    {
    public:
        /*! \brief  Constructs the guard and attempts to acquire the write lock.
            \param[in] rw: Reference to the RWMutex.
            \param[in] timeout: Maximum time to wait (ticks). Use NO_WAIT for non-blocking (TryLock).
        */
        explicit ScopedTimedLock(RWMutex &rw, Timeout timeout = WAIT_INFINITE)
            : m_rw(rw), m_locked(rw.TimedLock(timeout))
        {}
        ~ScopedTimedLock() { if (m_locked) m_rw.Unlock(); }

        bool IsLocked() const { return m_locked; }

    private:
        RWMutex &m_rw;
        bool     m_locked;
    };

    /*! \class ScopedTimedReadMutex
        \brief RAII wrapper for attempting shared read access with a timeout.
    */
    class ScopedTimedReadMutex {
    public:
        /*! \brief  Constructs the guard and attempts to acquire the read lock.
            \param[in] rw: Reference to the RWMutex.
            \param[in] timeout: Maximum time to wait (ticks). Use NO_WAIT for non-blocking (TryReadLock).
        */
        explicit ScopedTimedReadMutex(RWMutex &rw, Timeout timeout = WAIT_INFINITE)
            : m_rw(rw), m_locked(rw.TimedReadLock(timeout))
        {}
        ~ScopedTimedReadMutex() { if (m_locked) m_rw.ReadUnlock(); }

        bool IsLocked() const { return m_locked; }

    private:
        RWMutex &m_rw;
        bool     m_locked;
    };

    /*! \brief     Acquire the lock for shared reading with a timeout.
        \param[in] timeout: Maximum time to wait (ticks).
        \note      Non-recursive.
        \return    True if lock acquired, false if timeout occurred.
        \warning   ISR-unsafe.
    */
    bool TimedReadLock(Timeout timeout);

    /*! \brief     Acquire the lock for shared reading.
        \details   Blocks the calling task if a writer is currently active or if there
                   are writers waiting to acquire the lock.
        \note      Non-recursive.
        \warning   ISR-unsafe.
    */
    void ReadLock() { (void)TimedReadLock(WAIT_INFINITE); }

    /*! \brief     Attempt to acquire the lock for shared reading without blocking.
        \details   Checks if a writer is active or waiting. If the resource is available
                   for reading, it increments the reader count and returns immediately.
        \note      Non-recursive.
        \return    True if the read lock was acquired, false if a writer is active or waiting.
        \warning   ISR-unsafe.
    */
    bool TryReadLock() { return TimedReadLock(NO_WAIT); }

    /*! \brief     Release the shared reader lock.
        \details   Decrements the reader count. If this was the last active reader,
                   it will notify any waiting writers.
        \warning   ISR-unsafe.
    */
    void ReadUnlock();

    /*! \brief     Acquire the lock for exclusive writing with a timeout.
        \param[in] timeout: Maximum time to wait (ticks).
        \return    True if lock acquired, false if timeout occurred.
        \note      Non-recursive.
        \warning   ISR-unsafe.
    */
    bool TimedLock(Timeout timeout);

    /*! \brief     Acquire the lock for exclusive writing (IMutex interface).
        \details   Blocks the calling task until all active readers have released their
                   locks and no other writer is active.
        \note      Non-recursive.
        \warning   ISR-unsafe.
    */
    void Lock() { (void)TimedLock(WAIT_INFINITE); }

    /*! \brief     Attempt to acquire the lock for exclusive writing without blocking.
        \details   Checks if any readers are active or if another writer is active.
        \note      Non-recursive.
        \return    True if the exclusive lock was acquired, false otherwise.
        \warning   ISR-unsafe.
    */
    bool TryLock() { return TimedLock(NO_WAIT); }

    /*! \brief     Release the exclusive writer lock (IMutex interface).
        \details   Releases the lock and prioritizes waking waiting writers. If no
                   writers are waiting, it wakes all waiting readers.
        \warning   ISR-unsafe.
    */
    void Unlock();

private:
    STK_NONCOPYABLE_CLASS(RWMutex);

    ConditionVariable m_cv_readers;      //!< signaled when readers can proceed
    ConditionVariable m_cv_writers;      //!< signaled when a writer can proceed
    uint16_t          m_readers;         //!< current active readers
    uint16_t          m_writers_waiting; //!< count of writers waiting for access
    bool              m_writer_active;   //!< true if a writer currently holds the lock
};

// ---------------------------------------------------------------------------
// TimedReadLock
// ---------------------------------------------------------------------------

inline bool RWMutex::TimedReadLock(Timeout timeout)
{
    // not supported inside ISR
    STK_ASSERT(!hw::IsInsideISR());

    ScopedCriticalSection __cs;

    // wait if there is an active writer or if writers are waiting
    while (m_writer_active || (m_writers_waiting != 0))
    {
        if (!m_cv_readers.Wait(__cs, timeout))
            return false; // timeout

        // after waking, the loop re-checks the condition because another writer
        // might have queued up in the meantime (Writer Preference).
    }

    STK_ASSERT(m_readers < (UINT16_MAX - 1));

    ++m_readers;
    return true;
}

// ---------------------------------------------------------------------------
// ReadUnlock
// ---------------------------------------------------------------------------

inline void RWMutex::ReadUnlock()
{
    // mutex operations are not supported inside ISR
    STK_ASSERT(!hw::IsInsideISR());

    ScopedCriticalSection __cs;

    STK_ASSERT(m_readers != 0);

    // wake a waiting writer if this was the last reader
    if (--m_readers == 0)
        m_cv_writers.NotifyOne();
}

// ---------------------------------------------------------------------------
// TimedLock
// ---------------------------------------------------------------------------

inline bool RWMutex::TimedLock(Timeout timeout)
{
    // mutex operations are not supported inside ISR
    STK_ASSERT(!hw::IsInsideISR());

    ScopedCriticalSection __cs;

    STK_ASSERT(m_writers_waiting < (UINT16_MAX - 1));

    ++m_writers_waiting;

    // wait until there are no active readers and no active writer
    while (m_writer_active || (m_readers != 0))
    {
        if (!m_cv_writers.Wait(__cs, timeout))
        {
            // decrement waiting count on timeout and return
            --m_writers_waiting;
            return false;
        }
    }

    --m_writers_waiting;
    m_writer_active = true;
    return true;
}

// ---------------------------------------------------------------------------
// Unlock
// ---------------------------------------------------------------------------

inline void RWMutex::Unlock()
{
    // mutex operations are not supported inside ISR
    STK_ASSERT(!hw::IsInsideISR());

    ScopedCriticalSection __cs;

    m_writer_active = false;

    // prioritize waking waiting writers, otherwise wake all readers
    if (m_writers_waiting != 0)
        m_cv_writers.NotifyOne();
    else
        m_cv_readers.NotifyAll();
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_RWMUTEX_H_ */
