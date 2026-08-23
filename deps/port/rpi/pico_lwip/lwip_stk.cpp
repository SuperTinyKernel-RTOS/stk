/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com> (STK port)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>

#include "pico/async_context.h"
#include "pico/time.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "lwip/debug.h"
#include "lwip/sys.h"

#include "pico/lwip_stk.h"

/* STK includes. */
#include "stk.h"
#include "sync/stk_sync.h"

#if NO_SYS
#error lwip_stk_async_context_bindings requires NO_SYS=0
#endif

/*
 * Binds lwIP's NO_SYS=0 tcpip thread to a Pico SDK async_context, on top of the STK sys_arch
 * port (sys_arch.h / sys_arch.cpp).
 *
 * This file is compiled as C++ (like sys_arch.cpp), so - unlike a plain-C integration - it can
 * construct stk::sync::Semaphore objects directly rather than going through sys_sem_new() /
 * sys_sem_free(). The two semaphores below (_tcpip_task_blocker, _init_sem) have lifetimes that
 * don't fit sys_arch.cpp's fixed-capacity semaphore pool well - one is effectively permanent for
 * the life of the program, the other is scoped to a single stack frame - so constructing them
 * directly avoids either permanently reserving, or repeatedly churning, a slot out of
 * sys_arch.cpp's LWIP_STK_SEM_POOL_SIZE for them.
 *
 * Each is still wrapped in a `sys_sem_t { .sem = &the_semaphore }` so this file can reuse
 * sys_arch.cpp's own sys_sem_signal() / sys_arch_sem_wait() for the actual wait/signal
 * operations rather than duplicating that logic here (in particular sys_arch_sem_wait()'s
 * timeout_ms == 0 -> "wait forever" convention). Only construction/destruction bypasses
 * sys_arch.cpp - sys_sem_new() / sys_sem_free() are never called for either of these two.
 *
 * For pico_lwip_custom_lock_tcpip_core() / pico_lwip_custom_unlock_tcpip_core() below to
 * actually become lwIP's tcpip core lock, sys_arch.cpp must be built with
 * LWIP_STK_CUSTOM_CORE_LOCKING=1 (see sys_arch.cpp), which makes its
 * sys_lock_tcpip_core() / sys_unlock_tcpip_core() forward to these two functions instead of
 * using an independent stk::sync::Mutex. That makes lwIP's core lock *be* the async_context's
 * own lock, so code already running inside an async_context callback doesn't deadlock taking
 * it again.
 */

static async_context_t * volatile s_lwip_context;

/* lwIP's tcpip_task cannot be shut down, so lwip_stk_deinit() just parks it here instead;
 * lwip_stk_init() re-signals it to resume on a later re-init. Static storage duration, so it
 * must outlive the whole program - constructed directly as a stk::sync::Semaphore rather than
 * via sys_sem_new() (see the file-level comment above for why). */
static stk::sync::Semaphore s_tcpip_task_blocker_inst(0U, SYS_SEM_MAX_COUNT);
static sys_sem_t s_tcpip_task_blocker = { .sem = &s_tcpip_task_blocker_inst };

static void tcpip_init_done(void *param)
{
    sys_sem_signal((sys_sem_t *)param);
}

extern "C" bool lwip_stk_init(async_context_t *context)
{
    assert(!s_lwip_context || s_lwip_context == context);
    static bool done_lwip_init;
    if (!done_lwip_init)
    {
        s_lwip_context = context;
        done_lwip_init = true;

        /* Scoped to this call: tcpip_init_done() (invoked on the newly-spawned tcpip thread)
         * signals it, and this function blocks right below until that happens - so it's safe
         * to keep on the stack even though its address is handed off to another thread. */
        stk::sync::Semaphore init_sem_inst(0U, SYS_SEM_MAX_COUNT);
        sys_sem_t init_sem = { .sem = &init_sem_inst };

        /* s_tcpip_task_blocker is initialized at file scope */
        assert(sys_sem_valid_val(s_tcpip_task_blocker));

        tcpip_init(tcpip_init_done, &init_sem);
        /* timeout_ms == 0 means "wait forever", per sys_arch_sem_wait()'s contract. */
        sys_arch_sem_wait(&init_sem, 0);

    }
    else
    {
        sys_sem_signal(&s_tcpip_task_blocker);
    }
    return true;
}

static uint32_t clear_lwip_context(__unused void *param)
{
    s_lwip_context = NULL;
    return 0;
}

extern "C" void lwip_stk_deinit(__unused async_context_t *context)
{
    // clear the lwip context under lock as lwIP may still be running in tcpip_task
    async_context_execute_sync(context, clear_lwip_context, NULL);
}

extern "C" void pico_lwip_custom_lock_tcpip_core(void)
{
    while (!s_lwip_context)
    {
        /* timeout_ms == 0 means "wait forever". */
        sys_arch_sem_wait(&s_tcpip_task_blocker, 0);
    }
    async_context_acquire_lock_blocking(s_lwip_context);
}

extern "C" void pico_lwip_custom_unlock_tcpip_core(void)
{
    async_context_release_lock(s_lwip_context);
}
