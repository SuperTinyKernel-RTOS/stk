/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include <sync/stk_sync_barrier.h>
#include <sync/stk_sync_mutex.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_BARRIER_TEST_TASKS_MAX     5
#define _STK_BARRIER_TEST_TIMEOUT       1000
#define _STK_BARRIER_TEST_SHORT_SLEEP   10
#define _STK_BARRIER_TEST_LONG_SLEEP    100
#define _STK_BARRIER_STRESS_ITERATIONS  100
#ifdef __ARM_ARCH_6M__
#define _STK_BARRIER_STACK_SIZE         128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_BARRIER_STACK_SIZE         256
#define STK_TASK                        static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::barrier
    \brief     Namespace of Barrier test.
 */
namespace barrier {

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_ArrivedCount = 0;
static volatile int32_t g_TrueCount = 0;
static volatile int64_t g_ElapsedMs = 0;
static volatile int32_t g_InstancesDone = 0;
static volatile int32_t g_ArrivalSnapshot[_STK_BARRIER_TEST_TASKS_MAX] = {0};
static volatile int32_t g_Phase1[_STK_BARRIER_TEST_TASKS_MAX] = {0};
static volatile int32_t g_Phase2Sum[_STK_BARRIER_TEST_TASKS_MAX] = {0};

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_BARRIER_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test barriers - one per distinct group size exercised by the suite. Each is naturally left in
// its reset state (m_count == m_threshold) once every participating task has completed its final
// round, so - like g_TestMutex in the mutex suite - none of these are reconstructed between tests.
static sync::Barrier g_TestBarrier5(_STK_BARRIER_TEST_TASKS_MAX); // full pool (tests 1,2,3,5,7,8)
static sync::Barrier g_TestBarrier3(3);                           // partial group of 3 (test 6)
static sync::Barrier g_TestBarrier2(2);                           // partial group of 2 (test 4)

// Helper mutex used only for test bookkeeping (protecting shared counters/arrays below);
// it is NOT the primitive under test - Barrier itself provides no mutual exclusion guarantee.
static sync::Mutex g_CounterMtx;

/*! \class BasicRendezvousTask
    \brief Tests the core barrier property: no task proceeds past Wait() until all have arrived.
    \note  Verifies mutual synchronization, not mutual exclusion.
*/
template <EAccessMode _AccessMode>
class BasicRendezvousTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BasicRendezvousTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        g_CounterMtx.Lock();
        ++g_ArrivedCount;
        g_CounterMtx.Unlock();

        g_TestBarrier5.Wait();

        // By the time Wait() returns for ANY task, every task must already have
        // incremented g_ArrivedCount - otherwise the barrier released early.
        g_CounterMtx.Lock();
        g_ArrivalSnapshot[m_task_id] = g_ArrivedCount;
        g_CounterMtx.Unlock();

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            bool ok = true;
            for (int32_t i = 0; i < _STK_BARRIER_TEST_TASKS_MAX; ++i)
            {
                if (g_ArrivalSnapshot[i] != _STK_BARRIER_TEST_TASKS_MAX)
                {
                    ok = false;
                    printf("Rendezvous violation: task %d saw arrival count %d (expected %d)\n",
                        (int)i, (int)g_ArrivalSnapshot[i], (int)_STK_BARRIER_TEST_TASKS_MAX);
                }
            }

            printf("basic rendezvous: all snapshots == %d ? %s\n",
                (int)_STK_BARRIER_TEST_TASKS_MAX, ok ? "yes" : "no");

            g_TestResult = ok ? 1 : 0;
        }
    }
};

/*! \class LastReturnsTrueTask
    \brief Tests the Wait() return-value contract.
    \note  Verifies that exactly one of the N tasks - the one that trips the barrier - gets true.
*/
template <EAccessMode _AccessMode>
class LastReturnsTrueTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    LastReturnsTrueTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        bool was_last = g_TestBarrier5.Wait();

        if (was_last)
        {
            g_CounterMtx.Lock();
            ++g_TrueCount;
            g_CounterMtx.Unlock();
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            printf("last-returns-true: count=%d (expected 1)\n", (int)g_TrueCount);

            g_TestResult = (g_TrueCount == 1) ? 1 : 0;
        }
    }
};

/*! \class CyclicReuseTask
    \brief Tests that the barrier resets itself and can be reused across multiple rounds.
    \note  Verifies the generation counter: exactly one true per round, over several rounds.
*/
template <EAccessMode _AccessMode>
class CyclicReuseTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    enum { ROUNDS = 5 };

public:
    CyclicReuseTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        for (int32_t round = 0; round < ROUNDS; ++round)
        {
            bool was_last = g_TestBarrier5.Wait();

            if (was_last)
            {
                g_CounterMtx.Lock();
                ++g_TrueCount;
                g_CounterMtx.Unlock();
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            printf("cyclic reuse: true_count=%d (expected %d)\n", (int)g_TrueCount, (int)ROUNDS);

            g_TestResult = (g_TrueCount == ROUNDS) ? 1 : 0;
        }
    }
};

/*! \class BlocksUntilLastArrivesTask
    \brief Tests that Wait() actually blocks rather than returning immediately.
    \note  Verifies elapsed time, analogous to the TimedLock timing check in the mutex suite.
*/
template <EAccessMode _AccessMode>
class BlocksUntilLastArrivesTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BlocksUntilLastArrivesTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: arrive late, after task 1 is already blocked in Wait()
            stk::Sleep(_STK_BARRIER_TEST_LONG_SLEEP);
            g_TestBarrier2.Wait();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: arrive immediately and measure how long Wait() actually blocks
            int64_t start = GetTimeNowMs();
            g_TestBarrier2.Wait();
            int64_t elapsed = GetTimeNowMs() - start;

            g_ElapsedMs = elapsed;

            printf("blocks-until-last-arrives: elapsed=%d ms (expected >= %d)\n",
                (int)elapsed, (int)(_STK_BARRIER_TEST_LONG_SLEEP - _STK_BARRIER_TEST_SHORT_SLEEP));

            g_TestResult = (elapsed >= (_STK_BARRIER_TEST_LONG_SLEEP - _STK_BARRIER_TEST_SHORT_SLEEP)) ? 1 : 0;
        }
        // Tasks 2+ are not used by this test

        ++g_InstancesDone;
    }
};

/*! \class DataVisibilityTask
    \brief Tests that Wait() acts as a memory fence for the group.
    \note  Verifies a value written before Wait() is visible to every task after Wait() returns.
*/
template <EAccessMode _AccessMode>
class DataVisibilityTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    DataVisibilityTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        // Phase 1: publish a per-task value
        g_Phase1[m_task_id] = m_task_id + 1;

        g_TestBarrier5.Wait();

        // Phase 2: every task must now be able to see its neighbor's phase-1 value
        int32_t neighbor = (m_task_id + 1) % _STK_BARRIER_TEST_TASKS_MAX;
        g_Phase2Sum[m_task_id] = g_Phase1[m_task_id] + g_Phase1[neighbor];

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            bool ok = true;
            for (int32_t i = 0; i < _STK_BARRIER_TEST_TASKS_MAX; ++i)
            {
                int32_t neighbor_i = (i + 1) % _STK_BARRIER_TEST_TASKS_MAX;
                int32_t expected = (i + 1) + (neighbor_i + 1);

                if (g_Phase2Sum[i] != expected)
                {
                    ok = false;
                    printf("Visibility violation: task %d sum=%d (expected %d)\n",
                        (int)i, (int)g_Phase2Sum[i], (int)expected);
                }
            }

            printf("data visibility: all neighbor sums correct ? %s\n", ok ? "yes" : "no");

            g_TestResult = ok ? 1 : 0;
        }
    }
};

/*! \class PartialGroupThresholdTask
    \brief Tests a barrier whose threshold is smaller than the full task pool.
    \note  Verifies correctness when only a subset of the kernel's tasks form the barrier group.
*/
template <EAccessMode _AccessMode>
class PartialGroupThresholdTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    static const int32_t GROUP_SIZE = 3;

public:
    PartialGroupThresholdTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        bool was_last = g_TestBarrier3.Wait();

        g_CounterMtx.Lock();
        ++g_SharedCounter;
        if (was_last)
            ++g_TrueCount;
        g_CounterMtx.Unlock();

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < GROUP_SIZE)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            printf("partial group (size %d): counter=%d, true_count=%d\n",
                (int)GROUP_SIZE, (int)g_SharedCounter, (int)g_TrueCount);

            g_TestResult = ((g_SharedCounter == GROUP_SIZE) && (g_TrueCount == 1)) ? 1 : 0;
        }
    }
};

/*! \class StaggeredArrivalTask
    \brief Tests barrier correctness when tasks arrive at each round with different, staggered delays.
    \note  Verifies no round's counter leaks into another generation under uneven arrival timing.
*/
template <EAccessMode _AccessMode>
class StaggeredArrivalTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    enum { ROUNDS = 3 };

public:
    StaggeredArrivalTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        for (int32_t round = 0; round < ROUNDS; ++round)
        {
            // Stagger each task's arrival within the round by a task-id-dependent delay
            stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP * m_task_id);

            g_TestBarrier5.Wait();

            g_CounterMtx.Lock();
            ++g_SharedCounter;
            g_CounterMtx.Unlock();

            stk::Yield();
        }

        ++g_InstancesDone;

        if (m_task_id == (_STK_BARRIER_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            int32_t expected = ROUNDS * _STK_BARRIER_TEST_TASKS_MAX;

            printf("staggered arrival: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            g_TestResult = (g_SharedCounter == expected) ? 1 : 0;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test with many consecutive barrier rounds.
    \note  Verifies barrier stability under heavy, repeated full-group synchronization.
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    StressTestTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            g_TestBarrier5.Wait();

            g_CounterMtx.Lock();
            ++g_SharedCounter;
            g_CounterMtx.Unlock();

            if ((i % 20) == 0)
                stk::Delay(1);
        }

        ++g_InstancesDone;

        // Last task verifies total
        if (m_task_id == (_STK_BARRIER_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            int32_t expected = m_iterations * _STK_BARRIER_TEST_TASKS_MAX;

            printf("Stress test: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            g_TestResult = (g_SharedCounter == expected) ? 1 : 0;
        }
    }
};

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult = 0;
    g_SharedCounter = 0;
    g_ArrivedCount = 0;
    g_TrueCount = 0;
    g_ElapsedMs = 0;
    g_InstancesDone = 0;

    for (int32_t i = 0; i < _STK_BARRIER_TEST_TASKS_MAX; ++i)
    {
        g_ArrivalSnapshot[i] = 0;
        g_Phase1[i] = 0;
        g_Phase2Sum[i] = 0;
    }
}

} // namespace barrier
} // namespace test
} // namespace stk

/*! \fn    NeedsExtendedTasks
    \brief Returns true if the test requires tasks 3 and 4.
    \note  BlocksUntilLastArrives uses only tasks 0-1; PartialGroupThreshold uses only tasks 0-2.
*/
static bool NeedsExtendedTasks(const char *test_name)
{
    return  (strcmp(test_name, "BlocksUntilLastArrives")  != 0) &&
            (strcmp(test_name, "PartialGroupThreshold")   != 0);
}

/*! \fn    NeedsThreeTasks
    \brief Returns true if the test requires at least 3 tasks (0-2).
    \note  BlocksUntilLastArrives uses only tasks 0-1, so task 2 is also unnecessary.
*/
static bool NeedsThreeTasks(const char *test_name)
{
    return  (strcmp(test_name, "BlocksUntilLastArrives") != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::barrier;

    printf("Test: %s\n", test_name);

    ResetTestState();

    // Create tasks based on test type
    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_Kernel.AddTask(&task0);
    g_Kernel.AddTask(&task1);

    if (NeedsThreeTasks(test_name))
        g_Kernel.AddTask(&task2);

    if (NeedsExtendedTasks(test_name))
    {
        g_Kernel.AddTask(&task3);
        g_Kernel.AddTask(&task4);
    }

    g_Kernel.Start();

    int32_t result = (g_TestResult ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("Result: %s\n", result == TestContext::SUCCESS_EXIT_CODE ? "PASS" : "FAIL");
    printf("--------------\n");

    return result;
}

/*! \fn    main
    \brief Entry to the test suite.
*/
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    using namespace stk::test::barrier;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Basic rendezvous - no task proceeds until all have arrived
    if (RunTest<BasicRendezvousTask<ACCESS_PRIVILEGED>>("BasicRendezvous") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Exactly one Wait() call returns true
    if (RunTest<LastReturnsTrueTask<ACCESS_PRIVILEGED>>("LastReturnsTrue") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Cyclic reuse across multiple rounds
    if (RunTest<CyclicReuseTask<ACCESS_PRIVILEGED>>("CyclicReuse") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Wait() actually blocks until the last task arrives
    if (RunTest<BlocksUntilLastArrivesTask<ACCESS_PRIVILEGED>>("BlocksUntilLastArrives") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Data written before Wait() is visible to all tasks after Wait()
    if (RunTest<DataVisibilityTask<ACCESS_PRIVILEGED>>("DataVisibility") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: Barrier threshold smaller than the full task pool
    if (RunTest<PartialGroupThresholdTask<ACCESS_PRIVILEGED>>("PartialGroupThreshold") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Staggered arrival times across multiple rounds
    if (RunTest<StaggeredArrivalTask<ACCESS_PRIVILEGED>>("StaggeredArrival") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 8: Stress test
    if (RunTest<StressTestTask<ACCESS_PRIVILEGED>>("StressTest", _STK_BARRIER_STRESS_ITERATIONS) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    int32_t final_result = (total_failures == 0 ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", (int)total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
