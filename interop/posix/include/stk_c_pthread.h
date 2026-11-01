/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_PTHREAD_H_
#define STK_C_PTHREAD_H_

// Undefine system headers:
#undef _POSIX_THREADS

// The toolchain's own pthread types must not be defined: this header provides them. All headers of the layer
// install the guard below before any toolchain header is read, but a toolchain header which was included EARLIER
// (<sys/types.h>, <pthread.h>, <signal.h> of the toolchain) has already defined them: reported clearly here.
#if defined(_SYS__PTHREADTYPES_H_) && !defined(STK_C_PTHREADTYPES_GUARD)
    #error "The toolchain's pthread types are already defined: include the POSIX layer's headers (pthread.h, time.h, signal.h, ...) before any toolchain header, and put the layer's include directory ahead of the toolchain's"
#endif
#ifndef _SYS__PTHREADTYPES_H_
    #define _SYS__PTHREADTYPES_H_
    #define STK_C_PTHREADTYPES_GUARD
#endif

// Self-contained: does not depend on the C API (stk_c.h). Only the kernel configuration is needed to size the
// opaque storage of the C++ objects the shim constructs in place.
#ifdef __cplusplus
    #include <cstdint>
    #include <cstddef>
    #include <ctime>
#else
    #include <stdint.h>
    #include <stddef.h>
    #include <stdbool.h>
    #include <time.h>
#endif

#include <stk_config.h>

#include "sched.h" // struct sched_param, SCHED_*, sched_yield() and friends

// must match the definition used by the C++ kernel API (stk_c.h and stk.h derive it the same way)
#ifndef STK_SYNC_DEBUG_NAMES
    #if STK_SEGGER_SYSVIEW
        #define STK_SYNC_DEBUG_NAMES (1)
    #else
        #define STK_SYNC_DEBUG_NAMES (0)
    #endif
#endif

/*! \file     stk_c_pthread.h
    \brief    A minimal, POSIX-named pthreads-style API for STK. The public interface
              is self-contained (it needs neither stk_c.h nor any other STK header besides
              stk_config.h, and no C++ headers), so it can be used from C. The
              implementation (stk_c_pthread.cpp) calls the STK C++ API directly for
              thread/task control, TLS, critical sections, sleep/yield/time and the
              synchronization primitives; the pthread_stk_*_mem_t blobs embedded in the
              pthread_* types are used as in-place storage for those C++ objects.

    \details  Covers thread lifecycle and attributes, mutexes, condition variables,
              read-write locks, spinlocks, barriers, pthread_once() and thread-specific
              data. This is deliberately a subset of full POSIX threads, scoped to what
              maps cleanly onto STK's task/synchronization model:

    | POSIX area                                                                 | Support                                                                                  |
    |----------------------------------------------------------------------------|------------------------------------------------------------------------------------------|
    | pthread_create/join/detach/exit/self/equal                                 | Yes                                                                                      |
    | pthread_yield (non-POSIX; POSIX uses sched_yield())                        | Yes  (GNU/BSD-style extension)                                                           |
    | pthread_attr_t (stacksize, stack, detachstate)                             | Yes                                                                                      |
    | pthread_attr_set/getprivileged_np                                          | Yes  (non-portable STK extension)                                                        |
    | pthread_mutex_t (normal/default)                                           | Yes  (recursive in practice, see PTHREAD_MUTEX_NORMAL note)                              |
    | pthread_mutex_t (recursive, errorcheck)                                    | Yes                                                                                      |
    | pthread_cond_t (wait/timedwait/signal/broadcast)                           | Yes                                                                                      |
    | pthread_rwlock_t (rdlock/wrlock/tryrdlock/trywrlock/timed variants/unlock) | Yes                                                                                      |
    | pthread_spin_* (init/destroy/lock/trylock/unlock)                          | Yes                                                                                      |
    | pthread_barrier_t (init/destroy/wait)                                      | Yes                                                                                      |
    | pthread_once                                                               | Yes                                                                                      |
    | pthread_key_t (create/delete/setspecific/getspecific, destructors)         | Yes                                                                                      |
    | pthread_attr_set/getschedparam/schedpolicy/inheritsched/scope/guardsize    | Yes  (priority = task weight at creation; policy informational)                          |
    | pthread_getschedparam / pthread_setschedparam / pthread_setschedprio       | Yes  (run-time change via stk::SetWeight(), needs SwitchStrategyFixedPriority)           |
    | sched_yield, sched_get_priority_min/max  (<sched.h>)                       | Yes                                                                                      |
    | pthread_set/getname_np, pthread_attr_set/getname_np                        | Yes  (GNU-style extension)                                                               |
    | pthread_timedjoin_np / pthread_tryjoin_np                                  | Yes  (GNU-style extension)                                                               |
    | pthread_mutex_clocklock / cond_clockwait / rwlock_clockrd/wrlock           | Yes  (CLOCK_REALTIME and CLOCK_MONOTONIC share the STK tick timeline)                    |
    | pthread_*attr_set/getpshared, mutexattr protocol, condattr clock           | Yes  (PROCESS_PRIVATE only; PRIO_NONE/INHERIT)                                           |
    | sem_init/destroy/wait/trywait/timedwait/clockwait/post/getvalue  (<semaphore.h>)| Yes  (unnamed semaphores, see semaphore.h)                                               |
    | stk_pthread_bind_kernel()                                                  | Yes  (STK-specific bootstrap, not POSIX; required before pthread_create())               |
    | pthread_cancel/setcancelstate/setcanceltype/testcancel/cleanup_push/pop    | Yes  (deferred only; asynchronous cancellation: ENOTSUP)                                 |
    | pthread_getattr_np                                                         | Yes  (GNU-style extension)                                                               |
    | clock_gettime/getres, nanosleep, clock_nanosleep, usleep, sleep, timer_*  (<time.h>)| Yes  (see time.h; timers need a TimerHost, stk_posix_timer_bind_host())                  |
    | gettimeofday (<sys/time.h>), time (<time.h>)                                        | Yes  (kernel time + optional offset, stk_posix_set_wall_time())                          |
    | sysconf, getpid  (<unistd.h>)                                                       | Yes  (limits of this shim and kernel, single "process")                                  |
    | mq_open/close/unlink/send/receive/timed variants/getattr/setattr  (<mqueue.h>)     | Yes  (priorities honored; mq_notify: ENOSYS)                                             |
    | C11 thrd_*, mtx_*, cnd_*, tss_*, call_once  (<threads.h>)                  | Yes  (wrappers over this shim, see threads.h)                                            |
    | SCHED_FIFO / SCHED_RR policy enforcement                                   | No  (the kernel switch strategy defines scheduling, SCHED_* is informational)            |

    \par Requirements on the STK kernel configuration
    - The kernel type configured for the bound core (\c STK_C_KERNEL_TYPE_CPU_X in
      \c stk_config.h, X being the core number passed to stk_kernel_create()) \b must
      include \c KERNEL_DYNAMIC (threads finish by returning from their entry function
      or calling pthread_exit(), which STK only supports for dynamic kernels) and
      \c KERNEL_SYNC (which enables the Mutex/Event/Semaphore primitives this shim is
      built on). The shim cannot verify this: a mismatch is a build configuration
      error and is not reported through an error code.
    - \c STK_C_KERNEL_MAX_TASKS must be large enough to hold: the application's own
      pre-existing tasks + one internal reaper task (added to the kernel by
      stk_pthread_bind_kernel(), whether or not any thread is ever detached) +
      the maximum number of concurrently-alive pthreads (joined-but-not-yet-collected
      and detached-but-not-yet-reaped threads still count as "alive" until reclaimed).

    \par Memory
    A thread created with the default stack size (\c STK_C_PTHREAD_DEFAULT_STACK_WORDS)
    takes its stack from a static pool and uses no heap. Any other size set via
    pthread_attr_setstacksize() is allocated with \c malloc() and freed when the thread
    is reclaimed. A stack supplied through pthread_attr_setstack() is used as-is and is
    never freed by the shim. The internal reaper task uses a static stack of
    \c STK_C_PTHREAD_REAPER_STACK_WORDS words.
    - \c STK_C_PTHREAD_MAX_THREADS (this file) bounds how many pthread_t's can exist
      concurrently; increase it via a \c -D define or in \c stk_config.h.

    \par Bootstrapping
    This shim never creates or starts a kernel itself. The application must create
    and start its own STK kernel exactly as it would without pthreads, and then call
    \c stk_pthread_bind_kernel() once (before the first \c pthread_create() call) so
    the shim knows which kernel to add new threads to (the call also adds the internal
    reaper task to that kernel):

    \code
    stk_kernel_t *k = stk_kernel_create(0U);
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);
    stk_pthread_bind_kernel(k);

    stk_task_t *main_task = stk_task_create_user(MainEntry, NULL, main_stack, MAIN_STACK_WORDS);
    stk_kernel_add_task(k, main_task);
    stk_kernel_start(k);   // never returns; MainEntry() may now call pthread_create()
    \endcode

    \par pthread_self() and foreign tasks (adoption)
    \c pthread_self() is backed by STK's per-task TLS slot (\c stk_tls_get()), which
    this shim populates for every thread it creates via \c pthread_create(). A task
    the shim did not create - typically the application's bootstrap task added to the
    kernel before \c stk_kernel_start() - is \e adopted on first use: the first call to
    \c pthread_self() or \c pthread_setspecific() from such a task gives it a
    \c pthread_t and stores that handle in its TLS slot. Consequences:
    - each adopted task permanently consumes one \c STK_C_PTHREAD_MAX_THREADS slot, so
      size that limit as (pthread_create()'d threads + adopted tasks);
    - an adopted thread is not joinable and never reaped (pthread_join() and
      pthread_detach() return \c EINVAL for it);
    - the task's TLS slot now belongs to this shim: the application must not call
      \c stk_tls_set() in a task that uses the pthread API (it would replace the handle);
    - \c pthread_exit() from an adopted task runs its key destructors and then parks
      the task forever (the shim holds no kernel task object for it, so it cannot ask
      the kernel to remove it);
    - \c pthread_getspecific() never adopts (it returns \c NULL for a task that has not
      yet stored any value), so merely reading does not consume a slot.
    \c pthread_self() returns \c NULL only if no slot is left to adopt into.

    \par Context restrictions
    The blocking calls - pthread_join(), the lock/wait functions of mutexes, condition
    variables and read-write locks (including the timed variants, which block until the
    deadline), pthread_barrier_wait() and pthread_once() - must not be called from an
    ISR: the underlying STK primitives are ISR-unsafe for blocking operations. STK's
    non-blocking operations (try-lock, unlock, notify) are ISR-safe, but the shim adds
    lazy initialization and bookkeeping around them, so treat the pthread API as
    task-context only.

    \par pthread_cond_timedwait() / pthread_mutex_timedlock() limitation
    STK has no wall-clock (\c CLOCK_REALTIME) concept - only a monotonic tick count
    since \c stk_kernel_init(). \a abstime is therefore a point on the timeline of
    \c stk::GetTimeNowMs() (C alias: \c stk_time_now_ms()): derive it by adding a
    duration to a value read from that function, never from a time-of-day clock:

    \code
    stk_time_t deadline_ms = stk_time_now_ms() + 500;   // 500 ms from now
    struct timespec abstime;
    abstime.tv_sec  = (time_t)(deadline_ms / 1000);
    abstime.tv_nsec = (long)((deadline_ms % 1000) * 1000000);
    int rc = pthread_cond_timedwait(&cond, &mutex, &abstime);   // 0 or ETIMEDOUT
    \endcode

    \a abstime is converted to a relative timeout in milliseconds (and then to kernel
    ticks). A deadline that is already in the past yields a zero timeout, so the call
    behaves like a non-blocking attempt, and very distant deadlines are clamped to
    \c INT32_MAX ms. The remaining time is rounded UP to whole kernel ticks, so a call
    never reports \c ETIMEDOUT before \a abstime has passed. The same interpretation applies to
    \c pthread_rwlock_timedrdlock() and \c pthread_rwlock_timedwrlock().

    \par pthread_rwlock_unlock() disambiguation
    POSIX uses a single \c pthread_rwlock_unlock() call to release either a read or a
    write hold, but the underlying \c stk::sync::RWMutex exposes separate
    \c ReadUnlock() / \c Unlock() calls. This shim
    disambiguates by recording, inside the \c pthread_rwlock_t itself, whether the
    lock is currently held for writing; this is safe because a write hold is always
    exclusive (no reader can be active while it is set), so the flag unambiguously
    identifies which underlying call to make. As with real pthreads, calling
    \c pthread_rwlock_unlock() on a lock you do not hold is undefined behavior and is
    not detected here.

    \par pthread_spin_* note
    Spinlocks busy-wait and are not recursive: relocking from the owning thread
    deadlocks. Only \c PTHREAD_PROCESS_PRIVATE is supported for the \a pshared argument
    to \c pthread_spin_init(); \c PTHREAD_PROCESS_SHARED returns \c ENOTSUP (STK has no
    cross-process concept).

    \par pthread_barrier_wait() note
    Like the underlying \c stk::sync::Barrier::Wait(), \c pthread_barrier_wait() is
    ISR-unsafe - do not call it from interrupt context. It returns
    \c PTHREAD_BARRIER_SERIAL_THREAD to exactly one arbitrary calling thread per
    round (the one whose arrival trips the barrier) and \c 0 to the rest, matching
    POSIX.

    \par pthread_once() note
    Each \c pthread_once_t lazily creates its own guard mutex (on first use; it is
    embedded in the \c pthread_once_t and never explicitly destroyed) and
    blocks concurrent callers on it while \a init_routine runs, so - unlike a naive
    flag check - a thread that calls \c pthread_once() while another thread's
    \a init_routine is still running correctly waits for it to finish rather than
    racing ahead. \a init_routine must not itself call \c pthread_once() on the same
    \c pthread_once_t (self-deadlock, as in real POSIX).

    \par pthread_key_t / thread-specific data
    Per-thread values set via \c pthread_setspecific() are stored in the thread's
    control block (reached through STK's per-task TLS slot). Foreign tasks are adopted on
    first \c pthread_setspecific() as described above (\c ENOMEM if no slot is left).
    Destructors (if any) run when a thread finishes (natural return or \c pthread_exit()),
    for at most \c PTHREAD_DESTRUCTOR_ITERATIONS passes - matching POSIX, a
    destructor that calls \c pthread_setspecific() to set a new non-NULL value for
    its own key causes another pass. They do not run for an adopted task that simply
    never exits. \c STK_C_PTHREAD_KEYS_MAX (this file) bounds how many keys can exist
    concurrently.

    \defgroup c_api_pthread STK C pthread-compatible API
    \brief    Minimal POSIX-threads-style interface for SuperTinyKernel RTOS.
    @{
*/

#ifdef __cplusplus
extern "C" {
#endif

/*! \typedef   pthread_stk_word_t
    \brief     Native machine word used for stacks and opaque object storage (same as stk::Word).
*/
typedef uintptr_t pthread_stk_word_t;

/*! \def       __stk_pthread_aligned
    \brief     Alignment attribute of the opaque object storage (one machine word).
*/
#if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__)
    #define __stk_pthread_aligned __attribute__((aligned(sizeof(pthread_stk_word_t))))
#else
    #define __stk_pthread_aligned
#endif

/*! \brief     Sizes (in pthread_stk_word_t units) of the opaque storage for the C++ sync objects.
    \note      Verified at compile time against sizeof() of the real objects by stk_c_pthread.cpp.
*/
#define STK_C_PTHREAD_MUTEX_WORDS   (10U + (STK_SYNC_DEBUG_NAMES ? 1U : 0U))
#define STK_C_PTHREAD_CV_WORDS      (7U + (STK_SYNC_DEBUG_NAMES ? 1U : 0U))
#define STK_C_PTHREAD_RWMUTEX_WORDS (17U + (STK_SYNC_DEBUG_NAMES ? 3U : 0U))
#define STK_C_PTHREAD_BARRIER_WORDS (STK_C_PTHREAD_MUTEX_WORDS + STK_C_PTHREAD_CV_WORDS + 4U + (STK_SYNC_DEBUG_NAMES ? 1U : 0U))

typedef struct pthread_stk_mutex_mem_t   { pthread_stk_word_t data[STK_C_PTHREAD_MUTEX_WORDS]   __stk_pthread_aligned; } pthread_stk_mutex_mem_t;
typedef struct pthread_stk_cv_mem_t      { pthread_stk_word_t data[STK_C_PTHREAD_CV_WORDS]      __stk_pthread_aligned; } pthread_stk_cv_mem_t;
typedef struct pthread_stk_rwmutex_mem_t { pthread_stk_word_t data[STK_C_PTHREAD_RWMUTEX_WORDS] __stk_pthread_aligned; } pthread_stk_rwmutex_mem_t;
typedef struct pthread_stk_barrier_mem_t { pthread_stk_word_t data[STK_C_PTHREAD_BARRIER_WORDS] __stk_pthread_aligned; } pthread_stk_barrier_mem_t;

/*! \def       STK_C_PTHREAD_VERSION
    \brief     Interface version.
*/
#define STK_C_PTHREAD_VERSION (0x20261004)

// =============================================================================
// Configuration macros (can be overridden before including this file)
// =============================================================================

/*! \def       STK_C_PTHREAD_MAX_THREADS
    \brief     Maximum number of concurrently-alive pthread_t's (default: 8).
    \note      A thread stays "alive" from pthread_create() until its resources are
               reclaimed (by pthread_join(), or by the internal reaper for detached
               threads once they finish). Tasks adopted via pthread_self() /
               pthread_setspecific() count against this limit permanently.
*/
#ifndef STK_C_PTHREAD_MAX_THREADS
    #define STK_C_PTHREAD_MAX_THREADS (8U)
#endif

/*! \def       STK_C_PTHREAD_DEFAULT_STACK_WORDS
    \brief     Default per-thread stack size in pthread_stk_word_t units, used when
               pthread_attr_t does not specify a stack size or an external stack
               (default: 1024 words).
    \note      Threads created with this exact size are served from a static
               BlockMemoryPool (STK_C_PTHREAD_MAX_THREADS blocks, zero heap use).
               Any other requested size falls back to malloc().
*/
#ifndef STK_C_PTHREAD_DEFAULT_STACK_WORDS
    #define STK_C_PTHREAD_DEFAULT_STACK_WORDS (1024U)
#endif

/*! \def       STK_C_PTHREAD_REAPER_STACK_WORDS
    \brief     Stack size (in pthread_stk_word_t units) for the internal reaper task that
               reclaims detached-thread resources (default: 256 words).
*/
#ifndef STK_C_PTHREAD_REAPER_STACK_WORDS
    #define STK_C_PTHREAD_REAPER_STACK_WORDS (256U)
#endif

/*! \def       STK_C_PTHREAD_KEYS_MAX
    \brief     Maximum number of concurrently-alive pthread_key_t's (default: 8).
    \note      Also exposed under the POSIX-standard name \c PTHREAD_KEYS_MAX.
*/
#ifndef STK_C_PTHREAD_KEYS_MAX
    #define STK_C_PTHREAD_KEYS_MAX (8U)
#endif

// =============================================================================
// Bootstrap (STK-specific, not part of POSIX)
// =============================================================================

/*! \brief     Bind the STK kernel instance that pthread_create() will add new
               threads to.
    \details   Must be called exactly once, before the first pthread_create() call.
               The kernel must already be created and initialized (IKernel::Initialize()
               or stk_kernel_init()); it does not need to be started yet, but in practice
               pthread_create() will only usefully run new threads once the kernel has been
               started.
    \note      Also adds the internal reaper task to the kernel, so
               STK_C_KERNEL_MAX_TASKS must have room for it. Binding a second,
               different kernel is not supported (the reaper stays with the first).
    \param[in] kernel: Pointer to the kernel object, an stk::IKernel (from C++ pass \c static_cast<stk::IKernel *>(&kernel),
               from the C API pass the stk_kernel_t* returned by stk_kernel_create(), it is the
               same object). Typed \c void * to keep this header independent of stk_c.h.
               The kernel type must include KERNEL_DYNAMIC (threads need to be able to
               finish/return) and KERNEL_SYNC.
*/
void stk_pthread_bind_kernel(void *kernel);

// =============================================================================
// Types
// =============================================================================

/*! \brief     Opaque thread handle.
*/
typedef struct pthread_stk_ctrl_t *pthread_t;

/*! \def       STK_C_PTHREAD_NAME_MAX
    \brief     Capacity of a thread name buffer including the terminating NUL (like Linux, 16).
*/
#ifndef STK_C_PTHREAD_NAME_MAX
    #define STK_C_PTHREAD_NAME_MAX (16U)
#endif

/*! \def       STK_C_PTHREAD_RUNTIME_PRIORITY
    \brief     1 (default): pthread_setschedparam() / pthread_setschedprio() change the priority of a running thread with
               stk::SetWeight(), which requires a kernel switch strategy with WEIGHT_API = 1 and
               PRIORITY_INHERITANCE_API = 1 (SwitchStrategyFixedPriority). Set it to 0 for other strategies: the
               functions then return ENOTSUP for any change (an unchanged value still succeeds).
*/
#ifndef STK_C_PTHREAD_RUNTIME_PRIORITY
    #define STK_C_PTHREAD_RUNTIME_PRIORITY (1)
#endif

#ifndef CLOCK_REALTIME
    #define CLOCK_REALTIME  (0)
#endif
#ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC (1)
#endif

/*! \def       PTHREAD_CANCEL_ENABLE / PTHREAD_CANCEL_DISABLE / PTHREAD_CANCEL_DEFERRED / PTHREAD_CANCEL_ASYNCHRONOUS
    \brief     pthread_setcancelstate() / pthread_setcanceltype() values. Only deferred cancellation exists.
*/
#define PTHREAD_CANCEL_ENABLE       (0)
#define PTHREAD_CANCEL_DISABLE      (1)
#define PTHREAD_CANCEL_DEFERRED     (0)
#define PTHREAD_CANCEL_ASYNCHRONOUS (1)

/*! \def       PTHREAD_CANCELED
    \brief     Value returned by pthread_join() for a cancelled thread.
*/
#define PTHREAD_CANCELED ((void *)(intptr_t)-1)

/*! \def       STK_C_PTHREAD_CLEANUP_MAX
    \brief     Maximum nesting depth of pthread_cleanup_push() per thread.
*/
#ifndef STK_C_PTHREAD_CLEANUP_MAX
    #define STK_C_PTHREAD_CLEANUP_MAX (8U)
#endif

/*! \def       PTHREAD_INHERIT_SCHED / PTHREAD_EXPLICIT_SCHED
    \brief     pthread_attr_setinheritsched() values: take the priority from the creating thread, or from the attributes.
*/
#define PTHREAD_INHERIT_SCHED  (0)
#define PTHREAD_EXPLICIT_SCHED (1)

/*! \def       PTHREAD_SCOPE_SYSTEM / PTHREAD_SCOPE_PROCESS
    \brief     pthread_attr_setscope() values. Only PTHREAD_SCOPE_SYSTEM exists on STK (all tasks compete in the kernel).
*/
#define PTHREAD_SCOPE_SYSTEM  (0)
#define PTHREAD_SCOPE_PROCESS (1)

/*! \def       PTHREAD_PRIO_NONE / PTHREAD_PRIO_INHERIT / PTHREAD_PRIO_PROTECT
    \brief     pthread_mutexattr_setprotocol() values. PTHREAD_PRIO_PROTECT (priority ceiling) is not supported.
*/
#define PTHREAD_PRIO_NONE    (0)
#define PTHREAD_PRIO_INHERIT (1)
#define PTHREAD_PRIO_PROTECT (2)

/*! \brief     Thread creation attributes.
    \note      Fields are considered private; use the pthread_attr_* accessors.
*/
typedef struct pthread_attr_t
{
    size_t      __stack_bytes;  //!< 0 = use STK_C_PTHREAD_DEFAULT_STACK_WORDS.
    pthread_stk_word_t *__ext_stack;    //!< Caller-supplied stack, or NULL.
    int         __detachstate;  //!< PTHREAD_CREATE_JOINABLE / _DETACHED.
    int         __privileged;   //!< PTHREAD_CREATE_USER_NP (default) / _PRIVILEGED_NP.
    int         __policy;       //!< SCHED_OTHER (default) / SCHED_FIFO / SCHED_RR (informational).
    int         __priority;     //!< Task weight (sched_priority), used if __inheritsched is PTHREAD_EXPLICIT_SCHED.
    int         __inheritsched; //!< PTHREAD_EXPLICIT_SCHED (default) / PTHREAD_INHERIT_SCHED.
    int         __scope;        //!< Always PTHREAD_SCOPE_SYSTEM.
    size_t      __guardsize;    //!< Stored for pthread_attr_getguardsize(), has no effect.
    char        __name[STK_C_PTHREAD_NAME_MAX]; //!< Thread name, "" = default ("pthread").
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE (0)
#define PTHREAD_CREATE_DETACHED (1)

/*! \def       PTHREAD_CREATE_USER_NP / PTHREAD_CREATE_PRIVILEGED_NP
    \brief     Non-portable STK access modes for pthread_attr_setprivileged_np(); map to
               stk_task_create_user() / stk_task_create_privileged() respectively.
*/
#define PTHREAD_CREATE_USER_NP       (0)
#define PTHREAD_CREATE_PRIVILEGED_NP (1)

/*! \brief     Mutex attributes.
    \note      PTHREAD_MUTEX_NORMAL / DEFAULT, PTHREAD_MUTEX_RECURSIVE and
               PTHREAD_MUTEX_ERRORCHECK are supported. The underlying
               stk::sync::Mutex is always recursive, so PTHREAD_MUTEX_NORMAL behaves
               like PTHREAD_MUTEX_RECURSIVE (POSIX leaves relocking a NORMAL mutex
               undefined, so this is conforming). Behavior per type:
               - NORMAL/DEFAULT, RECURSIVE: relocking by the owner succeeds and
                 must be balanced by the same number of unlocks (maximum depth
                 stk::sync::Mutex::RECURSION_MAX, then EAGAIN).
               - ERRORCHECK: relocking by the owner returns EDEADLK (trylock: EBUSY).
               - all types: unlocking a mutex not owned by the caller returns EPERM.
*/
typedef struct pthread_mutexattr_t
{
    int __type;     //!< PTHREAD_MUTEX_NORMAL / DEFAULT / RECURSIVE / ERRORCHECK.
    int __protocol; //!< PTHREAD_PRIO_NONE (default) / PTHREAD_PRIO_INHERIT.
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_NORMAL     (0)
#define PTHREAD_MUTEX_DEFAULT    (PTHREAD_MUTEX_NORMAL)
#define PTHREAD_MUTEX_ERRORCHECK (1)
#define PTHREAD_MUTEX_RECURSIVE  (2)

/*! \def       STK_C_PTHREAD_ZERO_INIT
    \brief     All-zero aggregate initializer of the static initializers below. C++ uses \c {} (value-initialization):
               \c {0} initializes only the first member and makes -Wmissing-field-initializers warn there, while in C
               \c {0} is accepted by the compilers without a warning.
*/
#ifdef __cplusplus
    #define STK_C_PTHREAD_ZERO_INIT {}
#else
    #define STK_C_PTHREAD_ZERO_INIT {0}
#endif

/*! \brief     A pthread mutex.
    \note      Fields are considered private. May be statically initialized with
               PTHREAD_MUTEX_INITIALIZER, in which case the underlying STK mutex is
               created lazily on first use.
*/
typedef struct pthread_mutex_t
{
    pthread_stk_mutex_mem_t __mem;    //!< In-place storage for the STK mutex object.
    void           *__handle; //!< Object constructed in __mem, or NULL if not yet constructed.
    int             __type;   //!< PTHREAD_MUTEX_* type; 0 (NORMAL) when statically initialized.
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER STK_C_PTHREAD_ZERO_INIT

/*! \brief     Condition variable attributes (currently no settable properties).
*/
typedef struct pthread_condattr_t
{
    int __clock; //!< CLOCK_REALTIME (default) / CLOCK_MONOTONIC: both are the same STK tick timeline.
} pthread_condattr_t;

/*! \brief     A pthread condition variable.
    \note      Fields are considered private. May be statically initialized with
               PTHREAD_COND_INITIALIZER, in which case the underlying STK condition
               variable is created lazily on first use.
*/
typedef struct pthread_cond_t
{
    pthread_stk_cv_mem_t __mem;    //!< In-place storage for the STK condition variable object.
    void        *__handle; //!< Object constructed in __mem, or NULL if not yet constructed.
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER STK_C_PTHREAD_ZERO_INIT

/*! \brief     Read-write lock attributes (currently no settable properties).
    \note      In particular there is no reader/writer preference setting; the
               underlying stk_rwmutex_t always applies a writer-preference policy
               (see RWMutex::TimedReadLock()).
*/
typedef struct pthread_rwlockattr_t
{
    int __reserved; //!< Unused (no settable properties); reserved for future use.
} pthread_rwlockattr_t;

/*! \brief     A pthread read-write lock.
    \note      Fields are considered private. May be statically initialized with
               PTHREAD_RWLOCK_INITIALIZER, in which case the underlying STK rwmutex
               is created lazily on first use.
    \see       "pthread_rwlock_unlock() disambiguation" in the file-level docs
               regarding the __wrlocked field.
*/
typedef struct pthread_rwlock_t
{
    pthread_stk_rwmutex_mem_t __mem;      //!< In-place storage for the STK rwmutex object.
    void             *__handle;   //!< Object constructed in __mem, or NULL if not yet constructed.
    volatile bool     __wrlocked; //!< True while held for writing; selects Unlock() vs ReadUnlock() on release.
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER STK_C_PTHREAD_ZERO_INIT

#define PTHREAD_PROCESS_PRIVATE (0)
#define PTHREAD_PROCESS_SHARED  (1)

/*! \brief     A pthread spinlock.
    \note      Fields are considered private. Unlike the other pthread objects in
               this shim, spinlocks have no static-initializer macro (matching real
               POSIX) - pthread_spin_init() must be called before use.
    \see       "pthread_spin_* note" in the file-level docs.
*/
typedef struct pthread_spinlock_t
{
    struct {
        pthread_stk_word_t data[1] __stk_pthread_aligned; //!< Storage for the non-recursive fast hw::SpinLock.
    } __mem;                                //!< In-place storage for the STK spinlock object.
    void *__handle;                         //!< Object constructed in __mem, or NULL if not yet constructed.
} pthread_spinlock_t;

/*! \brief     Barrier attributes (currently no settable properties).
*/
typedef struct pthread_barrierattr_t
{
    int __reserved; //!< Unused (no settable properties); reserved for future use.
} pthread_barrierattr_t;

/*! \brief     A pthread barrier.
    \note      Fields are considered private. No static-initializer macro (matching
               real POSIX) - pthread_barrier_init() must be called before use, since
               it is where the trip count is supplied.
*/
typedef struct pthread_barrier_t
{
    pthread_stk_barrier_mem_t __mem;    //!< In-place storage for the STK barrier object.
    void             *__handle; //!< Object constructed in __mem, or NULL if not yet constructed.
} pthread_barrier_t;

/*! \def       PTHREAD_BARRIER_SERIAL_THREAD
    \brief     Returned by pthread_barrier_wait() to exactly one arbitrary caller
               per round; all others receive 0. See "pthread_barrier_wait() note" in
               the file-level docs.
*/
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

/*! \brief     A pthread_once() control object.
    \note      Fields are considered private. Statically initialized with
               PTHREAD_ONCE_INIT; the guard mutex used to block concurrent callers
               is created lazily on first use. See "pthread_once() note" in the
               file-level docs.
*/
typedef struct pthread_once_t
{
    int             __state;  //!< 0 = not started, 1 = running, 2 = done.
    pthread_stk_mutex_mem_t __mem;    //!< In-place storage for the guard mutex.
    void           *__handle; //!< Guard mutex constructed in __mem, or NULL if not yet constructed.
} pthread_once_t;

#define PTHREAD_ONCE_INIT STK_C_PTHREAD_ZERO_INIT

/*! \brief     A thread-specific data key.
    \note      An index into an internal key table; there is no analogue to a NULL
               handle, so use pthread_key_create()'s return value to check success.
*/
typedef unsigned int pthread_key_t;

/*! \def       PTHREAD_KEYS_MAX
    \brief     POSIX-standard name for STK_C_PTHREAD_KEYS_MAX (this file).
*/
#define PTHREAD_KEYS_MAX (STK_C_PTHREAD_KEYS_MAX)

/*! \def       PTHREAD_DESTRUCTOR_ITERATIONS
    \brief     Maximum number of passes over a finishing thread's keys made while
               destructors keep setting new non-NULL values for their own key. See
               "pthread_key_t / thread-specific data" in the file-level
               docs.
*/
#define PTHREAD_DESTRUCTOR_ITERATIONS (4)

// =============================================================================
// Thread attributes
// =============================================================================

/*! \brief     Initialize a thread attributes object with default values
               (default stack size, no external stack, joinable).
    \param[out] attr: Attributes object to initialize.
*/
int pthread_attr_init(pthread_attr_t *attr);

/*! \brief     Destroy a thread attributes object (no-op; no owned resources).
    \param[in] attr: Attributes object to destroy.
*/
int pthread_attr_destroy(pthread_attr_t *attr);

/*! \brief     Set the requested stack size in bytes.
    \param[in,out] attr: Attributes object to modify.
    \param[in] stacksize: Requested stack size in bytes. Must not be 0.
    \note      Only exactly STK_C_PTHREAD_DEFAULT_STACK_WORDS * sizeof(pthread_stk_word_t)
               bytes is served from the static pool; any other value falls back to
               malloc() at pthread_create() time.
*/
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);

/*! \brief     Get the stack size in bytes that threads created with \a attr will use.
    \details   If no size was set, reports STK_C_PTHREAD_DEFAULT_STACK_WORDS *
               sizeof(pthread_stk_word_t).
    \param[in] attr: Attributes object to query.
    \param[out] stacksize: Receives the stack size in bytes (the default size if none
               was set).
*/
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);

/*! \brief     Supply an external, caller-owned stack buffer for the thread.
    \details   The buffer is never freed by this shim; the caller must keep it
               valid for the entire lifetime of the thread.
    \param[in,out] attr: Attributes object to modify.
    \param[in] stackaddr: Pointer to a pthread_stk_word_t-aligned buffer.
    \param[in] stacksize: Size of the buffer in bytes.
*/
int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize);

/*! \brief     Get the previously-set external stack (\a stackaddr is NULL if none set).
    \param[in] attr: Attributes object to query.
    \param[out] stackaddr: Receives the external stack address, or NULL if none was
                set.
    \param[out] stacksize: Receives the configured stack size in bytes (0 if no size
                was set).
*/
int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, size_t *stacksize);

/*! \brief     Set PTHREAD_CREATE_JOINABLE or PTHREAD_CREATE_DETACHED.
    \param[in,out] attr: Attributes object to modify.
    \param[in] detachstate: PTHREAD_CREATE_JOINABLE or PTHREAD_CREATE_DETACHED.
*/
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);

/*! \brief     Get the current detach-state setting.
    \param[in] attr: Attributes object to query.
    \param[out] detachstate: Receives PTHREAD_CREATE_JOINABLE or
               PTHREAD_CREATE_DETACHED.
*/
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);

/*! \brief     Non-portable: select the STK access mode of threads created with \a attr.
    \details   PTHREAD_CREATE_USER_NP (default) creates the thread via stk_task_create_user();
               PTHREAD_CREATE_PRIVILEGED_NP via stk_task_create_privileged() (needed e.g. for
               direct GPIO/peripheral access on some MCUs).
    \param[in,out] attr: Attributes object to modify.
    \param[in] mode: PTHREAD_CREATE_USER_NP or PTHREAD_CREATE_PRIVILEGED_NP.
    \return    0 on success, EINVAL on NULL attr or unknown mode.
*/
int pthread_attr_setprivileged_np(pthread_attr_t *attr, int mode);

/*! \brief     Non-portable: get the access mode set in \a attr.
    \param[in] attr: Attributes object to query.
    \param[out] mode: Receives PTHREAD_CREATE_USER_NP or PTHREAD_CREATE_PRIVILEGED_NP.
*/
int pthread_attr_getprivileged_np(const pthread_attr_t *attr, int *mode);

// =============================================================================
// Thread lifecycle
// =============================================================================

/*! \brief     Create and start a new thread.
    \param[out] thread: Receives the new thread's handle on success.
    \param[in] attr: Optional attributes, or NULL for defaults.
    \param[in] start_routine: Thread entry function.
    \param[in] arg: Argument passed to start_routine.
    \return    0 on success, or an errno-style error code:
               EINVAL  - no kernel bound (see stk_pthread_bind_kernel()), or the kernel
                         rejected the task (invalid state / duplicate): not retryable.
               EAGAIN  - STK_C_PTHREAD_MAX_THREADS slots exhausted, stack allocation
                         failed, or the kernel has no free task slot (retryable).
    \note      Requires a bound kernel whose type includes KERNEL_DYNAMIC and KERNEL_SYNC
               (see the file-level docs).
*/
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*start_routine)(void *), void *arg);

/*! \brief     Block until the given joinable thread finishes, then reclaim its
               resources.
    \param[in] thread: Thread to join.
    \param[out] retval: Optional; receives the thread's return value / the value
               passed to pthread_exit().
    \return    0 on success, EINVAL if \a thread is NULL, already joined,
               detached or adopted, EDEADLK if \a thread is the calling thread.
*/
int pthread_join(pthread_t thread, void **retval);

/*! \brief     Mark a thread as detached.
    \details   A detached thread's resources are reclaimed automatically (by an
               internal reaper task) once it finishes; it must not be joined.
    \param[in] thread: Thread to detach, as returned by pthread_create().
    \return    0 on success, EINVAL if \a thread is NULL, already joined, or
               already detached.
*/
int pthread_detach(pthread_t thread);

/*! \brief     Terminate the calling thread.
    \param[in] retval: Value made available to a joiner via pthread_join(), or
               discarded if the thread is detached.
    \note      Does not return. For an adopted task (see "pthread_self() and foreign
               tasks") only the key destructors run and the task then parks forever.
*/
void pthread_exit(void *retval);

/*! \brief     Return the calling thread's own handle.
    \return    The calling thread's handle. A task not created through this shim
               (e.g. the bootstrap task) is adopted on first call; NULL only if no
               pthread slot is left to adopt into (see the file-level note).
*/
pthread_t pthread_self(void);

/*! \brief     Compare two thread handles for equality.
    \param[in] t1: First thread handle.
    \param[in] t2: Second thread handle.
*/
int pthread_equal(pthread_t t1, pthread_t t2);

/*! \brief     Voluntarily give up the CPU to another ready task (cooperative
               yield), then resume once rescheduled.
    \return    Always 0 (matches the non-standard GNU pthread_yield() signature;
               the underlying stk_yield() cannot fail).
    \note      Non-standard - not in POSIX (POSIX uses \c sched_yield()), but
               widely available as a GNU/BSD extension under this name.
*/
int pthread_yield(void);

// =============================================================================
// Mutex
// =============================================================================

/*! \brief     Initialize a mutex attributes object with default values
               (PTHREAD_MUTEX_DEFAULT).
    \param[out] attr: Attributes object to initialize.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_mutexattr_init(pthread_mutexattr_t *attr);

/*! \brief     Destroy a mutex attributes object (no-op; no owned resources).
    \param[in] attr: Attributes object to destroy.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);

/*! \brief     Set the mutex type.
    \param[in,out] attr: Attributes object to modify.
    \param[in] type: PTHREAD_MUTEX_NORMAL / DEFAULT / RECURSIVE / ERRORCHECK.
    \return    0 on success, EINVAL if \a attr is NULL or \a type is not one of the
               supported values.
*/
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

/*! \brief     Get the mutex type stored in \a attr.
    \param[in] attr: Attributes object to query.
    \param[out] type: Receives the mutex type.
    \return    0 on success, EINVAL if \a attr or \a type is NULL.
*/
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);

/*! \brief     Initialize a mutex.
    \param[out] mutex: Mutex to initialize (storage is embedded in the object, nothing
                is allocated).
    \param[in] attr: Optional, or NULL for defaults (PTHREAD_MUTEX_DEFAULT).
    \return    0 on success, EINVAL if \a mutex is NULL or \a attr holds an invalid
               type (the mutex is left uninitialized).
*/
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);

/*! \brief     Destroy a mutex, releasing the underlying STK mutex object.
    \details   Safe to call on a mutex that was statically initialized with
               PTHREAD_MUTEX_INITIALIZER but never used (nothing to destroy). The
               mutex must not be locked and no thread may be blocked on it;
               destroying a mutex in use is undefined behavior. After destruction
               the object may be re-initialized with pthread_mutex_init().
    \param[in,out] mutex: Mutex to destroy.
    \return    0 on success, EINVAL if \a mutex is NULL.
*/
int pthread_mutex_destroy(pthread_mutex_t *mutex);

/*! \brief     Lock a mutex, blocking until it becomes available.
    \details   The underlying STK mutex is created lazily on first use for a mutex
               statically initialized with PTHREAD_MUTEX_INITIALIZER. Blocking
               puts the calling task to sleep so other tasks can run. For
               NORMAL and RECURSIVE types relocking from the owning thread succeeds
               and must be balanced by the same number of unlocks (the underlying
               STK mutex is recursive). For ERRORCHECK relocking from the owner
               returns EDEADLK. Must not be called from ISR context.
    \param[in,out] mutex: Mutex to lock.
    \return    0 on success, EDEADLK (ERRORCHECK, already owned by the caller),
               EAGAIN (maximum recursion depth exceeded), EINVAL if \a mutex is NULL.
*/
int pthread_mutex_lock(pthread_mutex_t *mutex);

/*! \brief     Try to lock a mutex without blocking.
    \details   For NORMAL and RECURSIVE types succeeds if the caller already owns
               the mutex (nesting depth is incremented). For ERRORCHECK returns EBUSY.
    \param[in,out] mutex: Mutex to try to lock.
    \return    0 on success, EBUSY if the mutex is currently locked (or, for
               ERRORCHECK, owned by the caller), EAGAIN (maximum recursion depth
               exceeded), EINVAL if \a mutex is NULL.
*/
int pthread_mutex_trylock(pthread_mutex_t *mutex);

/*! \brief     Unlock a mutex held by the calling thread.
    \details   Unlike pthread_mutex_lock(), never creates the underlying STK mutex:
               unlocking a statically initialized mutex that has never been locked
               is an error. As with real pthreads, unlocking a mutex the caller
               does not own is undefined behavior and is not detected here.
    \param[in,out] mutex: Mutex to unlock; must be locked by the calling thread.
    \return    0 on success, EPERM if the caller does not own the mutex, EINVAL if
               \a mutex is NULL or was never initialized/used.
*/
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/*! \brief     Lock with an absolute deadline.
    \details   If \a abstime is already in the past the call degenerates into a
               non-blocking attempt. Timeouts are rounded up to STK kernel ticks.
    \param[in,out] mutex: Mutex to lock.
    \param[in] abstime: Absolute deadline on the stk_time_now_ms() timeline (not
               CLOCK_REALTIME); see the file-level docs.
    \return    0 on success, ETIMEDOUT if the deadline passed first, EDEADLK
               (ERRORCHECK, already owned by the caller), EAGAIN (maximum recursion
               depth exceeded), EINVAL if \a mutex or \a abstime is NULL.
    \see       "pthread_cond_timedwait() / pthread_mutex_timedlock() limitation"
               in the file-level docs regarding the meaning of \a abstime.
*/
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime);

// =============================================================================
// Condition variable
// =============================================================================

/*! \brief     Initialize a condition variable attributes object (no settable
               properties).
    \param[out] attr: Attributes object to initialize.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_condattr_init(pthread_condattr_t *attr);

/*! \brief     Destroy a condition variable attributes object (no-op).
    \param[in] attr: Attributes object to destroy.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_condattr_destroy(pthread_condattr_t *attr);

/*! \brief     Initialize a condition variable.
    \param[out] cond: Condition variable to initialize.
    \param[in] attr: Optional, or NULL for defaults (no settable properties; the
               argument is ignored).
    \return    0 on success, EINVAL if \a cond is NULL.
*/
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);

/*! \brief     Destroy a condition variable, releasing the underlying STK object.
    \details   Safe on a never-used PTHREAD_COND_INITIALIZER object. No thread may
               be waiting on it; destroying a condition variable in use is
               undefined behavior.
    \param[in,out] cond: Condition variable to destroy.
    \return    0 on success, EINVAL if \a cond is NULL.
*/
int pthread_cond_destroy(pthread_cond_t *cond);

/*! \brief     Atomically unlock \a mutex and wait for a signal; re-locks \a mutex
               before returning.
    \details   Waits without a timeout. As in POSIX, spurious wakeups are possible,
               so always re-check the predicate in a loop. \a mutex must be locked
               by the caller and must have been used or initialized before (its
               underlying STK mutex must exist). Must not be called from ISR context.
    \param[in,out] cond: Condition variable to wait on.
    \param[in,out] mutex: Mutex protecting the predicate; must be locked by the
               caller. It is released while waiting and re-locked before
               returning.
    \note      \a mutex must be held exactly once. Waiting on a NORMAL/RECURSIVE mutex
               locked more than once is undefined (as in POSIX): only one level is
               released, so other threads cannot acquire it.
    \return    0 on success, EPERM (ERRORCHECK, \a mutex not owned by the caller),
               EINVAL if \a cond or \a mutex is NULL, or \a mutex has never been
               initialized/locked.
*/
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);

/*! \brief     As pthread_cond_wait(), with an absolute deadline.
    \details   \a mutex is re-locked before returning in all cases, including on
               timeout.
    \param[in,out] cond: Condition variable to wait on.
    \param[in,out] mutex: Mutex protecting the predicate; must be locked by the
               caller. It is released while waiting and re-locked before
               returning.
    \param[in] abstime: Absolute deadline on the stk_time_now_ms() timeline (not
               CLOCK_REALTIME); see the file-level docs.
    \return    0 on success, ETIMEDOUT if the deadline passed first, EINVAL if
               \a cond, \a mutex or \a abstime is NULL, or \a mutex has never been
               initialized/locked.
    \see       "pthread_cond_timedwait() / pthread_mutex_timedlock() limitation"
               in the file-level docs regarding the meaning of \a abstime.
*/
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime);

/*! \brief     Wake one thread waiting on \a cond (no effect if none is waiting).
    \details   The condition variable is created lazily if needed. Signals are not
               queued: a signal sent while nobody waits is lost.
    \param[in,out] cond: Condition variable to signal.
    \return    0 on success, EINVAL if \a cond is NULL.
*/
int pthread_cond_signal(pthread_cond_t *cond);

/*! \brief     Wake all threads waiting on \a cond (no effect if none is waiting).
    \param[in,out] cond: Condition variable to broadcast.
    \return    0 on success, EINVAL if \a cond is NULL.
*/
int pthread_cond_broadcast(pthread_cond_t *cond);

// =============================================================================
// Read-write lock
// =============================================================================

/*! \brief     Initialize a read-write lock attributes object (no settable
               properties).
    \param[out] attr: Attributes object to initialize.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_rwlockattr_init(pthread_rwlockattr_t *attr);

/*! \brief     Destroy a read-write lock attributes object (no-op).
    \param[in] attr: Attributes object to destroy.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr);

/*! \brief     Initialize a read-write lock.
    \param[out] rwlock: Read-write lock to initialize.
    \param[in] attr: Optional, or NULL for defaults (no settable properties).
    \return    0 on success, EINVAL if \a rwlock is NULL.
*/
int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);

/*! \brief     Destroy a read-write lock, releasing the underlying STK object.
    \details   Safe on a never-used PTHREAD_RWLOCK_INITIALIZER object. The lock must
               not be held and no thread may be blocked on it.
    \param[in,out] rwlock: Read-write lock to destroy.
    \return    0 on success, EINVAL if \a rwlock is NULL.
*/
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);

/*! \brief     Acquire the lock for shared reading. Blocks until available.
    \details   Blocks if a writer is currently active or writers are waiting
               (writer-preference policy; see RWMutex::TimedReadLock()).
    \param[in,out] rwlock: Read-write lock to acquire for reading.
*/
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);

/*! \brief     Try to acquire the read lock without blocking.
    \param[in,out] rwlock: Read-write lock to try to acquire for reading.
    \return    0 on success, EBUSY if a writer is active or waiting.
*/
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);

/*! \brief     Acquire the read lock with an absolute deadline.
    \param[in,out] rwlock: Read-write lock to acquire for reading.
    \param[in] abstime: Absolute deadline on the stk_time_now_ms() timeline (not
               CLOCK_REALTIME); see the file-level docs.
    \return    0 on success, ETIMEDOUT if the deadline passed first.
    \see       "pthread_cond_timedwait() / pthread_mutex_timedlock() limitation"
               in the file-level docs regarding the meaning of \a abstime.
*/
int pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock, const struct timespec *abstime);

/*! \brief     Acquire the lock for exclusive writing. Blocks until available.
    \details   Blocks until all active readers have released their locks and no
               other writer is active.
    \param[in,out] rwlock: Read-write lock to acquire for writing.
*/
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);

/*! \brief     Try to acquire the write lock without blocking.
    \param[in,out] rwlock: Read-write lock to try to acquire for writing.
    \return    0 on success, EBUSY otherwise.
*/
int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);

/*! \brief     Acquire the write lock with an absolute deadline.
    \param[in,out] rwlock: Read-write lock to acquire for writing.
    \param[in] abstime: Absolute deadline on the stk_time_now_ms() timeline (not
               CLOCK_REALTIME); see the file-level docs.
    \return    0 on success, ETIMEDOUT if the deadline passed first.
    \see       "pthread_cond_timedwait() / pthread_mutex_timedlock() limitation"
               in the file-level docs regarding the meaning of \a abstime.
*/
int pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock, const struct timespec *abstime);

/*! \brief     Release a read or write hold, whichever the calling thread holds.
    \param[in,out] rwlock: Read-write lock to release; must be held by the calling
               thread (for reading or writing).
    \see       "pthread_rwlock_unlock() disambiguation" in the file-level docs.
*/
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

// =============================================================================
// Spin lock
// =============================================================================

/*! \brief     Initialize a spinlock.
    \param[out] lock: Spinlock to initialize.
    \param[in] pshared: Must be PTHREAD_PROCESS_PRIVATE.
    \return    0 on success, ENOTSUP if pshared is PTHREAD_PROCESS_SHARED, EAGAIN if
               the underlying STK spinlock could not be created.
*/
int pthread_spin_init(pthread_spinlock_t *lock, int pshared);

/*! \brief     Destroy a spinlock.
    \details   The spinlock must not be held. After destruction it must be
               re-initialized with pthread_spin_init() before further use.
    \param[in,out] lock: Spinlock to destroy.
    \return    0 on success, EINVAL if \a lock is NULL.
*/
int pthread_spin_destroy(pthread_spinlock_t *lock);

/*! \brief     Acquire the spinlock, spinning until available.
    \details   Busy-waits (the task is not put to sleep), so hold it only for very
               short critical sections. Not recursive: relocking from the owning
               thread deadlocks.
    \param[in,out] lock: Spinlock to acquire.
    \return    0 on success, EINVAL if \a lock is NULL or was never initialized.
    \see       "pthread_spin_* note" in the file-level docs.
*/
int pthread_spin_lock(pthread_spinlock_t *lock);

/*! \brief     Try to acquire the spinlock without blocking.
    \param[in,out] lock: Spinlock to try to acquire.
    \return    0 on success, EBUSY if currently held, EINVAL if \a lock is NULL or
               was never initialized.
*/
int pthread_spin_trylock(pthread_spinlock_t *lock);

/*! \brief     Release a spinlock held by the calling thread.
    \param[in,out] lock: Spinlock to release; must be held by the calling thread.
    \return    0 on success, EINVAL if \a lock is NULL or was never initialized.
*/
int pthread_spin_unlock(pthread_spinlock_t *lock);

// =============================================================================
// Barrier
// =============================================================================

/*! \brief     Initialize a barrier attributes object (no settable properties).
    \param[out] attr: Attributes object to initialize.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_barrierattr_init(pthread_barrierattr_t *attr);

/*! \brief     Destroy a barrier attributes object (no-op).
    \param[in] attr: Attributes object to destroy.
    \return    0 on success, EINVAL if \a attr is NULL.
*/
int pthread_barrierattr_destroy(pthread_barrierattr_t *attr);

/*! \brief     Initialize a barrier for \a count participating threads.
    \param[out] barrier: Barrier to initialize.
    \param[in] attr: Optional, or NULL for defaults (no settable properties).
    \param[in] count: Number of threads that must call pthread_barrier_wait() before
               any of them is released. Must not be 0.
    \return    0 on success, EINVAL if count is 0, EAGAIN if the underlying STK
               barrier could not be created.
*/
int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                          unsigned int count);

/*! \brief     Destroy a barrier, releasing the underlying STK barrier object.
    \details   No thread may be blocked in pthread_barrier_wait() on it. After
               destruction it must be re-initialized with pthread_barrier_init()
               before further use.
    \param[in,out] barrier: Barrier to destroy.
    \return    0 on success, EINVAL if \a barrier is NULL.
*/
int pthread_barrier_destroy(pthread_barrier_t *barrier);

/*! \brief     Block until \a count threads have called this function, then release
               them all together; the barrier resets for reuse.
    \param[in,out] barrier: Barrier to wait on.
    \return    PTHREAD_BARRIER_SERIAL_THREAD to exactly one arbitrary caller, 0 to
               the rest, EINVAL if \a barrier is NULL or was never initialized.
    \see       "pthread_barrier_wait() note" in the file-level docs (ISR-unsafe).
*/
int pthread_barrier_wait(pthread_barrier_t *barrier);

// =============================================================================
// Once
// =============================================================================

/*! \brief     Call \a init_routine exactly once for a given \a once_control, no
               matter how many threads call pthread_once() on it concurrently.
    \param[in,out] once_control: Control object, statically initialized with
               PTHREAD_ONCE_INIT.
    \param[in] init_routine: Function to call exactly once. Must not call
               pthread_once() on the same \a once_control.
    \see       "pthread_once() note" in the file-level docs.
    \return    0 on success, EINVAL if \a once_control or \a init_routine is NULL.
*/
int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

// =============================================================================
// Thread-specific data (keys)
// =============================================================================

/*! \brief     Allocate a new thread-specific data key.
    \param[out] key: Receives the new key on success.
    \param[in] destructor: Optional; called with a thread's non-NULL value for this
               key when that thread finishes. May be NULL.
    \return    0 on success, EINVAL if \a key is NULL, EAGAIN if
               STK_C_PTHREAD_KEYS_MAX slots are exhausted.
*/
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));

/*! \brief     Free a thread-specific data key.
    \details   Does not run destructors and does not clear any thread's stored
               value for this key (matching POSIX); reusing a stale value after
               deletion is undefined behavior, as in real pthreads.
    \param[in] key: Key to delete, as returned by pthread_key_create().
    \return    0 on success, EINVAL if \a key was never created (or already
               deleted).
*/
int pthread_key_delete(pthread_key_t key);

/*! \brief     Set the calling thread's value for \a key.
    \param[in] key: Key to set the calling thread's value for.
    \param[in] value: Value to associate with the key for the calling thread (may be
               NULL).
    \see       "pthread_key_t / thread-specific data" in the file-level docs.
    \return    0 on success, EINVAL if \a key is invalid, ENOMEM if the calling task
               is a foreign task and no pthread slot is left to adopt it into.
*/
int pthread_setspecific(pthread_key_t key, const void *value);

/*! \brief     Get the calling thread's value for \a key.
    \param[in] key: Key to read the calling thread's value for.
    \return    The stored value, or NULL if \a key is invalid or the calling thread
               has not set a value for it (never adopts a foreign task).
*/
void *pthread_getspecific(pthread_key_t key);

// -----------------------------------------------------------------------------
// Scheduling attributes, run-time scheduling and thread names
// -----------------------------------------------------------------------------

/*! \brief     Set the scheduling priority (task weight) of threads created with \a attr.
    \param[in] param: sched_priority in STK_C_SCHED_PRIORITY_MIN..STK_C_SCHED_PRIORITY_MAX (priority level of
               SwitchStrategyFixedPriority, higher = more urgent). It is used if the inherit mode is
               PTHREAD_EXPLICIT_SCHED (default); the default level is STK_C_SCHED_PRIORITY_DEFAULT (PRIORITY_NORMAL).
    \return    0 on success, EINVAL for a NULL argument or an out-of-range priority.
*/
int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param);

/*! \brief     Get the scheduling priority stored in \a attr.
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param);

/*! \brief     Set the scheduling policy stored in \a attr (SCHED_OTHER, SCHED_FIFO or SCHED_RR).
    \note      Informational only: the policy is defined by the kernel switch strategy.
    \return    0 on success, EINVAL for a NULL argument or unknown policy.
*/
int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy);

/*! \brief     Get the scheduling policy stored in \a attr.
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy);

/*! \brief     Select whether a new thread inherits the priority of the creating thread.
    \param[in] inheritsched: PTHREAD_INHERIT_SCHED (copy the weight of the calling thread; a thread which was not
               created by pthread_create() gives the default weight) or PTHREAD_EXPLICIT_SCHED (use the attributes).
    \return    0 on success, EINVAL for a NULL argument or invalid value.
*/
int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched);

/*! \brief     Get the inherit-scheduling mode stored in \a attr.
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inheritsched);

/*! \brief     Set the contention scope. Only PTHREAD_SCOPE_SYSTEM is supported.
    \return    0 on success, ENOTSUP for PTHREAD_SCOPE_PROCESS, EINVAL for a NULL argument or invalid value.
*/
int pthread_attr_setscope(pthread_attr_t *attr, int scope);

/*! \brief     Get the contention scope (always PTHREAD_SCOPE_SYSTEM).
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_attr_getscope(const pthread_attr_t *attr, int *scope);

/*! \brief     Set the guard size. Accepted and stored, but STK has no guard pages: it has no effect.
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize);

/*! \brief     Get the stored guard size (0 by default).
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize);

/*! \brief     Set the name of threads created with \a attr (non-portable, GNU-style).
    \return    0 on success, EINVAL for a NULL argument, ERANGE if the name does not fit STK_C_PTHREAD_NAME_MAX
               (including the NUL).
*/
int pthread_attr_setname_np(pthread_attr_t *attr, const char *name);

/*! \brief     Get the name stored in \a attr.
    \return    0 on success, EINVAL for a NULL argument, ERANGE if \a len is too small.
*/
int pthread_attr_getname_np(const pthread_attr_t *attr, char *name, size_t len);

/*! \brief     Get the policy and priority (task weight) of a thread.
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param);

/*! \brief     Change the policy/priority of a running thread.
    \details   The priority is applied with stk::SetWeight() (kernel critical section inside, task context only) and
               takes effect immediately. If the thread currently inherits a higher priority through a mutex the new
               base priority is used once the inheritance ends. The policy is only remembered.
    \return    0 on success, EINVAL for a NULL argument or an invalid value, ESRCH if the thread is not alive,
               ENOTSUP if STK_C_PTHREAD_RUNTIME_PRIORITY is 0 and the priority would change.
*/
int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param);

/*! \brief     Change the priority of a running thread (the policy is kept). See pthread_setschedparam().
    \return    0, EINVAL, ESRCH or ENOTSUP as pthread_setschedparam().
*/
int pthread_setschedprio(pthread_t thread, int prio);

/*! \brief     Set the name of a thread (shown by debug/trace tools), non-portable GNU-style.
    \return    0 on success, EINVAL for a NULL argument, ERANGE if the name is longer than
               STK_C_PTHREAD_NAME_MAX - 1 characters.
*/
int pthread_setname_np(pthread_t thread, const char *name);

/*! \brief     Get the name of a thread.
    \return    0 on success, EINVAL for a NULL argument, ERANGE if \a len is too small.
*/
int pthread_getname_np(pthread_t thread, char *name, size_t len);

/*! \brief     Set the concurrency level hint. Accepted and ignored (STK always schedules kernel tasks).
    \return    0, or EINVAL for a negative level.
*/
int pthread_setconcurrency(int level);

/*! \brief     Get the concurrency level hint.
    \return    Always 0 (the implementation decides).
*/
int pthread_getconcurrency(void);

/*! \brief     Try to join a thread without blocking (GNU extension).
    \return    0 if the thread has finished (it is joined), EBUSY if it is still running, EINVAL, EDEADLK as for
               pthread_join().
*/
int pthread_tryjoin_np(pthread_t thread, void **retval);

/*! \brief     Join a thread, waiting until the absolute deadline \a abstime (GNU extension).
    \return    0 on success, ETIMEDOUT if the thread did not finish by the deadline (it stays joinable), EINVAL,
               EDEADLK as for pthread_join(). Deadlines follow the rules of pthread_cond_timedwait().
*/
int pthread_timedjoin_np(pthread_t thread, void **retval, const struct timespec *abstime);

// -----------------------------------------------------------------------------
// Synchronization attributes (process-shared / protocol / clock)
// -----------------------------------------------------------------------------

/*! \brief     Set/get the process-shared attribute of mutex, condition variable, rwlock and barrier attributes.
    \details   Only PTHREAD_PROCESS_PRIVATE exists: the setters return ENOTSUP for PTHREAD_PROCESS_SHARED and EINVAL
               for other values, the getters always report PTHREAD_PROCESS_PRIVATE.
    \return    0 on success, ENOTSUP / EINVAL as described.
*/
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *pshared);
int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared);
int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr, int *pshared);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared);
int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr, int *pshared);

/*! \brief     Set the mutex protocol.
    \param[in] protocol: PTHREAD_PRIO_NONE or PTHREAD_PRIO_INHERIT. Priority inheritance is a property of
               stk::sync::Mutex itself (active when the kernel switch strategy supports it), so both values give
               the same mutex; PTHREAD_PRIO_PROTECT (priority ceiling) is not supported.
    \return    0 on success, ENOTSUP for PTHREAD_PRIO_PROTECT, EINVAL for a NULL argument or invalid value.
*/
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr, int protocol);

/*! \brief     Get the mutex protocol stored in \a attr.
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *attr, int *protocol);

/*! \brief     Set the clock used by pthread_cond_timedwait() deadlines.
    \param[in] clock_id: CLOCK_REALTIME or CLOCK_MONOTONIC. STK has a single timeline (stk::GetTimeNowMs()), both
               values select it.
    \return    0 on success, EINVAL for a NULL argument or any other clock.
*/
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);

/*! \brief     Get the clock stored in \a attr.
    \return    0 on success, EINVAL for a NULL argument.
*/
int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock_id);

// -----------------------------------------------------------------------------
// Clock-selecting timed waits (POSIX.1-2024)
// -----------------------------------------------------------------------------

/*! \brief     pthread_mutex_timedlock() with an explicit clock.
    \param[in] clock_id: CLOCK_REALTIME or CLOCK_MONOTONIC (same STK timeline), any other value gives EINVAL.
    \return    As pthread_mutex_timedlock(); EINVAL also for an invalid \a abstime.
*/
int pthread_mutex_clocklock(pthread_mutex_t *mutex, clockid_t clock_id, const struct timespec *abstime);

/*! \brief     pthread_cond_timedwait() with an explicit clock. See pthread_mutex_clocklock() for \a clock_id.
*/
int pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex, clockid_t clock_id,
                           const struct timespec *abstime);

/*! \brief     pthread_rwlock_timedrdlock() with an explicit clock. See pthread_mutex_clocklock() for \a clock_id.
*/
int pthread_rwlock_clockrdlock(pthread_rwlock_t *rwlock, clockid_t clock_id, const struct timespec *abstime);

/*! \brief     pthread_rwlock_timedwrlock() with an explicit clock. See pthread_mutex_clocklock() for \a clock_id.
*/
int pthread_rwlock_clockwrlock(pthread_rwlock_t *rwlock, clockid_t clock_id, const struct timespec *abstime);

// -----------------------------------------------------------------------------
// Cooperative cancellation
// -----------------------------------------------------------------------------

/*! \brief     Request cancellation of a thread.
    \details   Cancellation is deferred: the request is recorded and a blocked target is woken (its wait ends early),
               the target terminates as if it called pthread_exit(PTHREAD_CANCELED) when it reaches a cancellation
               point with cancellation enabled. Cleanup handlers (pthread_cleanup_push()) and key destructors run.
               Cancellation points: pthread_testcancel(), pthread_cond_wait/timedwait/clockwait(), pthread_join()
               and its timed forms, sem_wait/timedwait/clockwait(), mq_send/receive and timed forms, nanosleep(),
               clock_nanosleep(), usleep(), sleep(), thrd_sleep() and thrd_join(). Mutex, rwlock and barrier waits
               are NOT cancellation points (as in POSIX). A thread which never reaches a cancellation point can not
               be cancelled, asynchronous cancellation is not supported.
    \return    0, EINVAL for a NULL thread, ESRCH if the thread has already finished.
*/
int pthread_cancel(pthread_t thread);

/*! \brief     Enable or disable cancellation of the calling thread.
    \param[out] oldstate: Previous state (may be NULL).
    \return    0, EINVAL for an invalid \a state.
*/
int pthread_setcancelstate(int state, int *oldstate);

/*! \brief     Set the cancellation type. Only PTHREAD_CANCEL_DEFERRED is supported.
    \return    0, ENOTSUP for PTHREAD_CANCEL_ASYNCHRONOUS, EINVAL otherwise.
*/
int pthread_setcanceltype(int type, int *oldtype);

/*! \brief     Create a cancellation point: terminate the calling thread if a cancellation is pending and enabled.
*/
void pthread_testcancel(void);

/*! \brief     Push a cleanup handler (run in LIFO order by pthread_exit() and by cancellation).
    \note      Function form of the POSIX macro: no lexical pairing with pthread_cleanup_pop() is required, but the
               calls must be balanced. At most STK_C_PTHREAD_CLEANUP_MAX handlers per thread (extra ones are ignored).
*/
void pthread_cleanup_push(void (*routine)(void *), void *arg);

/*! \brief     Pop the most recent cleanup handler, running it if \a execute is non-zero.
*/
void pthread_cleanup_pop(int execute);

/*! \brief     Get the attributes of a running thread (GNU extension).
    \details   Fills \a attr with the stack (address and size), detach state, priority, policy and name of \a thread.
    \warning   The stack address is that of the running thread: do not create a new thread from \a attr.
    \return    0, EINVAL for a NULL argument, ESRCH if the thread is not alive.
*/
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);

// -----------------------------------------------------------------------------
// Terminating a thread by "signal", fork handlers
// -----------------------------------------------------------------------------

// values of the signals which pthread_kill() understands (same as newlib and glibc: identical redefinition is allowed)
#ifndef SIGKILL
    #define SIGKILL 9
#endif
#ifndef SIGTERM
    #define SIGTERM 15
#endif

/*! \brief     Send a "signal" to a thread. STK has no signals, three values are supported:
    \param[in] sig: - 0: existence check, \c 0 if the thread is alive, \c ESRCH if it has finished;
               - \c SIGKILL: terminate the thread immediately. The calling thread ends itself with stk::ExitTask(),
                 another thread is removed with IKernel::ScheduleTaskRemoval(). NO cleanup handler and NO key
                 destructor runs, locks held by the thread stay locked;
               - \c SIGTERM: as pthread_exit(PTHREAD_CANCELED) for the calling thread (cleanup handlers and key
                 destructors run); for another thread the same as \c SIGKILL, since a running task can not be made
                 to execute code.
               pthread_join() returns \c PTHREAD_CANCELED for a terminated thread. Any other signal number fails
               with \c ENOSYS, a negative one with \c EINVAL.
    \return    0, EINVAL, ENOSYS, ESRCH (thread not alive), ENOTSUP (SIGKILL/SIGTERM of an adopted task other than the
               caller: it has no ITask which could be removed).
    \note      Needs the KERNEL_DYNAMIC kernel which the layer requires anyway.
*/
int pthread_kill(pthread_t thread, int sig);

/*! \brief     Register fork handlers. STK has no fork(): the handlers are never run, the call only exists so that
               code and libraries which register them link and work.
    \return    Always 0.
*/
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* STK_C_PTHREAD_H_ */
