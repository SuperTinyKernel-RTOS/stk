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
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_RWMUTEX_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test RWMutex
static sync::RWMutex g_TestRWMutex;

// Cancellation test state (see CancelWaitTask)
static ITask * volatile g_Victim                 = nullptr;      // task whose wait gets cancelled (task 1)
static volatile TId     g_VictimTid              = TID_NONE;     // same, as TId (for SleepCancel)
static volatile int32_t g_VictimResult[2]        = {-100, -100}; // EWaitResult of task 1 / task 3 (-100 = not set)
static volatile int64_t g_VictimElapsed[2]       = {0, 0};       // ms spent acquiring the lock by task 1 / task 3

// Scenario selector of CancelWaitTask. It is a global, not a constructor argument, because RunTest() declares
// tasks 0-2 as 'static': a static local is constructed only once per template instantiation (on the first
// RunTest<CancelWaitTask> call), so a constructor argument would silently keep the value of the first case.
// Set by main() before each RunTest() call.
static volatile int32_t g_CancelParam            = 0;

// Cancel the wait of g_Victim either via IKernel::CancelTaskWait() or via stk::SleepCancel()
static void CancelVictim(bool use_sleep_cancel)
{
    if (use_sleep_cancel)
        stk::SleepCancel(g_VictimTid);
    else
        g_Kernel.CancelTaskWait(g_Victim);
}

// Checks that the lock ended up completely free and with consistent internal counters: a stale waiting
// writer (m_writers_waiting) would make TryReadLock() fail (writer preference), a stale reader (m_readers)
// or a stale active writer would make TryLock() fail
static bool IsLockPristine()
{
    const bool read_ok = g_TestRWMutex.TryReadLock();
    if (read_ok)
        g_TestRWMutex.ReadUnlock();

    const bool write_ok = g_TestRWMutex.TryLock();
    if (write_ok)
        g_TestRWMutex.Unlock();

    return read_ok && write_ok;
}

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

            int64_t start = GetTimeNowMs();
            g_TestRWMutex.Lock();
            int64_t elapsed = GetTimeNowMs() - start;

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
            int64_t start = GetTimeNowMs();
            bool acquired = g_TestRWMutex.TimedReadLock(50);
            int64_t elapsed = GetTimeNowMs() - start;

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
            int64_t start = GetTimeNowMs();
            bool acquired = g_TestRWMutex.TimedLock(50);
            int64_t elapsed = GetTimeNowMs() - start;

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
            int64_t start = GetTimeNowMs();
            bool acquired = g_TestRWMutex.TryReadLock();
            int64_t elapsed = GetTimeNowMs() - start;

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

            int64_t start = GetTimeNowMs();
            g_TestRWMutex.Lock();
            int64_t elapsed = GetTimeNowMs() - start;

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

static const int32_t CW_WRITER_INFINITE      = 1;
static const int32_t CW_WRITER_NOT_TIMEOUT   = 2;
static const int32_t CW_READER_INFINITE      = 3;
static const int32_t CW_READER_BEHIND_WRITER = 4;
static const int32_t CW_PREFERENCE_RELEASED  = 5;
static const int32_t CW_TWO_WRITERS          = 6;
static const int32_t CW_SIGNAL_WINS          = 7;
static const int32_t CW_LOCK_ABSORBS         = 8;
static const int32_t CW_READLOCK_ABSORBS     = 9;
static const int32_t CW_VIA_SLEEPCANCEL      = 100;

/*! \class CancelWaitTask
    \brief Tests cancelability of RWMutex acquisition (IKernel::CancelTaskWait() and stk::SleepCancel()).
    \note  Task 0 is the canceller/checker, task 1 is the victim (blocked in TimedLockEx() or
           TimedReadLockEx()), task 2 holds the lock from tick 0 until about tick 150, task 3 is a second
           waiter used by some scenarios, task 4 is not used. The scenario is selected by g_CancelParam:
           1 = cancel an infinite write-lock wait, 2 = cancel is not reported as a timeout,
           3 = cancel an infinite read-lock wait, 4 = cancelled reader queued behind a waiting writer
           leaves that writer intact, 5 = cancelling the only waiting writer releases the readers held
           back by writer preference, 6 = cancelling one of two waiting writers leaves the other intact,
           7 = a release that already woke the waiter wins over a late cancel, 8 = void Lock() absorbs a
           cancel, 9 = void ReadLock() absorbs a cancel. Adding 100 to g_CancelParam cancels via
           stk::SleepCancel() instead of IKernel::CancelTaskWait().
           Unless the scenario says otherwise the cancel is issued at about tick 60 and the checks run at
           about tick 80 (holder still owns the lock) and tick 200 (holder released, everything settled).
*/
template <EAccessMode _AccessMode>
class CancelWaitTask : public Task<_STK_RWMUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_scenario;
    bool    m_use_sleep_cancel;

public:
    CancelWaitTask(uint8_t task_id, int32_t) // scenario is taken from g_CancelParam in Run(), see g_CancelParam
        : m_task_id(task_id), m_scenario(0), m_use_sleep_cancel(false)
    {}

private:
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
        if (m_task_id == 2)
            RunHolder();
        else
        if (m_task_id == 3)
            RunSecondWaiter();
        // task 4 is not used by any scenario
    }

    // True if the victim waits for the read lock, false if it waits for the write lock
    bool VictimIsReader() const
    {
        return (m_scenario == CW_READER_INFINITE) || (m_scenario == CW_READER_BEHIND_WRITER) ||
               (m_scenario == CW_READLOCK_ABSORBS);
    }

    // True if the holder owns the write lock, false if it owns a read lock
    bool HolderIsWriter() const
    {
        return (m_scenario == CW_READER_INFINITE) || (m_scenario == CW_READLOCK_ABSORBS);
    }

    // Task 1: block on the lock, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        const bool    reader  = VictimIsReader();
        const Timeout timeout = (m_scenario == CW_WRITER_NOT_TIMEOUT ? 300 : WAIT_INFINITE);

        EWaitResult r;

        const int64_t start = GetTimeNowMs();

        if (m_scenario == CW_LOCK_ABSORBS)
        {
            g_TestRWMutex.Lock(); // not cancelable: returns only with the lock held
            r = WAIT_RESULT_SIGNAL;
        }
        else
        if (m_scenario == CW_READLOCK_ABSORBS)
        {
            g_TestRWMutex.ReadLock(); // not cancelable: returns only with the lock held
            r = WAIT_RESULT_SIGNAL;
        }
        else
        if (reader)
            r = g_TestRWMutex.TimedReadLockEx(timeout);
        else
            r = g_TestRWMutex.TimedLockEx(timeout);

        g_VictimElapsed[0] = GetTimeNowMs() - start;
        g_VictimResult[0]  = static_cast<int32_t>(r);

        // a failed attempt must not have taken the lock, release it only if it was acquired
        if (r == WAIT_RESULT_SIGNAL)
        {
            if (reader)
                g_TestRWMutex.ReadUnlock();
            else
                g_TestRWMutex.Unlock();
        }
    }

    // Task 2: lock holder, owns the lock from the start until about tick 150
    void RunHolder()
    {
        if (m_scenario == CW_SIGNAL_WINS)
            return; // the canceller holds the lock itself in this scenario

        if (HolderIsWriter())
        {
            g_TestRWMutex.Lock();
            stk::Sleep(150);
            g_TestRWMutex.Unlock();
        }
        else
        {
            g_TestRWMutex.ReadLock();
            stk::Sleep(150);
            g_TestRWMutex.ReadUnlock();
        }
    }

    // Task 3: second waiter, queued before (scenario 4) or behind (scenarios 5, 6) the victim
    void RunSecondWaiter()
    {
        if (m_scenario == CW_READER_BEHIND_WRITER)
        {
            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP / 2); // queue up as a writer before the victim reader
            LockSecond(false);
        }
        else
        if (m_scenario == CW_PREFERENCE_RELEASED)
        {
            stk::Sleep(2 * _STK_RWMUTEX_TEST_SHORT_SLEEP); // a reader blocked only by the victim's writer preference
            LockSecond(true);
        }
        else
        if (m_scenario == CW_TWO_WRITERS)
        {
            stk::Sleep(2 * _STK_RWMUTEX_TEST_SHORT_SLEEP); // a second writer queued behind the victim
            LockSecond(false);
        }
    }

    void LockSecond(bool reader)
    {
        const int64_t start = GetTimeNowMs();
        const EWaitResult r = (reader ? g_TestRWMutex.TimedReadLockEx(WAIT_INFINITE)
                                      : g_TestRWMutex.TimedLockEx(WAIT_INFINITE));

        g_VictimElapsed[1] = GetTimeNowMs() - start;
        g_VictimResult[1]  = static_cast<int32_t>(r);

        if (r == WAIT_RESULT_SIGNAL)
        {
            if (reader)
                g_TestRWMutex.ReadUnlock();
            else
                g_TestRWMutex.Unlock();
        }
    }

    // Task 0: cancel and verify
    void RunCanceller()
    {
        bool pass = false;

        if (m_scenario == CW_SIGNAL_WINS)
            g_TestRWMutex.ReadLock(); // the canceller is the lock holder in this scenario

        stk::Sleep(60); // the victim (and the second waiter) are blocked by now, the holder releases at ~150

        if (m_scenario == CW_SIGNAL_WINS)
        {
            // release and cancel back to back, no scheduling point in between: the last ReadUnlock() has
            // already woken the victim writer (a notified waiter is unlinked from the wait list and is
            // granted the lock), so the cancel must be a no-op
            g_TestRWMutex.ReadUnlock();
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP * 2);

            pass = (g_VictimResult[0] == WAIT_RESULT_SIGNAL) && IsLockPristine();
        }
        else
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP * 2); // tick ~80: cancel delivered, holder still owns the lock

            const int32_t victim_early = g_VictimResult[0];
            const int32_t second_early = g_VictimResult[1];

            stk::Sleep(120); // tick ~200: holder released at ~150, everything has settled

            const bool pristine = IsLockPristine();

            if ((m_scenario == CW_WRITER_INFINITE) || (m_scenario == CW_READER_INFINITE))
            {
                // started waiting at ~10 ms, cancelled at ~60 ms
                pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                       (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) && pristine;
            }
            else
            if (m_scenario == CW_WRITER_NOT_TIMEOUT)
            {
                // 300 ms timeout is far away: the wait must end early and as CANCELED, not TIMEOUT
                pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimElapsed[0] < 150) && pristine;
            }
            else
            if (m_scenario == CW_READER_BEHIND_WRITER)
            {
                // the cancelled reader must not have been counted as a reader: the writer queued before it
                // still gets the lock as soon as the holder releases (never, if a reader had leaked)
                pass = (victim_early == WAIT_RESULT_CANCELED) &&
                       (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                       (second_early == -100) &&                      // writer still blocked by the holder
                       (g_VictimResult[1] == WAIT_RESULT_SIGNAL) &&   // and granted the lock after its release
                       (g_VictimElapsed[1] >= 100) && pristine;
            }
            else
            if (m_scenario == CW_PREFERENCE_RELEASED)
            {
                // the reader (queued at ~20 ms) was blocked only by the victim's writer preference: once the
                // writer withdraws it must get the lock at once, while the holder (until ~150) still reads
                pass = (victim_early == WAIT_RESULT_CANCELED) &&
                       (second_early == WAIT_RESULT_SIGNAL) &&
                       (g_VictimElapsed[1] >= 25) && (g_VictimElapsed[1] <= 100) && pristine;
            }
            else
            if (m_scenario == CW_TWO_WRITERS)
            {
                // the second writer must be unaffected by the first one's withdrawal and get the lock when
                // the holder releases
                pass = (victim_early == WAIT_RESULT_CANCELED) &&
                       (second_early == -100) &&
                       (g_VictimResult[1] == WAIT_RESULT_SIGNAL) &&
                       (g_VictimElapsed[1] >= 100) && pristine;
            }
            else
            if ((m_scenario == CW_LOCK_ABSORBS) || (m_scenario == CW_READLOCK_ABSORBS))
            {
                // void Lock()/ReadLock() cannot report a cancel: it is absorbed and the wait retried, so the
                // victim stays blocked until the holder releases at ~150 and only then returns with the lock
                pass = (victim_early == -100) &&
                       (g_VictimResult[0] == WAIT_RESULT_SIGNAL) &&
                       (g_VictimElapsed[0] >= 110) && pristine;
            }

            printf("cancel wait (scenario=%d, via %s): victim=%d/%d ms second=%d/%d ms pristine=%d\n",
                (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
                (int)g_VictimResult[0], (int)g_VictimElapsed[0],
                (int)g_VictimResult[1], (int)g_VictimElapsed[1], (int)pristine);
        }

        // Safety net: if the cancel left a waiter blocked forever (e.g. readers never woken after the
        // withdrawal of a waiting writer) the kernel would never stop. Taking and dropping the write lock
        // makes Unlock() wake all waiting readers (or the next writer), so that the test fails instead of hanging.
        if (!pass)
        {
            for (int32_t i = 0; i < 2; ++i)
            {
                if (g_TestRWMutex.TimedLock(_STK_RWMUTEX_TEST_LONG_SLEEP))
                    g_TestRWMutex.Unlock();

                stk::Sleep(_STK_RWMUTEX_TEST_SHORT_SLEEP);
            }
        }

        if (pass)
            g_TestResult = 1;
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

    g_Victim    = nullptr;
    g_VictimTid = TID_NONE;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]  = -100;
        g_VictimElapsed[i] = 0;
    }
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

    // Tests 11-24: cancelability of RWMutex acquisition (tasks 0-3 used), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        struct CancelCase { const char *name; int32_t param; };

        static const CancelCase cases[] =
        {
            { "CancelWait_WriterInfinite",          CW_WRITER_INFINITE                          },
            { "CancelWait_WriterNotTimeout",        CW_WRITER_NOT_TIMEOUT                       },
            { "CancelWait_ReaderInfinite",          CW_READER_INFINITE                          },
            { "CancelWait_ReaderBehindWriter",      CW_READER_BEHIND_WRITER                     },
            { "CancelWait_PreferenceReleased",      CW_PREFERENCE_RELEASED                      },
            { "CancelWait_TwoWriters",              CW_TWO_WRITERS                              },
            { "CancelWait_SignalWins",              CW_SIGNAL_WINS                              },
            { "CancelWait_LockAbsorbs",             CW_LOCK_ABSORBS                             },
            { "CancelWait_ReadLockAbsorbs",         CW_READLOCK_ABSORBS                         },
            { "CancelWait_SleepCancelWriter",       CW_VIA_SLEEPCANCEL + CW_WRITER_INFINITE     },
            { "CancelWait_SleepCancelReader",       CW_VIA_SLEEPCANCEL + CW_READER_INFINITE     },
            { "CancelWait_SleepCancelPreference",   CW_VIA_SLEEPCANCEL + CW_PREFERENCE_RELEASED },
            { "CancelWait_SleepCancelTwoWriters",   CW_VIA_SLEEPCANCEL + CW_TWO_WRITERS         },
            { "CancelWait_SleepCancelSignal",       CW_VIA_SLEEPCANCEL + CW_SIGNAL_WINS         }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_CancelParam = cases[i].param;

            if (RunTest<CancelWaitTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param)
                != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

#endif // __ARM_ARCH_6M__

    // Test 10 (runs last, after the cancellation tests): Stress test mixing readers and writers under full contention
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
