/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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

// Generic LED-blink task
template <int32_t _Weight, stk::EAccessMode _AccessMode>
class LedTask : public stk::TaskW<_Weight, TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_led_id;
public:
    LedTask(uint8_t id) : m_led_id(id)
    {}

private:
    void Run() override
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
                stk::hw::ScopedCriticalSection __cs;

                Led::Set(static_cast<LedId>(m_led_id), led_state);
            }

            led_state = !led_state;

            // SWRR, unlike with fixed-priority SwitchStrategyFP32, does not require tasks cooperation with
            // Sleep() or Yield(), all tasks will get their CPU time slice, even tasks with lowest priority
            //stk::Sleep(10);
        }
    }
};

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // 3 tasks kernel with Smooth Weighted Round-Robin scheduling strategy
    static Kernel<KERNEL_STATIC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), 4,
            SwitchStrategySWRR, PlatformDefault> kernel;

    // RED is lowest priority (1), and BLUE is highest (69):
    // - BLUE gets more CPU time and will blink very often
    // - GRREN blinks less often than BLUE
    // - ORANGE is blinking less often than GREEN
    // - RED is least blinking as it gets gets the least CPU time
    // note: if you set the same priority for tasks LEDs of these tasks will blink equally
    static LedTask<1, ACCESS_PRIVILEGED> task_red(Led::RED);
    static LedTask<10, ACCESS_PRIVILEGED> task_org(Led::ORANGE);
    static LedTask<20, ACCESS_PRIVILEGED> task_green(Led::GREEN);
    static LedTask<69, ACCESS_PRIVILEGED> task_blue(Led::BLUE);

    kernel.Initialize();

    kernel.AddTask(&task_red);
    kernel.AddTask(&task_org);
    kernel.AddTask(&task_green);
    kernel.AddTask(&task_blue);

    // start scheduling (blocks forever)
    kernel.Start();
    STK_ASSERT(false);
}
