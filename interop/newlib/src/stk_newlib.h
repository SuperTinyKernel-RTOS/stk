/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_NEWLIB_H_
#define STK_NEWLIB_H_

#include <sys/lock.h>

/*! \file  stk_newlib.h
    \brief GCC Newlib OS interface: retargetable locking (__retarget_lock_*) on top of STK.

    Newlib must be built with _RETARGETABLE_LOCKING (the default for arm-none-eabi-gcc
    toolchains). Link stk_newlib.cpp into the application and Newlib will serialize its
    internal state (malloc, stdio, atexit, env, ...) through stk::sync::Mutex and
    stk::sync::FastMutex.

    Mapping:
    - __retarget_lock_*            -> stk::sync::FastMutex (non-recursive, self-lock panics)
    - __retarget_lock_*_recursive  -> stk::sync::Mutex     (recursive)

    Behavior notes:
    - Until the kernel is running (no STK task context), all lock operations are no-ops:
      this covers early startup, static constructors and __libc_init_array() when only one
      flow of execution exists.
    - Locks are ISR-unsafe. Do not call Newlib functions that take locks (malloc, printf, ...)
      from an ISR.
    - Newlib's static locks (__lock___malloc_recursive_mutex, etc.) are defined by
      stk_newlib.cpp and constructed lazily on first use, so they are safe to use before
      global constructors have run. Disable with STK_NEWLIB_STATIC_LOCKS = 0 if your
      Newlib build already provides them.
    - Dynamic locks (__retarget_lock_init*, e.g. one per open FILE stream) are taken from a
      fixed-size static pool of STK_NEWLIB_MAX_LOCKS blocks (stk::memory::BlockMemoryPool),
      so the shim itself never uses the heap. The pool is constructed on first use, so it is
      safe to use before global constructors have run.

    \note Requires the kernel to be compiled with \a KERNEL_SYNC mode enabled.
*/

#ifndef _RETARGETABLE_LOCKING
    #error "stk_newlib.h: Newlib must be built with _RETARGETABLE_LOCKING (sys/lock.h does not expose _LOCK_T)"
#endif

/*! \def   STK_NEWLIB_STATIC_LOCKS
    \brief 1 (default): stk_newlib.cpp defines Newlib's static lock objects (__lock___sinit_recursive_mutex, ...).
           0: do not define them (Newlib build or another module provides them).
*/
#ifndef STK_NEWLIB_STATIC_LOCKS
    #define STK_NEWLIB_STATIC_LOCKS 1
#endif

/*! \def   STK_NEWLIB_MAX_LOCKS
    \brief Capacity of the static pool for dynamic lock objects: the maximum number of locks
           created by __retarget_lock_init*() and not yet closed at the same time. Newlib
           creates one for each standard stream and each open FILE stream. Newlib's init hooks
           cannot report failure, so exhausting the pool panics with KERNEL_PANIC_NO_MEM.
           Default: 8.
*/
#ifndef STK_NEWLIB_MAX_LOCKS
    #define STK_NEWLIB_MAX_LOCKS 8U
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*! \brief     Allocate lock related resources.
    \param[in] lock: pointer to user defined lock object
*/
void __retarget_lock_init(_LOCK_T *lock);

/*! \brief     Allocate recursive lock related resources.
    \param[in] lock: pointer to user defined lock object
*/
void __retarget_lock_init_recursive(_LOCK_T *lock);

/*! \brief     Free lock related resources.
    \param[in] lock: user defined lock object
*/
void __retarget_lock_close(_LOCK_T lock);

/*! \brief     Free recursive lock related resources.
    \param[in] lock: user defined lock object
*/
void __retarget_lock_close_recursive(_LOCK_T lock);

/*! \brief     Acquire lock immediately after the lock object is available.
    \param[in] lock: user defined lock object
*/
void __retarget_lock_acquire(_LOCK_T lock);

/*! \brief     Acquire recursive lock immediately after the lock object is available.
    \param[in] lock: user defined lock object
*/
void __retarget_lock_acquire_recursive(_LOCK_T lock);

/*! \brief     Acquire lock if the lock object is available.
    \param[in] lock: user defined lock object
    \return    Zero for success and non-zero to indicate that the lock cannot be acquired.
*/
int __retarget_lock_try_acquire(_LOCK_T lock);

/*! \brief     Acquire recursive lock if the lock object is available.
    \param[in] lock: user defined lock object
    \return    Zero for success and non-zero to indicate that the lock cannot be acquired.
*/
int __retarget_lock_try_acquire_recursive(_LOCK_T lock);

/*! \brief     Relinquish the lock ownership.
    \param[in] lock: user defined lock object
*/
void __retarget_lock_release(_LOCK_T lock);

/*! \brief     Relinquish the recursive lock ownership.
    \param[in] lock: user defined lock object
*/
void __retarget_lock_release_recursive(_LOCK_T lock);

#ifdef __cplusplus
}
#endif

#endif /* STK_NEWLIB_H_ */
