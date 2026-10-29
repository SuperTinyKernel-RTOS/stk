/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_THREADS_H_
#define STK_C_THREADS_H_

/*! \file     threads.h
    \brief    C11 threads (<threads.h>) for STK: thin wrappers over the pthread shim (stk_c_pthread.h). Implemented
              by stk_c_threads.cpp. Needs stk_pthread_bind_kernel() as the pthread shim does.

    \details  All mutexes are recursive in practice (as pthread mutexes of the shim), mtx_plain and mtx_timed behave
              like mtx_recursive. thrd_create() uses a short-lived start record from a static table of
              STK_C_PTHREAD_MAX_THREADS entries, so no heap is needed.
              thrd_sleep() is a cancellation point and uses nanosleep() of time.h.
*/

#include "stk_c_pthread.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef pthread_t       thrd_t;
typedef int           (*thrd_start_t)(void *);
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t  cnd_t;
typedef pthread_key_t   tss_t;
typedef void          (*tss_dtor_t)(void *);
typedef pthread_once_t  once_flag;

enum { thrd_success = 0, thrd_busy = 1, thrd_error = 2, thrd_nomem = 3, thrd_timedout = 4 };
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };

#define ONCE_FLAG_INIT      PTHREAD_ONCE_INIT
#define TSS_DTOR_ITERATIONS (4)

int   thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int   thrd_equal(thrd_t lhs, thrd_t rhs);
thrd_t thrd_current(void);
int   thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void  thrd_yield(void);
void  thrd_exit(int res);
int   thrd_detach(thrd_t thr);
int   thrd_join(thrd_t thr, int *res);

int   mtx_init(mtx_t *mtx, int type);
int   mtx_lock(mtx_t *mtx);
int   mtx_timedlock(mtx_t *mtx, const struct timespec *ts);
int   mtx_trylock(mtx_t *mtx);
int   mtx_unlock(mtx_t *mtx);
void  mtx_destroy(mtx_t *mtx);

int   cnd_init(cnd_t *cond);
int   cnd_signal(cnd_t *cond);
int   cnd_broadcast(cnd_t *cond);
int   cnd_wait(cnd_t *cond, mtx_t *mtx);
int   cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts);
void  cnd_destroy(cnd_t *cond);

int   tss_create(tss_t *key, tss_dtor_t dtor);
void *tss_get(tss_t key);
int   tss_set(tss_t key, void *val);
void  tss_delete(tss_t key);

void  call_once(once_flag *flag, void (*func)(void));

#ifdef __cplusplus
}
#endif

#endif /* STK_C_THREADS_H_ */
