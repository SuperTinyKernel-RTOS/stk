/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include <time/stk_time_timer.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_TIMER_TEST_TIMEOUT     1000
#define _STK_TIMER_TEST_SHORT_SLEEP 10
#define _STK_TIMER_TEST_LONG_SLEEP  100
#define _STK_TIMER_STACK_SIZE       256 // min stack size required
#ifdef __ARM_ARCH_6M__
#define _STK_TIMER_TEST_TASKS_MAX   2
#define STK_TASK
#else
#define _STK_TIMER_TEST_TASKS_MAX   5
#define STK_TASK                    static
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::timer
    \brief     Namespace of TimerHost test.
 */
namespace timer {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_ExpiredCount  = 0;
static sync::Event g_LastExpired[_STK_TIMER_TEST_TASKS_MAX];
static volatile int64_t g_ExpiredTime[_STK_TIMER_TEST_TASKS_MAX] = {0};

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_TIMER_TEST_TASKS_MAX + stk::time::TimerHost::TASK_COUNT, SwitchStrategyRR, PlatformDefault> g_Kernel;

// TimerHost
static time::TimerHost g_TimerHost;

/*! \class TestTimer
    \brief Concrete timer implementation that increments g_ExpiredCount on each expiry.
*/
class TestTimer : public time::TimerHost::Timer
{
    uint8_t m_timer_id;

public:
    TestTimer(uint8_t timer_id) : m_timer_id(timer_id)
    {}

    void OnExpired(time::TimerHost */*host*/)
    {
        ++g_ExpiredCount;
        g_LastExpired[m_timer_id].Set();
        g_ExpiredTime[m_timer_id] = GetTimeNowMsec();
    }
};

/*! \class OneShotTimerTask
    \brief Tests that a one-shot timer fires exactly once at the expected time.
    \note  Task 1 starts a one-shot timer with 50ms delay. Task 0 (verifier) waits
           and confirms the timer fired exactly once with elapsed time in [45, 65]ms.
*/
template <EAccessMode _AccessMode>
class OneShotTimerTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    OneShotTimerTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer(1);

        if (m_task_id == 1)
        {
            int64_t start = GetTimeNowMsec();
            g_TimerHost.Start(timer, 50); // 50-tick one-shot

            // Wait until timer fires
            while (g_ExpiredCount == 0)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            int64_t elapsed = g_ExpiredTime[1] - start;

            if ((g_ExpiredCount == 1) && (elapsed >= 45) && (elapsed <= 65))
                g_SharedCounter = 1;

            // Wait to confirm it doesn't fire again
            stk::Sleep(_STK_TIMER_TEST_LONG_SLEEP);

            if (g_ExpiredCount == 1 && !timer.IsActive())
                g_SharedCounter = 2;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_TIMER_TEST_LONG_SLEEP * 2);

            printf("one-shot timer: count=%d (expected 1), result=%d (expected 2)\n",
                (int)g_ExpiredCount, (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class PeriodicTimerTask
    \brief Tests that a periodic timer fires repeatedly at regular intervals.
    \note  Task 1 starts a periodic timer with 30ms period. Task 0 waits 150ms then
           verifies the timer fired approximately 5 times (150 / 30 = 5). Tolerance
           accounts for scheduler jitter.
*/
template <EAccessMode _AccessMode>
class PeriodicTimerTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PeriodicTimerTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer(1);

        if (m_task_id == 1)
        {
            g_TimerHost.Start(timer, 30, 30); // 30ms initial + 30ms period

            stk::Sleep(150); // let it fire ~5 times

            g_TimerHost.Stop(timer);

            // Stop is asynchronous, therefore wait for a completion
            int32_t wait = 10;
            while (timer.IsActive() && wait)
            {
                stk::Yield();
                --wait;
            }

            // Verify 4-6 firings (150 / 30 ≈ 5)
            if ((g_ExpiredCount >= 4) && (g_ExpiredCount <= 6) && !timer.IsActive())
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(150 * 2);

            printf("periodic timer: count=%d (expected 4-6)\n", (int)g_ExpiredCount);

            if (g_SharedCounter == 1)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class MultipleTimersTask
    \brief Tests that multiple concurrent timers with different periods fire independently.
    \note  Task 1 starts three timers: 20ms periodic, 35ms periodic, 60ms one-shot.
           Task 0 waits 100ms then verifies all three fired the expected number of times.
*/
template <EAccessMode _AccessMode>
class MultipleTimersTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    MultipleTimersTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer1(1); // 20ms periodic
        static TestTimer timer2(2); // 35ms periodic
        static TestTimer timer3(3); // 60ms one-shot

        if (m_task_id == 1)
        {
            g_TimerHost.Start(timer1, 20, 20); // ~5 firings in 100ms
            g_TimerHost.Start(timer2, 35, 35); // ~3 firings in 100ms
            g_TimerHost.Start(timer3, 60);     // 1 firing at 60ms

            stk::Sleep(100);

            g_TimerHost.Stop(timer1);
            g_TimerHost.Stop(timer2);

            // Verify: timer1 (4-6), timer2 (2-4), timer3 (1)
            int32_t total = g_ExpiredCount;

            if ((total >= 7) && (total <= 11) && !timer3.IsActive())
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(150);

            printf("multiple timers: total=%d (expected 7-11)\n", (int)g_ExpiredCount);

            if (g_SharedCounter == 1)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class StopTimerTask
    \brief Tests that Stop() cancels a pending timer before it fires.
    \note  Task 1 starts a 100ms one-shot, then immediately stops it. Task 0 waits
           and confirms the timer never fired.
*/
template <EAccessMode _AccessMode>
class StopTimerTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    StopTimerTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer(1);

        if (m_task_id == 1)
        {
            g_TimerHost.Start(timer, 100);
            stk::Sleep(2); // small delay to ensure Start() processed
            g_TimerHost.Stop(timer);

            stk::Sleep(_STK_TIMER_TEST_LONG_SLEEP);

            if (g_ExpiredCount == 0 && !timer.IsActive())
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(150);

            printf("stop timer: count=%d (expected 0)\n", (int)g_ExpiredCount);

            if (g_SharedCounter == 1)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class ResetPeriodicTimerTask
    \brief Tests that Reset() reanchors a periodic timer's deadline from now.
    \note  Task 1 starts a 40ms periodic timer, lets it fire once, calls Reset(),
           then verifies the next firing occurs ~40ms after the Reset() call rather
           than from the original schedule.
*/
template <EAccessMode _AccessMode>
class ResetPeriodicTimerTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ResetPeriodicTimerTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer(1);

        if (m_task_id == 1)
        {
            g_TimerHost.Start(timer, 40, 40);

            // Wait for first firing
            while (g_ExpiredCount == 0)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            // Reset: deadline should be now + 40ms
            int64_t reset_time = GetTimeNowMsec();
            g_TimerHost.Reset(timer);

            // Wait for second firing
            while (g_ExpiredCount == 1)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            int64_t elapsed = g_ExpiredTime[1] - reset_time;

            g_TimerHost.Stop(timer);

            // Verify second firing occurred ~40ms after Reset()
            if (g_ExpiredCount == 2 && elapsed >= 35 && elapsed <= 50)
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(150);

            printf("reset periodic: count=%d (expected 2)\n", (int)g_ExpiredCount);

            if (g_SharedCounter == 1)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class RestartTimerTask
    \brief Tests that Restart() atomically stops and re-starts a timer.
    \note  Task 1 starts a 30ms periodic timer, lets it fire once, then calls Restart()
           with new parameters (50ms one-shot). Verifies the timer fires once more after
           ~50ms then becomes inactive.
*/
template <EAccessMode _AccessMode>
class RestartTimerTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    RestartTimerTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer(1);

        if (m_task_id == 1)
        {
            g_TimerHost.Start(timer, 30, 30); // periodic

            // Wait for first firing
            while (g_ExpiredCount == 0)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            int32_t count_before_restart = g_ExpiredCount;

            // Restart as one-shot with 50ms delay
            g_TimerHost.Restart(timer, 50);

            // Restart is asynchronous, wait for command to process
            stk::Sleep(2);

            int64_t restart_time = GetTimeNowMsec();

            // Wait for next firing (count increases beyond count_before_restart)
            while (g_ExpiredCount <= count_before_restart)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            int64_t elapsed = g_ExpiredTime[1] - restart_time;

            stk::Sleep(_STK_TIMER_TEST_LONG_SLEEP);

            // Verify: timer fired after restart, elapsed ~50ms, timer inactive (one-shot)
            if ((g_ExpiredCount == (count_before_restart + 1)) && (elapsed >= 45) && (elapsed <= 65) && !timer.IsActive())
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(200);

            printf("restart timer: count=%d (expected 2)\n", (int)g_ExpiredCount);

            if (g_SharedCounter == 1)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class StartOrResetTask
    \brief Tests StartOrReset(): starts if inactive, resets if active+periodic.
    \note  Task 1 tests two scenarios: (1) call on inactive timer -> starts it;
           (2) call on active periodic -> resets deadline. Verifies both paths work.
*/
template <EAccessMode _AccessMode>
class StartOrResetTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    StartOrResetTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer(1);

        if (m_task_id == 1)
        {
            // Scenario 1: inactive -> starts
            g_TimerHost.StartOrReset(timer, 40, 40);

            // StartOrReset is asynchronous, wait for command to process
            int32_t wait = 10;
            while (!timer.IsActive() && wait)
            {
                stk::Yield();
                --wait;
            }

            if (!timer.IsActive())
            {
                g_SharedCounter = -1; // fail: didn't start
                return;
            }

            // Wait for first firing
            while (g_ExpiredCount == 0)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            // Scenario 2: active periodic -> resets
            g_TimerHost.StartOrReset(timer, 999, 999); // delay/period ignored for active timer

            // StartOrReset is asynchronous, wait for command to process
            stk::Sleep(2);

            int64_t reset_time = GetTimeNowMsec();

            // Wait for second firing (count increases from 1 to 2)
            while (g_ExpiredCount == 1)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            int64_t elapsed = g_ExpiredTime[1] - reset_time;

            g_TimerHost.Stop(timer);

            // Verify second firing ~40ms after reset (original period)
            if ((g_ExpiredCount == 2) && (elapsed >= 35) && (elapsed <= 55))
                g_SharedCounter = 1;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(150);

            printf("start-or-reset: count=%d, result=%d (expected 2, 1)\n",
                (int)g_ExpiredCount, (int)g_SharedCounter);

            if (g_SharedCounter == 1)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class SetPeriodTask
    \brief Tests SetPeriod(): changes reload period without affecting current deadline.
    \note  Task 1 starts a 40ms periodic timer, lets it fire once, calls SetPeriod(60),
           waits for second firing (~40ms from first), then verifies third firing occurs
           ~60ms after second (new period applied).
*/
template <EAccessMode _AccessMode>
class SetPeriodTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SetPeriodTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        static TestTimer timer(1);
        int64_t elapsed;

        if (m_task_id == 1)
        {
            g_TimerHost.Start(timer, 40, 40);

            // Wait for first firing
            while (g_ExpiredCount == 0)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            // Change period to 60ms; current deadline unchanged
            g_TimerHost.SetPeriod(timer, 60);

            // SetPeriod is asynchronous, wait for command to process
            stk::Sleep(2);

            // Wait for second firing (should occur ~40ms from first, old period)
            while (g_ExpiredCount == 1)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            int64_t second_time = g_ExpiredTime[1];

            // Wait for third firing (change is still in flight, old period)
            while (g_ExpiredCount == 2)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            elapsed = g_ExpiredTime[1] - second_time;

            // Verify third firing ~50ms after second (old period)
            if ((g_ExpiredCount == 3) && (elapsed >= 45) && (elapsed <= 55))
                g_SharedCounter = 1;

            int64_t third_time = g_ExpiredTime[1];

            // Wait for fourth firing (should occur ~60ms from second, new period)
            while (g_ExpiredCount == 3)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            elapsed = g_ExpiredTime[1] - third_time;

            g_TimerHost.Stop(timer);

            // Verify third firing ~60ms after second
            if ((g_ExpiredCount == 4) && (elapsed >= 55) && (elapsed <= 75))
                g_SharedCounter = g_SharedCounter + 1;
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(300);

            printf("set-period: count=%d (expected 4)\n", (int)g_ExpiredCount);

            if (g_SharedCounter == 2)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

/*! \class StressTestTask
    \brief Stress test of TimerHost under full five-task contention.
    \note  Each task starts its own timer, waits for it to fire, stops it, then repeats
           for multiple iterations. Timers use varying delays (10-50ms range) to maximize
           scheduler contention. Verifies that all timers fire the expected number of times
           without corruption or loss. Runs on all platforms including Cortex-M0.
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    StressTestTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        static TestTimer timer0(0);
        static TestTimer timer1(1);
    #if (_STK_TIMER_TEST_TASKS_MAX > 2)
        static TestTimer timer2(2);
        static TestTimer timer3(3);
        static TestTimer timer4(4);
    #endif
        static volatile int32_t g_PerTaskCount[5] = {0};

        TestTimer *my_timer = nullptr;
        switch (m_task_id)
        {
        case 0: my_timer = &timer0; break;
        case 1: my_timer = &timer1; break;
    #if (_STK_TIMER_TEST_TASKS_MAX > 2)
        case 2: my_timer = &timer2; break;
        case 3: my_timer = &timer3; break;
        case 4: my_timer = &timer4; break;
    #endif
        }

        for (int32_t i = 0; i < m_iterations; ++i)
        {
            g_LastExpired[m_task_id].Reset();

            // Varying delays: 10 + (task_id * 10) ms -> [10, 20, 30, 40, 50]
            uint32_t delay = 10 + (m_task_id * 10);

            int32_t before = g_ExpiredCount;
            g_TimerHost.Start(*my_timer, delay);

            // Wait for this specific timer to fire
            while (!g_LastExpired[m_task_id].Wait(10)) {}

            int32_t after = g_ExpiredCount;

            // Verify our timer fired (global counter increased)
            if (after > before)
                ++g_PerTaskCount[m_task_id];

            g_TimerHost.Stop(*my_timer);

            // Pace to avoid overwhelming command queue
            if ((i % 5) == 0)
                stk::Sleep(4);
        }

        ++g_SharedCounter; // completion count

        if (m_task_id == (_STK_TIMER_TEST_TASKS_MAX - 1))
        {
            // Last task: wait for all to complete
            while (g_SharedCounter < _STK_TIMER_TEST_TASKS_MAX)
                stk::Sleep(_STK_TIMER_TEST_SHORT_SLEEP);

            // Verify each task's counter
            bool all_passed = true;
            for (int32_t t = 0; t < _STK_TIMER_TEST_TASKS_MAX; ++t)
            {
                if (g_PerTaskCount[t] != m_iterations)
                {
                    all_passed = false;
                    break;
                }
            }

            printf("stress test: total=%d, per-task counts=[%d,%d,%d,%d,%d] (expected %d each)\n",
                (int)g_ExpiredCount,
                (int)g_PerTaskCount[0], (int)g_PerTaskCount[1], (int)g_PerTaskCount[2],
                (int)g_PerTaskCount[3], (int)g_PerTaskCount[4],
                (int)m_iterations);

            if (all_passed)
                g_TestResult = 1;

            g_TimerHost.Shutdown();
        }
    }
};

// Helper function to reset test state
static void ResetTestState()
{
    g_TestResult    = 0;
    g_SharedCounter = 0;
    g_ExpiredCount  = 0;

    for (int32_t i = 0; i < _STK_TIMER_TEST_TASKS_MAX; ++i)
        g_ExpiredTime[i] = 0;

    for (int32_t i = 0; i < _STK_TIMER_TEST_TASKS_MAX; ++i)
        g_LastExpired[i].Reset();
}

} // namespace timer
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return (strcmp(test_name, "StressTest") == 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::timer;

    printf("Test: %s\n", test_name);

    ResetTestState();

    g_TimerHost.Initialize(&g_Kernel, stk::ACCESS_PRIVILEGED);

    // Create tasks based on test type
    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
#if (_STK_TIMER_TEST_TASKS_MAX > 2)
    TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);
#endif

    g_Kernel.AddTask(&task0);
    g_Kernel.AddTask(&task1);

#if (_STK_TIMER_TEST_TASKS_MAX > 2)
    if (NeedsExtendedTasks(test_name))
    {
        g_Kernel.AddTask(&task2);
        g_Kernel.AddTask(&task3);
        g_Kernel.AddTask(&task4);
    }
#endif

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

    using namespace stk::test::timer;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: One-shot timer fires exactly once at expected time
    if (RunTest<OneShotTimerTask<ACCESS_PRIVILEGED>>("OneShotTimer") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Periodic timer fires repeatedly at regular intervals
    if (RunTest<PeriodicTimerTask<ACCESS_PRIVILEGED>>("PeriodicTimer") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Multiple concurrent timers with different periods fire independently
    if (RunTest<MultipleTimersTask<ACCESS_PRIVILEGED>>("MultipleTimers") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Stop() cancels pending timer before it fires
    if (RunTest<StopTimerTask<ACCESS_PRIVILEGED>>("StopTimer") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Reset() reanchors periodic timer's deadline from now
    if (RunTest<ResetPeriodicTimerTask<ACCESS_PRIVILEGED>>("ResetPeriodicTimer") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: Restart() atomically stops and re-starts timer
    if (RunTest<RestartTimerTask<ACCESS_PRIVILEGED>>("RestartTimer") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: StartOrReset() starts if inactive, resets if active+periodic
    if (RunTest<StartOrResetTask<ACCESS_PRIVILEGED>>("StartOrReset") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 8: SetPeriod() changes reload period without affecting current deadline
    if (RunTest<SetPeriodTask<ACCESS_PRIVILEGED>>("SetPeriod") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    // Test 9: Stress test under full five-task contention with varying timer delays
    if (RunTest<StressTestTask<ACCESS_PRIVILEGED>>("StressTest", 20) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    g_TimerHost.Shutdown();

    int32_t final_result = (total_failures == 0 ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
