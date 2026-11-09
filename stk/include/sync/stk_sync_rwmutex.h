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

    \see   Mutex, ConditionVariable, ScopedTimedReadMutex, ScopedTimedLock
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
            : m_rw(rw), m_result(rw.TimedLockEx(timeout_ticks))
        {}

        ~ScopedTimedLock() { if (IsLocked()) { m_rw.Unlock(); } }

        /*! \return True if the write lock is held by this guard.
        */
        bool IsLocked() const { return (m_result == WAIT_RESULT_SIGNAL); }

        /*! \return Full outcome of the lock attempt (see RWMutex::TimedLockEx()).
        */
        EWaitResult GetResult() const { return m_result; }

    private:
        STK_NONCOPYABLE_CLASS(ScopedTimedLock);

        RWMutex    &m_rw;
        EWaitResult m_result;
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
            : m_rw(rw), m_result(rw.TimedReadLockEx(timeout_ticks))
        {}

        ~ScopedTimedReadMutex() { if (IsLocked()) { m_rw.ReadUnlock(); } }

        /*! \return True if the read lock is held by this guard.
        */
        bool IsLocked() const { return (m_result == WAIT_RESULT_SIGNAL); }

        /*! \return Full outcome of the lock attempt (see RWMutex::TimedReadLockEx()).
        */
        EWaitResult GetResult() const { return m_result; }

    private:
        STK_NONCOPYABLE_CLASS(ScopedTimedReadMutex);

        RWMutex    &m_rw;
        EWaitResult m_result;
    };

    /*! \brief     Acquire the lock for shared reading with a timeout, preserving the full wait outcome.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \note      Non-recursive.
        \return    \c WAIT_RESULT_SIGNAL if the lock was acquired (here "signal" means "lock acquired":
                   the predicate is re-checked after every wake), \c WAIT_RESULT_TIMEOUT if the timeout
                   expired (or \a NO_WAIT was passed and the lock was busy), \c WAIT_RESULT_CANCELED if
                   the wait was cancelled via IKernel::CancelTaskWait(), \c WAIT_RESULT_FAIL if the
                   kernel rejected the wait.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    EWaitResult TimedReadLockEx(Timeout timeout_ticks);

    /*! \brief     Acquire the lock for shared reading with a timeout.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \note      Non-recursive.
        \return    True if lock acquired, false otherwise (timeout, cancellation or NO_WAIT on a busy
                   lock). Use \c TimedReadLockEx() to tell these apart.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    bool TimedReadLock(Timeout timeout_ticks) { return (TimedReadLockEx(timeout_ticks) == WAIT_RESULT_SIGNAL); }

    /*! \brief     Acquire the lock for shared reading.
        \details   Blocks the calling task if a writer is currently active or if there
                   are writers waiting to acquire the lock.
        \note      Non-recursive.
        \note      Not cancelable: a \c void ReadLock() cannot report failure and must return with the
                   lock held, so a cancellation (IKernel::CancelTaskWait()) is absorbed and the wait is
                   retried. Use \c TimedReadLockEx() if cancellation must be observed. Panics
                   (KERNEL_PANIC_ASSERT) on any other failure.
        \warning   ISR-unsafe.
    */
    void ReadLock();

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

    /*! \brief     Acquire the lock for exclusive writing with a timeout, preserving the full wait outcome.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \return    \c WAIT_RESULT_SIGNAL if the lock was acquired (here "signal" means "lock acquired":
                   the predicate is re-checked after every wake), \c WAIT_RESULT_TIMEOUT if the timeout
                   expired (or \a NO_WAIT was passed and the lock was busy), \c WAIT_RESULT_CANCELED if
                   the wait was cancelled via IKernel::CancelTaskWait(), \c WAIT_RESULT_FAIL if the
                   kernel rejected the wait.
        \note      Non-recursive.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    EWaitResult TimedLockEx(Timeout timeout_ticks);

    /*! \brief     Acquire the lock for exclusive writing with a timeout.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \return    True if lock acquired, false otherwise (timeout, cancellation or NO_WAIT on a busy
                   lock). Use \c TimedLockEx() to tell these apart.
        \note      Non-recursive.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    bool TimedLock(Timeout timeout_ticks) { return (TimedLockEx(timeout_ticks) == WAIT_RESULT_SIGNAL); }

    /*! \brief     Acquire the lock for exclusive writing (IMutex interface).
        \details   Blocks the calling task until all active readers have released their
                   locks and no other writer is active.
        \note      Non-recursive.
        \note      Not cancelable: a \c void Lock() cannot report failure and must return with the
                   lock held, so a cancellation (IKernel::CancelTaskWait()) is absorbed and the wait is
                   retried. This also keeps the mutex re-acquisition at the end of
                   ConditionVariable::Wait() reliable (the kernel re-locks via this method).
                   Use \c TimedLockEx() if cancellation must be observed. Panics
                   (KERNEL_PANIC_ASSERT) on any other failure.
        \warning   ISR-unsafe.
    */
    void Lock() override;

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

inline EWaitResult RWMutex::TimedReadLockEx(Timeout timeout_ticks)
{
    EWaitResult result = WAIT_RESULT_SIGNAL;
    ScopedCriticalSection cs_;

    const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

    // capture an absolute deadline once, before entering the wait loop,
    // this prevents the timeout from being silently restarted on each
    // spurious wakeup; kept as 64-bit Ticks (not truncated to 32-bit Timeout) so that
    // the deadline and the current time stay comparable across the 32-bit tick wraparound
    const Ticks deadline = (timed_wait ?
        (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

    // wait if there is an active writer or if writers are waiting (Writer Preference)
    while (m_writer_active || (m_writers_waiting != 0U))
    {
        Timeout remaining = timeout_ticks;
        if (timed_wait)
        {
            // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
            const Ticks left = (deadline - GetTicks());
            remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
        }

        result = m_cv_readers.WaitEx(cs_, remaining);
        if (result != WAIT_RESULT_SIGNAL)
        {
            break; // timeout, cancellation or kernel failure: propagate to the caller
        }

        // re-check on wake: another writer may have queued up while this task was sleeping
    }

    // only increment reader count if the lock was successfully acquired
    if (result == WAIT_RESULT_SIGNAL)
    {
        STK_ASSERT(m_readers < READERS_MAX); // API contract: reader count must not exceed maximum

        m_readers = static_cast<uint16_t>(m_readers + 1U);
    }

    return result;
}

// ---------------------------------------------------------------------------
// ReadLock
// ---------------------------------------------------------------------------

inline void RWMutex::ReadLock()
{
    EWaitResult result;

    do
    {
        result = TimedReadLockEx(WAIT_INFINITE);
    }
    while (result == WAIT_RESULT_CANCELED);

    if (result != WAIT_RESULT_SIGNAL)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
    }
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

inline EWaitResult RWMutex::TimedLockEx(Timeout timeout_ticks)
{
    EWaitResult result = WAIT_RESULT_SIGNAL;
    ScopedCriticalSection cs_;

    STK_ASSERT(m_writers_waiting < WRITERS_MAX); // API contract: waiting writer count must not exceed maximum

    m_writers_waiting = static_cast<uint16_t>(m_writers_waiting + 1U);

    const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

    // capture an absolute deadline once, before entering the wait loop,
    // this prevents the timeout from being silently restarted on each
    // spurious wakeup; kept as 64-bit Ticks (not truncated to 32-bit Timeout) so that
    // the deadline and the current time stay comparable across the 32-bit tick wraparound
    const Ticks deadline = (timed_wait ?
        (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

    // wait until there are no active readers and no active writer
    while (m_writer_active || (m_readers != 0U))
    {
        Timeout remaining = timeout_ticks;
        if (timed_wait)
        {
            // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
            const Ticks left = (deadline - GetTicks());
            remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
        }

        result = m_cv_writers.WaitEx(cs_, remaining);
        if (result != WAIT_RESULT_SIGNAL)
        {
            // timeout, cancellation or kernel failure: withdraw from the waiting writers queue
            m_writers_waiting = static_cast<uint16_t>(m_writers_waiting - 1U);

            // readers may have been blocked only by this writer's preference: with no writer left
            // and none active, let them re-evaluate (otherwise they sleep until their own timeout,
            // or forever with WAIT_INFINITE)
            if (!m_writer_active)
            {
                if (m_writers_waiting == 0U)
                {
                    m_cv_readers.NotifyAll_CS();
                }
                // the withdrawing writer may have consumed a NotifyOne() meant for the next writer:
                // if the lock is free, pass the wake on (a spurious wake is harmless, the predicate
                // is re-checked by the woken writer)
                else if (m_readers == 0U)
                {
                    m_cv_writers.NotifyOne_CS();
                }
                else
                {
                    // readers still active, the last ReadUnlock() will wake the next writer
                }
            }

            break;
        }
    }

    // only finalize state if the wait didn't time out
    if (result == WAIT_RESULT_SIGNAL)
    {
        m_writers_waiting = static_cast<uint16_t>(m_writers_waiting - 1U);

        // kernel invariant: no readers and no active writer when lock is granted
        if ((m_readers != 0U) || m_writer_active)
        {
            STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
        }

        m_writer_active = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Lock
// ---------------------------------------------------------------------------

inline void RWMutex::Lock()
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
