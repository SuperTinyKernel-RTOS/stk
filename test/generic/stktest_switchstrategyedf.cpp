/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "stktest.h"

namespace stk {
namespace test {

// ============================================================================ //
// ============================ SwitchStrategyEDF ====================== //
// ============================================================================ //

TEST_GROUP(SwitchStrategyEDF)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.ExpectAssert(false);
        g_TestContext.RethrowAssertException(true);
    }
};

TEST(SwitchStrategyEDF, GetFirstEmpty)
{
    SwitchStrategyEDF rr;

    try
    {
        g_TestContext.ExpectAssert(true);
        rr.GetFirst();
        CHECK_TEXT(false, "expecting assertion when empty");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(SwitchStrategyEDF, GetNextEmpty)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyEDF, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&task1);
    kernel.RemoveTask(&task1);
    CHECK_EQUAL(0, strategy->GetSize());

    // expect to return NULL which puts core into a sleep mode, current is ignored by this strategy
    CHECK_EQUAL(0, strategy->GetNext());
}

TEST(SwitchStrategyEDF, OnTaskDeadlineMissedNotSupported)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyEDF, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    try
    {
        g_TestContext.ExpectAssert(true);
        strategy->OnTaskDeadlineMissed(strategy->GetFirst());
        CHECK_TEXT(false, "expecting assertion - OnTaskDeadlineMissed not supported");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // we need this workaround to pass 100% coverage test by blocking the exception
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    strategy->OnTaskDeadlineMissed(strategy->GetFirst());
}

TEST(SwitchStrategyEDF, PriorityNext)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 3, SwitchStrategyEDF, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    // periodicity, deadline, start_delay
    kernel.AddTask(&task1, 300, 300, 0); // latest deadline
    kernel.AddTask(&task2, 200, 200, 0);
    kernel.AddTask(&task3, 100, 100, 0); // earliest deadline

    // EDF must select the task with the earliest relative deadline
    IKernelTask *next = strategy->GetNext();

    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "EDF must select task with earliest relative deadline");

    // Repeated GetNext must still select the same task
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "EDF must continue selecting earliest-deadline task");

    // Remove earliest-deadline task
    kernel.RemoveTask(&task3);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "after removal, task2 has earliest relative deadline");

    // Remove next earliest
    kernel.RemoveTask(&task2);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "only remaining task must be selected");
}

TEST(SwitchStrategyEDF, Algorithm)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 3, SwitchStrategyEDF, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    // --- Stage 1: Add first task -----------------------------------------

    kernel.AddTask(&task1, 300, 300, 0);

    IKernelTask *next = strategy->GetFirst();

    // Single task -> always returned
    for (int32_t i = 0; i < 5; i++)
    {
        next = strategy->GetNext();
        CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "single task must always be selected");
    }

    // --- Stage 2: Add second task ----------------------------------------

    kernel.AddTask(&task2, 200, 200, 0); // earlier deadline than task1

    // EDF must pick the task with earliest relative deadline
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "task2 with earlier deadline should preempt task1");

    // Still highest-priority task keeps running until removed or asleep
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "task2 remains earliest-deadline task");

    // --- Stage 3: Add third task -----------------------------------------

    kernel.AddTask(&task3, 100, 100, 0); // earliest deadline

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "task3 with earliest deadline should run first");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "task3 continues to run as earliest-deadline task");

    // --- Stage 4: Remove tasks -------------------------------------------

    kernel.RemoveTask(&task3);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "task2 becomes earliest-deadline after task3 removal");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "task2 continues as earliest-deadline task");

    kernel.RemoveTask(&task2);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "task1 remains as only task");
}

TEST(SwitchStrategyEDF, RelativeDeadlineEvolution)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, SwitchStrategyEDF, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();

    // period = 10, deadline = 5, start_delay = 0
    kernel.AddTask(&task, 10, 5, 0);

    kernel.Start();

    // Obtain kernel task
    IKernelTask *ktask = kernel.GetSwitchStrategy()->GetFirst();
    CHECK_TRUE_TEXT(ktask != nullptr, "Kernel task must exist");

    // --- At release -------------------------------------------------------

    // duration = 0
    // relative_deadline = deadline
    CHECK_EQUAL_TEXT(5, ktask->GetHrtRelativeDeadline(), "at release: relative deadline must equal deadline");

    // --- Tick 1 -----------------------------------------------------------

    platform->ProcessTick(); // duration = 1
    CHECK_EQUAL_TEXT(4, ktask->GetHrtRelativeDeadline(), "after 1 tick: relative deadline must decrease by 1");

    // --- Tick 2 -----------------------------------------------------------

    platform->ProcessTick(); // duration = 2
    CHECK_EQUAL_TEXT(3, ktask->GetHrtRelativeDeadline(), "after 2 ticks: relative deadline must decrease by 2");

    // --- Tick 3 -----------------------------------------------------------

    platform->ProcessTick(); // duration = 3
    CHECK_EQUAL_TEXT(2, ktask->GetHrtRelativeDeadline(), "after 3 ticks: relative deadline must decrease by 3");

    // --- Tick 4 -----------------------------------------------------------

    platform->ProcessTick(); // duration = 4
    CHECK_EQUAL_TEXT(1, ktask->GetHrtRelativeDeadline(), "after 4 ticks: relative deadline must be 1");

    // --- Tick 5 (deadline boundary) --------------------------------------

    platform->ProcessTick(); // duration = 5
    CHECK_EQUAL_TEXT(0, ktask->GetHrtRelativeDeadline(), "at deadline: relative deadline must be zero");
}

static struct EDFDynamicSchedulingContext
{
    EDFDynamicSchedulingContext()
    {
        counter  = 0;
        checked  = 0;
        platform = NULL;
        task1    = NULL;
        task2    = NULL;
        task3    = NULL;
    }

    uint32_t counter, checked;
    PlatformTestMock *platform;
    TaskMock<ACCESS_USER> *task1, *task2, *task3;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        // active task after this tick
        Stack *active = platform->m_stack_active;

        if (counter == 1)
        {
            CHECK_EQUAL_TEXT((size_t)task3->GetStack(), active->SP, "tick 0: task3 earliest deadline");
            ++checked;
            Yield();
        }
        else
        if (counter == 2)
        {
            CHECK_EQUAL_TEXT((size_t)task2->GetStack(), active->SP, "tick 1: task2 earliest deadline");
            ++checked;
            Yield();
        }
        else
        if (counter == 3)
        {
            CHECK_EQUAL_TEXT((size_t)task1->GetStack(), active->SP, "tick 3+: task1 earliest deadline");
            ++checked;
            Yield();
        }
    }
}
g_EDFDynamicSchedulingContext;

static void g_EDFDynamicSchedulingContextProcess()
{
    g_EDFDynamicSchedulingContext.Process();
}

TEST(SwitchStrategyEDF, DynamicScheduling)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 3, SwitchStrategyEDF, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();

    // Add periodic tasks: (periodicity, deadline, start_delay)
    kernel.AddTask(&task1, 4, 4, 0); // task1: period 5
    kernel.AddTask(&task2, 3, 3, 0); // task2: period 3
    kernel.AddTask(&task3, 2, 2, 0); // task3: period 2

    kernel.Start();

    g_EDFDynamicSchedulingContext.platform = platform;
    g_EDFDynamicSchedulingContext.task1 = &task1;
    g_EDFDynamicSchedulingContext.task2 = &task2;
    g_EDFDynamicSchedulingContext.task3 = &task3;
    g_RelaxCpuHandler = g_EDFDynamicSchedulingContextProcess;

    // simulate ticks
    g_EDFDynamicSchedulingContextProcess();

    CHECK_EQUAL_TEXT(3, g_EDFDynamicSchedulingContext.checked, "all 3 tasks must be switched");
    CHECK_FALSE(platform->m_hard_fault);
    CHECK_EQUAL(0, task1.m_deadline_missed);
    CHECK_EQUAL(0, task2.m_deadline_missed);
    CHECK_EQUAL(0, task3.m_deadline_missed);
}

} // namespace stk
} // namespace test
