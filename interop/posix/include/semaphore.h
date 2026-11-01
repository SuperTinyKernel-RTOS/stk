/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_SEMAPHORE_H_
#define STK_C_SEMAPHORE_H_

/*! \file     semaphore.h
    \brief    POSIX-named counting semaphores (<semaphore.h>) for STK, backed by stk::sync::SemaphoreT.
              Implemented by stk_c_pthread.cpp (the same shim as <pthread.h>, no dependency on the STK C API);
              stk_pthread_bind_kernel() is not required, only a running kernel with KERNEL_SYNC.

    \details  Unnamed semaphores only: sem_init/destroy/wait/trywait/timedwait/clockwait/post/getvalue.
              Named semaphores (sem_open/close/unlink) are not provided, STK has no named kernel objects.

              Like POSIX, the functions return 0 on success and -1 on failure with \c errno set:
              - sem_wait(): EINTR if the wait was cancelled via IKernel::CancelTaskWait(), retry as for a signal.
              - sem_trywait(): EAGAIN if the count is zero.
              - sem_timedwait()/sem_clockwait(): ETIMEDOUT, or EINVAL for an invalid timeout. The deadline is
                interpreted on the STK tick timeline (see "timedwait limitation" in stk_c_pthread.h) and is
                rounded up to whole ticks. A token which is available is always taken, even if the deadline
                has already passed.
              - sem_post(): EOVERFLOW if the count is already SEM_VALUE_MAX and nobody is waiting.
              - sem_init(): EINVAL if the value exceeds SEM_VALUE_MAX, ENOSYS if \a pshared is non-zero.
*/

#ifdef __cplusplus
    #include <cstdint>
    #include <cstddef>
    #include <ctime>
#else
    #include <stdint.h>
    #include <stddef.h>
    #include <time.h>
#endif

#include <stk_config.h>

// must match the definition used by the C++ kernel API
#ifndef STK_SYNC_DEBUG_NAMES
    #if STK_SEGGER_SYSVIEW
        #define STK_SYNC_DEBUG_NAMES (1)
    #else
        #define STK_SYNC_DEBUG_NAMES (0)
    #endif
#endif

#ifndef CLOCK_REALTIME
    #define CLOCK_REALTIME  (0)
#endif
#ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC (1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*! \def       SEM_VALUE_MAX
    \brief     Maximum value of a semaphore (the counter is 32-bit, POSIX requires at least 32767).
*/
#define SEM_VALUE_MAX (2147483647)

/*! \def       STK_C_SEM_WORDS
    \brief     Size (in machine words) of the opaque storage of a sem_t: the C++ object (7 words + 1 with
               debug names) plus two 32-bit counters and alignment slack. Verified at compile time against
               sizeof() of the real object by stk_c_pthread.cpp.
*/
#define STK_C_SEM_WORDS (7U + (STK_SYNC_DEBUG_NAMES ? 1U : 0U) + \
                         ((2U * sizeof(uint32_t) + sizeof(uintptr_t) - 1U) / sizeof(uintptr_t)) + 1U)

#if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__)
    #define __stk_sem_aligned __attribute__((aligned(sizeof(uintptr_t))))
#else
    #define __stk_sem_aligned
#endif

/*! \brief     A POSIX counting semaphore.
    \note      Fields are considered private. There is no static initializer (as in POSIX): use sem_init().
*/
typedef struct sem_t
{
    struct {
        uintptr_t data[STK_C_SEM_WORDS] __stk_sem_aligned;
    } __mem;           //!< In-place storage of the C++ semaphore.
    void *__handle;    //!< NULL until sem_init(), then points into __mem.
} sem_t;

/*! \brief     Initialize an unnamed semaphore.
    \param[in] sem: Semaphore to initialize.
    \param[in] pshared: Must be 0 (process-shared semaphores are not supported).
    \param[in] value: Initial count, at most SEM_VALUE_MAX.
    \return    0 on success, -1 with errno = EINVAL (NULL \a sem or \a value too large) or ENOSYS (\a pshared != 0).
*/
int sem_init(sem_t *sem, int pshared, unsigned int value);

/*! \brief     Destroy a semaphore.
    \warning   No task may be waiting on it.
    \return    0 on success, -1 with errno = EINVAL if \a sem is NULL or not initialized.
*/
int sem_destroy(sem_t *sem);

/*! \brief     Take a token, blocking until one is available.
    \return    0 on success, -1 with errno = EINVAL, or EINTR if the wait was cancelled (retry).
    \warning   ISR-unsafe.
*/
int sem_wait(sem_t *sem);

/*! \brief     Take a token without blocking.
    \return    0 on success, -1 with errno = EAGAIN if the count is zero, or EINVAL.
    \note      ISR-safe.
*/
int sem_trywait(sem_t *sem);

/*! \brief     Take a token, waiting until the absolute deadline \a abstime.
    \return    0 on success, -1 with errno = ETIMEDOUT, EINVAL or EINTR.
    \warning   ISR-unsafe.
*/
int sem_timedwait(sem_t *sem, const struct timespec *abstime);

/*! \brief     sem_timedwait() with an explicit clock (POSIX.1-2024).
    \param[in] clock_id: CLOCK_REALTIME or CLOCK_MONOTONIC, both refer to the same STK tick timeline.
    \return    As sem_timedwait(), EINVAL for any other clock.
*/
int sem_clockwait(sem_t *sem, clockid_t clock_id, const struct timespec *abstime);

/*! \brief     Release a token (wakes the first waiter, otherwise increments the count).
    \return    0 on success, -1 with errno = EOVERFLOW if the count is at SEM_VALUE_MAX, or EINVAL.
    \note      ISR-safe.
*/
int sem_post(sem_t *sem);

/*! \brief     Get the current count.
    \param[out] sval: Receives the count (0 if tasks are waiting, STK does not report negative values).
    \return    0 on success, -1 with errno = EINVAL.
    \note      ISR-safe. The value may be stale as soon as it is returned.
*/
int sem_getvalue(sem_t *sem, int *sval);

#ifdef __cplusplus
}
#endif

#endif /* STK_C_SEMAPHORE_H_ */
