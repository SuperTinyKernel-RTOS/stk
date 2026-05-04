/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk.h>
#include <time/stk_time.h>
#include "example.h"

// for PipeT which is used as a non-blocking data pipe between Hard Real-Time (HRT) tasks which
// should not block to avoid missing the deadline
#include <sync/stk_sync_pipe.h>

using namespace bsp;

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

#define USE_EDF 0

// One single-slot pipe per LED task. CtrlTask writes a command token; the
// corresponding HwLedTask drains it with TryRead (non-blocking, HRT-safe).
// Capacity = 1: CtrlTask produces at most one pending command per period.
static stk::sync::PipeT<uint8_t, 1> g_LedPipe[LED_MAX];

template <stk::EAccessMode _AccessMode>
class HwLedTask final : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    HwLedTask(uint8_t task_id) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        for (;;)
        {
            // activation token here but could be used as a useful command
            uint8_t cmd;

            // TryRead (NO_WAIT): HRT tasks must never block — poll and proceed
            if (g_LedPipe[m_task_id].TryRead(cmd))
            {
                // switch on corresponding LED
                Led::SwitchOnExclusive(static_cast<LedId>(cmd));
            }

            // do some other work in the task
            for (volatile uint32_t i = 0; i < 1000; ++i);

            stk::Yield();
        }
    }

    void OnDeadlineMissed(uint32_t duration)
    {
        (void)duration;
    }
};

template <stk::EAccessMode _AccessMode>
class CtrlTask final : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    void Run() override
    {
        uint8_t led = 0;
        stk::time::PeriodicTrigger trigger(1000, true);

        for (;;)
        {
            // do some other work in the task
            for (volatile uint32_t i = 0; i < 3000; ++i);

            if (trigger.Poll())
            {
                led = (led + 1) % LED_MAX;

                // TryWrite: sole producer into a depth-1 pipe; pipe is always
                // empty here by design, so this never blocks
                g_LedPipe[led].TryWrite(led);
            }

            // wait for a next turn
            stk::Yield();
        }
    }

    void OnDeadlineMissed(uint32_t duration)
    {
        // actual duration of missed task
        (void)duration;
    }
};

// optional: you can override sleep and hard fault default behaviors
class PlatformEventHandler final : public stk::IPlatform::IEventOverrider
{
    bool OnHardFault() override
    {
        // switch on Red LED as indication of the error
        Led::SwitchOnExclusive(Led::RED);

        // if handled inside this function then return true, otherwise event will be handled by the driver
        // note: prior a call to this function a task which had deadline missed had a call to OnDeadlineMissed
        return false;
    }
};

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    enum { TASK_COUNT = 5 };

#if USE_EDF
    typedef SwitchStrategyEDF SchedulerType;
#else
    typedef SwitchStrategyRM SchedulerType;
#endif

    static Kernel<KERNEL_STATIC | KERNEL_HRT, TASK_COUNT, SchedulerType, PlatformDefault> kernel;

    // assume that hardware LED tasks have highest priority
    static HwLedTask<ACCESS_PRIVILEGED> hwt0(0), hwt1(1), hwt2(2), hwt3(3);

    // control task is sending commands to hardware tasks
    static CtrlTask<ACCESS_USER> ctrl;

    kernel.Initialize();

    // optional: you can override sleep and hard fault default behaviors
    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

#define MSEC(MS) GetTicksFromMs(MS, PERIODICITY_DEFAULT)

    //                    periodicity  deadline   start delay
    kernel.AddTask(&ctrl, MSEC(200),   MSEC(100), MSEC(0));
    kernel.AddTask(&hwt0, MSEC(200),   MSEC(20), MSEC(0));
    kernel.AddTask(&hwt1, MSEC(200),   MSEC(20), MSEC(0));
    kernel.AddTask(&hwt2, MSEC(200),   MSEC(20), MSEC(0));
    kernel.AddTask(&hwt3, MSEC(200),   MSEC(20), MSEC(0));

#if !USE_EDF
    auto wcrt_sched = SchedulabilityCheck::IsSchedulableWCRT<TASK_COUNT>(kernel.GetSwitchStrategy());
    STK_ASSERT(wcrt_sched == true);
#endif

    kernel.Start();

    // shall not reach here
    STK_ASSERT(false);
}

