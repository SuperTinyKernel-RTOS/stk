/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_EVENT_FLAGS_H_
#define STK_SYNC_EVENT_FLAGS_H_

#include "stk_sync_cv.h"

/*! \file  stk_sync_event_flags.h
    \brief Implementation of synchronization primitive: EventFlags.
*/

namespace stk {
namespace sync {

/*! \class EventFlags
    \brief 32-bit event flags group for multi-flag synchronization between tasks.

    EventFlags maintains a 32-bit word where each bit represents an independent
    boolean event. Tasks can set or clear any combination of flags, and wait for
    any subset to become set — either requiring \b all requested flags (AND
    semantics) or \b any one of them (OR semantics).

    Unlike \c Event, which models a single binary signal, \c EventFlags allows
    fine-grained coordination across up to 32 independent conditions in a single
    object.

    \note  **Wait semantics**: the caller selects the mode via the \a options
           parameter using \c WAIT_ANY (default) or \c WAIT_ALL.
    \note  **Auto-clear**: by default the matched flags are atomically cleared
           upon a successful \c Wait(). Pass \c NO_CLEAR to suppress this.
    \note  **Direct Handover**: \c Set() notifies \b all waiters so that every
           task re-evaluates its own predicate after waking. Each waiter clears
           only the flags it matched, so concurrent waiters with disjoint
           flag masks do not interfere with each other.
    \note  Maximum flag bit index is 30 (bits 0..30). Bit 31 is reserved for
           future error reporting and must never be set by the caller.

    \code
    // Example: sensor fusion — wait for GPS and IMU data to both arrive
    stk::sync::EventFlags g_SensorFlags;

    static const uint32_t FLAG_GPS = (1U << 0);
    static const uint32_t FLAG_IMU = (1U << 1);

    void ISR_GPS() {
        g_SensorFlags.Set(FLAG_GPS);   // ISR-safe
    }

    void ISR_IMU() {
        g_SensorFlags.Set(FLAG_IMU);   // ISR-safe
    }

    void Task_Fusion() {
        // block until BOTH flags are set simultaneously; clears them on return
        uint32_t raised = g_SensorFlags.Wait(FLAG_GPS | FLAG_IMU,
                                             EventFlags::WAIT_ALL, 5000);
        if (raised & EventFlags::ERROR_TIMEOUT) {
            // handle timeout
        } else {
            // process fused sensor data
        }
    }
    \endcode

    \see  Event, Semaphore, ConditionVariable
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
class EventFlags : public ITraceable
{
public:

    // -----------------------------------------------------------------------
    // Options constants (bitmask, passed to Wait())
    // -----------------------------------------------------------------------

    /*! \brief Wait for ANY of the specified flags to be set (OR semantics, default). */
    static const uint32_t OPT_WAIT_ANY = 0x00000000U;

    /*! \brief Wait for ALL of the specified flags to be set simultaneously (AND semantics). */
    static const uint32_t OPT_WAIT_ALL = 0x00000001U;

    /*! \brief Do not clear matched flags after a successful wait. */
    static const uint32_t OPT_NO_CLEAR = 0x00000002U;

    // -----------------------------------------------------------------------
    // Return-value error sentinels (bit 31 is the error indicator)
    // -----------------------------------------------------------------------

    /*! \brief Return sentinel: invalid \a flags argument (bit 31 set). */
    static const uint32_t ERROR_PARAMETER = 0x80000001U;

    /*! \brief Return sentinel: wait timed out before the flags condition was met. */
    static const uint32_t ERROR_TIMEOUT   = 0x80000002U;

    /*! \brief Return sentinel: called from an ISR with a blocking timeout. */
    static const uint32_t ERROR_ISR       = 0x80000004U;

    /*! \brief Reserved error mask. Any return value with bit 31 set is an error. */
    static const uint32_t ERROR_MASK      = 0x80000000U;

    // -----------------------------------------------------------------------
    // Validation helpers
    // -----------------------------------------------------------------------

    /*! \brief  Checks if a return value from Set(), Clear(), or Wait() is an error.
        \return \c true when the value carries an error sentinel (bit 31 set).
    */
    static bool IsError(uint32_t result) { return ((result & ERROR_MASK) != 0U); }

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    /*! \brief     Constructor.
        \param[in] initial_flags: Initial value of the 32-bit flags word.
                   Only bits 0..30 are meaningful; bit 31 is reserved.
    */
    explicit EventFlags(uint32_t initial_flags = 0U) : m_flags(initial_flags)
    {
        STK_ASSERT((initial_flags & ERROR_MASK) == 0U); // API contract: bit 31 must not be set
    }

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is a logical error
                   (dangling waiters). An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~EventFlags()
    {
        // API contract: must not be destroyed while tasks are waiting
        // (checked inside Mutex/ConditionVariable destructors via their own assertions)
    }

    // -----------------------------------------------------------------------
    // Flag manipulation
    // -----------------------------------------------------------------------

    /*! \brief     Set one or more flags.
        \details   Performs an atomic OR of \a flags into the internal flags word,
                   then wakes all tasks currently waiting on this object so that
                   each can re-evaluate its own wait condition.
        \param[in] flags: Bitmask of flags to set. Must not have bit 31 set.
        \return    The flags word value \b after setting, or \c ERROR_PARAMETER
                   if \a flags is 0 or has bit 31 set.
        \note      ISR-safe.
    */
    uint32_t Set(uint32_t flags);

    /*! \brief     Clear one or more flags.
        \details   Atomically clears the specified flags from the internal word.
        \param[in] flags: Bitmask of flags to clear. Must not have bit 31 set.
        \return    The flags word value \b before clearing, or \c ERROR_PARAMETER
                   if \a flags is 0 or has bit 31 set.
        \note      ISR-safe.
    */
    uint32_t Clear(uint32_t flags);

    /*! \brief     Read the current flags word without modifying it.
        \return    Snapshot of the current 32-bit flags word.
        \note      The returned value is a point-in-time snapshot; it may be stale
                   by the time the caller acts on it.
        \note      ISR-safe.
    */
    uint32_t Get() const;

    // -----------------------------------------------------------------------
    // Waiting
    // -----------------------------------------------------------------------

    /*! \brief     Wait for one or more flags to be set.
        \details   Suspends the calling task until the requested flag condition
                   is satisfied or the timeout expires.

                   On success the matched flags are atomically cleared (unless
                   \c NO_CLEAR is passed in \a options).

        \param[in] flags: Bitmask of flag bits to watch. Must not be 0 and must
                   not have bit 31 set.
        \param[in] options: Combination of \c WAIT_ANY / \c WAIT_ALL and optionally
                   \c NO_CLEAR. Default: \c WAIT_ANY (clear on success).
        \param[in] timeout: Maximum time to wait (ticks).
                   Use \c WAIT_INFINITE to block indefinitely,
                   \c NO_WAIT for a non-blocking poll.
        \return    Bitmask of the flags that caused the wakeup (the matched subset),
                   or one of the \c ERROR_* sentinels on failure. Always check
                   \c IsError() before using the return value as a flags mask.
        \note      If the predicate becomes satisfied in the same tick that the deadline
                   expires, the wait succeeds and returns the matched flags. The timeout
                   is only reported when the condition is not met at deadline time.
        \warning   ISR-safe only with \a timeout = \c NO_WAIT, ISR-unsafe otherwise.
    */
    uint32_t Wait(uint32_t flags, uint32_t options = OPT_WAIT_ANY, Timeout timeout = WAIT_INFINITE);

    /*! \brief     Non-blocking flag poll.
        \details   Checks immediately whether the flag condition is satisfied.
                   Clears matched flags on success unless \c NO_CLEAR is set.
        \param[in] flags: Bitmask of flag bits to watch.
        \param[in] options: \c WAIT_ANY (default) or \c WAIT_ALL, optionally \c NO_CLEAR.
        \return    Matched flags bitmask, or an \c ERROR_* sentinel.
        \note      ISR-safe.
    */
    uint32_t TryWait(uint32_t flags, uint32_t options = OPT_WAIT_ANY)
    {
        return Wait(flags, options, NO_WAIT);
    }

private:
    STK_NONCOPYABLE_CLASS(EventFlags);

    // Predicate: returns true when the wait condition for 'flags'/'options' is met.
    // Must be called with m_mutex held.
    bool IsSatisfied(uint32_t flags, uint32_t options) const
    {
        if (options & OPT_WAIT_ALL)
            return ((m_flags & flags) == flags);
        else
            return ((m_flags & flags) != 0U);
    }

    volatile uint32_t m_flags; //!< 32-bit flags word (bit 31 is reserved for errors)
    ConditionVariable m_cv;    //!< woken by Set() to re-evaluate waiting tasks
};

// ---------------------------------------------------------------------------
// Set
// ---------------------------------------------------------------------------

inline uint32_t EventFlags::Set(uint32_t flags)
{
    if ((flags == 0U) || ((flags & ERROR_MASK) != 0U))
        return ERROR_PARAMETER;

    ScopedCriticalSection cs_;

    m_flags |= flags;
    const uint32_t result = m_flags;

    // wake all waiters: each task will re-evaluate its own predicate
    m_cv.NotifyAll();

    return result;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

inline uint32_t EventFlags::Clear(uint32_t flags)
{
    if ((flags == 0U) || ((flags & ERROR_MASK) != 0U))
        return ERROR_PARAMETER;

    ScopedCriticalSection cs_;

    const uint32_t prev = m_flags;
    m_flags &= ~flags;

    return prev;
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------

inline uint32_t EventFlags::Get() const
{
    // atomic on 32-bit aligned targets, a critical section is not required for
    // a single-word read, matching the approach used by Semaphore::GetCount()
    return m_flags;
}

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

inline uint32_t EventFlags::Wait(uint32_t flags, uint32_t options, Timeout timeout)
{
    // validate: flags must be non-zero and must not have the error sentinel bit set
    if ((flags == 0U) || ((flags & ERROR_MASK) != 0U))
        return ERROR_PARAMETER;

    // ISR check: only NO_WAIT is permitted inside an ISR
    if (hw::IsInsideISR() && (timeout != NO_WAIT))
        return ERROR_ISR;

    const bool timed_wait = (timeout != WAIT_INFINITE) && (timeout != NO_WAIT);

    // capture an absolute deadline once, before entering the wait loop,
    // this prevents the timeout from being silently restarted on each
    // spurious wakeup (e.g. a partial Set() that does not satisfy WAIT_ALL)
    const Timeout deadline = (timed_wait ? static_cast<Timeout>(GetTicks() + timeout) : timeout);

    ScopedCriticalSection cs_;

    // spin (with blocking) until the predicate is satisfied or we time out
    while (!IsSatisfied(flags, options))
    {
        Timeout remaining = deadline;
        if (timed_wait)
        {
            const Timeout now = static_cast<Timeout>(GetTicks());
            remaining = (now >= deadline ? NO_WAIT : (deadline - now));
        }

        if (!m_cv.Wait(cs_, remaining))
        {
            // timeout: release the mutex and report failure
            return ERROR_TIMEOUT;
        }
    }

    // predicate satisfied: determine which flags matched
    const uint32_t matched = (options & OPT_WAIT_ALL ? flags : (m_flags & flags));

    // atomically clear the matched flags unless the caller opted out
    if ((options & OPT_NO_CLEAR) == 0U)
        m_flags &= ~matched;

    return matched;
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_EVENT_FLAGS_H_ */
