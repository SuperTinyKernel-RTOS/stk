/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_POSIX_PTHREAD_H_
#define STK_POSIX_PTHREAD_H_

/*! \file     pthread.h
    \brief    POSIX-named entry point for the STK pthread shim (stk_c_pthread.h).

    \details  Lets code written against <pthread.h> build unchanged on STK. This header only
              forwards to stk_c_pthread.h, which is the canonical definition.

              Opt-in: add this directory (include/posix) to the compiler include path with -I,
              ahead of the toolchain's system include directories, so that #include <pthread.h>
              resolves here instead of to the libc header. Projects that do not add it keep
              using stk_c_pthread.h directly.

    \note     Include order matters. stk_c_pthread.h suppresses a libc's own pthread types only
              if it is seen before the libc header that defines them; including a libc header
              that already declares pthread_t / pthread_mutex_t first can cause redefinition
              errors. See test/posix/pthread_compile_check.c.
*/

#include "stk_c_pthread.h"

#endif /* STK_POSIX_PTHREAD_H_ */
