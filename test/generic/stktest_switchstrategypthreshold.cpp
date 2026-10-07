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
// ====================== SwitchStrategyPreemptionThreshold =================== //
// ============================================================================ //

//! Shorthand: weight of a task is MakeWeight(priority, threshold), higher number = higher priority.
typedef SwitchStrategyPT32 PTS;

TEST_GROUP(SwitchStrategyPreemptionThreshold)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.ExpectAssert(false);
        g_TestContext.RethrowAssertException(true);
    }
};

// ---------------------------------------------------------------------------- //
// Weight encoding helpers
// ---------------------------------------------------------------------------- //

TEST(SwitchStrategyPreemptionThreshold, MakeWeight)
{
    CHECK_EQUAL(0,                     PTS::MakeWeight(0, 0));
    CHECK_EQUAL((3 << 8) | 5,          PTS::MakeWeight(3, 5));
    CHECK_EQUAL((31 << 8) | 31,        PTS::MakeWeight(31, 31));

    // priority occupies the high bits: weights are ordered by priority first, threshold second
    CHECK_TRUE(PTS::MakeWeight(2, 2)  > PTS::MakeWeight(1, 31));
    CHECK_TRUE(PTS::MakeWeight(2, 5)  > PTS::MakeWeight(2, 3));
}

TEST(SwitchStrategyPreemptionThreshold, FromThreadX)
{
    // ThreadX: 0 = highest priority. STK: higher number = higher priority.
    CHECK_EQUAL(PTS::MakeWeight(31, 31), PTS::FromThreadX(0, 0));
    CHECK_EQUAL(PTS::MakeWeight(0, 0),   PTS::FromThreadX(31, 31));
    CHECK_EQUAL(PTS::MakeWeight(21, 26), PTS::FromThreadX(10, 5)); // threshold 5 is "more important" than priority 10
    CHECK_EQUAL(PTS::MakeWeight(16, 16), PTS::FromThreadX(15, 15));
}

// ---------------------------------------------------------------------------- //
// Empty / invalid input
// ---------------------------------------------------------------------------- //

TEST(SwitchStrategyPreemptionThreshold, GetFirstEmpty)
{
    SwitchStrategyPT32 pts;

    try
    {
        g_TestContext.ExpectAssert(true);
        pts.GetFirst();
        CHECK_TEXT(false, "expecting assertion when empty");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(SwitchStrategyPreemptionThreshold, GetNextEmpty)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&task1);
    kernel.RemoveTask(&task1);
    CHECK_EQUAL(0, strategy->GetSize());

    // expect to return NULL which puts core into a sleep mode
    CHECK_EQUAL(0, strategy->GetNext());
}

TEST(SwitchStrategyPreemptionThreshold, GetNextEmptyAfterHeldTaskRemoved)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> task1; // holds its threshold when selected
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);

    CHECK_EQUAL(&task1, strategy->GetNext()->GetUserTask()); // now held

    kernel.RemoveTask(&task1);
    CHECK_EQUAL(0, strategy->GetSize());
    CHECK_EQUAL(0, strategy->GetNext());
}

TEST(SwitchStrategyPreemptionThreshold, AddTaskThresholdBelowPriorityAsserts)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(3, 1), ACCESS_USER> task1; // threshold < priority is invalid

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task1);
        CHECK_TEXT(false, "expecting assertion for threshold < priority");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(SwitchStrategyPreemptionThreshold, AddTaskPriorityOutOfRangeAsserts)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(32, 32), ACCESS_USER> task1; // MAX_PRIORITIES is 32: valid range is [0, 31]

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task1);
        CHECK_TEXT(false, "expecting assertion for priority out of range");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

// ---------------------------------------------------------------------------- //
// Behaviour with threshold == priority (plain fixed-priority + round-robin)
// ---------------------------------------------------------------------------- //

TEST(SwitchStrategyPreemptionThreshold, GetFirstHighestPriority)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> low;
    TaskMockW<PTS::MakeWeight(3, 3), ACCESS_USER> high;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> mid;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&low);
    kernel.AddTask(&high);
    kernel.AddTask(&mid);

    CHECK_EQUAL_TEXT(&high, strategy->GetFirst()->GetUserTask(), "expecting highest priority task first");
}

TEST(SwitchStrategyPreemptionThreshold, EndlessNext)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1, task2, task3; // same level, no protected band
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

TEST(SwitchStrategyPreemptionThreshold, Algorithm)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1, task2, task3;

    kernel.Initialize();

    kernel.AddTask(&task1);

    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    IKernelTask *next = strategy->GetFirst();

    // --- Stage 1: 1 task only ---------------------------------------------

    for (int32_t i = 0; i < 5; i++)
    {
        next = strategy->GetNext();
        CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Single task must always be selected");
    }

    // --- Stage 2: add second task -----------------------------------------

    kernel.AddTask(&task2);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should be task1");
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "Next task should be task2");
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should wrap to task1");

    // --- Stage 3: add third task ------------------------------------------

    kernel.AddTask(&task3);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task2, next->GetUserTask(), "Next task should be task2");
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Next task should be task3");
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should wrap to task1");

    // --- Stage 4: remove a task -------------------------------------------

    kernel.RemoveTask(&task2);

    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Next task should be task3 after removal");
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task1, next->GetUserTask(), "Next task should be task1 after removal");
    next = strategy->GetNext();
    CHECK_EQUAL_TEXT(&task3, next->GetUserTask(), "Next task should wrap to task3");
}

TEST(SwitchStrategyPreemptionThreshold, HigherPriorityAlwaysWins)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> low;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> high;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&low);
    kernel.AddTask(&high);

    for (int32_t i = 0; i < 5; i++)
    {
        CHECK_EQUAL_TEXT(&high, strategy->GetNext()->GetUserTask(), "low priority task must not run while high is ready");
    }

    kernel.RemoveTask(&high);

    CHECK_EQUAL_TEXT(&low, strategy->GetNext()->GetUserTask(), "expecting low priority task once high is gone");
}

// ---------------------------------------------------------------------------- //
// Preemption-threshold behaviour
// ---------------------------------------------------------------------------- //

// A running task with threshold T can be preempted only by tasks with priority > T.
// Tasks in the band (priority, T] must wait.
TEST(SwitchStrategyPreemptionThreshold, ThresholdBlocksPreemptionInBand)
{
    Kernel<KERNEL_DYNAMIC, 4, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> a;  // priority 1, threshold 3
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> b;  // inside band
    TaskMockW<PTS::MakeWeight(3, 3), ACCESS_USER> d;  // exactly at threshold: still blocked
    TaskMockW<PTS::MakeWeight(4, 4), ACCESS_USER> c;  // above threshold: preempts
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&a);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "expecting a (now holds its threshold)");

    kernel.AddTask(&b);
    for (int32_t i = 0; i < 3; i++)
    {
        CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "b (priority 2 <= threshold 3) must not preempt a");
    }

    kernel.AddTask(&d);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "d (priority 3 == threshold 3) must not preempt a");

    kernel.AddTask(&c);
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "c (priority 4 > threshold 3) must preempt a");
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "c keeps running");
}

// Preempted held task is resumed before ready tasks inside its protected band.
TEST(SwitchStrategyPreemptionThreshold, HeldTaskResumesAfterPreemptor)
{
    Kernel<KERNEL_DYNAMIC, 4, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> a;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> b;
    TaskMockW<PTS::MakeWeight(3, 3), ACCESS_USER> d;
    TaskMockW<PTS::MakeWeight(4, 4), ACCESS_USER> c;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&a);
    CHECK_EQUAL(&a, strategy->GetNext()->GetUserTask());

    kernel.AddTask(&b);
    kernel.AddTask(&d);
    kernel.AddTask(&c);
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "c preempts a");

    kernel.RemoveTask(&c);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a resumes before b and d (inside its band)");
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a keeps running");

    kernel.RemoveTask(&d);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a still held, b waits");

    // removing the holder releases the hold: next in priority order runs
    kernel.RemoveTask(&a);
    CHECK_EQUAL_TEXT(&b, strategy->GetNext()->GetUserTask(), "b runs once a is gone");
}

// A held task is never time-sliced: same-priority peers wait until it leaves.
TEST(SwitchStrategyPreemptionThreshold, HeldTaskNotTimeSliced)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> a;  // holds its threshold
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> peer;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&a);
    kernel.AddTask(&peer);

    for (int32_t i = 0; i < 5; i++)
    {
        CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "held task must not be time-sliced");
    }

    kernel.RemoveTask(&a);

    CHECK_EQUAL_TEXT(&peer, strategy->GetNext()->GetUserTask(), "peer runs after the holder is removed");
    CHECK_EQUAL_TEXT(&peer, strategy->GetNext()->GetUserTask(), "peer alone at its level");
}

// A task with threshold == priority never holds: same-priority peers are round-robin.
TEST(SwitchStrategyPreemptionThreshold, NoHoldWhenThresholdEqualsPriority)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> a, b;
    TaskMockW<PTS::MakeWeight(3, 3), ACCESS_USER> high;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&a);
    kernel.AddTask(&b);

    CHECK_EQUAL(&a, strategy->GetNext()->GetUserTask());
    CHECK_EQUAL(&b, strategy->GetNext()->GetUserTask());
    CHECK_EQUAL(&a, strategy->GetNext()->GetUserTask());

    // higher priority task preempts immediately
    kernel.AddTask(&high);
    CHECK_EQUAL(&high, strategy->GetNext()->GetUserTask());
}

// Nested holds: the highest-level holder defines the protected band.
TEST(SwitchStrategyPreemptionThreshold, NestedHolds)
{
    Kernel<KERNEL_DYNAMIC, 4, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> a;  // holds band [1..3]
    TaskMockW<PTS::MakeWeight(4, 5), ACCESS_USER> c;  // preempts a, holds band [4..5]
    TaskMockW<PTS::MakeWeight(5, 5), ACCESS_USER> x;  // inside c's band
    TaskMockW<PTS::MakeWeight(6, 6), ACCESS_USER> y;  // above c's threshold
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&a);
    CHECK_EQUAL(&a, strategy->GetNext()->GetUserTask());

    kernel.AddTask(&c);
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "c preempts a");

    kernel.AddTask(&x);
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "x (5 <= threshold 5) must not preempt c");

    kernel.AddTask(&y);
    CHECK_EQUAL_TEXT(&y, strategy->GetNext()->GetUserTask(), "y (6 > threshold 5) must preempt c");

    kernel.RemoveTask(&y);
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "c resumes");

    kernel.RemoveTask(&x);
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "c still holds");

    kernel.RemoveTask(&c);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a resumes as the outer holder");
}

// ---------------------------------------------------------------------------- //
// OnTaskYield
// ---------------------------------------------------------------------------- //

TEST(SwitchStrategyPreemptionThreshold, YieldHandsToSuccessor)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1, task2, task3;
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

TEST(SwitchStrategyPreemptionThreshold, YieldMovesCursorOntoYieldingTask)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1, task2, task3;
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

TEST(SwitchStrategyPreemptionThreshold, YieldSingleTask)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1;
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

TEST(SwitchStrategyPreemptionThreshold, YieldNeverSelectsLowerPriority)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> low;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> high1, high2;
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

    kernel.RemoveTask(&high1);
    kernel.RemoveTask(&high2);
    CHECK_EQUAL(&low, strategy->GetNext()->GetUserTask());
}

// Yield releases the yielder's hold so same-priority peers are not blocked by it.
TEST(SwitchStrategyPreemptionThreshold, YieldReleasesHoldForPeer)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> a;    // holds its threshold when selected
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> peer;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&a);
    kernel.AddTask(&peer);

    IKernelTask *ka = strategy->GetNext();
    CHECK_EQUAL(&a, ka->GetUserTask());
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a is held: it is selected again");

    CHECK_TRUE(strategy->OnTaskYield(ka));
    CHECK_EQUAL_TEXT(&peer, strategy->GetNext()->GetUserTask(), "peer runs after a yielded");

    // rotation continues and a is held again when it is re-selected
    CHECK_EQUAL_TEXT(&a,    strategy->GetNext()->GetUserTask(), "a is selected by round-robin");
    CHECK_EQUAL_TEXT(&a,    strategy->GetNext()->GetUserTask(), "a holds its threshold again");
}

// Yield releases the hold: a ready task inside the former protected band may run.
TEST(SwitchStrategyPreemptionThreshold, YieldReleasesHoldForTaskInBand)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> a;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> b;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&a);

    IKernelTask *ka = strategy->GetNext();
    CHECK_EQUAL(&a, ka->GetUserTask());

    kernel.AddTask(&b);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "b is blocked by a's threshold");

    CHECK_TRUE(strategy->OnTaskYield(ka));
    CHECK_EQUAL_TEXT(&b, strategy->GetNext()->GetUserTask(), "b runs once a yielded");
    CHECK_EQUAL_TEXT(&b, strategy->GetNext()->GetUserTask(), "a (lower priority) does not run while b is ready");

    kernel.RemoveTask(&b);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a runs when b is gone");
}

// Sleeping task is not in its runnable list: yield is not handled (kernel falls back to a sleep-yield).
// Note: Sleep() only marks the task as sleep-pending, the strategy gets OnTaskSleep() on the entry tick,
// so the check is done on the 2nd relax-CPU call.
static struct PTYieldSleepingContext
{
    PTYieldSleepingContext()
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
        if (counter == 1)
        {
            // the only task is sleeping: ready bitmap is empty and GetFirst() returns it from the sleep list
            IKernelTask *sleeping = strategy->GetFirst();

            size   = strategy->GetSize();
            result = strategy->OnTaskYield(sleeping);
            called = true;
        }

        platform->ProcessTick();

        ++counter;
    }
}
g_PTYieldSleepingContext;

static void PTYieldSleepingRelaxCpu()
{
    g_PTYieldSleepingContext.Process();
}

TEST(SwitchStrategyPreemptionThreshold, YieldSleepingTaskNotHandled)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_RelaxCpuHandler = PTYieldSleepingRelaxCpu;
    g_PTYieldSleepingContext.Clear();
    g_PTYieldSleepingContext.platform = platform;
    g_PTYieldSleepingContext.strategy = kernel.GetSwitchStrategy();

    Sleep(2);

    g_RelaxCpuHandler = NULL;

    CHECK_TEXT(g_PTYieldSleepingContext.called, "expecting relax-cpu handler to be called while task sleeps");
    CHECK_EQUAL(1, g_PTYieldSleepingContext.size);
    CHECK_FALSE_TEXT(g_PTYieldSleepingContext.result, "sleeping task is not in runnable list: yield not handled");
}

// ---------------------------------------------------------------------------- //
// OnTaskWeightChange (priority inheritance / restore)
// ---------------------------------------------------------------------------- //

// Inherited priority moves the task to the new level, restoring moves it back.
TEST(SwitchStrategyPreemptionThreshold, WeightChangeMovesTaskBetweenLevels)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> lo;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> mid;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&lo);
    IKernelTask *klo = strategy->GetNext();
    CHECK_EQUAL(&lo, klo->GetUserTask());

    kernel.AddTask(&mid);
    CHECK_EQUAL_TEXT(&mid, strategy->GetNext()->GetUserTask(), "mid has higher priority");

    // boost lo above mid (priority inheritance)
    Weight old_weight = klo->GetWeight();
    klo->SetCurrentWeight(PTS::MakeWeight(3, 3));
    strategy->OnTaskWeightChange(klo, old_weight);

    CHECK_EQUAL(2, strategy->GetSize());
    CHECK_EQUAL_TEXT(&lo, strategy->GetNext()->GetUserTask(), "boosted lo must run before mid");
    CHECK_EQUAL_TEXT(&lo, strategy->GetNext()->GetUserTask(), "boosted lo is alone at its level");

    // restore
    old_weight = klo->GetWeight();
    klo->SetCurrentWeight(NO_WEIGHT);
    strategy->OnTaskWeightChange(klo, old_weight);

    CHECK_EQUAL(2, strategy->GetSize());
    CHECK_EQUAL_TEXT(&mid, strategy->GetNext()->GetUserTask(), "mid runs again after lo's boost is gone");
}

// Inherited weight contributes only its priority: effective threshold is
// max(configured threshold, current priority), the waiter's threshold is ignored.
TEST(SwitchStrategyPreemptionThreshold, WeightChangeKeepsConfiguredThresholdAndMigratesHold)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 4), ACCESS_USER> a;  // configured band [1..4]
    TaskMockW<PTS::MakeWeight(4, 4), ACCESS_USER> c;
    TaskMockW<PTS::MakeWeight(5, 5), ACCESS_USER> d;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&a);
    IKernelTask *ka = strategy->GetNext(); // a becomes held on level 1
    CHECK_EQUAL(&a, ka->GetUserTask());

    // boost a to priority 3 with a waiter threshold that must be ignored (31)
    Weight old_weight = ka->GetWeight();
    ka->SetCurrentWeight(PTS::MakeWeight(3, 31));
    strategy->OnTaskWeightChange(ka, old_weight);

    // the hold migrated to level 3, effective threshold = max(4, 3) = 4
    kernel.AddTask(&c);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "c (4 <= threshold 4) must not preempt boosted a");

    kernel.AddTask(&d);
    CHECK_EQUAL_TEXT(&d, strategy->GetNext()->GetUserTask(), "d (5 > threshold 4) must preempt boosted a");

    kernel.RemoveTask(&d);
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "boosted a resumes");

    // restore: hold migrates back to level 1 (configured threshold 4 still protects against c)
    old_weight = ka->GetWeight();
    ka->SetCurrentWeight(NO_WEIGHT);
    strategy->OnTaskWeightChange(ka, old_weight);

    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "restored a keeps its configured threshold");

    kernel.RemoveTask(&a);
    CHECK_EQUAL_TEXT(&c, strategy->GetNext()->GetUserTask(), "c runs once a is gone");
}

// Hold follows a task to the new priority level: peers at the new level do not time-slice it.
TEST(SwitchStrategyPreemptionThreshold, WeightChangeHoldMigratedToNewLevel)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> a;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> peer;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&a);
    IKernelTask *ka = strategy->GetNext(); // held on level 1
    CHECK_EQUAL(&a, ka->GetUserTask());

    // boost a to priority 2 (threshold stays 3)
    const Weight old_weight = ka->GetWeight();
    ka->SetCurrentWeight(PTS::MakeWeight(2, 2));
    strategy->OnTaskWeightChange(ka, old_weight);

    kernel.AddTask(&peer);

    // if the hold was migrated, a is not time-sliced with its new level peer
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a runs");
    CHECK_EQUAL_TEXT(&a, strategy->GetNext()->GetUserTask(), "a is still held at the new level (no time-slicing)");

    kernel.RemoveTask(&a);
    CHECK_EQUAL_TEXT(&peer, strategy->GetNext()->GetUserTask(), "peer runs after a is gone");
}

// A boost without a protected band does not create a hold.
TEST(SwitchStrategyPreemptionThreshold, WeightChangeWithoutBandDoesNotHold)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> a;
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> peer;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    kernel.AddTask(&a);
    IKernelTask *ka = strategy->GetNext();
    CHECK_EQUAL(&a, ka->GetUserTask());

    const Weight old_weight = ka->GetWeight();
    ka->SetCurrentWeight(PTS::MakeWeight(2, 2));
    strategy->OnTaskWeightChange(ka, old_weight);

    kernel.AddTask(&peer);

    // a and peer are plain round-robin peers at level 2 (cursor was moved to the tail on AddTask)
    IKernelTask *n1 = strategy->GetNext();
    IKernelTask *n2 = strategy->GetNext();
    CHECK_TRUE_TEXT(n1->GetUserTask() != n2->GetUserTask(), "equal-priority tasks without a band must alternate");
}

// ---------------------------------------------------------------------------- //
// Integration with the kernel (tick-driven)
// ---------------------------------------------------------------------------- //

static struct PTPrioritySleepContext
{
    PTPrioritySleepContext()
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
g_PTPrioritySleepContext;

static void PTPrioritySleepRelaxCpu()
{
    g_PTPrioritySleepContext.Process();
}

TEST(SwitchStrategyPreemptionThreshold, Priority)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1; // low priority
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> task2; // high priority
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting high-priority task2 on start");

    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting task2");

    g_RelaxCpuHandler = PTPrioritySleepRelaxCpu;
    g_PTPrioritySleepContext.Clear();
    g_PTPrioritySleepContext.platform = platform;
    g_PTPrioritySleepContext.task1    = &task1;
    g_PTPrioritySleepContext.task2    = &task2;

    // task2 calls Sleep to become idle
    Sleep(2);

    // task2 is active again
    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting high-priority task2 again after it slept");

    // ISR calls OnSysTick, higher priority task2 is scheduled
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting high-priority task2 again");

    g_RelaxCpuHandler = NULL;
}

// Same as above but the low-priority task has a threshold covering the high-priority task:
// once it is scheduled (while the high-priority task sleeps) it holds the CPU even after
// the high-priority task wakes up.
static struct PTThresholdSleepContext
{
    PTThresholdSleepContext()
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

        // task1 (low priority, threshold 3) runs while task2 sleeps and keeps running after task2 woke up
        // (counter == 2 is the tick on which task2 becomes runnable again)
        CHECK_EQUAL_TEXT(active->SP, (Word)task1->GetStack(), "threshold: expecting task1 holding its threshold");

        ++counter;
    }
}
g_PTThresholdSleepContext;

static void PTThresholdSleepRelaxCpu()
{
    g_PTThresholdSleepContext.Process();
}

TEST(SwitchStrategyPreemptionThreshold, ThresholdHoldsAfterHigherPriorityTaskWakes)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 3), ACCESS_USER> task1; // low priority, threshold covers task2
    TaskMockW<PTS::MakeWeight(2, 2), ACCESS_USER> task2; // priority inside task1's band
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL_TEXT(active->SP, (Word)task2.GetStack(), "expecting task2 (highest ready priority) on start");

    g_RelaxCpuHandler = PTThresholdSleepRelaxCpu;
    g_PTThresholdSleepContext.Clear();
    g_PTThresholdSleepContext.platform = platform;
    g_PTThresholdSleepContext.task1    = &task1;
    g_PTThresholdSleepContext.task2    = &task2;

    // task2 calls Sleep: task1 is scheduled and holds its threshold
    Sleep(2);

    // task2 woke up, but its priority (2) is not above task1's threshold (3)
    CHECK_EQUAL_TEXT(active->SP, (Word)task1.GetStack(), "expecting task1 to keep the CPU after task2 woke up");

    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (Word)task1.GetStack(), "expecting task1 (not time-sliced, task2 is in its band)");

    g_RelaxCpuHandler = NULL;
}

// Run-time threshold change of the running task (tx_thread_preemption_change semantics).
TEST(SwitchStrategyPreemptionThreshold, ChangeThresholdOfRunningTask)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1, task2; // round-robin peers
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // make sure the strategy has selected the running task through GetNext()
    platform->ProcessTick();

    ITask *running = (active->SP == (Word)task1.GetStack()) ? static_cast<ITask *>(&task1) : static_cast<ITask *>(&task2);
    ITask *other   = (running == &task1)                    ? static_cast<ITask *>(&task2) : static_cast<ITask *>(&task1);

    IKernelService *service = IKernelService::GetInstance();

    // raise the threshold: the running task protects itself from its same-level peer
    CHECK_EQUAL_TEXT(1, SwitchStrategyPT32::ChangeThreshold(*service, service->GetTid(), 1, 2), "expecting previous threshold 1");

    for (int32_t i = 0; i < 3; i++)
    {
        platform->ProcessTick();
        CHECK_EQUAL_TEXT(active->SP, (Word)running->GetStack(), "raised threshold: running task must not be time-sliced");
    }

    // threshold equal to the priority releases the protection
    CHECK_EQUAL_TEXT(2, SwitchStrategyPT32::ChangeThreshold(*service, service->GetTid(), 1, 1), "expecting previous threshold 2");

    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (Word)other->GetStack(), "released: peer must be scheduled");
}

// RemoveTask() of a task that sits in the sleep list (m_sleep.Unlink() branch).
// Note: Sleep() only marks the task as sleep-pending, the strategy gets OnTaskSleep() on the entry tick,
// so the check is done on the 2nd relax-CPU call, when the task is already in the strategy's sleep list.
// The task is put back into the sleep list afterwards so the kernel can wake it up as usual.
static struct PTRemoveSleepingContext
{
    PTRemoveSleepingContext()
    {
        Clear();
    }

    void Clear()
    {
        counter       = 0;
        called        = false;
        task          = NULL;
        size_before   = 0;
        size_removed  = 0;
        size_readded  = 0;
        size_final    = 0;
        next_removed  = (IKernelTask *)1; // sentinel, must be overwritten with NULL
        platform      = NULL;
        strategy      = NULL;
    }

    uint32_t             counter;
    bool                 called;
    ITask               *task;
    size_t               size_before;
    size_t               size_removed;
    size_t               size_readded;
    size_t               size_final;
    IKernelTask         *next_removed;
    PlatformTestMock    *platform;
    ITaskSwitchStrategy *strategy;

    void Process()
    {
        if (counter == 1)
        {
            // the only task is sleeping: GetFirst() returns it from the sleep list
            IKernelTask *sleeping = strategy->GetFirst();
            CHECK_EQUAL(task, sleeping->GetUserTask());

            size_before = strategy->GetSize();

            // task is in m_sleep: RemoveTask() must unlink it from there
            strategy->RemoveTask(sleeping);
            size_removed = strategy->GetSize();
            next_removed = strategy->GetNext(); // nothing runnable and nothing sleeping

            // restore the original state: runnable first, then back to the sleep list
            strategy->AddTask(sleeping);
            size_readded = strategy->GetSize();
            strategy->OnTaskSleep(sleeping);
            size_final = strategy->GetSize();

            called = true;
        }

        platform->ProcessTick();

        ++counter;
    }
}
g_PTRemoveSleepingContext;

static void PTRemoveSleepingRelaxCpu()
{
    g_PTRemoveSleepingContext.Process();
}

TEST(SwitchStrategyPreemptionThreshold, RemoveSleepingTask)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyPT32, PlatformTestMock> kernel;
    TaskMockW<PTS::MakeWeight(1, 1), ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_RelaxCpuHandler = PTRemoveSleepingRelaxCpu;
    g_PTRemoveSleepingContext.Clear();
    g_PTRemoveSleepingContext.platform = platform;
    g_PTRemoveSleepingContext.strategy = kernel.GetSwitchStrategy();
    g_PTRemoveSleepingContext.task     = &task1;

    Sleep(3);

    g_RelaxCpuHandler = NULL;

    CHECK_TEXT(g_PTRemoveSleepingContext.called, "expecting relax-cpu handler to be called while task sleeps");
    CHECK_EQUAL(1, g_PTRemoveSleepingContext.size_before);
    CHECK_EQUAL_TEXT(0, g_PTRemoveSleepingContext.size_removed, "sleeping task must be removed from the strategy");
    CHECK_TEXT(g_PTRemoveSleepingContext.next_removed == NULL, "nothing to schedule after the sleeping task was removed");
    CHECK_EQUAL(1, g_PTRemoveSleepingContext.size_readded);
    CHECK_EQUAL(1, g_PTRemoveSleepingContext.size_final);
}

} // namespace test
} // namespace stk
