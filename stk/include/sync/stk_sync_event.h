/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_EVENT_H_
#define STK_SYNC_EVENT_H_

#include "stk_sync_cs.h"

/*! \file  stk_sync_event.h
    \brief Implementation of synchronization primitive: stk::sync::Event.
*/

namespace stk {
namespace sync {

/*! \class Event
    \brief Binary synchronization event (signaled / non-signaled) primitive.

    Supports two operation modes:
    - Auto-reset (default): Set() wakes one waiting task and automatically resets.
    - Manual-reset:         Set() wakes all waiting tasks; state remains signaled until Reset().

    Additionally, supports \c Pulse(): attempt to release waiters and then reset (Win32 PulseEvent() semantics).

    \note  Follows Win32 PulseEvent() behavior:
           - For auto-reset events: releases one waiting thread (if any) and resets to non-signaled.
           - For manual-reset events: releases all waiting threads (if any) and resets to non-signaled.
           - If no threads are waiting, resets to non-signaled.

    \warning Pulse semantics are inherently racy and considered unreliable in many Win32 usage scenarios.
             Prefer explicit \c Set() + \c Reset() patterns when possible.

    \code
    // Example: Task synchronization using Event
    stk::sync::Event g_DataReadyEvt;

    void Task_Producer() {
        // ... produce data ...

        // notify Task_Consumer that data is ready
        g_DataReadyEvt.Set();
    }

    void Task_Consumer() {
        // wait for a data-ready notification signal from Task_Producer with 1s timeout
        if (g_DataReadyEvt.Wait(1000)) {
            // ... process data ...
        }
    }
    \endcode

    \see  ISyncObject, IWaitObject, IKernelService::Wait
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
class Event : public ITraceable, private ISyncObject
{
public:
    /*! \brief     Constructor.
        \param[in] manual_reset:  If \c true creates a manual-reset event (stays signaled until explicitly reset).
                                  If \c false (default) creates an auto-reset event.
        \param[in] initial_state: Initial state of the event: \c true = signaled, \c false = non-signaled.
    */
    explicit Event(bool manual_reset = false, bool initial_state = false)
        : m_manual_reset(manual_reset), m_signaled(initial_state)
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error
                   (dangling waiters). An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~Event()
    {
        STK_ASSERT(m_wait_list.IsEmpty()); // API contract: must not be destroyed with waiting tasks
    }

    /*! \brief     Set event to signaled state.
        \return    \c true if state was changed from non-signaled to signaled,
                   \c false if event was already signaled.
        \note      - In auto-reset mode: wakes one waiting task (if any) and immediately resets.
                   - In manual-reset mode: wakes all waiting tasks (if any); state remains set.
                   - If already signaled, does nothing and returns \c false.
        \warning   The return value indicates whether the event state changed, not whether any
                   waiting task was woken. A return of \c false means the event was already
                   signaled, not that no task woke.
        \note      ISR-safe.
    */
    bool Set();

    /*! \brief     Reset event to non-signaled state.
        \return    \c true if state was changed from signaled to non-signaled,
                   \c false if event was already non-signaled.
        \note      Has no practical effect on auto-reset events that are already non-signaled.
                   Mainly used with manual-reset events.
        \note      ISR-safe.
    */
    bool Reset();

    /*! \brief     Wait until event becomes signaled or the timeout expires.
        \param[in] timeout: Maximum time to wait (ticks). Use \a WAIT_INFINITE for no timeout (wait forever).
        \warning   ISR-unsafe.
        \return    \c true if event was signaled (wait succeeded),
                   \c false if timeout occurred before the event was signaled.
    */
    bool Wait(Timeout timeout = WAIT_INFINITE);

    /*! \brief    Poll event state without blocking.
        \details  Checks if the event is currently signaled. If signaled, performs the auto-reset
                  (if auto-reset mode is active) and returns immediately without yielding the CPU
                  or entering a wait list.
        \note     ISR-safe.
        \return   \c true if event was signaled at the time of the call,
                  \c false if event was not signaled.
    */
    bool TryWait();

    /*! \brief    Pulse event: attempt to release waiters and then reset (Win32 PulseEvent() semantics).
        \note     Follows Win32 PulseEvent() behavior:
                   - For auto-reset events: releases one waiting thread (if any) and resets to non-signaled.
                   - For manual-reset events: releases all waiting threads (if any) and resets to non-signaled.
                   - If no threads are waiting, resets to non-signaled.
        \note     ISR-safe.
        \warning  Pulse semantics are inherently racy and considered unreliable in many Win32 usage scenarios.
                  Prefer explicit \c Set() + \c Reset() patterns when possible.
    */
    void Pulse();

private:
    STK_NONCOPYABLE_CLASS(Event);

    void RemoveWaitObject(IWaitObject *wobj);

    bool m_manual_reset; //!< \c true = manual-reset event, \c false = auto-reset
    bool m_signaled;     //!< current signaled state of the event
};

// ---------------------------------------------------------------------------
// Set
// ---------------------------------------------------------------------------

inline bool Event::Set()
{
    ScopedCriticalSection cs_;

    if (m_signaled)
        return false;

    m_signaled = true;
    __stk_full_memfence();

    if (m_manual_reset)
        WakeAll();
    else
        WakeOne(); // auto-reset is applied in RemoveWaitObject when the waiter is removed

    return true;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

inline bool Event::Reset()
{
    ScopedCriticalSection cs_;

    bool prev = m_signaled;

    m_signaled = false;
    __stk_full_memfence();

    return prev;
}

// ---------------------------------------------------------------------------
// Pulse
// ---------------------------------------------------------------------------

inline void Event::Pulse()
{
    ScopedCriticalSection cs_;

    // transition to signaled so that WakeOne/WakeAll can release waiters:
    // m_signaled is only visible as true while this critical section is held —
    // any woken task will have the event auto-reset in RemoveWaitObject before
    // it can observe m_signaled via Wait() or TryWait()
    m_signaled = true;
    __stk_full_memfence();

    if (!m_wait_list.IsEmpty())
    {
        if (m_manual_reset)
            WakeAll();
        else
            WakeOne(); // auto-reset applied in RemoveWaitObject
    }

    // force return to non-signaled regardless of whether anyone was waiting
    m_signaled = false;
    __stk_full_memfence();
}

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

inline bool Event::Wait(Timeout timeout)
{
    STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR

    ScopedCriticalSection cs_;

    // fast path: already signaled
    if (m_signaled)
    {
        if (!m_manual_reset)
        {
            m_signaled = false;
            __stk_full_memfence();
        }

        return true;
    }

    // non-blocking poll: event not signaled, return immediately
    if (timeout == NO_WAIT)
        return false;

    // slow path: block until Set() signals the event or timeout expires
    return !IKernelService::GetInstance()->Wait(this, &cs_, timeout)->IsTimeout();
}

// ---------------------------------------------------------------------------
// TryWait
// ---------------------------------------------------------------------------

inline bool Event::TryWait()
{
    ScopedCriticalSection cs_;

    if (m_signaled)
    {
        if (!m_manual_reset)
        {
            m_signaled = false;
            __stk_full_memfence();
        }

        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// RemoveWaitObject
// ---------------------------------------------------------------------------

inline void Event::RemoveWaitObject(IWaitObject *wobj)
{
    ISyncObject::RemoveWaitObject(wobj);

    // kernel invariant: auto-reset is applied here, not at the Set()/Pulse() call site,
    // when a task wakes due to a signal (not a timeout), the event transitions back to
    // non-signaled so that subsequent Wait() calls block until the next Set().
    if (!m_manual_reset && m_signaled && !wobj->IsTimeout())
    {
        m_signaled = false;
        __stk_full_memfence();
    }
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_EVENT_H_ */
