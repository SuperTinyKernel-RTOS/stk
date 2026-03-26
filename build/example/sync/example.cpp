/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

//#define _STK_ASSERT_REDIRECT

#ifdef _STK_ASSERT_REDIRECT
#include <stdint.h>
extern void STK_ASSERT_HANDLER(const char *err, const char *source, int32_t line);
#endif

#include <stk_config.h>
#include <stk.h>
#include <sync/stk_sync.h>
#include "example.h"

#ifdef _PICO_H
    #define STK_EXAMPLE_DUALCORE 1
#else
    #define STK_EXAMPLE_DUALCORE 0
#endif

#define STK_EXAMPLE_USE_PIPE 1

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

#ifdef _STK_ASSERT_REDIRECT
void STK_ASSERT_HANDLER(const char *err, const char *source, int32_t line)
{
    __stk_debug_break();
    while (true) {}
}
#endif

enum LedState
{
    LED_OFF = 0,
    LED_ON
};

#if STK_EXAMPLE_USE_PIPE
static stk::sync::Pipe<LedState, 1> g_CtrlSignalPipe;
#else
static stk::sync::Event g_EventReady, g_EventSwitchOn, g_EventSwitchOff;
#endif
static stk::sync::Mutex g_HwMutex;

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
    LedTask(LedState task_id) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        LedState task_id = m_task_id;

        while (true)
        {
            // time out in 100ms (just as an example, can be WAIT_INFINITE for max power saving)
        #if STK_EXAMPLE_USE_PIPE
            if (!g_CtrlSignalPipe.Read(task_id, 100))
                continue;
        #else
            switch (task_id)
            {
            case LED_ON:
                if (!g_EventSwitchOn.Wait(100))
                    continue;
                break;
            case LED_OFF:
                if (!g_EventSwitchOff.Wait(100))
                    continue;
                break;
            }
        #endif

            // switch LED on/off
            {
                // we do not want preemption during IO with hardware
                stk::sync::Mutex::ScopedLock guard(g_HwMutex);

                Led::Set(Led::GREEN, (task_id == LED_OFF ? false : true));
            }

        #if !STK_EXAMPLE_USE_PIPE
            g_EventReady.Set();
        #endif
        }
    }
};

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class CtrlTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
private:
    void Run()
    {
        int64_t task_start = stk::GetTimeNowMsec();

    #if !STK_EXAMPLE_USE_PIPE
        g_EventReady.Set();
    #endif

        bool led_sw = false;

        while (true)
        {
        #if !STK_EXAMPLE_USE_PIPE
            if (!g_EventReady.Wait())
                continue;
        #endif

            // sleep 1s and delegate work to another task switching another LED, hw thread could have
            // some latency, thus account for it
            int32_t sleep = 250 + (int32_t)(task_start - stk::GetTimeNowMsec());
            if (sleep > 0)
                stk::Sleep(sleep);

            led_sw = !led_sw;

            task_start = stk::GetTimeNowMsec();

        #if STK_EXAMPLE_USE_PIPE
            if (!g_CtrlSignalPipe.Write(led_sw ? LED_ON : LED_OFF))
                continue;
        #else
            if (led_sw)
                g_EventSwitchOn.Set();
            else
                g_EventSwitchOff.Set();
        #endif
        }
    }
};

#if STK_EXAMPLE_DUALCORE

void StartCore0()
{
    using namespace stk;

    // allocate scheduling kernel for 1 thread (tasks) with Round-robin scheduling strategy
    static Kernel<KERNEL_STATIC | KERNEL_SYNC, 2, SwitchStrategyRR, PlatformDefault> kernel;

    // these are secure/trusted tasks which are allowed to access hardware safely
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task0(LED_OFF);
#if !STK_EXAMPLE_USE_PIPE
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task1(LED_ON);
#endif

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    kernel.AddTask(&secure_hw_task0);
#if !STK_EXAMPLE_USE_PIPE
    kernel.AddTask(&secure_hw_task1);
#endif

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}

void StartCore1()
{
    using namespace stk;

    // allocate scheduling kernel for 1 thread (tasks) with Round-Robin scheduling strategy
    static Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformDefault> kernel;

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

    // optional, for debugging only
#if !STK_EXAMPLE_USE_PIPE
    g_EventReady.SetTraceName("rdy");
    g_EventSwitchOn.SetTraceName("sw-on");
    g_EventSwitchOff.SetTraceName("sw-off");
#endif

    // start on the main core (0) in the last step as it will be the last blocking call of RunExample
    Cpu::Start(1, StartCore1);
    Cpu::Start(0, StartCore0);

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}

#else

void RunExample()
{
    using namespace stk;

    InitLeds();

    // allocate scheduling kernel for 1 thread (tasks) with Round-Robin scheduling strategy
    static Kernel<KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), 3,
        SwitchStrategyRR, PlatformDefault> kernel;

    // these are secure/trusted tasks which are allowed to access hardware safely
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task0(LED_OFF);
#if !STK_EXAMPLE_USE_PIPE
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task1(LED_ON);
#endif

    // if MCU supports (for example Cortex-M7/M33), ACCESS_USER does not allow an access to a hardware directly,
    // therefore you can process in this thread/task an insecure context or data
    static CtrlTask<ACCESS_USER> unsecure_task;

    // optional, for debugging only
#if !STK_EXAMPLE_USE_PIPE
    g_EventReady.SetTraceName("rdy");
    g_EventSwitchOn.SetTraceName("sw-on");
    g_EventSwitchOff.SetTraceName("sw-off");
#endif

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    kernel.AddTask(&secure_hw_task0);
#if !STK_EXAMPLE_USE_PIPE
    kernel.AddTask(&secure_hw_task1);
#endif
    kernel.AddTask(&unsecure_task);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}

#endif
