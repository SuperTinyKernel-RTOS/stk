/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include "perf.h"

using namespace stk;

#define SLEEP_GRANULARITY (_STK_BENCH_WINDOW + 2)

static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_BENCH_TASK_MAX + 1, SwitchStrategyRR, PlatformDefault> g_Kernel;
static volatile uint32_t g_Ticks = 0;
static volatile bool g_Enable = false;

extern "C" void SysTick_Handler()
{
    //HAL_IncTick();

    if (g_Enable)
        ++g_Ticks;

    if (g_Kernel.GetState() == stk::IKernel::STATE_RUNNING)
        g_Kernel.GetPlatform()->ProcessTick();
}

class BenchTask : public Task<_STK_BENCH_STACK_SIZE, ACCESS_PRIVILEGED>
{
public:
    BenchTask() : m_id(~0), m_exited(false) {}

    void Initialize(uint8_t id) { m_id = id; }
    bool IsExited() const { return m_exited; }

private:
    void Run()
    {
        uint32_t index = m_id;

        // do processing
        g_Enable = true;
        while (g_Ticks < _STK_BENCH_WINDOW)
        {
            g_Bench[index].Process();
        }

        m_exited = true;
    }

    uint8_t       m_id;
    volatile bool m_exited;
};

static BenchTask g_Tasks[_STK_BENCH_TASK_MAX];

class ResultTask : public Task<_STK_BENCH_STACK_SIZE, ACCESS_PRIVILEGED>
{
private:
    void Run()
    {
        while (g_Ticks < _STK_BENCH_WINDOW + 2)
        {
            stk::Sleep(SLEEP_GRANULARITY);
        }

    wait:
        for (int32_t i = 0; i < _STK_BENCH_TASK_MAX; ++i)
        {
            if (!g_Tasks[i].IsExited())
                goto wait;
        }

        Crc32Bench::ShowResults();
    }
};

static ResultTask g_TaskResult;

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_Kernel.Initialize();

    for (int32_t i = 0; i < _STK_BENCH_TASK_MAX; ++i)
    {
        g_Bench[i].Initialize();
        g_Tasks[i].Initialize(i);
        g_Kernel.AddTask(&g_Tasks[i]);
    }

    g_Kernel.AddTask(&g_TaskResult);

    g_Kernel.Start();
    for (;;);
    return 0;
}

