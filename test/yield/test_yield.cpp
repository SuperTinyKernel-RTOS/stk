/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  test_yield.cpp
    \brief Test suite for stk::Yield().

    The suite is built twice, once per Round-Robin variant, because the strategy is a compile-time
    property of the kernel:

     - default build (STK_YIELD_TEST_NATIVE=0): SwitchStrategyRR, legacy Yield() (task is put to
       sleep for a tick by the kernel);
     - -DSTK_YIELD_TEST_NATIVE=1: SwitchStrategyRR_NSY, native Yield() via OnTaskYield() and
       STATE_YIELD_PENDING.

    Functional tests (no hang, no lost iterations, correct hand-off) must pass in both builds.
    Tests marked "native only" check properties that only the native path guarantees (latency of a
    lone runnable task, fairness of the rotation).
*/

#include <stk_config.h>
#include <stk.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#ifndef STK_YIELD_TEST_NATIVE
#define STK_YIELD_TEST_NATIVE       1
#endif

#define _STK_YIELD_TEST_TASKS_MAX   5
#define _STK_YIELD_TEST_SHORT_SLEEP 10
#define _STK_YIELD_TEST_PEER_SLEEP  300 // peers of the lone yielder sleep this long (ms)
#define _STK_YIELD_TEST_ADD_ITERS   50  // iterations of the task added at run time

// Upper bound (ms) for N yields of a lone runnable task in the native build. Legacy yield costs
// about one tick per call, so N=100 yields take ~100 ms at a 1 ms tick, while native yield only
// pays the context-switch cost. Tune this value for slow simulators/targets.
#ifndef _STK_YIELD_TEST_FAST_MS
#define _STK_YIELD_TEST_FAST_MS     50
#endif

#ifdef __ARM_ARCH_6M__
#define _STK_YIELD_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_YIELD_STACK_SIZE       256
#define STK_TASK                    static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::yield
    \brief     Namespace of Yield test.
 */
namespace yield {

#if STK_YIELD_TEST_NATIVE
typedef SwitchStrategyRR_NSY TestStrategy;
static const bool g_Native = true;
#else
typedef SwitchStrategyRR TestStrategy;
static const bool g_Native = false;
#endif

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_TaskCount = 0;   // number of tasks that must report completion
static volatile int32_t g_Counter[_STK_YIELD_TEST_TASKS_MAX] = {0}; // per-task progress (single writer each)
static volatile int32_t g_Done[_STK_YIELD_TEST_TASKS_MAX] = {0};    // per-task completion flag (single writer each)
static volatile int32_t g_Awake[_STK_YIELD_TEST_TASKS_MAX] = {0};   // per-task "woke from sleep" flag
static volatile int32_t g_Spread[_STK_YIELD_TEST_TASKS_MAX] = {0};  // per-task max observed progress spread
static volatile int32_t g_InOrder[_STK_YIELD_TEST_TASKS_MAX] = {0}; // per-task steps that followed the ring predecessor
static volatile int32_t g_OutOfOrder[_STK_YIELD_TEST_TASKS_MAX] = {0}; // per-task steps that did not
static volatile int32_t g_Last = -1;       // id of the task that made the previous step
static volatile int32_t g_Turn = 0;
static volatile int32_t g_Shared = 0;
static volatile int32_t g_Flag = 0;
static volatile int64_t g_Elapsed = 0;

// Kernel
static Kernel<KERNEL_DYNAMIC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_YIELD_TEST_TASKS_MAX, TestStrategy, PlatformDefault> g_Kernel;

/*! \fn    AllDone
    \brief Returns true when the first g_TaskCount tasks have reported completion.
    \note  Per-task flags are used instead of a shared counter so that no update can be lost
           when a task is preempted in the middle of a read-modify-write.
*/
static bool AllDone()
{
    for (int32_t i = 0; i < g_TaskCount; ++i)
    {
        if (!g_Done[i])
            return false;
    }
    return true;
}

static void WaitAllDone()
{
    while (!AllDone())
        stk::Sleep(_STK_YIELD_TEST_SHORT_SLEEP);
}

/*! \class YieldBasicTask
    \brief Every task yields after each step.
    \note  Verifies that Yield() returns, and that no iteration is lost or repeated.
*/
template <EAccessMode _AccessMode>
class YieldBasicTask : public Task<_STK_YIELD_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    YieldBasicTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            ++g_Counter[m_task_id];
            stk::Yield();
        }

        g_Done[m_task_id] = 1;

        if (m_task_id == 0)
        {
            WaitAllDone();

            bool ok = true;
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                printf("basic yield: task %d counter=%d (expected %d)\n",
                    (int)i, (int)g_Counter[i], (int)m_iterations);

                if (g_Counter[i] != m_iterations)
                    ok = false;
            }

            if (ok)
                g_TestResult = 1;
        }
    }
};

/*! \class PingPongTask
    \brief Tasks pass a token around a ring and spin on Yield() until it is theirs.
    \note  Verifies that Yield() hands the CPU to the other tasks so the awaited one gets to run,
           i.e. a yielding task neither monopolizes the CPU nor drops out of the rotation.
*/
template <EAccessMode _AccessMode>
class PingPongTask : public Task<_STK_YIELD_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    PingPongTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            while (g_Turn != m_task_id)
                stk::Yield();

            ++g_Counter[m_task_id];
            ++g_Shared; // only the token holder writes it
            g_Turn = (m_task_id + 1) % g_TaskCount;
        }

        g_Done[m_task_id] = 1;

        if (m_task_id == 0)
        {
            WaitAllDone();

            const int32_t expected = g_TaskCount * m_iterations;

            printf("ping-pong: shared=%d (expected %d)\n", (int)g_Shared, (int)expected);

            bool ok = (g_Shared == expected);
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                if (g_Counter[i] != m_iterations)
                    ok = false;
            }

            if (ok)
                g_TestResult = 1;
        }
    }
};

/*! \class LoneYielderTask
    \brief Task 0 yields in a loop while all other tasks (if any) sleep.
    \note  Verifies that a lone runnable task makes progress. In the native build Yield() must not
           stall it for a tick and it must finish long before the sleeping peers wake up.
*/
template <EAccessMode _AccessMode>
class LoneYielderTask : public Task<_STK_YIELD_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    LoneYielderTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            const int64_t start = GetTimeNowMs();

            for (int32_t i = 0; i < m_iterations; ++i)
            {
                ++g_Counter[0];
                stk::Yield();
            }

            g_Elapsed = GetTimeNowMs() - start;

            int32_t peers_awake = 0;
            for (int32_t i = 1; i < g_TaskCount; ++i)
                peers_awake += g_Awake[i];

            g_Done[0] = 1;
            WaitAllDone();

            printf("lone yielder: %d yields in %d ms, peers awake at end=%d\n",
                (int)g_Counter[0], (int)g_Elapsed, (int)peers_awake);

            bool ok = (g_Counter[0] == m_iterations);

            if (g_Native)
            {
                if (g_Elapsed >= _STK_YIELD_TEST_FAST_MS)
                    ok = false;

                if (peers_awake != 0)
                    ok = false;
            }

            if (ok)
                g_TestResult = 1;
        }
        else
        {
            stk::Sleep(_STK_YIELD_TEST_PEER_SLEEP);

            g_Awake[m_task_id] = 1;
            g_Done[m_task_id] = 1;
        }
    }
};

/*! \class SleeperTask
    \brief Task 0 sleeps while the other tasks spin on Yield() until it wakes up.
    \note  Verifies that Yield() does not disturb the sleep bookkeeping of other tasks: the sleeper
           wakes on time and the yielders keep making progress meanwhile.
*/
template <EAccessMode _AccessMode>
class SleeperTask : public Task<_STK_YIELD_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SleeperTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        enum { SLEEP_MS = 50 };

        if (m_task_id == 0)
        {
            const int64_t start = GetTimeNowMs();
            stk::Sleep(SLEEP_MS);
            g_Elapsed = GetTimeNowMs() - start;

            g_Flag = 1;
            g_Done[0] = 1;

            WaitAllDone();

            printf("sleeper: woke after %d ms (expected ~%d), yielder progress=%d/%d\n",
                (int)g_Elapsed, (int)SLEEP_MS, (int)g_Counter[1], (int)g_Counter[2]);

            if ((g_Elapsed >= (SLEEP_MS - 5)) && (g_Elapsed <= (SLEEP_MS + 25)) &&
                (g_Counter[1] > 0) && (g_Counter[2] > 0))
            {
                g_TestResult = 1;
            }
        }
        else
        {
            while (!g_Flag)
            {
                ++g_Counter[m_task_id];
                stk::Yield();
            }

            g_Done[m_task_id] = 1;
        }
    }
};

/*! \class FairnessTask
    \brief Every task counts one step and yields; ring order and progress spread are monitored.
    \note  Native only. A forced switch does not restart the tick, so a tick may cut the next
           task's slice short (or advance the cursor again before the first pick has run) and the
           task loses its step for that pass. Lost steps are never made up, therefore the absolute
           spread between tasks drifts with the number of ticks and cannot be bounded by a small
           constant. Two scale-free properties are checked instead:
            - ring adherence: the step is made by the ring successor of the previous stepper in at
              least 80% of the cases (a lost step costs a single out-of-order transition);
            - no starvation: the spread never exceeds iterations / 4.
*/
template <EAccessMode _AccessMode>
class FairnessTask : public Task<_STK_YIELD_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    FairnessTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        // workload start (all tasks start together, task 0 reports the total workload time)
        const Ticks start_us = hw::HiResClock::GetTimeUs();

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            ++g_Counter[m_task_id];

            // ring adherence (a preemption between these two statements can misclassify a step,
            // which is rare and only lowers the measured ratio slightly)
            const int32_t prev = g_Last;
            g_Last = m_task_id;

            if (prev >= 0)
            {
                if (prev == ((m_task_id + g_TaskCount - 1) % g_TaskCount))
                    ++g_InOrder[m_task_id];
                else
                    ++g_OutOfOrder[m_task_id];
            }

            // progress spread, evaluated only while no task has finished
            int32_t mn = g_Counter[0], mx = g_Counter[0];
            for (int32_t t = 1; t < g_TaskCount; ++t)
            {
                const int32_t c = g_Counter[t];
                if (c < mn) mn = c;
                if (c > mx) mx = c;
            }

            if ((mx < m_iterations) && ((mx - mn) > g_Spread[m_task_id]))
                g_Spread[m_task_id] = (mx - mn);

            stk::Yield();
        }

        g_Done[m_task_id] = 1;

        if (m_task_id == 0)
        {
            WaitAllDone();

            // workload time: from the start of task 0 until all tasks have completed
            const Ticks elapsed_us = hw::HiResClock::GetTimeUs() - start_us;

            int32_t spread = 0, in_order = 0, out_of_order = 0;
            bool ok = true;
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                if (g_Spread[i] > spread)
                    spread = g_Spread[i];

                in_order += g_InOrder[i];
                out_of_order += g_OutOfOrder[i];

                if (g_Counter[i] != m_iterations)
                    ok = false;
            }

            const int32_t transitions = in_order + out_of_order;
            const int32_t adherence = (transitions != 0) ? ((in_order * 100) / transitions) : 0;

            printf("fairness: ring adherence=%d%% (%d/%d, min 80%%), max progress spread=%d (limit %d), workload time=%u us\n",
                (int)adherence, (int)in_order, (int)transitions, (int)spread, (int)(m_iterations / 4),
                (unsigned int)elapsed_us);

            if (ok && (adherence >= 80) && (spread <= (m_iterations / 4)))
                g_TestResult = 1;
        }
    }
};

#ifndef __ARM_ARCH_6M__
// Task added from a running task (declared here, defined after YieldBasicTask is known).
static YieldBasicTask<ACCESS_PRIVILEGED> g_ExtraTask(3, _STK_YIELD_TEST_ADD_ITERS);

/*! \class AddTaskTask
    \brief Yielding tasks; task 0 adds one more task to the kernel in the middle of the run.
    \note  Verifies Yield() right after the strategy's task list changed under a running rotation
           (this exercises the cursor handling of AddTask() followed by Yield()). All tasks,
           including the added one, must complete every iteration.
*/
template <EAccessMode _AccessMode>
class AddTaskTask : public Task<_STK_YIELD_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    AddTaskTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            if ((m_task_id == 0) && (i == 5))
                g_Kernel.AddTask(&g_ExtraTask);

            ++g_Counter[m_task_id];
            stk::Yield();
        }

        g_Done[m_task_id] = 1;

        if (m_task_id == 0)
        {
            WaitAllDone();

            bool ok = true;
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                printf("add task: task %d counter=%d (expected %d)\n",
                    (int)i, (int)g_Counter[i], (int)m_iterations);

                if (g_Counter[i] != m_iterations)
                    ok = false;
            }

            if (ok)
                g_TestResult = 1;
        }
    }
};
#endif // __ARM_ARCH_6M__

/*! \class StressTask
    \brief Many yields mixed with busy delays and short sleeps.
    \note  Verifies stability of Yield() under heavy rotation, with ticks landing at arbitrary
           points of the yield sequence (including between OnTaskYield() and the forced switch).
*/
template <EAccessMode _AccessMode>
class StressTask : public Task<_STK_YIELD_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    StressTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            ++g_Counter[m_task_id];

            stk::Yield();

            if ((i % 10) == 0)
                stk::Delay(1);

            if ((i % 25) == 0)
                stk::Sleep(1);
        }

        g_Done[m_task_id] = 1;

        if (m_task_id == (g_TaskCount - 1))
        {
            WaitAllDone();

            bool ok = true;
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                if (g_Counter[i] != m_iterations)
                    ok = false;
            }

            printf("stress: all %d tasks completed %d iterations: %s\n",
                (int)g_TaskCount, (int)m_iterations, ok ? "yes" : "no");

            if (ok)
                g_TestResult = 1;
        }
    }
};

// Helper function to reset test state
static void ResetTestState(int32_t expected_done)
{
    g_TestResult = 0;
    g_TaskCount = expected_done;
    g_Turn = 0;
    g_Shared = 0;
    g_Flag = 0;
    g_Elapsed = 0;
    g_Last = -1;

    for (int32_t i = 0; i < _STK_YIELD_TEST_TASKS_MAX; ++i)
    {
        g_Counter[i] = 0;
        g_Done[i] = 0;
        g_Awake[i] = 0;
        g_Spread[i] = 0;
        g_InOrder[i] = 0;
        g_OutOfOrder[i] = 0;
    }
}

} // namespace yield
} // namespace test
} // namespace stk

/*! \fn    RunTest
    \brief Helper function to run a single test case.
    \param[in] test_name:     Name of the test.
    \param[in] tasks_added:   Number of tasks (0..N-1) added to the kernel before Start().
    \param[in] param:         Parameter passed to every task (iteration count).
    \param[in] expected_done: Number of tasks expected to report completion (defaults to
                              tasks_added; larger when a task adds more tasks at run time).
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t tasks_added, int32_t param, int32_t expected_done = -1)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::yield;

    printf("Test: %s\n", test_name);

    ResetTestState((expected_done < 0) ? tasks_added : expected_done);

    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_Kernel.AddTask(&task0);

    if (tasks_added > 1)
        g_Kernel.AddTask(&task1);
    if (tasks_added > 2)
        g_Kernel.AddTask(&task2);
    if (tasks_added > 3)
        g_Kernel.AddTask(&task3);
    if (tasks_added > 4)
        g_Kernel.AddTask(&task4);

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

    using namespace stk::test::yield;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");
    printf("Yield variant: %s\n", g_Native ? "native (SwitchStrategyRR_NSY)" : "legacy (SwitchStrategyRR)");
    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Basic yield loop, 3 tasks
    if (RunTest<YieldBasicTask<ACCESS_PRIVILEGED>>("BasicYield", 3, 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Token ring hand-off through Yield()
    if (RunTest<PingPongTask<ACCESS_PRIVILEGED>>("PingPong", 3, 30) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Single task in the system yields
    if (RunTest<LoneYielderTask<ACCESS_PRIVILEGED>>("LoneTask", 1, 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: One runnable task yields while the others are asleep
    if (RunTest<LoneYielderTask<ACCESS_PRIVILEGED>>("LoneRunnableWithSleepers", 3, 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Yielding tasks next to a sleeping task
    if (RunTest<SleeperTask<ACCESS_PRIVILEGED>>("YieldersAndSleeper", 3, 0) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6 (native only): Fairness of the rotation
    if (RunTest<FairnessTask<ACCESS_PRIVILEGED>>("RotationFairness", 3, 200) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Yield after the task list was changed at run time (3 added up front + 1 at run time)
    if (RunTest<AddTaskTask<ACCESS_PRIVILEGED>>("AddTaskThenYield", 3, _STK_YIELD_TEST_ADD_ITERS, 4) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 8: Stress test
    if (RunTest<StressTask<ACCESS_PRIVILEGED>>("StressTest", _STK_YIELD_TEST_TASKS_MAX, 400) != TestContext::SUCCESS_EXIT_CODE)
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
