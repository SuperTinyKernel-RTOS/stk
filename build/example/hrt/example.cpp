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
#include <time/stk_time.h>
#include "example.h"

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

#define USE_EDF 1

static uint8_t g_Led = 0;

template <stk::EAccessMode _AccessMode>
class HwLedTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    HwLedTask(uint8_t task_id) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        for (;;)
        {
            if (g_Led == m_task_id)
            {
                g_Led = ~0;

                switch (m_task_id)
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

            stk::Yield();
        }
    }

    void OnDeadlineMissed(uint32_t duration)
    {
        (void)duration;
    }
};

template <stk::EAccessMode _AccessMode>
class CtrlTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
private:
    void Run()
    {
        uint8_t led = 0;
        stk::time::PeriodicTrigger trigger(1000, true);

        for (;;)
        {
            if (trigger.Poll())
            {
                led = (led + 1) % 3;
                g_Led = led;
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

static void InitLeds()
{
    Led::Init(Led::RED, false);
    Led::Init(Led::GREEN, false);
    Led::Init(Led::BLUE, false);
}

// optional: you can override sleep and hard fault default behaviors
class PlatformEventHandler : public stk::IPlatform::IEventOverrider
{
private:
    bool OnSleep()
    {
        // if handled inside this function then return true, otherwise event will be handled by the driver
        // note: if returned false once this function will not be called again until Kernel is re-started
        return false;
    }

    bool OnHardFault()
    {
        // switch on Red LED as indication of the error
        Led::Set(Led::RED, true);
        Led::Set(Led::GREEN, false);
        Led::Set(Led::BLUE, false);

        // if handled inside this function then return true, otherwise event will be handled by the driver
        // note: prior a call to this function a task which had deadline missed had a call to OnDeadlineMissed
        return false;
    }
};

void RunExample()
{
    using namespace stk;

    InitLeds();

    enum { TASK_COUNT = 4 };

#if USE_EDF
    typedef SwitchStrategyEDF SchedulerType;
#else
    typedef SwitchStrategyRM SchedulerType;
#endif

    static Kernel<KERNEL_STATIC | KERNEL_HRT, TASK_COUNT, SchedulerType, PlatformDefault> kernel;

    // assume that hardware LED tasks have highest priority
    static HwLedTask<ACCESS_PRIVILEGED> hwt0(0), hwt1(1), hwt2(2);

    // control task is sending commands to hardware tasks
    static CtrlTask<ACCESS_USER> ctrl;

    kernel.Initialize();

    // optional: you can override sleep and hard fault default behaviors
    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

#define MSEC(MS) GetTicksFromMsec(MS, PERIODICITY_DEFAULT)

    //                    periodicity  deadline   start delay
    kernel.AddTask(&ctrl, MSEC(200),   MSEC(100), MSEC(0));
    kernel.AddTask(&hwt0, MSEC(200),   MSEC(20), MSEC(0));
    kernel.AddTask(&hwt1, MSEC(200),   MSEC(20), MSEC(0));
    kernel.AddTask(&hwt2, MSEC(200),   MSEC(20), MSEC(0));

#if !USE_EDF
    auto wcrt_sched = SchedulabilityCheck::IsSchedulableWCRT<TASK_COUNT>(kernel.GetSwitchStrategy());
    STK_ASSERT(wcrt_sched == true);
#endif

    kernel.Start();

    // shall not reach here
    STK_ASSERT(false);
}

