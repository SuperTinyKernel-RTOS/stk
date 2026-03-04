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

enum LedState
{
    LED_OFF, LED_ON, LED_NEXT
};

static volatile LedState g_TaskSwitch = LED_NEXT;
static volatile LedState g_Task = LED_OFF;

static void InitLeds()
{
    Led::Init(Led::GREEN, false);
}

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class LedTask : public stk::Task<2048, _AccessMode>
{
    LedState m_task_id;

public:
    LedTask(LedState task_id) : m_task_id(task_id) {}

    stk::RunFuncType GetFunc() {
        return [](void *p) {
            static_cast<LedTask *>(p)->Run();
        };
    }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        LedState task_id = m_task_id;

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

            // switch LED on/off
            {
                // we do not want preemption during IO with hardware
                stk::hw::CriticalSection::ScopedLock __cs;

                Led::Set(Led::GREEN, (task_id == LED_OFF ? false : true));
            }

            // wait for a next switch
            g_Task = task_id;
            g_TaskSwitch = LED_NEXT;
        }
    }
};

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class CtrlTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
public:
    CtrlTask() {}

    stk::RunFuncType GetFunc() {
        return [](void *p) {
            static_cast<CtrlTask *>(p)->Run();
        };
    }
    void *GetFuncUserData() { return this; }

private:
    void Run()
    {
        int64_t task_start = stk::GetTimeNowMsec();

        while (true)
        {
            if (g_TaskSwitch != LED_NEXT)
            {
                // to avoid hot loop and excessive CPU usage sleep 10ms while waiting for the own turn,
                // if scheduler does not have active threads then it will fall into a sleep mode which is
                // saving the consumed power
                stk::Sleep(10);
                continue;
            }

            // sleep 1s and delegate work to another task switching another LED, hw thread could have
            // some latency, thus account for it
            int32_t sleep = 1000 + (int32_t)(task_start - stk::GetTimeNowMsec());
            if (sleep > 0)
                stk::Sleep(sleep);

            switch (g_Task)
            {
            case LED_OFF:
                g_TaskSwitch = LED_ON;
                break;
            case LED_ON:
                g_TaskSwitch = LED_OFF;
                break;
            default:
                STK_ASSERT(false);
                break;
            }

            task_start = stk::GetTimeNowMsec();
        }
    }
};

void StartCore0()
{
    using namespace stk;

    // allocate scheduling kernel for 1 thread (tasks) with Round-robin scheduling strategy
    static Kernel<KERNEL_STATIC, 2, SwitchStrategyRoundRobin, PlatformDefault> kernel;

    // these are secure/trusted tasks which are allowed to access hardware safely
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task0(LED_OFF), secure_hw_task1(LED_ON);

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    kernel.AddTask(&secure_hw_task0);
    kernel.AddTask(&secure_hw_task1);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}

void StartCore1()
{
    using namespace stk;

    // allocate scheduling kernel for 1 thread (tasks) with Round-robin scheduling strategy
    static Kernel<KERNEL_STATIC, 1, SwitchStrategyRoundRobin, PlatformDefault> kernel;

    // if MCU supports (for example Cortex-M7/M33), ACCESS_USER does not allow an access to a hardware directly,
    // therefore you can process in this thread/task an insecure context or data
    static CtrlTask<ACCESS_USER> unsecure_task;

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    kernel.AddTask(&unsecure_task);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}

void RunExample()
{
    InitLeds();

    // start on the main core (0) in the last step as it will be the last blocking call of RunExample
    Cpu::Start(1, StartCore1);
    Cpu::Start(0, StartCore0);

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
