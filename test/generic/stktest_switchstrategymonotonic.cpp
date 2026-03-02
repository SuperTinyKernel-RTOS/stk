/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
// ============================ SwitchStrategyMonotonic ====================== //
// ============================================================================ //

TEST_GROUP(SwitchStrategyMonotonic)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.ExpectAssert(false);
        g_TestContext.RethrowAssertException(true);
    }
};

TEST(SwitchStrategyMonotonic, GetFirstEmpty)
{
    SwitchStrategyRM rr;

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

TEST(SwitchStrategyMonotonic, SleepNotSupported)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRM, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    try
    {
        g_TestContext.ExpectAssert(true);
        strategy->OnTaskSleep(strategy->GetFirst());
        CHECK_TEXT(false, "expecting assertion - OnTaskSleep not supported");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        strategy->OnTaskWake(strategy->GetFirst());
        CHECK_TEXT(false, "expecting assertion - OnTaskWake not supported");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // we need this workaround to pass 100% coverage test by blocking the exception
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    strategy->OnTaskSleep(strategy->GetFirst());
    strategy->OnTaskWake(strategy->GetFirst());
}

TEST(SwitchStrategyMonotonic, GetNextEmpty)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRM, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&task1);
    kernel.RemoveTask(&task1);
    CHECK_EQUAL(0, strategy->GetSize());

    try
    {
        g_TestContext.ExpectAssert(true);
        strategy->GetNext();
        CHECK_TEXT(false, "expecting assertion when empty");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

template <class _SwitchStrategy>
static void TestPriorityNext()
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 3, _SwitchStrategy, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&task1, 300, 300, 0);
    kernel.AddTask(&task3, 100, 100, 0);
    kernel.AddTask(&task2, 200, 200, 0);

    // Highest priority task must be selected first
    IKernelTask *next = strategy->GetFirst();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "task3 must be selected as highest priority");

    // GetNext must always return highest-priority READY task
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Highest priority task must repeat (no round-robin)");

    // Remove highest priority
    kernel.RemoveTask(&task3);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "task2 becomes highest priority after task3 removal");

    // Remove next highest
    kernel.RemoveTask(&task2);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "task1 remains as only task");
}

TEST(SwitchStrategyMonotonic, PriorityNextRM)
{
    TestPriorityNext<SwitchStrategyRM>();
}

TEST(SwitchStrategyMonotonic, PriorityNextDM)
{
    TestPriorityNext<SwitchStrategyDM>();
}

template <class _SwitchStrategy>
static void TestAlgorithm()
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 3, _SwitchStrategy, PlatformTestMock> kernel;
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
        CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Single task must always be selected");
    }

    // --- Stage 2: Add second task ----------------------------------------

    kernel.AddTask(&task2, 200, 200, 0); // higher priority than task1

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "Higher priority task2 should preempt task1");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "Highest priority task always runs");

    // --- Stage 3: Add third task -----------------------------------------

    kernel.AddTask(&task3, 100, 100, 0); // highest priority

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Highest priority task3 should run first");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Highest priority task always runs");

    // --- Stage 4: Remove a task ------------------------------------------

    kernel.RemoveTask(&task3);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "task2 becomes highest priority after task3 removal");

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "task2 remains highest priority");

    kernel.RemoveTask(&task2);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "task1 remains as only task");
}

TEST(SwitchStrategyMonotonic, AlgorithmRM)
{
    TestAlgorithm<SwitchStrategyRM>();
}

TEST(SwitchStrategyMonotonic, AlgorithmDM)
{
    TestAlgorithm<SwitchStrategyDM>();
}

TEST(SwitchStrategyMonotonic, FailedWCRT)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 2, SwitchStrategyRM, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    const SwitchStrategyRM *strategy = static_cast<const SwitchStrategyRM *>(kernel.GetSwitchStrategy());

    kernel.Initialize();

    // Overload CPU: C/T > RMUB
    kernel.AddTask(&task1, 50, 50, 0); // 100% CPU
    kernel.AddTask(&task2, 60, 30, 0); // additional load

    auto result = SchedulabilityCheck::IsSchedulableWCRT<2>(strategy);

    CHECK_EQUAL(100, result.info[0].cpu_load.total);
    CHECK_EQUAL(150, result.info[1].cpu_load.total);

    CHECK_FALSE_TEXT(result, "Task set should be unschedulable according to WCRT");
}

TEST(SwitchStrategyMonotonic, SchedulableWCRT)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 3, SwitchStrategyRM, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3, task4;
    const SwitchStrategyRM *strategy = static_cast<const SwitchStrategyRM *>(kernel.GetSwitchStrategy());

    kernel.Initialize();

    // --- Stage 1: Schedulable tasks ---------------------------------------

    // Task parameters: T = task period, C = execution time
    kernel.AddTask(&task1, 40, 20, 0);  // task1: C=20, T=40
    kernel.AddTask(&task2, 100, 30, 0); // task2: C=30, T=100
    kernel.AddTask(&task3, 200, 10, 0); // task3: C=10, T=200

    auto result = SchedulabilityCheck::IsSchedulableWCRT<3>(strategy);
    CHECK_TEXT(result, "Task set should be schedulable according to WCRT");

    CHECK_EQUAL(50, result.info[0].cpu_load.total);
    CHECK_EQUAL(80, result.info[1].cpu_load.total);
    CHECK_EQUAL(85, result.info[2].cpu_load.total);
}

} // namespace stk
} // namespace test
