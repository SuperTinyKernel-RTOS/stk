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
// ============================ SwitchStrategyRR ====================== //
// ============================================================================ //

TEST_GROUP(SwitchStrategyRoundRobin)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.ExpectAssert(false);
        g_TestContext.RethrowAssertException(true);
    }
};

TEST(SwitchStrategyRoundRobin, GetFirstEmpty)
{
    SwitchStrategyRR rr;

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

TEST(SwitchStrategyRoundRobin, GetNextEmpty)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&task1);
    kernel.RemoveTask(&task1);
    CHECK_EQUAL(0, strategy->GetSize());

    // expect to return NULL which puts core into a sleep mode, current is ignored by this strategy
    CHECK_EQUAL(0, strategy->GetNext());
}

TEST(SwitchStrategyRoundRobin, OnTaskDeadlineMissedNotSupported)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
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

TEST(SwitchStrategyRoundRobin, EndlessNext)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
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

TEST(SwitchStrategyRoundRobin, Algorithm)
{
    // Create kernel with 3 tasks
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
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

} // namespace stk
} // namespace test
