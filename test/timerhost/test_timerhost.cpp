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
        g_ExpiredTime[m_timer_id] = GetTimeNowMs();
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
            int64_t start = GetTimeNowMs();
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
            int64_t reset_time = GetTimeNowMs();
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

            int64_t restart_time = GetTimeNowMs();

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

            int64_t reset_time = GetTimeNowMs();

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

#ifndef __ARM_ARCH_6M__

/*! \enum  EHostInit
    \brief How RunTest() initializes g_TimerHost (selected by a test case before RunTest(), see InitHost()).
*/
enum EHostInit
{
    HOST_TASKS_DEFAULT = 0, //!< Initialize(kernel, mode) before the user tasks are added (all pre-existing tests)
    HOST_TASKS_CONFIG,      //!< Initialize(kernel, mode, Config) after the user tasks are added, Config is built by g_ConfigParam
    HOST_POLLED             //!< InitializePolled(), no host tasks
};

static volatile EHostInit  g_HostInit    = HOST_TASKS_DEFAULT;
static volatile int32_t    g_ConfigParam = 0;      // scenario of ConfigTask
static volatile int32_t    g_PolledParam = 0;      // scenario of PolledTask
static volatile EAddResult g_InitResult  = ADD_OK; // result of Initialize() made by InitHost()

// Initialize(Config) scenarios (see ConfigTask)
static const int32_t CF_DEFAULT_CFG       = 1;
static const int32_t CF_WEIGHTS           = 2;
static const int32_t CF_EXT_STACKS        = 3;
static const int32_t CF_EXT_TICK_STACK    = 4;
static const int32_t CF_EXT_HANDLER_STACK = 5;
static const int32_t CF_DUP_STACK         = 6;

// polled mode scenarios (see PolledTask)
static const int32_t PL_ONESHOT_INLINE    = 1;
static const int32_t PL_PERIODIC          = 2;
static const int32_t PL_DEFERRED_COMMANDS = 3;
static const int32_t PL_RESTART           = 4;
static const int32_t PL_RESET             = 5;
static const int32_t PL_RETURN_VALUE      = 6;
static const int32_t PL_OVERDUE           = 7;
static const int32_t PL_SELF_REARM        = 8;
static const int32_t PL_SHUTDOWN_REUSE    = 9;
static const int32_t PL_IDLE_NO_BLOCK     = 10;

// external stack buffers for the Config scenarios
typedef StackMemoryDef<time::TimerHost::TASK_TICK_MEMORY_SIZE>::Type   ExtTickStack;
typedef StackMemoryDef<time::TimerHost::TASK_HANDLER_STACK_SIZE>::Type ExtHandlerStack;

static ExtTickStack    g_ExtTickStack;
static ExtHandlerStack g_ExtHandlerStack[STK_TIMER_THREADS_COUNT];

// polled mode state
static volatile bool    g_InProcess   = false; // true while the poller is inside TimerHost::Process()
static volatile int32_t g_InlineCount = 0;     // number of OnExpired() calls made while g_InProcess was set
static sync::Event      g_PolledDone;

/*! \class PolledTimer
    \brief TestTimer which also counts the callbacks executed inline from TimerHost::Process().
*/
class PolledTimer : public TestTimer
{
public:
    PolledTimer(uint8_t timer_id) : TestTimer(timer_id)
    {}

    void OnExpired(time::TimerHost *host)
    {
        TestTimer::OnExpired(host);

        if (g_InProcess)
            ++g_InlineCount;
    }
};

/*! \class ReArmTimer
    \brief PolledTimer which re-starts itself (one-shot, 20 ticks) from its own callback until \a limit expirations.
*/
class ReArmTimer : public PolledTimer
{
    int32_t m_limit;

public:
    ReArmTimer(uint8_t timer_id, int32_t limit) : PolledTimer(timer_id), m_limit(limit)
    {}

    void OnExpired(time::TimerHost *host)
    {
        PolledTimer::OnExpired(host);

        if (g_ExpiredCount < m_limit)
            host->Start(*this, 20);
    }
};

static PolledTimer g_PolledTimer1(1);
static PolledTimer g_PolledTimer2(2);
static ReArmTimer  g_PolledReArm(3, 3);
static TestTimer   g_ConfigTimer(1);

/*! \class StackProbeTask
    \brief Minimal task which owns no stack but reports an existing buffer as its stack (never runs).
*/
class StackProbeTask : public ITask
{
    const Word *m_stack;
    size_t      m_size;

public:
    StackProbeTask(const void *stack, size_t size_words)
        : m_stack(static_cast<const Word *>(stack)), m_size(size_words)
    {}

    EAccessMode GetAccessMode() const override { return ACCESS_PRIVILEGED; }
    void OnDeadlineMissed(uint32_t)   override {}
    void OnExit()                     override {}
    Weight GetWeight()          const override { return DEFAULT_WEIGHT; }
    const char *GetTraceName()  const override { return nullptr; }
    const Word *GetStack()      const override { return m_stack; }
    size_t GetStackSize()       const override { return m_size; }

private:
    void Run() override {}
};

/*! \fn    IsStackRegistered
    \brief Check that a stack buffer is held by a task of the kernel: the kernel rejects a second task with
           the same stack (ADD_FAIL_DUPLICATE). Works on every platform, regardless of how a port initializes stacks.
    \note  If the buffer is NOT held by any task the probe is added to the kernel and the test fails.
*/
static bool IsStackRegistered(const void *stack, size_t size_words)
{
    StackProbeTask probe(stack, size_words);

    return (g_Kernel.AddTask(&probe) == ADD_FAIL_DUPLICATE);
}

static void SetExtTickStack(time::TimerHost::Config &cfg)
{
    cfg.tick_stack      = reinterpret_cast<Word *>(&g_ExtTickStack);
    cfg.tick_stack_size = time::TimerHost::TASK_TICK_MEMORY_SIZE;
}

static void SetExtHandlerStacks(time::TimerHost::Config &cfg)
{
    for (size_t i = 0; i < STK_TIMER_THREADS_COUNT; ++i)
        cfg.handler_stack[i] = reinterpret_cast<Word *>(&g_ExtHandlerStack[i]);

    cfg.handler_stack_size = time::TimerHost::TASK_HANDLER_STACK_SIZE;
}

/*! \fn    InitHost
    \brief Initialize g_TimerHost the way the current test case (g_HostInit, g_ConfigParam) requires.
    \param[in] user_task: A user task which is already added to the kernel (CF_DUP_STACK borrows its stack).
    \note  The result of Initialize() is stored in g_InitResult.
*/
static void InitHost(const ITask *user_task)
{
    if (g_HostInit == HOST_POLLED)
    {
        g_TimerHost.InitializePolled();
        g_InitResult = ADD_OK;
        return;
    }

    time::TimerHost::Config cfg; // defaults: DEFAULT_WEIGHT, internal stacks

    switch (g_ConfigParam)
    {
    case CF_WEIGHTS:
        // SwitchStrategyRR ignores the weights: they must be accepted and forwarded without any side effect
        cfg.tick_weight    = 3;
        cfg.handler_weight = 2;
        break;
    case CF_EXT_STACKS:
        SetExtTickStack(cfg);
        SetExtHandlerStacks(cfg);
        break;
    case CF_EXT_TICK_STACK:
        SetExtTickStack(cfg);
        break;
    case CF_EXT_HANDLER_STACK:
        SetExtHandlerStacks(cfg);
        break;
    case CF_DUP_STACK:
        // the handler stack collides with the stack of the user task which is already in the kernel
        cfg.handler_stack[0]   = const_cast<Word *>(user_task->GetStack());
        cfg.handler_stack_size = user_task->GetStackSize();
        break;
    default: // CF_DEFAULT_CFG
        break;
    }

    g_InitResult = g_TimerHost.Initialize(&g_Kernel, ACCESS_PRIVILEGED, cfg);
}

/*! \class ConfigTask
    \brief Tests TimerHost::Initialize() with TimerHost::Config: result code, weights and external stack buffers.
    \note  Task 1 starts a 30ms one-shot timer and waits for it, task 0 evaluates the scenario selected by
           g_ConfigParam: 1 = explicit default Config (ADD_OK, timer fires), 2 = custom weights are accepted
           and forwarded (RR ignores them), 3 = external stacks for the tick and handler tasks are used
           (the kernel holds the buffers: a probe task with the same stack is rejected with ADD_FAIL_DUPLICATE),
           4 = external tick stack only (handler tasks keep the
           internal buffers), 5 = external handler stacks only, 6 = a stack which collides with the stack of
           a user task makes Initialize() fail with ADD_FAIL_DUPLICATE and no host task stays registered (the
           kernel would never stop otherwise, because a handler task would wait for timers forever).
*/
template <EAccessMode _AccessMode>
class ConfigTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ConfigTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            if (g_InitResult == ADD_OK)
            {
                g_TimerHost.Start(g_ConfigTimer, 30);

                if (g_LastExpired[1].Wait(500) && (g_ExpiredCount == 1))
                    g_SharedCounter = 1;
            }
            else
            {
                g_SharedCounter = 1; // no host to talk to, task 0 checks the result code
            }
        }
        else
        if (m_task_id == 0)
        {
            stk::Sleep(150);

            const int32_t scenario  = g_ConfigParam;
            const EAddResult expect = (scenario == CF_DUP_STACK ? ADD_FAIL_DUPLICATE : ADD_OK);

            bool ok = (g_InitResult == expect) && (g_SharedCounter == 1);

            if ((scenario == CF_EXT_STACKS) || (scenario == CF_EXT_TICK_STACK))
                ok = ok && IsStackRegistered(&g_ExtTickStack, time::TimerHost::TASK_TICK_MEMORY_SIZE);

            if ((scenario == CF_EXT_STACKS) || (scenario == CF_EXT_HANDLER_STACK))
            {
                for (size_t i = 0; i < STK_TIMER_THREADS_COUNT; ++i)
                    ok = ok && IsStackRegistered(&g_ExtHandlerStack[i], time::TimerHost::TASK_HANDLER_STACK_SIZE);
            }

            printf("config %d: init=%d (expected %d), fired=%d (expected %d), result=%d (expected 1)\n",
                (int)scenario, (int)g_InitResult, (int)expect, (int)g_ExpiredCount,
                (int)(scenario == CF_DUP_STACK ? 0 : 1), (int)ok);

            if (ok)
                g_TestResult = 1;

            if (g_InitResult == ADD_OK)
                g_TimerHost.Shutdown();
        }
    }
};

// Polled mode helpers (task 1 of PolledTask is the only caller of TimerHost::Process())

static Timeout PollOnce()
{
    g_InProcess = true;
    const Timeout next = g_TimerHost.Process();
    g_InProcess = false;

    return next;
}

static void PollFor(int32_t ms)
{
    const int64_t end = GetTimeNowMs() + ms;

    while (GetTimeNowMs() < end)
    {
        PollOnce();
        stk::Sleep(2);
    }
}

static bool PollUntilCount(int32_t count, int32_t timeout_ms)
{
    const int64_t end = GetTimeNowMs() + timeout_ms;

    while (g_ExpiredCount < count)
    {
        if (GetTimeNowMs() >= end)
            return false;

        PollOnce();
        stk::Sleep(2);
    }

    return true;
}

static bool PolledOneShotInline()
{
    const int64_t start = GetTimeNowMs();

    g_TimerHost.Start(g_PolledTimer1, 50);
    PollFor(150);

    const int64_t elapsed = g_ExpiredTime[1] - start;

    // exactly one expiration, executed inline from Process(), on time (polled every 2 ticks), one-shot is inactive
    return (g_ExpiredCount == 1) && (g_InlineCount == 1) && (elapsed >= 45) && (elapsed <= 70) &&
        !g_PolledTimer1.IsActive();
}

static bool PolledPeriodic()
{
    g_TimerHost.Start(g_PolledTimer1, 30, 30);
    PollFor(150);

    g_TimerHost.Stop(g_PolledTimer1);
    PollFor(10); // Stop() is applied by the next Process()

    const int32_t count = g_ExpiredCount;

    PollFor(80); // must stay quiet after Stop()

    return (count >= 4) && (count <= 6) && (g_ExpiredCount == count) && (g_InlineCount == count) &&
        !g_PolledTimer1.IsActive();
}

static bool PolledDeferredCommands()
{
    g_TimerHost.Start(g_PolledTimer1, 100);
    const bool before_start = g_PolledTimer1.IsActive(); // command is not processed yet

    PollOnce();
    const bool after_start = g_PolledTimer1.IsActive();

    g_TimerHost.Stop(g_PolledTimer1);
    const bool before_stop = g_PolledTimer1.IsActive(); // still pending in the command queue

    PollOnce();
    const bool after_stop = g_PolledTimer1.IsActive();

    PollFor(150);

    return !before_start && after_start && before_stop && !after_stop && (g_ExpiredCount == 0);
}

static bool PolledRestart()
{
    g_TimerHost.Start(g_PolledTimer1, 30, 30); // periodic

    if (!PollUntilCount(1, 200))
        return false;

    const int32_t before     = g_ExpiredCount;
    const int64_t restart_at = GetTimeNowMs();

    g_TimerHost.Restart(g_PolledTimer1, 50); // one-shot

    if (!PollUntilCount(before + 1, 200))
        return false;

    const int64_t elapsed = g_ExpiredTime[1] - restart_at;

    PollFor(80); // one-shot: stays quiet

    return (elapsed >= 45) && (elapsed <= 65) && (g_ExpiredCount == (before + 1)) && !g_PolledTimer1.IsActive();
}

static bool PolledReset()
{
    g_TimerHost.Start(g_PolledTimer1, 40, 40);

    if (!PollUntilCount(1, 200))
        return false;

    const int64_t reset_at = GetTimeNowMs();

    g_TimerHost.Reset(g_PolledTimer1);

    if (!PollUntilCount(2, 200))
        return false;

    const int64_t elapsed = g_ExpiredTime[1] - reset_at;

    g_TimerHost.Stop(g_PolledTimer1);
    PollFor(10);

    // the second expiration is ~40 ticks after Reset(), not on the original schedule
    return (elapsed >= 35) && (elapsed <= 55) && (g_ExpiredCount == 2) && !g_PolledTimer1.IsActive();
}

static bool PolledReturnValue()
{
    bool ok = (PollOnce() == WAIT_INFINITE); // nothing is active

    g_TimerHost.Start(g_PolledTimer1, 200);
    g_TimerHost.Start(g_PolledTimer2, 60);

    const Timeout earliest = PollOnce(); // the earliest deadline wins
    ok = ok && (earliest >= 50) && (earliest <= 60);

    g_TimerHost.Stop(g_PolledTimer2);

    const Timeout remaining = PollOnce(); // Stop() is applied, only the 200 tick timer is left
    ok = ok && (remaining > 100) && (remaining <= 200);

    g_TimerHost.Stop(g_PolledTimer1);

    ok = ok && (PollOnce() == WAIT_INFINITE); // nothing is active again

    return ok && (g_ExpiredCount == 0);
}

static bool PolledOverdue()
{
    g_TimerHost.Start(g_PolledTimer1, 10, 10);

    stk::Sleep(55); // not polled: the timer is overdue by several periods

    int32_t calls = 0;
    bool once_per_call = true;
    Timeout next;

    do
    {
        const int32_t before = g_ExpiredCount;

        next = PollOnce();

        // an overdue periodic timer fires once per call until its schedule has caught up
        if ((g_ExpiredCount - before) > 1)
            once_per_call = false;

        ++calls;
    }
    while ((next == NO_WAIT) && (calls < 20));

    const int32_t count = g_ExpiredCount;

    g_TimerHost.Stop(g_PolledTimer1);
    PollOnce();

    return once_per_call && (calls >= 4) && (count >= 4) && (count <= 7) && (next > 0) && (next <= 10);
}

static bool PolledSelfReArm()
{
    // the callback re-starts the one-shot timer: the command is applied by the next Process()
    g_TimerHost.Start(g_PolledReArm, 20);
    PollFor(200);

    return (g_ExpiredCount == 3) && (g_InlineCount == 3) && !g_PolledReArm.IsActive();
}

static bool PolledShutdownReuse()
{
    g_TimerHost.Start(g_PolledTimer1, 30, 30);
    g_TimerHost.Start(g_PolledTimer2, 40, 40);
    PollFor(5);

    const size_t active = g_TimerHost.GetSize();

    g_TimerHost.Shutdown();
    PollOnce();

    const bool stopped = (g_TimerHost.GetSize() == 0) && !g_PolledTimer1.IsActive() && !g_PolledTimer2.IsActive();

    PollFor(100); // nothing fires after Shutdown()

    const int32_t quiet = g_ExpiredCount;

    g_TimerHost.Start(g_PolledTimer1, 20); // there are no tasks to exit, the host stays usable
    PollFor(60);

    return (active == 2) && stopped && (quiet == 0) && (g_ExpiredCount == 1);
}

static bool PolledIdleNoBlock()
{
    const int64_t start = GetTimeNowMs();
    bool ok = true;

    // without active timers and commands Process() must return at once (a blocking read would hang here)
    for (int32_t i = 0; i < 3; ++i)
        ok = ok && (PollOnce() == WAIT_INFINITE);

    return ok && ((GetTimeNowMs() - start) < 30);
}

static bool RunPolledScenario(int32_t scenario)
{
    switch (scenario)
    {
    case PL_ONESHOT_INLINE:    return PolledOneShotInline();
    case PL_PERIODIC:          return PolledPeriodic();
    case PL_DEFERRED_COMMANDS: return PolledDeferredCommands();
    case PL_RESTART:           return PolledRestart();
    case PL_RESET:             return PolledReset();
    case PL_RETURN_VALUE:      return PolledReturnValue();
    case PL_OVERDUE:           return PolledOverdue();
    case PL_SELF_REARM:        return PolledSelfReArm();
    case PL_SHUTDOWN_REUSE:    return PolledShutdownReuse();
    case PL_IDLE_NO_BLOCK:     return PolledIdleNoBlock();
    default:                   return false;
    }
}

/*! \class PolledTask
    \brief Tests the polled mode: TimerHost::InitializePolled() and TimerHost::Process().
    \note  No host tasks exist in this mode. Task 1 is the only caller of Process() and runs the scenario
           selected by g_PolledParam: 1 = one-shot timer fires once, inline from Process(); 2 = periodic timer
           fires regularly and stays quiet after Stop(); 3 = commands are deferred until the next Process();
           4 = Restart() re-arms a periodic timer as one-shot; 5 = Reset() re-anchors the deadline;
           6 = the return value of Process() (WAIT_INFINITE / ticks to the earliest deadline); 7 = an overdue
           periodic timer fires once per call until it has caught up (returns NO_WAIT meanwhile); 8 = a callback
           can re-start its own one-shot timer; 9 = Shutdown() stops all timers and the host stays usable;
           10 = Process() never blocks when nothing is active. Task 0 only waits for the scenario to complete.
           The scenario always ends with Shutdown() and one more Process() which releases all timers, because
           g_TimerHost is shared with the following tests.
*/
template <EAccessMode _AccessMode>
class PolledTask : public Task<_STK_TIMER_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PolledTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            if (RunPolledScenario(g_PolledParam))
                g_SharedCounter = 1;

            g_TimerHost.Shutdown();
            PollOnce();

            g_PolledDone.Set();
        }
        else
        if (m_task_id == 0)
        {
            const bool done = g_PolledDone.Wait(3000);

            printf("polled %d: done=%d, count=%d, inline=%d, result=%d (expected 1)\n",
                (int)g_PolledParam, (int)done, (int)g_ExpiredCount, (int)g_InlineCount, (int)g_SharedCounter);

            if (done && (g_SharedCounter == 1))
                g_TestResult = 1;
        }
    }
};

#endif // __ARM_ARCH_6M__

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

#ifndef __ARM_ARCH_6M__
    g_InProcess   = false;
    g_InlineCount = 0;
    g_InitResult  = ADD_OK;
    g_PolledDone.Reset();
#endif
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

    // test cases of the Config and polled groups select their own host initialization (see InitHost()),
    // it follows AddTask() of the user tasks, all other tests initialize the host before it
#ifndef __ARM_ARCH_6M__
    const bool custom_init = (g_HostInit != HOST_TASKS_DEFAULT);
#else
    const bool custom_init = false;
#endif

    if (!custom_init)
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

#ifndef __ARM_ARCH_6M__
    if (custom_init)
        InitHost(&task0);
#endif

    g_Kernel.Start();

#ifndef __ARM_ARCH_6M__
    g_HostInit = HOST_TASKS_DEFAULT; // the following tests use the default initialization again
#endif

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

    // Tests 9-14: Initialize() with Config: result code, weights and external stack buffers
    {
        struct ConfigCase { const char *name; int32_t param; };

        static const ConfigCase cases[] =
        {
            { "Config_Default",          CF_DEFAULT_CFG       },
            { "Config_Weights",          CF_WEIGHTS           },
            { "Config_ExtStacks",        CF_EXT_STACKS        },
            { "Config_ExtTickStack",     CF_EXT_TICK_STACK    },
            { "Config_ExtHandlerStack",  CF_EXT_HANDLER_STACK },
            { "Config_DuplicateStack",   CF_DUP_STACK         }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_HostInit    = HOST_TASKS_CONFIG;
            g_ConfigParam = cases[i].param;

            if (RunTest<ConfigTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param) != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

    // Tests 15-24: polled mode, InitializePolled() and Process() (no host tasks)
    {
        struct PolledCase { const char *name; int32_t param; };

        static const PolledCase cases[] =
        {
            { "Polled_OneShotInline",    PL_ONESHOT_INLINE    },
            { "Polled_Periodic",         PL_PERIODIC          },
            { "Polled_DeferredCommands", PL_DEFERRED_COMMANDS },
            { "Polled_Restart",          PL_RESTART           },
            { "Polled_Reset",            PL_RESET             },
            { "Polled_ReturnValue",      PL_RETURN_VALUE      },
            { "Polled_Overdue",          PL_OVERDUE           },
            { "Polled_SelfReArm",        PL_SELF_REARM        },
            { "Polled_ShutdownReuse",    PL_SHUTDOWN_REUSE    },
            { "Polled_IdleNoBlock",      PL_IDLE_NO_BLOCK     }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_HostInit    = HOST_POLLED;
            g_PolledParam = cases[i].param;

            if (RunTest<PolledTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param) != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

#endif // __ARM_ARCH_6M__

    // Test 25: Stress test under full five-task contention with varying timer delays (task mode again after the polled tests)
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
