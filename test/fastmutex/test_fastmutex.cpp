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
#include <sync/stk_sync_fastmutex.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_FMUTEX_TEST_TASKS_MAX   5
#define _STK_FMUTEX_TEST_SHORT_SLEEP 10
#define _STK_FMUTEX_TEST_LONG_SLEEP  100
#define TIMED_LOCK_TIMEOUT           50 // ms, timeout used by TimedLockTask
#ifdef __ARM_ARCH_6M__
#define _STK_FMUTEX_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_FMUTEX_STACK_SIZE       256
#define STK_TASK                     static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::fast_mutex
    \brief     Namespace of FastMutex test.
 */
namespace fast_mutex {

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_ExpectedCounter = 0;
static volatile int32_t g_AcquisitionOrder[_STK_FMUTEX_TEST_TASKS_MAX] = {0};
static volatile int32_t g_OrderIndex = 0;
static volatile int32_t g_InstancesDone = 0;
static volatile TId     g_Task1Tid = TID_NONE;
static ITask *volatile  g_WaiterTask = nullptr;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_FMUTEX_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test mutex
static sync::FastMutex g_TestMutex;

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

/*! \class BasicLockUnlockTask
    \brief Tests basic lock/unlock functionality.
    \note  Verifies that mutex provides mutual exclusion.
*/
template <EAccessMode _AccessMode>
class BasicLockUnlockTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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

        // Task 0 acts as verifier: no increment may be lost or doubled
        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_FMUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            int32_t expected = _STK_FMUTEX_TEST_TASKS_MAX * m_iterations;

            printf("basic lock/unlock: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            if ((g_SharedCounter == expected) && !g_TestMutex.IsLocked())
                g_TestResult = 1;
        }
    }
};

/*! \class TryLockTask
    \brief Tests TryLock() non-blocking behavior.
    \note  TryLock() must fail immediately while the lock is held and succeed once it is released.
*/
template <EAccessMode _AccessMode>
class TryLockTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
            stk::Sleep(_STK_FMUTEX_TEST_LONG_SLEEP);
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: Try to acquire while held - must fail immediately
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP); // Let task 0 acquire first

            int64_t start = GetTimeNowMs();
            bool acquired = g_TestMutex.TryLock();
            int64_t elapsed = GetTimeNowMs() - start;

            bool failed_while_held = (!acquired && (elapsed < _STK_FMUTEX_TEST_SHORT_SLEEP));

            if (acquired)
                g_TestMutex.Unlock();

            // After task 0 releases the lock, TryLock() must succeed
            stk::Sleep(_STK_FMUTEX_TEST_LONG_SLEEP * 2);

            bool ok_when_free = g_TestMutex.TryLock();

            if (ok_when_free)
            {
                ok_when_free = (g_TestMutex.GetOwner() == CurrentTid());
                g_TestMutex.Unlock();
            }

            g_TestResult = (failed_while_held && ok_when_free) ? 1 : 0;
        }

        MarkDone();
    }
};

/*! \class TimedLockTask
    \brief Tests TimedLock() timeout behavior.
    \note  Verifies that TimedLock() respects timeout values and that a timed-out waiter does not corrupt state.
*/
template <EAccessMode _AccessMode>
class TimedLockTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
            stk::Sleep(200);
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: Try to acquire with timeout
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP); // Let task 0 acquire first

            int64_t start = GetTimeNowMs();
            bool acquired = g_TestMutex.TimedLock(TIMED_LOCK_TIMEOUT);
            int64_t elapsed = GetTimeNowMs() - start;

            // Must not return before the timeout expires (-1 ms tolerates truncation of the two
            // millisecond timestamps) and must return reasonably soon after it (upper bound is
            // generous to tolerate tick alignment and scheduling jitter)
            if (!acquired && (elapsed >= (TIMED_LOCK_TIMEOUT - 1)) && (elapsed <= (TIMED_LOCK_TIMEOUT + 25)))
                g_SharedCounter++;

            if (acquired)
                g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            // Task 2: Successfully acquire after task 0 releases
            stk::Sleep(250);

            if (g_TestMutex.TimedLock(100))
            {
                if (g_TestMutex.GetOwner() == CurrentTid())
                    g_SharedCounter++;
                g_TestMutex.Unlock();
            }
        }

        MarkDone();

        // Final check
        if (m_task_id == 2)
        {
            stk::Sleep(_STK_FMUTEX_TEST_LONG_SLEEP);

            if ((g_SharedCounter == 2) && !g_TestMutex.IsLocked())
                g_TestResult = 1;
        }
    }
};

/*! \class FIFOOrderTask
    \brief Tests FIFO ordering of waiting threads.
    \note  Verifies that threads are woken in the order they blocked.
*/
template <EAccessMode _AccessMode>
class FIFOOrderTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP * m_task_id); // Stagger start times

            g_TestMutex.Lock();
            {
                int32_t idx = g_OrderIndex++;
                g_AcquisitionOrder[idx] = m_task_id;
            }
            g_TestMutex.Unlock();
        }

        MarkDone();

        // Last task verifies order
        if (m_task_id == (_STK_FMUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_FMUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            bool ordered = true;
            for (int32_t i = 0; i < (_STK_FMUTEX_TEST_TASKS_MAX - 1); ++i)
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

/*! \class OwnerStateTask
    \brief Tests GetOwner() and IsLocked() state reporting.
    \note  Free -> owned by task 0 (visible to task 1) -> free again.
*/
template <EAccessMode _AccessMode>
class OwnerStateTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
            bool ok = !g_TestMutex.IsLocked() && (g_TestMutex.GetOwner() == TID_NONE);

            g_TestMutex.Lock();

            ok = ok && g_TestMutex.IsLocked() && (g_TestMutex.GetOwner() == CurrentTid());

            stk::Sleep(50); // let task 1 observe the locked state

            g_TestMutex.Unlock();

            ok = ok && !g_TestMutex.IsLocked() && (g_TestMutex.GetOwner() == TID_NONE);

            if (ok)
                g_SharedCounter++;
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            // observed from a non-owner
            bool ok = g_TestMutex.IsLocked() &&
                      (g_TestMutex.GetOwner() != TID_NONE) &&
                      (g_TestMutex.GetOwner() != CurrentTid());

            stk::Sleep(_STK_FMUTEX_TEST_LONG_SLEEP);

            ok = ok && !g_TestMutex.IsLocked();

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
    \note  After task 0 unlocks, the mutex must already belong to the blocked task 1 so that
           task 0 cannot re-acquire it (no barging).
*/
template <EAccessMode _AccessMode>
class OwnershipHandoffTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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

            // ownership must have been passed to task 1 already
            bool handed_off = g_TestMutex.IsLocked() &&
                              (g_TestMutex.GetOwner() == g_Task1Tid);

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
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            g_Task1Tid = CurrentTid();
            g_TestMutex.Lock(); // blocks until task 0 releases

            // keep the lock long enough for task 0 to run its checks
            stk::Sleep(50);

            bool owner_ok = (g_TestMutex.GetOwner() == CurrentTid());

            g_TestMutex.Unlock();

            if (owner_ok && (g_SharedCounter == 1) && !g_TestMutex.IsLocked())
                g_TestResult = 1;
        }

        MarkDone();
    }
};

/*! \class InterTaskCoordinationTask
    \brief Tests mutex for coordinating work between tasks.
    \note  Verifies mutex correctly synchronizes shared state updates.
*/
template <EAccessMode _AccessMode>
class InterTaskCoordinationTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
                while ((g_SharedCounter % _STK_FMUTEX_TEST_TASKS_MAX) != m_task_id)
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
        if (m_task_id == (_STK_FMUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_FMUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            if (g_SharedCounter == 10 * _STK_FMUTEX_TEST_TASKS_MAX)
                g_TestResult = 1;

            printf("Coordination test: counter=%d (expected %d)\n",
                (int)g_SharedCounter, 10 * _STK_FMUTEX_TEST_TASKS_MAX);
        }
    }
};

/*! \class StressTestTask
    \brief Stress test with many lock/unlock cycles.
    \note  Mixes Lock(), TryLock() and TimedLock(). Every successful acquisition is counted locally and the
           sum must match the shared counter exactly (no lost updates).
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
            bool acquired = false;

            if (i % 3 == 0)
            {
                g_TestMutex.Lock();
                acquired = true;
            }
            else
            if (i % 3 == 1)
            {
                acquired = g_TestMutex.TryLock();
            }
            else
            {
                acquired = g_TestMutex.TimedLock(10);
            }

            if (acquired)
            {
                g_SharedCounter++;
                ++successes;
                g_TestMutex.Unlock();
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
        if (m_task_id == (_STK_FMUTEX_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_FMUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            printf("Stress test: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)g_ExpectedCounter);

            if ((g_SharedCounter > 0) && (g_SharedCounter == g_ExpectedCounter) && !g_TestMutex.IsLocked())
                g_TestResult = 1;
        }
    }
};

/*! \class CancelledWaitTask
    \brief Tests that a wait cancelled via IKernel::CancelTaskWait() fails cleanly.
    \note  The cancelled waiter must get false from TimedLock(), must not own the mutex, and must not be
           left in the wait list (otherwise Unlock() would hand the ownership to a task that is not waiting).
*/
template <EAccessMode _AccessMode>
class CancelledWaitTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
            stk::Sleep(50); // task 1 blocks in the meantime

            // interrupt the wait of task 1
            ITask *const waiter = g_WaiterTask;
            if (waiter != nullptr)
                g_Kernel.CancelTaskWait(waiter);

            stk::Sleep(20); // let task 1 resume

            // cancelled waiter must not have received the ownership
            bool ok = (waiter != nullptr) && g_TestMutex.IsLocked() && (g_TestMutex.GetOwner() == CurrentTid());

            g_TestMutex.Unlock();

            // and must not be left in the wait list: nothing to hand over to, the mutex is free now
            ok = ok && !g_TestMutex.IsLocked();

            if (ok)
                g_SharedCounter++;
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            g_WaiterTask = static_cast<ITask *>(this);

            // blocks until cancelled by task 0 (the lock is released only 20 ms later)
            bool acquired = g_TestMutex.TimedLock(WAIT_INFINITE);

            bool ok = !acquired && (g_TestMutex.GetOwner() != CurrentTid());

            // wait for task 0 to release the lock
            stk::Sleep(_STK_FMUTEX_TEST_LONG_SLEEP);

            // mutex must be fully usable again
            bool free_again = g_TestMutex.TryLock();
            if (free_again)
            {
                free_again = (g_TestMutex.GetOwner() == CurrentTid());
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
class MiddleTimeoutTask : public Task<_STK_FMUTEX_STACK_SIZE, _AccessMode>
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
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);        // 1st in the wait list
            g_TestMutex.Lock();
            Record(m_task_id);
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP * 2);    // 2nd in the wait list

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
            stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP * 3);    // 3rd in the wait list
            g_TestMutex.Lock();
            Record(m_task_id);
            g_TestMutex.Unlock();
        }

        MarkDone();

        // Task 4 only verifies
        if (m_task_id == 4)
        {
            while (g_InstancesDone < _STK_FMUTEX_TEST_TASKS_MAX)
                stk::Sleep(_STK_FMUTEX_TEST_SHORT_SLEEP);

            printf("Middle timeout: acquired %d tasks, order %d,%d, timed out %d\n",
                (int)g_OrderIndex, (int)g_AcquisitionOrder[0], (int)g_AcquisitionOrder[1], (int)g_SharedCounter);

            if ((g_OrderIndex == 2) &&
                (g_AcquisitionOrder[0] == 1) &&
                (g_AcquisitionOrder[1] == 3) &&
                (g_SharedCounter == 1) &&
                !g_TestMutex.IsLocked())
            {
                g_TestResult = 1;
            }
        }
    }
};

// Note: locking a FastMutex already owned by the caller triggers STK_KERNEL_PANIC(KERNEL_PANIC_SYNC_DEADLOCK),
// whose handler never returns, so this contract violation can not be exercised by this suite.

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult = 0;
    g_SharedCounter = 0;
    g_ExpectedCounter = 0;
    g_OrderIndex = 0;
    g_InstancesDone = 0;
    g_Task1Tid = TID_NONE;
    g_WaiterTask = nullptr;

    for (int32_t i = 0; i < _STK_FMUTEX_TEST_TASKS_MAX; ++i)
        g_AcquisitionOrder[i] = 0;
}

} // namespace fast_mutex
} // namespace test
} // namespace stk

/*! \fn    RunTest
    \brief Helper function to run a single test case.
    \param[in] test_name: Test name.
    \param[in] tasks_count: Number of tasks (2..5) the test needs.
    \param[in] param: Test specific parameter.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t tasks_count, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::fast_mutex;

    printf("Test: %s\n", test_name);

    // a previous test must not leave the mutex locked: it can not be reset from outside, so report
    // this clearly instead of producing misleading failures in this test
    if (g_TestMutex.IsLocked())
    {
        printf("Result: FAIL (mutex was left locked by a previous test)\n");
        printf("--------------\n");
        return TestContext::DEFAULT_FAILURE_EXIT_CODE;
    }

    ResetTestState();

    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_Kernel.AddTask(&task0);
    g_Kernel.AddTask(&task1);

    if (tasks_count > 2)
        g_Kernel.AddTask(&task2);

    if (tasks_count > 3)
    {
        g_Kernel.AddTask(&task3);
        g_Kernel.AddTask(&task4);
    }

    g_Kernel.Start();

    int32_t result = (g_TestResult ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    if (g_TestMutex.IsLocked())
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

    using namespace stk::test::fast_mutex;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Basic Lock/Unlock with mutual exclusion
    if (RunTest<BasicLockUnlockTask<ACCESS_PRIVILEGED>>("BasicLockUnlock", 5, 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: TryLock non-blocking behavior
    if (RunTest<TryLockTask<ACCESS_PRIVILEGED>>("TryLock", 2) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: TimedLock timeout behavior
    if (RunTest<TimedLockTask<ACCESS_PRIVILEGED>>("TimedLock", 3) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: FIFO ordering
    if (RunTest<FIFOOrderTask<ACCESS_PRIVILEGED>>("FIFOOrder", 5) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Owner / locked state reporting
    if (RunTest<OwnerStateTask<ACCESS_PRIVILEGED>>("OwnerState", 2) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: Direct ownership handoff to the first waiter
    if (RunTest<OwnershipHandoffTask<ACCESS_PRIVILEGED>>("OwnershipHandoff", 2) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Inter-task coordination
    if (RunTest<InterTaskCoordinationTask<ACCESS_PRIVILEGED>>("InterTaskCoordination", 5) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 8: Cancelled wait (IKernel::CancelTaskWait)
    if (RunTest<CancelledWaitTask<ACCESS_PRIVILEGED>>("CancelledWait", 2) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 9: Waiter timing out in the middle of the wait list
    if (RunTest<MiddleTimeoutTask<ACCESS_PRIVILEGED>>("MiddleTimeout", 5) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 10: Stress test
    if (RunTest<StressTestTask<ACCESS_PRIVILEGED>>("StressTest", 5, 400) != TestContext::SUCCESS_EXIT_CODE)
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
