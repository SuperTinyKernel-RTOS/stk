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
#include <sync/stk_sync_eventflags.h>
#include "example.h"

using namespace bsp;

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// One flag bit per LED task; task 0 (RED) goes first
static const uint32_t FLAGS_ALL[] = {
    (1U << LED_RED),
    (1U << LED_ORANGE),
    (1U << LED_GREEN),
    (1U << LED_BLUE)
};

// Start with the RED task's flag set so it runs first
static stk::sync::EventFlags g_TaskFlags(FLAGS_ALL[LED_RED]);

// Timeline for a precise LED switching
static stk::Ticks g_Timeline = 0;

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class MyTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t  m_task_id;
    uint32_t m_my_flag;
    uint32_t m_next_flag;

public:
    MyTask(uint8_t task_id) : m_task_id(task_id), m_my_flag(FLAGS_ALL[task_id]),
          m_next_flag(FLAGS_ALL[(task_id + 1) % LED_MAX])
    {}

private:
    void Run() override
    {
        // we switch LEDs with 1s period
        const stk::Timeout period = stk::GetTicksFromMs(1000);

        // get a start of the timeline
        g_Timeline = stk::GetTicks();

        while (true)
        {
            // block until this task's flag is set; auto-cleared on return
            uint32_t result = g_TaskFlags.Wait(m_my_flag, stk::sync::EventFlags::OPT_WAIT_ANY);
            if (stk::sync::EventFlags::IsError(result))
                continue;

            // change active LED
            {
                stk::hw::CriticalSection::ScopedLock __guard;
                Led::SwitchOnExclusive(static_cast<LedId>(m_task_id));
            }

            // sleep 1s drift-free and then delegate work to the next task
            // we could use simple stk::Sleep() but due to other work around Sleep call we
            // will get a time drift, STK allows to sleep until exact timestamp making it
            // possible precise sleeping with 1 tick precision, you could also use
            // time::TimerHost for timer-related tasks (see related 'timer' example)
            stk::SleepUntil(g_Timeline += period);

            // hand off to the next task
            g_TaskFlags.Set(m_next_flag);
        }
    }
};

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // operating in Static + Sync mode (EventFlags requires KERNEL_SYNC) and optionally tickless
    const uint8_t KernelMode = KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // allocate scheduling kernel for 3 threads (tasks) with Round-Robin scheduling strategy
    static Kernel<KernelMode, 4, SwitchStrategyRR, PlatformDefault> kernel;

    // note: using ACCESS_PRIVILEGED as Cortex-M3+ may not allow writing to GPIO from a less secure user thread
    static MyTask<ACCESS_PRIVILEGED> task1(LED_RED);
    static MyTask<ACCESS_PRIVILEGED> task2(LED_ORANGE);
    static MyTask<ACCESS_PRIVILEGED> task3(LED_GREEN);
    static MyTask<ACCESS_PRIVILEGED> task4(LED_BLUE);

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.AddTask(&task4);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
