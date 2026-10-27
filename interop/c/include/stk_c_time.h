/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_TIME_H_
#define STK_C_TIME_H_

#include "stk_c.h"

/*! \file     stk_c_time.h
    \brief    C language binding for stk::time::TimerHost and stk::time::Timer.

    One \a stk_timerhost_t instance is pre-allocated per CPU core (up to
    \a STK_C_CPU_COUNT cores).  Obtain a handle for a given core with
    \a stk_timerhost_get() before calling any other timer API.

    Concrete timers (\a stk_timer_t) are standalone objects constructed in caller-supplied
    memory (\a stk_timer_mem_t), exactly like the objects of the synchronization API
    (stk_mutex_create(), stk_sem_create(), ...): there is no internal timer pool, so the number
    of timers is limited only by the memory the application provides.  Each timer stores a user
    callback (\a stk_timer_callback_t) and an opaque user data pointer that are forwarded when
    the timer expires.

    Typical usage:
    \code
    static stk_timer_mem_t tmr_mem;

    static void OnTimer(stk_timerhost_t *host, stk_timer_t *tmr, void *user_data) { ... }

    stk_timer_t *tmr = stk_timer_create(&tmr_mem, OnTimer, NULL);
    if (stk_timer_start(stk_timerhost_get(0), tmr, 100, 100) != STK_TIMER_RES_OK)
    {
        // e.g. STK_TIMER_RES_NOT_READY, STK_TIMER_RES_QUEUE_FULL
    }
    \endcode

    \defgroup c_api_time STK C Timer API
    \brief    Pure C interface for stk::time::TimerHost and stk::time::Timer.
    @{
*/

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Configuration macros
// =============================================================================

/*! \def   STK_C_COMMAND_QUEUE_MAX
    \brief Capacity of the command queue of every \a stk_timerhost_t (default: 32), forwarded to
           \c STK_TIMER_COMMAND_QUEUE_SIZE of the C++ implementation.
    \note  It does NOT limit the number of timers: timers live in caller-supplied memory
           (\a stk_timer_mem_t) and are only linked into the host while they are active.  The
           queue holds the commands (\a stk_timer_start(), \a stk_timer_stop(), ...) which were
           issued but not yet executed by the tick task, size it for the largest burst of commands
           expected between two runs of the tick task.  When the queue is full a caller which can
           wait (task context, task mode) blocks until a slot is free, any other caller gets
           \a STK_TIMER_RES_QUEUE_FULL (see stk::time::TimerHost, "Command queue").
    \note  Must have the same value in every translation unit of the application.
    \note  For backward compatibility, if this macro is not defined but the deprecated STK_C_TIMER_MAX is,
           the value of STK_C_TIMER_MAX is used.
*/
#ifndef STK_C_TIMER_COMMAND_QUEUE_MAX
    #ifdef STK_C_TIMER_MAX
        #define STK_C_TIMER_COMMAND_QUEUE_MAX STK_C_TIMER_MAX
    #else
    #define STK_C_TIMER_COMMAND_QUEUE_MAX (32U)
    #endif
#endif

/*! \def   STK_C_TIMER_HANDLER_STACK_SIZE
    \brief Stack size of the timer handler, increase if your timers consume more (default: 256).
*/
#ifndef STK_C_TIMER_HANDLER_STACK_SIZE
    #define STK_C_TIMER_HANDLER_STACK_SIZE (256U)
#endif

/*! \def   STK_C_TIMER_THREADS_COUNT
    \brief Number of handler tasks serving expired timers in task mode (default: 1).
           Sizes \a stk_timerhost_config_t::handler_stack and is forwarded to
           \c STK_TIMER_THREADS_COUNT of the C++ implementation.
    \note  Must have the same value in every translation unit of the application.
*/
#ifndef STK_C_TIMER_THREADS_COUNT
    #define STK_C_TIMER_THREADS_COUNT (1U)
#endif

// =============================================================================
// Types
// =============================================================================

/*! \brief  Optional configuration of the timer host tasks, see \a stk_timerhost_init_ex().
    \note   Always fill with \a stk_timerhost_config_init() first, then override the fields of interest.
    \note   Weights are strategy-specific and forwarded to the kernel unchanged (see stk::time::TimerHost,
            "Scheduling weights"). With the preemption-threshold strategy the weights must be set explicitly,
            the default weight is not neutral for it.
    \note   Externally supplied stacks must outlive the host, be distinct from each other and satisfy the
            alignment / memory protection requirements of the platform.
*/
typedef struct stk_timerhost_config_t {
    stk_weight_t tick_weight;                                   //!< Weight of the tick task (default: kernel default weight).
    stk_weight_t handler_weight;                                //!< Weight of every handler task (default: kernel default weight).
    stk_word_t  *tick_stack;                                    //!< Stack of the tick task, or NULL for the internal buffer (default).
    size_t       tick_stack_size;                               //!< Size of \a tick_stack in stk_word_t elements, ignored if \a tick_stack is NULL.
    stk_word_t  *handler_stack[STK_C_TIMER_THREADS_COUNT];      //!< Stack of each handler task, a NULL entry selects the internal buffer (default).
    size_t       handler_stack_size;                            //!< Size of every non-NULL \a handler_stack entry in stk_word_t elements.
} stk_timerhost_config_t;

/*! \brief  Opaque handle to a \c TimerHost instance (one per CPU core).
*/
typedef struct stk_timerhost_t stk_timerhost_t;

/*! \brief  A memory size (multiples of uint64_t) required for the \a stk_timer_t instance.
    \note   The timer holds 64-bit tick values, the size covers LP64 targets (the 32-bit value is
            sufficient for 32-bit ones, but one value keeps the memory type portable). Verified at
            build time by ConstructIn() in stk_timer_create(), increase this value if it fails.
*/
#define STK_TIMER_IMPL_SIZE (14U)

/*! \brief  Opaque memory container for a \a stk_timer_t instance.
    \note   Declare as \c static or on the stack (not on the heap), it must outlive the timer.
    \note   Aligned to 8 bytes because the timer holds 64-bit Ticks values.
*/
typedef struct stk_timer_mem_t {
    uint64_t data[STK_TIMER_IMPL_SIZE] __stk_c_align(sizeof(uint64_t));
} stk_timer_mem_t;

/*! \brief  Opaque handle to a concrete timer managed by \a stk_timerhost_t.
*/
typedef struct stk_timer_t stk_timer_t;

/*! \brief  Outcome of a timer or host command, mirrors stk::time::TimerHost::EResult.
    \note   Negative values are errors: nothing was queued. Positive values report the state of the
            timer, or that the outcome is unknown. \a STK_TIMER_RES_OK is 0: a check like
            \c if(stk_timer_start(...)) means "failed", compare with \a STK_TIMER_RES_OK explicitly.
    \note   The control functions are asynchronous: \a STK_TIMER_RES_OK means the command was accepted
            into the command queue, it is executed later by the tick task and its outcome is not
            reported. The timer state checks (ACTIVE / INACTIVE / NOT_PERIODIC) are advisory: they
            are evaluated from the state applied by the tick task so far. The synchronous functions
            (\a stk_timer_start_sync() and the others) report the exact outcome instead.
*/
typedef enum stk_timer_result_t {
    STK_TIMER_RES_CANCELLED     = -5, //!< The wait for a free command queue slot was cancelled, nothing is queued.
    STK_TIMER_RES_INVALID_PARAM = -4, //!< An argument is out of range (e.g. zero period for stk_timer_set_period()).
    STK_TIMER_RES_QUEUE_FULL    = -3, //!< Command queue is full and the caller can not wait (ISR, kernel not started, polled mode, tick task).
    STK_TIMER_RES_NOT_READY     = -2, //!< Host is not initialized (or its initialization failed), or it has been shut down.
    STK_TIMER_RES_FAIL          = -1, //!< Unspecified failure.
    STK_TIMER_RES_OK            = 0,  //!< The command was accepted into the command queue.
    STK_TIMER_RES_ACTIVE        = 1,  //!< The timer is active, but the command requires it to be inactive.
    STK_TIMER_RES_INACTIVE      = 2,  //!< The timer is not active, but the command requires it to be active.
    STK_TIMER_RES_NOT_PERIODIC  = 3,  //!< The timer is not periodic, but the command requires it to be.
    STK_TIMER_RES_QUEUED        = 4   //!< Synchronous API only: the calling context can not wait, the command was queued asynchronously, its outcome is unknown.
} stk_timer_result_t;

/*! \brief     Timer expiration callback invoked from within the TimerHost handler task (task mode)
               or inline from \a stk_timerhost_process() (polled mode).
    \param[in] host: TimerHost that fired the timer.
    \param[in] timer: The timer that expired.
    \param[in] user_data: Opaque pointer supplied at timer creation time.
    \warning   Must not call blocking kernel services (e.g. stk_mutex_lock with a
               non-zero timeout) unless the handler task stack is large enough.
*/
typedef void (*stk_timer_callback_t)(stk_timerhost_t *host,
                                     stk_timer_t     *timer,
                                     void            *user_data);

// =============================================================================
// TimerHost - per-core host management
// =============================================================================

/*! \brief     Obtain the pre-allocated TimerHost for the given CPU core.
    \param[in] core_nr: CPU core index (0 … STK_C_CPU_COUNT-1).
    \return    TimerHost handle, or NULL if \a core_nr is out of range.
    \note      The host instance exists in static storage; do not free it.
*/
stk_timerhost_t *stk_timerhost_get(uint8_t core_nr);

/*! \brief     Initialize the TimerHost and register its internal tasks with the kernel.
    \param[in] host: TimerHost handle obtained via \a stk_timerhost_get().
    \param[in] kernel: Kernel instance the timer tasks will be added to.
    \param[in] privileged: If \c true the internal handler tasks run in privileged mode,
               otherwise they run in user mode.
    \note      Must be called before \a stk_kernel_start() and before any
               \a stk_timer_* operations on this host.
*/
void stk_timerhost_init(stk_timerhost_t *host,
                        stk_kernel_t    *kernel,
                        bool             privileged);

/*! \brief      Fill a configuration with defaults (default weights, internal stack buffers).
    \param[out] cfg: Configuration to initialize.
*/
void stk_timerhost_config_init(stk_timerhost_config_t *cfg);

/*! \brief     Initialize the TimerHost in task mode with explicit weights / stacks and report failures.
    \details   Same as \a stk_timerhost_init() but applies \a cfg and returns the result of adding the
               internal tasks to the kernel.
    \param[in] host: TimerHost handle obtained via \a stk_timerhost_get().
    \param[in] kernel: Kernel instance the timer tasks will be added to.
    \param[in] privileged: If \c true the handler tasks run in privileged mode, otherwise in user mode
               (the tick task always runs in user mode).
    \param[in] cfg: Weights and stacks, or NULL for defaults (equivalent to \a stk_timerhost_config_init()).
    \return    STK_ADD_OK on success, otherwise the first failure reported by the kernel, e.g.
               STK_ADD_FAIL_POOL_FULL (no free task slots), STK_ADD_FAIL_DUPLICATE (shared stack),
               STK_ADD_FAIL_INVALID (e.g. HRT kernel: use \a stk_timerhost_init_polled() there).
    \note      On failure the host is unusable; tasks added before the failure stay registered with the kernel.
    \note      The host occupies 1 + \a STK_C_TIMER_THREADS_COUNT kernel task slots.
    \see       stk_add_result_t, stk_timerhost_config_t
*/
stk_add_result_t stk_timerhost_init_ex(stk_timerhost_t              *host,
                                       stk_kernel_t                 *kernel,
                                       bool                          privileged,
                                       const stk_timerhost_config_t *cfg);

/*! \brief     Initialize the TimerHost in polled mode (no kernel tasks are created).
    \details   The application drives the host by calling \a stk_timerhost_process() periodically; expired
               timer callbacks run inline from that call. This is the mode to use with an HRT kernel,
               where Initialize/\a stk_timerhost_init() is rejected.
    \param[in] host: TimerHost handle.
    \note      Mutually exclusive with \a stk_timerhost_init() / \a stk_timerhost_init_ex(), call one of them
               before any other timer API. Cannot fail, needs no kernel, task slots, weights or stacks.
*/
void stk_timerhost_init_polled(stk_timerhost_t *host);

/*! \brief     Run the host once (polled mode only): execute pending commands, evaluate deadlines and
               invoke the callback of every expired timer inline.
    \param[in] host: TimerHost handle initialized with \a stk_timerhost_init_polled().
    \return    Ticks until the earliest active deadline: \c STK_NO_WAIT if a timer is already overdue
               (call again without delay), \c STK_WAIT_INFINITE if no timer is active.
    \note      Never blocks. Call from a single context only, not concurrently and not from a timer callback.
               \a stk_timer_start() and the other control functions stay callable from any task and take
               effect at the next call.
    \note      A timer fires on the first call at or after its deadline, so the calling period is the timer
               resolution. Callback execution time counts against the caller (e.g. the WCET of an HRT job).
    \warning   Only valid after \a stk_timerhost_init_polled().
*/
stk_timeout_t stk_timerhost_process(stk_timerhost_t *host);

/*! \brief     Gracefully shut down the TimerHost.
    \param[in] host: TimerHost handle.
    \return    \a STK_TIMER_RES_OK if the request was accepted, otherwise the error, e.g.
               \a STK_TIMER_RES_QUEUE_FULL or \a STK_TIMER_RES_NOT_READY (not initialized or already shut down).
    \note      All active timers are stopped.  Task mode: after shutdown the host must not be used until
               it is initialized again.  Polled mode: the host stays usable.
*/
stk_timer_result_t stk_timerhost_shutdown(stk_timerhost_t *host);

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

// =============================================================================
// Timer lifecycle
// =============================================================================

/*! \brief     Construct a timer in the supplied memory buffer.
    \param[in] membuf: Pointer to the caller-supplied memory container, must not be NULL.
    \param[in] callback: Function to call when the timer expires (must not be NULL).
    \param[in] user_data: Opaque pointer forwarded to \a callback on expiration.
    \return    Timer handle on success, or NULL if \a membuf is NULL.
    \note      The handle is valid until \a stk_timer_destroy() is called and \a membuf must stay valid
               (not reused, not freed) for that time.  There is no limit on the number of timers.
    \note      The timer is created inactive, start it with \a stk_timer_start().
*/
stk_timer_t *stk_timer_create(stk_timer_mem_t *const membuf,
                              stk_timer_callback_t   callback,
                              void                  *user_data);

/*! \brief     Destroy a timer (calls the C++ destructor in-place).
    \param[in] tmr: Timer handle. May be NULL (no-op).
    \warning   The timer must have been stopped (or never started) before calling this.
               Destroying an active timer is a programming error and triggers an assertion.
    \warning   The timer memory is owned by the caller and may be reused right after this call.  Stop the
               timer with \a stk_timer_stop_sync() first: it returns only when the host holds no reference
               to the timer and no callback of it is running.  The asynchronous \a stk_timer_stop() only
               queues the request and gives no such guarantee.
*/
void stk_timer_destroy(stk_timer_t *tmr);

// =============================================================================
// Timer control
// =============================================================================

/*! \brief     Start a timer.
    \param[in] host: TimerHost that will manage this timer.
    \param[in] tmr: Timer handle.  Must not already be active.
    \param[in] delay: Initial delay in ticks before the first expiration.
    \param[in] period: Reload period in ticks.  Pass 0 for a one-shot timer.
    \return    \a STK_TIMER_RES_OK if the command was accepted, otherwise the reason:
               \a STK_TIMER_RES_ACTIVE, \a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL, ...
*/
stk_timer_result_t stk_timer_start(stk_timerhost_t *host,
                                   stk_timer_t     *tmr,
                                   uint32_t         delay,
                                   uint32_t         period);

/*! \brief     Stop a running timer.
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle.  Must be currently active.
    \return    \a STK_TIMER_RES_OK if the command was accepted, otherwise the reason:
               \a STK_TIMER_RES_INACTIVE, \a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL, ...
*/
stk_timer_result_t stk_timer_stop(stk_timerhost_t *host, stk_timer_t *tmr);

/*! \brief     Reset a periodic timer's deadline (re-arm from now).
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle.  Must be active and periodic (period != 0).
    \return    \a STK_TIMER_RES_OK if the command was accepted, otherwise the reason:
               \a STK_TIMER_RES_INACTIVE, \a STK_TIMER_RES_NOT_PERIODIC, \a STK_TIMER_RES_NOT_READY,
               \a STK_TIMER_RES_QUEUE_FULL, ...
*/
stk_timer_result_t stk_timer_reset(stk_timerhost_t *host, stk_timer_t *tmr);

/*! \brief     Atomically stop and re-start a timer.
    \details   Unlike calling \a stk_timer_stop() + \a stk_timer_start(), this
               operation is atomic with respect to the tick task: the timer cannot
               fire between the implicit stop and re-start.  Consumes only one
               command queue slot.
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle (active or inactive).
    \param[in] delay: Initial delay in ticks before the first expiration.
    \param[in] period: Reload period in ticks (0 = one-shot).
    \return    \a STK_TIMER_RES_OK if the command was accepted, otherwise the reason:
               \a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL, ...
*/
stk_timer_result_t stk_timer_restart(stk_timerhost_t *host,
                                     stk_timer_t     *tmr,
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
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle (active or inactive).
    \param[in] delay: Initial delay in ticks (used only when starting).
    \param[in] period_ticks: Reload period in ticks (used only when starting, 0 = one-shot).
    \return    \a STK_TIMER_RES_OK if the command was accepted, otherwise the reason:
               \a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL, ...
*/
stk_timer_result_t stk_timer_start_or_reset(stk_timerhost_t *host,
                                            stk_timer_t     *tmr,
                                            uint32_t         delay,
                                            uint32_t         period_ticks);

/*! \brief     Change the period of a running periodic timer without affecting the current deadline.
    \details   The new period takes effect on the next reload after the current deadline fires.
               To apply immediately, follow with \a stk_timer_reset().
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle.  Must be active and periodic.
    \param[in] period_ticks: New reload period in ticks.  Must be non-zero.
    \return    \a STK_TIMER_RES_OK if the command was accepted, otherwise the reason:
               \a STK_TIMER_RES_INVALID_PARAM, \a STK_TIMER_RES_INACTIVE, \a STK_TIMER_RES_NOT_PERIODIC,
               \a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL, ...
*/
stk_timer_result_t stk_timer_set_period(stk_timerhost_t *host,
                                        stk_timer_t     *tmr,
                                        uint32_t         period_ticks);

// =============================================================================
// Timer control - synchronous API
// =============================================================================

/*! \brief  Consistent snapshot of a timer, see \a stk_timer_query_sync().
*/
typedef struct stk_timer_info_t {
    bool     active;    //!< true if the timer is active.
    uint32_t remaining; //!< Ticks until the next expiration, 0 if not active or overdue.
    uint32_t delay;     //!< Initial delay stored by the last start / restart / change.
    uint32_t period;    //!< Reload period in ticks, 0 for a one-shot timer.
} stk_timer_info_t;

/*! \defgroup c_api_timer_sync STK C Timer Synchronous API
    \brief    Blocking variants of the timer control functions.
    \details  The asynchronous functions above only queue a request and report whether it was accepted. The
               functions below queue the same command together with a completion point and block the caller
               until the tick task has executed it, so the result is the exact outcome of the command (e.g.
               \a STK_TIMER_RES_ACTIVE for a timer which was already active).
    \note     A call waits only where waiting is possible: task context, kernel running, task mode. From an ISR,
               before the kernel is started, or in polled mode the command is queued asynchronously and
               \a STK_TIMER_RES_QUEUED is returned (\a STK_TIMER_RES_QUEUE_FULL if the queue is full).
    \note     A sync call blocks while the command queue is full instead of failing (see \a STK_C_TIMER_COMMAND_QUEUE_MAX).
    \note     The tick task must be at least as urgent as every task which uses this API, see
               \a stk_timerhost_config_t (weights).
    \note     Callbacks may use this API in task mode (they run in handler tasks).
    @{
*/

/*! \brief     Start a timer and wait for the result.
    \param[in] host: TimerHost that will manage this timer.
    \param[in] tmr: Timer handle.  Must not be active.
    \param[in] delay: Initial delay in ticks before the first expiration (stored in the timer).
    \param[in] period: Reload period in ticks, 0 for a one-shot timer (stored in the timer).
    \return    \a STK_TIMER_RES_OK if started, \a STK_TIMER_RES_ACTIVE if already active (nothing is changed),
               \a STK_TIMER_RES_QUEUED, or an error (\a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL,
               \a STK_TIMER_RES_CANCELLED).
*/
stk_timer_result_t stk_timer_start_sync(stk_timerhost_t *host,
                                        stk_timer_t     *tmr,
                                        uint32_t         delay,
                                        uint32_t         period);

/*! \brief     Start a timer with the delay and period stored in it and wait for the result.
    \param[in] host: TimerHost that will manage this timer.
    \param[in] tmr: Timer handle.  Must not be active.
    \return    As \a stk_timer_start_sync().
    \note      The parameters are those of the last start, restart or \a stk_timer_change_sync(), the
               expiration is relative to the moment of the call.
*/
stk_timer_result_t stk_timer_activate_sync(stk_timerhost_t *host, stk_timer_t *tmr);

/*! \brief     Stop a timer and wait for the result.
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle.
    \return    \a STK_TIMER_RES_OK if it was active, \a STK_TIMER_RES_INACTIVE if it was not (e.g. a one-shot which
               already expired; its queued callback is cancelled if it has not started yet),
               \a STK_TIMER_RES_QUEUED, or an error (\a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL,
               \a STK_TIMER_RES_CANCELLED).
    \note      Also waits until a callback of this timer which a handler task has already started has returned:
               after \a STK_TIMER_RES_OK / \a STK_TIMER_RES_INACTIVE the host holds no reference to the timer and
               its memory (\a stk_timer_mem_t) may be released, see \a stk_timer_destroy().
    \warning   Called from a timer callback it does not wait for running callbacks (deadlock guard): it only
               guarantees that no further callback of the timer starts.  Do not release the timer from a
               callback of another timer.
*/
stk_timer_result_t stk_timer_stop_sync(stk_timerhost_t *host, stk_timer_t *tmr);

/*! \brief     Atomically stop and re-start a timer and wait for the result.
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle (active or inactive).
    \param[in] delay: Initial delay in ticks before the first expiration (stored in the timer).
    \param[in] period: Reload period in ticks, 0 for a one-shot timer (stored in the timer).
    \return    \a STK_TIMER_RES_OK, \a STK_TIMER_RES_QUEUED, or an error (\a STK_TIMER_RES_NOT_READY,
               \a STK_TIMER_RES_QUEUE_FULL, \a STK_TIMER_RES_CANCELLED).
*/
stk_timer_result_t stk_timer_restart_sync(stk_timerhost_t *host,
                                          stk_timer_t     *tmr,
                                          uint32_t         delay,
                                          uint32_t         period);

/*! \brief     Reset a periodic timer's deadline (re-arm from now) and wait for the result.
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle.  Must be active and periodic.
    \return    \a STK_TIMER_RES_OK, \a STK_TIMER_RES_INACTIVE, \a STK_TIMER_RES_NOT_PERIODIC,
               \a STK_TIMER_RES_QUEUED, or an error (\a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL,
               \a STK_TIMER_RES_CANCELLED).
*/
stk_timer_result_t stk_timer_reset_sync(stk_timerhost_t *host, stk_timer_t *tmr);

/*! \brief     Change the period of a running periodic timer without affecting the current deadline and
               wait for the result.
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle.  Must be active and periodic.
    \param[in] period_ticks: New reload period in ticks.  Must be non-zero.
    \return    \a STK_TIMER_RES_OK, \a STK_TIMER_RES_INACTIVE or \a STK_TIMER_RES_NOT_PERIODIC (nothing is changed),
               \a STK_TIMER_RES_INVALID_PARAM (zero or too large period, nothing is queued),
               \a STK_TIMER_RES_QUEUED, or an error (\a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL,
               \a STK_TIMER_RES_CANCELLED).
    \note      Unlike \a stk_timer_set_period() the state is evaluated by the tick task, so the outcome is exact.
               The new period takes effect on the next reload, follow with \a stk_timer_reset_sync() to
               apply it immediately.
*/
stk_timer_result_t stk_timer_set_period_sync(stk_timerhost_t *host,
                                             stk_timer_t     *tmr,
                                             uint32_t         period_ticks);

/*! \brief     Change the delay and period of an inactive timer and wait for the result.
    \param[in] host: TimerHost managing the timer.
    \param[in] tmr: Timer handle.  Must not be active.
    \param[in] delay: New initial delay in ticks.
    \param[in] period: New reload period in ticks, 0 for a one-shot timer.
    \return    \a STK_TIMER_RES_OK, \a STK_TIMER_RES_ACTIVE if the timer is active (nothing is changed),
               \a STK_TIMER_RES_QUEUED, or an error (\a STK_TIMER_RES_NOT_READY, \a STK_TIMER_RES_QUEUE_FULL,
               \a STK_TIMER_RES_CANCELLED).
    \note      The timer is not started, the values are used by the next \a stk_timer_activate_sync().
*/
stk_timer_result_t stk_timer_change_sync(stk_timerhost_t *host,
                                         stk_timer_t     *tmr,
                                         uint32_t         delay,
                                         uint32_t         period);

/*! \brief      Get a consistent snapshot of a timer (evaluated by the tick task).
    \param[in]  host: TimerHost managing the timer.
    \param[in]  tmr: Timer handle.
    \param[out] info: Snapshot, valid if the result is \a STK_TIMER_RES_OK.
    \return     \a STK_TIMER_RES_OK, or \a STK_TIMER_RES_NOT_READY if the host is not initialized or has been shut down.
    \note       From a context which can not wait the snapshot is read without synchronization and is advisory
                (\a STK_TIMER_RES_OK is returned).
*/
stk_timer_result_t stk_timer_query_sync(stk_timerhost_t *host, stk_timer_t *tmr, stk_timer_info_t *info);

/** @} */

// =============================================================================
// Timer query
// =============================================================================

/*! \brief     Check whether a timer is currently active (started and not yet expired/stopped).
    \param[in] tmr: Timer handle.
    \return    \c true if the timer is active.
    \note      Advisory - may change immediately after the call.
*/
bool stk_timer_is_active(const stk_timer_t *tmr);

/*! \brief     Get the timer's reload period.
    \param[in] tmr: Timer handle.
    \return    Period in ticks, or 0 for a one-shot timer.
*/
uint32_t stk_timer_get_period(const stk_timer_t *tmr);

/*! \brief     Get the absolute expiration tick count of the timer's next deadline.
    \param[in] tmr: Timer handle.
    \return    Absolute deadline (ticks).  Meaningful only when the timer is active.
*/
int64_t stk_timer_get_deadline(const stk_timer_t *tmr);

/*! \brief     Get the tick count at which the timer last expired.
    \param[in] tmr: Timer handle.
    \return    Expiration timestamp (ticks).  Zero if the timer has never fired.
*/
int64_t stk_timer_get_timestamp(const stk_timer_t *tmr);

/*! \brief     Get remaining ticks until next expiration.
    \param[in] tmr: Timer handle.
    \return    Remaining ticks, or 0 if already expired or not active.
    \note      Computed from the last value written by the host's tick task -
               may be up to one tick-task wake cycle stale.
*/
uint32_t stk_timer_get_remaining_ticks(const stk_timer_t *tmr);

// =============================================================================
// PeriodicTrigger - lightweight in-place periodic polling helper
// =============================================================================

/*! \defgroup c_api_periodic_trigger STK C PeriodicTrigger API
    \brief    Pure C interface for stk::time::PeriodicTrigger.

    Typical usage:
    \code
    static stk_periodic_trigger_mem_t mem;
    stk_periodic_trigger_t *trig = stk_periodic_trigger_create(&mem, 500, true);

    // Inside a task loop:
    if (stk_periodic_trigger_poll(trig))
    {
        // executed once per 500-tick period
    }
    \endcode
    @{
*/

/*! \brief  A memory size (multiples of stk_word_t) required for PeriodicTrigger instance.
*/
#define STK_PERIODIC_TRIGGER_IMPL_SIZE (4U)

/*! \brief  Opaque memory container for a \a stk_periodic_trigger_t instance.
    \note   Declare as \c static or on the stack (not on the heap).
    \note   Aligned to 8 bytes due to PeriodicTrigger using 64-bit Ticks type.
*/
typedef struct stk_periodic_trigger_mem_t {
    stk_word_t data[STK_PERIODIC_TRIGGER_IMPL_SIZE] __stk_c_align(8U);
} stk_periodic_trigger_mem_t;

/*! \brief  Opaque handle to a \a stk::time::PeriodicTrigger instance.
*/
typedef struct stk_periodic_trigger_t stk_periodic_trigger_t;

/*! \brief     Construct PeriodicTrigger instance in the supplied memory buffer.
    \param[in] membuf: Pointer to the caller-supplied memory container.
    \param[in] period_ticks: Trigger period in ticks. Must be > 0.
    \param[in] started: \c true to create the instance in a started state (first firing occurs
               no earlier than \a period ticks after construction); \c false to create it in a
               stopped state (call \a stk_periodic_trigger_restart() before polling).
    \return    Trigger handle on success, or \c NULL if \a membuf is \c NULL.
*/
stk_periodic_trigger_t *stk_periodic_trigger_create(stk_periodic_trigger_mem_t *const membuf,
                                                    uint32_t                    period_ticks,
                                                    bool                        started);

/*! \brief     Destroy instance (calls the C++ destructor in-place).
    \param[in] trig: Trigger handle. May be \c NULL (no-op).
*/
void stk_periodic_trigger_destroy(stk_periodic_trigger_t *const trig);

/*! \brief     Check whether the scheduled trigger time has been reached.
    \param[in] trig: Trigger handle (must be started).
    \return    \c true once when the current tick count reaches or exceeds
               the scheduled trigger time, \c false otherwise.
    \note      Implements absolute-time scheduling. When firing occurs,
               the internal next-trigger time is advanced by exactly one
               period (not reset to the current time), preserving long-term
               frequency stability.
    \note      At most one \c true is returned per call. If multiple full
               periods have elapsed since the previous call, subsequent
               calls will continue advancing one period at a time until
               the schedule catches up.
*/
bool stk_periodic_trigger_poll(stk_periodic_trigger_t *trig);

/*! \brief     Change the trigger period while preserving phase.
    \param[in] trig: Trigger handle.
    \param[in] period_ticks: New trigger period in ticks. Must be > 0.
    \note      Adjusts the internally stored next-trigger time so that the
               relative progress toward the next firing is preserved.
               Takes effect immediately.
*/
void stk_periodic_trigger_set_period(stk_periodic_trigger_t *trig, uint32_t period_ticks);

/*! \brief     Reset and start the trigger from the current tick count.
    \param[in] trig: Trigger handle.
    \note      Sets the internal next-trigger time to (current ticks + period).
               The next firing occurs no earlier than \a period ticks after this call.
               Does not change the configured period.
*/
void stk_periodic_trigger_restart(stk_periodic_trigger_t *trig);

/*! \brief     Get currently configured trigger period.
    \param[in] trig: Trigger handle.
    \return    Period in ticks.
*/
uint32_t stk_periodic_trigger_get_period(const stk_periodic_trigger_t *trig);

/** @} */

#ifdef __cplusplus
}
#endif

// =============================================================================
// C++ instance access
// =============================================================================
// Deliberately outside the extern "C" block above: the namespace declarations
// below require genuine C++ linkage, which extern "C" would break. This section
// is invisible to C translation units.
//
// Only forward declarations are made here, the full definitions are NOT pulled in.
// C++ code which dereferences the returned pointers must include "time/stk_time.h"
// itself, after this header: the configuration macros below must be in effect before
// stk_time.h is included.

#ifdef __cplusplus
// Fail loudly if stk_time.h was included first with a different configuration: TimerHost would be
// sized differently here and in the C binding (ODR violation). Include this header before stk_time.h.
#ifdef STK_TIME_TIMER_H_
    #if (STK_TIMER_COMMAND_QUEUE_SIZE != STK_C_TIMER_COMMAND_QUEUE_MAX) || \
        (STK_TIMER_THREADS_COUNT      != STK_C_TIMER_THREADS_COUNT)      || \
        (STK_TIMER_HANDLER_STACK_SIZE != STK_C_TIMER_HANDLER_STACK_SIZE)
        #error "stk_c_time.h must be included before stk_time.h (TimerHost configuration mismatch)"
    #endif
#endif

// STK_C_TIMER_COMMAND_QUEUE_MAX is the capacity of the command queue.
#undef STK_TIMER_COUNT_MAX
#undef STK_TIMER_COMMAND_QUEUE_SIZE
#define STK_TIMER_COMMAND_QUEUE_SIZE (STK_C_TIMER_COMMAND_QUEUE_MAX)
#undef STK_TIMER_THREADS_COUNT
#define STK_TIMER_THREADS_COUNT (STK_C_TIMER_THREADS_COUNT)
#undef STK_TIMER_HANDLER_STACK_SIZE
#define STK_TIMER_HANDLER_STACK_SIZE (STK_C_TIMER_HANDLER_STACK_SIZE)

namespace stk {
namespace time {
    class TimerHost;
    class Timer;
    class PeriodicTrigger;
} // namespace time
} // namespace stk

/*! \brief     Get the underlying C++ \c stk::time::TimerHost object wrapped by a \c stk_timerhost_t handle.
    \param[in] host: TimerHost handle obtained via \c stk_timerhost_get().
    \return    Pointer to the wrapped \c stk::time::TimerHost instance. Never \c NULL for a valid handle.
    \note      C++ callers only (guarded by \c __cplusplus); not part of the C ABI.
*/
extern "C" stk::time::TimerHost *stk_timerhost_get_instance(stk_timerhost_t *host);

/*! \brief     Get the underlying C++ \c stk::time::Timer object wrapped by a \c stk_timer_t handle.
    \param[in] tmr: Timer handle obtained via \c stk_timer_create().
    \return    Pointer to the wrapped \c stk::time::Timer instance. Never \c NULL for a valid handle.
    \note      C++ callers only (guarded by \c __cplusplus); not part of the C ABI.
    \warning   The concrete object is an internal subclass used to bridge to the C callback;
               do not \c static_cast or \c delete it - treat it strictly as a
               \c stk::time::Timer base pointer.
*/
extern "C" stk::time::Timer *stk_timer_get_instance(stk_timer_t *tmr);

/*! \brief     Get the underlying C++ \c stk::time::PeriodicTrigger object wrapped by a \c stk_periodic_trigger_t handle.
    \param[in] trig: Trigger handle obtained via \c stk_periodic_trigger_create().
    \return    Pointer to the wrapped \c stk::time::PeriodicTrigger instance. Never \c NULL for a valid handle.
    \note      C++ callers only (guarded by \c __cplusplus); not part of the C ABI.
*/
extern "C" stk::time::PeriodicTrigger *stk_periodic_trigger_get_instance(stk_periodic_trigger_t *trig);
#endif // __cplusplus

/** @} */

#endif /* STK_C_TIME_H_ */
