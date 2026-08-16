/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk.h>
#include <sync/stk_sync.h>
#include <time/stk_time.h>
#include <arch/arm/cortex-m/stk_arch_arm-tz.h> // for Secure/Non-Secure-specific TrustZone API

#include "example.h"

#if (__ARM_FEATURE_CMSE & 1) == 0
#error "Need ARMv8-M security extensions"
#elif (__ARM_FEATURE_CMSE & 2) != 0
#error "Compile without --mcmse"
#endif

#include "pico/unique_id.h"

using namespace bsp;

void NSC_OnExitNs(void);
uint32_t NSC_GetKey(uint8_t key[], uint32_t size);
void NSC_GetBoardUID(pico_unique_board_id_t *id_out);
void NSC_bsp_Led_SwitchOnExclusive(bsp::Led::Id led);

extern "C" void runtime_init_clocks(void)
{
    // clocks are initialized on Secure side of the binary
}

void pico_get_unique_board_id(pico_unique_board_id_t *id_out)
{
    // you can get Id via NSC call, if needed for a Non-Secure side of the binary
    //*id_out = pico_unique_board_id_t{};

    NSC_GetBoardUID(id_out);
}

// Size of the task's stack (number of stk::Word)
// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
static constexpr size_t TASK_STACK_SIZE = 1024U;
#else
static constexpr size_t TASK_STACK_SIZE = 256U;
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
        // we switch LEDs with 250ms period
        const stk::Ticks period = stk::GetTicksFromMs(250);

        // get a start of the timeline
        g_Timeline = stk::GetTicks();

        while (true)
        {
            // block until this task's flag is set; auto-cleared on return
            const uint32_t result = g_TaskFlags.Wait(m_my_flag, stk::sync::EventFlags::OPT_WAIT_ANY);
            if (stk::sync::EventFlags::IsError(result))
                continue;

            // change active LED
            {
                const stk::hw::CriticalSection::ScopedLock __guard;
                NSC_bsp_Led_SwitchOnExclusive(static_cast<LedId>(m_task_id));
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

    uint8_t key[4] = {0};
    NSC_GetKey(key, 4);

    // Note: using ACCESS_PRIVILEGED as Cortex-M3+ may not allow writing to GPIO from a less secure user thread.
    static MyTask<ACCESS_USER> task1(LED_RED);
    static MyTask<ACCESS_USER> task2(LED_ORANGE);
    static MyTask<ACCESS_USER> task3(LED_GREEN); // you can set to ACCESS_PRIVILEGED too, see NSC_bsp_Led_SwitchOnExclusive on secure side, but for smooth performance on RP2350 let background thread handle GPIO
    static MyTask<ACCESS_USER> task4(LED_BLUE);

    static tz::nsec::Kernel kernel;

    kernel.Initialize(0);

    // Register threads (tasks).
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.AddTask(&task4);

    // Start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line.
    kernel.Start();

    // Return back to Secure binary. Note: kernel.Start() will exit only if Secure side initialized Kernel instance
    // with KERNEL_DYNAMIC mode and when all tasks exited on both sides.
    NSC_OnExitNs();
}
