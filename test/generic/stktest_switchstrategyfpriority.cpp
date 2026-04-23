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
// ========================== SwitchStrategyFixedPriority ===================== //
// ============================================================================ //

TEST_GROUP(SwitchStrategyFixedPriority)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.ExpectAssert(false);
        g_TestContext.RethrowAssertException(true);
    }
};

TEST(SwitchStrategyFixedPriority, GetFirstEmpty)
{
    SwitchStrategyFP32 rr;

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

TEST(SwitchStrategyFixedPriority, GetNextEmpty)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&task1);
    kernel.RemoveTask(&task1);
    CHECK_EQUAL(0, strategy->GetSize());

    // expect to return NULL which puts core into a sleep mode, current is ignored by this strategy
    CHECK_EQUAL(0, strategy->GetNext());
}

TEST(SwitchStrategyFixedPriority, EndlessNext)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    IKernelTask *first = strategy->GetFirst();
    CHECK_EQUAL_TEXT(&task1, first->GetUserTask(), "expecting first task1");

    IKernelTask *next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "expecting next task1");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "expecting next task2");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "expecting next task3");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "expecting next task1 again (endless looping)");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "expecting next task2 again (endless looping)");

    kernel.RemoveTask(&task2);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "expecting next task3 again (endless looping)");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "expecting next task1 again (endless looping)");
}

TEST(SwitchStrategyFixedPriority, Algorithm)
{
    // Create kernel with 3 tasks
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;

    kernel.Initialize();

    // Add tasks
    kernel.AddTask(&task1);

    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    IKernelTask *next = strategy->GetFirst();

    // --- Stage 1: 1 task only ---------------------------------------------

    // Always returns the same task
    for (int32_t i = 0; i < 5; i++)
    {
        next = strategy->GetNext();
        CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Single task must always be selected");
    }

    // --- Stage 2: add second task -----------------------------------------

    kernel.AddTask(&task2);

    next = strategy->GetNext(); // should still return task1 as task2 will be scheduled after this call
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should be task1");
    next = strategy->GetNext(); // should return task2
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "Next task should be task2");
    next = strategy->GetNext(); // should wrap around to task1
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should wrap to task1");

    // --- Stage 3: add third task ------------------------------------------

    kernel.AddTask(&task3);

    // Expected sequence: task1 -> task2 -> task3 -> task1 ...
    next = strategy->GetNext(); // task2
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "Next task should be task2");
    next = strategy->GetNext(); // task3
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Next task should be task3");
    next = strategy->GetNext(); // task1
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should wrap to task1");

    // --- Stage 4: remove a task -------------------------------------------

    kernel.RemoveTask(&task2);

    // Expected sequence: task1 -> task3 -> task1 -> task3 ...
    next = strategy->GetNext(); // task3
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Next task should be task3 after removal");
    next = strategy->GetNext(); // task1
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should be task1 after removal");
    next = strategy->GetNext(); // task3
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Next task should wrap to task3");
}

static struct PrioritySleepRelaxCpuContext
{
    PrioritySleepRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        platform = NULL;
        task1    = NULL;
        task2    = NULL;
    }

    uint32_t          counter;
    PlatformTestMock *platform;
    ITask            *task1, *task2;

    void Process()
    {
        Stack *&active = platform->m_stack_active;

        platform->ProcessTick();

        // ISR calls OnSysTick (task1 = active, task2 = idle (sleeping))
        if (counter == 0)
        {
            CHECK_EQUAL_TEXT(active->SP, (Word)task1->GetStack(), "sleep: expecting low-priority task1");
        }
        else
        // ISR calls OnSysTick (task1 = idle (lower priority), task2 = active (higher priority))
        if (counter == 1)
        {
            CHECK_EQUAL_TEXT(active->SP, (Word)task2->GetStack(), "sleep: expecting high-priority task2");
        }

        ++counter;
    }
}
g_PrioritySleepRelaxCpuContext;

static void PrioritySleepRelaxCpu()
{
    g_PrioritySleepRelaxCpuContext.Process();
}

TEST(SwitchStrategyFixedPriority, Priority)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyFixedPriority<5>, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1; // low priority
    TaskMockW<2, ACCESS_USER> task2; // high priority
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting high-priority task2 on start");

    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting task2");

    g_RelaxCpuHandler = PrioritySleepRelaxCpu;
    g_PrioritySleepRelaxCpuContext.Clear();
    g_PrioritySleepRelaxCpuContext.platform = platform;
    g_PrioritySleepRelaxCpuContext.task1    = &task1;
    g_PrioritySleepRelaxCpuContext.task2    = &task2;

    // task2 calls Sleep to become idle
    Sleep(2);

    // task2 is active again
    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting high-priority task2 again after it slept");

    // ISR calls OnSysTick, higher priority task2 is scheduled
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting high-priority task2 again");

    g_RelaxCpuHandler = NULL;
}

} // namespace stk
} // namespace test
