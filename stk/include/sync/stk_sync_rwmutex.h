/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_RWMUTEX_H_
#define STK_SYNC_RWMUTEX_H_

#include "stk_sync_cv.h"

/*! \file  stk_sync_rwmutex.h
    \brief Implementation of synchronization primitive: stk::sync::RWMutex.
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

    \note  Maximum number of concurrent readers must not exceed READERS_MAX (0xFFFE).
           Maximum number of waiting writers must not exceed WRITERS_MAX (0xFFFE).

    \note  Non-recursive: a task must not call ReadLock() or Lock() more than once
           without a matching unlock. Doing so will deadlock.

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
class RWMutex final : public IMutex, public ITraceable
{
public:
    /*! \brief  Construct an RWMutex in the unlocked state with no active readers or writers.
    */
    explicit RWMutex() : m_readers(0U), m_writers_waiting(0U), m_writer_active(false)
    {}

    /*! \brief  Destructor.
        \note   Destroying an RWMutex while readers are active, writers are waiting,
                or a writer holds the lock is a logical error (dangling state).
                An assertion is triggered in debug builds.
        \note   MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~RWMutex()
    {
        // API contract: must not be destroyed while readers are active, writers are waiting,
        // or a writer holds the lock
        STK_ASSERT((m_readers == 0U) && (m_writers_waiting == 0U) && !m_writer_active);
    }

    /*! \class ScopedTimedLock
        \brief RAII wrapper for attempting exclusive write access with a timeout.
    */
    class ScopedTimedLock
    {
    public:
        /*! \brief     Constructs the guard and attempts to acquire the write lock.
            \param[in] rw: Reference to the RWMutex.
            \param[in] timeout_ticks: Maximum time to wait (ticks). Use NO_WAIT for non-blocking (TryLock).
        */
        explicit ScopedTimedLock(RWMutex &rw, Timeout timeout_ticks = WAIT_INFINITE)
            : m_rw(rw), m_locked(rw.TimedLock(timeout_ticks))
        {}

        ~ScopedTimedLock() { if (m_locked) { m_rw.Unlock(); } }

        bool IsLocked() const { return m_locked; }

    private:
        STK_NONCOPYABLE_CLASS(ScopedTimedLock);

        RWMutex &m_rw;
        bool     m_locked;
    };

    /*! \class ScopedTimedReadMutex
        \brief RAII wrapper for attempting shared read access with a timeout.
    */
    class ScopedTimedReadMutex
    {
    public:
        /*! \brief     Constructs the guard and attempts to acquire the read lock.
            \param[in] rw: Reference to the RWMutex.
            \param[in] timeout_ticks: Maximum time to wait (ticks). Use NO_WAIT for non-blocking (TryReadLock).
        */
        explicit ScopedTimedReadMutex(RWMutex &rw, Timeout timeout_ticks = WAIT_INFINITE)
            : m_rw(rw), m_locked(rw.TimedReadLock(timeout_ticks))
        {}

        ~ScopedTimedReadMutex() { if (m_locked) { m_rw.ReadUnlock(); } }

        bool IsLocked() const { return m_locked; }

    private:
        STK_NONCOPYABLE_CLASS(ScopedTimedReadMutex);

        RWMutex &m_rw;
        bool     m_locked;
    };

    /*! \brief     Acquire the lock for shared reading with a timeout.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \note      Non-recursive.
        \return    True if lock acquired, false if timeout occurred.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    bool TimedReadLock(Timeout timeout_ticks);

    /*! \brief     Acquire the lock for shared reading.
        \details   Blocks the calling task if a writer is currently active or if there
                   are writers waiting to acquire the lock.
        \note      Non-recursive.
        \warning   ISR-unsafe.
    */
    void ReadLock() { STK_UNUSED(TimedReadLock(WAIT_INFINITE)); }

    /*! \brief     Attempt to acquire the lock for shared reading without blocking.
        \details   Checks if a writer is active or waiting. If the resource is available
                   for reading, it increments the reader count and returns immediately.
        \note      Non-recursive.
        \return    True if the read lock was acquired, false if a writer is active or waiting.
        \warning   ISR-safe.
    */
    bool TryReadLock() { return TimedReadLock(NO_WAIT); }

    /*! \brief     Release the shared reader lock.
        \details   Decrements the reader count. If this was the last active reader,
                   notifies any waiting writers.
        \warning   ISR-safe.
    */
    void ReadUnlock();

    /*! \brief     Acquire the lock for exclusive writing with a timeout.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \return    True if lock acquired, false if timeout occurred.
        \note      Non-recursive.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    bool TimedLock(Timeout timeout_ticks);

    /*! \brief     Acquire the lock for exclusive writing (IMutex interface).
        \details   Blocks the calling task until all active readers have released their
                   locks and no other writer is active.
        \note      Non-recursive.
        \warning   ISR-safe.
    */
    void Lock() override { STK_UNUSED(TimedLock(WAIT_INFINITE)); }

    /*! \brief     Attempt to acquire the lock for exclusive writing without blocking.
        \details   Checks if any readers are active or if another writer is active.
        \note      Non-recursive.
        \return    True if the exclusive lock was acquired, false otherwise.
        \warning   ISR-safe.
    */
    bool TryLock() { return TimedLock(NO_WAIT); }

    /*! \brief     Release the exclusive writer lock (IMutex interface).
        \details   Releases the lock and prioritizes waking waiting writers. If no
                   writers are waiting, wakes all waiting readers.
        \warning   ISR-safe.
    */
    void Unlock() override;

private:
    STK_NONCOPYABLE_CLASS(RWMutex);

    static const uint16_t READERS_MAX = 0xFFFEU; //!< maximum number of concurrent readers
    static const uint16_t WRITERS_MAX = 0xFFFEU; //!< maximum number of waiting writers

    ConditionVariable m_cv_readers;      //!< signaled when readers can proceed
    ConditionVariable m_cv_writers;      //!< signaled when a writer can proceed
    uint16_t          m_readers;         //!< current active reader count
    uint16_t          m_writers_waiting; //!< count of writers waiting for access
    bool              m_writer_active;   //!< true if a writer currently holds the lock
};

// ---------------------------------------------------------------------------
// TimedReadLock
// ---------------------------------------------------------------------------

inline bool RWMutex::TimedReadLock(Timeout timeout_ticks)
{
    bool success = true;
    ScopedCriticalSection cs_;

    // wait if there is an active writer or if writers are waiting (Writer Preference)
    while (m_writer_active || (m_writers_waiting != 0U))
    {
        if (!m_cv_readers.Wait(cs_, timeout_ticks))
        {
            success = false; // timeout
            break;
        }

        // re-check on wake: another writer may have queued up while this task was sleeping
    }

    // only increment reader count if the lock was successfully acquired without timing out
    if (success)
    {
        STK_ASSERT(m_readers < READERS_MAX); // API contract: reader count must not exceed maximum

        m_readers = static_cast<uint16_t>(m_readers + 1U);
    }

    return success;
}

// ---------------------------------------------------------------------------
// ReadUnlock
// ---------------------------------------------------------------------------

inline void RWMutex::ReadUnlock()
{
    const ScopedCriticalSection cs_;

    STK_ASSERT(m_readers != 0U); // API contract: must have a matching ReadLock()

    m_readers = static_cast<uint16_t>(m_readers - 1U);

    // wake a waiting writer when the last reader exits
    if (m_readers == 0U)
    {
        m_cv_writers.NotifyOne_CS();
    }
}

// ---------------------------------------------------------------------------
// TimedLock
// ---------------------------------------------------------------------------

inline bool RWMutex::TimedLock(Timeout timeout_ticks)
{
    bool success = true;
    ScopedCriticalSection cs_;

    STK_ASSERT(m_writers_waiting < WRITERS_MAX); // API contract: waiting writer count must not exceed maximum

    m_writers_waiting = static_cast<uint16_t>(m_writers_waiting + 1U);

    // wait until there are no active readers and no active writer
    while (m_writer_active || (m_readers != 0U))
    {
        if (!m_cv_writers.Wait(cs_, timeout_ticks))
        {
            // timed out: withdraw from the waiting writers queue
            m_writers_waiting = static_cast<uint16_t>(m_writers_waiting - 1U);
            success = false;
            break;
        }
    }

    // only finalize state if the wait didn't time out
    if (success)
    {
        m_writers_waiting = static_cast<uint16_t>(m_writers_waiting - 1U);

        // kernel invariant: no readers and no active writer when lock is granted
        if ((m_readers != 0U) || m_writer_active)
        {
            STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
        }

        m_writer_active = true;
    }

    return success;
}

// ---------------------------------------------------------------------------
// Unlock
// ---------------------------------------------------------------------------

inline void RWMutex::Unlock()
{
    const ScopedCriticalSection cs_;

    STK_ASSERT(m_writer_active); // API contract: caller must hold the write lock

    m_writer_active = false;

    // prioritize waking waiting writers to prevent writer starvation;
    // only wake readers if no writers are queued
    if (m_writers_waiting != 0U)
    {
        m_cv_writers.NotifyOne_CS();
    }
    else
    {
        m_cv_readers.NotifyAll_CS();
    }
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_RWMUTEX_H_ */
