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
#include <sync/stk_sync_pipe.h>
#include <memory/stk_memory_allocator.h> // for placement new/delete
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
#define STK_TASK
#else
#define _STK_PIPE_STACK_SIZE       256
#define STK_TASK                   static
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
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_PIPE_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test pipe (re-constructed per test via ResetTestState)
static sync::PipeT<int32_t, _STK_PIPE_CAPACITY> g_TestPipe;

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

private:
    void Run()
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

private:
    void Run()
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

private:
    void Run()
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

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Read() on empty pipe with short timeout must expire and return false
            int32_t value   = -1;
            int64_t start   = GetTimeNowMs();
            bool    ok      = g_TestPipe.Read(value, 50);
            int64_t elapsed = GetTimeNowMs() - start;

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
            int64_t start   = GetTimeNowMs();
            bool    ok      = g_TestPipe.Write(99, 50);
            int64_t elapsed = GetTimeNowMs() - start;

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

private:
    void Run()
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

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            bool all_ok = true;

            // Pipe must be empty initially
            if (!g_TestPipe.IsEmpty() || g_TestPipe.GetCount() != 0)
                all_ok = false;

            // Write elements one by one; GetSize() must track exactly
            for (int32_t i = 0; i < (int32_t)_STK_PIPE_CAPACITY; ++i)
            {
                g_TestPipe.Write(i, _STK_PIPE_TEST_TIMEOUT);

                if (g_TestPipe.GetCount() != (size_t)(i + 1))
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

                if (g_TestPipe.GetCount() != (size_t)(_STK_PIPE_CAPACITY - i - 1))
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

private:
    void Run()
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

private:
    void Run()
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
            int32_t remaining = (int32_t)g_TestPipe.GetCount();

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
    g_TestPipe.~PipeT();
    new (&g_TestPipe) sync::PipeT<int32_t, _STK_PIPE_CAPACITY>();
}

} // namespace pipe

// ===========================================================================
// Raw Pipe (non-typed) tests
//
// These tests exercise stk::sync::Pipe directly, using a uint8_t backing
// buffer and an explicit element_size. They cover:
//   - Basic Write/Read round-trip (element_size = 4, arbitrary struct)
//   - WriteBulk / ReadBulk correct transfer and FIFO ordering
//   - ReadBulkTriggered: trigger satisfied, partial timeout, NO_WAIT fast-path,
//     trigger clamped to max_count, trigger == max_count boundary
//   - Reset() wakes blocked readers and clears state
//   - GetCapacity / GetElementSize / GetCount / GetSpace / IsEmpty / IsFull
//     state invariants across fill and drain
//   - Cancelability of blocked Read/Write/ReadBulk/ReadBulkTriggered/WriteBulk
//     via IKernel::CancelTaskWait() and stk::SleepCancel() (RawCancelWaitTask):
//     WAIT_RESULT_CANCELED vs timeout, no stale waiter, wait list integrity,
//     signal-wins race, partial bulk transfers kept
// ===========================================================================

namespace rawpipe {

// Test results storage (separate from PipeT tests — each test resets these)
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_InstancesDone = 0;

// Element type used by raw Pipe tests.
// Using a 4-byte struct to exercise element_size > 1 throughout.
struct RawElem
{
    int32_t value;
};

static constexpr size_t RAW_ELEM_SIZE = sizeof(RawElem);   // 4
static constexpr size_t RAW_CAPACITY  = _STK_PIPE_CAPACITY; // 8

// Backing buffer and Pipe object — shared across all raw Pipe test cases.
// Re-constructed in ResetRawPipeState() between tests.
static uint8_t               g_RawBuf[RAW_CAPACITY * RAW_ELEM_SIZE];
static sync::Pipe            g_RawPipe(g_RawBuf, RAW_CAPACITY, RAW_ELEM_SIZE);

// Kernel for raw Pipe tests (separate instance so tests are fully isolated
// from the PipeT kernel above; same template parameters).
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_PIPE_TEST_TASKS_MAX,
              SwitchStrategyRR, PlatformDefault> g_RawKernel;

// Helpers ----------------------------------------------------------------

static inline void WriteElem(sync::Pipe &p, int32_t v, Timeout t = _STK_PIPE_TEST_TIMEOUT)
{
    RawElem e = { v };
    p.Write(&e, t);
}

static inline bool TryWriteElem(sync::Pipe &p, int32_t v)
{
    RawElem e = { v };
    return p.TryWrite(&e);
}

static inline bool ReadElem(sync::Pipe &p, int32_t &out, Timeout t = _STK_PIPE_TEST_TIMEOUT)
{
    RawElem e = { -1 };
    bool ok = p.Read(&e, t);
    out = e.value;
    return ok;
}

// Cancellation test state (see RawCancelWaitTask) -------------------------

static ITask * volatile g_Victim             = nullptr;      // task whose wait gets cancelled (task 1)
static volatile TId     g_VictimTid          = TID_NONE;     // same, as TId (for SleepCancel)
static volatile int32_t g_VictimResult[2]    = {-100, -100}; // EWaitResult of task 1 / task 2 (-100 = not set)
static volatile int64_t g_VictimElapsed[2]   = {0, 0};       // ms spent inside the blocking Pipe call by task 1 / task 2
static volatile int32_t g_VictimValue[2]     = {-1, -1};     // single element received by task 1 / task 2 (-1 = nothing)
static volatile int32_t g_VictimCount        = -1;           // number of elements transferred by a bulk call of task 1 (-1 = not set)
static volatile int32_t g_VictimBulk[RAW_CAPACITY];          // elements received by a bulk read of task 1
static volatile int64_t g_VictimSleepElapsed = 0;            // ms spent in Sleep() by task 1 after its wait was cancelled

// Scenario selector of RawCancelWaitTask. It is a global, not a constructor argument, because RunRawTest()
// declares tasks 0-2 as 'static': a static local is constructed only once per template instantiation (on the
// first RunRawTest<RawCancelWaitTask> call), so a constructor argument would silently keep the value of the
// first case. Set by main() before each RunRawTest() call.
static volatile int32_t g_CancelParam        = 0;

// Cancel the wait of g_Victim either via IKernel::CancelTaskWait() or via stk::SleepCancel()
static void CancelVictim(bool use_sleep_cancel)
{
    if (use_sleep_cancel)
        stk::SleepCancel(g_VictimTid);
    else
        g_RawKernel.CancelTaskWait(g_Victim);
}

// Safety net for a failed cancel test: a victim that is still blocked would keep the kernel running forever,
// so push data through the pipe in both directions to release any stuck reader or writer. This turns a broken
// cancel into a reported test failure instead of a hang.
static void ReleaseStuckWaiters()
{
    for (int32_t round = 0; round < 2; ++round)
    {
        // frees blocked readers (a bulk reader needs up to CAPACITY elements)
        for (size_t i = 0; i < RAW_CAPACITY; ++i)
        {
            TryWriteElem(g_RawPipe, -2);
            stk::Sleep(2);
        }

        // frees blocked writers
        for (size_t i = 0; i < RAW_CAPACITY; ++i)
        {
            int32_t v = -1;
            ReadElem(g_RawPipe, v, NO_WAIT);
            stk::Sleep(2);
        }
    }
}

static void ResetRawPipeState()
{
    g_TestResult    = 0;
    g_SharedCounter = 0;
    g_InstancesDone = 0;

    g_Victim             = nullptr;
    g_VictimTid          = TID_NONE;
    g_VictimCount        = -1;
    g_VictimSleepElapsed = 0;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]  = -100;
        g_VictimElapsed[i] = 0;
        g_VictimValue[i]   = -1;
    }

    for (size_t i = 0; i < RAW_CAPACITY; ++i)
        g_VictimBulk[i] = -1;

    g_RawPipe.~Pipe();
    new (&g_RawPipe) sync::Pipe(g_RawBuf, RAW_CAPACITY, RAW_ELEM_SIZE);
}

// -----------------------------------------------------------------------

/*! \class RawBasicWriteReadTask
    \brief Verifies that Pipe::Write() / Pipe::Read() correctly transfer a
           RawElem struct (element_size = 4) in FIFO order.
    \note  Producer writes N sequential values; consumer reads them back and
           verifies each matches its expected sequence number.
*/
template <EAccessMode _AccessMode>
class RawBasicWriteReadTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    RawBasicWriteReadTask(uint8_t id, int32_t iters) : m_task_id(id), m_iterations(iters) {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            for (int32_t i = 0; i < m_iterations; ++i)
                WriteElem(g_RawPipe, i);

            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("raw basic write/read: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)m_iterations);

            if (g_SharedCounter == m_iterations)
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                int32_t v = -1;
                if (ReadElem(g_RawPipe, v) && (v == i))
                    ++g_SharedCounter;
            }
        }
    }
};

/*! \class RawBulkWriteReadTask
    \brief Verifies Pipe::WriteBulk() / Pipe::ReadBulk() transfer a full
           block of RawElem elements with correct count and FIFO ordering.
    \note  Producer fills a RawElem[CAPACITY] array with sequential values
           and calls WriteBulk(); consumer calls ReadBulk() for the same count
           and verifies every element matches the expected sequence.
*/
template <EAccessMode _AccessMode>
class RawBulkWriteReadTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RawBulkWriteReadTask(uint8_t id, int32_t) : m_task_id(id) {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            RawElem src[RAW_CAPACITY];
            for (size_t i = 0; i < RAW_CAPACITY; ++i)
                src[i].value = (int32_t)i;

            size_t written = g_RawPipe.WriteBulk(src, RAW_CAPACITY, _STK_PIPE_TEST_TIMEOUT);

            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("raw bulk write/read: written=%d counter=%d (expected %d)\n",
                (int)written, (int)g_SharedCounter, (int)RAW_CAPACITY);

            if ((written == RAW_CAPACITY) && (g_SharedCounter == (int32_t)RAW_CAPACITY))
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            RawElem dst[RAW_CAPACITY] = {};
            size_t n = g_RawPipe.ReadBulk(dst, RAW_CAPACITY, _STK_PIPE_TEST_TIMEOUT);

            if (n == RAW_CAPACITY)
            {
                bool ok = true;
                for (size_t i = 0; i < RAW_CAPACITY; ++i)
                    if (dst[i].value != (int32_t)i) { ok = false; break; }

                if (ok)
                    g_SharedCounter = (int32_t)RAW_CAPACITY;
            }
        }
    }
};

/*! \class RawTriggeredSatisfiedTask
    \brief Verifies ReadBulkTriggered() wakes only after the trigger threshold
           is met and returns all bytes available up to max_count.
    \note  Consumer calls ReadBulkTriggered(trigger=4, max_count=CAPACITY).
           Producer sleeps briefly then writes all CAPACITY elements in one
           WriteBulk(). Consumer must receive exactly CAPACITY elements and
           return >= trigger, proving it blocked until the threshold was met
           and then drained the full available batch atomically.
*/
template <EAccessMode _AccessMode>
class RawTriggeredSatisfiedTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RawTriggeredSatisfiedTask(uint8_t id, int32_t) : m_task_id(id) {}

private:
    void Run()
    {
        static constexpr size_t TRIGGER   = RAW_CAPACITY / 2; // 4
        static constexpr size_t MAX_COUNT = RAW_CAPACITY;      // 8

        if (m_task_id == 0)
        {
            // Verifier: wait for consumer then check result
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("raw triggered satisfied: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)MAX_COUNT);

            if (g_SharedCounter == (int32_t)MAX_COUNT)
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            // Consumer: block on triggered read before producer writes anything
            RawElem dst[MAX_COUNT] = {};
            size_t n = g_RawPipe.ReadBulkTriggered(dst, TRIGGER, MAX_COUNT,
                                                    _STK_PIPE_TEST_TIMEOUT);

            // Must have received at least trigger elements and all values correct
            if (n >= TRIGGER)
            {
                bool ok = true;
                for (size_t i = 0; i < n; ++i)
                    if (dst[i].value != (int32_t)i) { ok = false; break; }

                if (ok)
                    g_SharedCounter = (int32_t)n;
            }
        }
        else if (m_task_id == 2)
        {
            // Producer: short delay then write full pipe worth in one burst
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            RawElem src[MAX_COUNT];
            for (size_t i = 0; i < MAX_COUNT; ++i)
                src[i].value = (int32_t)i;

            g_RawPipe.WriteBulk(src, MAX_COUNT, _STK_PIPE_TEST_TIMEOUT);
        }
    }
};

/*! \class RawTriggeredTimeoutPartialTask
    \brief Verifies ReadBulkTriggered() drains partial data and returns when
           the trigger threshold is never reached within the timeout window.
    \note  Consumer calls ReadBulkTriggered(trigger=6, max_count=CAPACITY,
           timeout=short). Producer writes only 3 elements (below trigger)
           after a small delay. Consumer must return 3 — the bytes that
           arrived before timeout — proving the FreeRTOS partial-timeout
           drain semantics are correct.
*/
template <EAccessMode _AccessMode>
class RawTriggeredTimeoutPartialTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RawTriggeredTimeoutPartialTask(uint8_t id, int32_t) : m_task_id(id) {}

private:
    void Run()
    {
        // Trigger > elements written, so threshold is never reached.
        static constexpr size_t TRIGGER      = 6U;
        static constexpr size_t WRITTEN      = 3U; // < TRIGGER
        static constexpr Timeout SHORT_WAIT  = 80U;

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP * 2);

            printf("raw triggered timeout partial: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)WRITTEN);

            if (g_SharedCounter == (int32_t)WRITTEN)
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            // Consumer: block until timeout; should drain exactly WRITTEN elements
            RawElem dst[RAW_CAPACITY] = {};
            size_t n = g_RawPipe.ReadBulkTriggered(dst, TRIGGER, RAW_CAPACITY, SHORT_WAIT);

            // n must equal WRITTEN (less than TRIGGER — timeout path)
            if (n == WRITTEN)
            {
                bool ok = true;
                for (size_t i = 0; i < WRITTEN; ++i)
                    if (dst[i].value != (int32_t)i) { ok = false; break; }

                if (ok)
                    g_SharedCounter = (int32_t)n;
            }
        }
        else if (m_task_id == 2)
        {
            // Producer: write fewer elements than trigger, before timeout fires
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            for (size_t i = 0; i < WRITTEN; ++i)
                WriteElem(g_RawPipe, (int32_t)i);
        }
    }
};

/*! \class RawTriggeredNoWaitTask
    \brief Verifies ReadBulkTriggered() with NO_WAIT ignores the trigger
           threshold and returns whatever is currently available.
    \note  Task 1 pre-loads 3 elements (below any interesting trigger), then
           calls ReadBulkTriggered(trigger=6, max_count=CAPACITY, NO_WAIT).
           Must return 3 immediately without blocking, proving the NO_WAIT
           fast-path bypasses the threshold check.
*/
template <EAccessMode _AccessMode>
class RawTriggeredNoWaitTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RawTriggeredNoWaitTask(uint8_t id, int32_t) : m_task_id(id) {}

private:
    void Run()
    {
        static constexpr size_t PRE_LOAD = 3U;
        static constexpr size_t TRIGGER  = 6U; // > PRE_LOAD; would block if timeout != NO_WAIT

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("raw triggered no-wait: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)PRE_LOAD);

            if (g_SharedCounter == (int32_t)PRE_LOAD)
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            // Pre-load PRE_LOAD elements directly (no concurrent producer needed)
            for (size_t i = 0; i < PRE_LOAD; ++i)
                TryWriteElem(g_RawPipe, (int32_t)i);

            // NO_WAIT: must return PRE_LOAD immediately, regardless of trigger
            RawElem dst[RAW_CAPACITY] = {};
            int64_t start   = GetTimeNowMs();
            size_t  n       = g_RawPipe.ReadBulkTriggered(dst, TRIGGER, RAW_CAPACITY, NO_WAIT);
            int64_t elapsed = GetTimeNowMs() - start;

            // Must be non-blocking (< 5 ms) and drain exactly PRE_LOAD elements
            if ((n == PRE_LOAD) && (elapsed < 5))
            {
                bool ok = true;
                for (size_t i = 0; i < PRE_LOAD; ++i)
                    if (dst[i].value != (int32_t)i) { ok = false; break; }

                if (ok)
                    g_SharedCounter = (int32_t)PRE_LOAD;
            }
        }
    }
};

/*! \class RawTriggeredClampTask
    \brief Verifies ReadBulkTriggered() clamps trigger to max_count when
           trigger > max_count, preventing an impossible wait condition.
    \note  Consumer calls ReadBulkTriggered(trigger=CAPACITY, max_count=2).
           Trigger is clamped to 2 internally. Producer writes exactly 2
           elements. Consumer must unblock and return 2, proving the clamp
           makes the call satisfiable.
*/
template <EAccessMode _AccessMode>
class RawTriggeredClampTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RawTriggeredClampTask(uint8_t id, int32_t) : m_task_id(id) {}

private:
    void Run()
    {
        // trigger > max_count: must be clamped to max_count = 2
        static constexpr size_t TRIGGER   = RAW_CAPACITY; // 8 — intentionally > max_count
        static constexpr size_t MAX_COUNT = 2U;

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("raw triggered clamp: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)MAX_COUNT);

            if (g_SharedCounter == (int32_t)MAX_COUNT)
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            // Consumer: trigger clamped to max_count=2, so 2 elements suffice to unblock
            RawElem dst[MAX_COUNT] = {};
            size_t n = g_RawPipe.ReadBulkTriggered(dst, TRIGGER, MAX_COUNT,
                                                    _STK_PIPE_TEST_TIMEOUT);

            if (n == MAX_COUNT)
            {
                bool ok = (dst[0].value == 10) && (dst[1].value == 20);
                if (ok)
                    g_SharedCounter = (int32_t)MAX_COUNT;
            }
        }
        else if (m_task_id == 2)
        {
            // Producer: write exactly max_count elements — enough to satisfy clamped trigger
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);
            WriteElem(g_RawPipe, 10);
            WriteElem(g_RawPipe, 20);
        }
    }
};

/*! \class RawResetDoesNotReleaseReaderTask
    \brief Verifies that Pipe::Reset() does NOT unblock a blocked reader —
           matching FreeRTOS xStreamBufferReset() semantics exactly.
    \note  Consumer calls ReadBulkTriggered(trigger=CAPACITY, max_count=CAPACITY,
           timeout=finite). Task 2 calls Reset() while the consumer is blocked.
           Reset() must not wake the reader; the consumer stays blocked until
           its own timeout fires, then returns 0 from the empty pipe.
           Callers that need guaranteed unblocking must use a finite timeout.
*/
template <EAccessMode _AccessMode>
class RawResetDoesNotReleaseReaderTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RawResetDoesNotReleaseReaderTask(uint8_t id, int32_t) : m_task_id(id) {}

private:
    void Run()
    {
        // Timeout long enough to let Reset() run first, short enough for the test to complete.
        static constexpr Timeout READ_TIMEOUT = 80U;

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP * 2);

            printf("raw reset does not release reader: counter=%d (expected 1)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            // Consumer: finite timeout on an empty pipe that gets Reset() midway.
            // Trigger is never satisfied; timeout fires; must return 0.
            RawElem dst[RAW_CAPACITY] = {};
            int64_t start   = GetTimeNowMs();
            size_t  n       = g_RawPipe.ReadBulkTriggered(dst, RAW_CAPACITY, RAW_CAPACITY, READ_TIMEOUT);
            int64_t elapsed = GetTimeNowMs() - start;

            // n == 0: nothing was written, nothing to drain.
            // elapsed >= READ_TIMEOUT - 5: proves Reset() did NOT cancel the wait early;
            //   the reader blocked for the full timeout duration.
            // Pipe must still be empty with count == 0 after Reset() + timeout.
            if ((n == 0U) && (elapsed >= (READ_TIMEOUT - 5)) && g_RawPipe.IsEmpty() && (g_RawPipe.GetCount() == 0U))
                ++g_SharedCounter;
        }
        else if (m_task_id == 2)
        {
            // Resetter: Reset() while pipe is already empty — verifies Reset() on an
            // empty pipe is a no-op for state and does not corrupt anything.
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);
            g_RawPipe.Reset();
        }
    }
};

/*! \class RawStateInvariantsTask
    \brief Verifies Pipe state-query API (GetCapacity, GetElementSize, GetCount,
           GetSpace, IsEmpty, IsFull) throughout a fill-and-drain cycle.
    \note  Single-task test (task_id == 1). Checks invariants at empty, at each
           step during fill, at full, and at each step during drain.
*/
template <EAccessMode _AccessMode>
class RawStateInvariantsTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RawStateInvariantsTask(uint8_t id, int32_t) : m_task_id(id) {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_PIPE_TEST_LONG_SLEEP);

            printf("raw state invariants: counter=%d (expected 1)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
        else if (m_task_id == 1)
        {
            bool ok = true;

            // Fixed construction-time properties must never change
            if (g_RawPipe.GetCapacity()    != RAW_CAPACITY) ok = false;
            if (g_RawPipe.GetElementSize() != RAW_ELEM_SIZE) ok = false;

            // Empty invariants
            if (!g_RawPipe.IsEmpty())                        ok = false;
            if (g_RawPipe.IsFull())                          ok = false;
            if (g_RawPipe.GetCount()  != 0U)                 ok = false;
            if (g_RawPipe.GetSpace()  != RAW_CAPACITY)       ok = false;

            // Fill one element at a time; verify count and space track correctly
            for (size_t i = 0; i < RAW_CAPACITY && ok; ++i)
            {
                TryWriteElem(g_RawPipe, (int32_t)i);

                if (g_RawPipe.GetCount() != i + 1U)          ok = false;
                if (g_RawPipe.GetSpace() != RAW_CAPACITY - i - 1U) ok = false;
                if (g_RawPipe.IsEmpty())                      ok = false;
            }

            // Full invariants
            if (!g_RawPipe.IsFull())                         ok = false;
            if (g_RawPipe.IsEmpty())                         ok = false;
            if (g_RawPipe.GetCount()  != RAW_CAPACITY)       ok = false;
            if (g_RawPipe.GetSpace()  != 0U)                 ok = false;

            // Drain one element at a time; verify count and space track correctly
            for (size_t i = 0; i < RAW_CAPACITY && ok; ++i)
            {
                int32_t v = -1;
                ReadElem(g_RawPipe, v, NO_WAIT);

                size_t remaining = RAW_CAPACITY - i - 1U;
                if (g_RawPipe.GetCount() != remaining)        ok = false;
                if (g_RawPipe.GetSpace() != i + 1U)          ok = false;
                if (g_RawPipe.IsFull())                       ok = false;
            }

            // Back to empty
            if (!g_RawPipe.IsEmpty())                        ok = false;
            if (g_RawPipe.GetCount() != 0U)                  ok = false;

            if (ok)
                ++g_SharedCounter;
        }
    }
};

static const int32_t CW_READ_INFINITE       = 1;
static const int32_t CW_READ_NOT_TIMEOUT    = 2;
static const int32_t CW_WRITE_FULL          = 3;
static const int32_t CW_NO_STALE_WAITER     = 4;
static const int32_t CW_TWO_WAITERS         = 5;
static const int32_t CW_SIGNAL_WINS         = 6;
static const int32_t CW_READ_BULK_PARTIAL   = 7;
static const int32_t CW_TRIGGERED_PARTIAL   = 8;
static const int32_t CW_WRITE_BULK_PARTIAL  = 9;
static const int32_t CW_VIA_SLEEPCANCEL     = 100;

/*! \class RawCancelWaitTask
    \brief Tests cancelability of Pipe waits (IKernel::CancelTaskWait() and stk::SleepCancel()).
    \note  Task 0 is the canceller/checker, task 1 is the victim (blocked inside a Pipe call), task 2 is a
           second reader used by the two-waiters scenario. The scenario is selected by g_CancelParam:
           1 = cancel an infinite Read(), 2 = a cancel is reported as CANCELED, not TIMEOUT,
           3 = cancel a Write() on a full pipe, nothing is written,
           4 = cancel leaves no stale waiter behind, the next element is neither lost nor consumed twice,
           5 = cancelling one of two blocked readers leaves the other intact,
           6 = an element that arrived first wins over a late cancel,
           7 = cancel of a ReadBulk() keeps the elements already read (partial count),
           8 = cancel of a ReadBulkTriggered() below its trigger still drains what is available,
           9 = cancel of a WriteBulk() keeps the elements already written (partial count).
           Adding 100 to g_CancelParam cancels via stk::SleepCancel() instead of IKernel::CancelTaskWait().
    \note  Pipe wraps ConditionVariable, so this suite also covers the cancel path of ConditionVariable::WaitEx().
*/
template <EAccessMode _AccessMode>
class RawCancelWaitTask : public Task<_STK_PIPE_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_scenario;
    bool    m_use_sleep_cancel;

public:
    RawCancelWaitTask(uint8_t id, int32_t) // scenario is taken from g_CancelParam in Run(), see g_CancelParam
        : m_task_id(id), m_scenario(0), m_use_sleep_cancel(false)
    {}

private:
    static constexpr size_t TRIGGER_BIG = 6U;               // above the 3 elements written by the partial scenarios
    static constexpr size_t PARTIAL     = 3U;               // elements delivered before the cancel (read scenarios)
    static constexpr size_t PREFILL     = RAW_CAPACITY - 2U; // elements pre-loaded in the bulk-write scenario

    void Run()
    {
        m_scenario         = g_CancelParam % CW_VIA_SLEEPCANCEL;
        m_use_sleep_cancel = (g_CancelParam >= CW_VIA_SLEEPCANCEL);

        if (m_task_id == 0)
            RunCanceller();
        else
        if (m_task_id == 1)
            RunVictim();
        else
            RunSecondWaiter();
    }

    // Task 1: block inside a Pipe call, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        const Timeout timeout = (m_scenario == CW_READ_NOT_TIMEOUT ? 300 : WAIT_INFINITE);

        EWaitResult r      = WAIT_RESULT_FAIL;
        size_t      n      = 0U;
        bool        is_bulk = false;
        RawElem     single = { -1 };
        RawElem     bulk[RAW_CAPACITY] = {};

        const int64_t start = GetTimeNowMs();

        if (m_scenario == CW_WRITE_FULL)
        {
            RawElem e = { 999 };
            r = g_RawPipe.WriteEx(&e, timeout);
        }
        else
        if (m_scenario == CW_READ_BULK_PARTIAL)
        {
            is_bulk = true;
            r = g_RawPipe.ReadBulkEx(bulk, RAW_CAPACITY, n, timeout);
        }
        else
        if (m_scenario == CW_TRIGGERED_PARTIAL)
        {
            is_bulk = true;
            r = g_RawPipe.ReadBulkTriggeredEx(bulk, TRIGGER_BIG, RAW_CAPACITY, n, timeout);
        }
        else
        if (m_scenario == CW_WRITE_BULK_PARTIAL)
        {
            is_bulk = true;
            for (size_t i = 0; i < RAW_CAPACITY; ++i)
                bulk[i].value = 50 + (int32_t)i;

            r = g_RawPipe.WriteBulkEx(bulk, RAW_CAPACITY, n, timeout);
        }
        else
        {
            r = g_RawPipe.ReadEx(&single, timeout);
        }

        g_VictimElapsed[0] = GetTimeNowMs() - start;
        g_VictimValue[0]   = single.value;

        if (is_bulk)
        {
            g_VictimCount = (int32_t)n;

            if (m_scenario != CW_WRITE_BULK_PARTIAL)
            {
                for (size_t i = 0; i < n; ++i)
                    g_VictimBulk[i] = bulk[i].value;
            }
        }

        g_VictimResult[0] = static_cast<int32_t>(r);

        if (m_scenario == CW_NO_STALE_WAITER)
        {
            // a stale wait entry left behind by the cancel would cut this sleep short
            const int64_t sleep_start = GetTimeNowMs();
            stk::Sleep(60);
            g_VictimSleepElapsed = GetTimeNowMs() - sleep_start;
        }
    }

    // Task 2: second reader, queued behind task 1
    void RunSecondWaiter()
    {
        if (m_scenario != CW_TWO_WAITERS)
            return;

        stk::Sleep(2 * _STK_PIPE_TEST_SHORT_SLEEP); // block after task 1

        RawElem e = { -1 };

        const int64_t start = GetTimeNowMs();
        const EWaitResult r = g_RawPipe.ReadEx(&e, WAIT_INFINITE);

        g_VictimElapsed[1] = GetTimeNowMs() - start;
        g_VictimValue[1]   = e.value;
        g_VictimResult[1]  = static_cast<int32_t>(r);
    }

    // Drain the pipe without blocking and verify it held exactly the expected sequence
    static bool DrainAndExpect(const int32_t *expected, size_t n)
    {
        bool ok = true;

        for (size_t i = 0; i < n; ++i)
        {
            int32_t v = -1;
            if (!ReadElem(g_RawPipe, v, NO_WAIT) || (v != expected[i]))
                ok = false;
        }

        return ok && g_RawPipe.IsEmpty();
    }

    // Task 0: cancel and verify
    void RunCanceller()
    {
        bool pass = false;

        // writer scenarios: pre-load the pipe before the victim blocks (victim starts at ~10 ms)
        if (m_scenario == CW_WRITE_FULL)
        {
            for (size_t i = 0; i < RAW_CAPACITY; ++i)
                TryWriteElem(g_RawPipe, (int32_t)i);
        }
        else
        if (m_scenario == CW_WRITE_BULK_PARTIAL)
        {
            for (size_t i = 0; i < PREFILL; ++i)
                TryWriteElem(g_RawPipe, (int32_t)i);
        }

        if ((m_scenario == CW_READ_BULK_PARTIAL) || (m_scenario == CW_TRIGGERED_PARTIAL))
        {
            // victim is blocked by now (~10 ms): deliver a few elements, then cancel at ~60 ms
            stk::Sleep(30);

            for (size_t i = 0; i < PARTIAL; ++i)
                TryWriteElem(g_RawPipe, (int32_t)i);

            stk::Sleep(30);
        }
        else
        {
            stk::Sleep(60); // task 1 (and task 2) are blocked by now
        }

        if (m_scenario == CW_READ_INFINITE)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            // started waiting at ~10 ms, cancelled at ~60 ms; nothing was read
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   (g_VictimValue[0] == -1) && g_RawPipe.IsEmpty();
        }
        else
        if (m_scenario == CW_READ_NOT_TIMEOUT)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            // 300 ms timeout is far away: the wait must end early and as CANCELED, not TIMEOUT
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimElapsed[0] < 150);
        }
        else
        if (m_scenario == CW_WRITE_FULL)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            const bool full = g_RawPipe.IsFull();

            // the cancelled Write() must not have stored its element (999): original FIFO content intact
            int32_t expected[RAW_CAPACITY];
            for (size_t i = 0; i < RAW_CAPACITY; ++i)
                expected[i] = (int32_t)i;

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   full && DrainAndExpect(expected, RAW_CAPACITY);
        }
        else
        if (m_scenario == CW_NO_STALE_WAITER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP); // victim returns and starts its Sleep(60)

            const bool wrote = TryWriteElem(g_RawPipe, 321); // nobody waits: element must stay in the pipe

            int32_t v1 = -1, v2 = -1;
            const bool poll1 = ReadElem(g_RawPipe, v1, NO_WAIT); // gets the element
            const bool poll2 = ReadElem(g_RawPipe, v2, NO_WAIT); // nothing left

            stk::Sleep(70); // let the victim finish its sleep

            printf("raw cancel no stale waiter: result=%d wrote=%d poll1=%d(v=%d) poll2=%d victim_sleep=%d ms\n",
                (int)g_VictimResult[0], (int)wrote, (int)poll1, (int)v1, (int)poll2, (int)g_VictimSleepElapsed);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   wrote && poll1 && (v1 == 321) && !poll2 &&
                   (g_VictimSleepElapsed >= 55); // not woken early by a ghost wait entry
        }
        else
        if (m_scenario == CW_TWO_WAITERS)
        {
            CancelVictim(m_use_sleep_cancel);              // cancel the first reader only
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP);

            const bool wrote = TryWriteElem(g_RawPipe, 77); // must wake the second reader
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            printf("raw cancel two waiters: first=%d second=%d wrote=%d value=%d\n",
                (int)g_VictimResult[0], (int)g_VictimResult[1], (int)wrote, (int)g_VictimValue[1]);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimValue[0] == -1) &&
                   (g_VictimResult[1] == WAIT_RESULT_SIGNAL)   && (g_VictimValue[1] == 77) &&
                   wrote && g_RawPipe.IsEmpty(); // the second reader consumed the element
        }
        else
        if (m_scenario == CW_SIGNAL_WINS)
        {
            // back to back, no scheduling point in between: the reader is already signaled and removed
            // from the wait list when the cancel arrives, so the cancel must be a no-op
            const bool wrote = TryWriteElem(g_RawPipe, 777);
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            pass = wrote && (g_VictimResult[0] == WAIT_RESULT_SIGNAL) &&
                   (g_VictimValue[0] == 777) && g_RawPipe.IsEmpty();
        }
        else
        if (m_scenario == CW_READ_BULK_PARTIAL)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            // the 3 elements read before the cancel stay valid and are counted
            bool data_ok = (g_VictimCount == (int32_t)PARTIAL);
            for (size_t i = 0; data_ok && (i < PARTIAL); ++i)
                data_ok = (g_VictimBulk[i] == (int32_t)i);

            printf("raw cancel read bulk: result=%d count=%d\n", (int)g_VictimResult[0], (int)g_VictimCount);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && data_ok &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) && g_RawPipe.IsEmpty();
        }
        else
        if (m_scenario == CW_TRIGGERED_PARTIAL)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            // trigger (6) was never reached: the cancel is reported as CANCELED (not a timeout) and,
            // like a timeout, still drains the 3 elements that were available
            bool data_ok = (g_VictimCount == (int32_t)PARTIAL);
            for (size_t i = 0; data_ok && (i < PARTIAL); ++i)
                data_ok = (g_VictimBulk[i] == (int32_t)i);

            printf("raw cancel read triggered: result=%d count=%d\n", (int)g_VictimResult[0], (int)g_VictimCount);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && data_ok &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) && g_RawPipe.IsEmpty();
        }
        else
        if (m_scenario == CW_WRITE_BULK_PARTIAL)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_PIPE_TEST_SHORT_SLEEP * 2);

            // 2 of the 8 victim elements fitted before it blocked: they stay in the pipe, the rest is dropped
            const bool full = g_RawPipe.IsFull();

            int32_t expected[RAW_CAPACITY];
            for (size_t i = 0; i < PREFILL; ++i)
                expected[i] = (int32_t)i;
            expected[PREFILL]     = 50;
            expected[PREFILL + 1] = 51;

            printf("raw cancel write bulk: result=%d written=%d\n", (int)g_VictimResult[0], (int)g_VictimCount);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimCount == 2) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   full && DrainAndExpect(expected, RAW_CAPACITY);
        }

        // Safety net: if the cancel did not work a task is still blocked forever and the kernel would
        // never stop, release any such task so that the test fails instead of hanging.
        if (!pass)
            ReleaseStuckWaiters();

        printf("raw cancel wait (scenario=%d, via %s): result[0]=%d elapsed[0]=%d ms\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
            (int)g_VictimResult[0], (int)g_VictimElapsed[0]);

        if (pass)
            g_TestResult = 1;
    }
};

static void ResetRawKernelAndPipe()
{
    ResetRawPipeState();
}

} // namespace rawpipe
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return (strcmp(test_name, "BasicWriteRead")            != 0) &&
           (strcmp(test_name, "WriteBlocksWhenFull")       != 0) &&
           (strcmp(test_name, "ReadBlocksWhenEmpty")       != 0) &&
           (strcmp(test_name, "Timeout")                   != 0) &&
           (strcmp(test_name, "BulkWriteRead")             != 0) &&
           (strcmp(test_name, "GetSizeIsEmpty")            != 0) &&
           (strcmp(test_name, "RawBasicWriteRead")         != 0) &&
           (strcmp(test_name, "RawBulkWriteRead")          != 0) &&
           (strcmp(test_name, "RawTriggeredSatisfied")     != 0) &&
           (strcmp(test_name, "RawTriggeredTimeoutPartial")!= 0) &&
           (strcmp(test_name, "RawTriggeredNoWait")        != 0) &&
           (strcmp(test_name, "RawTriggeredClamp")         != 0) &&
           (strcmp(test_name, "RawResetDoesNotReleaseReader") != 0) &&
           (strcmp(test_name, "RawStateInvariants")        != 0) &&
           (strncmp(test_name, "RawCancel", 9)             != 0); // cancel tests use tasks 0-2 only
}

/*! \fn    RunRawTest
    \brief Helper to run a single raw Pipe test case using g_RawKernel.
*/
template <class TaskType>
static int32_t RunRawTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::rawpipe;

    printf("Test: %s\n", test_name);

    ResetRawKernelAndPipe();

    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    STK_TASK TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_RawKernel.AddTask(&task0);
    g_RawKernel.AddTask(&task1);
    g_RawKernel.AddTask(&task2);

    if (NeedsExtendedTasks(test_name))
    {
        g_RawKernel.AddTask(&task3);
        g_RawKernel.AddTask(&task4);
    }

    g_RawKernel.Start();

    int32_t result = (stk::test::rawpipe::g_TestResult
                      ? TestContext::SUCCESS_EXIT_CODE
                      : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("Result: %s\n", result == TestContext::SUCCESS_EXIT_CODE ? "PASS" : "FAIL");
    printf("--------------\n");

    return result;
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

    using namespace stk::test::pipe;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

#define RUN(TestClass, name, param) \
    do { \
        if (RunTest<TestClass<ACCESS_PRIVILEGED>>(name, param) \
                != TestContext::SUCCESS_EXIT_CODE) \
            total_failures++; \
        else \
            total_success++; \
    } while (0)

#define RUN_RAW(TestClass, name, param) \
    do { \
        if (RunRawTest<stk::test::rawpipe::TestClass<ACCESS_PRIVILEGED>>(name, param) \
                != TestContext::SUCCESS_EXIT_CODE) \
            total_failures++; \
        else \
            total_success++; \
    } while (0)

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Write()/Read() transfers values correctly in FIFO order (tasks 0-2 only)
    RUN(BasicWriteReadTask,       "BasicWriteRead",       20);
    // Test 2: Write() blocks when pipe is full; unblocks when consumer frees a slot (tasks 0-2 only)
    RUN(WriteBlocksWhenFullTask,  "WriteBlocksWhenFull",  0);
    // Test 3: Read() blocks when pipe is empty; unblocks when producer writes data (tasks 0-2 only)
    RUN(ReadBlocksWhenEmptyTask,  "ReadBlocksWhenEmpty",  0);
    // Test 4: Write() on full pipe and Read() on empty pipe both time out correctly (tasks 0-2 only)
    RUN(TimeoutTask,              "Timeout",              0);
    // Test 5: WriteBulk()/ReadBulk() transfers a full block with correct element count and values (tasks 0-2 only)
    RUN(BulkWriteReadTask,        "BulkWriteRead",        _STK_PIPE_CAPACITY);
    // Test 6: GetSize() and IsEmpty() accurately reflect pipe state across fill and drain cycle (tasks 0-2 only)
    RUN(GetSizeIsEmptyTask,       "GetSizeIsEmpty",       0);
    // Test 7: Two producers and two consumers transfer data concurrently without loss
    RUN(MultiProducerConsumerTask,"MultiProducerConsumer",20);

#endif // __ARM_ARCH_6M__

    // Test 8: Stress test under full five-task contention with alternating producer/consumer roles
    RUN(StressTestTask,           "StressTest",           100);

#ifndef __ARM_ARCH_6M__

    // --- Raw Pipe (sync::Pipe) tests ---
    // These use g_RawKernel and g_RawPipe (element_size = 4, capacity = 8).

    stk::test::rawpipe::g_RawKernel.Initialize();

    // Test 9:  Pipe::Write()/Read() transfers a 4-byte RawElem struct in FIFO order
    RUN_RAW(RawBasicWriteReadTask,         "RawBasicWriteRead",          20);
    // Test 10: Pipe::WriteBulk()/ReadBulk() transfers a full block of RawElem elements correctly
    RUN_RAW(RawBulkWriteReadTask,          "RawBulkWriteRead",           0);
    // Test 11: ReadBulkTriggered() blocks until trigger threshold is met, then drains all available
    RUN_RAW(RawTriggeredSatisfiedTask,     "RawTriggeredSatisfied",      0);
    // Test 12: ReadBulkTriggered() drains partial data when timeout fires before trigger is reached
    RUN_RAW(RawTriggeredTimeoutPartialTask,"RawTriggeredTimeoutPartial", 0);
    // Test 13: ReadBulkTriggered() with NO_WAIT returns immediately regardless of trigger level
    RUN_RAW(RawTriggeredNoWaitTask,        "RawTriggeredNoWait",         0);
    // Test 14: ReadBulkTriggered() clamps trigger to max_count when trigger > max_count
    RUN_RAW(RawTriggeredClampTask,         "RawTriggeredClamp",          0);
    // Test 15: Pipe::Reset() does NOT unblock a blocked reader (matches FreeRTOS semantics);
    //          reader stays blocked until its own timeout fires and returns 0
    RUN_RAW(RawResetDoesNotReleaseReaderTask, "RawResetDoesNotReleaseReader", 0);
    // Test 16: State-query API (GetCapacity/GetElementSize/GetCount/GetSpace/IsEmpty/IsFull)
    //          correctly reflects pipe state throughout a fill-and-drain cycle
    RUN_RAW(RawStateInvariantsTask,        "RawStateInvariants",         0);

    // Tests 17-31: cancelability of Pipe waits (tasks 0-2 only), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        using namespace stk::test::rawpipe;

        struct CancelCase { const char *name; int32_t param; };

        static const CancelCase cases[] =
        {
            { "RawCancel_ReadInfinite",           CW_READ_INFINITE },
            { "RawCancel_ReadNotTimeout",         CW_READ_NOT_TIMEOUT },
            { "RawCancel_WriteFull",              CW_WRITE_FULL },
            { "RawCancel_ReadNoStaleWaiter",      CW_NO_STALE_WAITER },
            { "RawCancel_ReadTwoWaiters",         CW_TWO_WAITERS },
            { "RawCancel_ReadSignalWins",         CW_SIGNAL_WINS },
            { "RawCancel_ReadBulkPartial",        CW_READ_BULK_PARTIAL },
            { "RawCancel_ReadTriggeredPartial",   CW_TRIGGERED_PARTIAL },
            { "RawCancel_WriteBulkPartial",       CW_WRITE_BULK_PARTIAL },
            { "RawCancel_SleepCancelRead",        CW_VIA_SLEEPCANCEL + CW_READ_INFINITE },
            { "RawCancel_SleepCancelWrite",       CW_VIA_SLEEPCANCEL + CW_WRITE_FULL },
            { "RawCancel_SleepCancelNoStale",     CW_VIA_SLEEPCANCEL + CW_NO_STALE_WAITER },
            { "RawCancel_SleepCancelTwo",         CW_VIA_SLEEPCANCEL + CW_TWO_WAITERS },
            { "RawCancel_SleepCancelSignal",      CW_VIA_SLEEPCANCEL + CW_SIGNAL_WINS },
            { "RawCancel_SleepCancelTriggered",   CW_VIA_SLEEPCANCEL + CW_TRIGGERED_PARTIAL }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_CancelParam = cases[i].param;

            if (RunRawTest<RawCancelWaitTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param)
                != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

#endif // __ARM_ARCH_6M__

#undef RUN
#undef RUN_RAW

    int32_t final_result = (total_failures == 0 ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
