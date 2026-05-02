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
#include "example.h"

// Sync primitives used:
//   sync::PipeT<LedState, 1> - typed single-slot FIFO per LedTask; CtrlTask writes a command,
//                              the matching LedTask blocks in Read() instead of spin-sleeping.
//   sync::Event              - CtrlTask waits on this event for the 1-second interval; a future
//                              caller (e.g. a button ISR) can call Set() to interrupt the delay
//                              early without any code change to CtrlTask.
#include <sync/stk_sync_pipe.h>
#include <sync/stk_sync_event.h>

using namespace bsp;

enum LedState : uint8_t
{
    LED_OFF = 0,
    LED_ON  = 1
};

// One single-slot pipe per LED task.  CtrlTask writes the command; the
// corresponding LedTask blocks in Read() until its turn arrives.
// Capacity = 1: only one pending command is ever needed per task.
static stk::sync::PipeT<LedState, 1> g_PipeOff;  // commands for LED_OFF task
static stk::sync::PipeT<LedState, 1> g_PipeOn;   // commands for LED_ON  task

// CtrlTask uses this event to sleep for around 1 s. Keeping it as an Event (rather
// than a plain stk::Sleep) means an external caller, e.g. a button ISR, can
// call g_WakeCtrl.Set() to shorten or cancel the delay at any time.
static stk::sync::Event g_WakeCtrl;

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class LedTask : public stk::Task<2048, _AccessMode>
{
    LedState                       m_task_id;
    stk::sync::PipeT<LedState, 1> &m_pipe;   // reference to this task's command pipe

public:
    LedTask(LedState task_id, stk::sync::PipeT<LedState, 1> &pipe)
        : m_task_id(task_id), m_pipe(pipe)
    {}

private:
    void Run() override
    {
        while (true)
        {
            // block here until CtrlTask sends a command on our pipe
            LedState cmd;
            if (!m_pipe.Read(cmd))
                continue;

            // switch LED on/off
            {
                // we do not want preemption during IO with hardware
                stk::hw::CriticalSection::ScopedLock __cs;

                Led::Set(Led::GREEN, (m_task_id == LED_ON));
            }
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
private:
    void Run() override
    {
        LedState next = LED_ON;   // first command sent after startup

        while (true)
        {
            // sleep 1s and delegate work to another task switching another LED;
            // Wait(1000) returns false on timeout (normal tick) or true if woken
            // early by Set() - either way proceed to the next toggle
            g_WakeCtrl.Wait(1000);
            g_WakeCtrl.Reset(); // re-arm for the next iteration

            // TryWrite is used because each pipe has capacity 1 and CtrlTask is
            // the sole producer; the pipe is always empty here by design
            if (next == LED_ON)
            {
                g_PipeOn.TryWrite(LED_ON);
                next = LED_OFF;
            }
            else
            {
                g_PipeOff.TryWrite(LED_OFF);
                next = LED_ON;
            }
        }
    }
};

void StartCore0()
{
    using namespace stk;

    // allocate scheduling kernel for 1 thread (tasks) with Round-robin scheduling strategy
    static Kernel<KERNEL_STATIC | KERNEL_SYNC, 2, SwitchStrategyRoundRobin, PlatformDefault> kernel;

    // these are secure/trusted tasks which are allowed to access hardware safely
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task0(LED_OFF, g_PipeOff);
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task1(LED_ON,  g_PipeOn);

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
    static Kernel<KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), 1,
            SwitchStrategyRoundRobin, PlatformDefault> kernel;

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
    Led::InitAll(false);

    // start on the main core (0) in the last step as it will be the last blocking call of RunExample
    Cpu::Start(1, StartCore1);
    Cpu::Start(0, StartCore0);

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
