/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include <sync/stk_sync_mutex.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_MUTEX_TEST_TASKS_MAX   5
#define _STK_MUTEX_TEST_TIMEOUT     1000
#define _STK_MUTEX_TEST_SHORT_SLEEP 10
#define _STK_MUTEX_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_MUTEX_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_MUTEX_STACK_SIZE       256
#define STK_TASK                    static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::mutex
    \brief     Namespace of Mutex test.
 */
namespace mutex {

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_AcquisitionOrder[_STK_MUTEX_TEST_TASKS_MAX] = {0};
static volatile int32_t g_OrderIndex = 0;
static volatile bool    g_TestComplete = false;
static volatile int32_t g_InstancesDone = 0;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_MUTEX_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test mutex
static sync::Mutex g_TestMutex;

/*! \class BasicLockUnlockTask
    \brief Tests basic lock/unlock functionality.
    \note  Verifies that mutex provides mutual exclusion.
*/
template <EAccessMode _AccessMode>
class BasicLockUnlockTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    BasicLockUnlockTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        int32_t workload = 0;

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            g_TestMutex.Lock();

            // Critical section - increment shared counter
            int32_t temp = g_SharedCounter;
            if (++workload % 4 == 0)
                stk::Delay(1); // Small delay to increase chance of race if mutex broken
            g_SharedCounter = temp + 1;

            g_TestMutex.Unlock();

            stk::Yield(); // Yield to other tasks
        }

        ++g_InstancesDone;

        // Task 0 acts as verifier: waits for all other tasks to finish then checks
        // that the counter equals exactly tasks_count * iterations, confirming that
        // no increment was lost or doubled due to a broken mutual exclusion.
        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_MUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            int32_t expected = _STK_MUTEX_TEST_TASKS_MAX * m_iterations;

            printf("basic lock/unlock: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class RecursiveLockTask
    \brief Tests recursive locking capability.
    \note  Verifies that same thread can acquire mutex multiple times.
*/
template <EAccessMode _AccessMode>
class RecursiveLockTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RecursiveLockTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        g_TestMutex.Lock();
        {
            g_TestMutex.Lock(); // Recursive acquisition
            {
                g_TestMutex.Lock(); // Third level
                {
                    g_SharedCounter++;
                }
                g_TestMutex.Unlock();
            }
            g_TestMutex.Unlock();
        }
        g_TestMutex.Unlock();

        ++g_InstancesDone;

        // Verify counter was incremented exactly once per task
        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_MUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            int32_t expected = _STK_MUTEX_TEST_TASKS_MAX;

            printf("recursive lock/unlock: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == _STK_MUTEX_TEST_TASKS_MAX)
                g_TestResult = 1;
        }
    }
};

/*! \class TryLockTask
    \brief Tests TryLock() non-blocking behavior.
    \note  Verifies that TryLock() returns immediately without blocking.
*/
template <EAccessMode _AccessMode>
class TryLockTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryLockTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: Hold the lock
            g_TestMutex.Lock();
            g_SharedCounter = 1;
            stk::Sleep(_STK_MUTEX_TEST_LONG_SLEEP);
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: Try to acquire while held — must fail immediately
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP); // Let task 0 acquire first

            int64_t start = GetTimeNowMsec();
            bool acquired = g_TestMutex.TryLock();
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!acquired && (elapsed < _STK_MUTEX_TEST_SHORT_SLEEP))
                g_TestResult = 1;
            else
                g_TestResult = 0;

            if (acquired)
                g_TestMutex.Unlock();
        }
        // Tasks 2+ are not used by this test

        ++g_InstancesDone;
    }
};

/*! \class TimedLockTask
    \brief Tests TimedLock() timeout behavior.
    \note  Verifies that TimedLock() respects timeout values.
*/
template <EAccessMode _AccessMode>
class TimedLockTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedLockTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: Hold the lock for extended period
            g_TestMutex.Lock();
            stk::Sleep(200); // Hold for 200ms
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: Try to acquire with timeout
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP); // Let task 0 acquire first

            int64_t start = GetTimeNowMsec();
            bool acquired = g_TestMutex.TimedLock(50); // 50ms timeout
            int64_t elapsed = GetTimeNowMsec() - start;

            // Should timeout after ~50ms
            if (!acquired && elapsed >= 45 && elapsed <= 60)
            {
                g_SharedCounter++;
            }

            if (acquired)
                g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            // Task 2: Successfully acquire after task 0 releases
            stk::Sleep(250); // Wait for task 0 to release

            if (g_TestMutex.TimedLock(100))
            {
                g_SharedCounter++;
                g_TestMutex.Unlock();
            }
        }

        ++g_InstancesDone;

        // Final check
        if (m_task_id == 2)
        {
            stk::Sleep(_STK_MUTEX_TEST_LONG_SLEEP);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class FIFOOrderTask
    \brief Tests FIFO ordering of waiting threads.
    \note  Verifies that threads are woken in the order they blocked.
*/
template <EAccessMode _AccessMode>
class FIFOOrderTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    FIFOOrderTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: Acquire lock first
            g_TestMutex.Lock();
            stk::Sleep(50); // Hold to let others queue up
            g_TestMutex.Unlock();
        }
        else
        {
            // Tasks 1-4: Wait in order
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP * m_task_id); // Stagger start times

            g_TestMutex.Lock();
            {
                // Record acquisition order
                int32_t idx = g_OrderIndex++;
                g_AcquisitionOrder[idx] = m_task_id;
            }
            g_TestMutex.Unlock();
        }

        ++g_InstancesDone;

        // Task 4 verifies order
        if (m_task_id == (_STK_MUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_MUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            // Check if tasks acquired in FIFO order (1, 2, 3, 4)
            bool ordered = true;
            for (int32_t i = 0; i < (_STK_MUTEX_TEST_TASKS_MAX - 1); ++i)
            {
                if (g_AcquisitionOrder[i] != (i + 1))
                {
                    ordered = false;
                    printf("Order violation: position %d has task %d (expected %d)\n",
                        (int)i, (int)g_AcquisitionOrder[i], (int)(i + 1));
                    break;
                }
            }

            if (ordered)
                g_TestResult = 1;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test with many lock/unlock cycles.
    \note  Verifies mutex stability under heavy contention.
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
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
            // Mix of operations
            if (i % 3 == 0)
            {
                // Regular lock
                g_TestMutex.Lock();
                g_SharedCounter++;
                g_TestMutex.Unlock();
            }
            else
            if (i % 3 == 1)
            {
                // Try lock
                if (g_TestMutex.TryLock())
                {
                    g_SharedCounter++;
                    g_TestMutex.Unlock();
                }
            }
            else
            {
                // Timed lock
                if (g_TestMutex.TimedLock(10))
                {
                    g_SharedCounter++;
                    g_TestMutex.Unlock();
                }
            }

            if ((i % 10) == 0)
                stk::Delay(1);
        }

        ++g_InstancesDone;

        // Last task verifies total
        if (m_task_id == (_STK_MUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_MUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            // All increments should be accounted for (may be less if TryLock failed)
            if (g_SharedCounter > 0)
                g_TestResult = 1;

            printf("Stress test: counter=%d\n", (int)g_SharedCounter);
        }
    }
};

/*! \class RecursiveDepthTask
    \brief Tests deep recursive locking.
    \note  Verifies mutex handles multiple recursion levels correctly.
*/
template <EAccessMode _AccessMode>
class RecursiveDepthTask : public Task<1024, _AccessMode>
{
    uint8_t m_task_id;
    enum { DEPTH = 8 };

public:
    RecursiveDepthTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void RecursiveLock(int32_t depth)
    {
        if (depth == 0)
            return;

        g_TestMutex.Lock();
        RecursiveLock(depth - 1);
        g_SharedCounter++;
        g_TestMutex.Unlock();
    }

    void Run()
    {
        // Recursive lock to depth 50
        RecursiveLock(DEPTH);

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_MUTEX_TEST_LONG_SLEEP);

            int32_t expected = _STK_MUTEX_TEST_TASKS_MAX * DEPTH;

            printf("recursive depth: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class InterTaskCoordinationTask
    \brief Tests mutex for coordinating work between tasks.
    \note  Verifies mutex correctly synchronizes shared state updates.
*/
template <EAccessMode _AccessMode>
class InterTaskCoordinationTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    InterTaskCoordinationTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        for (int32_t round = 0; round < 10; ++round)
        {
            g_TestMutex.Lock();
            {
                // Each task increments in sequence
                while ((g_SharedCounter % _STK_MUTEX_TEST_TASKS_MAX) != m_task_id)
                {
                    g_TestMutex.Unlock();
                    stk::Delay(1);
                    g_TestMutex.Lock();
                }

                g_SharedCounter++;
            }
            g_TestMutex.Unlock();
        }

        ++g_InstancesDone;

        // Last task verifies
        if (m_task_id == (_STK_MUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_MUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            // Should be exactly 10 rounds * number of tasks
            if (g_SharedCounter == 10 * _STK_MUTEX_TEST_TASKS_MAX)
                g_TestResult = 1;

            printf("Coordination test: counter=%d (expected %d)\n",
                (int)g_SharedCounter, 10 * _STK_MUTEX_TEST_TASKS_MAX);
        }
    }
};

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult = 0;
    g_SharedCounter = 0;
    g_OrderIndex = 0;
    g_TestComplete = false;
    g_InstancesDone = 0;

    for (int32_t i = 0; i < _STK_MUTEX_TEST_TASKS_MAX; ++i)
        g_AcquisitionOrder[i] = 0;
}

} // namespace mutex
} // namespace test
} // namespace stk

/*! \fn    NeedsExtendedTasks
    \brief Returns true if the test requires tasks 3 and 4.
    \note  TryLock uses only tasks 0-1; TimedLock uses only tasks 0-2.
*/
static bool NeedsExtendedTasks(const char *test_name)
{
    return  (strcmp(test_name, "TryLock")   != 0) &&
            (strcmp(test_name, "TimedLock") != 0);
}

/*! \fn    NeedsThreeTasks
    \brief Returns true if the test requires at least 3 tasks (0-2).
    \note  TryLock uses only tasks 0-1, so task 2 is also unnecessary.
*/
static bool NeedsThreeTasks(const char *test_name)
{
    return  (strcmp(test_name, "TryLock") != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::mutex;

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

    using namespace stk::test::mutex;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Basic Lock/Unlock with mutual exclusion
    if (RunTest<BasicLockUnlockTask<ACCESS_PRIVILEGED>>("BasicLockUnlock", 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Recursive locking
    if (RunTest<RecursiveLockTask<ACCESS_PRIVILEGED>>("RecursiveLock") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: TryLock non-blocking behavior
    if (RunTest<TryLockTask<ACCESS_PRIVILEGED>>("TryLock") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: TimedLock timeout behavior
    if (RunTest<TimedLockTask<ACCESS_PRIVILEGED>>("TimedLock") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: FIFO ordering
    if (RunTest<FIFOOrderTask<ACCESS_PRIVILEGED>>("FIFOOrder") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: Deep recursion
    if (RunTest<RecursiveDepthTask<ACCESS_PRIVILEGED>>("RecursiveDepth") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Inter-task coordination
    if (RunTest<InterTaskCoordinationTask<ACCESS_PRIVILEGED>>("InterTaskCoordination") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 8: Stress test
    if (RunTest<StressTestTask<ACCESS_PRIVILEGED>>("StressTest", 400) != TestContext::SUCCESS_EXIT_CODE)
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
