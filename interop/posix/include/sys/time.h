/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_SYS_TIME_POSIX_H_
#define STK_C_SYS_TIME_POSIX_H_

/*! \file     sys/time.h
    \brief    POSIX <sys/time.h> extension for STK: gettimeofday(). Implemented by stk_c_pthread.cpp, together with
              time() of <time.h>.

    \details  This header EXTENDS the toolchain's <sys/time.h> (included first via \c #include_next on GCC/Clang, or the
              header named by \c STK_C_SYSTEM_SYS_TIME_H on other compilers).

              Wall time: STK has no real-time clock. gettimeofday() and time() report the kernel time since start plus
              an offset which is 0 until the application sets the current Unix time with stk_posix_set_wall_time()
              (declared in time.h; e.g. after reading an RTC or a network time). The offset applies ONLY to
              gettimeofday() and time(): clock_gettime(), all absolute deadlines (pthread_cond_timedwait(), sem_timedwait(),
              mq_timedreceive(), ...) and TIMER_ABSTIME stay on the kernel timeline, so do not derive such a deadline
              from gettimeofday()/time().

              These functions replace the ones of the C library (newlib's gettimeofday()/time() end in a system call
              stub), the object file of this shim must be linked before libc.
*/

#ifndef _SYS__PTHREADTYPES_H_
    #define _SYS__PTHREADTYPES_H_
#endif

#ifndef STK_C_SYSTEM_SYS_TIME_H
    #if defined(__GNUC__) || defined(__clang__)
        #include_next <sys/time.h>
    #else
        #error "Define STK_C_SYSTEM_SYS_TIME_H to the toolchain's sys/time.h for this compiler"
    #endif
#else
    #include STK_C_SYSTEM_SYS_TIME_H
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*! \def       STK_C_NO_GETTIMEOFDAY_DECL
    \brief     Define it to skip the declaration below if the toolchain's <sys/time.h> already declares it with a
               conflicting spelling.
*/
#ifndef STK_C_NO_GETTIMEOFDAY_DECL

/*! \brief     Get the wall time (microsecond fields, tick resolution).
    \param[out] tv: Receives the time since the Unix epoch (see the file description for what that means on STK).
    \param[in] tz: Obsolete, ignored.
    \return    0, or -1 with errno = EINVAL if \a tv is NULL.
    \warning   ISR-unsafe.
*/
int gettimeofday(struct timeval *tv, void *tz);

#endif /* STK_C_NO_GETTIMEOFDAY_DECL */

#ifdef __cplusplus
}
#endif

#endif /* STK_C_SYS_TIME_POSIX_H_ */
