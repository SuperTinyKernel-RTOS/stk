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
#include <sync/stk_sync_event.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_EVT_TEST_TASKS_MAX   5
#define _STK_EVT_TEST_TIMEOUT     300
#define _STK_EVT_TEST_SHORT_SLEEP 10
#define _STK_EVT_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_EVT_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_EVT_STACK_SIZE       256
#define STK_TASK                  static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::event
    \brief     Namespace of Event test.
 */
namespace event {

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_AcquisitionOrder[_STK_EVT_TEST_TASKS_MAX] = {0};
static volatile int32_t g_OrderIndex = 0;
static volatile bool    g_TestComplete = false;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_EVT_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test event (re-constructed per test via ResetTestState)
static sync::Event g_TestEvent;

/*! \class AutoResetBasicTask
    \brief Tests auto-reset event: Set() wakes exactly one waiting task then resets.
    \note  Verifies that after Set() the event returns to non-signaled state so that
           subsequent Wait() calls block. Only one of the four waiting tasks must be
           woken per Set(); the total woken count must equal the number of Set() calls.
*/
template <EAccessMode _AccessMode>
class AutoResetBasicTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    AutoResetBasicTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: producer - fire one Set() per iteration; each must wake exactly one consumer
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP); // let consumers block first

            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestEvent.Set();
                stk::Delay(1); // pace so consumer can unblock between signals
            }

            stk::Sleep(_STK_EVT_TEST_LONG_SLEEP);

            printf("auto-reset basic: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)m_iterations);

            if (g_SharedCounter == m_iterations)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: consumers - each loops waiting for the event
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                if (g_TestEvent.Wait(_STK_EVT_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }
        }
    }
};

/*! \class ManualResetBasicTask
    \brief Tests manual-reset event: Set() wakes all waiting tasks and state stays signaled.
    \note  Verifies that one Set() releases all four consumer tasks and that the event
           remains signaled (so a late-arriving Wait() on the still-set event also succeeds),
           and that Reset() then clears the state.
*/
template <EAccessMode _AccessMode>
class ManualResetBasicTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ManualResetBasicTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: let all consumers block, then fire a single Set()
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP);

            g_TestEvent.Set();

            stk::Sleep(_STK_EVT_TEST_LONG_SLEEP);

            // All 4 consumers must have woken; event still signaled before Reset()
            bool still_signaled = g_TestEvent.TryWait(); // fast-path check, does NOT reset (manual-reset)
            g_TestEvent.Reset();

            printf("manual-reset basic: counter=%d (expected %d), still_signaled=%d (expected 1)\n",
                (int)g_SharedCounter, (int)(_STK_EVT_TEST_TASKS_MAX - 1), (int)still_signaled);

            if ((g_SharedCounter == (_STK_EVT_TEST_TASKS_MAX - 1)) && still_signaled)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: each waits for the single Set(); all should be released
            if (g_TestEvent.Wait(_STK_EVT_TEST_TIMEOUT))
                ++g_SharedCounter;
        }
    }
};

/*! \class InitialStateTask
    \brief Tests event constructed with initial_state=true.
    \note  Verifies that Wait() returns immediately on the fast path without any
           preceding Set(), and that the auto-reset clears the state so the next
           Wait() blocks as expected.
*/
template <EAccessMode _AccessMode>
class InitialStateTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    InitialStateTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // First Wait() must succeed immediately (initial_state=true, auto-reset clears it)
            if (g_TestEvent.Wait(_STK_EVT_TEST_SHORT_SLEEP))
                ++g_SharedCounter;

            // Second Wait() must time out: event was auto-reset by the first Wait()
            bool second = g_TestEvent.Wait(_STK_EVT_TEST_SHORT_SLEEP);

            if (!second)
                ++g_SharedCounter;
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EVT_TEST_LONG_SLEEP);

            printf("initial state: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class TimeoutWaitTask
    \brief Tests Wait() timeout behavior.
    \note  Verifies that Wait() returns false within the expected time window when
           no Set() is called, and that a subsequent Wait() succeeds once Set() fires.
*/
template <EAccessMode _AccessMode>
class TimeoutWaitTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimeoutWaitTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: withhold Set() well past the timeout window, then fire it
            stk::Sleep(200);
            g_TestEvent.Set();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: Wait with 50-tick timeout; must expire before task 0 fires Set()
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP);

            int64_t start   = GetTimeNowMsec();
            bool acquired   = g_TestEvent.Wait(50);
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!acquired && elapsed >= 45 && elapsed <= 60)
                ++g_SharedCounter;

            if (acquired)
                g_TestEvent.Reset(); // return state to non-signaled if unexpectedly acquired
        }
        else
        if (m_task_id == 2)
        {
            // Task 2: Wait with generous timeout after task 0 fires Set()
            stk::Sleep(210);

            if (g_TestEvent.Wait(100))
                ++g_SharedCounter;
        }

        if (m_task_id == 2)
        {
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP);

            printf("timeout wait: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class TryWaitTask
    \brief Tests TryWait() non-blocking poll behavior.
    \note  Verifies that TryWait() returns false immediately on a non-signaled event,
           and returns true (and auto-resets) when the event is already signaled.
*/
template <EAccessMode _AccessMode>
class TryWaitTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryWaitTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // TryWait on a non-signaled event must return false immediately
            int64_t start   = GetTimeNowMsec();
            bool acquired   = g_TestEvent.TryWait();
            int64_t elapsed = GetTimeNowMsec() - start;

            if (!acquired && elapsed < _STK_EVT_TEST_SHORT_SLEEP)
                ++g_SharedCounter;
        }
        else
        if (m_task_id == 2)
        {
            // Signal the event, then TryWait must return true and auto-reset it
            g_TestEvent.Set();

            int64_t start   = GetTimeNowMsec();
            bool acquired   = g_TestEvent.TryWait();
            int64_t elapsed = GetTimeNowMsec() - start;

            if (acquired && elapsed < _STK_EVT_TEST_SHORT_SLEEP)
                ++g_SharedCounter;

            // Verify auto-reset: a second TryWait must now return false
            if (!g_TestEvent.TryWait())
                ++g_SharedCounter;
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EVT_TEST_LONG_SLEEP);

            printf("try-wait: counter=%d (expected 3)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 3)
                g_TestResult = 1;
        }
    }
};

/*! \class ResetManualTask
    \brief Tests Reset() on a manual-reset event.
    \note  Verifies that Reset() clears the signaled state and that subsequent Wait()
           calls block correctly; also verifies Reset() return value (true on actual
           state change, false if already non-signaled).
*/
template <EAccessMode _AccessMode>
class ResetManualTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ResetManualTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Set the event (manual-reset), verify state, then Reset() and verify block
            bool set_changed   = g_TestEvent.Set();
            bool reset_changed = g_TestEvent.Reset();
            bool reset_again   = g_TestEvent.Reset(); // already non-signaled: must return false

            // Both tasks 1 and 2 were sleeping during Set/Reset; they should now time out
            stk::Sleep(_STK_EVT_TEST_LONG_SLEEP);

            printf("reset manual: set_changed=%d (expected 1), reset_changed=%d (expected 1), "
                "reset_again=%d (expected 0), counter=%d (expected 0)\n",
                (int)set_changed, (int)reset_changed, (int)reset_again, (int)g_SharedCounter);

            if (set_changed && reset_changed && !reset_again && (g_SharedCounter == 0))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Wait after Reset(); must time out because event was reset before we got here
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP); // ensure task 0 has Set+Reset'd by now

            bool acquired = g_TestEvent.Wait(_STK_EVT_TEST_SHORT_SLEEP);

            if (!acquired)
                ; // expected; do not count
            else
                ++g_SharedCounter; // unexpected wake counts as a failure
        }
        else
        if (m_task_id == 2)
        {
            // Same as task 1: must also time out
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP);

            bool acquired = g_TestEvent.Wait(_STK_EVT_TEST_SHORT_SLEEP);

            if (acquired)
                ++g_SharedCounter; // unexpected wake counts as a failure
        }
    }
};

/*! \class PulseAutoResetTask
    \brief Tests Pulse() on an auto-reset event.
    \note  Verifies that Pulse() releases exactly one waiting task then resets the
           event to non-signaled regardless of whether a waiter was present.
*/
template <EAccessMode _AccessMode>
class PulseAutoResetTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    PulseAutoResetTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: one Pulse() per iteration wakes exactly one consumer
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP); // let consumers block first

            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestEvent.Pulse();
                stk::Delay(1);
            }

            stk::Sleep(_STK_EVT_TEST_LONG_SLEEP);

            // Pulse with no waiters must leave event non-signaled
            g_TestEvent.Pulse(); // no one waiting
            bool still_signaled = g_TestEvent.TryWait();

            printf("pulse auto-reset: counter=%d (expected %d), still_signaled=%d (expected 0)\n",
                (int)g_SharedCounter, (int)m_iterations, (int)still_signaled);

            if ((g_SharedCounter == m_iterations) && !still_signaled)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: consume Pulse events; each loop iteration waits for one pulse
            int32_t share = m_iterations / (_STK_EVT_TEST_TASKS_MAX - 1);

            for (int32_t i = 0; i < share; ++i)
            {
                if (g_TestEvent.Wait(_STK_EVT_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }
        }
    }
};

/*! \class PulseManualResetTask
    \brief Tests Pulse() on a manual-reset event.
    \note  Verifies that Pulse() releases all waiting tasks and always resets the event
           to non-signaled, even if no tasks are waiting at the time of the call.
*/
template <EAccessMode _AccessMode>
class PulseManualResetTask : public Task<_STK_EVT_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PulseManualResetTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Let all consumers block, then fire a single Pulse()
            stk::Sleep(_STK_EVT_TEST_SHORT_SLEEP);

            g_TestEvent.Pulse(); // must wake all 4 waiters and reset

            stk::Sleep(_STK_EVT_TEST_LONG_SLEEP);

            // Event must be non-signaled after Pulse()
            bool still_signaled = g_TestEvent.TryWait();

            // Pulse() with no waiters must also leave event non-signaled
            g_TestEvent.Pulse();
            bool after_empty_pulse = g_TestEvent.TryWait();

            printf("pulse manual-reset: counter=%d (expected %d), "
                "still_signaled=%d (expected 0), after_empty_pulse=%d (expected 0)\n",
                (int)g_SharedCounter, (int)(_STK_EVT_TEST_TASKS_MAX - 1),
                (int)still_signaled, (int)after_empty_pulse);

            if ((g_SharedCounter == (_STK_EVT_TEST_TASKS_MAX - 1)) &&
                !still_signaled && !after_empty_pulse)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: all block and must all be released by the single Pulse()
            if (g_TestEvent.Wait(_STK_EVT_TEST_TIMEOUT))
                ++g_SharedCounter;
        }
    }
};

// Helper function to reset test state
static void ResetTestState(bool manual_reset = false, bool initial_state = false)
{
    g_TestResult    = 0;
    g_SharedCounter = 0;
    g_OrderIndex    = 0;
    g_TestComplete  = false;

    for (int32_t i = 0; i < _STK_EVT_TEST_TASKS_MAX; ++i)
        g_AcquisitionOrder[i] = 0;

    // Re-construct the event in-place with the requested mode and initial state
    g_TestEvent.~Event();
    new (&g_TestEvent) sync::Event(manual_reset, initial_state);
}

} // namespace event
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return  (strcmp(test_name, "InitialState")  != 0) &&
            (strcmp(test_name, "TimeoutWait")   != 0) &&
            (strcmp(test_name, "TryWait")       != 0) &&
            (strcmp(test_name, "ResetManual")   != 0);
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0,
    bool manual_reset = false, bool initial_state = false)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::event;

    printf("Test: %s\n", test_name);

    ResetTestState(manual_reset, initial_state);

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

    using namespace stk::test::event;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

    // Test 1: Auto-reset event wakes exactly one waiter per Set()
    if (RunTest<AutoResetBasicTask<ACCESS_PRIVILEGED>>("AutoResetBasic", 20) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#ifndef __ARM_ARCH_6M__

    // Test 2: Manual-reset event wakes all waiters and stays signaled until Reset()
    if (RunTest<ManualResetBasicTask<ACCESS_PRIVILEGED>>("ManualResetBasic", 0, true) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: initial_state=true provides immediate fast-path Wait(), then auto-resets (tasks 0-2 only)
    if (RunTest<InitialStateTask<ACCESS_PRIVILEGED>>("InitialState", 0, false, true) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Wait() times out correctly when no Set() fires (tasks 0-2 only)
    if (RunTest<TimeoutWaitTask<ACCESS_PRIVILEGED>>("TimeoutWait") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: TryWait() returns immediately and auto-resets on success (tasks 0-2 only)
    if (RunTest<TryWaitTask<ACCESS_PRIVILEGED>>("TryWait") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: Reset() clears manual-reset event; return value reflects actual state change (tasks 0-2 only)
    if (RunTest<ResetManualTask<ACCESS_PRIVILEGED>>("ResetManual", 0, true) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Pulse() on auto-reset event wakes one waiter and always resets
    if (RunTest<PulseAutoResetTask<ACCESS_PRIVILEGED>>("PulseAutoReset", 20) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 8: Pulse() on manual-reset event wakes all waiters and always resets
    if (RunTest<PulseManualResetTask<ACCESS_PRIVILEGED>>("PulseManualReset", 0, true) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#endif // __ARM_ARCH_6M__

    int32_t final_result = (total_failures == 0 ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
