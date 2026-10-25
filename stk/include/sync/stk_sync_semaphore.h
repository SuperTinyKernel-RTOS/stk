/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_SEMAPHORE_H_
#define STK_SYNC_SEMAPHORE_H_

#include "stk_sync_cs.h"

/*! \file  stk_sync_semaphore.h
    \brief Implementation of synchronization primitive: stk::sync::SemaphoreT and stk::sync::Semaphore.
*/

namespace stk {
namespace sync {

/*! \class SemaphoreT
    \brief Counting semaphore primitive for resource management and signaling, with a
           selectable counter type.

    Counting semaphore maintains an internal counter to manage access to a limited
    number of resources. Unlike a Condition Variable, a Semaphore is stateful: if
    \c Signal() is called when no tasks are waiting, the signal is "remembered" by
    incrementing the internal counter.

    \tparam T: Unsigned integer type used for the counter and its limit (e.g. uint8_t,
               uint16_t, uint32_t, uint64_t). Signed types are rejected at compile time.
               Note that uint8_t does not necessarily reduce the object size because of
               alignment padding, verify with sizeof() on your target before relying on it.
               A type wider than a pointer (e.g. uint64_t on a 32-bit MCU) is supported: all
               counter updates are done under the critical section, and GetCount() takes it
               as well so that the value can not be read torn.

    \note  This implementation uses a Direct Handover policy: when a task is waiting,
           \c Signal() gives the resource "token" directly to the first task in the
           wait list without incrementing the counter. The waking task is then
           guaranteed ownership of that token upon returning from \c Wait().

    \code
    // Example: Resource throttling
    // Initialize with 3 permits (e.g., max 3 concurrent tasks accessing the same resource)
    stk::sync::Semaphore g_Limiter(3);

    void Worker() {
        // attempt to acquire a permit with a 1000 tick timeout
        if (g_Limiter.Wait(1000)) {

            // ... access limited resource ...

            // release the permit back to the semaphore
            g_Limiter.Signal();
        }
    }

    // Example: wide counter (e.g. counting bytes or credits)
    stk::sync::SemaphoreT<uint32_t> g_Credits(0);

    // Example: 64-bit counter (e.g. a port where a native unsigned type is 64-bit wide)
    stk::sync::SemaphoreT<uint64_t> g_Ticket(0);
    \endcode

    \see  Semaphore, ISyncObject, IWaitObject, IKernelService::Wait
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
template <typename T>
class SemaphoreT final : public SyncObjectBase, public ITraceable
{
    STK_STATIC_ASSERT_DESC(static_cast<T>(-1) > static_cast<T>(0), "SemaphoreT: T must be an unsigned integer type");

public:
    /*! \brief     Counter type.
    */
    typedef T ValueType;

    /*! \brief     Max count value supported (maximum value of T).
    */
    static constexpr T COUNT_MAX = static_cast<T>(~static_cast<T>(0U));

    /*! \brief     Constructor.
        \details   Initializes the semaphore with a specific count and maximum limit.
                   Setting \a initial_count to a non-zero value allows for immediate
                   resource acquisition.
        \param[in] initial_count: Starting value of the internal counter (available permits).
        \param[in] max_count: Maximum value the counter is allowed to reach.
        \pre       \a initial_count <= \a max_count.
        \note      Triggers an assertion in a debug build if \a initial_count exceeds \a max_count.
    */
    explicit SemaphoreT(T initial_count = 0U, T max_count = COUNT_MAX)
        : m_count(initial_count), m_count_max(max_count)
    {
        STK_ASSERT(initial_count <= max_count); // API contract: initial count must not exceed maximum
    }

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical
                   error (dangling waiters).
                   An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~SemaphoreT()
    {
        STK_ASSERT(m_wait_list.IsEmpty()); // API contract: must not be destroyed with waiting tasks
    }

    /*! \brief     Wait for a signal (decrement counter), preserving the full wait outcome.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if a token was acquired, \c WAIT_RESULT_TIMEOUT if no token
                   was available and the timeout expired (or \a NO_WAIT was passed),
                   \c WAIT_RESULT_CANCELED if the wait was cancelled via IKernel::CancelTaskWait(),
                   \c WAIT_RESULT_FAIL if the kernel rejected the wait. A non-signal result never
                   consumes a token.
    */
    EWaitResult WaitEx(Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Wait for a signal (decrement counter).
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \warning   ISR-safe only with \a timeout_ticks = \c NO_WAIT, ISR-unsafe otherwise.
        \return    True if acquired, false otherwise (timeout, cancellation or NO_WAIT with no token
                   available). Use \c WaitEx() to tell these apart.
    */
    bool Wait(Timeout timeout_ticks = WAIT_INFINITE) { return (WaitEx(timeout_ticks) == WAIT_RESULT_SIGNAL); }

    /*! \brief     Poll the semaphore without blocking (decrement counter if available).
        \details   Checks if a resource token is available. If so, decrements the counter
                   and returns immediately without yielding the CPU or touching the kernel
                   wait list. If the counter is zero, returns immediately with \c false.
        \note      ISR-safe.
        \return    \c true if a resource token was acquired, \c false if count was zero.
    */
    bool TryWait() { return Wait(NO_WAIT); }

    /*! \brief     Post a signal (increment counter).
        \note      Gives "token" directly to the waking task. The count is not incremented, and
                   the waking task does not decrement it.
        \note      ISR-safe.
        \warning   API contract: the counter must not already be at \a max_count with no task
                   waiting. Violating this triggers an assertion in a debug build. Use
                   \c TrySignal() instead if a signal that would exceed \a max_count should be
                   tolerated (e.g. a redundant post) rather than treated as a caller error.
    */
    void Signal();

    /*! \brief     Try to post a signal without exceeding \a max_count.
        \details   Identical to \c Signal(), except that if the counter is already at
                   \a max_count and no task is waiting, this returns \c false instead of
                   asserting. The check and the increment (or hand-off to a waiter) happen
                   atomically under the same critical section, so this is safe to call from
                   multiple concurrent signalers without racing each other the way a separate
                   \c GetCount() check followed by \c Signal() would.
        \note      ISR-safe.
        \return    \c true if signaled (incremented, or handed directly to a waiter),
                   \c false if the counter was already at \a max_count and nobody was waiting.
    */
    bool TrySignal();

    /*! \brief     Try to post a signal without exceeding a caller-supplied ceiling.
        \details   Same as \c TrySignal(), but the counter is additionally limited by
                   \a ceiling (the effective limit is the smaller of \a ceiling and the
                   \a max_count given at construction). Intended for APIs where the ceiling is
                   a per-call argument (e.g. ThreadX tx_semaphore_ceiling_put). The check and
                   the increment (or hand-off to a waiter) are atomic.
        \param[in] ceiling: Maximum counter value allowed for this call.
        \note      ISR-safe.
        \return    \c true if signaled (incremented, or handed directly to a waiter),
                   \c false if the counter was already at or above the effective limit and
                   nobody was waiting.
    */
    bool TrySignal(const T ceiling);

    /*! \brief     Get current counter value.
        \return    Advisory snapshot of the counter. May be stale by the time the
                   caller acts on it.
        \note      ISR-safe. If T is not wider than a pointer the value is read directly (a single
                   aligned load on all supported STK architectures), otherwise (e.g. uint64_t on a
                   32-bit MCU) it is read under a critical section so it can not be torn by a
                   concurrent update. Not safe for read-modify-write use.
    */
    T GetCount() const { return ReadCount(ReadTag<(sizeof(T) > sizeof(void *))>()); }

private:
    STK_NONCOPYABLE_CLASS(SemaphoreT);

    //! Selects how the counter is read: \a WIDE is true if T is wider than a pointer.
    template <bool WIDE> struct ReadTag {};

    //! Counter fits the native word: a single load is atomic.
    __stk_forceinline T ReadCount(ReadTag<false>) const { return m_count; }

    //! Counter is wider than the native word: a plain load may be split into several, guard it.
    __stk_forceinline T ReadCount(ReadTag<true>) const
    {
        const ScopedCriticalSection cs_;
        return m_count;
    }

    /*! \brief     Common signal path: hand off to a waiter or increment up to \a limit.
        \param[in] limit: Effective counter limit for this call.
        \return    \c true if signaled, \c false if saturated with no waiter.
    */
    bool TrySignalLimited(const T limit);

    T m_count;     //!< Internal resource counter
    T m_count_max; //!< Counter max limit
};

#if (__cplusplus < 201703L)
// out-of-class definition, required before C++17 if COUNT_MAX is ODR-used (e.g. bound to a const reference);
// since C++17 static constexpr data members are implicitly inline and this definition is redundant (deprecated)
template <typename T>
constexpr T SemaphoreT<T>::COUNT_MAX;
#endif

/*! \typedef   Semaphore
    \brief     Default counting semaphore with a 16-bit counter (max count 0xFFFF).
    \see       SemaphoreT
*/
typedef SemaphoreT<uint16_t> Semaphore;

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

template <typename T>
inline EWaitResult SemaphoreT<T>::WaitEx(Timeout timeout_ticks)
{
    // guard
    ScopedCriticalSection cs_;

    // NO_WAIT with no token available is reported as an immediately-expired timeout
    EWaitResult result = WAIT_RESULT_TIMEOUT;

    // fast path: resource is available
    if (m_count != 0U)
    {
        m_count = static_cast<T>(m_count - 1U);
        result = WAIT_RESULT_SIGNAL;
    }
    // slow path: block until Signal() or timeout
    else if (timeout_ticks != NO_WAIT)
    {
        STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR if timeout_ticks!=NO_WAIT

        // note: after waking, if not a timeout, we effectively own the resource that Signal() produced
        // but didn't put into m_count (see logic of if (m_wait_list.IsEmpty()) in TrySignalLimited())
        result = IKernelService::GetInstance()->Wait(this, &cs_, timeout_ticks);
    }
    // try lock behavior (timeout_ticks=NO_WAIT)
    else
    {
        // result is WAIT_RESULT_TIMEOUT already, noop
    }

    return result;
}

// ---------------------------------------------------------------------------
// Signal
// ---------------------------------------------------------------------------

template <typename T>
inline void SemaphoreT<T>::Signal()
{
    const bool success = TrySignal();
    STK_UNUSED(success);
    STK_ASSERT(success); // API contract: the count must not exceed maximum
}

// ---------------------------------------------------------------------------
// TrySignal
// ---------------------------------------------------------------------------

template <typename T>
inline bool SemaphoreT<T>::TrySignal()
{
    return TrySignalLimited(m_count_max);
}

template <typename T>
inline bool SemaphoreT<T>::TrySignal(const T ceiling)
{
    // effective limit is the smaller of the per-call ceiling and the per-object maximum
    return TrySignalLimited((ceiling < m_count_max) ? ceiling : m_count_max);
}

template <typename T>
inline bool SemaphoreT<T>::TrySignalLimited(const T limit)
{
    bool success = true;

    // guard
    const ScopedCriticalSection cs_;

    if (m_wait_list.IsEmpty())
    {
        if (m_count >= limit)
        {
            // counter already saturated and no one is waiting to hand the token to directly
            success = false;
        }
        else
        {
            // no one is waiting, save signal for later
            m_count = static_cast<T>(m_count + 1U);
        }
    }
    else
    {
        // give signal directly to the first waiting task
        WakeOne();
    }

    return success;
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_SEMAPHORE_H_ */
