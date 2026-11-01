/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  test_sleep.cpp
    \brief Test suite for stk::Sleep(), stk::SleepUntil() and for cancellation of sleeping / waiting
           tasks (stk::SleepCancel(), IKernel::CancelTaskWait()).

    Cancellation semantics covered by the suite:
     - stk::SleepCancel(tid) ends a plain Sleep()/SleepUntil() early, and also cancels a wait on a
       synchronization object (Wait() returns WAIT_RESULT_CANCELED);
     - IKernel::CancelTaskWait() only affects a task blocked in IKernelService::Wait() on a
       synchronization object, it is a no-op for a task in a plain Sleep()/SleepUntil().

    The kernel is built with KERNEL_SYNC. To keep the suite self-contained (no dependency on the
    sync primitives headers) a minimal synchronization object (TestSync, built on SyncObjectBase) and
    a no-op mutex (TestMutex) are defined locally and used with IKernelService::Wait() directly.
    A no-op mutex is sufficient here: only the kernel's Wait() contract is under test and the
    tests have a single waiter per scenario (or independent waiters).

    Timing notes (tick = 1 ms, the default): Sleep(N) wakes no earlier than N ticks after the call
    and typically N + 1 or N + 2 ticks (entry tick + scheduling latency), therefore the lower bound is
    tight (LO_MS slack for measurement jitter) and the upper bound is generous (HI_MS).
*/

#include <stk_config.h>
#include <stk.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_SLEEP_TEST_TASKS_MAX      5
#define _STK_SLEEP_TEST_SHORT_SLEEP    10   // polling interval of the verifier (ms)
#define _STK_SLEEP_TEST_LO_MS          2    // allowed "early" slack of a measured sleep (ms)
#define _STK_SLEEP_TEST_HI_MS          25   // allowed "late" slack of a measured sleep (ms)
#define _STK_SLEEP_TEST_FAST_MS        50   // upper bound for 100 x Sleep(0) (ms)
#define _STK_SLEEP_TEST_LONG_MS        500  // sleep/wait time of a task that is going to be cancelled (ms)
#define _STK_SLEEP_TEST_CANCEL_AT_MS   20   // time at which the canceller acts (ms)

#if defined(__ARM_ARCH_6M__)
#define _STK_SLEEP_STACK_SIZE          256 // ARM Cortex-M0
#define STK_TASK
#elif defined(_STK_ARCH_RISC_V)
#define _STK_SLEEP_STACK_SIZE          384
#define STK_TASK                       static
#else
#define _STK_SLEEP_STACK_SIZE          256
#define STK_TASK                       static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::sleeptest
    \brief     Namespace of Sleep test.
 */
namespace sleeptest {

// Test results storage (every array element has a single writer)
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_TaskCount = 0;
static volatile int32_t g_Counter[_STK_SLEEP_TEST_TASKS_MAX] = {0};
static volatile int32_t g_Done[_STK_SLEEP_TEST_TASKS_MAX] = {0};
static volatile int32_t g_Elapsed[_STK_SLEEP_TEST_TASKS_MAX] = {0}; // measured sleep/wait time (ms)
static volatile int32_t g_WakeMs[_STK_SLEEP_TEST_TASKS_MAX] = {0};  // absolute wake time (ms)
static volatile int32_t g_Early[_STK_SLEEP_TEST_TASKS_MAX] = {0};   // number of early wakeups
static volatile int32_t g_Result[_STK_SLEEP_TEST_TASKS_MAX] = {0};  // per-task result (EWaitResult / bool)
static volatile int32_t g_Result2 = 0;                              // second wait result (victim only)
static volatile int32_t g_Elapsed2 = 0;                             // second wait time (victim only)
static volatile int32_t g_Flag = 0;
static volatile TId     g_Tid[_STK_SLEEP_TEST_TASKS_MAX] = {0};
static ITask *volatile  g_TaskPtr[_STK_SLEEP_TEST_TASKS_MAX] = {nullptr};

// Kernel (KERNEL_SYNC is required by CancelTaskWait and Wait)
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_SLEEP_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

/*! \class TestSync
    \brief Minimal synchronization object: just the wait list, signalled explicitly by the test.
*/
class TestSync : public SyncObjectBase
{
public:
    TestSync() {}
};

/*! \class TestMutex
    \brief No-op mutex handed to IKernelService::Wait() (see the file note).
*/
class TestMutex : public IMutex
{
public:
    void Lock() override {}
    void Unlock() override {}
    bool TryLock() override { return false; }
};

static TestSync  g_Sync;
static TestMutex g_Mutex;

static Timeout ToTicks(int32_t ms)
{
    return GetTicksFromMsClampedToTimeout(ms);
}

static bool AllDone()
{
    for (int32_t i = 0; i < g_TaskCount; ++i)
    {
        if (!g_Done[i])
            return false;
    }
    return true;
}

static void WaitAllDone()
{
    while (!AllDone())
        stk::Sleep(ToTicks(_STK_SLEEP_TEST_SHORT_SLEEP));
}

static bool InRange(int32_t v, int32_t lo, int32_t hi)
{
    return ((v >= lo) && (v <= hi));
}

//! Sleep of `ms` must last at least ms (minus measurement jitter) and not unreasonably longer.
static bool IsSleepOk(int32_t elapsed, int32_t ms)
{
    return InRange(elapsed, ms - _STK_SLEEP_TEST_LO_MS, ms + _STK_SLEEP_TEST_HI_MS);
}

//! Cancelled sleep: it ended after the canceller acted and long before the full sleep time.
static bool IsCancelledEarly(int32_t elapsed)
{
    return ((elapsed >= (_STK_SLEEP_TEST_CANCEL_AT_MS - _STK_SLEEP_TEST_LO_MS)) &&
            (elapsed < (_STK_SLEEP_TEST_LONG_MS / 2)));
}

#ifndef __ARM_ARCH_6M__

/*! \class SleepDurationTask
    \brief Tasks sleep for different times: 30, 10 and 20 ms.
    \note  Verifies that every sleep lasts the requested time and that the wake order follows the
           sleep durations (task 1 < task 2 < task 0).
*/
template <EAccessMode _AccessMode>
class SleepDurationTask : public Task<_STK_SLEEP_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SleepDurationTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    static int32_t Duration(uint8_t id)
    {
        static const int32_t d[3] = { 30, 10, 20 };
        return d[id];
    }

    void Run()
    {
        const int64_t start = GetTimeNowMs();

        stk::Sleep(ToTicks(Duration(m_task_id)));

        const int64_t end = GetTimeNowMs();
        g_Elapsed[m_task_id] = (int32_t)(end - start);
        g_WakeMs[m_task_id]  = (int32_t)end;
        g_Done[m_task_id] = 1;

        if (m_task_id == 0)
        {
            WaitAllDone();

            bool ok = true;
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                printf("sleep duration: task %d slept %d ms (requested %d)\n",
                    (int)i, (int)g_Elapsed[i], (int)Duration((uint8_t)i));

                if (!IsSleepOk(g_Elapsed[i], Duration((uint8_t)i)))
                    ok = false;
            }

            if (!((g_WakeMs[1] <= g_WakeMs[2]) && (g_WakeMs[2] <= g_WakeMs[0])))
            {
                printf("sleep duration: wake order is wrong (t1=%d t2=%d t0=%d)\n",
                    (int)g_WakeMs[1], (int)g_WakeMs[2], (int)g_WakeMs[0]);
                ok = false;
            }

            if (ok)
                g_TestResult = 1;
        }
    }
};

/*! \class SleepEdgeTask
    \brief Single task: Sleep(0) and the minimum non-zero sleep Sleep(1).
    \note  Sleep(0) must return promptly (it neither sleeps nor yields); Sleep(1) must not return
           immediately, each call lasts at least one tick.
*/
template <EAccessMode _AccessMode>
class SleepEdgeTask : public Task<_STK_SLEEP_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    SleepEdgeTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        // Sleep(0) x N
        const int64_t t0 = GetTimeNowMs();
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            stk::Sleep(0);
            ++g_Counter[0];
        }
        const int32_t zero_ms = (int32_t)(GetTimeNowMs() - t0);

        // Sleep(1) x 20
        enum { ONE_TICK_ITERS = 20 };
        const int64_t t1 = GetTimeNowMs();
        for (int32_t i = 0; i < ONE_TICK_ITERS; ++i)
            stk::Sleep(1);
        const int32_t one_ms = (int32_t)(GetTimeNowMs() - t1);

        printf("sleep edge: %d x Sleep(0) in %d ms (limit %d), %d x Sleep(1) in %d ms\n",
            (int)m_iterations, (int)zero_ms, (int)_STK_SLEEP_TEST_FAST_MS, (int)ONE_TICK_ITERS, (int)one_ms);

        bool ok = (g_Counter[0] == m_iterations) &&
                  (zero_ms < _STK_SLEEP_TEST_FAST_MS) &&
                  (one_ms >= ONE_TICK_ITERS) &&
                  (one_ms <= (3 * ONE_TICK_ITERS + _STK_SLEEP_TEST_HI_MS));

        g_Done[0] = 1;

        if (ok)
            g_TestResult = 1;
    }
};

/*! \class SleepUntilBasicTask
    \brief Single task: SleepUntil() with a future, past, current and next-tick timestamp.
    \note  Future: returns true, wakes not before the timestamp. Past/current: returns false at once.
*/
template <EAccessMode _AccessMode>
class SleepUntilBasicTask : public Task<_STK_SLEEP_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SleepUntilBasicTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        bool ok = true;

        // future timestamp
        {
            const Ticks target = GetTicks() + 40;
            const bool r = stk::SleepUntil(target);
            const Ticks after = GetTicks();

            printf("sleepuntil: future target=%d now=%d result=%d\n", (int)target, (int)after, (int)r);

            if (!r) ok = false;
            if (after < target) ok = false;
            if (after > (target + _STK_SLEEP_TEST_HI_MS)) ok = false;
        }

        // timestamp in the past: must return false immediately
        {
            const Ticks t0 = GetTicks();
            const bool r = stk::SleepUntil(t0 - 5);
            const Ticks t1 = GetTicks();

            printf("sleepuntil: past result=%d took %d ticks\n", (int)r, (int)(t1 - t0));

            if (r) ok = false;
            if ((t1 - t0) > 2) ok = false;
        }

        // timestamp equal to the current time: nothing to wait for
        {
            const Ticks t0 = GetTicks();
            const bool r = stk::SleepUntil(t0);
            const Ticks t1 = GetTicks();

            printf("sleepuntil: current result=%d took %d ticks\n", (int)r, (int)(t1 - t0));

            if (r) ok = false;
            if ((t1 - t0) > 2) ok = false;
        }

        // next tick: may be true or false depending on the tick boundary, but never early
        {
            const Ticks target = GetTicks() + 1;
            stk::SleepUntil(target);

            if (GetTicks() < target) ok = false;
        }

        g_Done[0] = 1;

        if (ok)
            g_TestResult = 1;
    }
};

/*! \class SleepUntilPeriodicTask
    \brief Tasks run a periodic loop with periods of 5, 10 and 15 ms using absolute timestamps.
    \note  Verifies no early wakeup and no accumulation of drift: the sum of N periods ends within
           HI_MS of N * period because every target is derived from the previous target.
*/
template <EAccessMode _AccessMode>
class SleepUntilPeriodicTask : public Task<_STK_SLEEP_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    SleepUntilPeriodicTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    static int32_t Period(uint8_t id)
    {
        static const int32_t p[3] = { 5, 10, 15 };
        return p[id];
    }

    void Run()
    {
        const int32_t period = Period(m_task_id);
        const Ticks start = GetTicks();
        Ticks target = start;

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            target += period;
            stk::SleepUntil(target);

            if (GetTicks() < target)
                ++g_Early[m_task_id];

            ++g_Counter[m_task_id];
        }

        g_Elapsed[m_task_id] = (int32_t)(GetTicks() - start);
        g_Done[m_task_id] = 1;

        if (m_task_id == 0)
        {
            WaitAllDone();

            bool ok = true;
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                const int32_t total = m_iterations * Period((uint8_t)i);

                printf("sleepuntil periodic: task %d period=%d ms, %d iterations in %d ms (expected %d), early=%d\n",
                    (int)i, (int)Period((uint8_t)i), (int)g_Counter[i], (int)g_Elapsed[i], (int)total, (int)g_Early[i]);

                if (g_Counter[i] != m_iterations) ok = false;
                if (g_Early[i] != 0) ok = false;
                if (!InRange(g_Elapsed[i], total, total + _STK_SLEEP_TEST_HI_MS)) ok = false;
            }

            if (ok)
                g_TestResult = 1;
        }
    }
};

/*! \class SleeperAmongSpinnersTask
    \brief Task 0 sleeps 50 ms while tasks 1 and 2 busy-spin (no Yield) until it wakes.
    \note  Verifies that the sleeper wakes on time while the CPU is fully loaded by other tasks
           (the sleeper is preempted back in by the tick) and that the spinners keep running.
*/
template <EAccessMode _AccessMode>
class SleeperAmongSpinnersTask : public Task<_STK_SLEEP_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SleeperAmongSpinnersTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        enum { SLEEP_MS = 50 };

        if (m_task_id == 0)
        {
            const int64_t start = GetTimeNowMs();
            stk::Sleep(ToTicks(SLEEP_MS));
            g_Elapsed[0] = (int32_t)(GetTimeNowMs() - start);

            g_Flag = 1;
            g_Done[0] = 1;

            WaitAllDone();

            printf("sleeper among spinners: woke after %d ms (expected ~%d), spinner progress=%d/%d\n",
                (int)g_Elapsed[0], (int)SLEEP_MS, (int)g_Counter[1], (int)g_Counter[2]);

            if (IsSleepOk(g_Elapsed[0], SLEEP_MS) && (g_Counter[1] > 0) && (g_Counter[2] > 0))
                g_TestResult = 1;
        }
        else
        {
            while (!g_Flag)
                ++g_Counter[m_task_id];

            g_Done[m_task_id] = 1;
        }
    }
};

/*! \enum  EScenario
    \brief Scenarios of CancelTask. Task 0 is the victim (sleeps or waits), task 1 is the canceller /
           signaller (acts after CANCEL_AT_MS), task 2 is a bystander (SC_SELECTIVE_CANCEL only).
*/
enum EScenario : int32_t
{
    SC_SLEEP_CANCEL = 0,       //!< SleepCancel() ends a plain Sleep() early.
    SC_SLEEPUNTIL_CANCEL,      //!< SleepCancel() ends a SleepUntil() early.
    SC_CANCELWAIT_ON_SLEEP,    //!< CancelTaskWait() on a task in plain Sleep() is a no-op.
    SC_WAIT_TIMEOUT,           //!< Wait() with nobody acting returns WAIT_RESULT_TIMEOUT on time.
    SC_WAIT_SIGNAL,            //!< Wait() woken by a signal returns WAIT_RESULT_SIGNAL.
    SC_WAIT_CANCEL,            //!< CancelTaskWait() interrupts a timed Wait(): WAIT_RESULT_CANCELED.
    SC_WAIT_CANCEL_INFINITE,   //!< CancelTaskWait() interrupts a WAIT_INFINITE Wait().
    SC_WAIT_SLEEPCANCEL,       //!< SleepCancel() on a task blocked in Wait(): WAIT_RESULT_CANCELED.
    SC_SELECTIVE_CANCEL,       //!< Only the targeted waiter is cancelled, the other one times out.
    SC_CANCEL_THEN_REWAIT      //!< After a cancelled Wait() the next Wait() works normally (times out).
};

/*! \class CancelTask
    \brief Sleep / wait cancellation scenarios (see EScenario).
*/
template <EAccessMode _AccessMode>
class CancelTask : public Task<_STK_SLEEP_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_scenario;

    enum { SHORT_WAIT_MS = 30, BYSTANDER_WAIT_MS = 150, CANCELWAIT_SLEEP_MS = 100 };

public:
    CancelTask(uint8_t task_id, int32_t scenario) : m_task_id(task_id), m_scenario(scenario)
    {}

private:
    Timeout WaitTimeout() const
    {
        switch (m_scenario)
        {
        case SC_WAIT_TIMEOUT:          return ToTicks(SHORT_WAIT_MS);
        case SC_WAIT_CANCEL_INFINITE:  return WAIT_INFINITE;
        case SC_SELECTIVE_CANCEL:      return ToTicks(BYSTANDER_WAIT_MS);
        default:                       return ToTicks(_STK_SLEEP_TEST_LONG_MS);
        }
    }

    static EWaitResult DoWait(Timeout timeout)
    {
        return IKernelService::GetInstance()->Wait(&g_Sync, &g_Mutex, timeout);
    }

    void RunVictim()
    {
        const int64_t start = GetTimeNowMs();

        switch (m_scenario)
        {
        case SC_SLEEP_CANCEL:
            stk::Sleep(ToTicks(_STK_SLEEP_TEST_LONG_MS));
            break;
        case SC_SLEEPUNTIL_CANCEL:
            g_Result[0] = stk::SleepUntil(GetTicks() + ToTicks(_STK_SLEEP_TEST_LONG_MS)) ? 1 : 0;
            break;
        case SC_CANCELWAIT_ON_SLEEP:
            stk::Sleep(ToTicks(CANCELWAIT_SLEEP_MS));
            break;
        default:
            g_Result[0] = DoWait(WaitTimeout());
            break;
        }

        g_Elapsed[0] = (int32_t)(GetTimeNowMs() - start);

        if (m_scenario == SC_CANCEL_THEN_REWAIT)
        {
            const int64_t start2 = GetTimeNowMs();
            g_Result2 = DoWait(ToTicks(SHORT_WAIT_MS));
            g_Elapsed2 = (int32_t)(GetTimeNowMs() - start2);
        }

        g_Done[0] = 1;

        WaitAllDone();
        Verify();
    }

    void RunActor()
    {
        stk::Sleep(ToTicks(_STK_SLEEP_TEST_CANCEL_AT_MS));

        // make sure the victim has published its identity
        while (g_TaskPtr[0] == nullptr)
            stk::Sleep(1);

        switch (m_scenario)
        {
        case SC_SLEEP_CANCEL:
        case SC_SLEEPUNTIL_CANCEL:
        case SC_WAIT_SLEEPCANCEL:
            stk::SleepCancel(g_Tid[0]);
            break;
        case SC_CANCELWAIT_ON_SLEEP:
        case SC_WAIT_CANCEL:
        case SC_WAIT_CANCEL_INFINITE:
        case SC_SELECTIVE_CANCEL:
        case SC_CANCEL_THEN_REWAIT:
            g_Kernel.CancelTaskWait(g_TaskPtr[0]);
            break;
        case SC_WAIT_SIGNAL:
            {
                const hw::CriticalSection::ScopedLock cs_;
                IKernelService::GetInstance()->Wake(&g_Sync, false);
            }
            break;
        case SC_WAIT_TIMEOUT:
        default:
            break; // nobody acts: the wait must time out
        }

        g_Done[1] = 1;
    }

    void RunBystander()
    {
        const int64_t start = GetTimeNowMs();
        g_Result[2] = DoWait(WaitTimeout());
        g_Elapsed[2] = (int32_t)(GetTimeNowMs() - start);
        g_Done[2] = 1;
    }

    void Verify()
    {
        const int32_t e = g_Elapsed[0];
        const int32_t r = g_Result[0];
        bool ok = false;

        switch (m_scenario)
        {
        case SC_SLEEP_CANCEL:
        case SC_SLEEPUNTIL_CANCEL:
            ok = IsCancelledEarly(e);
            break;
        case SC_CANCELWAIT_ON_SLEEP:
            // no-op: the sleep must last in full
            ok = IsSleepOk(e, CANCELWAIT_SLEEP_MS);
            break;
        case SC_WAIT_TIMEOUT:
            ok = (r == WAIT_RESULT_TIMEOUT) && IsSleepOk(e, SHORT_WAIT_MS);
            break;
        case SC_WAIT_SIGNAL:
            ok = (r == WAIT_RESULT_SIGNAL) && IsCancelledEarly(e);
            break;
        case SC_WAIT_CANCEL:
        case SC_WAIT_CANCEL_INFINITE:
        case SC_WAIT_SLEEPCANCEL:
            ok = (r == WAIT_RESULT_CANCELED) && IsCancelledEarly(e);
            break;
        case SC_SELECTIVE_CANCEL:
            ok = (r == WAIT_RESULT_CANCELED) && IsCancelledEarly(e) &&
                 (g_Result[2] == WAIT_RESULT_TIMEOUT) && IsSleepOk(g_Elapsed[2], BYSTANDER_WAIT_MS);
            break;
        case SC_CANCEL_THEN_REWAIT:
            ok = (r == WAIT_RESULT_CANCELED) && IsCancelledEarly(e) &&
                 (g_Result2 == WAIT_RESULT_TIMEOUT) && IsSleepOk(g_Elapsed2, SHORT_WAIT_MS);
            break;
        default:
            break;
        }

        printf("cancel: scenario=%d victim: result=%d elapsed=%d ms; bystander: result=%d elapsed=%d ms; "
               "rewait: result=%d elapsed=%d ms\n",
            (int)m_scenario, (int)r, (int)e, (int)g_Result[2], (int)g_Elapsed[2], (int)g_Result2, (int)g_Elapsed2);

        if (ok)
            g_TestResult = 1;
    }

    void Run()
    {
        g_Tid[m_task_id] = stk::GetTid();
        g_TaskPtr[m_task_id] = this;

        switch (m_task_id)
        {
        case 0:  RunVictim();    break;
        case 1:  RunActor();     break;
        default: RunBystander(); break;
        }
    }
};

#endif // __ARM_ARCH_6M__

/*! \class SleepStressTask
    \brief Many short sleeps, alternating Sleep() and SleepUntil() with different durations per task.
    \note  Verifies that no wakeup is early and no iteration is lost under heavy sleep/wake traffic
           with all tasks sleeping and waking at arbitrary moments. Runs on all platforms.
*/
template <EAccessMode _AccessMode>
class SleepStressTask : public Task<_STK_SLEEP_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    SleepStressTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            const Timeout d = 1 + ((i * 7 + m_task_id * 3) % 5); // 1..5 ticks

            if ((i & 1) != 0)
            {
                const Ticks t0 = GetTicks();
                stk::Sleep(d);

                if ((GetTicks() - t0) < d)
                    ++g_Early[m_task_id];
            }
            else
            {
                const Ticks target = GetTicks() + d;
                stk::SleepUntil(target);

                if (GetTicks() < target)
                    ++g_Early[m_task_id];
            }

            ++g_Counter[m_task_id];
        }

        g_Done[m_task_id] = 1;

        if (m_task_id == (g_TaskCount - 1))
        {
            WaitAllDone();

            bool ok = true;
            int32_t early = 0;
            for (int32_t i = 0; i < g_TaskCount; ++i)
            {
                early += g_Early[i];

                if (g_Counter[i] != m_iterations)
                    ok = false;
            }

            if (early != 0)
                ok = false;

            printf("sleep stress: %d tasks x %d iterations, early wakeups=%d: %s\n",
                (int)g_TaskCount, (int)m_iterations, (int)early, ok ? "yes" : "no");

            if (ok)
                g_TestResult = 1;
        }
    }
};

// Helper function to reset test state
static void ResetTestState(int32_t expected_done)
{
    g_TestResult = 0;
    g_TaskCount = expected_done;
    g_Flag = 0;
    g_Result2 = 0;
    g_Elapsed2 = 0;

    for (int32_t i = 0; i < _STK_SLEEP_TEST_TASKS_MAX; ++i)
    {
        g_Counter[i] = 0;
        g_Done[i] = 0;
        g_Elapsed[i] = 0;
        g_WakeMs[i] = 0;
        g_Early[i] = 0;
        g_Result[i] = 0;
        g_Tid[i] = 0;
        g_TaskPtr[i] = nullptr;
    }
}

} // namespace sleeptest
} // namespace test
} // namespace stk

/*! \fn    RunTest
    \brief Helper function to run a single test case.
    \param[in] test_name:   Name of the test.
    \param[in] tasks_added: Number of tasks (0..N-1) added to the kernel before Start().
    \param[in] param:       Parameter passed to every task (iteration count or scenario id).
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t tasks_added, int32_t param)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::sleeptest;

    printf("Test: %s\n", test_name);

    ResetTestState(tasks_added);

    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_Kernel.AddTask(&task0);

    if (tasks_added > 1)
        g_Kernel.AddTask(&task1);
    if (tasks_added > 2)
        g_Kernel.AddTask(&task2);
    if (tasks_added > 3)
        g_Kernel.AddTask(&task3);
    if (tasks_added > 4)
        g_Kernel.AddTask(&task4);

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

    using namespace stk::test::sleeptest;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#define RUN(TestClass, name, tasks, param) \
    do { \
        if (RunTest<TestClass<ACCESS_PRIVILEGED>>(name, tasks, param) \
                != TestContext::SUCCESS_EXIT_CODE) \
            total_failures++; \
        else \
            total_success++; \
    } while (0)

#ifndef __ARM_ARCH_6M__

    // Test 1: Sleep() durations of 30/10/20 ms are respected and tasks wake in order of duration
    RUN(SleepDurationTask,          "SleepDurationAndOrder",   3, 0);

    // Test 2: Sleep(0) returns promptly (no sleep, no yield), Sleep(1) lasts at least one tick
    RUN(SleepEdgeTask,              "SleepZeroAndOneTick",     1, 100);

    // Test 3: Sleeper wakes on time while two other tasks keep the CPU fully busy (no Yield)
    // Note: disabled for RISC-V due to QEMU RISC-V timing bug which starves 3-rd task
#ifndef _STK_ARCH_RISC_V
    RUN(SleeperAmongSpinnersTask,   "SleeperAmongSpinners",    3, 0);
#endif

    // Test 4: SleepUntil() with future (true), past (false), current (false) and next-tick timestamps
    RUN(SleepUntilBasicTask,        "SleepUntilBasic",         1, 0);

    // Test 5: Periodic SleepUntil() loops (5/10/15 ms) with no early wakeup and no accumulated drift
    RUN(SleepUntilPeriodicTask,     "SleepUntilPeriodic",      3, 10);

    // Tests 6-15: cancellation of sleeping and waiting tasks (tasks 0-2 only): task 0 is the victim,
    // task 1 is the canceller/signaller, task 2 is a bystander (selective cancel only). The scenario id
    // is passed as the task parameter.
    {
        struct CancelCase { const char *name; int32_t tasks; int32_t param; };

        static const CancelCase cases[] =
        {
            // Test 6: SleepCancel() ends a plain Sleep() early
            { "SleepCancelOnSleep",              2, SC_SLEEP_CANCEL },

            // Test 7: SleepCancel() ends a SleepUntil() early
            { "SleepCancelOnSleepUntil",         2, SC_SLEEPUNTIL_CANCEL },

            // Test 8: CancelTaskWait() on a task in a plain Sleep() is a no-op, the sleep lasts in full
            { "CancelTaskWaitOnPlainSleepIsNoop",2, SC_CANCELWAIT_ON_SLEEP },

            // Test 9: Control, Wait() with nobody acting returns WAIT_RESULT_TIMEOUT on time
            { "WaitTimeout",                     2, SC_WAIT_TIMEOUT },

            // Test 10: Control, Wait() woken by a signal returns WAIT_RESULT_SIGNAL
            { "WaitSignal",                      2, SC_WAIT_SIGNAL },

            // Test 11: CancelTaskWait() interrupts a timed Wait(), result is WAIT_RESULT_CANCELED
            { "CancelTaskWaitTimedWait",         2, SC_WAIT_CANCEL },

            // Test 12: CancelTaskWait() interrupts a WAIT_INFINITE Wait()
            { "CancelTaskWaitInfiniteWait",      2, SC_WAIT_CANCEL_INFINITE },

            // Test 13: SleepCancel() on a task blocked in Wait() cancels the wait the same way
            { "SleepCancelOnWait",               2, SC_WAIT_SLEEPCANCEL },

            // Test 14: Two waiters on one sync object, only the targeted one is cancelled (task 2 times out)
            { "CancelTaskWaitSelective",         3, SC_SELECTIVE_CANCEL },

            // Test 15: After a cancelled Wait() the next Wait() of the same task works normally (times out)
            { "CancelThenRewait",                2, SC_CANCEL_THEN_REWAIT }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            RUN(CancelTask, cases[i].name, cases[i].tasks, cases[i].param);
        }
    }

#endif // __ARM_ARCH_6M__

    // Test 16: Stress - all tasks alternate short Sleep()/SleepUntil() concurrently (ARM-M0 compatible)
    RUN(SleepStressTask,            "SleepStress",             _STK_SLEEP_TEST_TASKS_MAX, 100);

#undef RUN

    int32_t final_result = (total_failures == 0
        ? TestContext::SUCCESS_EXIT_CODE
        : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", (int)total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
