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
 * \file stk_mbedtls_threading.cpp
 *
 * \brief mbedTLS threading abstraction (MBEDTLS_THREADING_ALT) port for
 *        SuperTinyKernel(TM) RTOS (STK) - implementation.
 *
 * Deploy this file, together with threading_alt.h, under
 * deps/port/mbedtls/ in the STK repository. Compile as C++ (it is included
 * in the mbedTLS build as an extra translation unit; only its extern "C"
 * entry points and the mbedtls_platform_*_t layout are visible to the rest
 * of mbedTLS, which stays plain C).
 */

#include "mbedtls/threading.h" /* declares mbedtls_threading_set_alt() and pulls in threading_alt.h */
#include "mbedtls/platform_util.h"
#include "mbedtls/platform.h"

#include "stk.h"
#include "sync/stk_sync.h"

#include <new>
#include <cstddef>

// ----------------------------------------------------------------------------
namespace stk {
namespace port {
// ----------------------------------------------------------------------------

static __stk_forceinline stk::sync::Mutex *AsMutex(mbedtls_platform_mutex_t *mutex)
{
    STK_ASSERT(mutex != nullptr);
    return reinterpret_cast<stk::sync::Mutex *>(mutex->storage.raw);
}

static __stk_forceinline stk::sync::ConditionVariable *AsCond(mbedtls_platform_condition_variable_t *cond)
{
    STK_ASSERT(cond != nullptr);
    return reinterpret_cast<stk::sync::ConditionVariable *>(cond->storage.raw);
}

/*
 * Compile-time guardrails: if STK_MBEDTLS_MUTEX_STORAGE_SIZE /
 * STK_MBEDTLS_COND_STORAGE_SIZE (threading_alt.h) are too small or under-
 * aligned for stk::sync::Mutex / stk::sync::ConditionVariable on this
 * target, fail here with a clear message rather than corrupting memory at
 * runtime.
 */
static_assert(sizeof(stk::sync::Mutex) <= sizeof(mbedtls_platform_mutex_t::storage.raw),
      "STK_MBEDTLS_MUTEX_STORAGE_SIZE (threading_alt.h) is too small for "
      "stk::sync::Mutex on this target - increase it and rebuild.");
static_assert(alignof(mbedtls_platform_mutex_t) % alignof(stk::sync::Mutex) == 0,
      "mbedtls_platform_mutex_t's storage union is not aligned strictly "
      "enough for stk::sync::Mutex - widen the union in threading_alt.h.");

static_assert(sizeof(stk::sync::ConditionVariable) <= sizeof(mbedtls_platform_condition_variable_t::storage.raw),
      "STK_MBEDTLS_COND_STORAGE_SIZE (threading_alt.h) is too small for "
      "stk::sync::ConditionVariable on this target - increase it and rebuild.");
static_assert(alignof(mbedtls_platform_condition_variable_t) % alignof(stk::sync::ConditionVariable) == 0,
      "mbedtls_platform_condition_variable_t's storage union is not aligned "
      "strictly enough for stk::sync::ConditionVariable - widen the union in "
      "threading_alt.h.");

// ----------------------------------------------------------------------------
} // namespace port
} // namespace stk
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
extern "C" {
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Mutex
// ----------------------------------------------------------------------------

static int stk_mutex_init(mbedtls_platform_mutex_t *mutex)
{
    /* stk::sync::Mutex's constructor is non-allocating and cannot fail. */
    ::new (mutex->storage.raw) stk::sync::Mutex();
    return 0;
}

static void stk_mutex_destroy(mbedtls_platform_mutex_t *mutex)
{
    /* Contract (mbedtls/threading.h): must not be locked and must have no
     * waiters; ~Mutex() enforces this with an assertion in debug builds. */
    stk::port::AsMutex(mutex)->~Mutex();
}

static int stk_mutex_lock(mbedtls_platform_mutex_t *mutex)
{
    /* mbedTLS never re-locks a mutex already held by the calling thread, so
     * Mutex's recursion support is simply unused here. */
    stk::port::AsMutex(mutex)->Lock();
    return 0;
}

static int stk_mutex_unlock(mbedtls_platform_mutex_t *mutex)
{
    stk::port::AsMutex(mutex)->Unlock();
    return 0;
}

// ----------------------------------------------------------------------------
// Condition variable
// ----------------------------------------------------------------------------

static int stk_cond_init(mbedtls_platform_condition_variable_t *cond)
{
    ::new (cond->storage.raw) stk::sync::ConditionVariable();
    return 0;
}

static void stk_cond_destroy(mbedtls_platform_condition_variable_t *cond)
{
    /* Contract: no task may be waiting on cond when this is called;
     * ~ConditionVariable() enforces this with an assertion in debug builds. */
    stk::port::AsCond(cond)->~ConditionVariable();
}

static int stk_cond_signal(mbedtls_platform_condition_variable_t *cond)
{
    stk::port::AsCond(cond)->NotifyOne();
    return 0;
}

static int stk_cond_broadcast(mbedtls_platform_condition_variable_t *cond)
{
    stk::port::AsCond(cond)->NotifyAll();
    return 0;
}

static int stk_cond_wait(mbedtls_platform_condition_variable_t *cond,
                         mbedtls_platform_mutex_t *mutex)
{
    /* mutex must already be held by the calling task (mbedTLS's contract).
     * ConditionVariable::Wait() atomically releases it, blocks the task,
     * and re-locks it before returning - exactly what mbedtls_condition_
     * variable_wait() requires. WAIT_INFINITE matches mbedTLS's expectation
     * that this only returns once signaled/broadcast; the API explicitly
     * tolerates spurious wakeups, so no predicate re-check loop is needed
     * here (the caller is required to loop on its own predicate anyway).
     *
     * Wait() returns false not just on timeout (which WAIT_INFINITE should
     * never produce) but also on WAIT_RESULT_FAIL (an internal STK error).
     * Report that as a usage error instead of silently claiming success -
     * mirroring how the reference pthread backend's err_from_posix() never
     * swallows a failed pthread_cond_wait(). */
    const bool signaled = stk::port::AsCond(cond)->Wait(*stk::port::AsMutex(mutex), stk::WAIT_INFINITE);
    return (signaled ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR);
}

void stk_mbedtls_threading_init(void)
{
    mbedtls_threading_set_alt(
        stk_mutex_init, stk_mutex_destroy, stk_mutex_lock, stk_mutex_unlock,
        stk_cond_init, stk_cond_destroy, stk_cond_signal, stk_cond_broadcast, stk_cond_wait);
}

// ----------------------------------------------------------------------------
} // extern "C"
// ----------------------------------------------------------------------------
