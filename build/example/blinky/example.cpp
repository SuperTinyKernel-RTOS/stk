/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include "example.h"

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

static volatile uint8_t g_TaskSwitch = 0;

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class MyTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    const char *m_name;

public:
    MyTask(uint8_t task_id, const char *name) : m_task_id(task_id), m_name(name)
    {}

    size_t GetId() const  { return m_task_id; }
    const char *GetName() const  { return m_name; }

private:
    void Run()
    {
        uint8_t task_id = m_task_id;

        while (true)
        {
            if (g_TaskSwitch != task_id)
            {
                // to avoid hot loop and excessive CPU usage sleep 10ms while waiting for the own turn,
                // if scheduler does not have active threads then it will fall into a sleep mode which is
                // saving the consumed power
                stk::Sleep(10);
                continue;
            }

            {
                stk::hw::CriticalSection::ScopedLock __guard;

                SwitchOnLED(task_id);
            }

            // sleep 1s and delegate work to another task switching another LED
            stk::Sleep(1000);
            g_TaskSwitch = (task_id + 1) % 3;
        }
    }

    static void SwitchOnLED(uint8_t task_id)
    {
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
    }
};

static void InitLeds()
{
    Led::Init(Led::RED, false);
    Led::Init(Led::GREEN, false);
    Led::Init(Led::BLUE, false);
}

void RunExample()
{
    using namespace stk;

    InitLeds();

    // allocate scheduling kernel for 3 threads (tasks) with Round-Robin scheduling strategy
    static Kernel<KERNEL_STATIC, 3, SwitchStrategyRR, PlatformDefault> kernel;

    // note: using ACCESS_PRIVILEGED as Cortex-M3+ may not allow writing to GPIO from a less secure user thread
    static MyTask<ACCESS_PRIVILEGED> task1(0, "LED-red");
    static MyTask<ACCESS_PRIVILEGED> task2(1, "LED-grn");
    static MyTask<ACCESS_PRIVILEGED> task3(2, "LED-blu");

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
