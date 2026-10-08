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
        if ((counter == 0) || (counter == 1))
        {
            CHECK_EQUAL_TEXT(active->SP, (Word)task1->GetStack(), "sleep: expecting low-priority task1");
        }
        else
        // ISR calls OnSysTick (task1 = idle (lower priority), task2 = active (higher priority))
        if (counter == 2)
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

// ============================================================================ //
// ============================ OnTaskYield (FP32) ============================ //
// ============================================================================ //

// SwitchStrategyFP32 (NoSleepYield = false): OnTaskYield() must report "not handled" (false) so the
// kernel falls back to the legacy sleep-based yield, and must not touch the per-level cursor.
TEST(SwitchStrategyFixedPriority, YieldLegacyReturnsFalse)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyFP32, PlatformTestMock> kernel;
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

TEST(SwitchStrategyFixedPriority, YieldLegacySingleTaskReturnsFalse)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    IKernelTask *k1 = strategy->GetNext();

    CHECK_FALSE(strategy->OnTaskYield(k1));
    CHECK_EQUAL(&task1, strategy->GetNext()->GetUserTask());
}

// ============================================================================ //
// ========================== OnTaskYield (FP32_NSY) ========================== //
// ============================================================================ //

// Yield on the task the cursor is on: next task at the same level is returned.
TEST(SwitchStrategyFixedPriority, YieldNoSleepHandsToSuccessor)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyFP32_NSY, PlatformTestMock> kernel;
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
TEST(SwitchStrategyFixedPriority, YieldNoSleepMovesCursorOntoYieldingTask)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyFP32_NSY, PlatformTestMock> kernel;
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

// Alone at its level: yield is handled and the same task is selected again.
TEST(SwitchStrategyFixedPriority, YieldNoSleepSingleTask)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyFP32_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    IKernelTask *k1 = strategy->GetNext();
    CHECK_EQUAL(&task1, k1->GetUserTask());

    for (int32_t i = 0; i < 3; i++)
    {
        CHECK_TRUE(strategy->OnTaskYield(k1));
        CHECK_EQUAL_TEXT(&task1, strategy->GetNext()->GetUserTask(), "alone at level: expecting task1 again");
    }

    CHECK_EQUAL(1, strategy->GetSize());
}

// Yield never hands the CPU to a lower priority level, and a higher level still wins.
TEST(SwitchStrategyFixedPriority, YieldNoSleepNeverSelectsLowerPriority)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyFP32_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> low;
    TaskMockW<2, ACCESS_USER> high1, high2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&low);
    kernel.AddTask(&high1);
    kernel.AddTask(&high2);

    IKernelTask *kh1 = strategy->GetNext();
    IKernelTask *kh2 = strategy->GetNext();
    CHECK_EQUAL(&high1, kh1->GetUserTask());
    CHECK_EQUAL(&high2, kh2->GetUserTask());

    // high-priority tasks ping-pong via yield, low-priority task never runs
    for (int32_t i = 0; i < 4; i++)
    {
        CHECK_TRUE(strategy->OnTaskYield(kh2));
        CHECK_EQUAL_TEXT(&high1, strategy->GetNext()->GetUserTask(), "expecting high1");

        CHECK_TRUE(strategy->OnTaskYield(kh1));
        CHECK_EQUAL_TEXT(&high2, strategy->GetNext()->GetUserTask(), "expecting high2");
    }

    // once the high level is empty the low task is scheduled
    kernel.RemoveTask(&high1);
    kernel.RemoveTask(&high2);
    CHECK_EQUAL(&low, strategy->GetNext()->GetUserTask());
}

// Yielding low-priority task keeps its place in its own level only: higher level is still selected.
TEST(SwitchStrategyFixedPriority, YieldNoSleepLowPriorityYieldKeepsHigherSelected)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyFP32_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> low1, low2;
    TaskMockW<2, ACCESS_USER> high;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&low1);
    kernel.AddTask(&low2);
    kernel.AddTask(&high);

    IKernelTask *kh = strategy->GetNext();
    CHECK_EQUAL(&high, kh->GetUserTask());

    // use GetFirst() chain of the low level: remove high temporarily is not possible, so obtain low tasks
    // through the kernel by removing the high task and re-adding it afterwards
    kernel.RemoveTask(&high);
    IKernelTask *kl1 = strategy->GetNext();
    IKernelTask *kl2 = strategy->GetNext();
    CHECK_EQUAL(&low1, kl1->GetUserTask());
    CHECK_EQUAL(&low2, kl2->GetUserTask());
    kernel.AddTask(&high);

    CHECK_TRUE(strategy->OnTaskYield(kl2));
    CHECK_EQUAL_TEXT(&high, strategy->GetNext()->GetUserTask(), "higher level must still win after low-level yield");

    // after high leaves, the yield made at the low level is honored: low2 yielded -> low1 next
    kernel.RemoveTask(&high);
    CHECK_EQUAL_TEXT(&low1, strategy->GetNext()->GetUserTask(), "expecting low1 (successor of yielded low2)");
}

// Task that is not in its runnable list (sleeping) is not handled: kernel falls back to legacy yield.
static struct YieldSleepingContext
{
    YieldSleepingContext()
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
            // the only task is sleeping: runnable bitmap is empty and GetFirst() returns it from the sleep list
            IKernelTask *sleeping = strategy->GetFirst();

            size   = strategy->GetSize();
            result = strategy->OnTaskYield(sleeping);
            called = true;
        }

        platform->ProcessTick();

        ++counter;
    }
}
g_YieldSleepingContext;

static void YieldSleepingRelaxCpu()
{
    g_YieldSleepingContext.Process();
}

TEST(SwitchStrategyFixedPriority, YieldNoSleepSleepingTaskNotHandled)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyFP32_NSY, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_RelaxCpuHandler = YieldSleepingRelaxCpu;
    g_YieldSleepingContext.Clear();
    g_YieldSleepingContext.platform = platform;
    g_YieldSleepingContext.strategy = kernel.GetSwitchStrategy();

    Sleep(2);

    g_RelaxCpuHandler = NULL;

    CHECK_TEXT(g_YieldSleepingContext.called, "expecting relax-cpu handler to be called while task sleeps");
    CHECK_EQUAL(1, g_YieldSleepingContext.size);
    CHECK_FALSE_TEXT(g_YieldSleepingContext.result, "sleeping task is not in runnable list: yield not handled");
}

} // namespace stk
} // namespace test
