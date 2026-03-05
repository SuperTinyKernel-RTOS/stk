/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "cmsis_device.h"
#include "perf.h"

static volatile int64_t g_Ticks = 0;
static volatile bool g_Enable = false;

extern "C" void SysTick_Handler()
{
    //HAL_IncTick();

    if (g_Enable)
        ++g_Ticks;
}

template <int32_t _Count> struct Processor
{
    // processing unrolled _Count number of times
    static void Process()
    {
        g_Bench[_Count - 1].Process();
        Processor<_Count - 1>::Process();
    }
};

template <> struct Processor<0>
{
    static void Process() {}
};

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // periodicity: 1 ms
    SysTick_Config((uint32_t)((int64_t)SystemCoreClock * 1000 / 1000000));

    // warm up
    for (volatile int32_t i = 1000000; i > 0; i--) {}

    // do processing
    g_Enable = true;
    while (g_Ticks < _STK_BENCH_WINDOW)
    {
        Processor<_STK_BENCH_TASK_MAX>::Process();
    }

    Crc32Bench::ShowResults();

    for (;;);
    return 0;
}

