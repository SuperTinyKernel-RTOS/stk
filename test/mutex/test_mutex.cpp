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
#define _STK_MUTEX_TIMED_LOCK_TIMEOUT 50 // ms, timeout used by TimedLockTask
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
static volatile int32_t g_ExpectedCounter = 0;
static volatile TId     g_Task1Tid = TID_NONE;
static volatile bool    g_Task1Acquired = false;
static ITask *volatile  g_WaiterTask = nullptr;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_MUTEX_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test mutex
static sync::Mutex g_TestMutex;

static inline TId CurrentTid()
{
    return IKernelService::GetInstance()->GetTid();
}

// Task completion counter: increment is a read-modify-write on a volatile, guard it against preemption
// so that no update is lost (a lost update would make the verifier task wait forever)
static inline void MarkDone()
{
    const sync::ScopedCriticalSection cs_;
    g_InstancesDone = g_InstancesDone + 1;
}

// Mutex is free: no owner and zero recursion depth
static inline bool IsMutexFree()
{
    return (g_TestMutex.GetOwner() == TID_NONE) && (g_TestMutex.GetRecursionCount() == 0U);
}

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

        MarkDone();

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

        MarkDone();

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

            int64_t start = GetTimeNowMs();
            bool acquired = g_TestMutex.TryLock();
            int64_t elapsed = GetTimeNowMs() - start;

            bool failed_while_held = (!acquired && (elapsed < _STK_MUTEX_TEST_SHORT_SLEEP));

            if (acquired)
                g_TestMutex.Unlock();

            // After task 0 releases the lock, TryLock() must succeed and make the caller the owner
            stk::Sleep(_STK_MUTEX_TEST_LONG_SLEEP * 2);

            bool ok_when_free = g_TestMutex.TryLock();

            // Recursive re-entry by the owner must always succeed and increase the depth
            bool ok_recursive = false;

            if (ok_when_free)
            {
                ok_when_free = (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);

                ok_recursive = g_TestMutex.TryLock() && (g_TestMutex.GetRecursionCount() == 2U);
                if (ok_recursive)
                    g_TestMutex.Unlock();

                g_TestMutex.Unlock();
            }

            g_TestResult = (failed_while_held && ok_when_free && ok_recursive && IsMutexFree()) ? 1 : 0;
        }
        // Tasks 2+ are not used by this test

        MarkDone();
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

            int64_t start = GetTimeNowMs();
            bool acquired = g_TestMutex.TimedLock(_STK_MUTEX_TIMED_LOCK_TIMEOUT);
            int64_t elapsed = GetTimeNowMs() - start;

            // Must not return before the timeout expires (-1 ms tolerates truncation of the two millisecond
            // timestamps) and must return reasonably soon after it (upper bound is generous to tolerate
            // tick alignment and scheduling jitter)
            if (!acquired && (elapsed >= (_STK_MUTEX_TIMED_LOCK_TIMEOUT - 1)) &&
                (elapsed <= (_STK_MUTEX_TIMED_LOCK_TIMEOUT + 25)))
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

        MarkDone();

        // Final check
        if (m_task_id == 2)
        {
            stk::Sleep(_STK_MUTEX_TEST_LONG_SLEEP);

            if ((g_SharedCounter == 2) && IsMutexFree())
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

        MarkDone();

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
        int32_t successes = 0;

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            // Mix of operations
            if (i % 3 == 0)
            {
                // Regular lock
                g_TestMutex.Lock();
                g_SharedCounter++;
                ++successes;
                g_TestMutex.Unlock();
            }
            else
            if (i % 3 == 1)
            {
                // Try lock
                if (g_TestMutex.TryLock())
                {
                    g_SharedCounter++;
                ++successes;
                    g_TestMutex.Unlock();
                }
            }
            else
            {
                // Timed lock
                if (g_TestMutex.TimedLock(10))
                {
                    g_SharedCounter++;
                ++successes;
                    g_TestMutex.Unlock();
                }
            }

            if ((i % 10) == 0)
                stk::Delay(1);
        }

        // publish local success count
        g_TestMutex.Lock();
        g_ExpectedCounter += successes;
        g_TestMutex.Unlock();

        MarkDone();

        // Last task verifies total
        if (m_task_id == (_STK_MUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_MUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            printf("Stress test: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)g_ExpectedCounter);

            // Every successful acquisition must be accounted for exactly (no lost updates)
            if ((g_SharedCounter > 0) && (g_SharedCounter == g_ExpectedCounter) && IsMutexFree())
                g_TestResult = 1;
        }
    }
};

/*! \class RecursiveDepthTask
    \brief Tests deep recursive locking.
    \note  Verifies mutex handles multiple recursion levels correctly.
*/
template <EAccessMode _AccessMode>
class RecursiveDepthTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    enum { DEPTH = 3 };

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
        // Recursive lock to depth DEPTH
        RecursiveLock(DEPTH);

        MarkDone();

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

        MarkDone();

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

/*! \class OwnerStateTask
    \brief Tests GetOwner() and GetRecursionCount() state reporting.
    \note  Free -> owned by task 0 at depth 2 (visible to task 1) -> depth 1 -> free again.
*/
template <EAccessMode _AccessMode>
class OwnerStateTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    OwnerStateTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // initially free
            bool ok = IsMutexFree();

            g_TestMutex.Lock();
            ok = ok && (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);

            g_TestMutex.Lock();
            ok = ok && (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 2U);

            stk::Sleep(50); // let task 1 observe the locked state

            // nested unlock: still owned
            g_TestMutex.Unlock();
            ok = ok && (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);

            g_TestMutex.Unlock();
            ok = ok && IsMutexFree();

            if (ok)
                g_SharedCounter++;
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            // observed from a non-owner
            bool ok = (g_TestMutex.GetOwner() != TID_NONE) &&
                      (g_TestMutex.GetOwner() != CurrentTid()) &&
                      (g_TestMutex.GetRecursionCount() == 2U);

            stk::Sleep(_STK_MUTEX_TEST_LONG_SLEEP);

            ok = ok && IsMutexFree();

            if (ok)
                g_SharedCounter++;

            // both observers agree
            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }

        MarkDone();
    }
};

/*! \class OwnershipHandoffTask
    \brief Tests direct ownership transfer to the first waiter on Unlock().
    \note  After task 0 unlocks, the mutex must already belong to the blocked task 1 with a recursion depth of 1,
           so that task 0 cannot re-acquire it (no barging).
*/
template <EAccessMode _AccessMode>
class OwnershipHandoffTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    OwnershipHandoffTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            g_TestMutex.Lock();
            stk::Sleep(50); // task 1 blocks in the meantime
            g_TestMutex.Unlock();

            // ownership must have been passed to task 1 already, with a fresh depth of 1
            bool handed_off = (g_TestMutex.GetOwner() == g_Task1Tid) &&
                              (g_TestMutex.GetRecursionCount() == 1U);

            // if ownership was not handed off (barging), TryLock() succeeds: release it so that
            // a failure of this test does not leave the mutex locked for the following tests
            if (g_TestMutex.TryLock())
            {
                g_TestMutex.Unlock();
                handed_off = false;
            }

            if (handed_off)
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            g_Task1Tid = CurrentTid();
            g_TestMutex.Lock(); // blocks until task 0 releases

            // keep the lock long enough for task 0 to run its checks
            stk::Sleep(50);

            bool owner_ok = (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);

            g_TestMutex.Unlock();

            if (owner_ok && (g_SharedCounter == 1) && IsMutexFree())
                g_TestResult = 1;
        }

        MarkDone();
    }
};

/*! \class CancelledWaitTask
    \brief Tests that a wait cancelled via IKernel::CancelTaskWait() fails cleanly.
    \note  The owner holds the lock recursively (depth 2). The cancelled waiter must get false, must not own the
           mutex, must not disturb the owner's recursion depth and must not be left in the wait list (otherwise
           the final Unlock() would hand the ownership to a task that is not waiting).
*/
template <EAccessMode _AccessMode>
class CancelledWaitTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    CancelledWaitTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            g_TestMutex.Lock();
            g_TestMutex.Lock();
            stk::Sleep(50); // task 1 blocks in the meantime

            // interrupt the wait of task 1
            ITask *const waiter = g_WaiterTask;
            if (waiter != nullptr)
                g_Kernel.CancelTaskWait(waiter);

            stk::Sleep(20); // let task 1 resume

            // cancelled waiter must not have received the ownership, depth unchanged
            bool ok = (waiter != nullptr) &&
                      (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 2U);

            g_TestMutex.Unlock();
            ok = ok && (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);

            g_TestMutex.Unlock();

            // and must not be left in the wait list: nothing to hand over to, the mutex is free now
            ok = ok && IsMutexFree();

            if (ok)
                g_SharedCounter++;
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            g_WaiterTask = static_cast<ITask *>(this);

            // blocks until cancelled by task 0 (the lock is released only 20 ms later)
            bool acquired = g_TestMutex.TimedLock(WAIT_INFINITE);

            bool ok = !acquired && (g_TestMutex.GetOwner() != CurrentTid());

            // wait for task 0 to release the lock
            stk::Sleep(_STK_MUTEX_TEST_LONG_SLEEP);

            // mutex must be fully usable again
            bool free_again = g_TestMutex.TryLock();
            if (free_again)
            {
                free_again = (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);
                g_TestMutex.Unlock();
            }

            if (ok && free_again)
                g_SharedCounter++;

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }

        MarkDone();
    }
};

/*! \class MiddleTimeoutTask
    \brief Tests a waiter timing out in the middle of the wait list.
    \note  Task 1 and task 3 wait without timeout, task 2 waits between them with a short timeout and gives up.
           The remaining waiters must still receive the lock in FIFO order (1, then 3), skipping task 2.
*/
template <EAccessMode _AccessMode>
class MiddleTimeoutTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    MiddleTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    static void Record(uint8_t task_id)
    {
        // called with g_TestMutex held
        int32_t idx = g_OrderIndex++;
        g_AcquisitionOrder[idx] = task_id;
    }

    void Run()
    {
        if (m_task_id == 0)
        {
            g_TestMutex.Lock();
            stk::Sleep(100); // all waiters queue up (and task 2 times out) in the meantime
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);        // 1st in the wait list
            g_TestMutex.Lock();
            Record(m_task_id);
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP * 2);    // 2nd in the wait list

            if (g_TestMutex.TimedLock(30))
            {
                Record(m_task_id); // not expected: lock is held by task 0 for much longer
                g_TestMutex.Unlock();
            }
            else
            {
                g_SharedCounter++; // timed out as expected
            }
        }
        else
        if (m_task_id == 3)
        {
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP * 3);    // 3rd in the wait list
            g_TestMutex.Lock();
            Record(m_task_id);
            g_TestMutex.Unlock();
        }

        MarkDone();

        // Task 4 only verifies
        if (m_task_id == 4)
        {
            while (g_InstancesDone < _STK_MUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            printf("Middle timeout: acquired %d tasks, order %d,%d, timed out %d\n",
                (int)g_OrderIndex, (int)g_AcquisitionOrder[0], (int)g_AcquisitionOrder[1], (int)g_SharedCounter);

            if ((g_OrderIndex == 2) &&
                (g_AcquisitionOrder[0] == 1) &&
                (g_AcquisitionOrder[1] == 3) &&
                (g_SharedCounter == 1) &&
                IsMutexFree())
            {
                g_TestResult = 1;
            }
        }
    }
};

/*! \class RecursiveContendedTask
    \brief Tests that a nested Unlock() does not release the mutex to a waiter.
    \note  Task 0 holds the lock at depth 2 while task 1 is blocked. After the first Unlock() the mutex must still
           belong to task 0 (task 1 must not run); only after the second Unlock() it is handed over to task 1,
           with a fresh depth of 1.
*/
template <EAccessMode _AccessMode>
class RecursiveContendedTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RecursiveContendedTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            g_TestMutex.Lock();
            g_TestMutex.Lock();
            stk::Sleep(50); // task 1 blocks in the meantime

            // nested unlock: depth drops to 1, the mutex must stay with task 0
            g_TestMutex.Unlock();

            stk::Sleep(20); // task 1 would run now if the mutex was released prematurely

            bool partial_ok = !g_Task1Acquired &&
                              (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);

            // final unlock: ownership passes to task 1 with depth 1
            g_TestMutex.Unlock();

            bool handoff_ok = (g_TestMutex.GetOwner() == g_Task1Tid) && (g_TestMutex.GetRecursionCount() == 1U);

            if (partial_ok && handoff_ok)
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MUTEX_TEST_SHORT_SLEEP);

            g_Task1Tid = CurrentTid();
            g_TestMutex.Lock(); // blocks until task 0 fully releases
            g_Task1Acquired = true;

            // keep the lock long enough for task 0 to run its checks
            stk::Sleep(30);

            bool owner_ok = (g_TestMutex.GetOwner() == CurrentTid()) && (g_TestMutex.GetRecursionCount() == 1U);

            g_TestMutex.Unlock();

            if (owner_ok && (g_SharedCounter == 1) && IsMutexFree())
                g_TestResult = 1;
        }

        MarkDone();
    }
};

/*! \class RecursionMaxTask
    \brief Tests the maximum recursion depth (sync::Mutex::RECURSION_MAX).
    \note  Locks exactly RECURSION_MAX times (one more would be a contract violation) and unlocks back to free.
*/
template <EAccessMode _AccessMode>
class RecursionMaxTask : public Task<_STK_MUTEX_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RecursionMaxTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            for (uint32_t i = 0; i < sync::Mutex::RECURSION_MAX; ++i)
                g_TestMutex.Lock();

            bool ok = (g_TestMutex.GetOwner() == CurrentTid()) &&
                      (g_TestMutex.GetRecursionCount() == sync::Mutex::RECURSION_MAX);

            for (uint32_t i = 0; i < sync::Mutex::RECURSION_MAX; ++i)
                g_TestMutex.Unlock();

            ok = ok && IsMutexFree();

            if (ok)
                g_TestResult = 1;
        }

        MarkDone();
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
    g_ExpectedCounter = 0;
    g_Task1Tid = TID_NONE;
    g_Task1Acquired = false;
    g_WaiterTask = nullptr;

    for (int32_t i = 0; i < _STK_MUTEX_TEST_TASKS_MAX; ++i)
        g_AcquisitionOrder[i] = 0;
}

} // namespace mutex
} // namespace test
} // namespace stk

/*! \fn    IsTwoTaskTest
    \brief Returns true if the test uses only tasks 0-1.
*/
static bool IsTwoTaskTest(const char *test_name)
{
    return  (strcmp(test_name, "TryLock")            == 0) ||
            (strcmp(test_name, "OwnerState")         == 0) ||
            (strcmp(test_name, "OwnershipHandoff")   == 0) ||
            (strcmp(test_name, "CancelledWait")      == 0) ||
            (strcmp(test_name, "RecursiveContended") == 0) ||
            (strcmp(test_name, "RecursionMax")       == 0);
}

/*! \fn    NeedsExtendedTasks
    \brief Returns true if the test requires tasks 3 and 4.
    \note  Two-task tests use only tasks 0-1; TimedLock uses only tasks 0-2.
*/
static bool NeedsExtendedTasks(const char *test_name)
{
    return  !IsTwoTaskTest(test_name) &&
            (strcmp(test_name, "TimedLock") != 0);
}

/*! \fn    NeedsThreeTasks
    \brief Returns true if the test requires at least 3 tasks (0-2).
    \note  Two-task tests use only tasks 0-1, so task 2 is also unnecessary.
*/
static bool NeedsThreeTasks(const char *test_name)
{
    return !IsTwoTaskTest(test_name);
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

    // a previous test must not leave the mutex locked: it can not be reset from outside, so report
    // this clearly instead of producing misleading failures in this test
    if (!IsMutexFree())
    {
        printf("Result: FAIL (mutex was left locked by a previous test)\n");
        printf("--------------\n");
        return TestContext::DEFAULT_FAILURE_EXIT_CODE;
    }

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

    if (!IsMutexFree())
    {
        printf("Mutex left locked at test end\n");
        result = TestContext::DEFAULT_FAILURE_EXIT_CODE;
    }

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

    // Test 8: Owner / recursion depth state reporting
    if (RunTest<OwnerStateTask<ACCESS_PRIVILEGED>>("OwnerState") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 9: Direct ownership handoff to the first waiter
    if (RunTest<OwnershipHandoffTask<ACCESS_PRIVILEGED>>("OwnershipHandoff") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 10: Cancelled wait (IKernel::CancelTaskWait)
    if (RunTest<CancelledWaitTask<ACCESS_PRIVILEGED>>("CancelledWait") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 11: Waiter timing out in the middle of the wait list
    if (RunTest<MiddleTimeoutTask<ACCESS_PRIVILEGED>>("MiddleTimeout") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 12: Nested Unlock does not release to a waiter
    if (RunTest<RecursiveContendedTask<ACCESS_PRIVILEGED>>("RecursiveContended") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 13: Maximum recursion depth
    if (RunTest<RecursionMaxTask<ACCESS_PRIVILEGED>>("RecursionMax") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 14: Stress test
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
