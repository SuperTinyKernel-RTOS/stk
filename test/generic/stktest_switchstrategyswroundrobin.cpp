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

// ============================================================================ //
// =========================== OnTaskYield (SWRR) ============================= //
// ============================================================================ //

// SwitchStrategySWRR (NoSleepYield = false): OnTaskYield() must report "not handled" (false) so the
// kernel falls back to the legacy sleep-based yield, and must not influence the selection.
TEST(SwitchStrategySWRoundRobin, YieldLegacyReturnsFalse)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategySWRR, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    // equal weights: strict rotation task1 -> task2 -> task3
    IKernelTask *k1 = strategy->GetNext();
    IKernelTask *k2 = strategy->GetNext();
    IKernelTask *k3 = strategy->GetNext();
    CHECK_EQUAL(&task1, k1->GetUserTask());
    CHECK_EQUAL(&task2, k2->GetUserTask());
    CHECK_EQUAL(&task3, k3->GetUserTask());

    CHECK_FALSE(strategy->OnTaskYield(k1));
    CHECK_FALSE(strategy->OnTaskYield(k2));
    CHECK_FALSE(strategy->OnTaskYield(k3));

    // all tasks are still runnable and no task is excluded from the selection
    CHECK_EQUAL(3, strategy->GetSize());
    CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "legacy yield must not affect selection (task1)");
    CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "legacy yield must not affect selection (task2)");
    CHECK_EQUAL_TEXT(&task3, strategy->GetNext()->GetUserTask(), "legacy yield must not affect selection (task3)");
}

TEST(SwitchStrategySWRoundRobin, YieldLegacySingleTaskReturnsFalse)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategySWRR, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    IKernelTask *k1 = strategy->GetNext();

    CHECK_FALSE(strategy->OnTaskYield(k1));
    CHECK_EQUAL(&task1, strategy->GetNext()->GetUserTask());
}

// ============================================================================ //
// ========================== OnTaskYield (SWRR_NSY) ========================== //
// ============================================================================ //

TEST(SwitchStrategySWRoundRobin, YieldNoSleepConfig)
{
    CHECK_EQUAL(0, SwitchStrategySWRR::NOSLEEP_YIELD_API);
    CHECK_EQUAL(1, SwitchStrategySWRR_NSY::NOSLEEP_YIELD_API);
}

// Without yields SWRR_NSY behaves exactly like SWRR: equal weights give strict rotation.
TEST(SwitchStrategySWRoundRobin, YieldNoSleepNoYieldBaseline)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategySWRR_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    for (int32_t i = 0; i < 3; i++)
    {
        CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1");
        CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "expecting task2");
        CHECK_EQUAL_TEXT(&task3, strategy->GetNext()->GetUserTask(), "expecting task3");
    }
}

// Yielding task, which would have been selected next, is skipped (one-shot) and stays runnable.
TEST(SwitchStrategySWRoundRobin, YieldNoSleepSkipsYieldingTaskOnce)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategySWRR_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    IKernelTask *k1 = strategy->GetNext(); // task1 (current weights: -2, 1, 1)
    CHECK_EQUAL(&task1, k1->GetUserTask());

    // without a yield task2 would be next, but it is the one yielding
    IKernelTask *k2 = strategy->GetFirst();
    k2 = (*k2->GetNext()); // second task in the runnable list
    CHECK_EQUAL(&task2, k2->GetUserTask());

    CHECK_TRUE(strategy->OnTaskYield(k2));
    CHECK_EQUAL_TEXT(&task3, strategy->GetNext()->GetUserTask(), "expecting task3 (task2 yielded)");

    // exclusion is one-shot and task2 kept accruing weight: it is selected right away
    CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "expecting task2 (yield exclusion is one-shot)");
    CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1");

    // yielding task stays runnable
    CHECK_EQUAL(3, strategy->GetSize());
}

// Weighted case: the heavy task that yields is skipped once even though it has the highest weight.
TEST(SwitchStrategySWRoundRobin, YieldNoSleepHeavyTaskYields)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategySWRR_NSY, PlatformTestMock> kernel;
    TaskMockW<3, ACCESS_USER> task1; // heavy
    TaskMockW<1, ACCESS_USER> task2; // light
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    // reference SWRR sequence for weights 3:1 is task1, task1, task2, task1
    IKernelTask *k1 = strategy->GetNext();
    CHECK_EQUAL(&task1, k1->GetUserTask()); // current weights: -1, 1

    // task1 would win again (current weight 2 vs 2 -> first), but it yields
    CHECK_TRUE(strategy->OnTaskYield(k1));
    CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "expecting task2 (heavy task1 yielded)");

    // task1 retained the weight it accrued while yielding: it is selected next
    CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1 after the one-shot exclusion");
}

// Alone in the runnable list: yield is handled and the same task is selected anyway.
TEST(SwitchStrategySWRoundRobin, YieldNoSleepSingleTask)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategySWRR_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    IKernelTask *k1 = strategy->GetNext();
    CHECK_EQUAL(&task1, k1->GetUserTask());

    for (int32_t i = 0; i < 5; i++)
    {
        CHECK_TRUE(strategy->OnTaskYield(k1));
        CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "single task must be selected despite the yield");
    }

    CHECK_EQUAL(1, strategy->GetSize());
}

// Two equal tasks ping-pong via yield.
TEST(SwitchStrategySWRoundRobin, YieldNoSleepPingPong)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategySWRR_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    IKernelTask *k1 = strategy->GetNext(); // task1
    CHECK_EQUAL(&task1, k1->GetUserTask());
    IKernelTask *k2 = (*k1->GetNext());
    CHECK_EQUAL(&task2, k2->GetUserTask());

    for (int32_t i = 0; i < 4; i++)
    {
        CHECK_TRUE(strategy->OnTaskYield(k1));
        CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "expecting task2 after task1 yielded");

        CHECK_TRUE(strategy->OnTaskYield(k2));
        CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1 after task2 yielded");
    }

    CHECK_EQUAL(2, strategy->GetSize());
}

// A yielding task removed before the next GetNext() must not leave a stale exclusion behind.
TEST(SwitchStrategySWRoundRobin, YieldNoSleepRemovedTaskDropsExclusion)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategySWRR_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    IKernelTask *k1 = strategy->GetNext(); // task1 (current weights: -2, 1, 1)
    CHECK_EQUAL(&task1, k1->GetUserTask());

    IKernelTask *k2 = (*k1->GetNext());
    CHECK_EQUAL(&task2, k2->GetUserTask());

    CHECK_TRUE(strategy->OnTaskYield(k2));
    kernel.RemoveTask(&task2);
    CHECK_EQUAL(2, strategy->GetSize());

    // remaining tasks are scheduled normally and the removed task is never returned
    for (int32_t i = 0; i < 6; i++)
    {
        IKernelTask *next = strategy->GetNext();
        CHECK_TEXT(next != NULL, "expecting a runnable task");
        CHECK_TEXT(next->GetUserTask() != &task2, "removed task must not be selected");
    }
}

// Task that is not in the runnable list (sleeping) is not handled: kernel falls back to legacy yield.
static struct SWRRYieldSleepingContext
{
    SWRRYieldSleepingContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        called   = false;
        result   = true;
        size     = 0;
        platform = NULL;
        strategy = NULL;
    }

    uint32_t             counter;
    bool                 called;
    bool                 result;
    size_t               size;
    PlatformTestMock    *platform;
    ITaskSwitchStrategy *strategy;

    void Process()
    {
        // Sleep() only marks the task as sleep-pending: the strategy receives OnTaskSleep() (task moves
        // to its sleep list) on the entry tick. The 1st call happens before that tick, so the check
        // is done on the 2nd call, when the task is already in the strategy's sleep list.
        if (counter == 1)
        {
            // the only task is sleeping: runnable list is empty and GetFirst() returns it from the sleep list
            IKernelTask *sleeping = strategy->GetFirst();

            size   = strategy->GetSize();
            result = strategy->OnTaskYield(sleeping);
            called = true;
        }

        platform->ProcessTick();

        ++counter;
    }
}
g_SWRRYieldSleepingContext;

static void SWRRYieldSleepingRelaxCpu()
{
    g_SWRRYieldSleepingContext.Process();
}

TEST(SwitchStrategySWRoundRobin, YieldNoSleepSleepingTaskNotHandled)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategySWRR_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_RelaxCpuHandler = SWRRYieldSleepingRelaxCpu;
    g_SWRRYieldSleepingContext.Clear();
    g_SWRRYieldSleepingContext.platform = platform;
    g_SWRRYieldSleepingContext.strategy = kernel.GetSwitchStrategy();

    Sleep(2);

    g_RelaxCpuHandler = NULL;

    CHECK_TEXT(g_SWRRYieldSleepingContext.called, "expecting relax-cpu handler to be called while task sleeps");
    CHECK_EQUAL(1, g_SWRRYieldSleepingContext.size);
    CHECK_FALSE_TEXT(g_SWRRYieldSleepingContext.result, "sleeping task is not in runnable list: yield not handled");
}

} // namespace stk
} // namespace test
