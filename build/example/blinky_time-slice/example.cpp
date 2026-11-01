/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

// Time-slicing demo (KERNEL_TIMESLICE).
//
// Four CPU-bound tasks of the SAME priority never sleep, never yield and never block. The kernel
// alone rotates the CPU between them when the per-task time quantum expires. Each task keeps its
// own LED lit while it runs, so the lit LED is the task that currently owns the CPU.
//
// Two phases alternate every PHASE_MS:
//  - PHASE_WEIGHTED: quanta are 100/200/400/800 ms, so LEDs stay lit in proportion to the quantum
//  - PHASE_EQUAL:    all quanta are 250 ms, so every LED stays lit for the same time
// The RED task switches phases at run-time by calling SetTimeSlice() for every task.

#include <stk.h>
#include "example.h"

using namespace bsp;

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// How long each phase lasts
static const uint32_t PHASE_MS = 10000;

// Quantum (ms) of each task, indexed by LedId: LED_GREEN, LED_ORANGE, LED_RED, LED_BLUE
static const uint32_t QUANTUM_WEIGHTED_MS[LED_MAX] = { 100, 200, 400, 800 };
static const uint32_t QUANTUM_EQUAL_MS             = 250;

static stk::Timeout MsToTicks(uint32_t ms)
{
    return static_cast<stk::Timeout>(stk::GetTicksFromMs(ms));
}

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class LedTaskT : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    LedTaskT(uint8_t task_id) : m_task_id(task_id)
    {}

    // Initial time quantum of this task (see PHASE_WEIGHTED), can be changed later with SetTimeSlice()
    stk::Timeout GetTimeSlice() const override { return MsToTicks(QUANTUM_WEIGHTED_MS[m_task_id]); }

    // All tasks MUST have the same priority and the threshold equal to it: a task with the preemption
    // threshold higher than its priority is "held" and is never time-sliced
    stk::Weight GetWeight() const override { return stk::sched::SwitchStrategyPT32_TS::MakeWeight(1, 1); }

private:
    void Run() override;
};

typedef LedTaskT<stk::ACCESS_PRIVILEGED> LedTask;

// note: tasks are not copyable (the kernel refers to a task by its address), so they are separate objects
static LedTask g_TaskGreen(LED_GREEN);
static LedTask g_TaskOrange(LED_ORANGE);
static LedTask g_TaskRed(LED_RED);
static LedTask g_TaskBlue(LED_BLUE);

// All tasks indexed by LedId, used by the RED task to change quanta of every task
static LedTask *const g_Tasks[LED_MAX] = { &g_TaskGreen, &g_TaskOrange, &g_TaskRed, &g_TaskBlue };

// Set quantum of every task according to the phase
static void ApplyPhase(bool weighted)
{
    for (uint8_t i = 0; i < LED_MAX; ++i)
    {
        // returns the previous quantum, which we do not need here
        (void)stk::SetTimeSlice(g_Tasks[i]->GetTid(), MsToTicks(weighted ? QUANTUM_WEIGHTED_MS[i] : QUANTUM_EQUAL_MS));
    }
}

template <stk::EAccessMode _AccessMode>
void LedTaskT<_AccessMode>::Run()
{
    // only the RED task manages phases
    const bool is_manager = (m_task_id == LED_RED);

    bool weighted = true;
    stk::Ticks next_phase = stk::GetTicks() + stk::GetTicksFromMs(PHASE_MS);

    while (true)
    {
        // we never sleep: the LED of whichever task is running is lit, the kernel takes the CPU away
        // from us when our quantum expires and hands it to the next task of the same priority
        Led::SwitchOnExclusive(static_cast<LedId>(m_task_id));

        if (is_manager && (stk::GetTicks() >= next_phase))
        {
            weighted = !weighted;
            next_phase += stk::GetTicksFromMs(PHASE_MS);

            ApplyPhase(weighted);
        }
    }
}

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // operating in Static + TimeSlice mode, KERNEL_SYNC is not needed as tasks do not use sync objects,
    // tickless mode is not used either as tasks never sleep and the CPU is never idle
    const uint8_t KernelMode = KERNEL_STATIC | KERNEL_TIMESLICE;

    // allocate scheduling kernel for 4 threads (tasks) with Preemption-Threshold strategy with time-slicing
    static Kernel<KernelMode, LED_MAX, sched::SwitchStrategyPT32_TS, PlatformDefault> kernel;

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    for (uint8_t i = 0; i < LED_MAX; ++i)
    {
        kernel.AddTask(g_Tasks[i]);
    }

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
