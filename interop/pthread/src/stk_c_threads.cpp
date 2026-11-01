/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  stk_c_threads.cpp
    \brief C11 <threads.h> on top of the pthread shim (stk_c_pthread.cpp). Uses only the public C interface of the
           shim (pthread.h / time.h), no STK API.
*/

#include <cstdint>
#include <cstddef>
#include <cerrno>

#include "threads.h"

namespace
{

// Start records: thrd_start_t returns int while a pthread start routine returns void *, so a trampoline carries
// func/arg. A record is only needed until the new thread has copied it, hence a small static table is enough.
struct ThrdStart
{
    thrd_start_t func;
    void        *arg;
    bool         used;
};

ThrdStart       s_Starts[STK_C_PTHREAD_MAX_THREADS];
pthread_mutex_t s_StartsMtx = PTHREAD_MUTEX_INITIALIZER;

ThrdStart *ClaimStart(thrd_start_t func, void *arg)
{
    ThrdStart *result = nullptr;

    (void)pthread_mutex_lock(&s_StartsMtx);

    for (std::size_t i = 0U; i < STK_C_PTHREAD_MAX_THREADS; ++i)
    {
        if (!s_Starts[i].used)
        {
            s_Starts[i].used = true;
            s_Starts[i].func = func;
            s_Starts[i].arg  = arg;
            result = &s_Starts[i];
            break;
        }
    }

    (void)pthread_mutex_unlock(&s_StartsMtx);

    return result;
}

void ReleaseStart(ThrdStart *rec)
{
    (void)pthread_mutex_lock(&s_StartsMtx);
    rec->used = false;
    (void)pthread_mutex_unlock(&s_StartsMtx);
}

void *Trampoline(void *p)
{
    ThrdStart *const rec  = static_cast<ThrdStart *>(p);
    const thrd_start_t fn = rec->func;
    void *const arg       = rec->arg;

    ReleaseStart(rec); // copied: the record is free for the next thrd_create()

    return reinterpret_cast<void *>(static_cast<std::intptr_t>(fn(arg)));
}

int ToThrd(int err)
{
    int result = thrd_error;

    if (err == 0)
    {
        result = thrd_success;
    }
    else if (err == EBUSY)
    {
        result = thrd_busy;
    }
    else if (err == ETIMEDOUT)
    {
        result = thrd_timedout;
    }
    else if (err == ENOMEM)
    {
        result = thrd_nomem;
    }
    else
    {
        // thrd_error
    }

    return result;
}

} // namespace

extern "C" {

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    int result = thrd_error;

    if ((thr != nullptr) && (func != nullptr))
    {
        ThrdStart *const rec = ClaimStart(func, arg);

        if (rec == nullptr)
        {
            result = thrd_nomem;
        }
        else
        {
            const int err = pthread_create(thr, nullptr, &Trampoline, rec);

            if (err != 0)
            {
                ReleaseStart(rec);
                result = (err == EAGAIN) ? thrd_nomem : thrd_error;
            }
            else
            {
                result = thrd_success;
            }
        }
    }

    return result;
}

int thrd_equal(thrd_t lhs, thrd_t rhs)
{
    return pthread_equal(lhs, rhs);
}

thrd_t thrd_current(void)
{
    return pthread_self();
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
    int result = 0;

    if (nanosleep(duration, remaining) != 0)
    {
        result = (errno == EINTR) ? -1 : -2; // C11: -1 interrupted, other negative value for an error
    }

    return result;
}

void thrd_yield(void)
{
    (void)sched_yield();
}

void thrd_exit(int res)
{
    pthread_exit(reinterpret_cast<void *>(static_cast<std::intptr_t>(res)));
}

int thrd_detach(thrd_t thr)
{
    return (pthread_detach(thr) == 0) ? thrd_success : thrd_error;
}

int thrd_join(thrd_t thr, int *res)
{
    void *retval = nullptr;
    int   result = ToThrd(pthread_join(thr, &retval));

    if ((result == thrd_success) && (res != nullptr))
    {
        *res = static_cast<int>(reinterpret_cast<std::intptr_t>(retval));
    }

    return result;
}

int mtx_init(mtx_t *mtx, int type)
{
    pthread_mutexattr_t attr;
    int result = thrd_error;

    if ((mtx != nullptr) && (pthread_mutexattr_init(&attr) == 0))
    {
        if (pthread_mutexattr_settype(&attr, ((type & mtx_recursive) != 0) ? PTHREAD_MUTEX_RECURSIVE :
                                                                             PTHREAD_MUTEX_DEFAULT) == 0)
        {
            result = ToThrd(pthread_mutex_init(mtx, &attr));
        }

        (void)pthread_mutexattr_destroy(&attr);
    }

    return result;
}

int mtx_lock(mtx_t *mtx)                                  { return ToThrd(pthread_mutex_lock(mtx)); }
int mtx_timedlock(mtx_t *mtx, const struct timespec *ts)  { return ToThrd(pthread_mutex_timedlock(mtx, ts)); }
int mtx_trylock(mtx_t *mtx)                               { return ToThrd(pthread_mutex_trylock(mtx)); }
int mtx_unlock(mtx_t *mtx)                                { return ToThrd(pthread_mutex_unlock(mtx)); }
void mtx_destroy(mtx_t *mtx)                              { (void)pthread_mutex_destroy(mtx); }

int cnd_init(cnd_t *cond)                                 { return ToThrd(pthread_cond_init(cond, nullptr)); }
int cnd_signal(cnd_t *cond)                               { return ToThrd(pthread_cond_signal(cond)); }
int cnd_broadcast(cnd_t *cond)                            { return ToThrd(pthread_cond_broadcast(cond)); }
int cnd_wait(cnd_t *cond, mtx_t *mtx)                     { return ToThrd(pthread_cond_wait(cond, mtx)); }
int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts)
{
    return ToThrd(pthread_cond_timedwait(cond, mtx, ts));
}
void cnd_destroy(cnd_t *cond)                             { (void)pthread_cond_destroy(cond); }

int tss_create(tss_t *key, tss_dtor_t dtor)               { return ToThrd(pthread_key_create(key, dtor)); }
void *tss_get(tss_t key)                                  { return pthread_getspecific(key); }
int tss_set(tss_t key, void *val)                         { return ToThrd(pthread_setspecific(key, val)); }
void tss_delete(tss_t key)                                { (void)pthread_key_delete(key); }

void call_once(once_flag *flag, void (*func)(void))       { (void)pthread_once(flag, func); }

} // extern "C"
