/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_SIGNAL_POSIX_H_
#define STK_C_SIGNAL_POSIX_H_

/*! \file     signal.h
    \brief    Wrapper of the toolchain's <signal.h> for use together with the STK POSIX layer.

    \details  STK has no signals, this header only makes the toolchain's signal types usable next to the layer:

              - the layer's pthread types (pthread_t, pthread_attr_t, ...) are defined BEFORE the toolchain's
                <signal.h> is read, because newlib's <sys/signal.h> declares pthread_kill(pthread_t, int) and
                pthread_sigmask() and refers to pthread_attr_t;
              - _POSIX_THREADS is defined while the toolchain's header is parsed: newlib declares the members
                sigev_notify_function and sigev_notify_attributes of struct sigevent only if it is defined, and
                SIGEV_THREAD timers (timer_create()) need them. The macro is restored afterwards.

              The toolchain's <signal.h> is included with \c #include_next (GCC/Clang) or from the header named by
              \c STK_C_SYSTEM_SIGNAL_H (other compilers). No signal function is implemented: sigaction(), kill(),
              pthread_kill(), pthread_sigmask(), ... may be declared but are not provided (link error if used).
*/

/* the layer's pthread types first (this also installs the guard against the toolchain's own pthread types) */
#include <stk_c_pthread.h>

#ifndef _POSIX_THREADS
    #define _POSIX_THREADS 200809L
    #define STK_C_SIGNAL_DEFINED_POSIX_THREADS
#endif

#ifndef STK_C_SYSTEM_SIGNAL_H
    #if defined(__GNUC__) || defined(__clang__)
        #include_next <signal.h>
    #else
        #error "Define STK_C_SYSTEM_SIGNAL_H to the toolchain's signal.h for this compiler"
    #endif
#else
    #include STK_C_SYSTEM_SIGNAL_H
#endif

#ifdef STK_C_SIGNAL_DEFINED_POSIX_THREADS
    #undef _POSIX_THREADS
    #undef STK_C_SIGNAL_DEFINED_POSIX_THREADS
#endif

#endif /* STK_C_SIGNAL_POSIX_H_ */
