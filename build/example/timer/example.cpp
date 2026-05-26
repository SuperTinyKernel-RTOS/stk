/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

//#define _STK_ASSERT_REDIRECT

#include <stk.h>
#include <time/stk_time.h>
#include "example.h"

#ifdef _STK_ASSERT_REDIRECT
#include <stdint.h>
extern void STK_ASSERT_HANDLER(const char *err, const char *source, int32_t line);
#endif

using namespace bsp;

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

#ifdef _STK_ASSERT_REDIRECT
void STK_ASSERT_HANDLER(const char *err, const char *source, int32_t line)
{
    __stk_debug_break();
    while (true) {}
}
#endif

static stk::time::TimerHost g_Timers;

// ---------------------------------------------------------------------------
// LED timer callback — toggles green LED every 100 ms
// ---------------------------------------------------------------------------

struct LedTimer : public stk::time::TimerHost::Timer
{
    uint8_t m_led;

    LedTimer() : m_led(0)
    {}

    void OnExpired(stk::time::TimerHost *host)
    {
        Led::SwitchOnExclusive(static_cast<Led::Id>(m_led));
        m_led = (m_led + 1) % LED_MAX;
    }
};

// ---------------------------------------------------------------------------
// Shutdown timer callback — stops the timer host after 20 s
// ---------------------------------------------------------------------------

struct ShutdownTimer : public stk::time::TimerHost::Timer
{
    void OnExpired(stk::time::TimerHost *host)
    {
        Led::InitAll(true);

        host->Shutdown();
    }
};

// ---------------------------------------------------------------------------
// RunExample
// ---------------------------------------------------------------------------

void RunExample()
{
    Led::InitAll(false);

    static stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC | (STK_TICKLESS_IDLE ? stk::KERNEL_TICKLESS : 0),
            stk::time::TimerHost::TASK_COUNT, stk::SwitchStrategyRR, stk::PlatformDefault> g_Kernel;

    static LedTimer      LedTimer;
    static ShutdownTimer ShutdownTimer;

    g_Kernel.Initialize();
    g_Timers.Initialize(&g_Kernel, stk::ACCESS_PRIVILEGED);
    g_Timers.Start(LedTimer, 0, stk::GetTicksFromMs(100));        // periodic timer, triggered every 100ms
    g_Timers.Start(ShutdownTimer, stk::GetTicksFromMs(20000), 0); // one-shot timer, triggered once in 20s
    g_Kernel.Start();

    Led::InitAll(false);

    for (;;) {}
}
