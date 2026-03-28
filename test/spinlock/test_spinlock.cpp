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
#include <sync/stk_sync_spinlock.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_SL_TEST_TASKS_MAX   5
#define _STK_SL_TEST_SHORT_SLEEP 10
#define _STK_SL_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_SL_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_SL_STACK_SIZE       256
#define STK_TASK                 static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::spinlock
    \brief     Namespace of SpinLock test.
 */
namespace spinlock {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_OrderIndex    = 0;
static volatile bool    g_TestComplete  = false;
static volatile int32_t g_InstancesDone = 0;

// Kernel (SpinLock does not require KERNEL_SYNC)
static Kernel<KERNEL_DYNAMIC, _STK_SL_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test spinlock - shared across tasks; low spin count forces Yield() quickly under contention
static sync::SpinLock g_TestSpinLock;

/*! \class MutualExclusionTask
    \brief Tests that Lock()/Unlock() provides mutual exclusion under concurrent access.
    \note  Each task performs a deliberate read-modify-Yield-write sequence inside the
           locked region to maximally expose any race. If the lock is broken the final
           counter will be less than tasks_count * iterations.
*/
template <EAccessMode _AccessMode>
class MutualExclusionTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    MutualExclusionTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        int32_t workload = 0;

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            g_TestSpinLock.Lock();

            // read-modify-Yield-write: exposes races if the lock is not working
            int32_t temp = g_SharedCounter;
            if (++workload % 4 == 0)
               stk::Delay(1);  // Small delay to increase chance of race if mutex broken
            g_SharedCounter = temp + 1;

            g_TestSpinLock.Unlock();

            stk::Yield(); // Yield to other tasks
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_SL_TEST_TASKS_MAX)
                stk::Sleep(_STK_SL_TEST_LONG_SLEEP);

            int32_t expected = _STK_SL_TEST_TASKS_MAX * m_iterations;

            printf("mutual exclusion: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class TryLockFreeTask
    \brief Tests TryLock() succeeds immediately when the lock is free.
    \note  Verifies return value is true, that the lock is actually held afterwards
           (a concurrent TryLock by another task must fail), and that Unlock() releases
           it correctly so a subsequent TryLock can succeed again.
*/
template <EAccessMode _AccessMode>
class TryLockFreeTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryLockFreeTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // TryLock on a free lock must succeed immediately
            bool acquired = g_TestSpinLock.TryLock();

            if (acquired)
            {
                // Signal task 2 to attempt TryLock while we still hold it (counter=2)
                g_SharedCounter = 2;

                stk::Sleep(_STK_SL_TEST_SHORT_SLEEP); // hold while task 2 tries

                g_TestSpinLock.Unlock();

                // Re-acquire after release to confirm the lock is free again
                if (g_TestSpinLock.TryLock())
                {
                    ++g_SharedCounter; // 4: re-acquired after release
                    g_TestSpinLock.Unlock();
                }
            }
        }
        else
        if (m_task_id == 2)
        {
            // Wait until task 1 signals it holds the lock
            while (g_SharedCounter < 2)
                stk::Yield();

            // Must fail: task 1 still holds the lock
            bool acquired = g_TestSpinLock.TryLock();

            if (!acquired)
                ++g_SharedCounter; // 3: correctly returned false

            if (acquired)
                g_TestSpinLock.Unlock();
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_SL_TEST_LONG_SLEEP);

            printf("try-lock free: counter=%d (expected 4)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 4)
                g_TestResult = 1;
        }
    }
};

/*! \class TryLockContendedTask
    \brief Tests TryLock() returns false immediately when the lock is held by another task.
    \note  Task 0 holds the lock for an extended period; task 1 calls TryLock() and
           verifies it returns false quickly without blocking.
*/
template <EAccessMode _AccessMode>
class TryLockContendedTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryLockContendedTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Acquire and hold; signal task 1 that the lock is held
            g_TestSpinLock.Lock();
            g_SharedCounter = 1;
            stk::Sleep(_STK_SL_TEST_LONG_SLEEP);
            g_TestSpinLock.Unlock();
        }
        else
        if (m_task_id == 1)
        {
            // Wait until task 0 signals it has acquired the lock
            while (g_SharedCounter == 0)
                stk::Yield();

            int64_t start   = GetTimeNowMsec();
            bool acquired   = g_TestSpinLock.TryLock();
            int64_t elapsed = GetTimeNowMsec() - start;

            // Must fail immediately: task 0 holds the lock
            if (!acquired && elapsed < _STK_SL_TEST_SHORT_SLEEP)
                ++g_SharedCounter; // 2: correctly returned false immediately

            if (acquired)
                g_TestSpinLock.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            stk::Sleep(_STK_SL_TEST_LONG_SLEEP + _STK_SL_TEST_SHORT_SLEEP);

            printf("try-lock contended: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

/*! \class RecursiveLockTask
    \brief Tests recursive Lock()/Unlock() by the same task.
    \note  Verifies that a task can acquire the spinlock multiple times without
           deadlocking, and that the lock is only fully released after the matching
           number of Unlock() calls. All tasks recurse to the same depth; the total
           counter must equal tasks_count * depth.
*/
template <EAccessMode _AccessMode>
class RecursiveLockTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    enum { DEPTH = 3 };

public:
    RecursiveLockTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void AcquireRecursive(int32_t depth)
    {
        if (depth == 0)
            return;

        g_TestSpinLock.Lock();
        AcquireRecursive(depth - 1);
        ++g_SharedCounter;
        g_TestSpinLock.Unlock();
    }

    void Run()
    {
        AcquireRecursive(DEPTH);

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_SL_TEST_TASKS_MAX)
                stk::Sleep(_STK_SL_TEST_SHORT_SLEEP);

            int32_t expected = _STK_SL_TEST_TASKS_MAX * DEPTH;

            printf("recursive lock: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class RecursiveTryLockTask
    \brief Tests TryLock() recursive acquisition by the owning task.
    \note  Verifies that TryLock() succeeds and increments the recursion count when
           called by the already-owning task, that the lock is not released until all
           matching Unlock() calls are made, and that a non-owning task's TryLock()
           correctly fails while the owner still holds a nested level.
*/
template <EAccessMode _AccessMode>
class RecursiveTryLockTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RecursiveTryLockTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Acquire recursively via TryLock three levels deep
            bool l1 = g_TestSpinLock.TryLock(); // depth 1
            bool l2 = g_TestSpinLock.TryLock(); // depth 2 - recursive, must succeed
            bool l3 = g_TestSpinLock.TryLock(); // depth 3 - recursive, must succeed

            if (l1 && l2 && l3)
                ++g_SharedCounter; // 1: all three TryLock() calls succeeded

            g_TestSpinLock.Unlock(); // depth 2 remains
            g_TestSpinLock.Unlock(); // depth 1 remains

            // Signal task 2: task 1 still holds depth 1, task 2 must fail TryLock
            g_SharedCounter = 2;

            stk::Sleep(_STK_SL_TEST_SHORT_SLEEP); // hold for task 2 to attempt

            g_TestSpinLock.Unlock(); // depth 0 - fully released
        }
        else
        if (m_task_id == 2)
        {
            // Wait for task 1 to signal it still holds the lock at depth 1
            while (g_SharedCounter < 2)
                stk::Yield();

            bool acquired = g_TestSpinLock.TryLock();

            if (!acquired)
                ++g_SharedCounter; // 3: correctly blocked while task 1 held depth 1

            if (acquired)
                g_TestSpinLock.Unlock();
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_SL_TEST_LONG_SLEEP);

            printf("recursive try-lock: counter=%d (expected 3)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 3)
                g_TestResult = 1;
        }
    }
};

/*! \class YieldUnderContentionTask
    \brief Tests that Lock() yields cooperatively when spin count is exhausted.
    \note  With a very low spin count (_STK_SL_TEST_SPIN_COUNT = 10) and all five
           tasks contending simultaneously, the kernel must not livelock. Verifies
           that all increments complete correctly despite frequent Yield() calls
           triggered inside Lock().
*/
template <EAccessMode _AccessMode>
class YieldUnderContentionTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    YieldUnderContentionTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        // All tasks hammer the lock simultaneously; low spin count forces frequent Yield()
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            g_TestSpinLock.Lock();
            ++g_SharedCounter;
            g_TestSpinLock.Unlock();
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_SL_TEST_TASKS_MAX)
                stk::Sleep(_STK_SL_TEST_LONG_SLEEP);

            int32_t expected = _STK_SL_TEST_TASKS_MAX * m_iterations;

            printf("yield under contention: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class UnlockTransferTask
    \brief Tests that Unlock() correctly transfers the lock to a waiting contender.
    \note  All tasks compete for the lock equally; each acquires, increments, and
           releases. Verifies that no increment is lost or duplicated across the full
           transfer chain: total must equal tasks_count * iterations exactly.
*/
template <EAccessMode _AccessMode>
class UnlockTransferTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    UnlockTransferTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            g_TestSpinLock.Lock();
            ++g_SharedCounter;
            g_TestSpinLock.Unlock();

            stk::Delay(1); // stagger re-acquisitions to spread lock transfers across all tasks
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_SL_TEST_TASKS_MAX)
                stk::Sleep(_STK_SL_TEST_LONG_SLEEP);

            int32_t expected = _STK_SL_TEST_TASKS_MAX * m_iterations;

            printf("unlock transfer: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test mixing Lock() and TryLock() under full five-task contention.
    \note  Even iterations use blocking Lock(); odd iterations use non-blocking TryLock().
           TryLock() failures are silent (expected under contention). Verifies that
           every successful acquisition increments the counter exactly once and that
           no corruption occurs: final counter must be at least the guaranteed minimum
           from the blocking iterations alone.
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_SL_STACK_SIZE, _AccessMode>
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
            if ((i % 2) == 0)
            {
                // Blocking path: always acquires
                g_TestSpinLock.Lock();
                ++g_SharedCounter;
                g_TestSpinLock.Unlock();
            }
            else
            {
                // Non-blocking path: may fail under contention
                if (g_TestSpinLock.TryLock())
                {
                    ++g_SharedCounter;
                    g_TestSpinLock.Unlock();
                }
            }
        }

        ++g_InstancesDone;

        if (m_task_id == (_STK_SL_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_SL_TEST_TASKS_MAX)
                stk::Sleep(_STK_SL_TEST_LONG_SLEEP);

            // Minimum guaranteed: blocking iterations only (m_iterations / 2 per task)
            int32_t min_expected = _STK_SL_TEST_TASKS_MAX * (m_iterations / 2);

            printf("stress test: counter=%d (min expected %d)\n",
                (int)g_SharedCounter, (int)min_expected);

            if (g_SharedCounter >= min_expected)
                g_TestResult = 1;
        }
    }
};

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult    = 0;
    g_SharedCounter = 0;
    g_OrderIndex    = 0;
    g_TestComplete  = false;
    g_InstancesDone = 0;
}

} // namespace spinlock
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return  (strcmp(test_name, "TryLockFree")      != 0) &&
            (strcmp(test_name, "TryLockContended") != 0) &&
            (strcmp(test_name, "RecursiveTryLock") != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::spinlock;

    printf("Test: %s\n", test_name);

    ResetTestState();

    // Create tasks based on test type
    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    STK_TASK TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_Kernel.AddTask(&task0);
    g_Kernel.AddTask(&task1);
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

    using namespace stk::test::spinlock;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Lock/Unlock provides mutual exclusion under contention
    if (RunTest<MutualExclusionTask<ACCESS_PRIVILEGED>>("MutualExclusion", 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: TryLock() succeeds on a free lock; fails while another task holds it (tasks 0-2 only)
    if (RunTest<TryLockFreeTask<ACCESS_PRIVILEGED>>("TryLockFree") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: TryLock() returns false immediately when lock is held by another task (tasks 0-2 only)
    if (RunTest<TryLockContendedTask<ACCESS_PRIVILEGED>>("TryLockContended") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Recursive Lock() does not deadlock; fully released only after matching Unlock() calls
    if (RunTest<RecursiveLockTask<ACCESS_PRIVILEGED>>("RecursiveLock") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: TryLock() recursive for owner; correctly fails for non-owner (tasks 0-2 only)
    if (RunTest<RecursiveTryLockTask<ACCESS_PRIVILEGED>>("RecursiveTryLock") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: Lock() yields cooperatively when spin count exhausted; no livelock
    if (RunTest<YieldUnderContentionTask<ACCESS_PRIVILEGED>>("YieldUnderContention", 200) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Unlock() transfers to a waiting task; no lost or duplicate increments
    if (RunTest<UnlockTransferTask<ACCESS_PRIVILEGED>>("UnlockTransfer", 50) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 8: Stress test mixing blocking Lock() and non-blocking TryLock()
    if (RunTest<StressTestTask<ACCESS_PRIVILEGED>>("StressTest", 400) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    int32_t final_result = (total_failures == 0 ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
