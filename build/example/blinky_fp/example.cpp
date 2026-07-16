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

// Generic LED-blink task
template <int32_t _Priority, stk::EAccessMode _AccessMode>
class LedTask : public stk::TaskW<_Priority, TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_led_id;
public:
    LedTask(uint8_t id) : m_led_id(id)
    {}

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
                stk::hw::ScopedCriticalSection __cs;

                Led::Set((Led::Id)m_led_id, led_state);
            }

            led_state = !led_state;

            // with Fixed-Priority strategy tasks must cooperate by giving up processing with Sleep(), note
            // that Yield() may not be sufficient with >2 tasks because task with highest priority may
            // return in processing and task with lowest priority will never get its CPU time slice.
            // More robust scheduling strategy in this respect is  SwitchStrategySWRR which guarantees CPU
            // time slice for tasks with lowest priority (weight) and SWRR does not require tasks to
            // cooperate with Sleep() or Yield() which you can still use to achieve lower power consumption.
            stk::Sleep(10);
        }
    }
};

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // 3 tasks kernel with 3 priorities
    static Kernel<KERNEL_STATIC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), 4,
            SwitchStrategyFP32, PlatformDefault> kernel;

    // RED has lowest priority, and BLUE has highest:
    // - BLUE gets more CPU time and will blink very often
    // - GREEN blinks less often than BLUE
    // - ORANGE is blinking less often than GREEN
    // - RED is least blinking as it gets gets the least CPU time
    // note: if you set the same priority for tasks LEDs of these tasks will blink equally
    static LedTask<SwitchStrategyFP32::PRIORITY_LOWEST, ACCESS_PRIVILEGED> task_red(Led::RED);
    static LedTask<SwitchStrategyFP32::PRIORITY_LOWER, ACCESS_PRIVILEGED> task_org(Led::ORANGE);
    static LedTask<SwitchStrategyFP32::PRIORITY_NORMAL, ACCESS_PRIVILEGED> task_green(Led::GREEN);
    static LedTask<SwitchStrategyFP32::PRIORITY_HIGHEST, ACCESS_PRIVILEGED> task_blue(Led::BLUE);

    kernel.Initialize();

    kernel.AddTask(&task_red);
    kernel.AddTask(&task_org);
    kernel.AddTask(&task_green);
    kernel.AddTask(&task_blue);

    // start scheduling (blocks forever)
    kernel.Start();
    STK_ASSERT(false);
}
