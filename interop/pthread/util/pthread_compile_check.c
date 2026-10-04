/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*
 * Compile-only check that <pthread.h> resolves to the STK wrapper (include/posix/pthread.h) and
 * that the whole pthread API is declared and usable from both C and C++. Nothing here is linked
 * or run; the function below only has to compile.
 *
 * Build it with -c, once per language and toolchain, with include/posix BEFORE any system path:
 *
 *   <cc>  -c -I include/posix -I include [target flags] test/posix/pthread_compile_check.c
 *   <cc>  -c -x c++ -I include/posix -I include [target flags] test/posix/pthread_compile_check.c
 *
 * Repeat each build with -DSTK_CHECK_LIBC_FIRST to include a libc header before <pthread.h>.
 * If that variant fails with redefinition errors for pthread_t / pthread_mutex_t, the libc
 * declares its own pthread types and application code must include <pthread.h> before the
 * libc header (see the note in include/posix/pthread.h).
 */

#ifdef STK_CHECK_LIBC_FIRST
    #include <sys/types.h>
#endif

#include <pthread.h>

#ifndef STK_C_PTHREAD_H_
    #error "<pthread.h> did not resolve to the STK wrapper: put -I include/posix ahead of the system include paths."
#endif

#include <errno.h>
#include <time.h>

static pthread_mutex_t    g_Mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t     g_Cond   = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t   g_RwLock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_once_t     g_Once   = PTHREAD_ONCE_INIT;
static pthread_key_t      g_Key;
static pthread_spinlock_t g_Spin;
static pthread_barrier_t  g_Barrier;

static void InitOnce(void)
{
}

static void KeyDestructor(void *value)
{
    (void)value;
}

static void *ThreadEntry(void *arg)
{
    return arg;
}

int stk_pthread_compile_check(void)
{
    int rc = 0;

    pthread_t           thread;
    pthread_attr_t      attr;
    pthread_mutexattr_t mattr;
    struct timespec     abstime;
    void               *retval = NULL;
    int                 mode   = 0;

    abstime.tv_sec  = 0;
    abstime.tv_nsec = 0;

    /* thread attributes and lifecycle */
    rc |= pthread_attr_init(&attr);
    rc |= pthread_attr_setstacksize(&attr, (size_t)(STK_C_PTHREAD_DEFAULT_STACK_WORDS * sizeof(stk_word_t)));
    rc |= pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    rc |= pthread_attr_setprivileged_np(&attr, PTHREAD_CREATE_USER_NP);
    rc |= pthread_attr_getprivileged_np(&attr, &mode);
    rc |= pthread_create(&thread, &attr, ThreadEntry, NULL);
    rc |= pthread_join(thread, &retval);
    rc |= pthread_detach(thread);
    rc |= pthread_equal(pthread_self(), thread);
    rc |= pthread_yield();
    rc |= pthread_attr_destroy(&attr);

    /* mutex */
    rc |= pthread_mutexattr_init(&mattr);
    rc |= pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK);
    rc |= pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    rc |= pthread_mutex_init(&g_Mutex, &mattr);
    rc |= pthread_mutex_lock(&g_Mutex);
    rc |= pthread_mutex_trylock(&g_Mutex);
    rc |= pthread_mutex_timedlock(&g_Mutex, &abstime);
    rc |= pthread_mutex_unlock(&g_Mutex);
    rc |= pthread_mutex_destroy(&g_Mutex);
    rc |= pthread_mutexattr_destroy(&mattr);

    /* condition variable */
    rc |= pthread_cond_init(&g_Cond, NULL);
    rc |= pthread_cond_wait(&g_Cond, &g_Mutex);
    rc |= pthread_cond_timedwait(&g_Cond, &g_Mutex, &abstime);
    rc |= pthread_cond_signal(&g_Cond);
    rc |= pthread_cond_broadcast(&g_Cond);
    rc |= pthread_cond_destroy(&g_Cond);

    /* read-write lock */
    rc |= pthread_rwlock_init(&g_RwLock, NULL);
    rc |= pthread_rwlock_rdlock(&g_RwLock);
    rc |= pthread_rwlock_tryrdlock(&g_RwLock);
    rc |= pthread_rwlock_timedrdlock(&g_RwLock, &abstime);
    rc |= pthread_rwlock_wrlock(&g_RwLock);
    rc |= pthread_rwlock_trywrlock(&g_RwLock);
    rc |= pthread_rwlock_timedwrlock(&g_RwLock, &abstime);
    rc |= pthread_rwlock_unlock(&g_RwLock);
    rc |= pthread_rwlock_destroy(&g_RwLock);

    /* spinlock */
    rc |= pthread_spin_init(&g_Spin, PTHREAD_PROCESS_PRIVATE);
    rc |= pthread_spin_lock(&g_Spin);
    rc |= pthread_spin_trylock(&g_Spin);
    rc |= pthread_spin_unlock(&g_Spin);
    rc |= pthread_spin_destroy(&g_Spin);

    /* barrier */
    rc |= pthread_barrier_init(&g_Barrier, NULL, 2U);
    rc |= (pthread_barrier_wait(&g_Barrier) == PTHREAD_BARRIER_SERIAL_THREAD);
    rc |= pthread_barrier_destroy(&g_Barrier);

    /* once and thread-specific data */
    rc |= pthread_once(&g_Once, InitOnce);
    rc |= pthread_key_create(&g_Key, KeyDestructor);
    rc |= pthread_setspecific(g_Key, &rc);
    rc |= (pthread_getspecific(g_Key) != NULL);
    rc |= pthread_key_delete(g_Key);
    rc |= (int)PTHREAD_KEYS_MAX;

    /* errno values the shim returns must come from <errno.h> */
    rc |= (EINVAL | EBUSY | EAGAIN | EDEADLK | EPERM | ETIMEDOUT | ENOTSUP | ENOMEM);

    pthread_exit(NULL);

    return rc;
}
