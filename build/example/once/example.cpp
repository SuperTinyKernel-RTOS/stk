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

static volatile uint8_t g_TaskSwitch = 0;

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
    void Run()
    {
        uint8_t task_id = m_task_id;

        while (true)
        {
            if (g_TaskSwitch != task_id)
            {
                stk::Sleep(100);
                continue;
            }

            Led::SwitchOnExclusive(static_cast<LedId>(task_id));

            stk::Sleep(1000);

            g_TaskSwitch = (task_id + 1) % LED_MAX;
            return;
        }
    }
};

static stk::Kernel<stk::KERNEL_DYNAMIC, 4, stk::SwitchStrategyRoundRobin, stk::PlatformDefault> g_Kernel;

// note: using ACCESS_PRIVILEGED as some MCUs may not allow writing to GPIO from a user thread, such as i.MX RT1050 (Arm Cortex-M7)
static MyTask<stk::ACCESS_PRIVILEGED>
    g_Task1(Led::RED),
    g_Task2(Led::ORANGE),
    g_Task3(Led::GREEN),
    g_Task4(Led::BLUE);

static void RunOnce()
{
    g_Kernel.AddTask(&g_Task1);
    g_Kernel.AddTask(&g_Task2);
    g_Kernel.AddTask(&g_Task3);
    g_Kernel.AddTask(&g_Task4);

    g_TaskSwitch = 0;

    // note: kernel will exit from Start() once all tasks exit (complete their work)
    g_Kernel.Start();
}

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    g_Kernel.Initialize();

    // repeat 3 times
    for (int32_t i = 0; i < 3; ++i)
    {
        RunOnce();
    }

    // switched on all LEDs when execution ends
    Led::InitAll(true);

    while (true) {}
}
