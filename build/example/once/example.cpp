/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include "example.h"

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

        volatile float count = 0;
        volatile uint64_t count_skip = 0;

        while (true)
        {
            if (g_TaskSwitch != task_id)
            {
                ++count_skip;
                continue;
            }

            ++count;

            switch (task_id)
            {
            case 0:
                Led::Set(Led::RED, true);
                Led::Set(Led::GREEN, false);
                Led::Set(Led::BLUE, false);
                break;
            case 1:
                Led::Set(Led::RED, false);
                Led::Set(Led::GREEN, true);
                Led::Set(Led::BLUE, false);
                break;
            case 2:
                Led::Set(Led::RED, false);
                Led::Set(Led::GREEN, false);
                Led::Set(Led::BLUE, true);
                break;
            }

            stk::Sleep(1000);

            g_TaskSwitch = (task_id + 1) % 3;
            return;
        }
    }
};

static void InitLeds()
{
    Led::Init(Led::RED, false);
    Led::Init(Led::GREEN, false);
    Led::Init(Led::BLUE, false);
}

static stk::Kernel<stk::KERNEL_DYNAMIC, 3, stk::SwitchStrategyRoundRobin, stk::PlatformDefault> g_Kernel;

// note: using ACCESS_PRIVILEGED as some MCUs may not allow writing to GPIO from a user thread, such as i.MX RT1050 (Arm Cortex-M7)
static MyTask<stk::ACCESS_PRIVILEGED> g_Task1(0), g_Task2(1), g_Task3(2);

static void RunOnce()
{
    g_Kernel.AddTask(&g_Task1);
    g_Kernel.AddTask(&g_Task2);
    g_Kernel.AddTask(&g_Task3);

    g_TaskSwitch = 0;

    // note: kernel will exit from Start() once all tasks exit (complete their work)
    g_Kernel.Start();
}

void RunExample()
{
    using namespace stk;

    InitLeds();

    g_Kernel.Initialize();

    // repeat 3 times
    for (int i = 0; i < 3; ++i)
    {
        RunOnce();
    }

    // switched on all LEDs when execution ends
    Led::Set(Led::RED, true);
    Led::Set(Led::GREEN, true);
    Led::Set(Led::BLUE, true);

    while (true) {}
}
