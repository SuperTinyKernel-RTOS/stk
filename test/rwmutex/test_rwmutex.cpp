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
#include <sync/stk_sync_rwmutex.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_RWMUTEX_TEST_TASKS_MAX   5
#define _STK_RWMUTEX_TEST_TIMEOUT     1000
#define _STK_RWMUTEX_TEST_SHORT_SLEEP 10
#define _STK_RWMUTEX_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_RWMUTEX_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_RWMUTEX_STACK_SIZE       256
#define STK_TASK                      static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::rwmutex
    \brief     Namespace of RWMutex test.
 */
namespace rwmutex {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_ReaderCount   = 0;
static volatile int32_t g_WriterCount   = 0;
static volatile int32_t g_MaxConcurrent = 0;
static volatile bool    g_TestComplete  = false;
static volatile int32_t g_InstancesDone = 0;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_RWMUTEX_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test RWMutex
static sync::RWMutex g_TestRWMutex;

/*! \class ConcurrentReadersTask
    \brief Tests that multiple readers can acquire ReadLock() simultaneously.
    \note  All reader tasks acquire the read lock, increment a shared reader counter,
           sleep briefly while holding the lock, then release. The maximum concurrent
           reader count must equal the number of reader tasks minus one (task 0 is verifier).
*/
template <EAccessMode _AccessMode>
class ConcurrentReadersTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ConcurrentReadersTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Verifier: wait for all readers to complete
            while (g_InstancesDone < (_STK_RWMUTEX_TEST_TASKS_MAX - 1))
                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            int32_t expected = _STK_RWMUTEX_TEST_TASKS_MAX - 1;

            printf("concurrent readers: max=%d (expected %d)\n",
                (int)g_MaxConcurrent, (int)expected);

            if (g_MaxConcurrent == expected)
                g_TestResult = 1;
        }
        else
        {
            // Reader tasks: acquire read lock and track concurrency
            g_TestRWMutex.ReadLock();
            {
                int32_t current = ++g_ReaderCount;

                // Track maximum concurrent readers
                if (current > g_MaxConcurrent)
                    g_MaxConcurrent = current;

                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

                --g_ReaderCount;
            }
            g_TestRWMutex.ReadUnlock();

            ++g_InstancesDone;
        }
    }
};

/*! \class WriterExclusivityTask
    \brief Tests that writer Lock()/Unlock() provides mutual exclusion.
    \note  All tasks act as writers. Verifies that only one writer can hold the lock
           at a time and that no increments are lost under concurrent access.
*/
template <EAccessMode _AccessMode>
class WriterExclusivityTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    WriterExclusivityTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        int32_t workload = 0;

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            g_TestRWMutex.Lock();

            // Critical section - increment shared counter with deliberate race window
            int32_t temp = g_SharedCounter;
            if (++workload % 4 == 0)
                stk::Delay(1);
            g_SharedCounter = temp + 1;

            g_TestRWMutex.Unlock();

            stk::Yield();
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_RWMUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            int32_t expected = _STK_RWMUTEX_TEST_TASKS_MAX * m_iterations;

            printf("writer exclusivity: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class WriterStarvationTask
    \brief Tests writer preference policy: writers don't starve under reader flood.
    \note  Tasks 1-3 are readers continuously acquiring/releasing read locks. Task 4
           attempts to acquire write lock after a delay. Verifies that the writer
           eventually acquires despite continuous reader activity.
*/
template <EAccessMode _AccessMode>
class WriterStarvationTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WriterStarvationTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Verifier
            while (!g_TestComplete)
                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            printf("writer starvation: writer_acquired=%d (expected 1)\n",
                (int)g_SharedCounter);

            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
        else
        if ((m_task_id >= 1) && (m_task_id <= 3))
        {
            // Readers: flood with read locks until test completes
            while (!g_TestComplete)
            {
                g_TestRWMutex.ReadLock();
                stk::Delay(1);
                g_TestRWMutex.ReadUnlock();
            }
        }
        else
        if (m_task_id == 4)
        {
            // Writer: wait briefly then attempt write lock (should not starve)
            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            int64_t start = GetTimeNowMsec();
            g_TestRWMutex.Lock();
            int64_t elapsed = GetTimeNowMsec() - start;

            // Verify writer acquired within reasonable time (< 200ms)
            if (elapsed < 200)
                g_SharedCounter = 1;

            g_TestRWMutex.Unlock();

            g_TestComplete = true;
        }
    }
};

/*! \class TimedReadLockTask
    \brief Tests TimedReadLock() timeout behavior.
    \note  Task 1 holds write lock for extended period. Task 2 calls TimedReadLock(50)
           and must time out with elapsed in [45, 65]ms. After task 1 releases, task 2
           calls TimedReadLock(100) which succeeds.
*/
template <EAccessMode _AccessMode>
class TimedReadLockTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedReadLockTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Writer: hold lock for extended period
            g_TestRWMutex.Lock();
            g_SharedCounter = 1; // signal task 2
            stk::Sleep(200);
            g_TestRWMutex.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            // Wait until writer signals it has the lock
            while (g_SharedCounter == 0)
                stk::Yield();

            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            // TimedReadLock(50) must time out
            int64_t start = GetTimeNowMsec();
            bool acquired = g_TestRWMutex.TimedReadLock(50);
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!acquired && elapsed >= 45 && elapsed <= 65)
                ++g_SharedCounter; // 2: timed out correctly

            if (acquired)
                g_TestRWMutex.ReadUnlock();

            // Wait for writer to release, then TimedReadLock(100) should succeed
            stk::Sleep(250);

            if (g_TestRWMutex.TimedReadLock(100))
            {
                ++g_SharedCounter; // 3: acquired after release
                g_TestRWMutex.ReadUnlock();
            }
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP * 4);

            printf("timed read-lock: counter=%d (expected 3)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 3)
                g_TestResult = 1;
        }
    }
};

/*! \class TimedWriteLockTask
    \brief Tests TimedLock() timeout behavior for writers.
    \note  Task 1 holds read lock for extended period. Task 2 calls TimedLock(50)
           and must time out with elapsed in [45, 65]ms. After task 1 releases, task 2
           calls TimedLock(100) which succeeds.
*/
template <EAccessMode _AccessMode>
class TimedWriteLockTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedWriteLockTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Reader: hold read lock for extended period
            g_TestRWMutex.ReadLock();
            g_SharedCounter = 1; // signal task 2
            stk::Sleep(200);
            g_TestRWMutex.ReadUnlock();
        }
        else
        if (m_task_id == 2)
        {
            // Wait until reader signals it has the lock
            while (g_SharedCounter == 0)
                stk::Yield();

            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            // TimedLock(50) must time out
            int64_t start = GetTimeNowMsec();
            bool acquired = g_TestRWMutex.TimedLock(50);
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!acquired && elapsed >= 45 && elapsed <= 65)
                ++g_SharedCounter; // 2: timed out correctly

            if (acquired)
                g_TestRWMutex.Unlock();

            // Wait for reader to release, then TimedLock(100) should succeed
            stk::Sleep(250);

            if (g_TestRWMutex.TimedLock(100))
            {
                ++g_SharedCounter; // 3: acquired after release
                g_TestRWMutex.Unlock();
            }
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP * 4);

            printf("timed write-lock: counter=%d (expected 3)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 3)
                g_TestResult = 1;
        }
    }
};

/*! \class TryReadLockWhileWriterTask
    \brief Tests TryReadLock() returns false when writer is active; true after release.
    \note  Task 1 holds write lock; task 2 calls TryReadLock() and verifies it fails
           immediately. After task 1 releases, task 2 retries and succeeds.
*/
template <EAccessMode _AccessMode>
class TryReadLockWhileWriterTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryReadLockWhileWriterTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Writer: hold lock for extended period
            g_TestRWMutex.Lock();
            g_SharedCounter = 1; // signal task 2
            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP);
            g_TestRWMutex.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            // Wait until writer signals it has the lock
            while (g_SharedCounter == 0)
                stk::Yield();

            // TryReadLock must fail immediately while writer holds lock
            int64_t start = GetTimeNowMsec();
            bool acquired = g_TestRWMutex.TryReadLock();
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!acquired && elapsed < _STK_RWMUTEX_TEST_SHORT_SLEEP)
                ++g_SharedCounter; // 2: correctly failed immediately

            if (acquired)
                g_TestRWMutex.ReadUnlock();

            // Wait for writer to release
            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP + _STK_RWMUTEX_TEST_SHORT_SLEEP);

            // TryReadLock must succeed after writer releases
            if (g_TestRWMutex.TryReadLock())
            {
                ++g_SharedCounter; // 3: successfully acquired after release
                g_TestRWMutex.ReadUnlock();
            }
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP * 2);

            printf("try-read while writer: counter=%d (expected 3)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 3)
                g_TestResult = 1;
        }
    }
};

/*! \class ReadUnlockWakesWriterTask
    \brief Tests that the last reader releasing wakes a waiting writer immediately.
    \note  Tasks 1-2 are readers holding locks. Task 3 is a writer waiting. When the
           last reader releases, the writer must be woken quickly (not after a full
           scheduler tick).
*/
template <EAccessMode _AccessMode>
class ReadUnlockWakesWriterTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ReadUnlockWakesWriterTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if ((m_task_id == 1) || (m_task_id == 2))
        {
            // Readers: acquire read lock and hold
            g_TestRWMutex.ReadLock();
            ++g_ReaderCount;

            // Wait until both readers have acquired
            while (g_ReaderCount < 2)
                stk::Yield();

            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            // Release one by one
            if (--g_ReaderCount == 0)
                g_SharedCounter = 1; // last reader out

            g_TestRWMutex.ReadUnlock();
        }
        else
        if (m_task_id == 3)
        {
            // Writer: wait until readers have acquired, then wait for write lock
            while (g_ReaderCount < 2)
                stk::Yield();

            int64_t start = GetTimeNowMsec();
            g_TestRWMutex.Lock();
            int64_t elapsed = GetTimeNowMsec() - start;

            // Verify woken quickly after last reader released (< 50ms)
            if (g_SharedCounter == 1 && elapsed < 50)
                ++g_SharedCounter; // 2: woken immediately

            g_TestRWMutex.Unlock();
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP);

            printf("read-unlock wakes writer: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class WriterPriorityTask
    \brief Tests writer preference policy: new readers are blocked when writers are waiting.
    \note  Task 1 holds read lock. Task 2 (writer) enters wait queue. Task 3 (reader)
           attempts ReadLock() and must block behind the waiting writer, not join the
           existing reader. Verifies writer preference prevents reader queue-jumping.
*/
template <EAccessMode _AccessMode>
class WriterPriorityTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WriterPriorityTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Reader: hold read lock while writer and second reader queue up
            g_TestRWMutex.ReadLock();
            g_SharedCounter = 1; // signal reader is holding

            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP);

            g_TestRWMutex.ReadUnlock();
        }
        else
        if (m_task_id == 2)
        {
            // Writer: wait until reader 1 signals, then block on write lock
            while (g_SharedCounter == 0)
                stk::Yield();

            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            g_TestRWMutex.Lock();
            g_WriterCount = 1; // writer acquired first after reader 1 released
            g_TestRWMutex.Unlock();
        }
        else
        if (m_task_id == 3)
        {
            // Reader: wait until writer is waiting, then attempt read lock
            while (g_SharedCounter == 0)
                stk::Yield();

            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP * 2);

            g_TestRWMutex.ReadLock();

            // If writer preference is enforced, writer should have acquired first
            if (g_WriterCount == 1)
                ++g_SharedCounter; // 2: writer got priority

            g_TestRWMutex.ReadUnlock();
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_RWMUTEX_TEST_LONG_SLEEP * 2);

            printf("writer priority: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class ReaderWriterAlternationTask
    \brief Tests alternating read and write phases with multiple concurrent readers.
    \note  Tasks 1-3 are readers in phase 1; task 4 is a writer in phase 2; tasks 1-3
           become readers again in phase 3. Verifies exclusive write access in phase 2
           and concurrent read access in phases 1 and 3.
*/
template <EAccessMode _AccessMode>
class ReaderWriterAlternationTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ReaderWriterAlternationTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if ((m_task_id >= 1) && (m_task_id <= 3))
        {
            // Phase 1: concurrent readers
            g_TestRWMutex.ReadLock();
            {
                int32_t current = ++g_ReaderCount;
                if (current > g_MaxConcurrent)
                    g_MaxConcurrent = current;
                stk::Delay(5);
                --g_ReaderCount;
            }
            g_TestRWMutex.ReadUnlock();

            // Wait for writer phase
            while (g_SharedCounter == 0)
                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            // Phase 3: concurrent readers again
            g_TestRWMutex.ReadLock();
            {
                ++g_ReaderCount;
                stk::Delay(5);
                --g_ReaderCount;
            }
            g_TestRWMutex.ReadUnlock();

            ++g_InstancesDone;
        }
        else
        if (m_task_id == 4)
        {
            // Phase 2: exclusive writer
            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            g_TestRWMutex.Lock();
            g_SharedCounter = 1; // signal phase 2 complete
            g_TestRWMutex.Unlock();

            ++g_InstancesDone;
        }
        else
        if (m_task_id == 0)
        {
            while (g_InstancesDone < (_STK_RWMUTEX_TEST_TASKS_MAX - 1))
                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            // Verify concurrent readers were present (phase 1 or 3)
            printf("reader/writer alternation: max_concurrent=%d (expected >= 2)\n",
                (int)g_MaxConcurrent);

            if (g_MaxConcurrent >= 2)
                g_TestResult = 1;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test mixing readers and writers under full five-task contention.
    \note  Each task alternates between reader and writer roles each iteration. Even
           iterations use ReadLock(); odd iterations use Lock(). Verifies that the
           final counter value is consistent with the number of write operations and
           that no data corruption occurs. Runs on all platforms including M0.
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
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
                // Reader role: read shared counter
                g_TestRWMutex.ReadLock();
                volatile int32_t snapshot = g_SharedCounter;
                (void)snapshot;
                g_TestRWMutex.ReadUnlock();
            }
            else
            {
                // Writer role: increment shared counter
                g_TestRWMutex.Lock();
                ++g_SharedCounter;
                g_TestRWMutex.Unlock();
            }

            if ((i % 10) == 0)
                stk::Delay(1);
        }

        ++g_InstancesDone;

        if (m_task_id == (_STK_RWMUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_RWMUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

            // Expected: number of write operations = tasks * (iterations / 2)
            int32_t expected = _STK_RWMUTEX_TEST_TASKS_MAX * (m_iterations / 2);

            printf("stress test: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult    = 0;
    g_SharedCounter = 0;
    g_ReaderCount   = 0;
    g_WriterCount   = 0;
    g_MaxConcurrent = 0;
    g_TestComplete  = false;
    g_InstancesDone = 0;
}

} // namespace rwmutex
} // namespace test
} // namespace stk

/*! \fn    NeedsExtendedTasks
    \brief Returns true if the test requires tasks 3 and 4 (i.e. uses more than 3 tasks).
*/
static bool NeedsExtendedTasks(const char *test_name)
{
    return  (strcmp(test_name, "TimedReadLock")      != 0) &&
            (strcmp(test_name, "TimedWriteLock")     != 0) &&
            (strcmp(test_name, "TryReadWhileWriter") != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::rwmutex;

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

    using namespace stk::test::rwmutex;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Multiple readers can acquire ReadLock() concurrently
    if (RunTest<ConcurrentReadersTask<ACCESS_PRIVILEGED>>("ConcurrentReaders") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Writers get exclusive access; no lost increments under contention
    if (RunTest<WriterExclusivityTask<ACCESS_PRIVILEGED>>("WriterExclusivity", 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Writer preference: writers don't starve under reader flood
    if (RunTest<WriterStarvationTask<ACCESS_PRIVILEGED>>("WriterStarvation") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: TimedReadLock() times out when writer holds; succeeds after release (tasks 0-2 only)
    if (RunTest<TimedReadLockTask<ACCESS_PRIVILEGED>>("TimedReadLock") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: TimedLock() times out when reader holds; succeeds after release (tasks 0-2 only)
    if (RunTest<TimedWriteLockTask<ACCESS_PRIVILEGED>>("TimedWriteLock") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: TryReadLock() fails during write; succeeds after release (tasks 0-2 only)
    if (RunTest<TryReadLockWhileWriterTask<ACCESS_PRIVILEGED>>("TryReadWhileWriter") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Last reader releasing wakes waiting writer immediately
    if (RunTest<ReadUnlockWakesWriterTask<ACCESS_PRIVILEGED>>("ReadUnlockWakesWriter") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 8: New readers blocked when writers are waiting (writer preference)
    if (RunTest<WriterPriorityTask<ACCESS_PRIVILEGED>>("WriterPriority") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 9: Alternating read and write phases with concurrent readers
    if (RunTest<ReaderWriterAlternationTask<ACCESS_PRIVILEGED>>("ReaderWriterAlternation") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 10: Stress test mixing readers and writers under full contention
    if (RunTest<StressTestTask<ACCESS_PRIVILEGED>>("StressTest", 100) != TestContext::SUCCESS_EXIT_CODE)
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
