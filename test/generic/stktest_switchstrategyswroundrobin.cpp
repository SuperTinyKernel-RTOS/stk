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
// =================== SwitchStrategySmoothWeightedRoundRobin ================= //
// ============================================================================ //

TEST_GROUP(SwitchStrategySWRoundRobin)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.ExpectAssert(false);
        g_TestContext.RethrowAssertException(true);
    }
};

TEST(SwitchStrategySWRoundRobin, GetFirstEmpty)
{
    SwitchStrategySWRR rr;

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

TEST(SwitchStrategySWRoundRobin, GetNextEmpty)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategySWRR, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&task1);
    kernel.RemoveTask(&task1);
    CHECK_EQUAL(0, strategy->GetSize());

    // expect to return NULL which puts core into a sleep mode, current is ignored by this strategy
    CHECK_EQUAL(0, strategy->GetNext());
}

TEST(SwitchStrategySWRoundRobin, OnTaskDeadlineMissedNotSupported)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategySWRR, PlatformTestMock> kernel;
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

TEST(SwitchStrategySWRoundRobin, EndlessNext)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategySWRR, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1, task2, task3;

    kernel.Initialize();
    kernel.AddTask(&task1);

    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    IKernelTask *next = strategy->GetFirst();

    // --- Stage 1: 1 task only ---------------------------------------------

    // The scheduler must ALWAYS return task1
    for (int32_t i = 0; i < 10; i++)
    {
        next = strategy->GetNext();
        CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Single task must always be selected");
    }

    // --- Stage 2: add second task -----------------------------------------

    kernel.AddTask(&task2);

    // Both tasks must appear, and none should be starved
    bool seen1 = false;
    bool seen2 = false;
    for (int i = 0; i < 10; i++)
    {
        next = strategy->GetNext();
        if (next->GetUserTask() == &task1) seen1 = true;
        if (next->GetUserTask() == &task2) seen2 = true;
    }
    CHECK_TEXT(seen1, "Task1 must be selected after adding Task2");
    CHECK_TEXT(seen2, "Task2 must be selected after adding Task2");

    // --- Stage 3: add third task ------------------------------------------

    kernel.AddTask(&task3);

    seen1 = seen2 = false;
    bool seen3 = false;
    for (int32_t i = 0; i < 20; i++)
    {
        next = strategy->GetNext();
        if (next->GetUserTask() == &task1) seen1 = true;
        if (next->GetUserTask() == &task2) seen2 = true;
        if (next->GetUserTask() == &task3) seen3 = true;
    }

    CHECK_TEXT(seen1, "Task1 must run after adding Task3");
    CHECK_TEXT(seen2, "Task2 must run after adding Task3");
    CHECK_TEXT(seen3, "Task3 must run after adding Task3");

    // --- Stage 4: remove task1 --------------------------------------------

    kernel.RemoveTask(&task1);

    seen2 = seen3 = false;
    for (int32_t i = 0; i < 10; i++)
    {
        next = strategy->GetNext();
        if (next->GetUserTask() == &task2) seen2 = true;
        if (next->GetUserTask() == &task3) seen3 = true;
        CHECK_TEXT(next->GetUserTask() != &task1, "Task1 must not be selected after removal");
    }
    CHECK_TEXT(seen2, "Task2 must run after removing Task1");
    CHECK_TEXT(seen3, "Task3 must run after removing Task1");
}

TEST(SwitchStrategySWRoundRobin, Algorithm)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategySWRR, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1; // weight 1
    TaskMockW<2, ACCESS_USER> task2; // weight 2
    TaskMockW<3, ACCESS_USER> task3; // weight 3

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();
    IKernelTask *next = strategy->GetFirst();

    // scheduling stats
    int32_t count1 = 0, count2 = 0, count3 = 0;

    // Run enough steps to reach stable proportions
    const int32_t steps = 120; // increased steps for better statistical stability
    for (int32_t i = 0; i < steps; i++)
    {
        next = strategy->GetNext();

        if (next->GetUserTask() == &task1) ++count1;
        else if (next->GetUserTask() == &task2) ++count2;
        else if (next->GetUserTask() == &task3) ++count3;
        else CHECK_TEXT(false, "Unknown task selected");
    }

    const int32_t w1 = 1, w2 = 2, w3 = 3;
    const int32_t total_w = w1 + w2 + w3;

    const int32_t exp1 = (steps * w1) / total_w; // integer expected counts
    const int32_t exp2 = (steps * w2) / total_w;
    const int32_t exp3 = steps - exp1 - exp2;    // ensure sum == steps

    // Relative tolerance (fraction). 25% is conservative for small sample sizes.
    const float tol_frac = 0.25f;

    auto within_tol = [&](int expected, int actual, float frac) -> bool
    {
        int tol = (int)(expected * frac) + 1; // +1 to avoid zero tolerance for small expected
        int diff = expected > actual ? expected - actual : actual - expected;
        return diff <= tol;
    };

    CHECK_TRUE_TEXT(within_tol(exp1, count1, tol_frac), "Task1 proportion off (weight 1)");
    CHECK_TRUE_TEXT(within_tol(exp2, count2, tol_frac), "Task2 proportion off (weight 2)");
    CHECK_TRUE_TEXT(within_tol(exp3, count3, tol_frac), "Task3 proportion off (weight 3)");

    // Validate non-starvation: each must get at least one selection
    CHECK_TEXT(count1 > 0, "Task1 must run at least once");
    CHECK_TEXT(count2 > 0, "Task2 must run at least once");
    CHECK_TEXT(count3 > 0, "Task3 must run at least once");

    // Remove highest-weight task, check proportions adjust to 1:2
    kernel.RemoveTask(&task3);

    // reset counters and sample again
    count1 = count2 = 0;

    // advance next once to avoid stuck reference (optional)
    next = strategy->GetNext();

    for (int32_t i = 0; i < steps; i++)
    {
        next = strategy->GetNext();

        if      (next->GetUserTask() == &task1) ++count1;
        else if (next->GetUserTask() == &task2) ++count2;
        else    CHECK_TEXT(false, "Unknown task selected after removal");
    }

    const int32_t nw1 = 1, nw2 = 2;
    const int32_t ntotal = nw1 + nw2;
    const int32_t nexp1 = (steps * nw1) / ntotal;
    const int32_t nexp2 = steps - nexp1;

    CHECK_TRUE_TEXT(within_tol(nexp1, count1, tol_frac), "Task1 proportion off after removal");
    CHECK_TRUE_TEXT(within_tol(nexp2, count2, tol_frac), "Task2 proportion off after removal");

    CHECK_TEXT(count1 > 0, "Task1 must run after removal");
    CHECK_TEXT(count2 > 0, "Task2 must run after removal");
}

} // namespace stk
} // namespace test
