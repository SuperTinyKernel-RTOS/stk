/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  test_kernel.cpp
    \brief Test suite for kernel functions running on a real kernel instance (target / simulator).
           New kernel level tests are added here.

    Currently covered: the scheduler lock API
     - stk::LockScheduler(), stk::UnlockScheduler(), stk::RestoreSchedulerLock() and their results,
     - stk::ScopedSchedulerLock (RAII, nesting),
     - IKernel::GetState() reporting KSTATE_LOCKED,
     and the behavior of a locked kernel observed from other tasks:
     - no task switch happens while the scheduler is locked (the spinner makes no progress),
     - the lock does not stop the tick: time advances, sleep timers expire, but the woken task runs
       only after the unlock,
     - a wake-up of a task blocked on a synchronization object is deferred until the unlock,
     - nested locks keep the scheduler locked until the outermost one is released,
     - mutual exclusion of a section guarded by stk::ScopedSchedulerLock.

    Not covered here (contract violations assert in a debug build and can not run to completion on a real
    target; they, as well as the ISR and KERNEL_HRT failure results, are covered by the mock based unit tests
    of the kernel, see stktest_kernel.cpp): Sleep(), SleepUntil(), Yield(), Wait(), SuspendTask() of the lock
    holder and a task exit while locked.

    The kernel is built with KERNEL_SYNC. To keep the suite self-contained (no dependency on the sync
    primitives headers) a minimal synchronization object (TestSync, built on SyncObjectBase) and a no-op
    mutex (TestMutex) are defined locally and used with IKernelService::Wait() / Wake() directly.

    Timing notes (tick = 1 ms, the default): the lower bounds are tight (LO_MS slack for measurement jitter),
    the upper bounds are generous (HI_MS).
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

#define _STK_KERNEL_TEST_TASKS_MAX      5
#define _STK_KERNEL_TEST_SHORT_SLEEP    10    // polling interval of the verifier (ms)
#define _STK_KERNEL_TEST_LO_MS          2     // allowed "early" slack of a measured time (ms)
#define _STK_KERNEL_TEST_HI_MS          25    // allowed "late" slack of a measured time (ms)
#define _STK_KERNEL_TEST_LOCK_MS        40    // time the lock holder keeps the scheduler locked (ms)
#define _STK_KERNEL_TEST_SETTLE_MS      20    // time given to the other tasks to run after the unlock (ms)
#define _STK_KERNEL_TEST_REGION_LOOPS   5000  // busy loop inside of a guarded section (widens the race window)
#define _STK_KERNEL_TEST_REGION_ITERS   200   // guarded section entries per task

#if defined(__ARM_ARCH_6M__)
#define _STK_KERNEL_STACK_SIZE          256 // ARM Cortex-M0
#define STK_TASK
#elif defined(_STK_ARCH_RISC_V)
#define _STK_KERNEL_STACK_SIZE          384
#define STK_TASK                        static
#else
#define _STK_KERNEL_STACK_SIZE          256
#define STK_TASK                        static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::kerneltest
    \brief     Namespace of Kernel test.
 */
namespace kerneltest {

// Test results storage (every array element has a single writer)
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_TaskCount = 0;
static volatile int32_t g_Counter[_STK_KERNEL_TEST_TASKS_MAX] = {0};
static volatile int32_t g_Done[_STK_KERNEL_TEST_TASKS_MAX] = {0};
static volatile int32_t g_WakeMs[_STK_KERNEL_TEST_TASKS_MAX] = {0};  // absolute time of an event (ms), 0 = not happened
static volatile int32_t g_Result[_STK_KERNEL_TEST_TASKS_MAX] = {0};  // per-task result (EWaitResult)
static volatile int32_t g_Flag = 0;
static volatile int32_t g_Inside = 0;     // number of tasks inside of the guarded section
static volatile int32_t g_Violations = 0; // guarded section entered by more than one task at a time
static volatile int32_t g_Shared = 0;     // value modified by a non-atomic read-modify-write in the guarded section
static volatile int32_t g_Spin = 0;       // busy loop sink

// Kernel (KERNEL_SYNC is required by Wait/Wake)
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_KERNEL_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

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
        stk::Sleep(ToTicks(_STK_KERNEL_TEST_SHORT_SLEEP));
}

//! Wait (sleeping, no lock held) until a counter of another task reaches the value.
static void WaitCounter(int32_t idx, int32_t value)
{
    while (g_Counter[idx] < value)
        stk::Sleep(1);
}

//! Check helper: reports a failed check and clears \a ok.
#define STK_KTEST_CHECK(ok, cond) \
    do { \
        if (!(cond)) \
        { \
            printf("  check failed (line %d): %s\n", (int)__LINE__, #cond); \
            (ok) = false; \
        } \
    } while (0)

/*! \class LockApiTask
    \brief Single task: results and kernel state of LockScheduler(), UnlockScheduler(),
           RestoreSchedulerLock() and ScopedSchedulerLock.
    \note  Lock()/Unlock() report the PREVIOUS state, RestoreSchedulerLock() reports the NEW state.
*/
template <EAccessMode _AccessMode>
class LockApiTask : public Task<_STK_KERNEL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    LockApiTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    static bool IsState(IKernel::EKernelState s)
    {
        return (g_Kernel.GetState() == s);
    }

    void Run()
    {
        bool ok = true;

        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));

        // Lock / Unlock: previous state is reported
        STK_KTEST_CHECK(ok, stk::LockScheduler() == LOCK_WAS_UNLOCKED);
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));

        STK_KTEST_CHECK(ok, stk::LockScheduler() == LOCK_WAS_LOCKED); // not nested: still one lock
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));

        STK_KTEST_CHECK(ok, stk::UnlockScheduler() == LOCK_WAS_LOCKED);
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING)); // a single unlock releases it

        STK_KTEST_CHECK(ok, stk::UnlockScheduler() == LOCK_WAS_UNLOCKED);
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));

        // RestoreSchedulerLock: the new state is reported
        STK_KTEST_CHECK(ok, stk::RestoreSchedulerLock(true) == LOCK_WAS_LOCKED);
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));

        STK_KTEST_CHECK(ok, stk::RestoreSchedulerLock(true) == LOCK_WAS_LOCKED);
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));

        STK_KTEST_CHECK(ok, stk::RestoreSchedulerLock(false) == LOCK_WAS_UNLOCKED);
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));

        STK_KTEST_CHECK(ok, stk::RestoreSchedulerLock(false) == LOCK_WAS_UNLOCKED);
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));

        // emulated nesting with the saved state: the outer lock stays held
        {
            const ELockResult outer = stk::LockScheduler();
            const ELockResult inner = stk::LockScheduler();

            STK_KTEST_CHECK(ok, outer == LOCK_WAS_UNLOCKED);
            STK_KTEST_CHECK(ok, inner == LOCK_WAS_LOCKED);

            STK_KTEST_CHECK(ok, stk::RestoreSchedulerLock(inner == LOCK_WAS_LOCKED) == LOCK_WAS_LOCKED);
            STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));

            STK_KTEST_CHECK(ok, stk::RestoreSchedulerLock(outer == LOCK_WAS_LOCKED) == LOCK_WAS_UNLOCKED);
            STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));
        }

        // RAII guard
        {
            const stk::ScopedSchedulerLock guard;
            STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));
        }
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));

        // nested RAII guards: the state before construction is restored, the outer guard stays locked
        {
            const stk::ScopedSchedulerLock outer;
            {
                const stk::ScopedSchedulerLock inner;
                STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));
            }
            STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));
        }
        STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));

        // RAII guard inside of a manual lock: the manual lock stays held after the guard
        {
            STK_KTEST_CHECK(ok, stk::LockScheduler() == LOCK_WAS_UNLOCKED);
            {
                const stk::ScopedSchedulerLock guard;
                STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));
            }
            STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_LOCKED));
            STK_KTEST_CHECK(ok, stk::UnlockScheduler() == LOCK_WAS_LOCKED);
            STK_KTEST_CHECK(ok, IsState(IKernel::KSTATE_RUNNING));
        }

        // a task keeps running normally after the lock was used (sleeps and wakes)
        {
            const Ticks t0 = GetTicks();
            stk::Sleep(ToTicks(5));
            STK_KTEST_CHECK(ok, (GetTicks() - t0) >= 5);
        }

        printf("lock api: %s\n", ok ? "ok" : "failed");

        g_Done[m_task_id] = 1;

        if (ok)
            g_TestResult = 1;
    }
};

#ifndef __ARM_ARCH_6M__

/*! \enum  EScenario
    \brief Scenarios of LockTask. Task 0 is the lock holder, task 1 is the observer (spinner, sleeper or waiter).
           SC_MUTUAL_EXCLUSION uses all tasks.
*/
enum EScenario : int32_t
{
    SC_NO_SWITCH = 0,      //!< No task switch while locked: a spinning task makes no progress, ticks advance.
    SC_NESTED_GUARDS,      //!< Nested ScopedSchedulerLock: still no switch after the inner guard has been released.
    SC_SLEEPER_DEFERRED,   //!< A sleeper whose timer expires while locked runs only after the unlock.
    SC_WAKE_DEFERRED,      //!< A waiter woken (IKernelService::Wake) under the lock resumes only after the unlock.
    SC_MUTUAL_EXCLUSION    //!< A section guarded by ScopedSchedulerLock is never entered by two tasks at a time.
};

/*! \class LockTask
    \brief Observable effects of a locked scheduler (see EScenario).
*/
template <EAccessMode _AccessMode>
class LockTask : public Task<_STK_KERNEL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_scenario;

    enum { OBSERVER_SLEEP_MS = 10 };

public:
    LockTask(uint8_t task_id, int32_t scenario) : m_task_id(task_id), m_scenario(scenario)
    {}

private:
    // ---- observer (task 1) --------------------------------------------------------------------------------

    //! Spinner: increments a counter without ever yielding, until told to stop.
    void RunSpinner()
    {
        while (!g_Flag)
            ++g_Counter[1];

        g_Done[1] = 1;
    }

    //! Sleeper: sleeps and records the time it resumed at.
    void RunSleeper()
    {
        g_Counter[1] = 1; // about to sleep
        stk::Sleep(ToTicks(OBSERVER_SLEEP_MS));
        g_WakeMs[1] = (int32_t)GetTimeNowMs();

        g_Done[1] = 1;
    }

    //! Waiter: blocks on the synchronization object until woken.
    void RunWaiter()
    {
        g_Counter[1] = 1; // about to wait
        g_Result[1] = IKernelService::GetInstance()->Wait(&g_Sync, &g_Mutex, WAIT_INFINITE);
        g_WakeMs[1] = (int32_t)GetTimeNowMs();
        g_Counter[1] = 2; // returned

        g_Done[1] = 1;
    }

    // ---- lock holder (task 0) -----------------------------------------------------------------------------

    //! Keeps the scheduler locked for LOCK_MS and verifies that the spinner made no progress.
    static bool HoldAndCheckNoSwitch(bool &ok, int32_t &progress_after)
    {
        const int32_t c0 = g_Counter[1];
        const Ticks   t0 = GetTicks();

        stk::Delay(ToTicks(_STK_KERNEL_TEST_LOCK_MS)); // busy-wait: allowed under the lock

        const int32_t c1 = g_Counter[1];
        const Ticks   t1 = GetTicks();

        printf("  locked for %d ticks: spinner progress %d -> %d\n", (int)(t1 - t0), (int)c0, (int)c1);

        // no task switch: the spinner did not run at all
        STK_KTEST_CHECK(ok, c1 == c0);

        // the tick is not stopped by the lock: time advanced by the full busy-wait
        STK_KTEST_CHECK(ok, (t1 - t0) >= (_STK_KERNEL_TEST_LOCK_MS - _STK_KERNEL_TEST_LO_MS));

        progress_after = c1;
        return ok;
    }

    //! After the unlock the spinner must get the CPU again.
    static void CheckSpinnerResumed(bool &ok, int32_t progress_after)
    {
        stk::Sleep(ToTicks(_STK_KERNEL_TEST_SETTLE_MS));

        printf("  after unlock: spinner progress %d -> %d\n", (int)progress_after, (int)g_Counter[1]);

        STK_KTEST_CHECK(ok, g_Counter[1] > progress_after);
    }

    void RunHolderNoSwitch(bool nested)
    {
        bool ok = true;
        int32_t progress = 0;

        // let the spinner run first
        stk::Sleep(ToTicks(_STK_KERNEL_TEST_SHORT_SLEEP));
        STK_KTEST_CHECK(ok, g_Counter[1] > 0);

        if (!nested)
        {
            STK_KTEST_CHECK(ok, stk::LockScheduler() == LOCK_WAS_UNLOCKED);
            STK_KTEST_CHECK(ok, g_Kernel.GetState() == IKernel::KSTATE_LOCKED);

            HoldAndCheckNoSwitch(ok, progress);

            STK_KTEST_CHECK(ok, stk::UnlockScheduler() == LOCK_WAS_LOCKED);
        }
        else
        {
            {
                const stk::ScopedSchedulerLock outer;
                {
                    const stk::ScopedSchedulerLock inner;
                    STK_KTEST_CHECK(ok, g_Kernel.GetState() == IKernel::KSTATE_LOCKED);
                }

                // the inner guard has been released, the outer one still holds the lock
                STK_KTEST_CHECK(ok, g_Kernel.GetState() == IKernel::KSTATE_LOCKED);

                HoldAndCheckNoSwitch(ok, progress);
            }
        }

        STK_KTEST_CHECK(ok, g_Kernel.GetState() == IKernel::KSTATE_RUNNING);

        CheckSpinnerResumed(ok, progress);

        g_Flag = 1; // stop the spinner
        g_Done[0] = 1;
        WaitAllDone();

        printf("lock holder (%s): %s\n", nested ? "nested guards" : "no switch", ok ? "ok" : "failed");

        if (ok)
            g_TestResult = 1;
    }

    void RunHolderSleeperDeferred()
    {
        bool ok = true;

        // let the sleeper start its sleep (it would never start it once the scheduler is locked)
        WaitCounter(1, 1);
        stk::Sleep(ToTicks(2));

        const int32_t lock_ms = (int32_t)GetTimeNowMs();
        {
            const stk::ScopedSchedulerLock guard;

            // the sleeper's timer (10 ms) expires while the scheduler is locked: the tick is still running
            stk::Delay(ToTicks(_STK_KERNEL_TEST_LOCK_MS));

            STK_KTEST_CHECK(ok, GetTimeNowMs() >= (lock_ms + _STK_KERNEL_TEST_LOCK_MS - _STK_KERNEL_TEST_LO_MS));

            // ... but the sleeper has not been scheduled
            STK_KTEST_CHECK(ok, g_WakeMs[1] == 0);
        }
        const int32_t unlock_ms = (int32_t)GetTimeNowMs();

        // the expired sleeper runs soon after the unlock (not before it)
        stk::Sleep(ToTicks(_STK_KERNEL_TEST_SETTLE_MS));

        printf("  locked at %d ms, unlocked at %d ms, sleeper resumed at %d ms\n",
            (int)lock_ms, (int)unlock_ms, (int)g_WakeMs[1]);

        STK_KTEST_CHECK(ok, g_WakeMs[1] != 0);
        STK_KTEST_CHECK(ok, g_WakeMs[1] >= (unlock_ms - _STK_KERNEL_TEST_LO_MS));
        STK_KTEST_CHECK(ok, g_WakeMs[1] <= (unlock_ms + _STK_KERNEL_TEST_HI_MS));

        g_Done[0] = 1;
        WaitAllDone();

        printf("lock holder (sleeper deferred): %s\n", ok ? "ok" : "failed");

        if (ok)
            g_TestResult = 1;
    }

    void RunHolderWakeDeferred()
    {
        bool ok = true;

        // let the waiter block on the synchronization object
        WaitCounter(1, 1);
        stk::Sleep(ToTicks(_STK_KERNEL_TEST_SHORT_SLEEP));

        STK_KTEST_CHECK(ok, g_Counter[1] == 1); // still waiting

        int32_t unlock_ms;
        {
            const stk::ScopedSchedulerLock guard;

            {
                const hw::CriticalSection::ScopedLock cs_;
                IKernelService::GetInstance()->Wake(&g_Sync, false);
            }

            stk::Delay(ToTicks(_STK_KERNEL_TEST_LOCK_MS));

            // woken, but not scheduled while the scheduler is locked
            STK_KTEST_CHECK(ok, g_Counter[1] == 1);
            STK_KTEST_CHECK(ok, g_WakeMs[1] == 0);

            unlock_ms = (int32_t)GetTimeNowMs();
        }

        stk::Sleep(ToTicks(_STK_KERNEL_TEST_SETTLE_MS));

        printf("  unlocked at %d ms, waiter resumed at %d ms, result=%d\n",
            (int)unlock_ms, (int)g_WakeMs[1], (int)g_Result[1]);

        STK_KTEST_CHECK(ok, g_Counter[1] == 2);
        STK_KTEST_CHECK(ok, g_Result[1] == WAIT_RESULT_SIGNAL);
        STK_KTEST_CHECK(ok, g_WakeMs[1] >= (unlock_ms - _STK_KERNEL_TEST_LO_MS));
        STK_KTEST_CHECK(ok, g_WakeMs[1] <= (unlock_ms + _STK_KERNEL_TEST_HI_MS));

        g_Done[0] = 1;
        WaitAllDone();

        printf("lock holder (wake deferred): %s\n", ok ? "ok" : "failed");

        if (ok)
            g_TestResult = 1;
    }

    // ---- mutual exclusion (all tasks) ---------------------------------------------------------------------

    void RunMutualExclusion()
    {
        for (int32_t i = 0; i < _STK_KERNEL_TEST_REGION_ITERS; ++i)
        {
            {
                const stk::ScopedSchedulerLock guard;

                if (++g_Inside != 1)
                    ++g_Violations;

                // non-atomic read-modify-write with a long window: lost updates show a missing exclusion
                const int32_t v = g_Shared;
                for (int32_t k = 0; k < _STK_KERNEL_TEST_REGION_LOOPS; ++k)
                    ++g_Spin;
                g_Shared = v + 1;

                --g_Inside;
            }

            // outside of the section: let the other tasks interleave
            if ((i & 7) == 0)
                stk::Sleep(1);
        }

        g_Done[m_task_id] = 1;

        if (m_task_id == (g_TaskCount - 1))
        {
            WaitAllDone();

            const int32_t expected = g_TaskCount * _STK_KERNEL_TEST_REGION_ITERS;

            printf("mutual exclusion: %d tasks x %d entries, shared=%d (expected %d), violations=%d\n",
                (int)g_TaskCount, (int)_STK_KERNEL_TEST_REGION_ITERS, (int)g_Shared, (int)expected, (int)g_Violations);

            if ((g_Shared == expected) && (g_Violations == 0) && (g_Inside == 0))
                g_TestResult = 1;
        }
    }

    void Run()
    {
        if (m_scenario == SC_MUTUAL_EXCLUSION)
        {
            RunMutualExclusion();
            return;
        }

        if (m_task_id == 0)
        {
            switch (m_scenario)
            {
            case SC_NO_SWITCH:         RunHolderNoSwitch(false);      break;
            case SC_NESTED_GUARDS:     RunHolderNoSwitch(true);       break;
            case SC_SLEEPER_DEFERRED:  RunHolderSleeperDeferred();    break;
            case SC_WAKE_DEFERRED:     RunHolderWakeDeferred();       break;
            default:                                                  break;
            }
        }
        else
        {
            switch (m_scenario)
            {
            case SC_NO_SWITCH:
            case SC_NESTED_GUARDS:     RunSpinner();                  break;
            case SC_SLEEPER_DEFERRED:  RunSleeper();                  break;
            case SC_WAKE_DEFERRED:     RunWaiter();                   break;
            default:                                                  break;
            }
        }
    }
};

#endif // __ARM_ARCH_6M__

// Helper function to reset test state
static void ResetTestState(int32_t expected_done)
{
    g_TestResult = 0;
    g_TaskCount = expected_done;
    g_Flag = 0;
    g_Inside = 0;
    g_Violations = 0;
    g_Shared = 0;
    g_Spin = 0;

    for (int32_t i = 0; i < _STK_KERNEL_TEST_TASKS_MAX; ++i)
    {
        g_Counter[i] = 0;
        g_Done[i] = 0;
        g_WakeMs[i] = 0;
        g_Result[i] = 0;
    }
}

} // namespace kerneltest
} // namespace test
} // namespace stk

/*! \fn    RunTest
    \brief Helper function to run a single test case.
    \param[in] test_name:   Name of the test.
    \param[in] tasks_added: Number of tasks (0..N-1) added to the kernel before Start().
    \param[in] param:       Parameter passed to every task (scenario id).
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t tasks_added, int32_t param)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::kerneltest;

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

    using namespace stk::test::kerneltest;

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

    // Test 1: scheduler lock API: results (previous state for Lock/Unlock, new state for Restore),
    // KSTATE_LOCKED, emulated nesting with the saved state, ScopedSchedulerLock (also nested)
    RUN(LockApiTask,    "SchedulerLockApi",                 1, 0);

#ifndef __ARM_ARCH_6M__

    // Test 2: No task switch while locked: the spinner makes no progress, the tick keeps advancing,
    // the spinner resumes after the unlock
    RUN(LockTask,       "SchedulerLockNoSwitch",            2, SC_NO_SWITCH);

    // Test 3: Same with nested ScopedSchedulerLock: released inner guard keeps the scheduler locked
    RUN(LockTask,       "SchedulerLockNestedGuards",        2, SC_NESTED_GUARDS);

    // Test 4: A sleeper whose timer expires while locked is scheduled only after the unlock
    RUN(LockTask,       "SchedulerLockSleeperDeferred",     2, SC_SLEEPER_DEFERRED);

    // Test 5: A waiter woken under the lock resumes (WAIT_RESULT_SIGNAL) only after the unlock
    RUN(LockTask,       "SchedulerLockWakeDeferred",        2, SC_WAKE_DEFERRED);

    // Test 6: Mutual exclusion of a section guarded by ScopedSchedulerLock (4 tasks)
    RUN(LockTask,       "SchedulerLockMutualExclusion",     4, SC_MUTUAL_EXCLUSION);

#endif // __ARM_ARCH_6M__

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
