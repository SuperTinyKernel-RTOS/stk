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

static void InitLeds()
{
    Led::Init(Led::RED,   false);
    Led::Init(Led::GREEN, false);
    Led::Init(Led::BLUE,  false);
}

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// Generic LED-blink task
template <int32_t _Priority, stk::EAccessMode _AccessMode>
class LedTask : public stk::TaskW<_Priority, TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_led_id;
public:
    LedTask(uint8_t id) : m_led_id(id)
    {}

    stk::RunFuncType GetFunc() {
        return [](void *p) {
            static_cast<LedTask *>(p)->Run();
        };
    }
    void *GetFuncUserData() override { return this; }

private:
    void Run()
    {
        bool led_state = false;

        while (true)
        {
            // do some busy work to create CPU load, this ensures tasks are always ready to run (not sleeping)
            // due to load higher priority tasks will preempt lower priority ones
            for (volatile uint32_t i = 0; i < 300000; i++)
            {}

            // toggle LED for this task
            {
                // protect from preemption during hardware IO
                stk::hw::CriticalSection::ScopedLock __cs;

                Led::Set((Led::Id)m_led_id, led_state);
            }

            led_state = !led_state;

            // SWRR, unlike Fixed-Priority (FP), does not require tasks cooperation with Sleep() or Yield(), all
            // tasks will get their CPU time slice, even tasks with lowest priority (see task_red in this example).
            //stk::Sleep(10);
        }
    }
};

void RunExample()
{
    using namespace stk;

    InitLeds();

    // 3 tasks kernel with Smooth Weighted Round-Robin scheduling strategy
    static Kernel<KERNEL_STATIC, 3, SwitchStrategySWRR, PlatformDefault> kernel;

    // RED is lowest priority (1), and BLUE is highest (87):
    // - BLUE gets more CPU time and will blink very often
    // - GRREN blinks less often than BLUE
    // - RED is least blinking as it gets gets the least CPU time
    // note: if you set the same priority for tasks LEDs of these tasks will blink equally
    static LedTask<5, ACCESS_PRIVILEGED> task_red(Led::RED);
    static LedTask<20, ACCESS_PRIVILEGED> task_green(Led::GREEN);
    static LedTask<75, ACCESS_PRIVILEGED> task_blue(Led::BLUE);

    kernel.Initialize();

    kernel.AddTask(&task_red);
    kernel.AddTask(&task_green);
    kernel.AddTask(&task_blue);

    // start scheduling (blocks forever)
    kernel.Start();
    STK_ASSERT(false);
}
