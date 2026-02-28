/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_TIME_H_
#define STK_C_TIME_H_

#include "stk_c.h"

/*! \file   stk_c_time.h
    \brief  C language binding for stk::time::TimerHost and stk::time::TimerHost::Timer.

    One \a stk_timerhost_t instance is pre-allocated per CPU core (up to
    \a STK_C_CPU_COUNT cores).  Obtain a handle for a given core with
    \a stk_timerhost_get() before calling any other timer API.

    Concrete timers (\a stk_timer_t) are allocated from a fixed-size static
    pool of \a STK_C_TIMER_MAX slots shared across all cores.  Each timer
    stores a user callback (\a stk_timer_callback_t) and an opaque user data
    pointer that are forwarded when the timer expires.

    \defgroup c_api_time STK C Timer API
    \brief    Pure C interface for stk::time::TimerHost and stk::time::TimerHost::Timer.
    @{
*/

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Configuration macros
// ─────────────────────────────────────────────────────────────────────────────

/*! \def   STK_C_TIMER_MAX
    \brief Maximum number of concurrent \a stk_timer_t instances per core (default: 32).
    \note  Increase if your application needs more simultaneous timers.
*/
#ifndef STK_C_TIMER_MAX
    #define STK_C_TIMER_MAX 32
#endif

/*! \def   STK_C_TIMER_HANDLER_STACK_SIZE
    \brief Stack size of the timer handler, increase if your timers consume more (default: 256).
*/
#ifndef STK_C_TIMER_HANDLER_STACK_SIZE
    #define STK_C_TIMER_HANDLER_STACK_SIZE 256
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

/*! \brief  Opaque handle to a \c TimerHost instance (one per CPU core).
*/
typedef struct stk_timerhost_t stk_timerhost_t;

/*! \brief  Opaque handle to a concrete timer managed by \a stk_timerhost_t.
*/
typedef struct stk_timer_t stk_timer_t;

/*! \brief  Timer expiration callback invoked from within the TimerHost handler task.
    \param[in] host:      TimerHost that fired the timer.
    \param[in] timer:     The timer that expired.
    \param[in] user_data: Opaque pointer supplied at timer creation time.
    \warning  Must not call blocking kernel services (e.g. stk_mutex_lock with a
              non-zero timeout) unless the handler task stack is large enough.
*/
typedef void (*stk_timer_callback_t)(stk_timerhost_t *host,
                                     stk_timer_t     *timer,
                                     void            *user_data);

// ─────────────────────────────────────────────────────────────────────────────
// TimerHost — per-core host management
// ─────────────────────────────────────────────────────────────────────────────

/*! \brief     Obtain the pre-allocated TimerHost for the given CPU core.
    \param[in] core_nr: CPU core index (0 … STK_C_CPU_COUNT-1).
    \return    TimerHost handle, or NULL if \a core_nr is out of range.
    \note      The host instance exists in static storage; do not free it.
*/
stk_timerhost_t *stk_timerhost_get(uint8_t core_nr);

/*! \brief     Initialize the TimerHost and register its internal tasks with the kernel.
    \param[in] host:       TimerHost handle obtained via \a stk_timerhost_get().
    \param[in] kernel:     Kernel instance the timer tasks will be added to.
    \param[in] privileged: If \c true the internal handler tasks run in privileged mode,
                           otherwise they run in user mode.
    \note      Must be called before \a stk_kernel_start() and before any
               \a stk_timer_* operations on this host.
*/
void stk_timerhost_init(stk_timerhost_t *host,
                        stk_kernel_t    *kernel,
                        bool             privileged);

/*! \brief     Gracefully shut down the TimerHost.
    \param[in] host: TimerHost handle.
    \return    \c true on success, \c false if the internal command queue is full.
    \note      All active timers are stopped.  After shutdown the host must not be used.
*/
bool stk_timerhost_shutdown(stk_timerhost_t *host);

/*! \brief     Return \c true when no timers are currently active on this host.
    \param[in] host: TimerHost handle.
    \return    Advisory empty flag (may change immediately after the call).
*/
bool stk_timerhost_is_empty(const stk_timerhost_t *host);

/*! \brief     Return the number of currently active timers on this host.
    \param[in] host: TimerHost handle.
    \return    Advisory active timer count (may change immediately after the call).
*/
size_t stk_timerhost_get_size(const stk_timerhost_t *host);

/*! \brief     Return the last tick count snapshot maintained by the host's tick task.
    \param[in] host: TimerHost handle.
    \return    Tick count (may be one tick-task wake cycle stale).
*/
int64_t stk_timerhost_get_time_now(const stk_timerhost_t *host);

// ─────────────────────────────────────────────────────────────────────────────
// Timer lifecycle
// ─────────────────────────────────────────────────────────────────────────────

/*! \brief     Allocate a timer from the static pool.
    \param[in] callback:  Function to call when the timer expires (must not be NULL).
    \param[in] user_data: Opaque pointer forwarded to \a callback on expiration.
    \return    Timer handle, or NULL if the pool is exhausted (\a STK_C_TIMER_MAX reached).
    \note      The returned handle is valid until \a stk_timer_destroy() is called.
               It must not be active (i.e. not currently started) when destroyed.
*/
stk_timer_t *stk_timer_create(stk_timer_callback_t callback, void *user_data);

/*! \brief     Return a timer handle back to the static pool.
    \param[in] timer: Timer handle.
    \warning   The timer must have been stopped (or never started) before calling this.
               Destroying an active timer is a programming error and triggers an assertion.
*/
void stk_timer_destroy(stk_timer_t *timer);

// ─────────────────────────────────────────────────────────────────────────────
// Timer control
// ─────────────────────────────────────────────────────────────────────────────

/*! \brief     Start a timer.
    \param[in] host:   TimerHost that will manage this timer.
    \param[in] timer:  Timer handle.  Must not already be active.
    \param[in] delay:  Initial delay in ticks before the first expiration.
    \param[in] period: Reload period in ticks.  Pass 0 for a one-shot timer.
    \return    \c true on success, \c false if the timer is already active or
               the command queue is full.
*/
bool stk_timer_start(stk_timerhost_t *host,
                     stk_timer_t     *timer,
                     uint32_t         delay,
                     uint32_t         period);

/*! \brief     Stop a running timer.
    \param[in] host:  TimerHost managing the timer.
    \param[in] timer: Timer handle.  Must be currently active.
    \return    \c true on success, \c false if the timer is not active or
               the command queue is full.
*/
bool stk_timer_stop(stk_timerhost_t *host, stk_timer_t *timer);

/*! \brief     Reset a periodic timer's deadline (re-arm from now).
    \param[in] host:  TimerHost managing the timer.
    \param[in] timer: Timer handle.  Must be active and periodic (period != 0).
    \return    \c true on success, \c false if preconditions are not met or
               the command queue is full.
*/
bool stk_timer_reset(stk_timerhost_t *host, stk_timer_t *timer);

/*! \brief     Atomically stop and re-start a timer.
    \details   Unlike calling \a stk_timer_stop() + \a stk_timer_start(), this
               operation is atomic with respect to the tick task: the timer cannot
               fire between the implicit stop and re-start.  Consumes only one
               command queue slot.
    \param[in] host:   TimerHost managing the timer.
    \param[in] timer:  Timer handle (active or inactive).
    \param[in] delay:  Initial delay in ticks before the first expiration.
    \param[in] period: Reload period in ticks (0 = one-shot).
    \return    \c true on success, \c false if the command queue is full.
*/
bool stk_timer_restart(stk_timerhost_t *host,
                       stk_timer_t     *timer,
                       uint32_t         delay,
                       uint32_t         period);

/*! \brief     Start the timer if inactive, or reset its deadline if already active and periodic.
    \details   Collapses the common pattern:
               \code
               if (stk_timer_is_active(t)) stk_timer_reset(host, t);
               else                        stk_timer_start(host, t, delay, period);
               \endcode
               into a single atomic operation, eliminating the TOCTOU race.
               If the timer is active but one-shot, no action is taken.
    \param[in] host:   TimerHost managing the timer.
    \param[in] timer:  Timer handle (active or inactive).
    \param[in] delay:  Initial delay in ticks (used only when starting).
    \param[in] period: Reload period in ticks (used only when starting, 0 = one-shot).
    \return    \c true on success, \c false if the command queue is full.
*/
bool stk_timer_start_or_reset(stk_timerhost_t *host,
                               stk_timer_t     *timer,
                               uint32_t         delay,
                               uint32_t         period);

/*! \brief     Change the period of a running periodic timer without affecting the current deadline.
    \details   The new period takes effect on the next reload after the current deadline fires.
               To apply immediately, follow with \a stk_timer_reset().
    \param[in] host:   TimerHost managing the timer.
    \param[in] timer:  Timer handle.  Must be active and periodic.
    \param[in] period: New reload period in ticks.  Must be non-zero.
    \return    \c true on success, \c false if preconditions are not met or
               the command queue is full.
*/
bool stk_timer_set_period(stk_timerhost_t *host,
                           stk_timer_t     *timer,
                           uint32_t         period);

// ─────────────────────────────────────────────────────────────────────────────
// Timer query
// ─────────────────────────────────────────────────────────────────────────────

/*! \brief     Check whether a timer is currently active (started and not yet expired/stopped).
    \param[in] timer: Timer handle.
    \return    \c true if the timer is active.
    \note      Advisory — may change immediately after the call.
*/
bool stk_timer_is_active(const stk_timer_t *timer);

/*! \brief     Get the timer's reload period.
    \param[in] timer: Timer handle.
    \return    Period in ticks, or 0 for a one-shot timer.
*/
uint32_t stk_timer_get_period(const stk_timer_t *timer);

/*! \brief     Get the absolute expiration tick count of the timer's next deadline.
    \param[in] timer: Timer handle.
    \return    Absolute deadline (ticks).  Meaningful only when the timer is active.
*/
int64_t stk_timer_get_deadline(const stk_timer_t *timer);

/*! \brief     Get the tick count at which the timer last expired.
    \param[in] timer: Timer handle.
    \return    Expiration timestamp (ticks).  Zero if the timer has never fired.
*/
int64_t stk_timer_get_timestamp(const stk_timer_t *timer);

/*! \brief     Get remaining ticks until next expiration.
    \param[in] timer: Timer handle.
    \return    Remaining ticks, or 0 if already expired or not active.
    \note      Computed from the last value written by the host's tick task —
               may be up to one tick-task wake cycle stale.
*/
uint32_t stk_timer_get_remaining_time(const stk_timer_t *timer);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* STK_C_TIME_H_ */
