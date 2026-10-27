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

/*! \file  stk_sync_eventflags.h
    \brief Implementation of synchronization primitive: stk::sync::EventFlagsT and stk::sync::EventFlags.
*/

namespace stk {
namespace sync {

/*! \class EventFlagsT
    \brief Event flags group for multi-flag synchronization between tasks, with a selectable
           flags word type.

    EventFlagsT maintains an unsigned word where each bit represents an independent
    boolean event. Tasks can set or clear any combination of flags, and wait for
    any subset to become set — either requiring \b all requested flags (AND
    semantics) or \b any one of them (OR semantics).

    Unlike \c Event, which models a single binary signal, \c EventFlagsT allows
    fine-grained coordination across up to (bit width of T) - 1 independent conditions
    in a single object.

    \tparam T: Unsigned integer type used for the flags word (e.g. uint8_t, uint16_t, uint32_t,
               uint64_t). Signed types are rejected at compile time. The most significant bit of T
               is reserved as the error indicator of the legacy Wait() API, so the number of usable
               flags is <tt>8 * sizeof(T) - 1</tt> (7 for uint8_t, 31 for uint32_t, 63 for uint64_t).
               Note that uint8_t does not necessarily reduce the object size because of
               alignment padding, verify with sizeof() on your target before relying on it.
               A type wider than a pointer (e.g. uint64_t on a 32-bit MCU) is supported: all
               flags updates are done under the critical section, and Get() takes it as well
               so that the value can not be read torn.

    \note  **Wait semantics**: the caller selects the mode via the \a options
           parameter using \c OPT_WAIT_ANY (default) or \c OPT_WAIT_ALL. \a options is always a
           \c uint32_t regardless of \a T.
    \note  **Auto-clear**: by default the matched flags are atomically cleared
           upon a successful \c Wait(). Pass \c OPT_NO_CLEAR to suppress this.
    \note  **Direct Handover**: \c Set() notifies \b all waiters so that every
           task re-evaluates its own predicate after waking. Each waiter clears
           only the flags it matched, so concurrent waiters with disjoint
           flag masks do not interfere with each other.
    \note  The most significant bit of \a T (see \c ERROR_MASK) is reserved for error reporting
           and must never be set by the caller. The highest usable flag bit index is
           <tt>8 * sizeof(T) - 2</tt>.

    \code
    // Example: sensor fusion — wait for GPS and IMU data to both arrive
    stk::sync::EventFlags g_SensorFlags; // 32-bit flags (EventFlagsT<uint32_t>)

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
                                             EventFlags::OPT_WAIT_ALL, 5000);
        if (EventFlags::IsError(raised)) {
            // handle timeout (raised == EventFlags::ERROR_TIMEOUT), cancellation, etc.
        } else {
            // process fused sensor data
        }
    }

    // Example: 64-bit flags group (up to 63 independent events)
    stk::sync::EventFlagsT<uint64_t> g_Channels;

    static const uint64_t CH_40 = (static_cast<uint64_t>(1U) << 40);

    // Example: compact 8-bit group (up to 7 independent events)
    stk::sync::EventFlagsT<uint8_t> g_Small;
    \endcode

    \note  **Rendezvous**: \c Sync() sets flags and waits for a group of flags atomically and releases all
           participants at once (multi-task barrier, equivalent of FreeRTOS xEventGroupSync()).

    \see  EventFlags, Event, Semaphore, ConditionVariable
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
template <typename T>
class EventFlagsT final : public ITraceable
{
    STK_STATIC_ASSERT_DESC(static_cast<T>(-1) > static_cast<T>(0), "EventFlagsT: T must be an unsigned integer type");

public:
    /*! \brief     Flags value type.
    */
    typedef T FlagsValueType;

    // -----------------------------------------------------------------------
    // Word constants
    // -----------------------------------------------------------------------

    /*! \brief     Maximum value of the flags word (all bits set, maximum value of T).
    */
    static constexpr T FLAGS_MAX = static_cast<T>(~static_cast<T>(0U));

    /*! \brief     Reserved error mask: the most significant bit of T. Any return value of the legacy
                   API with this bit set is an error.
    */
    static constexpr T ERROR_MASK = static_cast<T>(~static_cast<T>(FLAGS_MAX >> 1U));

    /*! \brief     Mask of all bits which may be used as flags (every bit except \c ERROR_MASK).
    */
    static constexpr T FLAGS_MASK = static_cast<T>(~ERROR_MASK);

    // -----------------------------------------------------------------------
    // Options constants (bitmask, passed to Wait())
    // -----------------------------------------------------------------------

    /*! \brief Wait for ANY of the specified flags to be set (OR semantics, default). */
    static constexpr uint32_t OPT_WAIT_ANY = 0x00000000U;

    /*! \brief Wait for ALL of the specified flags to be set simultaneously (AND semantics). */
    static constexpr uint32_t OPT_WAIT_ALL = 0x00000001U;

    /*! \brief Do not clear matched flags after a successful wait. */
    static constexpr uint32_t OPT_NO_CLEAR = 0x00000002U;

    // -----------------------------------------------------------------------
    // Return-value error sentinels (the most significant bit of T is the error indicator)
    // -----------------------------------------------------------------------

    /*! \brief Return sentinel: invalid \a flags argument (error bit set or no bits requested). */
    static constexpr T ERROR_PARAMETER = static_cast<T>(ERROR_MASK | static_cast<T>(0x01U));

    /*! \brief Return sentinel: wait timed out before the flags condition was met. */
    static constexpr T ERROR_TIMEOUT   = static_cast<T>(ERROR_MASK | static_cast<T>(0x02U));

    /*! \brief Return sentinel: called from an ISR with a blocking timeout. */
    static constexpr T ERROR_ISR       = static_cast<T>(ERROR_MASK | static_cast<T>(0x04U));

    /*! \brief Return sentinel: the wait was cancelled via IKernel::CancelTaskWait()
               before the flag condition was met (independent of the deadline). */
    static constexpr T ERROR_CANCELED  = static_cast<T>(ERROR_MASK | static_cast<T>(0x08U));

    /*! \brief Return sentinel: the kernel rejected the wait (IKernelService::Wait() returned
               WAIT_RESULT_FAIL, e.g. KERNEL_SYNC mode is not enabled). */
    static constexpr T ERROR_FAIL      = static_cast<T>(ERROR_MASK | static_cast<T>(0x10U));

    // -----------------------------------------------------------------------
    // Validation helpers
    // -----------------------------------------------------------------------

    /*! \brief  Checks if a return value from Set(), Clear(), or Wait() is an error.
        \return \c true when the value carries an error sentinel (most significant bit set).
    */
    static bool IsError(T result) { return ((result & ERROR_MASK) != 0U); }

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    /*! \brief     Constructor.
        \param[in] initial_flags: Initial value of the flags word.
                   Only bits covered by \c FLAGS_MASK are meaningful; the most significant bit is reserved.
    */
    explicit EventFlagsT(T initial_flags = 0U) : m_flags(initial_flags)
    {
        STK_ASSERT((initial_flags & ERROR_MASK) == 0U); // API contract: the reserved bit must not be set
    }

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is a logical error
                   (dangling waiters). An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~EventFlagsT() = default;

    // -----------------------------------------------------------------------
    // Flag manipulation
    // -----------------------------------------------------------------------

    /*! \brief     Set one or more flags.
        \details   Performs an atomic OR of \a flags into the internal flags word,
                   then wakes all tasks currently waiting on this object so that
                   each can re-evaluate its own wait condition.
        \param[in] flags: Bitmask of flags to set. Must not have the reserved bit set.
        \return    The flags word value \b after setting, or \c ERROR_PARAMETER
                   if \a flags is 0 or has the reserved bit set.
        \note      ISR-safe.
    */
    T Set(T flags);

    /*! \brief     Clear one or more flags.
        \details   Atomically clears the specified flags from the internal word.
        \param[in] flags: Bitmask of flags to clear. Must not have the reserved bit set.
        \return    The flags word value \b before clearing, or \c ERROR_PARAMETER
                   if \a flags is 0 or has the reserved bit set.
        \note      ISR-safe.
    */
    T Clear(T flags);

    /*! \brief     AND the flags word with a mask (equivalent of ThreadX tx_event_flags_set(TX_AND)).
        \details   Atomically performs <tt>flags &= mask</tt>: every flag bit that is 0 in \a mask is
                   cleared, every flag bit that is 1 in \a mask is preserved. This is the inverse of
                   Clear(), which clears the bits that are 1 in its argument. Any \a mask value is
                   valid, including 0 (clears all flags) and \c FLAGS_MAX (no change).
        \param[in] mask: Bitmask of flags to \b keep.
        \return    The flags word value \b before masking.
        \note      Does not wake waiters: this operation can only remove flags, so it can never
                   satisfy a pending wait condition.
        \note      ISR-safe.
        \see       Clear
    */
    T And(T mask);

    /*! \brief     Read the current flags word without modifying it.
        \return    Snapshot of the current flags word.
        \note      The returned value is a point-in-time snapshot; it may be stale
                   by the time the caller acts on it.
        \note      ISR-safe. If T is not wider than a pointer the value is read directly (a single
                   aligned load on all supported STK architectures), otherwise (e.g. uint64_t on a
                   32-bit MCU) it is read under a critical section so it can not be torn by a
                   concurrent update.
    */
    T Get() const { return ReadFlags(ReadTag<(sizeof(T) > sizeof(void *))>()); }

    // -----------------------------------------------------------------------
    // Waiting
    // -----------------------------------------------------------------------

    /*! \brief     Wait for one or more flags to be set.
        \details   Suspends the calling task until the requested flag condition
                   is satisfied or the timeout expires.

                   On success the matched flags are atomically cleared (unless
                   \c OPT_NO_CLEAR is passed in \a options).

        \param[in] flags: Bitmask of flag bits to watch. Must not be 0 and must
                   not have the reserved bit set.
        \param[in] options: Combination of \c OPT_WAIT_ANY / \c OPT_WAIT_ALL and optionally
                   \c OPT_NO_CLEAR. Default: \c OPT_WAIT_ANY (clear on success).
        \param[in] timeout_ticks: Maximum time to wait (ticks).
                   Use \c WAIT_INFINITE to block indefinitely,
                   \c NO_WAIT for a non-blocking poll.
        \return    Bitmask of the flags that caused the wakeup (the matched subset),
                   or one of the \c ERROR_* sentinels on failure. Always check
                   \c IsError() before using the return value as a flags mask.
        \note      If the predicate becomes satisfied in the same tick that the deadline
                   expires, the wait succeeds and returns the matched flags. The timeout
                   is only reported when the condition is not met at deadline time.
        \note      Returns \c ERROR_CANCELED, not \c ERROR_TIMEOUT, if the wait was
                   interrupted via \c IKernel::CancelTaskWait() before either the flag
                   condition was met or the deadline expired.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    T Wait(T flags, uint32_t options = OPT_WAIT_ANY, Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Wait for one or more flags to be set, reporting the outcome via EWaitResult.
        \details   Same wait semantics as Wait(), but the status and the flags
                   word are reported separately, so no bit of the flags word is overloaded as an
                   error indicator (equivalent of ThreadX tx_event_flags_get() with \c actual_flags).
        \param[in]  flags: Bitmask of flag bits to watch. Must not be 0 and must not have the reserved bit set.
        \param[in]  options: Combination of \c OPT_WAIT_ANY / \c OPT_WAIT_ALL and optionally
                    \c OPT_NO_CLEAR.
        \param[in]  timeout_ticks: Maximum time to wait (ticks), \c WAIT_INFINITE or \c NO_WAIT.
        \param[out] actual_flags: Snapshot of the \b entire flags word taken when the call completed,
                    \b before any auto-clear is applied. On \c WAIT_RESULT_SIGNAL the matched subset
                    is <tt>actual_flags & flags</tt> (OPT_WAIT_ANY) or \a flags (OPT_WAIT_ALL). On timeout,
                    cancellation or failure it is the current flags word at that moment.
        \return    \c WAIT_RESULT_SIGNAL if the condition was met (matched flags cleared unless
                   \c OPT_NO_CLEAR), otherwise the timeout / \c WAIT_RESULT_CANCELED /
                   \c WAIT_RESULT_FAIL result of the kernel wait.
        \note      Invalid arguments (\a flags is 0 or has the reserved bit set) and a blocking wait from
                   an ISR are reported as \c WAIT_RESULT_FAIL, and the wait is not performed. Use the
                   legacy overload if the distinct \c ERROR_PARAMETER / \c ERROR_ISR codes are needed.
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    EWaitResult WaitEx(T flags, uint32_t options, Timeout timeout_ticks, T &actual_flags);

    /*! \brief      Non-blocking flag poll with EWaitResult and full flags snapshot.
        \param[in]  flags: Bitmask of flag bits to watch.
        \param[in]  options: \c OPT_WAIT_ANY or \c OPT_WAIT_ALL, optionally \c OPT_NO_CLEAR.
        \param[out] actual_flags: Snapshot of the entire flags word (see WaitEx()).
        \return     \c WAIT_RESULT_SIGNAL if satisfied, otherwise a non-signal result.
        \note       ISR-safe.
    */
    EWaitResult TryWaitEx(T flags, uint32_t options, T &actual_flags)
    {
        return WaitEx(flags, options, NO_WAIT, actual_flags);
    }

    /*! \brief  State of one Sync() rendezvous group, supplied by the caller.
        \details One SyncContext object represents one rendezvous group: it must be shared by (and only
                 by) all tasks which rendezvous together, e.g. a global or static object next to the
                 EventFlagsT object. It carries the generation counter and the flags snapshot used to
                 release all blocked participants when the rendezvous completes, so EventFlagsT itself
                 needs no extra state. Independent groups on the same EventFlagsT object use separate
                 contexts. The object must outlive all calls using it and must not be copied while tasks
                 are blocked on it.
    */
    struct SyncContext
    {
        SyncContext() : m_gen(0U), m_snapshot(0U)
        {}

        uint32_t m_gen;      //!< rendezvous generation, incremented when the rendezvous completes
        T        m_snapshot; //!< flags word captured when the last rendezvous completed
    };

    /*! \brief      Atomically set flags and wait for a group of flags: a multi-task rendezvous
                    (equivalent of FreeRTOS xEventGroupSync()).
        \details    Sets \a set_flags, then blocks until \b all of \a wait_flags are set. The set-then-wait
                    sequence is indivisible and, unlike a Set() followed by Wait(OPT_WAIT_ALL), it releases
                    \b every task blocked in Sync() on the same \a ctx when the last participant arrives:
                    the task which completes the condition clears \a wait_flags once and all blocked
                    participants return success. (With Set() + Wait(OPT_WAIT_ALL) the first task to run
                    would clear the flags and the other participants would never see the condition
                    satisfied again.)
        \param[in,out] ctx: Rendezvous group state shared by all participants of the group (see SyncContext).
        \param[in]  set_flags: Flags to set on entry (this task's "I have arrived" bits). May be 0 (observer
                    role: wait without contributing a bit). Must not have the reserved bit set.
        \param[in]  wait_flags: Flags which must all be set to release the group. Must not be 0 and must
                    not have the reserved bit set. All participants sharing one \a ctx must pass the same value.
        \param[in]  timeout_ticks: Maximum time to wait (ticks), \c WAIT_INFINITE or \c NO_WAIT.
        \param[out] actual_flags: Snapshot of the \b entire flags word taken when the rendezvous completed,
                    \b before \a wait_flags were cleared. On timeout, cancellation or failure it is the
                    current flags word at that moment.
        \return     \c WAIT_RESULT_SIGNAL if the rendezvous completed, otherwise the timeout /
                    \c WAIT_RESULT_CANCELED / \c WAIT_RESULT_FAIL result. Invalid arguments and a blocking
                    call from an ISR are reported as \c WAIT_RESULT_FAIL and nothing is set.
        \note       Flags set by a task which then times out stay set (they are not rolled back).
        \note       A completion can also be caused by a plain Set() which satisfies \a wait_flags: the next
                    woken Sync() waiter then completes the rendezvous.
        \warning    ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
    */
    EWaitResult Sync(SyncContext &ctx, T set_flags, T wait_flags, Timeout timeout_ticks, T &actual_flags);

    /*! \brief     Non-blocking flag poll.
        \details   Checks immediately whether the flag condition is satisfied.
                   Clears matched flags on success unless \c OPT_NO_CLEAR is set.
        \param[in] flags: Bitmask of flag bits to watch.
        \param[in] options: \c OPT_WAIT_ANY (default) or \c OPT_WAIT_ALL, optionally \c OPT_NO_CLEAR.
        \return    Matched flags bitmask, or an \c ERROR_* sentinel.
        \note      ISR-safe.
    */
    T TryWait(T flags, uint32_t options = OPT_WAIT_ANY)
    {
        return Wait(flags, options, NO_WAIT);
    }

    /*! \brief     Get list of tasks blocked on this object.
        \note      Read-only access for diagnostics / telemetry.
        \note      The result is a snapshot: call inside the critical section (see \a hw::CriticalSection,
                   \a sync::ScopedCrticalSection) if the list may change concurrently.
    */
    const IWaitObject::ListHeadType &GetWaitList() const
    {
        return m_cv.GetWaitList();
    }

    /*! \brief     Get id of the task which is first in the wait list (FIFO order), i.e. the one
                   that WakeOne() would wake next.
        \return    TId of the first waiter, or \a TID_NONE if no tasks are currently waiting.
        \note      Read-only introspection for diagnostics / telemetry.
        \note      The result is a snapshot: call inside the critical section (see \a hw::CriticalSection,
                   \a sync::ScopedCrticalSection) if the list may change concurrently.
        \see       GetWaitList
    */
    TId GetFirstWaiterTid() const
    {
        return m_cv.GetFirstWaiterTid();
    }

private:
    STK_NONCOPYABLE_CLASS(EventFlagsT);

    //! Selects how the flags word is read: \a WIDE is true if T is wider than a pointer.
    template <bool WIDE> struct ReadTag {};

    //! Flags word fits the native word: a single load is atomic.
    __stk_forceinline T ReadFlags(ReadTag<false>) const { return m_flags; }

    //! Flags word is wider than the native word: a plain load may be split into several, guard it.
    __stk_forceinline T ReadFlags(ReadTag<true>) const
    {
        const hw::CriticalSection::ScopedLock cs_;
        return m_flags;
    }

    // Predicate: returns true when the wait condition for 'flags'/'options' is met.
    // Must be called inside the critical section.
    bool IsSatisfied(T flags, uint32_t options) const
    {
        return ((options & OPT_WAIT_ALL) == OPT_WAIT_ALL) ? ((m_flags & flags) == flags) :
            ((m_flags & flags) != 0U);
    }

    // Core wait loop shared by Wait() and WaitEx() (arguments must already be validated).
    // Returns WAIT_RESULT_SIGNAL when the predicate was satisfied, otherwise the failing result.
    // actual_flags receives the whole flags word before auto-clear.
    EWaitResult WaitCore(T flags, uint32_t options, Timeout timeout_ticks, T &actual_flags);

    volatile T        m_flags; //!< flags word (the most significant bit is reserved for errors)
    ConditionVariable m_cv;    //!< woken by Set() to re-evaluate waiting tasks
};

#if (__cplusplus < 201703L)
// out-of-class definitions, required before C++17 if a constant is ODR-used (e.g. bound to a const
// reference); since C++17 static constexpr data members are implicitly inline and these definitions
// are redundant (deprecated)
template <typename T> constexpr T        EventFlagsT<T>::FLAGS_MAX;
template <typename T> constexpr T        EventFlagsT<T>::ERROR_MASK;
template <typename T> constexpr T        EventFlagsT<T>::FLAGS_MASK;
template <typename T> constexpr uint32_t EventFlagsT<T>::OPT_WAIT_ANY;
template <typename T> constexpr uint32_t EventFlagsT<T>::OPT_WAIT_ALL;
template <typename T> constexpr uint32_t EventFlagsT<T>::OPT_NO_CLEAR;
template <typename T> constexpr T        EventFlagsT<T>::ERROR_PARAMETER;
template <typename T> constexpr T        EventFlagsT<T>::ERROR_TIMEOUT;
template <typename T> constexpr T        EventFlagsT<T>::ERROR_ISR;
template <typename T> constexpr T        EventFlagsT<T>::ERROR_CANCELED;
template <typename T> constexpr T        EventFlagsT<T>::ERROR_FAIL;
#endif

/*! \typedef   EventFlags
    \brief     Default event flags group with a 32-bit flags word (31 usable flags, bit 31 reserved).
    \see       EventFlagsT
*/
typedef EventFlagsT<uint32_t> EventFlags;

// ---------------------------------------------------------------------------
// Set
// ---------------------------------------------------------------------------

template <typename T>
inline T EventFlagsT<T>::Set(T flags)
{
    T final_result = ERROR_PARAMETER;

    if ((flags != 0U) && ((flags & ERROR_MASK) == 0U))
    {
        const hw::CriticalSection::ScopedLock cs_;

        m_flags = static_cast<T>(m_flags | flags);
        final_result = m_flags;

        // wake all waiters: each task will re-evaluate its own predicate
        m_cv.NotifyAll_CS();
    }

    return final_result;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

template <typename T>
inline T EventFlagsT<T>::Clear(T flags)
{
    T result = ERROR_PARAMETER;

    if ((flags != 0U) && ((flags & ERROR_MASK) == 0U))
    {
        const hw::CriticalSection::ScopedLock cs_;

        result = m_flags;
        m_flags = static_cast<T>(m_flags & static_cast<T>(~flags));
    }

    return result;
}

// ---------------------------------------------------------------------------
// And
// ---------------------------------------------------------------------------

template <typename T>
inline T EventFlagsT<T>::And(T mask)
{
    const hw::CriticalSection::ScopedLock cs_;

    const T result = m_flags;
    m_flags = static_cast<T>(m_flags & mask);

    return result;
}

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

template <typename T>
inline EWaitResult EventFlagsT<T>::WaitCore(T flags, uint32_t options,
    Timeout timeout_ticks, T &actual_flags)
{
    EWaitResult result = WAIT_RESULT_SIGNAL;

    const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

    // capture an absolute deadline once, before entering the wait loop,
    // this prevents the timeout from being silently restarted on each
    // spurious wakeup (e.g. a partial Set() that does not satisfy OPT_WAIT_ALL)
    const Timeout deadline = (timed_wait ?
        static_cast<Timeout>(GetTicks() + timeout_ticks) : timeout_ticks);

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

        const EWaitResult wr = m_cv.WaitEx(cs_, remaining);

        if (wr != WAIT_RESULT_SIGNAL)
        {
            // timeout (a real deadline miss, or NO_WAIT after 'remaining' collapsed to NO_WAIT),
            // cancellation by IKernel::CancelTaskWait() or kernel failure: stop immediately,
            // do not reinterpret the result and do not keep spinning on the predicate
            result = wr;
            break;
        }
        // else: WAIT_RESULT_SIGNAL - a Set() woke us for re-evaluation (possibly a
        // spurious/partial match under OPT_WAIT_ALL); loop back and re-check IsSatisfied()
    }

    // full group snapshot, taken before any auto-clear
    actual_flags = m_flags;

    if (result == WAIT_RESULT_SIGNAL)
    {
        // atomically clear the matched flags unless the caller opted out
        if ((options & OPT_NO_CLEAR) == 0U)
        {
            const T matched = (((options & OPT_WAIT_ALL) == OPT_WAIT_ALL) ?
                flags : static_cast<T>(m_flags & flags));
            m_flags = static_cast<T>(m_flags & static_cast<T>(~matched));
        }
    }

    return result;
}

template <typename T>
inline T EventFlagsT<T>::Wait(T flags, uint32_t options, Timeout timeout_ticks)
{
    T final_result = 0U;

    // validate: flags must be non-zero and must not have the error sentinel bit set
    if ((flags == 0U) || ((flags & ERROR_MASK) != 0U))
    {
        final_result = ERROR_PARAMETER;
    }
    // ISR check: only NO_WAIT is permitted inside an ISR
    else if (hw::IsInsideISR() && (timeout_ticks != NO_WAIT))
    {
        final_result = ERROR_ISR;
    }
    else
    {
        T actual = 0U;
        const EWaitResult wr = WaitCore(flags, options, timeout_ticks, actual);

        if (wr == WAIT_RESULT_SIGNAL)
        {
            // predicate satisfied: report the matched flags
            final_result = (((options & OPT_WAIT_ALL) == OPT_WAIT_ALL) ?
                flags : static_cast<T>(actual & flags));
        }
        else if (wr == WAIT_RESULT_CANCELED)
        {
            final_result = ERROR_CANCELED;
        }
        else if (wr == WAIT_RESULT_FAIL)
        {
            final_result = ERROR_FAIL;
        }
        else
        {
            final_result = ERROR_TIMEOUT;
        }
    }

    return final_result;
}

template <typename T>
inline EWaitResult EventFlagsT<T>::WaitEx(T flags, uint32_t options,
    Timeout timeout_ticks, T &actual_flags)
{
    EWaitResult result = WAIT_RESULT_FAIL;

    if ((flags == 0U) || ((flags & ERROR_MASK) != 0U) ||
        (hw::IsInsideISR() && (timeout_ticks != NO_WAIT)))
    {
        // invalid argument or blocking wait inside an ISR: wait is not performed
        actual_flags = Get();
    }
    else
    {
        result = WaitCore(flags, options, timeout_ticks, actual_flags);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Sync
// ---------------------------------------------------------------------------

template <typename T>
inline EWaitResult EventFlagsT<T>::Sync(SyncContext &ctx, T set_flags, T wait_flags,
    Timeout timeout_ticks, T &actual_flags)
{
    EWaitResult result = WAIT_RESULT_FAIL;

    if ((wait_flags == 0U) || ((wait_flags & ERROR_MASK) != 0U) || ((set_flags & ERROR_MASK) != 0U) ||
        (hw::IsInsideISR() && (timeout_ticks != NO_WAIT)))
    {
        // invalid argument or blocking call inside an ISR: nothing is set, nothing is waited for
        actual_flags = Get();
    }
    else
    {
        const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

        // absolute deadline captured once (see WaitCore)
        const Timeout deadline = (timed_wait ?
            static_cast<Timeout>(GetTicks() + timeout_ticks) : timeout_ticks);

        ScopedCriticalSection cs_;

        // set this task's bits and remember the rendezvous generation in the same critical section:
        // the set-then-wait sequence is indivisible for every other task
        m_flags = static_cast<T>(m_flags | set_flags);
        const uint32_t gen = ctx.m_gen;

        for (;;)
        {
            if (ctx.m_gen != gen)
            {
                // released by the task which completed the rendezvous: it has already cleared the flags,
                // so the predicate must not be re-evaluated here
                actual_flags = ctx.m_snapshot;
                result = WAIT_RESULT_SIGNAL;
                break;
            }

            if (IsSatisfied(wait_flags, OPT_WAIT_ALL))
            {
                // this task completes the rendezvous (last arriver, or flags satisfied by a plain Set()):
                // publish the snapshot, clear the group flags once and release every blocked participant
                ctx.m_snapshot = m_flags;
                m_flags = static_cast<T>(m_flags & static_cast<T>(~wait_flags));
                ++ctx.m_gen;
                m_cv.NotifyAll_CS();

                actual_flags = ctx.m_snapshot;
                result = WAIT_RESULT_SIGNAL;
                break;
            }

            Timeout remaining = deadline;
            if (timed_wait)
            {
                const Timeout now = static_cast<Timeout>(GetTicks());
                remaining = (now >= deadline ? NO_WAIT : (deadline - now));
            }

            const EWaitResult wr = m_cv.WaitEx(cs_, remaining);

            if (wr != WAIT_RESULT_SIGNAL)
            {
                // timeout, cancellation or kernel failure; a completion which raced with the deadline is
                // still honoured: re-check the generation before giving up
                if (ctx.m_gen != gen)
                {
                    actual_flags = ctx.m_snapshot;
                    result = WAIT_RESULT_SIGNAL;
                }
                else
                {
                    actual_flags = m_flags;
                    result = wr;
                }
                break;
            }
            // else: woken for re-evaluation, loop back
        }
    }

    return result;
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_EVENT_FLAGS_H_ */
