/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Ported to SuperTinyKernel(TM) (STK) native API.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// NOTE: this file was previously named cyw43_arch_stk.c. It must be a .cpp file: unlike
// some of pico_async_context's other backends, pico/async_context_stk.h and
// async_context_stk_config_t are C++-only (they hold stk::IKernel*, stk::time::TimerHost*,
// stk::Weight, stk::Word* members), so a plain C translation unit cannot include it.

#if PICO_CYW43_ARCH_STK

#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "pico/cyw43_arch_stk.h"
#include "pico/cyw43_arch/arch_stk.h"

#if CYW43_LWIP
#include "pico/lwip_stk.h"
#include <lwip/tcpip.h>
#endif

#if CYW43_ENABLE_BLUETOOTH
#include "pico/btstack_cyw43.h"
#endif

#if NO_SYS
#error pico_cyw43_arch_stk requires NO_SYS=0
#endif

static async_context_stk_t s_cyw43_async_context_stk;

// STK has no dynamic task-creation path (see async_context_stk.h): the caller always owns the
// task's stack storage, so there is no build configuration under which this array would *not*
// be needed; CYW43_NO_DEFAULT_TASK_STACK is the only opt-out, for callers who want to supply
// their own config.task_stack instead.
#if !CYW43_NO_DEFAULT_TASK_STACK
static stk::Word s_cyw43_async_context_stk_task_stack[CYW43_TASK_STACK_SIZE];
#endif

// Set by cyw43_arch_stk_preinit() (see pico/cyw43_arch_stk.h). STK has no global registry for
// either a kernel or a TimerHost, and doesn't lazily create private ones under the hood, so
// cyw43_arch_init_default_async_context() can't discover them on its own; the application must
// hand them in once, before calling cyw43_arch_init().
static stk::IKernel         *s_cyw43_arch_stk_kernel     = nullptr;
static stk::time::TimerHost *s_cyw43_arch_stk_timer_host = nullptr;

void cyw43_arch_stk_preinit(stk::IKernel *kernel, stk::time::TimerHost *timer_host)
{
    s_cyw43_arch_stk_kernel     = kernel;
    s_cyw43_arch_stk_timer_host = timer_host;
}

async_context_t *cyw43_arch_init_default_async_context(void)
{
    // If these fire, cyw43_arch_stk_preinit() was not called (with an already-Initialize()'d
    // kernel and a TimerHost Initialize()'d against that same kernel) before cyw43_arch_init().
    hard_assert(s_cyw43_arch_stk_kernel != nullptr);
    hard_assert(s_cyw43_arch_stk_timer_host != nullptr);

    async_context_stk_config_t config = async_context_stk_default_config();
    config.kernel     = s_cyw43_arch_stk_kernel;
    config.timer_host = s_cyw43_arch_stk_timer_host;
#ifdef CYW43_TASK_PRIORITY
    config.task_weight = CYW43_TASK_PRIORITY;   // config field is task_weight, not task_priority
#endif
#ifdef CYW43_TASK_STACK_SIZE
    config.task_stack_size = CYW43_TASK_STACK_SIZE;
#endif
#if !CYW43_NO_DEFAULT_TASK_STACK
    config.task_stack = s_cyw43_async_context_stk_task_stack;
#endif
    if (async_context_stk_init(&s_cyw43_async_context_stk, &config))
    { return &s_cyw43_async_context_stk.core; }
    return NULL;
}

int cyw43_arch_init(void)
{
    async_context_t *context = cyw43_arch_async_context();
    if (!context)
    {
        context = cyw43_arch_init_default_async_context();
        if (!context) { return PICO_ERROR_GENERIC; }
        cyw43_arch_set_async_context(context);
    }
    bool ok = cyw43_driver_init(context);
#if CYW43_LWIP
    ok &= lwip_stk_init(context);
#endif
#if CYW43_ENABLE_BLUETOOTH
    ok &= btstack_cyw43_init(context);
#endif
    if (!ok)
    {
        cyw43_arch_deinit();
        return PICO_ERROR_GENERIC;
    }
    else
    {
        return 0;
    }
}

void cyw43_arch_deinit(void)
{
    async_context_t *context = cyw43_arch_async_context();
#if CYW43_ENABLE_BLUETOOTH
    btstack_cyw43_deinit(context);
#endif
    // there is a bit of a circular dependency here between lwIP and cyw43_driver. We
    // shut down cyw43_driver first as it has IRQs calling back into lwIP. Also lwIP itself
    // does not actually get shut down.
    // todo add a "pause" method to async_context if we need to provide some atomicity (we
    // don't want to take the lock as these methods may invoke execute_sync())
    cyw43_driver_deinit(context);
#if CYW43_LWIP
    lwip_stk_deinit(context);
#endif
    // if it is our context, then we de-init it.
    if (context == &s_cyw43_async_context_stk.core)
    {
        async_context_deinit(context);
        cyw43_arch_set_async_context(NULL);
    }
}

#endif
