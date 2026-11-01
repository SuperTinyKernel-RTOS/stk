/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_TIME_POSIX_H_
#define STK_C_TIME_POSIX_H_

/*! \file     time.h
    \brief    POSIX time extensions for STK: clock_gettime(), clock_getres(), nanosleep(), clock_nanosleep(),
              usleep(), sleep(), timespec_get() and POSIX timers (timer_create() and friends).
              Implemented by stk_c_pthread.cpp (no dependency on the STK C API).

    \details  This header EXTENDS the toolchain's <time.h>: it first includes the standard header (via
              \c #include_next on GCC/Clang, or the header named by \c STK_C_SYSTEM_TIME_H on other compilers) and
              then adds the declarations above, so it can be placed ahead of the toolchain on the include path.

              Clocks: STK has a single time source, the kernel tick counter. CLOCK_MONOTONIC and CLOCK_REALTIME both
              report the time since the kernel was started (there is no calendar time), with the resolution of a
              kernel tick. All absolute deadlines of the pthread/semaphore/mqueue timed calls use the same timeline.

              Sleeping functions are cancellation points of pthread_cancel() and can be interrupted early
              (EINTR) by IKernelService::SleepCancel(); timers are provided on top of an application-owned
              stk::time::TimerHost, see stk_posix_timer_bind_host().

    \note     nanosleep()/sleep()/usleep() are ISR-unsafe and not available in HRT mode.
*/

/* Same newlib guard as in stk_c_pthread.h: <sys/types.h> must not define its own pthread types, the shim provides
   them (needed here because this header may be included before stk_c_pthread.h). */
#ifndef _SYS__PTHREADTYPES_H_
    #define _SYS__PTHREADTYPES_H_
#endif

#ifndef STK_C_SYSTEM_TIME_H
    #if defined(__GNUC__) || defined(__clang__)
        #include_next <time.h>
    #else
        #error "Define STK_C_SYSTEM_TIME_H to the toolchain's time.h (e.g. <../inc/c/time.h>) for this compiler"
    #endif
#else
    #include STK_C_SYSTEM_TIME_H
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h> /* useconds_t */

/* Toolchains which already ship the POSIX timer types: newlib (arm-none-eabi-gcc) defines timer_t in <sys/types.h> and
   struct itimerspec in <sys/timespec.h>, glibc has both too. Detected here, after the toolchain's headers were read,
   so that the types are not declared twice. Other toolchains: define the STK_C_NO_* macros below by hand. */
#if defined(__NEWLIB__) || defined(__GLIBC__)
    #ifndef STK_C_NO_TIMER_T
        #define STK_C_NO_TIMER_T
    #endif
    #ifndef STK_C_NO_ITIMERSPEC
        #define STK_C_NO_ITIMERSPEC
    #endif
#endif
#if defined(_SYS_TIMESPEC_H) && !defined(STK_C_NO_ITIMERSPEC)
    #define STK_C_NO_ITIMERSPEC
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*! \def       STK_C_NO_TIME_DECLS / STK_C_NO_SLEEP_DECLS / STK_C_NO_TIMER_DECLS / STK_C_NO_ITIMERSPEC
    \brief     Define any of them (before including this header, or in the build system) to skip declarations which the
               toolchain's <time.h>/<unistd.h> already provide with a different spelling:
               - STK_C_NO_TIME_DECLS:  clock_gettime, clock_getres, clock_nanosleep, nanosleep, timespec_get
               - STK_C_NO_SLEEP_DECLS: usleep, sleep
               - STK_C_NO_TIMER_DECLS: timer_create, timer_delete, timer_settime, timer_gettime, timer_getoverrun
               - STK_C_NO_ITIMERSPEC:  struct itimerspec
               Together with STK_C_NO_TIMER_T and STK_C_DEFINE_SIGEVENT=0 (see below) this makes the header a pure
               add-on for toolchains (e.g. newlib with _POSIX_TIMERS) which already ship the types.
*/

#ifndef CLOCK_REALTIME
    #define CLOCK_REALTIME  (0)
#endif
#ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC (1)
#endif
#ifndef TIMER_ABSTIME
    #define TIMER_ABSTIME   (1)
#endif
#ifndef TIME_UTC
    #define TIME_UTC        (1)
#endif

#ifndef STK_C_NO_TIME_DECLS

/*! \brief     Get the current time of a clock.
    \param[in] clk_id: CLOCK_REALTIME or CLOCK_MONOTONIC (the same STK kernel timeline).
    \return    0, or -1 with errno = EINVAL.
    \warning   ISR-unsafe.
*/
int clock_gettime(clockid_t clk_id, struct timespec *ts);

/*! \brief     Get the resolution (one kernel tick) of a clock.
    \return    0, or -1 with errno = EINVAL.
    \warning   ISR-unsafe.
*/
int clock_getres(clockid_t clk_id, struct timespec *res);

/*! \brief     C11 timespec_get(): \a base must be TIME_UTC (reports the same STK timeline).
    \return    \a base on success, 0 otherwise.
*/
int timespec_get(struct timespec *ts, int base);

/*! \brief     Suspend the calling task for a relative time (rounded UP to whole ticks). Cancellation point.
    \param[out] rem: Receives the remaining time if interrupted (may be NULL).
    \return    0, or -1 with errno = EINVAL (invalid \a req) or EINTR (woken early).
*/
int nanosleep(const struct timespec *req, struct timespec *rem);

/*! \brief     Suspend for a relative time or until an absolute time (\a flags = TIMER_ABSTIME). Cancellation point.
    \return    0, or the error number (EINVAL, EINTR); unlike nanosleep() it does not use errno.
*/
int clock_nanosleep(clockid_t clk_id, int flags, const struct timespec *req, struct timespec *rem);

#endif /* STK_C_NO_TIME_DECLS */

#ifndef STK_C_NO_SLEEP_DECLS

/*! \brief     Suspend for \a usec microseconds. Cancellation point.
    \return    0, or -1 with errno = EINTR.
*/
int usleep(useconds_t usec);

/*! \brief     Suspend for \a seconds seconds. Cancellation point.
    \return    0 if the full time elapsed, otherwise the unslept seconds.
*/
unsigned int sleep(unsigned int seconds);

#endif /* STK_C_NO_SLEEP_DECLS */

/* --------------------------------------------------------------------------------------------------------------- */
/* POSIX timers                                                                                                    */
/* --------------------------------------------------------------------------------------------------------------- */

/*! \def       STK_C_NO_TIMER_T
    \brief     Define it if the toolchain already provides \c timer_t (e.g. newlib's <sys/types.h>), the shim then
               only needs it to be an integer or pointer type wide enough for a pointer.
*/
#ifndef STK_C_NO_TIMER_T
    typedef void *timer_t;
#endif

/*! \def       STK_C_DEFINE_SIGEVENT
    \brief     Set to 0 if the toolchain's <signal.h> already provides union sigval / struct sigevent / SIGEV_*.
*/
#ifndef STK_C_DEFINE_SIGEVENT
    #define STK_C_DEFINE_SIGEVENT (1)
#endif

#if STK_C_DEFINE_SIGEVENT && !defined(SIGEV_NONE) /* <signal.h> was included first: it already provides them */
    union sigval
    {
        int   sival_int;
        void *sival_ptr;
    };

    struct sigevent
    {
        int          sigev_notify;                            /*!< SIGEV_NONE or SIGEV_THREAD. */
        int          sigev_signo;                             /*!< Unused (no signals). */
        union sigval sigev_value;                             /*!< Passed to the notification function. */
        void       (*sigev_notify_function)(union sigval);    /*!< SIGEV_THREAD: called when the timer expires. */
        void        *sigev_notify_attributes;                 /*!< Unused. */
    };

    #define SIGEV_NONE   (0)
    #define SIGEV_SIGNAL (1)
    #define SIGEV_THREAD (2)
#endif

#ifndef STK_C_NO_ITIMERSPEC
struct itimerspec
{
    struct timespec it_interval; /*!< Reload period (0 = one-shot). */
    struct timespec it_value;    /*!< Time until the first expiration (0 = disarmed). */
};
#endif

/*! \def       STK_C_TIMERS_MAX
    \brief     Number of POSIX timers which can exist at the same time.
*/
#ifndef STK_C_TIMERS_MAX
    #define STK_C_TIMERS_MAX (8U)
#endif

/*! \brief     Set the current wall time used by time() and gettimeofday() (see sys/time.h).
    \param[in] unix_seconds: Seconds since 1970-01-01 00:00:00 UTC at the moment of the call.
    \note      Only time() and gettimeofday() are shifted, the kernel timeline of clock_gettime() and of all deadlines
               is not changed. Thread-safe, ISR-unsafe.
*/
void stk_posix_set_wall_time(int64_t unix_seconds);

/*! \brief     Provide the timer host which runs POSIX timers.
    \param[in] timer_host: Pointer to an initialized stk::time::TimerHost (task mode), or NULL to unbind.
               The host is owned by the application (it decides weights, stacks and thread count);
               without a bound host timer_create() fails with EAGAIN.
    \note      Notifications run in a handler task of the host, keep them short.
*/
void stk_posix_timer_bind_host(void *timer_host);

#ifndef STK_C_NO_TIMER_DECLS

/*! \brief     Create a timer.
    \param[in] sevp: SIGEV_THREAD (the function is called from a timer handler task) or SIGEV_NONE (poll with
               timer_gettime()). NULL means SIGEV_NONE (there are no signals). SIGEV_SIGNAL gives ENOTSUP.
    \return    0, or -1 with errno = EINVAL, ENOTSUP, EAGAIN (no host bound or no free timer).
*/
int timer_create(clockid_t clk_id, struct sigevent *sevp, timer_t *timerid);

/*! \brief     Delete a timer (waits until a running notification has returned).
    \return    0, or -1 with errno = EINVAL.
*/
int timer_delete(timer_t timerid);

/*! \brief     Arm (it_value != 0) or disarm (it_value == 0) a timer.
    \param[in] flags: 0 (relative) or TIMER_ABSTIME. Times are rounded up to whole ticks.
    \param[out] old_value: Previous setting (may be NULL).
    \return    0, or -1 with errno = EINVAL, EAGAIN (host command queue full), EINTR.
    \warning   ISR-unsafe. Do not delete or re-arm a timer from the notification of another timer's host task
               if that would wait for it (see TimerHost "Synchronous API").
*/
int timer_settime(timer_t timerid, int flags, const struct itimerspec *new_value, struct itimerspec *old_value);

/*! \brief     Get the time until the next expiration and the period.
    \return    0, or -1 with errno = EINVAL.
*/
int timer_gettime(timer_t timerid, struct itimerspec *curr_value);

/*! \brief     Get the overrun count. STK does not track overruns.
    \return    Always 0 for a valid timer, -1 with errno = EINVAL otherwise.
*/
int timer_getoverrun(timer_t timerid);

#endif /* STK_C_NO_TIMER_DECLS */

#ifdef __cplusplus
}
#endif

#endif /* STK_C_TIME_POSIX_H_ */
