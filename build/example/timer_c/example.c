/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <stk_c.h>
#include <stk_c_time.h>
#include "example.h"

// ---------------------------------------------------------------------------
// LED timer callback — toggles green LED every 1 s
// ---------------------------------------------------------------------------

static bool s_LedToggle = false;

static void on_led_timer_expired(stk_timerhost_t *host,
                                 stk_timer_t     *timer,
                                 void            *user_data)
{
    (void)host;
    (void)timer;
    (void)user_data;

    s_LedToggle = !s_LedToggle;
    Led_Set(LED_GREEN, s_LedToggle);
}

// ---------------------------------------------------------------------------
// Shutdown timer callback — stops the timer host after 20 s
// ---------------------------------------------------------------------------

static void on_shutdown_timer_expired(stk_timerhost_t *host,
                                      stk_timer_t     *timer,
                                      void            *user_data)
{
    (void)timer;
    (void)user_data;

    Led_InitAll(true);

    stk_timerhost_shutdown(host);
}

// ---------------------------------------------------------------------------
// RunExample
// ---------------------------------------------------------------------------

void RunExample(void)
{
    Led_InitAll(false);

    // Create kernel for core 0
    stk_kernel_t *kernel = stk_kernel_create(0);
    stk_kernel_init(kernel, STK_PERIODICITY_DEFAULT);

    // Obtain timer host for core 0 and initialize it
    stk_timerhost_t *timers = stk_timerhost_get(0);
    stk_timerhost_init(timers, kernel, /*privileged=*/true);

    // Helper: convert milliseconds to ticks using current tick resolution
    int32_t res = stk_tick_resolution(); /* microseconds per tick */

    // Create and start LED timer — periodic, fires every 1 s
    stk_timer_t *led_timer = stk_timer_create(on_led_timer_expired, NULL);
    stk_timer_start(timers, led_timer,
                    0,                                          /* no initial delay */
                    (uint32_t)stk_ticks_from_ms_r(1000, res));  /* 1000 ms -> ticks  */

    // Create and start shutdown timer — one-shot, fires once after 20 s
    stk_timer_t *shutdown_timer = stk_timer_create(on_shutdown_timer_expired, NULL);
    stk_timer_start(timers, shutdown_timer,
                    (uint32_t)stk_ticks_from_ms_r(20000, res),  /* 20000 ms -> ticks */
                    0);                                         /* one-shot */

    // Start the kernel — never returns
    stk_kernel_start(kernel);

    Led_InitAll(false);

    for (;;) {}
}
