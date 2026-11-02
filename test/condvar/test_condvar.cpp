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
#include <sync/stk_sync_cv.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_CV_TEST_TASKS_MAX   5
#define _STK_CV_TEST_TIMEOUT     300
#define _STK_CV_TEST_SHORT_SLEEP 10
#define _STK_CV_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_CV_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_CV_STACK_SIZE       256
#define STK_TASK                 static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::condvar
    \brief     Namespace of ConditionVariable test.
 */
namespace condvar {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_AcquisitionOrder[_STK_CV_TEST_TASKS_MAX] = {0};
static volatile int32_t g_OrderIndex    = 0;
static volatile bool    g_TestComplete  = false;
static volatile int32_t g_InstancesDone = 0;

// Kernel (ConditionVariable requires KERNEL_SYNC)
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_CV_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test primitives (re-constructed per test via ResetTestState)
static sync::Mutex             g_TestMutex;
static sync::ConditionVariable g_TestCond;

// Cancellation test state (see CancelWaitTask)
static ITask * volatile g_Victim                 = nullptr;      // task whose wait gets cancelled (task 1)
static volatile TId     g_VictimTid              = TID_NONE;     // same, as TId (for SleepCancel)
static volatile int32_t g_VictimResult[2]        = {-100, -100}; // EWaitResult of task 1 / task 2 (-100 = not set)
static volatile int64_t g_VictimElapsed[2]       = {0, 0};       // ms spent in WaitEx() by task 1 / task 2
static volatile int32_t g_VictimOwnsMutex[2]     = {0, 0};       // 1 if task 1 / task 2 owned g_TestMutex on return from WaitEx()
static volatile int64_t g_VictimSleepElapsed     = 0;            // ms spent in Sleep() by task 1 after its wait ended

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

/*! \class NotifyOneWakesTask
    \brief Tests that NotifyOne() wakes exactly one waiting task per call.
    \note  Four consumer tasks block on Wait(WAIT_INFINITE); the producer calls
           NotifyOne() once per iteration with a Delay(1) between calls so each
           notification has time to land on exactly one waiter.
           If NotifyOne() wakes more than one task per call the counter exceeds
           m_iterations; if it wakes none the counter falls short.
*/
template <EAccessMode _AccessMode>
class NotifyOneWakesTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    NotifyOneWakesTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: let all consumers block first, then fire one notification per iteration
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestMutex.Lock();
                g_TestCond.NotifyOne();
                g_TestMutex.Unlock();

                stk::Delay(1); // pace so each notification wakes exactly one blocked consumer
            }

            stk::Sleep(_STK_CV_TEST_LONG_SLEEP);

            printf("notify-one wakes: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)m_iterations);

            if (g_SharedCounter == m_iterations)
                g_TestResult = 1;
        }
        else
        {
            // Consumers: loop waiting; each successful wake increments the counter
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestMutex.Lock();
                if (g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_TIMEOUT))
                    ++g_SharedCounter;
                g_TestMutex.Unlock();
            }
        }
    }
};

/*! \class NotifyAllWakesTask
    \brief Tests that NotifyAll() wakes every waiting task in one call.
    \note  All four consumer tasks block on Wait(); the producer fires a single
           NotifyAll() after all consumers are confirmed sleeping.
           The counter must equal the number of consumers (tasks_max - 1).
*/
template <EAccessMode _AccessMode>
class NotifyAllWakesTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    NotifyAllWakesTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: let all 4 consumers reach Wait() before signaling
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            g_TestMutex.Lock();
            g_TestCond.NotifyAll();
            g_TestMutex.Unlock();

            stk::Sleep(_STK_CV_TEST_LONG_SLEEP);

            int32_t expected = _STK_CV_TEST_TASKS_MAX - 1;

            printf("notify-all wakes: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
        else
        {
            // All consumer tasks block and must all be released by the single NotifyAll()
            g_TestMutex.Lock();
            if (g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_TIMEOUT))
                ++g_SharedCounter;
            g_TestMutex.Unlock();
        }
    }
};

/*! \class TimeoutExpiresTask
    \brief Tests that Wait() returns false within the expected time when no notification arrives.
    \note  Task 1 calls Wait() with a short timeout and verifies: return value is false,
           elapsed time is within the expected window. Task 2 then calls Wait() after the
           producer fires NotifyOne() to confirm that a post-timeout notification still works.
*/
template <EAccessMode _AccessMode>
class TimeoutExpiresTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimeoutExpiresTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Wait with a short timeout; no producer fires, so it must expire
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            g_TestMutex.Lock();
            int64_t start   = GetTimeNowMs();
            bool    woken   = g_TestCond.Wait(g_TestMutex, 50);
            int64_t elapsed = GetTimeNowMs() - start;
            g_TestMutex.Unlock();

            // Must return false (timeout), and elapsed must be within the 50-tick window
            if (!woken && elapsed >= 45 && elapsed <= 65)
                ++g_SharedCounter; // 1: timeout returned false in correct time
        }
        else
        if (m_task_id == 2)
        {
            // Wait with generous timeout; producer fires after task 1's timeout expires
            stk::Sleep(120); // well past task 1's 50-tick timeout

            g_TestMutex.Lock();
            bool woken = g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_TIMEOUT);
            g_TestMutex.Unlock();

            if (woken)
                ++g_SharedCounter; // 2: correctly woken by producer
        }
        else
        if (m_task_id == 0)
        {
            // Producer: fires after both task 1 timeout and task 2 block
            stk::Sleep(130);

            g_TestMutex.Lock();
            g_TestCond.NotifyOne();
            g_TestMutex.Unlock();

            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            printf("timeout expires: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class MutexReacquiredTask
    \brief Tests that Wait() atomically releases the mutex and re-acquires it before returning.
    \note  Consumer holds the mutex, enters Wait(); while it is sleeping the producer
           acquires the mutex (proving it was released), increments a flag, then calls
           NotifyOne(). Consumer resumes; if the mutex was correctly re-acquired the
           consumer can call Unlock() without triggering the ownership assert.
           Both the signaled path and the timeout path are tested.
*/
template <EAccessMode _AccessMode>
class MutexReacquiredTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    MutexReacquiredTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Consumer (signaled path): Wait() must re-acquire mutex on wake
            g_TestMutex.Lock();

            bool woken = g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_TIMEOUT);

            // If mutex was correctly re-acquired this Unlock() will not assert
            if (woken && g_SharedCounter == 1)
                ++g_SharedCounter; // 2: mutex re-acquired after signaled wake
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 2)
        {
            // Consumer (timeout path): Wait() must also re-acquire mutex on timeout
            g_TestMutex.Lock();

            bool woken = g_TestCond.Wait(g_TestMutex, 50);

            // If mutex was correctly re-acquired this Unlock() will not assert
            if (!woken)
                ++g_SharedCounter; // 3: mutex re-acquired after timeout
            g_TestMutex.Unlock();
        }
        else
        if (m_task_id == 0)
        {
            // Producer: wait for consumer 1 to enter Wait() then prove the mutex is free
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            g_TestMutex.Lock(); // must succeed: consumer released it inside Wait()
            ++g_SharedCounter;  // 1: mutex was free while consumer waited
            g_TestCond.NotifyOne();
            g_TestMutex.Unlock();

            stk::Sleep(_STK_CV_TEST_LONG_SLEEP);

            printf("mutex reacquired: counter=%d (expected 3)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 3)
                g_TestResult = 1;
        }
    }
};

/*! \class PredicateLoopTask
    \brief Tests the canonical spurious-wakeup-safe predicate loop pattern.
    \note  Each consumer loops: while (!g_SharedCondition) Wait(); so that spurious
           wakeups or mis-ordered notifications are handled correctly.
           The producer sets the shared condition flag and calls NotifyOne() exactly
           once per consumer. All consumers must exit their loops and the final
           counter must equal the number of consumers.
*/
template <EAccessMode _AccessMode>
class PredicateLoopTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PredicateLoopTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: let all consumers reach their predicate-Wait() loops, then
            // notify one at a time until all consumers have exited
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            int32_t consumers = _STK_CV_TEST_TASKS_MAX - 1;

            for (int32_t i = 0; i < consumers; ++i)
            {
                g_TestMutex.Lock();
                ++g_SharedCounter; // raise the predicate: one more consumer may proceed
                g_TestCond.NotifyOne();
                g_TestMutex.Unlock();

                stk::Delay(1); // pace so each consumer gets a chance to re-evaluate
            }

            stk::Sleep(_STK_CV_TEST_LONG_SLEEP);

            printf("predicate loop: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)(_STK_CV_TEST_TASKS_MAX - 1));

            if (g_SharedCounter == (_STK_CV_TEST_TASKS_MAX - 1))
                g_TestResult = 1;
        }
        else
        {
            // Consumers: wait until the predicate is satisfied for them specifically.
            // Each consumer claims one unit of g_SharedCounter; the loop guards against
            // spurious wakeups.
            g_TestMutex.Lock();

            while (g_SharedCounter < (int32_t)m_task_id)
                g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_TIMEOUT);

            // predicate satisfied: consumer has logically consumed its slot
            g_TestMutex.Unlock();
        }
    }
};

/*! \class NotifyOneOrderTask
    \brief Tests that NotifyOne() releases waiters in FIFO arrival order.
    \note  Tasks 1-4 register their wait arrival in g_AcquisitionOrder in order of
           scheduling; the producer calls NotifyOne() once per consumer with a Delay(1)
           between calls. On each wake the consumer records its task_id in the order
           array. The final order must match the arrival order exactly.
*/
template <EAccessMode _AccessMode>
class NotifyOneOrderTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    NotifyOneOrderTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: let all consumers block first, then wake them one by one
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            int32_t consumers = _STK_CV_TEST_TASKS_MAX - 1;

            for (int32_t i = 0; i < consumers; ++i)
            {
                g_TestMutex.Lock();
                g_TestCond.NotifyOne();
                g_TestMutex.Unlock();

                stk::Delay(1); // give the woken consumer time to record its order entry
            }

            stk::Sleep(_STK_CV_TEST_LONG_SLEEP);

            // Verify FIFO: each woken task_id must be strictly greater than the previous
            bool ordered = true;
            for (int32_t i = 1; i < consumers; ++i)
            {
                if (g_AcquisitionOrder[i] <= g_AcquisitionOrder[i - 1])
                {
                    ordered = false;
                    break;
                }
            }

            printf("notify-one order: order=[%d,%d,%d,%d], ordered=%d (expected 1)\n",
                (int)g_AcquisitionOrder[0], (int)g_AcquisitionOrder[1],
                (int)g_AcquisitionOrder[2], (int)g_AcquisitionOrder[3],
                (int)ordered);

            if (ordered && g_SharedCounter == consumers)
                g_TestResult = 1;
        }
        else
        {
            // Consumers: block in order 1, 2, 3, 4; on wake, record arrival position
            g_TestMutex.Lock();

            if (g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_TIMEOUT))
            {
                int32_t slot = g_OrderIndex++;
                g_AcquisitionOrder[slot] = m_task_id;
                ++g_SharedCounter;
            }

            g_TestMutex.Unlock();
        }
    }
};

/*! \class NoWaitTimeoutTask
    \brief Tests that Wait(NO_WAIT) returns false immediately without blocking.
    \note  Wait() called with NO_WAIT (== 0) is defined to return false immediately
           per the implementation (see ConditionVariable::Wait, timeout == NO_WAIT path).
           Verifies elapsed time is well below SHORT_SLEEP; also verifies that a
           normal Wait() still works correctly on the same condition variable afterwards.
*/
template <EAccessMode _AccessMode>
class NoWaitTimeoutTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    NoWaitTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // NO_WAIT must return false immediately with no blocking
            g_TestMutex.Lock();
            int64_t start   = GetTimeNowMs();
            bool    woken   = g_TestCond.Wait(g_TestMutex, NO_WAIT);
            int64_t elapsed = GetTimeNowMs() - start;
            g_TestMutex.Unlock();

            if (!woken && elapsed < _STK_CV_TEST_SHORT_SLEEP)
                ++g_SharedCounter; // 1: returned false immediately
        }
        else
        if (m_task_id == 2)
        {
            // Normal WAIT_INFINITE Wait() after NO_WAIT must still work correctly
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            g_TestMutex.Lock();
            bool woken = g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_TIMEOUT);
            g_TestMutex.Unlock();

            if (woken)
                ++g_SharedCounter; // 2: woken correctly by producer
        }
        else
        if (m_task_id == 0)
        {
            // Producer: fires after task 2 has entered Wait()
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP + 5);

            g_TestMutex.Lock();
            g_TestCond.NotifyOne();
            g_TestMutex.Unlock();

            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            printf("no-wait timeout: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test of ConditionVariable under full five-task contention.
    \note  Each task alternates producer and consumer roles each iteration.
           Odd iterations: acquire mutex, increment shared data, NotifyOne(), release.
           Even iterations: acquire mutex, Wait() until notified, release.
           The total number of successful wakes across all tasks must reach the
           guaranteed minimum (number of producer iterations across all tasks).
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
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
                // Producer role: signal one waiter
                g_TestMutex.Lock();
                ++g_SharedCounter;
                g_TestCond.NotifyOne();
                g_TestMutex.Unlock();
            }
            else
            {
                // Consumer role: wait for a signal (may fail under contention if no producer fires)
                g_TestMutex.Lock();
                g_TestCond.Wait(g_TestMutex, _STK_CV_TEST_SHORT_SLEEP);
                g_TestMutex.Unlock();
            }
        }

        ++g_InstancesDone;

        if (m_task_id == (_STK_CV_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_CV_TEST_TASKS_MAX)
                stk::Sleep(_STK_CV_TEST_LONG_SLEEP);

            // Minimum guaranteed: producer iterations only (m_iterations / 2 per task,
            // rounded up since even index 0 is always a producer iteration)
            int32_t min_expected = _STK_CV_TEST_TASKS_MAX * ((m_iterations + 1) / 2);

            printf("stress test: counter=%d (min expected %d)\n",
                (int)g_SharedCounter, (int)min_expected);

            if (g_SharedCounter >= min_expected)
                g_TestResult = 1;
        }
    }
};

static const int32_t CW_INFINITE        = 1;
static const int32_t CW_NOT_TIMEOUT     = 2;
static const int32_t CW_NO_STALE_WAITER = 3;
static const int32_t CW_MUTEX_HELD      = 4;
static const int32_t CW_TWO_WAITERS     = 5;
static const int32_t CW_SIGNAL_WINS     = 6;
static const int32_t CW_VIA_SLEEPCANCEL = 100;

/*! \class CancelWaitTask
    \brief Tests cancelability of ConditionVariable waits (IKernel::CancelTaskWait() and stk::SleepCancel()).
    \note  Task 0 is the canceller/checker, task 1 is the victim (blocked in WaitEx()), task 2 is a
           second waiter used by the two-waiters scenario. The scenario is selected by g_CancelParam:
           1 = cancel an infinite wait, 2 = cancel is not reported as a timeout, 3 = cancel leaves
           no stale waiter behind, 4 = a cancelled waiter still re-acquires the mutex (and blocks
           while another task holds it), 5 = cancelling one of two waiters leaves the other intact,
           6 = a notification that arrived first wins over a late cancel (the cancel lands while the
           woken waiter re-locks the mutex and must be absorbed). Adding 100 to g_CancelParam cancels
           via stk::SleepCancel() instead of IKernel::CancelTaskWait().
           In every scenario the victim must own the mutex when WaitEx() returns, regardless of
           how the wait ended.
*/
template <EAccessMode _AccessMode>
class CancelWaitTask : public Task<_STK_CV_STACK_SIZE, _AccessMode>
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
            RunSecondWaiter();
    }

    // Task 1: block on the condition variable, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        const Timeout timeout = (m_scenario == CW_NOT_TIMEOUT ? 300 : WAIT_INFINITE);

        g_TestMutex.Lock();

        const int64_t start = GetTimeNowMs();
        const EWaitResult r = g_TestCond.WaitEx(g_TestMutex, timeout);

        g_VictimElapsed[0]    = GetTimeNowMs() - start;
        g_VictimResult[0]     = static_cast<int32_t>(r);
        g_VictimOwnsMutex[0]  = (g_TestMutex.GetOwner() == g_VictimTid ? 1 : 0);

        g_TestMutex.Unlock();

        if (m_scenario == CW_NO_STALE_WAITER)
        {
            // a stale wait entry left behind by the cancel would cut this sleep short
            const int64_t sleep_start = GetTimeNowMs();
            stk::Sleep(60);
            g_VictimSleepElapsed = GetTimeNowMs() - sleep_start;
        }
    }

    // Task 2: second waiter, queued behind task 1
    void RunSecondWaiter()
    {
        if (m_scenario != CW_TWO_WAITERS)
            return;

        stk::Sleep(2 * _STK_CV_TEST_SHORT_SLEEP); // block after task 1

        g_TestMutex.Lock();

        const int64_t start = GetTimeNowMs();
        const EWaitResult r = g_TestCond.WaitEx(g_TestMutex, WAIT_INFINITE);

        g_VictimElapsed[1]   = GetTimeNowMs() - start;
        g_VictimResult[1]    = static_cast<int32_t>(r);
        g_VictimOwnsMutex[1] = (g_TestMutex.GetOwner() == stk::GetTid() ? 1 : 0);

        g_TestMutex.Unlock();
    }

    // Task 0: cancel and verify
    void RunCanceller()
    {
        bool pass = false;

        stk::Sleep(60); // task 1 (and task 2) are blocked in WaitEx() by now

        if (m_scenario == CW_INFINITE)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP * 2);

            // started waiting at ~10 ms, cancelled at ~60 ms
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   (g_VictimOwnsMutex[0] == 1);
        }
        else
        if (m_scenario == CW_NOT_TIMEOUT)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP * 2);

            // 300 ms timeout is far away: the wait must end early and as CANCELED, not TIMEOUT
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimElapsed[0] < 150) &&
                   (g_VictimOwnsMutex[0] == 1);
        }
        else
        if (m_scenario == CW_NO_STALE_WAITER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP); // victim returns and starts its Sleep(60)

            // nobody waits: a notification is lost, but a ghost wait entry would be woken by it
            g_TestCond.NotifyAll();

            stk::Sleep(70); // let the victim finish its sleep

            printf("cancel no stale waiter: result=%d owns=%d victim_sleep=%d ms\n",
                (int)g_VictimResult[0], (int)g_VictimOwnsMutex[0], (int)g_VictimSleepElapsed);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimOwnsMutex[0] == 1) &&
                   (g_VictimSleepElapsed >= 55); // not woken early by a ghost wait entry
        }
        else
        if (m_scenario == CW_MUTEX_HELD)
        {
            // hold the mutex (free: the victim released it inside WaitEx()) across the cancel: the victim
            // is woken as CANCELED but must stay blocked re-acquiring the mutex until it is released
            g_TestMutex.Lock();

            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            const bool still_blocked = (g_VictimResult[0] == -100);
            const bool owner_is_me   = (g_TestMutex.GetOwner() == stk::GetTid());

            g_TestMutex.Unlock(); // hands the mutex over to the victim
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP * 2);

            printf("cancel mutex held: still_blocked=%d owner_is_me=%d result=%d owns=%d\n",
                (int)still_blocked, (int)owner_is_me, (int)g_VictimResult[0], (int)g_VictimOwnsMutex[0]);

            pass = still_blocked && owner_is_me &&
                   (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimOwnsMutex[0] == 1);
        }
        else
        if (m_scenario == CW_TWO_WAITERS)
        {
            CancelVictim(m_use_sleep_cancel);              // cancel the first waiter only
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);

            g_TestMutex.Lock();
            g_TestCond.NotifyOne();                        // must wake the second waiter, not a ghost of the first
            g_TestMutex.Unlock();
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP * 2);

            printf("cancel two waiters: first=%d second=%d owns=[%d,%d]\n",
                (int)g_VictimResult[0], (int)g_VictimResult[1],
                (int)g_VictimOwnsMutex[0], (int)g_VictimOwnsMutex[1]);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimResult[1] == WAIT_RESULT_SIGNAL) &&
                   (g_VictimOwnsMutex[0] == 1) && (g_VictimOwnsMutex[1] == 1);
        }
        else
        if (m_scenario == CW_SIGNAL_WINS)
        {
            // notify and cancel back to back while holding the mutex: the waiter is already signaled and
            // removed from the wait list, it is only blocked re-locking the mutex when the cancel arrives,
            // so the cancel must be absorbed (Mutex::Lock() is not cancelable) and the result stay SIGNAL
            g_TestMutex.Lock();
            g_TestCond.NotifyOne();
            CancelVictim(m_use_sleep_cancel);
            g_TestMutex.Unlock();
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP * 2);

            pass = (g_VictimResult[0] == WAIT_RESULT_SIGNAL) && (g_VictimOwnsMutex[0] == 1);
        }

        // Safety net: if the cancel did not work a waiter is still blocked forever and the kernel would
        // never stop, release any such waiter so that the test fails instead of hanging.
        if (!pass)
        {
            g_TestCond.NotifyAll();
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);
            g_TestCond.NotifyAll();
            stk::Sleep(_STK_CV_TEST_SHORT_SLEEP);
        }

        printf("cancel wait (scenario=%d, via %s): result[0]=%d elapsed[0]=%d ms\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
            (int)g_VictimResult[0], (int)g_VictimElapsed[0]);

        if (pass)
            g_TestResult = 1;
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

    for (int32_t i = 0; i < _STK_CV_TEST_TASKS_MAX; ++i)
        g_AcquisitionOrder[i] = 0;

    g_Victim             = nullptr;
    g_VictimTid          = TID_NONE;
    g_VictimSleepElapsed = 0;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]    = -100;
        g_VictimElapsed[i]   = 0;
        g_VictimOwnsMutex[i] = 0;
    }

    // Re-construct the mutex and condition variable in-place for a clean state
    g_TestMutex.~Mutex();
    new (&g_TestMutex) sync::Mutex();

    g_TestCond.~ConditionVariable();
    new (&g_TestCond) sync::ConditionVariable();
}

} // namespace condvar
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return  (strcmp(test_name, "TimeoutExpires")   != 0) &&
            (strcmp(test_name, "MutexReacquired")  != 0) &&
            (strcmp(test_name, "NoWaitTimeout")    != 0) &&
            (strncmp(test_name, "CancelWait", 10)  != 0); // cancel tests use tasks 0-2 only
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::condvar;

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

    using namespace stk::test::condvar;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: NotifyOne() wakes exactly one waiting task per call
    if (RunTest<NotifyOneWakesTask<ACCESS_PRIVILEGED>>("NotifyOneWakes", 20) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: NotifyAll() wakes every waiting task in a single call
    if (RunTest<NotifyAllWakesTask<ACCESS_PRIVILEGED>>("NotifyAllWakes") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Wait() returns false within correct time window when no notification arrives (tasks 0-2 only)
    if (RunTest<TimeoutExpiresTask<ACCESS_PRIVILEGED>>("TimeoutExpires") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Wait() atomically releases mutex and re-acquires it before returning (tasks 0-2 only)
    if (RunTest<MutexReacquiredTask<ACCESS_PRIVILEGED>>("MutexReacquired") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Spurious-wakeup-safe predicate loop correctly drives all consumers to completion
    if (RunTest<PredicateLoopTask<ACCESS_PRIVILEGED>>("PredicateLoop") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: NotifyOne() releases waiters in FIFO arrival order
    if (RunTest<NotifyOneOrderTask<ACCESS_PRIVILEGED>>("NotifyOneOrder") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Wait(NO_WAIT) returns false immediately without blocking (tasks 0-2 only)
    if (RunTest<NoWaitTimeoutTask<ACCESS_PRIVILEGED>>("NoWaitTimeout") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Tests 9-19: cancelability of ConditionVariable waits (tasks 0-2 only), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        struct CancelCase { const char *name; int32_t param; };

        static const CancelCase cases[] =
        {
            { "CancelWait_Infinite",            CW_INFINITE                              },
            { "CancelWait_NotTimeout",          CW_NOT_TIMEOUT                           },
            { "CancelWait_NoStaleWaiter",       CW_NO_STALE_WAITER                       },
            { "CancelWait_MutexHeld",           CW_MUTEX_HELD                            },
            { "CancelWait_TwoWaiters",          CW_TWO_WAITERS                           },
            { "CancelWait_SignalWins",          CW_SIGNAL_WINS                           },
            { "CancelWait_SleepCancel",         CW_VIA_SLEEPCANCEL + CW_INFINITE         },
            { "CancelWait_SleepCancelNoStale",  CW_VIA_SLEEPCANCEL + CW_NO_STALE_WAITER  },
            { "CancelWait_SleepCancelMutex",    CW_VIA_SLEEPCANCEL + CW_MUTEX_HELD       },
            { "CancelWait_SleepCancelTwo",      CW_VIA_SLEEPCANCEL + CW_TWO_WAITERS      },
            { "CancelWait_SleepCancelSignal",   CW_VIA_SLEEPCANCEL + CW_SIGNAL_WINS      }
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

    // Test 8 (runs last, after the cancellation tests): Stress test under full five-task contention with alternating producer/consumer roles
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
