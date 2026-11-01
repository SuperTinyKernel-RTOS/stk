/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_UNISTD_POSIX_H_
#define STK_C_UNISTD_POSIX_H_

/*! \file     unistd.h
    \brief    POSIX <unistd.h> extensions for STK: sysconf() and getpid(). Implemented by stk_c_pthread.cpp.

    \details  This header EXTENDS the toolchain's <unistd.h> (it is included first via \c #include_next on GCC/Clang,
              or the header named by \c STK_C_SYSTEM_UNISTD_H on other compilers), the \c _SC_* names come from there.

              sysconf() reports the limits of this shim and of the kernel configuration: _SC_CLK_TCK (kernel tick rate),
              _SC_NPROCESSORS_ONLN/_CONF (STK_C_SYSCONF_NPROCESSORS), _SC_PAGESIZE (STK_C_SYSCONF_PAGESIZE), thread, semaphore,
              message queue and timer limits, and the POSIX option versions (200809L for the supported options, -1 for
              the unsupported ones such as _SC_THREAD_PROCESS_SHARED). Unknown names fail with errno = EINVAL.

              getpid() returns the constant STK_C_POSIX_PID: there is a single "process" (the firmware).
*/

/* same newlib guard as in stk_c_pthread.h: <sys/types.h> must not define pthread types of its own */
#ifndef _SYS__PTHREADTYPES_H_
    #define _SYS__PTHREADTYPES_H_
#endif

#ifndef STK_C_SYSTEM_UNISTD_H
    #if defined(__GNUC__) || defined(__clang__)
        #include_next <unistd.h>
    #else
        #error "Define STK_C_SYSTEM_UNISTD_H to the toolchain's unistd.h for this compiler"
    #endif
#else
    #include STK_C_SYSTEM_UNISTD_H
#endif

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! \def       STK_C_POSIX_PID / STK_C_SYSCONF_NPROCESSORS / STK_C_SYSCONF_PAGESIZE
    \brief     Values reported by getpid() and sysconf(). Override in the build system.
*/
#ifndef STK_C_POSIX_PID
    #define STK_C_POSIX_PID (1)
#endif
#ifndef STK_C_SYSCONF_NPROCESSORS
    #define STK_C_SYSCONF_NPROCESSORS (1L)
#endif
#ifndef STK_C_SYSCONF_PAGESIZE
    #define STK_C_SYSCONF_PAGESIZE (4096L)
#endif

/*! \def       STK_C_NO_SYSCONF_DECLS
    \brief     Define it to skip the declarations below if the toolchain's <unistd.h> already declares them
               with a conflicting spelling.
*/
#ifndef STK_C_NO_SYSCONF_DECLS

/*! \brief     Get a configuration value.
    \return    The value, -1 without changing errno if the option is not supported, or -1 with errno = EINVAL for an
               unknown \a name.
    \warning   ISR-unsafe (reads the kernel tick resolution).
*/
long sysconf(int name);

/*! \brief     Get the process id: always STK_C_POSIX_PID.
*/
pid_t getpid(void);

#endif /* STK_C_NO_SYSCONF_DECLS */

#ifdef __cplusplus
}
#endif

#endif /* STK_C_UNISTD_POSIX_H_ */
