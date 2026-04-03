/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

//#define _STK_ASSERT_REDIRECT

#include <stk_config.h>
#include <stk.h>
#include <time/stk_time.h>
#include "example.h"

#ifdef _STK_ASSERT_REDIRECT
#include <stdint.h>
extern void STK_ASSERT_HANDLER(const char *err, const char *source, int32_t line);
#endif

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

struct LedTimer : public stk::time::TimerHost::Timer
{
    bool m_toggle;

    LedTimer() : m_toggle(false)
    {}

    void OnExpired(stk::time::TimerHost *host)
    {
        Led::Set(Led::GREEN, m_toggle = !m_toggle);
    }
};
static LedTimer g_LedTimer;

struct ShutdownTimer : public stk::time::TimerHost::Timer
{
    void OnExpired(stk::time::TimerHost *host)
    {
        host->Shutdown();
    }
};
static ShutdownTimer g_ShutdownTimer;

static stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC, stk::time::TimerHost::TASK_COUNT, stk::SwitchStrategyRR, stk::PlatformDefault> g_Kernel;

static void InitLeds()
{
    Led::Init(Led::RED, false);
    Led::Init(Led::GREEN, false);
    Led::Init(Led::BLUE, false);
}

void RunExample()
{
    InitLeds();

    g_Kernel.Initialize();
    g_Timers.Initialize(&g_Kernel, stk::ACCESS_PRIVILEGED);
    g_Timers.Start(g_LedTimer, 0, stk::GetTicksFromMs(1000)); // periodic timer, triggered every 1s
    g_Timers.Start(g_ShutdownTimer, stk::GetTicksFromMs(20000), 0); // one-shot timer, triggered once in 20s
    g_Kernel.Start();

    for (;;) {}
}
