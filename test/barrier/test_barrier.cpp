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
#include <sync/stk_sync_barrier.h>
#include <sync/stk_sync_mutex.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_BARRIER_TEST_TASKS_MAX     5
#define _STK_BARRIER_TEST_TIMEOUT       1000
#define _STK_BARRIER_TEST_SHORT_SLEEP   10
#define _STK_BARRIER_TEST_LONG_SLEEP    100
#define _STK_BARRIER_STRESS_ITERATIONS  100
#ifdef __ARM_ARCH_6M__
#define _STK_BARRIER_STACK_SIZE         128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_BARRIER_STACK_SIZE         256
#define STK_TASK                        static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::barrier
    \brief     Namespace of Barrier test.
 */
namespace barrier {

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_ArrivedCount = 0;
static volatile int32_t g_TrueCount = 0;
static volatile int64_t g_ElapsedMs = 0;
static volatile int32_t g_InstancesDone = 0;
static volatile int32_t g_ArrivalSnapshot[_STK_BARRIER_TEST_TASKS_MAX] = {0};
static volatile int32_t g_Phase1[_STK_BARRIER_TEST_TASKS_MAX] = {0};
static volatile int32_t g_Phase2Sum[_STK_BARRIER_TEST_TASKS_MAX] = {0};

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_BARRIER_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test barriers - one per distinct group size exercised by the suite. Each is naturally left in
// its reset state (m_count == m_threshold) once every participating task has completed its final
// round, so - like g_TestMutex in the mutex suite - none of these are reconstructed between tests.
static sync::Barrier g_TestBarrier5(_STK_BARRIER_TEST_TASKS_MAX); // full pool (tests 1,2,3,5,7,8)
static sync::Barrier g_TestBarrier3(3);                           // partial group of 3 (test 6)
static sync::Barrier g_TestBarrier2(2);                           // partial group of 2 (test 4)

// Helper mutex used only for test bookkeeping (protecting shared counters/arrays below);
// it is NOT the primitive under test - Barrier itself provides no mutual exclusion guarantee.
static sync::Mutex g_CounterMtx;

// Barrier of the cancellation tests (see CancelWaitTask). Unlike the barriers above it is re-constructed for every
// test (its threshold depends on the scenario, and a failed cancel must not leave a corrupted count behind for the
// next test). It lives in raw storage and is accessed through the pointer returned by placement new, because Barrier
// has a const member (m_threshold): the old name must not be used to refer to a re-constructed object.
alignas(sync::Barrier) static uint8_t g_CancelBarrierStorage[sizeof(sync::Barrier)];
static sync::Barrier * g_CancelBarrier = nullptr;

// Cancellation test state (see CancelWaitTask); the first index is the task id
static const int32_t    CR_NOT_SET      = -100; // result not recorded yet
static ITask * volatile g_CancelTask[_STK_BARRIER_TEST_TASKS_MAX];      // published by every task when it starts
static volatile TId     g_CancelTid[_STK_BARRIER_TEST_TASKS_MAX];       // same, as TId (for SleepCancel)
static volatile int32_t g_CancelResult[_STK_BARRIER_TEST_TASKS_MAX][2]; // Barrier::EResult of the 1st / 2nd WaitEx() call
static volatile int64_t g_CancelElapsed[_STK_BARRIER_TEST_TASKS_MAX][2];// ms spent in the 1st / 2nd WaitEx() call
static volatile int32_t g_CancelDone[_STK_BARRIER_TEST_TASKS_MAX];      // 1 once the task has finished its script

// Scenario selector and threshold of the cancellation tests. Globals, not constructor arguments, because RunTest()
// declares tasks 0-1 as 'static': a static local is constructed only once per template instantiation (on the first
// RunTest<CancelWaitTask> call), so a constructor argument would silently keep the value of the first case.
// Set by main() before each RunTest() call.
static volatile int32_t  g_CancelParam     = 0;
static volatile uint32_t g_CancelThreshold = 2U;

// Cancel the wait of the victim (task 1) either via IKernel::CancelTaskWait() or via stk::SleepCancel()
static void CancelVictim(bool use_sleep_cancel)
{
    if (use_sleep_cancel)
        stk::SleepCancel(g_CancelTid[1]);
    else
        g_Kernel.CancelTaskWait(g_CancelTask[1]);
}

/*! \class BasicRendezvousTask
    \brief Tests the core barrier property: no task proceeds past Wait() until all have arrived.
    \note  Verifies mutual synchronization, not mutual exclusion.
*/
template <EAccessMode _AccessMode>
class BasicRendezvousTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BasicRendezvousTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        g_CounterMtx.Lock();
        ++g_ArrivedCount;
        g_CounterMtx.Unlock();

        g_TestBarrier5.Wait();

        // By the time Wait() returns for ANY task, every task must already have
        // incremented g_ArrivedCount - otherwise the barrier released early.
        g_CounterMtx.Lock();
        g_ArrivalSnapshot[m_task_id] = g_ArrivedCount;
        g_CounterMtx.Unlock();

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            bool ok = true;
            for (int32_t i = 0; i < _STK_BARRIER_TEST_TASKS_MAX; ++i)
            {
                if (g_ArrivalSnapshot[i] != _STK_BARRIER_TEST_TASKS_MAX)
                {
                    ok = false;
                    printf("Rendezvous violation: task %d saw arrival count %d (expected %d)\n",
                        (int)i, (int)g_ArrivalSnapshot[i], (int)_STK_BARRIER_TEST_TASKS_MAX);
                }
            }

            printf("basic rendezvous: all snapshots == %d ? %s\n",
                (int)_STK_BARRIER_TEST_TASKS_MAX, ok ? "yes" : "no");

            g_TestResult = ok ? 1 : 0;
        }
    }
};

/*! \class LastReturnsTrueTask
    \brief Tests the Wait() return-value contract.
    \note  Verifies that exactly one of the N tasks - the one that trips the barrier - gets true.
*/
template <EAccessMode _AccessMode>
class LastReturnsTrueTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    LastReturnsTrueTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        bool was_last = g_TestBarrier5.Wait();

        if (was_last)
        {
            g_CounterMtx.Lock();
            ++g_TrueCount;
            g_CounterMtx.Unlock();
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            printf("last-returns-true: count=%d (expected 1)\n", (int)g_TrueCount);

            g_TestResult = (g_TrueCount == 1) ? 1 : 0;
        }
    }
};

/*! \class CyclicReuseTask
    \brief Tests that the barrier resets itself and can be reused across multiple rounds.
    \note  Verifies the generation counter: exactly one true per round, over several rounds.
*/
template <EAccessMode _AccessMode>
class CyclicReuseTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    enum { ROUNDS = 5 };

public:
    CyclicReuseTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        for (int32_t round = 0; round < ROUNDS; ++round)
        {
            bool was_last = g_TestBarrier5.Wait();

            if (was_last)
            {
                g_CounterMtx.Lock();
                ++g_TrueCount;
                g_CounterMtx.Unlock();
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            printf("cyclic reuse: true_count=%d (expected %d)\n", (int)g_TrueCount, (int)ROUNDS);

            g_TestResult = (g_TrueCount == ROUNDS) ? 1 : 0;
        }
    }
};

/*! \class BlocksUntilLastArrivesTask
    \brief Tests that Wait() actually blocks rather than returning immediately.
    \note  Verifies elapsed time, analogous to the TimedLock timing check in the mutex suite.
*/
template <EAccessMode _AccessMode>
class BlocksUntilLastArrivesTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BlocksUntilLastArrivesTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: arrive late, after task 1 is already blocked in Wait()
            stk::Sleep(_STK_BARRIER_TEST_LONG_SLEEP);
            g_TestBarrier2.Wait();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: arrive immediately and measure how long Wait() actually blocks
            int64_t start = GetTimeNowMs();
            g_TestBarrier2.Wait();
            int64_t elapsed = GetTimeNowMs() - start;

            g_ElapsedMs = elapsed;

            printf("blocks-until-last-arrives: elapsed=%d ms (expected >= %d)\n",
                (int)elapsed, (int)(_STK_BARRIER_TEST_LONG_SLEEP - _STK_BARRIER_TEST_SHORT_SLEEP));

            g_TestResult = (elapsed >= (_STK_BARRIER_TEST_LONG_SLEEP - _STK_BARRIER_TEST_SHORT_SLEEP)) ? 1 : 0;
        }
        // Tasks 2+ are not used by this test

        ++g_InstancesDone;
    }
};

/*! \class DataVisibilityTask
    \brief Tests that Wait() acts as a memory fence for the group.
    \note  Verifies a value written before Wait() is visible to every task after Wait() returns.
*/
template <EAccessMode _AccessMode>
class DataVisibilityTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    DataVisibilityTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        // Phase 1: publish a per-task value
        g_Phase1[m_task_id] = m_task_id + 1;

        g_TestBarrier5.Wait();

        // Phase 2: every task must now be able to see its neighbor's phase-1 value
        int32_t neighbor = (m_task_id + 1) % _STK_BARRIER_TEST_TASKS_MAX;
        g_Phase2Sum[m_task_id] = g_Phase1[m_task_id] + g_Phase1[neighbor];

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            bool ok = true;
            for (int32_t i = 0; i < _STK_BARRIER_TEST_TASKS_MAX; ++i)
            {
                int32_t neighbor_i = (i + 1) % _STK_BARRIER_TEST_TASKS_MAX;
                int32_t expected = (i + 1) + (neighbor_i + 1);

                if (g_Phase2Sum[i] != expected)
                {
                    ok = false;
                    printf("Visibility violation: task %d sum=%d (expected %d)\n",
                        (int)i, (int)g_Phase2Sum[i], (int)expected);
                }
            }

            printf("data visibility: all neighbor sums correct ? %s\n", ok ? "yes" : "no");

            g_TestResult = ok ? 1 : 0;
        }
    }
};

/*! \class PartialGroupThresholdTask
    \brief Tests a barrier whose threshold is smaller than the full task pool.
    \note  Verifies correctness when only a subset of the kernel's tasks form the barrier group.
*/
template <EAccessMode _AccessMode>
class PartialGroupThresholdTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    static const int32_t GROUP_SIZE = 3;

public:
    PartialGroupThresholdTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        bool was_last = g_TestBarrier3.Wait();

        g_CounterMtx.Lock();
        ++g_SharedCounter;
        if (was_last)
            ++g_TrueCount;
        g_CounterMtx.Unlock();

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < GROUP_SIZE)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            printf("partial group (size %d): counter=%d, true_count=%d\n",
                (int)GROUP_SIZE, (int)g_SharedCounter, (int)g_TrueCount);

            g_TestResult = ((g_SharedCounter == GROUP_SIZE) && (g_TrueCount == 1)) ? 1 : 0;
        }
    }
};

/*! \class StaggeredArrivalTask
    \brief Tests barrier correctness when tasks arrive at each round with different, staggered delays.
    \note  Verifies no round's counter leaks into another generation under uneven arrival timing.
*/
template <EAccessMode _AccessMode>
class StaggeredArrivalTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    enum { ROUNDS = 3 };

public:
    StaggeredArrivalTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        for (int32_t round = 0; round < ROUNDS; ++round)
        {
            // Stagger each task's arrival within the round by a task-id-dependent delay
            stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP * m_task_id);

            g_TestBarrier5.Wait();

            g_CounterMtx.Lock();
            ++g_SharedCounter;
            g_CounterMtx.Unlock();

            stk::Yield();
        }

        ++g_InstancesDone;

        if (m_task_id == (_STK_BARRIER_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            int32_t expected = ROUNDS * _STK_BARRIER_TEST_TASKS_MAX;

            printf("staggered arrival: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            g_TestResult = (g_SharedCounter == expected) ? 1 : 0;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test with many consecutive barrier rounds.
    \note  Verifies barrier stability under heavy, repeated full-group synchronization.
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
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
            g_TestBarrier5.Wait();

            g_CounterMtx.Lock();
            ++g_SharedCounter;
            g_CounterMtx.Unlock();

            if ((i % 20) == 0)
                stk::Delay(1);
        }

        ++g_InstancesDone;

        // Last task verifies total
        if (m_task_id == (_STK_BARRIER_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_BARRIER_TEST_TASKS_MAX)
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);

            int32_t expected = m_iterations * _STK_BARRIER_TEST_TASKS_MAX;

            printf("Stress test: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            g_TestResult = (g_SharedCounter == expected) ? 1 : 0;
        }
    }
};

static const int32_t CW_BASIC             = 1;
static const int32_t CW_ROLLBACK          = 2;
static const int32_t CW_ONE_OF_MANY       = 3;
static const int32_t CW_SIGNAL_WINS       = 4;
static const int32_t CW_TRIP_AFTER_CANCEL = 5;
static const int32_t CW_VIA_SLEEPCANCEL   = 100;

static const int32_t CW_VERIFY_AT         = 200; // tick at which task 4 evaluates a scenario (every script is over by ~140)

static const int32_t CR_RELEASED          = static_cast<int32_t>(sync::Barrier::BARRIER_RELEASED);
static const int32_t CR_LAST              = static_cast<int32_t>(sync::Barrier::BARRIER_LAST_ARRIVAL);
static const int32_t CR_CANCELED          = static_cast<int32_t>(sync::Barrier::BARRIER_CANCELED);

/*! \class CancelWaitTask
    \brief Tests cancelability of Barrier waits (IKernel::CancelTaskWait() and stk::SleepCancel()).
    \note  Tasks 0-3 are the parties of the barrier under test (threshold 2 or 3, see g_CancelThreshold), task 1 is
           the victim whose wait gets cancelled, task 0 issues the cancel and task 4 is the verifier and watchdog:
           it evaluates the recorded results at tick CW_VERIFY_AT and then cancels every task still blocked in the
           barrier, so that a failed cancel is reported as a test failure instead of hanging the kernel.
           The scenario is selected by g_CancelParam: 1 = a cancelled wait ends as CANCELED, 2 = the cancelled
           arrival is rolled back (it does not count toward the round), 3 = cancelling one of several waiters
           leaves the others waiting and the round is completed by replacement arrivals, 4 = a barrier that
           tripped before the cancel wins (the cancel is a no-op and the barrier stays consistent for the next
           round), 5 = a cancel issued before the barrier tripped, but not yet acted upon by the victim when the
           barrier trips, must not roll back an arrival that the trip has already consumed. Adding 100 to
           g_CancelParam cancels via stk::SleepCancel() instead of IKernel::CancelTaskWait().
           Scenarios 4 and 5 rely on there being no scheduling point between two back-to-back calls of task 0.
*/
template <EAccessMode _AccessMode>
class CancelWaitTask : public Task<_STK_BARRIER_STACK_SIZE, _AccessMode>
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

        // publish this task for the canceller (task 1 is the victim) and for the watchdog
        g_CancelTask[m_task_id] = static_cast<ITask *>(this);
        g_CancelTid[m_task_id]  = stk::GetTid();

        if (m_task_id == (_STK_BARRIER_TEST_TASKS_MAX - 1))
        {
            RunVerifier();
        }
        else
        {
            RunParty();
            g_CancelDone[m_task_id] = 1;
        }
    }

    // Record how the call-th WaitEx() of this task ended
    void Record(int32_t call, sync::Barrier::EResult r, int64_t start)
    {
        g_CancelElapsed[m_task_id][call] = GetTimeNowMs() - start;
        g_CancelResult[m_task_id][call]  = static_cast<int32_t>(r);
    }

    // Arrive at the barrier for the call-th time
    void Arrive(int32_t call)
    {
        const int64_t start = GetTimeNowMs();
        const sync::Barrier::EResult r = g_CancelBarrier->WaitEx();

        Record(call, r, start);
    }

    // Tasks 0-3: the script of this task in the selected scenario
    void RunParty()
    {
        if (m_scenario == CW_BASIC)
        {
            // threshold 2: the victim waits for a second party that never comes, until it is cancelled
            if (m_task_id == 0)
            {
                stk::Sleep(60);
                CancelVictim(m_use_sleep_cancel);
            }
            else
            if (m_task_id == 1)
            {
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);
                Arrive(0);
            }
        }
        else
        if (m_scenario == CW_ROLLBACK)
        {
            // threshold 2: victim cancelled at ~60, task 2 arrives at ~90, task 0 completes the round at ~130
            if (m_task_id == 0)
            {
                stk::Sleep(60);
                CancelVictim(m_use_sleep_cancel);
                stk::Sleep(70);
                Arrive(0);
            }
            else
            if (m_task_id == 1)
            {
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);
                Arrive(0);
            }
            else
            if (m_task_id == 2)
            {
                stk::Sleep(90);
                Arrive(0);
            }
        }
        else
        if (m_scenario == CW_ONE_OF_MANY)
        {
            // threshold 3: victim (~10) and task 2 (~20) wait, victim cancelled at ~60; replacement arrivals
            // of task 3 (~100) and task 0 (~130) complete the round
            if (m_task_id == 0)
            {
                stk::Sleep(60);
                CancelVictim(m_use_sleep_cancel);
                stk::Sleep(70);
                Arrive(0);
            }
            else
            if (m_task_id == 1)
            {
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);
                Arrive(0);
            }
            else
            if (m_task_id == 2)
            {
                stk::Sleep(2 * _STK_BARRIER_TEST_SHORT_SLEEP);
                Arrive(0);
            }
            else
            {
                stk::Sleep(100);
                Arrive(0);
            }
        }
        else
        if (m_scenario == CW_SIGNAL_WINS)
        {
            // threshold 2: task 0 trips the barrier at ~60 and cancels the victim right after, then both
            // tasks go through a second round (victim at ~60, task 0 at ~100)
            if (m_task_id == 0)
            {
                stk::Sleep(60);

                const int64_t start = GetTimeNowMs();
                const sync::Barrier::EResult r = g_CancelBarrier->WaitEx();
                CancelVictim(m_use_sleep_cancel); // back to back: the victim is released by the trip but has not run yet
                Record(0, r, start);

                stk::Sleep(40);
                Arrive(1);
            }
            else
            if (m_task_id == 1)
            {
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);
                Arrive(0);
                Arrive(1);
            }
        }
        else
        if (m_scenario == CW_TRIP_AFTER_CANCEL)
        {
            // threshold 3: victim (~10) and task 2 (~20) wait; task 0 cancels the victim and arrives right after,
            // so that the barrier trips before the victim has run; then all three go through a second round
            if (m_task_id == 0)
            {
                stk::Sleep(60);

                const int64_t start = GetTimeNowMs();
                CancelVictim(m_use_sleep_cancel);
                const sync::Barrier::EResult r = g_CancelBarrier->WaitEx(); // back to back with the cancel
                Record(0, r, start);

                Arrive(1);
            }
            else
            if (m_task_id == 1)
            {
                stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);
                Arrive(0);
                Arrive(1);
            }
            else
            if (m_task_id == 2)
            {
                stk::Sleep(2 * _STK_BARRIER_TEST_SHORT_SLEEP);
                Arrive(0);
                Arrive(1);
            }
        }
        else
        {
            // unknown scenario: nothing to do
        }
    }

    // True if every party (tasks 0-3) has finished its script
    static bool AllDone()
    {
        for (int32_t id = 0; id < (_STK_BARRIER_TEST_TASKS_MAX - 1); ++id)
        {
            if (g_CancelDone[id] == 0)
                return false;
        }

        return true;
    }

    // Task 4: evaluate the scenario, then release anything still blocked
    void RunVerifier()
    {
        stk::Sleep(CW_VERIFY_AT);

        const bool pass = Evaluate();

        // Safety net: if the cancel did not work (or left the barrier with a wrong count) some parties are blocked
        // forever and the kernel would never stop. Cancel them so that the test fails instead of hanging; repeat,
        // because a released party may block again in its next barrier call.
        for (int32_t i = 0; (i < 4) && !AllDone(); ++i)
        {
            for (int32_t id = 0; id < (_STK_BARRIER_TEST_TASKS_MAX - 1); ++id)
            {
                if (g_CancelDone[id] == 0)
                    g_Kernel.CancelTaskWait(g_CancelTask[id]);
            }

            stk::Sleep(_STK_BARRIER_TEST_SHORT_SLEEP);
        }

        if (pass)
            g_TestResult = 1;
    }

    // Check the recorded results of the selected scenario
    bool Evaluate() const
    {
        bool pass = false;

        if (m_scenario == CW_BASIC)
        {
            // started waiting at ~10 ms, cancelled at ~60 ms
            pass = (g_CancelResult[1][0] == CR_CANCELED) &&
                   (g_CancelElapsed[1][0] >= 35) && (g_CancelElapsed[1][0] <= 75);
        }
        else
        if (m_scenario == CW_ROLLBACK)
        {
            // task 2 arrives at ~90 ms and task 0 at ~130 ms: had the victim's arrival not been rolled back, task 2
            // would have tripped the barrier by itself at once (LAST, elapsed ~0) and task 0 would be left waiting
            pass = (g_CancelResult[1][0] == CR_CANCELED) &&
                   (g_CancelResult[2][0] == CR_RELEASED) &&
                   (g_CancelElapsed[2][0] >= 30) && (g_CancelElapsed[2][0] <= 70) &&
                   (g_CancelResult[0][0] == CR_LAST);
        }
        else
        if (m_scenario == CW_ONE_OF_MANY)
        {
            // task 2 arrived at ~20 ms and must stay blocked until the round is completed at ~130 ms; had the
            // victim's arrival not been rolled back, task 3 (~100 ms) would have tripped the barrier too early
            pass = (g_CancelResult[1][0] == CR_CANCELED) &&
                   (g_CancelResult[2][0] == CR_RELEASED) &&
                   (g_CancelElapsed[2][0] >= 90) && (g_CancelElapsed[2][0] <= 130) &&
                   (g_CancelResult[3][0] == CR_RELEASED) &&
                   (g_CancelResult[0][0] == CR_LAST);
        }
        else
        if (m_scenario == CW_SIGNAL_WINS)
        {
            // round 1: the victim is released by the trip, the cancel is a no-op; round 2 (the victim waits from
            // ~60 ms, task 0 completes it at ~100 ms) shows that the barrier is consistent and that the cancel
            // left nothing behind that would hit the victim's next wait
            pass = (g_CancelResult[0][0] == CR_LAST) &&
                   (g_CancelResult[1][0] == CR_RELEASED) &&
                   (g_CancelResult[1][1] == CR_RELEASED) && (g_CancelElapsed[1][1] >= 30) &&
                   (g_CancelResult[0][1] == CR_LAST);
        }
        else
        if (m_scenario == CW_TRIP_AFTER_CANCEL)
        {
            // round 1: task 0 completes the round while the victim's cancel is still pending, so the victim has
            // been released by the trip, not cancelled out of it
            const bool round1 = (g_CancelResult[0][0] == CR_LAST) &&
                                (g_CancelResult[1][0] == CR_RELEASED) &&
                                (g_CancelResult[2][0] == CR_RELEASED);

            // round 2: all three arrive again: exactly one of them trips the barrier and the others are released
            // (with a corrupted count the barrier would need more arrivals and the round would never complete)
            int32_t last = 0, released = 0;
            for (int32_t id = 0; id < 3; ++id)
            {
                if (g_CancelResult[id][1] == CR_LAST)
                    ++last;
                else
                if (g_CancelResult[id][1] == CR_RELEASED)
                    ++released;
            }

            pass = round1 && (last == 1) && (released == 2);
        }
        else
        {
            // unknown scenario: fail
        }

        printf("cancel wait (scenario=%d, via %s, threshold=%d): call0=[%d,%d,%d,%d] call1=[%d,%d,%d,%d] "
               "elapsed(1,0)=%d elapsed(2,0)=%d ms\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait", (int)g_CancelThreshold,
            (int)g_CancelResult[0][0], (int)g_CancelResult[1][0], (int)g_CancelResult[2][0], (int)g_CancelResult[3][0],
            (int)g_CancelResult[0][1], (int)g_CancelResult[1][1], (int)g_CancelResult[2][1], (int)g_CancelResult[3][1],
            (int)g_CancelElapsed[1][0], (int)g_CancelElapsed[2][0]);

        return pass;
    }
};

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult = 0;
    g_SharedCounter = 0;
    g_ArrivedCount = 0;
    g_TrueCount = 0;
    g_ElapsedMs = 0;
    g_InstancesDone = 0;

    for (int32_t i = 0; i < _STK_BARRIER_TEST_TASKS_MAX; ++i)
    {
        g_ArrivalSnapshot[i] = 0;
        g_Phase1[i] = 0;
        g_Phase2Sum[i] = 0;

        g_CancelTask[i] = nullptr;
        g_CancelTid[i]  = TID_NONE;
        g_CancelDone[i] = 0;

        for (int32_t call = 0; call < 2; ++call)
        {
            g_CancelResult[i][call]  = CR_NOT_SET;
            g_CancelElapsed[i][call] = 0;
        }
    }

    // Re-construct the barrier of the cancellation tests with the threshold of the scenario
    if (g_CancelBarrier != nullptr)
        g_CancelBarrier->~Barrier();

    g_CancelBarrier = new (g_CancelBarrierStorage) sync::Barrier(g_CancelThreshold);
}

} // namespace barrier
} // namespace test
} // namespace stk

/*! \fn    NeedsExtendedTasks
    \brief Returns true if the test requires tasks 3 and 4.
    \note  BlocksUntilLastArrives uses only tasks 0-1; PartialGroupThreshold uses only tasks 0-2.
*/
static bool NeedsExtendedTasks(const char *test_name)
{
    return  (strcmp(test_name, "BlocksUntilLastArrives")  != 0) &&
            (strcmp(test_name, "PartialGroupThreshold")   != 0);
}

/*! \fn    NeedsThreeTasks
    \brief Returns true if the test requires at least 3 tasks (0-2).
    \note  BlocksUntilLastArrives uses only tasks 0-1, so task 2 is also unnecessary.
*/
static bool NeedsThreeTasks(const char *test_name)
{
    return  (strcmp(test_name, "BlocksUntilLastArrives") != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::barrier;

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

    using namespace stk::test::barrier;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Basic rendezvous - no task proceeds until all have arrived
    if (RunTest<BasicRendezvousTask<ACCESS_PRIVILEGED>>("BasicRendezvous") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Exactly one Wait() call returns true
    if (RunTest<LastReturnsTrueTask<ACCESS_PRIVILEGED>>("LastReturnsTrue") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Cyclic reuse across multiple rounds
    if (RunTest<CyclicReuseTask<ACCESS_PRIVILEGED>>("CyclicReuse") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Wait() actually blocks until the last task arrives
    if (RunTest<BlocksUntilLastArrivesTask<ACCESS_PRIVILEGED>>("BlocksUntilLastArrives") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Data written before Wait() is visible to all tasks after Wait()
    if (RunTest<DataVisibilityTask<ACCESS_PRIVILEGED>>("DataVisibility") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: Barrier threshold smaller than the full task pool
    if (RunTest<PartialGroupThresholdTask<ACCESS_PRIVILEGED>>("PartialGroupThreshold") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Staggered arrival times across multiple rounds
    if (RunTest<StaggeredArrivalTask<ACCESS_PRIVILEGED>>("StaggeredArrival") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Tests 9-18: cancelability of Barrier waits (all five tasks: 0-3 are the parties, 4 verifies), via
    // IKernel::CancelTaskWait() and via stk::SleepCancel() (param + 100)
    {
        struct CancelCase { const char *name; int32_t param; uint32_t threshold; };

        static const CancelCase cases[] =
        {
            { "CancelWait_Basic",                 CW_BASIC,                                  2 },
            { "CancelWait_Rollback",              CW_ROLLBACK,                               2 },
            { "CancelWait_OneOfMany",             CW_ONE_OF_MANY,                            3 },
            { "CancelWait_SignalWins",            CW_SIGNAL_WINS,                            2 },
            { "CancelWait_TripAfterCancel",       CW_TRIP_AFTER_CANCEL,                      3 },
            { "CancelWait_SleepCancel",           CW_VIA_SLEEPCANCEL + CW_BASIC,             2 },
            { "CancelWait_SleepCancelRollback",   CW_VIA_SLEEPCANCEL + CW_ROLLBACK,          2 },
            { "CancelWait_SleepCancelOneOfMany",  CW_VIA_SLEEPCANCEL + CW_ONE_OF_MANY,       3 },
            { "CancelWait_SleepCancelSignal",     CW_VIA_SLEEPCANCEL + CW_SIGNAL_WINS,       2 },
            { "CancelWait_SleepCancelTrip",       CW_VIA_SLEEPCANCEL + CW_TRIP_AFTER_CANCEL, 3 }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_CancelParam     = cases[i].param;
            g_CancelThreshold = cases[i].threshold;

            if (RunTest<CancelWaitTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param)
                != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

#endif // __ARM_ARCH_6M__

    // Test 8 (runs last, after the cancellation tests): Stress test
    if (RunTest<StressTestTask<ACCESS_PRIVILEGED>>("StressTest", _STK_BARRIER_STRESS_ITERATIONS) != TestContext::SUCCESS_EXIT_CODE)
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
