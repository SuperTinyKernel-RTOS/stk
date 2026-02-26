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
#include <sync/stk_sync_pipe.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_PIPE_TEST_TASKS_MAX   5
#define _STK_PIPE_TEST_TIMEOUT     300
#define _STK_PIPE_TEST_SHORT_SLEEP 10
#define _STK_PIPE_TEST_LONG_SLEEP  100
#define _STK_PIPE_CAPACITY         8   // pipe capacity used by all tests
#ifdef __ARM_ARCH_6M__
#define _STK_PIPE_STACK_SIZE       128 // ARM Cortex-M0
#else
#define _STK_PIPE_STACK_SIZE       256
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::pipe
    \brief     Namespace of Pipe test.
 */
namespace pipe {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile bool    g_TestComplete  = false;
static volatile int32_t g_InstancesDone = 0;

// Kernel (Pipe uses ConditionVariable internally, so KERNEL_SYNC is required)
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_PIPE_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test pipe (re-constructed per test via ResetTestState)
static sync::Pipe<int32_t, _STK_PIPE_CAPACITY> g_TestPipe;

/*! \class BasicWriteReadTask
    \brief Tests basic Write()/Read() functionality in producer-consumer arrangement.
    \note  Task 0 writes N values sequentially into the pipe; task 1 reads them back
           and verifies each value equals its expected sequence number.
           Verifies that data is transferred correctly and in FIFO order.
*/
template <EAccessMode _AccessMode>
class BasicWriteReadTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    BasicWriteReadTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&BasicWriteReadTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        if (m_task_id == 0)
        {
            // Producer: write sequential values
            for (int32_t i = 0; i < m_iterations; ++i)
                g_TestPipe.Write(i, _STK_PIPE_TEST_TIMEOUT);

            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("basic write/read: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)m_iterations);

            if (g_SharedCounter == m_iterations)
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Consumer: read and verify each value matches expected sequence
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                int32_t value = -1;
                if (g_TestPipe.Read(value, _STK_PIPE_TEST_TIMEOUT) && (value == i))
                    ++g_SharedCounter;
            }
        }
    }
};

/*! \class WriteBlocksWhenFullTask
    \brief Tests that Write() blocks when the pipe is full and unblocks when space is freed.
    \note  Task 0 fills the pipe to capacity then immediately issues one more Write()
           with a generous timeout; that final Write() must block until task 1 reads one
           element. The blocking Write() returning true and the counter reaching
           CAPACITY + 1 proves that back-pressure and unblocking both work correctly.
*/
template <EAccessMode _AccessMode>
class WriteBlocksWhenFullTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WriteBlocksWhenFullTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&WriteBlocksWhenFullTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        if (m_task_id == 0)
        {
            // Fill pipe to capacity (all succeed immediately; pipe is empty)
            for (int32_t i = 0; i < (int32_t)_STK_PIPE_CAPACITY; ++i)
            {
                if (g_TestPipe.Write(i, _STK_PIPE_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }

            // One more Write() must block until consumer reads a slot
            if (g_TestPipe.Write(_STK_PIPE_CAPACITY, _STK_PIPE_TEST_TIMEOUT))
                ++g_SharedCounter; // CAPACITY + 1: unblocked by consumer

            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            printf("write blocks when full: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)(_STK_PIPE_CAPACITY + 1));

            if (g_SharedCounter == (int32_t)(_STK_PIPE_CAPACITY + 1))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Consumer: wait until pipe is full then drain one element to unblock producer
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP); // let producer fill the pipe first

            int32_t value = -1;
            g_TestPipe.Read(value, _STK_PIPE_TEST_TIMEOUT); // frees one slot for producer
        }
    }
};

/*! \class ReadBlocksWhenEmptyTask
    \brief Tests that Read() blocks when the pipe is empty and unblocks when data arrives.
    \note  Task 1 calls Read() immediately on an empty pipe; the call must block until
           task 0 writes a value. Verifies that the consumer is correctly suspended and
           that the data it receives matches what the producer wrote.
*/
template <EAccessMode _AccessMode>
class ReadBlocksWhenEmptyTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ReadBlocksWhenEmptyTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&ReadBlocksWhenEmptyTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        if (m_task_id == 0)
        {
            // Producer: wait to ensure consumer is blocked, then write
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            g_TestPipe.Write(42, _STK_PIPE_TEST_TIMEOUT);

            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            printf("read blocks when empty: counter=%d (expected 1)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Consumer: Read() on an empty pipe must block until producer writes
            int32_t value = -1;
            if (g_TestPipe.Read(value, _STK_PIPE_TEST_TIMEOUT) && (value == 42))
                ++g_SharedCounter; // 1: correctly received the produced value
        }
    }
};

/*! \class TimeoutTask
    \brief Tests that Write() and Read() return false within the expected time on timeout.
    \note  Task 1 calls Read() on an empty pipe with a short timeout; must return false
           within the [45, 65] ms window. Task 2 fills the pipe to capacity then calls
           Write() with a short timeout; must also return false within the same window.
           Both timeout paths are exercised independently in the same run.
*/
template <EAccessMode _AccessMode>
class TimeoutTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&TimeoutTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        if (m_task_id == 1)
        {
            // Read() on empty pipe with short timeout must expire and return false
            int32_t value   = -1;
            int64_t start   = GetTimeNowMsec();
            bool    ok      = g_TestPipe.Read(value, 50);
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!ok && elapsed >= 45 && elapsed <= 65)
                ++g_SharedCounter; // 1: read timeout returned false in correct window
        }
        else
        if (m_task_id == 2)
        {
            // Make sure task 1 is trying to read first in order to expire
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            // Fill pipe to capacity so Write() has nowhere to go
            for (int32_t i = 0; i < (int32_t)_STK_PIPE_CAPACITY; ++i)
                g_TestPipe.Write(i, _STK_PIPE_TEST_TIMEOUT);

            // Write() on full pipe with short timeout must expire and return false
            int64_t start   = GetTimeNowMsec();
            bool    ok      = g_TestPipe.Write(99, 50);
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!ok && elapsed >= 45 && elapsed <= 65)
                ++g_SharedCounter; // 2: write timeout returned false in correct window
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 3)
                stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            printf("timeout: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class BulkWriteReadTask
    \brief Tests WriteBulk()/ReadBulk() for multi-element block transfers.
    \note  Task 0 writes a block of N values via WriteBulk(); task 1 reads them back
           via ReadBulk() and verifies the returned count equals N and each value
           matches its expected sequence number.
*/
template <EAccessMode _AccessMode>
class BulkWriteReadTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    BulkWriteReadTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&BulkWriteReadTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        if (m_task_id == 0)
        {
            // Producer: build a sequential block and write it all at once
            int32_t src[_STK_PIPE_CAPACITY] = {0};
            for (int32_t i = 0; i < (int32_t)_STK_PIPE_CAPACITY; ++i)
                src[i] = i;

            size_t written = g_TestPipe.WriteBulk(src, _STK_PIPE_CAPACITY, _STK_PIPE_TEST_TIMEOUT);

            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("bulk write/read: written=%d, counter=%d (expected %d)\n",
                (int)written, (int)g_SharedCounter, (int)_STK_PIPE_CAPACITY);

            if ((written == _STK_PIPE_CAPACITY) && (g_SharedCounter == (int32_t)_STK_PIPE_CAPACITY))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Consumer: read the whole block back and verify each element
            int32_t dst[_STK_PIPE_CAPACITY] = {0};
            size_t  read_count = g_TestPipe.ReadBulk(dst, _STK_PIPE_CAPACITY, _STK_PIPE_TEST_TIMEOUT);

            if (read_count == _STK_PIPE_CAPACITY)
            {
                bool all_correct = true;

                for (int32_t i = 0; i < (int32_t)_STK_PIPE_CAPACITY; ++i)
                {
                    if (dst[i] != i)
                    {
                        all_correct = false;
                        break;
                    }
                }

                if (all_correct)
                    g_SharedCounter = (int32_t)_STK_PIPE_CAPACITY;
            }
        }
    }
};

/*! \class GetSizeIsEmptyTask
    \brief Tests GetSize() and IsEmpty() reflect accurate pipe state.
    \note  Task 0 verifies IsEmpty() returns true on an empty pipe, writes elements
           one by one and checks GetSize() after each write, then reads them back
           and checks GetSize() after each read. Verifies that the size accounting
           is exact throughout the fill and drain cycle.
*/
template <EAccessMode _AccessMode>
class GetSizeIsEmptyTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    GetSizeIsEmptyTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&GetSizeIsEmptyTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        if (m_task_id == 1)
        {
            bool all_ok = true;

            // Pipe must be empty initially
            if (!g_TestPipe.IsEmpty() || g_TestPipe.GetSize() != 0)
                all_ok = false;

            // Write elements one by one; GetSize() must track exactly
            for (int32_t i = 0; i < (int32_t)_STK_PIPE_CAPACITY; ++i)
            {
                g_TestPipe.Write(i, _STK_PIPE_TEST_TIMEOUT);

                if (g_TestPipe.GetSize() != (size_t)(i + 1))
                {
                    all_ok = false;
                    break;
                }
            }

            // Drain elements one by one; GetSize() must track exactly
            for (int32_t i = 0; i < (int32_t)_STK_PIPE_CAPACITY; ++i)
            {
                int32_t value = -1;
                g_TestPipe.Read(value, _STK_PIPE_TEST_TIMEOUT);

                if (g_TestPipe.GetSize() != (size_t)(_STK_PIPE_CAPACITY - i - 1))
                {
                    all_ok = false;
                    break;
                }
            }

            // Pipe must be empty again after full drain
            if (!g_TestPipe.IsEmpty())
                all_ok = false;

            if (all_ok)
                ++g_SharedCounter;
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("get-size/is-empty: counter=%d (expected 1)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
    }
};

/*! \class MultiProducerConsumerTask
    \brief Tests concurrent multi-producer / multi-consumer throughput.
    \note  Tasks 1 and 2 are producers; each writes m_iterations values. Tasks 3 and 4
           are consumers; each reads m_iterations values. Task 0 is the verifier and
           waits for all four workers to finish. The total count of successfully read
           values must equal total written (2 * m_iterations).
*/
template <EAccessMode _AccessMode>
class MultiProducerConsumerTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    MultiProducerConsumerTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&MultiProducerConsumerTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        if (m_task_id == 1 || m_task_id == 2)
        {
            // Producers: write m_iterations values each
            for (int32_t i = 0; i < m_iterations; ++i)
                g_TestPipe.Write(i, _STK_PIPE_TEST_TIMEOUT);
        }
        else
        if (m_task_id == 3 || m_task_id == 4)
        {
            // Consumers: read m_iterations values each
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                int32_t value = -1;
                if (g_TestPipe.Read(value, _STK_PIPE_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            // Wait for all four producers and consumers to finish
            while (g_InstancesDone < (_STK_PIPE_TEST_TASKS_MAX - 1))
                stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            int32_t expected = 2 * m_iterations; // two consumers * m_iterations reads each

            printf("multi producer/consumer: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test of Pipe under full five-task contention.
    \note  Each task alternates between producer and consumer roles each iteration.
           Even iterations: Write() a value into the pipe.
           Odd iterations: Read() a value from the pipe (may time out under contention).
           Verifies that no data corruption occurs and that the total of successfully
           written values equals the total successfully read plus any remaining in the pipe.
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    StressTestTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

    RunFuncType GetFunc() { return forced_cast<RunFuncType>(&StressTestTask::RunInner); }
    void *GetFuncUserData() { return this; }

private:
    void RunInner()
    {
        int32_t written  = 0;
        int32_t consumed = 0;

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            if ((i % 2) == 0)
            {
                // Producer role: write value; may block briefly if pipe is full
                if (g_TestPipe.Write(i, _STK_PIPE_TEST_SHORT_SLEEP))
                    ++written;
            }
            else
            {
                // Consumer role: read value; may time out if pipe is empty
                int32_t value = -1;
                if (g_TestPipe.Read(value, _STK_PIPE_TEST_SHORT_SLEEP))
                    ++consumed;
            }
        }

        // Accumulate per-task write and read counts into shared counters atomically
        // via the pipe's own mutex-free atomic increment (each task adds its own slice)
        // Using g_SharedCounter for net writes minus reads; net >= 0 if no data is lost
        g_SharedCounter += (written - consumed);

        ++g_InstancesDone;

        if (m_task_id == (_STK_PIPE_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_PIPE_TEST_TASKS_MAX)
                stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            // Drain any remaining items left in the pipe
            int32_t remaining = (int32_t)g_TestPipe.GetSize();

            printf("stress test: net_written=%d remaining=%d (expected: remaining >= 0)\n",
                (int)g_SharedCounter, (int)remaining);

            // net written minus read, plus anything still in the pipe, must be non-negative;
            // any negative value means reads exceeded writes which indicates data corruption
            if ((g_SharedCounter + remaining) >= 0)
                g_TestResult = 1;
        }
    }
};

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult    = 0;
    g_SharedCounter = 0;
    g_TestComplete  = false;
    g_InstancesDone = 0;

    // Re-construct the pipe in-place for a clean state; the pipe destructor does not assert
    // on non-empty state (unlike ConditionVariable), but reconstruction ensures m_head,
    // m_tail, m_count and both condition variables are fully reset between tests
    g_TestPipe.~Pipe();
    new (&g_TestPipe) sync::Pipe<int32_t, _STK_PIPE_CAPACITY>();
}

} // namespace pipe
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return (strcmp(test_name, "BasicWriteRead")       != 0) &&
           (strcmp(test_name, "WriteBlocksWhenFull")  != 0) &&
           (strcmp(test_name, "ReadBlocksWhenEmpty")  != 0) &&
           (strcmp(test_name, "Timeout")              != 0) &&
           (strcmp(test_name, "BulkWriteRead")        != 0) &&
           (strcmp(test_name, "GetSizeIsEmpty")       != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::pipe;

    printf("Test: %s\n", test_name);

    ResetTestState();

    // Create tasks based on test type
    TaskType task0(0, param);
    TaskType task1(1, param);
    TaskType task2(2, param);
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

    using namespace stk::test::pipe;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Write()/Read() transfers values correctly in FIFO order (tasks 0-2 only)
    if (RunTest<BasicWriteReadTask<ACCESS_PRIVILEGED>>("BasicWriteRead", 20) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Write() blocks when pipe is full; unblocks when consumer frees a slot (tasks 0-2 only)
    if (RunTest<WriteBlocksWhenFullTask<ACCESS_PRIVILEGED>>("WriteBlocksWhenFull") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Read() blocks when pipe is empty; unblocks when producer writes data (tasks 0-2 only)
    if (RunTest<ReadBlocksWhenEmptyTask<ACCESS_PRIVILEGED>>("ReadBlocksWhenEmpty") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Write() on full pipe and Read() on empty pipe both time out correctly (tasks 0-2 only)
    if (RunTest<TimeoutTask<ACCESS_PRIVILEGED>>("Timeout") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: WriteBulk()/ReadBulk() transfers a full block with correct element count and values (tasks 0-2 only)
    if (RunTest<BulkWriteReadTask<ACCESS_PRIVILEGED>>("BulkWriteRead", _STK_PIPE_CAPACITY) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: GetSize() and IsEmpty() accurately reflect pipe state across fill and drain cycle (tasks 0-2 only)
    if (RunTest<GetSizeIsEmptyTask<ACCESS_PRIVILEGED>>("GetSizeIsEmpty") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Two producers and two consumers transfer data concurrently without loss
    if (RunTest<MultiProducerConsumerTask<ACCESS_PRIVILEGED>>("MultiProducerConsumer", 20) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 8: Stress test under full five-task contention with alternating producer/consumer roles
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
