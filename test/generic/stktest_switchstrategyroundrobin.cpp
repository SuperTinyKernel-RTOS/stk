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

// ============================================================================ //
// ============================ OnTaskYield (RR) ============================== //
// ============================================================================ //

// SwitchStrategyRR (NoSleepYield = false): OnTaskYield() must report "not handled" (false) so the
// kernel falls back to the legacy sleep-based yield, and must not touch the cursor.
TEST(SwitchStrategyRoundRobin, YieldLegacyReturnsFalse)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    IKernelTask *k1 = strategy->GetNext();
    IKernelTask *k2 = strategy->GetNext();
    IKernelTask *k3 = strategy->GetNext();
    CHECK_EQUAL(&task1, k1->GetUserTask());
    CHECK_EQUAL(&task2, k2->GetUserTask());
    CHECK_EQUAL(&task3, k3->GetUserTask());

    // not handled for any of the tasks (cursor is on task3 at this point)
    CHECK_FALSE(strategy->OnTaskYield(k1));
    CHECK_FALSE(strategy->OnTaskYield(k2));
    CHECK_FALSE(strategy->OnTaskYield(k3));

    // all tasks are still runnable and the rotation is untouched: task3 -> task1 -> task2 -> task3
    CHECK_EQUAL(3, strategy->GetSize());
    CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "legacy yield must not move cursor (task1)");
    CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "legacy yield must not move cursor (task2)");
    CHECK_EQUAL_TEXT(&task3, strategy->GetNext()->GetUserTask(), "legacy yield must not move cursor (task3)");
}

TEST(SwitchStrategyRoundRobin, YieldLegacySingleTaskReturnsFalse)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    IKernelTask *k1 = strategy->GetNext();

    CHECK_FALSE(strategy->OnTaskYield(k1));
    CHECK_EQUAL(&task1, strategy->GetNext()->GetUserTask());
}

// ============================================================================ //
// =========================== OnTaskYield (RR_NSY) =========================== //
// ============================================================================ //

TEST(SwitchStrategyRoundRobin, YieldNoSleepConfig)
{
    CHECK_EQUAL(0, SwitchStrategyRR::NOSLEEP_YIELD_API);
    CHECK_EQUAL(1, SwitchStrategyRR_NSY::NOSLEEP_YIELD_API);
}

// Yield on the task the cursor is on: its successor is returned next.
TEST(SwitchStrategyRoundRobin, YieldNoSleepHandsToSuccessor)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    IKernelTask *k1 = strategy->GetNext(); // task1
    IKernelTask *k2 = strategy->GetNext(); // task2
    IKernelTask *k3 = strategy->GetNext(); // task3
    CHECK_EQUAL(&task1, k1->GetUserTask());
    CHECK_EQUAL(&task2, k2->GetUserTask());
    CHECK_EQUAL(&task3, k3->GetUserTask());

    // cursor is on task3, task3 yields -> task1 (wrap-around)
    CHECK_TRUE(strategy->OnTaskYield(k3));
    CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1 after task3 yielded");

    // task1 yields -> task2
    CHECK_TRUE(strategy->OnTaskYield(k1));
    CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "expecting task2 after task1 yielded");

    // task2 yields -> task3
    CHECK_TRUE(strategy->OnTaskYield(k2));
    CHECK_EQUAL_TEXT(&task3, strategy->GetNext()->GetUserTask(), "expecting task3 after task2 yielded");

    // yielding task stays runnable
    CHECK_EQUAL(3, strategy->GetSize());
}

// Yield by a task the cursor is NOT on: cursor is moved onto the yielding task.
TEST(SwitchStrategyRoundRobin, YieldNoSleepMovesCursorOntoYieldingTask)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    IKernelTask *k1 = strategy->GetNext(); // task1
    IKernelTask *k2 = strategy->GetNext(); // task2 (cursor on task2)
    CHECK_EQUAL(&task1, k1->GetUserTask());
    CHECK_EQUAL(&task2, k2->GetUserTask());

    // task1 yields while cursor is on task2: cursor jumps back to task1 -> task2 is selected again
    CHECK_TRUE(strategy->OnTaskYield(k1));
    CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "expecting task2 (successor of task1)");
    CHECK_EQUAL_TEXT(&task3, strategy->GetNext()->GetUserTask(), "expecting task3");
    CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1");
}

// Alone in the runnable list: yield is handled and the same task is selected again.
TEST(SwitchStrategyRoundRobin, YieldNoSleepSingleTask)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    IKernelTask *k1 = strategy->GetNext();
    CHECK_EQUAL(&task1, k1->GetUserTask());

    for (int32_t i = 0; i < 5; i++)
    {
        CHECK_TRUE(strategy->OnTaskYield(k1));
        CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "single task must be selected again after yield");
    }

    CHECK_EQUAL(1, strategy->GetSize());
}

// Two tasks ping-pong via yield without any of them being skipped or put to sleep.
TEST(SwitchStrategyRoundRobin, YieldNoSleepPingPong)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    IKernelTask *k1 = strategy->GetNext(); // task1
    IKernelTask *k2 = strategy->GetNext(); // task2
    CHECK_EQUAL(&task1, k1->GetUserTask());
    CHECK_EQUAL(&task2, k2->GetUserTask());

    for (int32_t i = 0; i < 4; i++)
    {
        CHECK_TRUE(strategy->OnTaskYield(k2));
        CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1 after task2 yielded");

        CHECK_TRUE(strategy->OnTaskYield(k1));
        CHECK_EQUAL_TEXT(&task2, strategy->GetNext()->GetUserTask(), "expecting task2 after task1 yielded");
    }

    CHECK_EQUAL(2, strategy->GetSize());
}

// Yield after the successor was removed: rotation continues with the next remaining task.
TEST(SwitchStrategyRoundRobin, YieldNoSleepAfterRemoval)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    IKernelTask *k1 = strategy->GetNext(); // task1
    CHECK_EQUAL(&task1, k1->GetUserTask());

    kernel.RemoveTask(&task2);

    // task1 yields: successor is now task3
    CHECK_TRUE(strategy->OnTaskYield(k1));
    CHECK_EQUAL_TEXT(&task3, strategy->GetNext()->GetUserTask(), "expecting task3 (task2 was removed)");
    CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "expecting task1 (wrap-around)");
}

// Task that is not in the runnable list (sleeping) is not handled: kernel falls back to legacy yield.
static struct RRYieldSleepingContext
{
    RRYieldSleepingContext()
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
g_RRYieldSleepingContext;

static void RRYieldSleepingRelaxCpu()
{
    g_RRYieldSleepingContext.Process();
}

TEST(SwitchStrategyRoundRobin, YieldNoSleepSleepingTaskNotHandled)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_RelaxCpuHandler = RRYieldSleepingRelaxCpu;
    g_RRYieldSleepingContext.Clear();
    g_RRYieldSleepingContext.platform = platform;
    g_RRYieldSleepingContext.strategy = kernel.GetSwitchStrategy();

    Sleep(2);

    g_RelaxCpuHandler = NULL;

    CHECK_TEXT(g_RRYieldSleepingContext.called, "expecting relax-cpu handler to be called while task sleeps");
    CHECK_EQUAL(1, g_RRYieldSleepingContext.size);
    CHECK_FALSE_TEXT(g_RRYieldSleepingContext.result, "sleeping task is not in runnable list: yield not handled");
}

} // namespace stk
} // namespace test
