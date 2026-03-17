/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include <sync/stk_sync_semaphore.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_SEM_TEST_TASKS_MAX   5
#define _STK_SEM_TEST_TIMEOUT     1000
#define _STK_SEM_TEST_SHORT_SLEEP 10
#define _STK_SEM_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_SEM_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_SEM_STACK_SIZE       256
#define STK_TASK                  static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::semaphore
    \brief     Namespace of Semaphore test.
 */
namespace semaphore {

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_AcquisitionOrder[_STK_SEM_TEST_TASKS_MAX] = {0};
static volatile int32_t g_OrderIndex = 0;
static volatile bool    g_TestComplete = false;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_SEM_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test semaphore (re-constructed per test via ResetTestState)
static sync::Semaphore g_TestSemaphore;

/*! \class BasicSignalWaitTask
    \brief Tests basic Signal/Wait functionality.
    \note  Verifies that Wait() blocks until Signal() is called, and that every
           signal produces exactly one successful Wait().
*/
template <EAccessMode _AccessMode>
class BasicSignalWaitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    BasicSignalWaitTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: producer - signal once per iteration for each consumer
            for (int32_t i = 0; i < m_iterations * (_STK_SEM_TEST_TASKS_MAX - 1); ++i)
            {
                stk::Delay(1); // pace signals so consumers can catch up
                g_TestSemaphore.Signal();
            }

            stk::Sleep(_STK_SEM_TEST_LONG_SLEEP);

            int32_t expected = m_iterations * (_STK_SEM_TEST_TASKS_MAX - 1);

            printf("basic signal/wait: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: consumers - wait and increment counter per acquired signal
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestSemaphore.Wait();
                ++g_SharedCounter;
            }
        }
    }
};

/*! \class InitialCountTask
    \brief Tests semaphore constructed with a non-zero initial count.
    \note  Verifies that the first N Wait() calls succeed immediately (fast path)
           when the semaphore is initialized with count N, and that the count
           reaches zero afterwards.
*/
template <EAccessMode _AccessMode>
class InitialCountTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    InitialCountTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        // Semaphore is pre-loaded with (_STK_SEM_TEST_TASKS_MAX - 1) permits in ResetTestState.
        // Tasks 1-4 grab one permit each immediately; task 0 must block (count already at 0).
        if (m_task_id != 0)
        {
            // Should succeed on fast path - count is pre-loaded
            if (g_TestSemaphore.Wait(_STK_SEM_TEST_SHORT_SLEEP))
                ++g_SharedCounter;
        }
        else
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP / 2); // let consumers drain the count first

            // Count should now be 0; this Wait must block until a signal arrives
            bool acquired = g_TestSemaphore.Wait(_STK_SEM_TEST_SHORT_SLEEP);

            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            int32_t expected = _STK_SEM_TEST_TASKS_MAX - 1;

            printf("initial count: fast-path counter=%d (expected %d), blocked-wait acquired=%d (expected 0)\n",
                (int)g_SharedCounter, (int)expected, (int)acquired);

            if ((g_SharedCounter == expected) && !acquired)
                g_TestResult = 1;
        }
    }
};

/*! \class TimeoutWaitTask
    \brief Tests Wait() timeout behavior.
    \note  Verifies that Wait() respects the timeout and returns false when no
           signal arrives within the allotted time.
*/
template <EAccessMode _AccessMode>
class TimeoutWaitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimeoutWaitTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: hold the semaphore at zero; signal only after the test window
            stk::Sleep(200); // wait well past timeout window before signalling
            g_TestSemaphore.Signal();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: Wait with 50-tick timeout while semaphore stays at zero
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // let task 0 establish the zero state

            int64_t start   = GetTimeNowMsec();
            bool acquired   = g_TestSemaphore.Wait(50); // 50-tick timeout
            int64_t elapsed = GetTimeNowMsec() - start;

            // Should time out after ~50 ms and return false
            if (!acquired && elapsed >= 45 && elapsed <= 60)
                ++g_SharedCounter;

            if (acquired)
                g_TestSemaphore.Signal(); // return the token
        }
        else
        if (m_task_id == 2)
        {
            // Task 2: verify that a Wait with a generous timeout succeeds after task 0 signals
            stk::Sleep(210); // after task 0's signal

            if (g_TestSemaphore.Wait(100))
                ++g_SharedCounter;
        }

        if (m_task_id == 2)
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            printf("timeout wait: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class ZeroTimeoutTask
    \brief Tests Wait(0) (NO_WAIT) non-blocking behavior.
    \note  Verifies that Wait(0) returns immediately without blocking when the
           semaphore count is zero, and returns true immediately when count > 0.
*/
template <EAccessMode _AccessMode>
class ZeroTimeoutTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ZeroTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Wait(0) on a zero-count semaphore must return false immediately
            int64_t start   = GetTimeNowMsec();
            bool acquired   = g_TestSemaphore.Wait(NO_WAIT);
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!acquired && elapsed < _STK_SEM_TEST_SHORT_SLEEP)
                ++g_SharedCounter;
        }
        else
        if (m_task_id == 2)
        {
            // Pre-load one permit then Wait(0) must succeed immediately
            g_TestSemaphore.Signal();

            int64_t start   = GetTimeNowMsec();
            bool acquired   = g_TestSemaphore.Wait(NO_WAIT);
            int64_t elapsed = GetTimeNowMsec() - start;

            if (acquired && elapsed < _STK_SEM_TEST_SHORT_SLEEP)
                ++g_SharedCounter;
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_SEM_TEST_LONG_SLEEP);

            printf("zero-timeout wait: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class SignalBeforeWaitTask
    \brief Tests that Signal() before Wait() is remembered by the counter.
    \note  Verifies the "stateful" property: signals posted with no waiters
           increment m_count, and a subsequent Wait() drains them on the fast path.
*/
template <EAccessMode _AccessMode>
class SignalBeforeWaitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    SignalBeforeWaitTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Fire all signals upfront before any waiter is ready
            for (int32_t i = 0; i < m_iterations; ++i)
                g_TestSemaphore.Signal();

            // Verify count accumulated correctly
            uint32_t count_after_signals = g_TestSemaphore.GetCount();

            stk::Sleep(_STK_SEM_TEST_LONG_SLEEP);

            printf("signal-before-wait: count_after_signals=%d (expected %d), counter=%d (expected %d)\n",
                (int)count_after_signals, (int)m_iterations, (int)g_SharedCounter, (int)m_iterations);

            if ((count_after_signals == (uint32_t)m_iterations) && (g_SharedCounter == m_iterations))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Single consumer drains all pre-posted signals on the fast path
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // ensure signals are already posted

            for (int32_t i = 0; i < m_iterations; ++i)
            {
                if (g_TestSemaphore.Wait(_STK_SEM_TEST_SHORT_SLEEP))
                    ++g_SharedCounter;
            }
        }
    }
};

/*! \class FIFOOrderTask
    \brief Tests FIFO ordering of waiting tasks.
    \note  Verifies that tasks are woken in the order they blocked on Wait().
*/
template <EAccessMode _AccessMode>
class FIFOOrderTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
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
            // Task 0: producer - stagger signals so all waiters are queued before first wake
            stk::Sleep(50); // give consumers time to block in FIFO order

            for (int32_t i = 0; i < (_STK_SEM_TEST_TASKS_MAX - 1); ++i)
            {
                g_TestSemaphore.Signal();
                stk::Delay(1); // small pause between signals
            }

            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            // Check if tasks acquired in FIFO order (1, 2, 3, 4)
            bool ordered = true;
            for (int32_t i = 0; i < (_STK_SEM_TEST_TASKS_MAX - 1); ++i)
            {
                if (g_AcquisitionOrder[i] != (i + 1))
                {
                    ordered = false;
                    printf("order violation: position %d has task %d (expected %d)\n",
                        (int)i, (int)g_AcquisitionOrder[i], (int)(i + 1));
                    break;
                }
            }

            if (ordered)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: stagger entry so they queue in ascending order
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * m_task_id);

            g_TestSemaphore.Wait();

            // Record acquisition order
            int32_t idx = g_OrderIndex++;
            g_AcquisitionOrder[idx] = m_task_id;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test with many interleaved Signal/Wait cycles.
    \note  Verifies semaphore stability under heavy contention by checking that
           no signal is lost: every Signal() produces exactly one successful Wait().
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    StressTestTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: fire signals continuously
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestSemaphore.Signal();

                if ((i % 10) == 0)
                    stk::Delay(1);
            }
        }
        else
        {
            // Consumers: each drains its share of signals with a generous timeout
            int32_t share = m_iterations / (_STK_SEM_TEST_TASKS_MAX - 1);

            for (int32_t i = 0; i < share; ++i)
            {
                if (g_TestSemaphore.Wait(_STK_SEM_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }
        }

        if (m_task_id == (_STK_SEM_TEST_TASKS_MAX - 1))
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            // Remaining permits in the semaphore plus consumed ones must equal total signals
            int32_t total_consumed = g_SharedCounter;
            int32_t remaining      = (int32_t)g_TestSemaphore.GetCount();

            printf("stress test: consumed=%d remaining=%d total=%d (expected %d)\n",
                (int)total_consumed, (int)remaining, (int)(total_consumed + remaining), (int)m_iterations);

            if ((total_consumed + remaining) == m_iterations)
                g_TestResult = 1;
        }
    }
};

/*! \class BoundedBufferTask
    \brief Tests classic producer/consumer synchronization pattern.
    \note  Verifies that a semaphore correctly gates a producer and consumer:
           the producer signals N times, the consumer waits N times, and the
           total transfer count matches exactly.
*/
template <EAccessMode _AccessMode>
class BoundedBufferTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    BoundedBufferTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: write items and signal readiness
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                stk::Delay(1); // simulate work
                g_TestSemaphore.Signal();
            }
        }
        else
        if (m_task_id == 1)
        {
            // Consumer: wait for each item produced by task 0
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                if (g_TestSemaphore.Wait(_STK_SEM_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }
        }

        // Task 1 is the verifier
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            printf("bounded buffer: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)m_iterations);

            if (g_SharedCounter == m_iterations)
                g_TestResult = 1;
        }
    }
};

// Helper function to reset test state
static void ResetTestState(uint32_t initial_count = 0)
{
    g_TestResult  = 0;
    g_SharedCounter = 0;
    g_OrderIndex  = 0;
    g_TestComplete = false;

    for (int32_t i = 0; i < _STK_SEM_TEST_TASKS_MAX; ++i)
        g_AcquisitionOrder[i] = 0;

    // Re-construct the semaphore in-place with the requested initial count
    g_TestSemaphore.~Semaphore();
    new (&g_TestSemaphore) sync::Semaphore(initial_count);
}

} // namespace semaphore
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return (strcmp(test_name, "TimeoutWait") != 0) &&
           (strcmp(test_name, "ZeroTimeout") != 0) &&
           (strcmp(test_name, "SignalBeforeWait") != 0) &&
           (strcmp(test_name, "BoundedBuffer") != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0, uint32_t initial_count = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::semaphore;

    printf("Test: %s\n", test_name);

    ResetTestState(initial_count);

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

    using namespace stk::test::semaphore;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Basic Signal/Wait with producer-consumer mutual exclusion
    if (RunTest<BasicSignalWaitTask<ACCESS_PRIVILEGED>>("BasicSignalWait", 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Non-zero initial count (fast-path drain, then block)
    if (RunTest<InitialCountTask<ACCESS_PRIVILEGED>>("InitialCount", 0, _STK_SEM_TEST_TASKS_MAX - 1) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: TimedWait timeout behavior
    if (RunTest<TimeoutWaitTask<ACCESS_PRIVILEGED>>("TimeoutWait") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Wait(0) non-blocking behavior
    if (RunTest<ZeroTimeoutTask<ACCESS_PRIVILEGED>>("ZeroTimeout") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Signal posted before Wait is remembered
    if (RunTest<SignalBeforeWaitTask<ACCESS_PRIVILEGED>>("SignalBeforeWait", 200) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: FIFO wakeup ordering
    if (RunTest<FIFOOrderTask<ACCESS_PRIVILEGED>>("FIFOOrder") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Classic bounded-buffer producer/consumer
    if (RunTest<BoundedBufferTask<ACCESS_PRIVILEGED>>("BoundedBuffer", 200) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 8: Stress test (no signals lost)
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
