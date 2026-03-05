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
#include "example.h"

static volatile uint8_t g_TaskSwitch = 0;

// It is a simple TLS, you can host a high-level implementation instead with access by index
struct MyTls
{
    uint8_t task_id;
};

static void InitLEDs()
{
    Led::Init(Led::RED, false);
    Led::Init(Led::GREEN, false);
    Led::Init(Led::BLUE, false);
}

// Task function switching the LED, TLS provides the ID of the task for the logic of this function
static void SwitchOnLED()
{
    // for demonstration purpose we get task_id from our TLS and switch on corresponding LED
    uint8_t task_id = stk::hw::GetTlsPtr<MyTls>()->task_id;

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

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// Task's core (thread)
template <uint8_t _TaskId, stk::EAccessMode _AccessMode>
class MyTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    MyTls m_tls; // task-local TLS, you can provide your own implementation

public:
    MyTask()
    {
        m_tls.task_id = _TaskId;
    }

private:
    void Run()
    {
        // set your TLS (it can host any complex implementation of your choice)
        stk::hw::SetTlsPtr(&m_tls);

        // just fake counters to demonstrate that scheduler is saving/restoring context correctly
        // preserving values of floating-point and 64 bit variables
        volatile float count = 0;
        volatile uint64_t count_skip = 0;

        while (true)
        {
            if (g_TaskSwitch != _TaskId)
            {
                // to avoid hot loop and excessive CPU usage sleep 10ms while waiting for the own turn,
                // if scheduler does not have active threads then it will fall into a sleep mode which
                // saving the consumed power
                stk::Sleep(10);

                ++count_skip;
                continue;
            }

            ++count;

            SwitchOnLED();

            // sleep 1s and delegate work to another task switching another LED
            stk::Sleep(1000);
            g_TaskSwitch = (_TaskId + 1) % 3;
        }
    }
};

void RunExample()
{
    using namespace stk;

    InitLEDs();

    // allocate scheduling kernel for 3 threads (tasks) with Round-robin scheduling strategy
    static Kernel<KERNEL_STATIC, 3, SwitchStrategyRoundRobin, PlatformDefault> kernel;

    // note: using ACCESS_PRIVILEGED as some MCUs may not allow writing to GPIO from a user thread, such as i.MX RT1050 (Arm Cortex-M7)
    static MyTask<0, ACCESS_PRIVILEGED> task1;
    static MyTask<1, ACCESS_PRIVILEGED> task2;
    static MyTask<2, ACCESS_PRIVILEGED> task3;

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
