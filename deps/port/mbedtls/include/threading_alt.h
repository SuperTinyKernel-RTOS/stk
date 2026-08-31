/**
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * \file threading_alt.h
 *
 * \brief mbedTLS threading abstraction (MBEDTLS_THREADING_ALT) port for
 *        SuperTinyKernel(TM) RTOS (STK).
 *
 * Deploy this file, together with threading_stk.cpp, under
 * deps/port/mbedTLS/ in the STK repository.
 *
 * To use this port, enable in mbedtls_config.h:
 *
 *     #define MBEDTLS_THREADING_C
 *     #define MBEDTLS_THREADING_ALT
 *
 * and, before any other mbedTLS API call (and before stk::IKernel::Start()
 * is required, since this only registers function pointers and placement-
 * constructs two static global mutexes - no STK task context is needed
 * yet), call:
 *
 *     stk_mbedtls_threading_init();
 *
 * That single call also placement-constructs and initializes whichever of
 * mbedTLS's static global mutexes are compiled in: mbedtls_threading_
 * readdir_mutex (MBEDTLS_FS_IO), ..._gmtime_mutex (MBEDTLS_HAVE_TIME_DATE),
 * and, if MBEDTLS_PSA_CRYPTO_C is enabled (typical for current builds),
 * ..._key_slot_mutex, ..._psa_globaldata_mutex and ..._psa_rngdata_mutex -
 * see threading_internal.h. No action is needed here to support that; it
 * falls out of registering the mutex_init callback.
 *
 * Notes:
 *   - mbedTLS's threading functions (lock/unlock/wait/...) will subsequently
 *     be called from wherever TLS/X.509/PK code runs. Those calls MUST
 *     happen from inside an STK task context (a valid, non-ISR TId), since
 *     stk::sync::Mutex/ConditionVariable assert on that. Do not drive
 *     mbedTLS from an ISR.
 *   - stk::sync::ConditionVariable requires the kernel to be instantiated
 *     with the stk::KERNEL_SYNC mode bit enabled.
 *   - This header must remain valid C: core mbedTLS translation units are
 *     compiled as C even though the objects wrapped here are C++. The real
 *     stk::sync::Mutex / stk::sync::ConditionVariable are only named in
 *     threading_stk.cpp (compiled as C++), which placement-constructs them
 *     into the raw storage reserved below.
 */

#ifndef STK_MBEDTLS_THREADING_ALT_H_
#define STK_MBEDTLS_THREADING_ALT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Storage reserved for the wrapped STK objects.
 *
 * sizeof(stk::sync::Mutex) / sizeof(stk::sync::ConditionVariable) depend on
 * your STK build configuration (32- vs 64-bit stk::Word, STK_ARCH_CPU_COUNT,
 * TrustZone variant, etc.), so exact sizes can't be hardcoded here. The
 * defaults below are generous; threading_stk.cpp contains static_asserts
 * that will fail to compile - with a clear message - if they are ever too
 * small for your target. If that happens, override the macros (via a build
 * define or by editing them here) and rebuild.
 */
#if !defined(STK_MBEDTLS_MUTEX_STORAGE_SIZE)
#define STK_MBEDTLS_MUTEX_STORAGE_SIZE 11U
#endif

#if !defined(STK_MBEDTLS_COND_STORAGE_SIZE)
#define STK_MBEDTLS_COND_STORAGE_SIZE 8U
#endif

/** Opaque storage for a placement-constructed stk::sync::Mutex.
 *  You should define the types mbedtls_platform_mutex_t and
 *  mbedtls_platform_condition_variable_t in your header (per
 *  mbedtls/threading.h) - this is that definition for the STK port.
 */
typedef struct mbedtls_platform_mutex_t
{
    struct {
        uintptr_t raw[STK_MBEDTLS_MUTEX_STORAGE_SIZE];
    } storage;
} mbedtls_platform_mutex_t;

/** Opaque storage for a placement-constructed stk::sync::ConditionVariable. */
typedef struct mbedtls_platform_condition_variable_t
{
    struct {
        uintptr_t raw[STK_MBEDTLS_COND_STORAGE_SIZE];
    } storage;
} mbedtls_platform_condition_variable_t;

/**
 * \brief   Register the STK mutex/condition-variable callbacks with mbedTLS
 *          and initialize mbedTLS's global mutexes (readdir/gmtime, if
 *          enabled).
 *
 * \note    Must be called exactly once, before any other mbedTLS API call,
 *          per the contract of mbedtls_threading_set_alt() (see
 *          mbedtls/threading.h). Pair with mbedtls_threading_free_alt() at
 *          shutdown if your application tears mbedTLS down.
 */
void stk_mbedtls_threading_init(void);

#ifdef __cplusplus
}
#endif

#endif /* STK_MBEDTLS_THREADING_ALT_H_ */
