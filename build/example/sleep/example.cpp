/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk.h>
#include "example.h"

using namespace bsp;

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

template <stk::EAccessMode _AccessMode>
class MyTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    MyTask(uint8_t task_id) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        // task 0: sleep 1000 ms
        // task 1: sleep 2000 ms
        // task 2: sleep 3000 ms
        // task 3: sleep 4000 ms
        stk::Sleep(1000 * (m_task_id + 1));

        bsp::Led::SwitchOnExclusive(static_cast<Led::Id>(m_task_id));
    }
};

static stk::Kernel<stk::KERNEL_DYNAMIC | (STK_TICKLESS_IDLE ? stk::KERNEL_TICKLESS : 0), 4,
        stk::SwitchStrategyRR, stk::PlatformDefault> g_Kernel;

// note: using ACCESS_PRIVILEGED as some MCUs may not allow writing to GPIO from a user thread, such as i.MX RT1050 (Arm Cortex-M7)
static MyTask<stk::ACCESS_PRIVILEGED> g_Task1(0), g_Task2(1), g_Task3(2), g_Task4(3);

static void RunCycle()
{
    g_Kernel.AddTask(&g_Task1);
    g_Kernel.AddTask(&g_Task2);
    g_Kernel.AddTask(&g_Task3);
    g_Kernel.AddTask(&g_Task4);

    g_Kernel.Start();
}

void RunExample()
{
    Led::InitAll(false);

    g_Kernel.Initialize();

    for (int i = 0; i < 4; ++i)
    {
        RunCycle();
    }

    // switch on all leds
    Led::InitAll(true);

    while (true) {}
}
