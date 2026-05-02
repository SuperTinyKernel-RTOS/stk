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

// Warning: Use -ffixed-r9 compiler flag which prevents the compiler from ever allocating r9 as a
//          scratch or callee-saved register.
//          Otherwise, refrain from using stk::GetTls/SetTls and implement TLS via ITask by getting
//          it vis TId by stk::GetTId().

// Sync primitive used:
//
//   Semaphore (one per task) - each task blocks on its own semaphore; the
//   running task signals the next task's semaphore after its 1-second sleep.
//   With per-task semaphores the handover is point-to-point - exactly one
//   task is woken per Signal(), with no counter to check and no Reset() needed.
//   Kernel instance must be created with KERNEL_SYNC flag to support synchronization
//   objects.
#include <sync/stk_sync_semaphore.h>

using namespace bsp;

// One semaphore per LED task. Initial count 0: all tasks block on Wait()
// until their semaphore is signaled. task0 is pre-seeded with count 1 so
// it runs first without any external trigger.
static stk::sync::Semaphore g_Sem0(1); // task 0 runs first
static stk::sync::Semaphore g_Sem1(0);
static stk::sync::Semaphore g_Sem2(0);
static stk::sync::Semaphore g_Sem3(0);

// Indexed accessor so task template code can use g_Sem[_TaskId] style.
static stk::sync::Semaphore *const g_Sem[LED_MAX] =
{
    &g_Sem0, &g_Sem1, &g_Sem2, &g_Sem3
};

// Timeline for a precise LED switching
static stk::Ticks g_Timeline = 0;

// Simple thread-local storage, the complexity can be any
struct MyTls
{
    Led::Id led;
};

// Task function switching the LED, TLS provides the ID of the task for the logic of this function
static void SwitchOnLED()
{
    // for demonstration purpose we get task_id from our TLS and switch on corresponding LED
    Led::Id led = stk::hw::GetTlsPtr<MyTls>()->led;

    bsp::Led::SwitchOnExclusive(led);
}

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// Task's core (thread)
// _TaskId maps directly to a semaphore slot and the next-task index.
template <uint8_t _TaskId, Led::Id _LedId, stk::EAccessMode _AccessMode>
class MyTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    MyTls m_tls; // task-local TLS, you can provide your own implementation

public:
    MyTask()
    {
        // init TLS with id of the LED (see SwitchOnLED)
        m_tls.led = _LedId;
    }

private:
    void Run() override
    {
        // set TLS for this task
        stk::hw::SetTlsPtr(&m_tls);

        g_Timeline = stk::GetTicks();

        while (true)
        {
            // block until the previous task signals our semaphore,
            // each task owns exactly one semaphore slot - no broadcast wake,
            // no shared counter to check, no Reset() required
            g_Sem[_TaskId]->Wait();

            // it is static function and does not have the 'this' pointer to the task instance
            // we use TLS to get the led id
            SwitchOnLED();

            // sleep 1s and delegate work to another task switching another LED
            // we could sleep with a simple stk::Sleep if precision is not needed, but in other
            // case we could sleep until calculated precise timestamp to avoid a time drift
            //stk::SleepMs(1000);
            g_Timeline += stk::GetTicksFromMs(1000);
            stk::SleepUntil(g_Timeline);

            // hand off to the next task in the ring by signaling its semaphore
            // Signal() is ISR-safe: could also be called from a hardware timer ISR
            // to drive the ring externally without changing any task code
            g_Sem[(_TaskId + 1) % 4]->Signal();
        }
    }
};

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // allocate scheduling kernel for 4 threads (tasks) with Round-robin scheduling strategy
    static Kernel<KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), 4,
            SwitchStrategyRR, PlatformDefault> kernel;

    // note: using ACCESS_PRIVILEGED as Cortex-M7 or Cortex-M33 may require privileged access to peripherals
    static MyTask<0, Led::RED,    ACCESS_PRIVILEGED> task1;
    static MyTask<1, Led::ORANGE, ACCESS_PRIVILEGED> task2;
    static MyTask<2, Led::GREEN,  ACCESS_PRIVILEGED> task3;
    static MyTask<3, Led::BLUE,   ACCESS_PRIVILEGED> task4;

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
