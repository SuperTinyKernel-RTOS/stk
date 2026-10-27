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
#include <sync/stk_sync_eventflags.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_EF_TEST_TASKS_MAX   5
#define _STK_EF_TEST_TIMEOUT     300
#define _STK_EF_TEST_SHORT_SLEEP 10
#define _STK_EF_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_EF_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_EF_STACK_SIZE       256
#define STK_TASK                 static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

// Shared flag bit definitions used across all tests
static const uint32_t FLAG_A = (1U << 0);
static const uint32_t FLAG_B = (1U << 1);
static const uint32_t FLAG_C = (1U << 2);
static const uint32_t FLAG_D = (1U << 3);

namespace stk {
namespace test {

/*! \namespace stk::test::eventflags
    \brief     Namespace of EventFlags test.
 */
namespace eventflags {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile bool    g_TestComplete  = false;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_EF_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test object (re-constructed per test via ResetTestState)
static sync::EventFlags g_Flags;

// Rendezvous contexts of Sync() tests: one context per rendezvous group (re-initialized per test)
static sync::EventFlags::SyncContext g_SyncCtx[2];

// Cancellation test state (see CancelWaitTask)
static ITask * volatile g_Victim                 = nullptr;     // task whose wait gets cancelled (task 1)
static volatile TId     g_VictimTid              = TID_NONE;    // same, as TId (for SleepCancel)
static volatile uint32_t g_VictimResult[2]       = {0, 0};      // value returned by Wait() in task 1 / task 2 (0 = not set, Wait() never returns 0)
static volatile int32_t  g_VictimEx              = 99;          // EWaitResult returned by the WaitEx() overload in task 1 (99 = not set)
static volatile uint32_t g_VictimActual          = 0;           // actual_flags reported by that overload in task 1
static volatile int64_t g_VictimElapsed[2]       = {0, 0};      // ms spent in Wait() by task 1 / task 2
static volatile int64_t g_VictimSleepElapsed     = 0;           // ms spent in Sleep() by task 1 after its wait ended

// Scenario selector of CancelWaitTask. It is a global, not a constructor argument, because RunTest() declares
// tasks 0-1 as 'static': a static local is constructed only once per template instantiation (on the first
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

// ---------------------------------------------------------------------------

/*! \class SetWaitAnyTask
    \brief Tests WAIT_ANY (OR) semantics: a single flag unblocks a waiting task.
    \note  Four consumer tasks each wait for FLAG_A | FLAG_B | FLAG_C | FLAG_D with
           WAIT_ANY. The producer fires one flag per iteration. Each Set() must
           unblock exactly one consumer; the total woken count must equal the number
           of Set() calls, and each returned value must carry only the set flag.
*/
template <EAccessMode _AccessMode>
class SetWaitAnyTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    SetWaitAnyTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: alternate between two flags so consumers see different bits
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP); // let consumers block first

            for (int32_t i = 0; i < m_iterations; ++i)
            {
                uint32_t flag = (i % 2 == 0) ? FLAG_A : FLAG_B;
                g_Flags.Set(flag);
                stk::Delay(1); // pace so a consumer can unblock between signals
            }

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("set-wait-any: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)m_iterations);

            if (g_SharedCounter == m_iterations)
                g_TestResult = 1;
        }
        else
        {
            // Consumers: wait for any of the four flags, count each successful wake
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                uint32_t result = g_Flags.Wait(FLAG_A | FLAG_B | FLAG_C | FLAG_D,
                                               sync::EventFlags::OPT_WAIT_ANY,
                                               _STK_EF_TEST_TIMEOUT);
                if (!sync::EventFlags::IsError(result))
                    ++g_SharedCounter;
            }
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class SetWaitAllTask
    \brief Tests WAIT_ALL (AND) semantics: all requested flags must be set before unblocking.
    \note  Task 1 waits for FLAG_A | FLAG_B | FLAG_C simultaneously. The producer
           sets them one by one with short delays. The consumer must not wake until
           all three bits are present, and the returned value must equal the full mask.
*/
template <EAccessMode _AccessMode>
class SetWaitAllTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SetWaitAllTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP); // let task 1 block first

            // Set flags one at a time with gaps; consumer must hold until all three arrive
            g_Flags.Set(FLAG_A);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);
            g_Flags.Set(FLAG_B);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);
            g_Flags.Set(FLAG_C); // this final Set() should unblock task 1

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("set-wait-all: counter=%d (expected 1)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Must block until all three flags are simultaneously present
            uint32_t result = g_Flags.Wait(FLAG_A | FLAG_B | FLAG_C,
                                           sync::EventFlags::OPT_WAIT_ALL,
                                           _STK_EF_TEST_TIMEOUT);

            // Return value must equal the full requested mask with no extra bits
            if (!sync::EventFlags::IsError(result) && (result == (FLAG_A | FLAG_B | FLAG_C)))
                ++g_SharedCounter;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class ClearTask
    \brief Tests Clear() return value and effect on subsequent Wait() calls.
    \note  Verifies that Clear() returns the pre-clear flags word, that cleared bits
           block subsequent WAIT_ANY waiters, and that un-cleared bits still unblock them.
*/
template <EAccessMode _AccessMode>
class ClearTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ClearTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Set FLAG_A and FLAG_B, clear FLAG_A, verify only FLAG_B remains
            g_Flags.Set(FLAG_A | FLAG_B);
            uint32_t pre_clear = g_Flags.Clear(FLAG_A); // returns value before clear

            // pre_clear must have had both bits; FLAG_A must now be absent
            bool pre_ok    = ((pre_clear & (FLAG_A | FLAG_B)) == (FLAG_A | FLAG_B));
            bool post_ok   = ((g_Flags.Get() & FLAG_A) == 0U);
            bool flag_b_ok = ((g_Flags.Get() & FLAG_B) != 0U);

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("clear: pre_ok=%d (expected 1), post_ok=%d (expected 1), "
                "flag_b_ok=%d (expected 1), counter=%d (expected 1)\n",
                (int)pre_ok, (int)post_ok, (int)flag_b_ok, (int)g_SharedCounter);

            if (pre_ok && post_ok && flag_b_ok && (g_SharedCounter == 1))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Wait for FLAG_B: must succeed because Clear() only removed FLAG_A
            uint32_t result = g_Flags.Wait(FLAG_B, sync::EventFlags::OPT_WAIT_ANY, _STK_EF_TEST_TIMEOUT);

            if (!sync::EventFlags::IsError(result))
                ++g_SharedCounter;
        }
        else
        if (m_task_id == 2)
        {
            // Wait for FLAG_A after it was cleared: must time out
            uint32_t result = g_Flags.Wait(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, _STK_EF_TEST_SHORT_SLEEP);

            // A timeout is the expected outcome here; a non-error result is a failure
            if (sync::EventFlags::IsError(result))
                ; // expected — do not count
            else
                ++g_SharedCounter; // unexpected wake: failure
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class NoClearTask
    \brief Tests NO_CLEAR option: matched flags are not consumed after a successful Wait().
    \note  Task 1 waits with NO_CLEAR; the flags word must still be set afterwards.
           Task 2 waits on the same flags after task 1 returns and must succeed immediately
           on the fast path, proving the flags were not cleared.
*/
template <EAccessMode _AccessMode>
class NoClearTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    NoClearTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP); // let tasks 1 and 2 reach their Wait() calls

            g_Flags.Set(FLAG_A);

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("no-clear: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Wait with NO_CLEAR; flags must remain set after return
            uint32_t result = g_Flags.Wait(FLAG_A,
                                           sync::EventFlags::OPT_WAIT_ANY | sync::EventFlags::OPT_NO_CLEAR,
                                           _STK_EF_TEST_TIMEOUT);

            if (!sync::EventFlags::IsError(result))
                ++g_SharedCounter;
        }
        else
        if (m_task_id == 2)
        {
            // Stagger slightly so task 1 wins first; then verify flags still present
            stk::Sleep(1);

            uint32_t result = g_Flags.Wait(FLAG_A,
                                           sync::EventFlags::OPT_WAIT_ANY | sync::EventFlags::OPT_NO_CLEAR,
                                           _STK_EF_TEST_TIMEOUT);

            if (!sync::EventFlags::IsError(result))
                ++g_SharedCounter;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class TimeoutTask
    \brief Tests Wait() timeout when the required flags are never set.
    \note  Verifies that Wait() returns ERROR_TIMEOUT within a reasonable time window
           and does not block indefinitely. A subsequent Wait() after the producer
           eventually sets the flag must succeed.
*/
template <EAccessMode _AccessMode>
class TimeoutTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Set FLAG_A well past the timeout window used by task 1
            stk::Sleep(200);
            g_Flags.Set(FLAG_A);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

            // Wait with a 50-tick timeout; must expire before task 0 fires Set()
            int64_t start   = GetTimeNowMs();
            uint32_t result = g_Flags.Wait(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, 50);
            int64_t elapsed = GetTimeNowMs() - start;

            if ((result == sync::EventFlags::ERROR_TIMEOUT) && (elapsed >= 45) && (elapsed <= 60))
                ++g_SharedCounter;
        }
        else
        if (m_task_id == 2)
        {
            // Wait with generous timeout after task 0 fires Set(); must succeed
            stk::Sleep(210);

            uint32_t result = g_Flags.Wait(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, 100);

            if (!sync::EventFlags::IsError(result))
                ++g_SharedCounter;
        }

        if (m_task_id == 2)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

            printf("timeout: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class TryWaitTask
    \brief Tests TryWait() non-blocking poll.
    \note  Verifies that TryWait() returns ERROR_TIMEOUT immediately on unset flags,
           returns the matched mask when flags are set (and auto-clears them), and that
           a second TryWait() on the now-cleared flags returns an error without blocking.
*/
template <EAccessMode _AccessMode>
class TryWaitTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
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
            // TryWait on a non-set flag must return immediately with an error
            int64_t  start   = GetTimeNowMs();
            uint32_t result  = g_Flags.TryWait(FLAG_A);
            int64_t  elapsed = GetTimeNowMs() - start;

            if (sync::EventFlags::IsError(result) && (elapsed < _STK_EF_TEST_SHORT_SLEEP))
                ++g_SharedCounter;
        }
        else
        if (m_task_id == 2)
        {
            // Set FLAG_A then TryWait; must succeed and auto-clear
            g_Flags.Set(FLAG_A);

            int64_t  start   = GetTimeNowMs();
            uint32_t result  = g_Flags.TryWait(FLAG_A);
            int64_t  elapsed = GetTimeNowMs() - start;

            if (!sync::EventFlags::IsError(result) && (elapsed < _STK_EF_TEST_SHORT_SLEEP))
                ++g_SharedCounter;

            // Verify auto-clear: second TryWait must return an error
            if (sync::EventFlags::IsError(g_Flags.TryWait(FLAG_A)))
                ++g_SharedCounter;
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("try-wait: counter=%d (expected 3)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 3)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class GetTask
    \brief Tests Get() returns a non-destructive snapshot of the flags word.
    \note  Verifies that Get() reflects Set() and Clear() operations without consuming
           any flag, and that a Wait() after Get() still succeeds (flags still present).
*/
template <EAccessMode _AccessMode>
class GetTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    GetTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Get() on empty flags word must be zero
            bool empty_ok = (g_Flags.Get() == 0U);

            g_Flags.Set(FLAG_A | FLAG_C);

            // Get() must reflect the two set bits
            bool set_ok = ((g_Flags.Get() & (FLAG_A | FLAG_C)) == (FLAG_A | FLAG_C));

            g_Flags.Clear(FLAG_C);

            // Get() must now show only FLAG_A
            bool clear_ok = ((g_Flags.Get() == FLAG_A));

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("get: empty_ok=%d (expected 1), set_ok=%d (expected 1), "
                "clear_ok=%d (expected 1), counter=%d (expected 1)\n",
                (int)empty_ok, (int)set_ok, (int)clear_ok, (int)g_SharedCounter);

            if (empty_ok && set_ok && clear_ok && (g_SharedCounter == 1))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP); // let task 0 set flags first

            // Get() must not consume the flag; a subsequent Wait() must still succeed
            uint32_t snapshot = g_Flags.Get();
            uint32_t result   = g_Flags.Wait(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, _STK_EF_TEST_TIMEOUT);

            if (((snapshot & FLAG_A) != 0U) && !sync::EventFlags::IsError(result))
                ++g_SharedCounter;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class MultiWaiterAnyTask
    \brief Tests WAIT_ANY with multiple concurrent waiters watching different flag subsets.
    \note  Each consumer waits for its own unique flag bit. The producer sets all four
           flag bits at once. Every consumer must wake exactly once and observe its own
           bit in the returned mask.
*/
template <EAccessMode _AccessMode>
class MultiWaiterAnyTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    MultiWaiterAnyTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Let all four consumers block, then release all at once
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

            g_Flags.Set(FLAG_A | FLAG_B | FLAG_C | FLAG_D); // wakes all four consumers

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("multi-waiter-any: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)(_STK_EF_TEST_TASKS_MAX - 1));

            if (g_SharedCounter == (_STK_EF_TEST_TASKS_MAX - 1))
                g_TestResult = 1;
        }
        else
        {
            // Each consumer watches a unique flag bit
            static const uint32_t k_flags[4] = { FLAG_A, FLAG_B, FLAG_C, FLAG_D };
            uint32_t my_flag = k_flags[m_task_id - 1];

            uint32_t result = g_Flags.Wait(my_flag, sync::EventFlags::OPT_WAIT_ANY, _STK_EF_TEST_TIMEOUT);

            // Return value must contain the consumer's own flag
            if (!sync::EventFlags::IsError(result) && ((result & my_flag) != 0U))
                ++g_SharedCounter;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class MultiWaiterAllTask
    \brief Tests WAIT_ALL with multiple concurrent waiters, each requiring the full flag set.
    \note  All four consumers block on WAIT_ALL for FLAG_A | FLAG_B. The producer sets
           both flags simultaneously. Every consumer must eventually wake with the full
           mask in the returned value. Because the flags are cleared by the first waker,
           the producer re-sets them so remaining consumers can also succeed.
*/
template <EAccessMode _AccessMode>
class MultiWaiterAllTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_consumers;

public:
    MultiWaiterAllTask(uint8_t task_id, int32_t consumers) : m_task_id(task_id), m_consumers(consumers)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP); // let consumers block

            // Re-set flags for each consumer in turn (they clear on wakeup)
            for (int32_t i = 0; i < m_consumers; ++i)
            {
                g_Flags.Set(FLAG_A | FLAG_B);
                stk::Delay(1);
            }

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("multi-waiter-all: counter=%d (expected %d)\n",
                (int)g_SharedCounter, (int)m_consumers);

            if (g_SharedCounter == m_consumers)
                g_TestResult = 1;
        }
        else
        {
            // Each consumer waits for both FLAG_A and FLAG_B together
            uint32_t result = g_Flags.Wait(FLAG_A | FLAG_B,
                                           sync::EventFlags::OPT_WAIT_ALL,
                                           _STK_EF_TEST_TIMEOUT);

            if (!sync::EventFlags::IsError(result) && (result == (FLAG_A | FLAG_B)))
                ++g_SharedCounter;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class InitialFlagsTask
    \brief Tests EventFlags constructed with a non-zero initial flags word.
    \note  Verifies that Wait() returns immediately on the fast path (no Set() needed)
           and that the matched flags are cleared, so a subsequent Wait() for the same
           flags blocks and times out.
*/
template <EAccessMode _AccessMode>
class InitialFlagsTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    InitialFlagsTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // First Wait() must succeed immediately (initial_flags had FLAG_A set)
            uint32_t first = g_Flags.Wait(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, _STK_EF_TEST_SHORT_SLEEP);

            if (!sync::EventFlags::IsError(first))
                ++g_SharedCounter;

            // Second Wait() must time out: FLAG_A was consumed by the first Wait()
            uint32_t second = g_Flags.Wait(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, _STK_EF_TEST_SHORT_SLEEP);

            if (second == sync::EventFlags::ERROR_TIMEOUT)
                ++g_SharedCounter;
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("initial-flags: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------

static const int32_t CW_INFINITE         = 1;
static const int32_t CW_NOT_TIMEOUT      = 2;
static const int32_t CW_NO_STALE_WAITER  = 3;
static const int32_t CW_FLAGS_UNTOUCHED  = 4;
static const int32_t CW_TWO_WAITERS      = 5;
static const int32_t CW_SIGNAL_WINS      = 6;
static const int32_t CW_WAIT_ALL_PARTIAL = 7;
static const int32_t CW_EX_CANCELED      = 8;
static const int32_t CW_VIA_SLEEPCANCEL  = 100;

/*! \class CancelWaitTask
    \brief Tests cancelability of EventFlags waits (IKernel::CancelTaskWait() and stk::SleepCancel()).
    \note  Task 0 is the canceller/checker, task 1 is the victim (blocked in Wait()), task 2 is a
           second waiter used by the two-waiters scenario. The scenario is selected by g_CancelParam:
           1 = cancel an infinite wait (ERROR_CANCELED is returned), 2 = cancel is not reported
           as a timeout, 3 = cancel leaves no stale waiter behind, 4 = cancel does not modify the
           flags word (the test is started with FLAG_B preset), 5 = cancelling one of two waiters
           leaves the other intact, 6 = a Set() that arrived first wins over a late cancel,
           7 = cancel of a WAIT_ALL waiter that already woke up for a partial match does not
           consume the partially matched flags, 8 = cancel of a wait made with the
           WaitEx() overload (WAIT_RESULT_CANCELED is returned and actual_flags holds the
           untouched flags word, the test is started with FLAG_B preset). Adding 100 to g_CancelParam cancels via
           stk::SleepCancel() instead of IKernel::CancelTaskWait().
*/
template <EAccessMode _AccessMode>
class CancelWaitTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
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

    // Task 1: block on the flags, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        if (m_scenario == CW_EX_CANCELED)
        {
            // extended overload: status and flags snapshot are reported separately
            uint32_t actual = 0xA5A5A5A5U; // sentinel: must be overwritten by Wait()

            const int64_t     start = GetTimeNowMs();
            const EWaitResult wr    = g_Flags.WaitEx(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE, actual);

            g_VictimElapsed[0] = GetTimeNowMs() - start;
            g_VictimActual     = actual;
            g_VictimEx         = wr;
            return;
        }

        const Timeout  timeout = (m_scenario == CW_NOT_TIMEOUT ? 300 : WAIT_INFINITE);
        const bool     wait_all = (m_scenario == CW_WAIT_ALL_PARTIAL);
        const uint32_t flags   = (wait_all ? (FLAG_A | FLAG_B) : FLAG_A);
        const uint32_t options = (wait_all ? sync::EventFlags::OPT_WAIT_ALL : sync::EventFlags::OPT_WAIT_ANY);

        const int64_t  start = GetTimeNowMs();
        const uint32_t r     = g_Flags.Wait(flags, options, timeout);

        g_VictimElapsed[0] = GetTimeNowMs() - start;
        g_VictimResult[0]  = r;

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

        stk::Sleep(2 * _STK_EF_TEST_SHORT_SLEEP); // block after task 1

        const int64_t  start = GetTimeNowMs();
        const uint32_t r     = g_Flags.Wait(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, WAIT_INFINITE);

        g_VictimElapsed[1] = GetTimeNowMs() - start;
        g_VictimResult[1]  = r;
    }

    // Task 0: cancel and verify
    void RunCanceller()
    {
        bool pass = false;

        stk::Sleep(60); // task 1 (and task 2) are blocked in Wait() by now

        if (m_scenario == CW_INFINITE)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP * 2);

            // started waiting at ~10 ms, cancelled at ~60 ms
            pass = (g_VictimResult[0] == sync::EventFlags::ERROR_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75);
        }
        else
        if (m_scenario == CW_NOT_TIMEOUT)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP * 2);

            // 300 ms timeout is far away: the wait must end early and as ERROR_CANCELED, not ERROR_TIMEOUT
            pass = (g_VictimResult[0] == sync::EventFlags::ERROR_CANCELED) && (g_VictimElapsed[0] < 150);
        }
        else
        if (m_scenario == CW_NO_STALE_WAITER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP); // victim returns and starts its Sleep(60)

            const uint32_t set_result = g_Flags.Set(FLAG_A);     // nobody waits: flag must simply become set
            const uint32_t poll1      = g_Flags.TryWait(FLAG_A); // consumes the flag
            const uint32_t poll2      = g_Flags.TryWait(FLAG_A); // nothing left

            stk::Sleep(70); // let the victim finish its sleep

            printf("cancel no stale waiter: result=0x%X set=0x%X poll1=0x%X poll2=0x%X victim_sleep=%d ms\n",
                (unsigned)g_VictimResult[0], (unsigned)set_result, (unsigned)poll1, (unsigned)poll2,
                (int)g_VictimSleepElapsed);

            pass = (g_VictimResult[0] == sync::EventFlags::ERROR_CANCELED) &&
                   (set_result == FLAG_A) && (poll1 == FLAG_A) && sync::EventFlags::IsError(poll2) &&
                   (g_VictimSleepElapsed >= 55); // not woken early by a ghost wait entry
        }
        else
        if (m_scenario == CW_FLAGS_UNTOUCHED)
        {
            // flags word was constructed with FLAG_B, the victim waits for FLAG_A
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

            const uint32_t after_cancel = g_Flags.Get();                 // the cancel must not touch the flags word
            const uint32_t poll_a       = g_Flags.TryWait(FLAG_A);       // FLAG_A was never set
            const uint32_t set_result   = g_Flags.Set(FLAG_A);
            const uint32_t peek         = g_Flags.TryWait(FLAG_A, sync::EventFlags::OPT_NO_CLEAR); // keeps FLAG_A
            const uint32_t after_peek   = g_Flags.Get();
            const uint32_t take         = g_Flags.TryWait(FLAG_A);       // consumes FLAG_A
            const uint32_t final_flags  = g_Flags.Get();

            printf("cancel flags untouched: result=0x%X after_cancel=0x%X set=0x%X peek=0x%X take=0x%X final=0x%X\n",
                (unsigned)g_VictimResult[0], (unsigned)after_cancel, (unsigned)set_result,
                (unsigned)peek, (unsigned)take, (unsigned)final_flags);

            pass = (g_VictimResult[0] == sync::EventFlags::ERROR_CANCELED) &&
                   (after_cancel == FLAG_B) && sync::EventFlags::IsError(poll_a) &&
                   (set_result == (FLAG_A | FLAG_B)) &&
                   (peek == FLAG_A) && (after_peek == (FLAG_A | FLAG_B)) &&
                   (take == FLAG_A) && (final_flags == FLAG_B);
        }
        else
        if (m_scenario == CW_TWO_WAITERS)
        {
            CancelVictim(m_use_sleep_cancel);                 // cancel the first waiter only
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

            const uint32_t set_result = g_Flags.Set(FLAG_A);  // must wake the second waiter
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP * 2);

            const uint32_t leftover = g_Flags.TryWait(FLAG_A); // the second waiter consumed the flag

            printf("cancel two waiters: first=0x%X second=0x%X set=0x%X leftover=0x%X\n",
                (unsigned)g_VictimResult[0], (unsigned)g_VictimResult[1], (unsigned)set_result, (unsigned)leftover);

            pass = (g_VictimResult[0] == sync::EventFlags::ERROR_CANCELED) &&
                   (g_VictimResult[1] == FLAG_A) &&
                   !sync::EventFlags::IsError(set_result) && sync::EventFlags::IsError(leftover);
        }
        else
        if (m_scenario == CW_SIGNAL_WINS)
        {
            // back to back, no scheduling point in between: the waiter is already notified and removed
            // from the wait list when the cancel arrives, so the cancel must be a no-op
            g_Flags.Set(FLAG_A);
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP * 2);

            const uint32_t leftover = g_Flags.TryWait(FLAG_A); // the woken waiter consumed the flag

            pass = (g_VictimResult[0] == FLAG_A) && sync::EventFlags::IsError(leftover);
        }
        else
        if (m_scenario == CW_WAIT_ALL_PARTIAL)
        {
            // victim waits for FLAG_A | FLAG_B (WAIT_ALL): a partial Set() wakes it for re-evaluation,
            // it must go back to sleep without consuming FLAG_A and still be cancelable afterwards
            g_Flags.Set(FLAG_A);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

            const bool still_blocked = (g_VictimResult[0] == 0U); // partial match must not complete the wait

            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP * 2);

            const uint32_t flags_left = g_Flags.Get();

            printf("cancel wait-all partial: blocked=%d result=0x%X flags=0x%X\n",
                (int)still_blocked, (unsigned)g_VictimResult[0], (unsigned)flags_left);

            // started waiting at ~10 ms, cancelled at ~70 ms
            pass = still_blocked &&
                   (g_VictimResult[0] == sync::EventFlags::ERROR_CANCELED) &&
                   (g_VictimElapsed[0] >= 45) && (g_VictimElapsed[0] <= 85) &&
                   (flags_left == FLAG_A);
        }

        if (m_scenario == CW_EX_CANCELED)
        {
            // started waiting at ~10 ms, cancelled at ~60 ms; flags word was constructed with FLAG_B
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP * 2);

            const uint32_t flags_after = g_Flags.Get();

            printf("cancel ex: wr=%d (expected %d) actual=0x%X (expected 0x%X) flags=0x%X elapsed=%d ms\n",
                (int)g_VictimEx, (int)WAIT_RESULT_CANCELED, (unsigned)g_VictimActual, (unsigned)FLAG_B,
                (unsigned)flags_after, (int)g_VictimElapsed[0]);

            pass = (g_VictimEx == WAIT_RESULT_CANCELED) && // not TIMEOUT, not SIGNAL
                   (g_VictimActual == FLAG_B) &&           // snapshot of the whole word, written on cancel too
                   (flags_after == FLAG_B) &&              // cancel does not modify the flags word
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75);
        }

        // Safety net: if the cancel did not work a waiter is still blocked forever and the kernel would
        // never stop, release any such waiter so that the test fails instead of hanging.
        if (!pass)
        {
            for (int32_t i = 0; i < 3; ++i)
            {
                g_Flags.Set(FLAG_A | FLAG_B);
                stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);
            }
        }

        printf("cancel wait (scenario=%d, via %s): result[0]=0x%X elapsed[0]=%d ms\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
            (unsigned)g_VictimResult[0], (int)g_VictimElapsed[0]);

        if (pass)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------
// And() - AND-mask operation (ThreadX tx_event_flags_set(TX_AND) equivalent)
// ---------------------------------------------------------------------------

/*! \class AndTask
    \brief Tests And(): flags &= mask, returns the pre-mask word.
    \note  Single task (task 0), no blocking. Checks a partial mask, an all-ones mask (no change),
           a mask with bits that are not set (must not set anything), the complement of a flag
           (same effect as Clear()) and a zero mask (clears everything).
*/
template <EAccessMode _AccessMode>
class AndTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    AndTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id != 0)
            return;

        g_Flags.Set(FLAG_A | FLAG_B | FLAG_C | FLAG_D);

        // keep A and C: B and D are cleared, the returned value is the word before masking
        const uint32_t r1 = g_Flags.And(FLAG_A | FLAG_C);
        const uint32_t f1 = g_Flags.Get();

        // all-ones mask: no change
        const uint32_t r2 = g_Flags.And(0xFFFFFFFFU);
        const uint32_t f2 = g_Flags.Get();

        // mask contains B which is not set: And() can only remove flags, never set them
        const uint32_t r3 = g_Flags.And(FLAG_A | FLAG_B);
        const uint32_t f3 = g_Flags.Get();

        // complement of a flag is equivalent to Clear(): A|B -> B
        g_Flags.Set(FLAG_B);
        const uint32_t r4 = g_Flags.And(~FLAG_A);
        const uint32_t f4 = g_Flags.Get();

        // zero mask clears everything, and a zero mask on an empty word is a no-op
        const uint32_t r5 = g_Flags.And(0U);
        const uint32_t f5 = g_Flags.Get();
        const uint32_t r6 = g_Flags.And(0U);
        const uint32_t f6 = g_Flags.Get();

        printf("and: r1=0x%X f1=0x%X r2=0x%X f2=0x%X r3=0x%X f3=0x%X r4=0x%X f4=0x%X r5=0x%X f5=0x%X r6=0x%X f6=0x%X\n",
            (unsigned)r1, (unsigned)f1, (unsigned)r2, (unsigned)f2, (unsigned)r3, (unsigned)f3,
            (unsigned)r4, (unsigned)f4, (unsigned)r5, (unsigned)f5, (unsigned)r6, (unsigned)f6);

        const bool pass =
            (r1 == (FLAG_A | FLAG_B | FLAG_C | FLAG_D)) && (f1 == (FLAG_A | FLAG_C)) &&
            (r2 == (FLAG_A | FLAG_C))                   && (f2 == (FLAG_A | FLAG_C)) &&
            (r3 == (FLAG_A | FLAG_C))                   && (f3 == FLAG_A)            &&
            (r4 == (FLAG_A | FLAG_B))                   && (f4 == FLAG_B)            &&
            (r5 == FLAG_B)                              && (f5 == 0U)                &&
            (r6 == 0U)                                  && (f6 == 0U)                &&
            !sync::EventFlags::IsError(r1) && !sync::EventFlags::IsError(r6); // And() never reports an error

        if (pass)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------

/*! \class AndWaiterTask
    \brief Tests that And() withdraws a flag that a WAIT_ALL waiter was partially matched on.
    \note  Task 1 waits for FLAG_A | FLAG_B (WAIT_ALL). Task 0 sets FLAG_A (partial match, the waiter
           re-evaluates and goes back to sleep), then removes FLAG_A with And(~FLAG_A) and sets FLAG_B.
           Because FLAG_A is gone the condition is never met and the waiter must time out, with FLAG_B
           left in the flags word.
*/
template <EAccessMode _AccessMode>
class AndWaiterTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    AndWaiterTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);          // let task 1 block first

            g_Flags.Set(FLAG_A);                           // partial match
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);

            const uint32_t before = g_Flags.And(~FLAG_A);  // withdraw FLAG_A
            g_Flags.Set(FLAG_B);                           // would complete the match if FLAG_A were still there

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP + 50);      // task 1 times out at ~100 ms

            const uint32_t flags = g_Flags.Get();

            printf("and-waiter: counter=%d (expected 1) before=0x%X (expected 0x%X) flags=0x%X (expected 0x%X)\n",
                (int)g_SharedCounter, (unsigned)before, (unsigned)FLAG_A, (unsigned)flags, (unsigned)FLAG_B);

            if ((g_SharedCounter == 1) && (before == FLAG_A) && (flags == FLAG_B))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            const uint32_t result = g_Flags.Wait(FLAG_A | FLAG_B, sync::EventFlags::OPT_WAIT_ALL, 100);

            if (result == sync::EventFlags::ERROR_TIMEOUT)
                ++g_SharedCounter;
        }
    }
};

// ---------------------------------------------------------------------------
// WaitEx()/TryWaitEx() returning EWaitResult with the flags snapshot in actual_flags
// ---------------------------------------------------------------------------

/*! \class WaitExTask
    \brief Tests WaitEx(flags, options, timeout, actual_flags): status and full flags word reported separately.
    \note  Task 1 performs four consecutive waits, task 0 is the producer. Each step that behaves as expected
           sets one bit in g_SharedCounter (expected final value 0xF):
           1. WAIT_ANY on FLAG_A while FLAG_A | FLAG_C are set: WAIT_RESULT_SIGNAL, actual_flags is the whole
              word (A|C, including the unrelated FLAG_C), only FLAG_A is cleared afterwards.
           2. WAIT_ALL on FLAG_A | FLAG_B while A|B|D are set on top of C: actual_flags is the whole word
              (A|B|C|D) captured before the auto-clear, only A|B are cleared.
           3. WAIT_ANY | NO_CLEAR on FLAG_D (fast path): actual_flags is C|D, nothing is cleared.
           4. Timeout: WAIT_RESULT_TIMEOUT (not SIGNAL/FAIL/CANCELED) after ~40 ms, actual_flags still
              reports the current word (C|D), flags are untouched.
*/
template <EAccessMode _AccessMode>
class WaitExTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WaitExTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);              // let task 1 block (step 1)
            g_Flags.Set(FLAG_A | FLAG_C);                      // wakes step 1

            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP);              // task 1 is blocked in step 2 by now
            g_Flags.Set(FLAG_A | FLAG_B | FLAG_D);             // completes step 2

            stk::Sleep(_STK_EF_TEST_LONG_SLEEP + 50);          // steps 3 and 4 (40 ms timeout) finish

            printf("wait-ex: steps=0x%X (expected 0xF)\n", (unsigned)g_SharedCounter);

            if (g_SharedCounter == 0xF)
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            uint32_t    actual;
            EWaitResult wr;

            // step 1: WAIT_ANY, blocks until task 0 sets FLAG_A | FLAG_C
            actual = 0xA5A5A5A5U;
            wr     = g_Flags.WaitEx(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, _STK_EF_TEST_TIMEOUT, actual);
            const uint32_t after1 = g_Flags.Get();
            printf("wait-ex step1: wr=%d actual=0x%X after=0x%X\n", (int)wr, (unsigned)actual, (unsigned)after1);
            if ((wr == WAIT_RESULT_SIGNAL) && (actual == (FLAG_A | FLAG_C)) && (after1 == FLAG_C))
                g_SharedCounter = g_SharedCounter | 0x1;

            // step 2: WAIT_ALL, blocks until task 0 sets FLAG_A | FLAG_B | FLAG_D (FLAG_C is still set)
            actual = 0xA5A5A5A5U;
            wr     = g_Flags.WaitEx(FLAG_A | FLAG_B, sync::EventFlags::OPT_WAIT_ALL, _STK_EF_TEST_TIMEOUT, actual);
            const uint32_t after2 = g_Flags.Get();
            printf("wait-ex step2: wr=%d actual=0x%X after=0x%X\n", (int)wr, (unsigned)actual, (unsigned)after2);
            if ((wr == WAIT_RESULT_SIGNAL) && (actual == (FLAG_A | FLAG_B | FLAG_C | FLAG_D)) &&
                (after2 == (FLAG_C | FLAG_D)))
                g_SharedCounter = g_SharedCounter | 0x2;

            // step 3: NO_CLEAR on an already set flag (fast path, no blocking)
            actual = 0xA5A5A5A5U;
            wr     = g_Flags.WaitEx(FLAG_D, sync::EventFlags::OPT_WAIT_ANY | sync::EventFlags::OPT_NO_CLEAR,
                                  _STK_EF_TEST_SHORT_SLEEP, actual);
            const uint32_t after3 = g_Flags.Get();
            printf("wait-ex step3: wr=%d actual=0x%X after=0x%X\n", (int)wr, (unsigned)actual, (unsigned)after3);
            if ((wr == WAIT_RESULT_SIGNAL) && (actual == (FLAG_C | FLAG_D)) && (after3 == (FLAG_C | FLAG_D)))
                g_SharedCounter = g_SharedCounter | 0x4;

            // step 4: FLAG_A is never set again: timeout, snapshot still reported
            actual = 0xA5A5A5A5U;
            const int64_t start = GetTimeNowMs();
            wr = g_Flags.WaitEx(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, 40, actual);
            const int64_t  elapsed = GetTimeNowMs() - start;
            const uint32_t after4  = g_Flags.Get();
            printf("wait-ex step4: wr=%d actual=0x%X after=0x%X elapsed=%d ms\n",
                (int)wr, (unsigned)actual, (unsigned)after4, (int)elapsed);
            if ((wr == WAIT_RESULT_TIMEOUT) && (actual == (FLAG_C | FLAG_D)) && (after4 == (FLAG_C | FLAG_D)) &&
                (elapsed >= 35) && (elapsed <= 55))
                g_SharedCounter = g_SharedCounter | 0x8;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class PollExTask
    \brief Tests TryWaitEx(flags, options, actual_flags) and argument validation of WaitEx().
    \note  Single task (task 0), no blocking. The test is started with FLAG_C preset. Checks that
           a failed poll reports a non-SIGNAL result and the current word, that a WAIT_ALL partial match is
           not consumed, that NO_CLEAR keeps the flags, that a successful poll reports the word before the
           auto-clear, and that invalid arguments (flags == 0, bit 31 set) return WAIT_RESULT_FAIL with
           actual_flags still written and the flags word untouched.
*/
template <EAccessMode _AccessMode>
class PollExTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PollExTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id != 0)
            return;

        uint32_t actual;
        bool     pass = true;

        // 1: FLAG_A is not set (only FLAG_C is): the poll fails without blocking and reports the word
        actual = 0xA5A5A5A5U;
        const EWaitResult wr1 = g_Flags.TryWaitEx(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, actual);
        const bool ok1 = (wr1 != WAIT_RESULT_SIGNAL) && (actual == FLAG_C) && (g_Flags.Get() == FLAG_C);

        // 2: NO_CLEAR keeps the matched flag, actual_flags includes the unrelated FLAG_C
        g_Flags.Set(FLAG_A);
        actual = 0xA5A5A5A5U;
        const EWaitResult wr2 = g_Flags.TryWaitEx(FLAG_A, sync::EventFlags::OPT_WAIT_ANY | sync::EventFlags::OPT_NO_CLEAR, actual);
        const bool ok2 = (wr2 == WAIT_RESULT_SIGNAL) && (actual == (FLAG_A | FLAG_C)) &&
                         (g_Flags.Get() == (FLAG_A | FLAG_C));

        // 3: WAIT_ALL with FLAG_B missing: no match, nothing is consumed
        actual = 0xA5A5A5A5U;
        const EWaitResult wr3 = g_Flags.TryWaitEx(FLAG_A | FLAG_B, sync::EventFlags::OPT_WAIT_ALL, actual);
        const bool ok3 = (wr3 != WAIT_RESULT_SIGNAL) && (actual == (FLAG_A | FLAG_C)) &&
                         (g_Flags.Get() == (FLAG_A | FLAG_C));

        // 4: successful poll auto-clears the matched flag, actual_flags is the word before the clear
        actual = 0xA5A5A5A5U;
        const EWaitResult wr4 = g_Flags.TryWaitEx(FLAG_A, sync::EventFlags::OPT_WAIT_ANY, actual);
        const bool ok4 = (wr4 == WAIT_RESULT_SIGNAL) && (actual == (FLAG_A | FLAG_C)) && (g_Flags.Get() == FLAG_C);

        // 5: invalid arguments: flags == 0
        actual = 0xA5A5A5A5U;
        const EWaitResult wr5 = g_Flags.WaitEx(0U, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual);
        const bool ok5 = (wr5 == WAIT_RESULT_FAIL) && (actual == FLAG_C) && (g_Flags.Get() == FLAG_C);

        // 6: invalid arguments: bit 31 set (reserved)
        actual = 0xA5A5A5A5U;
        const EWaitResult wr6 = g_Flags.WaitEx(0x80000000U | FLAG_C, sync::EventFlags::OPT_WAIT_ANY, NO_WAIT, actual);
        const bool ok6 = (wr6 == WAIT_RESULT_FAIL) && (actual == FLAG_C) && (g_Flags.Get() == FLAG_C);

        // 7: the legacy overload is unchanged and shares the same state: it consumes FLAG_C
        const uint32_t legacy = g_Flags.TryWait(FLAG_C);
        const bool ok7 = (legacy == FLAG_C) && (g_Flags.Get() == 0U);

        printf("poll-ex: wr1=%d ok1=%d | wr2=%d ok2=%d | wr3=%d ok3=%d | wr4=%d ok4=%d | wr5=%d ok5=%d | wr6=%d ok6=%d | legacy=0x%X ok7=%d\n",
            (int)wr1, (int)ok1, (int)wr2, (int)ok2, (int)wr3, (int)ok3, (int)wr4, (int)ok4,
            (int)wr5, (int)ok5, (int)wr6, (int)ok6, (unsigned)legacy, (int)ok7);

        pass = ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7;

        if (pass)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------
// Sync() - multi-task rendezvous with a caller-supplied SyncContext
// ---------------------------------------------------------------------------

/*! \class SyncTask
    \brief Tests Sync() as a barrier for 3 tasks, over 2 consecutive rounds with the same context.
    \note  Task i sets its own flag (FLAG_A, FLAG_B, FLAG_C) and waits for all three. The tasks arrive at
           staggered times (0, 10 and 30 ms) so that tasks 0 and 1 are already blocked when task 2
           completes the rendezvous. Every task must be released with WAIT_RESULT_SIGNAL and with actual_flags
           holding the whole word before the clear (FLAG_A | FLAG_B | FLAG_C). With Set() + Wait(WAIT_ALL)
           the first task to wake would clear the flags and the others would never be released.
           Round 2 reuses the same context (generation counter) with a different arrival order.
           Bits 0..2 of the counter = round 1 ok per task, bits 3..5 = round 2 ok per task.
*/
template <EAccessMode _AccessMode>
class SyncTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SyncTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id > 2)
            return;

        static const uint32_t bits[3]     = { FLAG_A, FLAG_B, FLAG_C };
        static const uint32_t delay1[3]   = { 0, 10, 30 };  // round 1: task 2 is the last arriver
        static const uint32_t delay2[3]   = { 30, 10, 0 };  // round 2: task 0 is the last arriver
        const uint32_t        all         = FLAG_A | FLAG_B | FLAG_C;

        for (int32_t round = 0; round < 2; ++round)
        {
            stk::Sleep(round == 0 ? delay1[m_task_id] : delay2[m_task_id]);

            uint32_t actual = 0xA5A5A5A5U;
            const EWaitResult wr = g_Flags.Sync(g_SyncCtx[0], bits[m_task_id], all, _STK_EF_TEST_TIMEOUT, actual);

            printf("sync round %d task %d: wr=%d actual=0x%X\n", (int)(round + 1), (int)m_task_id, (int)wr, (unsigned)actual);

            if ((wr == WAIT_RESULT_SIGNAL) && (actual == all))
                g_SharedCounter = g_SharedCounter | (1 << ((round * 3) + m_task_id));

            // let the whole group leave round 1 before round 2 starts
            stk::Sleep(_STK_EF_TEST_SHORT_SLEEP + 40);
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("sync: ok=0x%X (expected 0x3F) flags=0x%X (expected 0)\n",
                (unsigned)g_SharedCounter, (unsigned)g_Flags.Get());

            if ((g_SharedCounter == 0x3F) && (g_Flags.Get() == 0U))
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------

/*! \class SyncTimeoutTask
    \brief Tests Sync() timeout, NO_WAIT, immediate completion and argument validation (single task).
    \note  Steps (bit set in the counter on success):
           0x1: alone, 40 ms timeout: WAIT_RESULT_TIMEOUT after ~40 ms, actual_flags = FLAG_A, and the flag
                that this task set stays set (it is not rolled back).
           0x2: NO_WAIT while the condition is not met: WAIT_RESULT_TIMEOUT, actual_flags = FLAG_A.
           0x4: FLAG_B arrives from a plain Set(): Sync() with NO_WAIT completes immediately, actual_flags is
                the word before the clear (FLAG_A | FLAG_B) and the group flags are cleared.
           0x8: invalid arguments (wait_flags 0, bit 31 in set_flags or in wait_flags) -> WAIT_RESULT_FAIL and
                nothing is set.
*/
template <EAccessMode _AccessMode>
class SyncTimeoutTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SyncTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id != 0)
            return;

        uint32_t    actual;
        EWaitResult wr;

        // step 1: nobody else joins -> timeout, the flag set by this task stays set
        const int64_t start = GetTimeNowMs();
        actual = 0xA5A5A5A5U;
        wr     = g_Flags.Sync(g_SyncCtx[0], FLAG_A, FLAG_A | FLAG_B, 40, actual);
        const int64_t elapsed = GetTimeNowMs() - start;
        printf("sync-timeout step1: wr=%d actual=0x%X flags=0x%X elapsed=%d ms\n",
            (int)wr, (unsigned)actual, (unsigned)g_Flags.Get(), (int)elapsed);
        if ((wr == WAIT_RESULT_TIMEOUT) && (actual == FLAG_A) && (g_Flags.Get() == FLAG_A) &&
            (elapsed >= 35) && (elapsed <= 55))
            g_SharedCounter = g_SharedCounter | 0x1;

        // step 2: NO_WAIT, condition not met
        actual = 0xA5A5A5A5U;
        wr     = g_Flags.Sync(g_SyncCtx[0], 0U, FLAG_A | FLAG_B, NO_WAIT, actual);
        printf("sync-timeout step2: wr=%d actual=0x%X\n", (int)wr, (unsigned)actual);
        if ((wr == WAIT_RESULT_TIMEOUT) && (actual == FLAG_A))
            g_SharedCounter = g_SharedCounter | 0x2;

        // step 3: condition met by a plain Set(): completes immediately and clears the group flags
        g_Flags.Set(FLAG_B);
        actual = 0xA5A5A5A5U;
        wr     = g_Flags.Sync(g_SyncCtx[0], 0U, FLAG_A | FLAG_B, NO_WAIT, actual);
        printf("sync-timeout step3: wr=%d actual=0x%X flags=0x%X\n", (int)wr, (unsigned)actual, (unsigned)g_Flags.Get());
        if ((wr == WAIT_RESULT_SIGNAL) && (actual == (FLAG_A | FLAG_B)) && (g_Flags.Get() == 0U))
            g_SharedCounter = g_SharedCounter | 0x4;

        // step 4: invalid arguments: nothing is set
        const EWaitResult wr1 = g_Flags.Sync(g_SyncCtx[0], FLAG_A, 0U, NO_WAIT, actual);
        const EWaitResult wr2 = g_Flags.Sync(g_SyncCtx[0], FLAG_A | 0x80000000U, FLAG_A, NO_WAIT, actual);
        const EWaitResult wr3 = g_Flags.Sync(g_SyncCtx[0], FLAG_A, FLAG_A | 0x80000000U, NO_WAIT, actual);
        printf("sync-timeout step4: wr=%d,%d,%d flags=0x%X\n", (int)wr1, (int)wr2, (int)wr3, (unsigned)g_Flags.Get());
        if ((wr1 == WAIT_RESULT_FAIL) && (wr2 == WAIT_RESULT_FAIL) && (wr3 == WAIT_RESULT_FAIL) &&
            (g_Flags.Get() == 0U))
            g_SharedCounter = g_SharedCounter | 0x8;

        printf("sync-timeout: steps=0x%X (expected 0xF)\n", (unsigned)g_SharedCounter);

        if (g_SharedCounter == 0xF)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------

/*! \class SyncGroupsTask
    \brief Tests that independent rendezvous groups on one EventFlags object do not release each other.
    \note  Group X (context 0, wait FLAG_A | FLAG_B): task 0 arrives at 10 ms, task 1 at 30 ms.
           Group Y (context 1, wait FLAG_C | FLAG_D): task 2 arrives at 20 ms, task 3 at 60 ms.
           Group X completes at ~30 ms while task 2 is blocked in group Y: task 2 must stay blocked until
           task 3 arrives (~60 ms). actual_flags of group X is the whole word including FLAG_C, that of
           group Y holds only FLAG_C | FLAG_D (A and B were cleared by group X). Counter bit i = task i ok.
*/
template <EAccessMode _AccessMode>
class SyncGroupsTask : public Task<_STK_EF_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SyncGroupsTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id > 3)
            return;

        static const uint32_t bits[4]  = { FLAG_A, FLAG_B, FLAG_C, FLAG_D };
        static const uint32_t delay[4] = { 10, 30, 20, 60 };
        const uint32_t        group    = (m_task_id < 2 ? 0U : 1U);
        const uint32_t        wait_for = (group == 0U ? (FLAG_A | FLAG_B) : (FLAG_C | FLAG_D));

        const int64_t start = GetTimeNowMs();

        stk::Sleep(delay[m_task_id]);

        uint32_t actual = 0xA5A5A5A5U;
        const EWaitResult wr = g_Flags.Sync(g_SyncCtx[group], bits[m_task_id], wait_for, _STK_EF_TEST_TIMEOUT, actual);
        const int64_t elapsed = GetTimeNowMs() - start;

        printf("sync-groups task %d: wr=%d actual=0x%X elapsed=%d ms\n",
            (int)m_task_id, (int)wr, (unsigned)actual, (int)elapsed);

        bool ok = (wr == WAIT_RESULT_SIGNAL);

        if (group == 0U)
        {
            ok = ok && (actual == (FLAG_A | FLAG_B | FLAG_C)) && (elapsed >= 25) && (elapsed <= 50);
        }
        else
        {
            // task 2 must not be released by the completion of group X at ~30 ms
            ok = ok && (actual == (FLAG_C | FLAG_D)) && (elapsed >= 55) && (elapsed <= 85);
        }

        if (ok)
            g_SharedCounter = g_SharedCounter | (1 << m_task_id);

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_EF_TEST_LONG_SLEEP);

            printf("sync-groups: ok=0x%X (expected 0xF) flags=0x%X (expected 0)\n",
                (unsigned)g_SharedCounter, (unsigned)g_Flags.Get());

            if ((g_SharedCounter == 0xF) && (g_Flags.Get() == 0U))
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Helper: reset test state and re-construct the EventFlags object in-place
// ---------------------------------------------------------------------------

static void ResetTestState(uint32_t initial_flags = 0U)
{
    g_TestResult    = 0;
    g_SharedCounter = 0;
    g_TestComplete  = false;

    g_Victim             = nullptr;
    g_VictimTid          = TID_NONE;
    g_VictimSleepElapsed = 0;
    g_VictimEx           = 99;
    g_VictimActual       = 0;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]  = 0;
        g_VictimElapsed[i] = 0;
    }

    // Re-construct the EventFlags object in-place with the requested initial value
    g_Flags.~EventFlagsT();
    new (&g_Flags) sync::EventFlags(initial_flags);

    for (int32_t i = 0; i < 2; ++i)
        g_SyncCtx[i] = sync::EventFlags::SyncContext();
}

} // namespace eventflags
} // namespace test
} // namespace stk

// ---------------------------------------------------------------------------
// RunTest helper
// ---------------------------------------------------------------------------

static bool NeedsExtendedTasks(const char *test_name)
{
    return (strcmp(test_name, "SetWaitAll")    != 0) &&
           (strcmp(test_name, "Clear")         != 0) &&
           (strcmp(test_name, "NoClear")       != 0) &&
           (strcmp(test_name, "Timeout")       != 0) &&
           (strcmp(test_name, "TryWait")       != 0) &&
           (strcmp(test_name, "Get")           != 0) &&
           (strcmp(test_name, "InitialFlags")  != 0) &&
           (strcmp(test_name, "And")           != 0) &&
           (strcmp(test_name, "AndWaiter")     != 0) &&
           (strcmp(test_name, "WaitEx")        != 0) &&
           (strcmp(test_name, "PollEx")        != 0) &&
           (strcmp(test_name, "Sync")          != 0) &&
           (strcmp(test_name, "SyncTimeout")   != 0) &&
           (strncmp(test_name, "CancelWait", 10) != 0); // cancel tests use tasks 0-2 only
}

/*! \fn    RunTest
    \brief Helper function to run a single EventFlags test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0, uint32_t initial_flags = 0U)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::eventflags;

    printf("Test: %s\n", test_name);

    ResetTestState(initial_flags);

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

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/*! \fn    main
    \brief Entry point to the EventFlags test suite.
*/
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    using namespace stk::test::eventflags;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

    // Test 1: WAIT_ANY — one Set() unblocks exactly one consumer per call
    if (RunTest<SetWaitAnyTask<ACCESS_PRIVILEGED>>("SetWaitAny", 20) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

#ifndef __ARM_ARCH_6M__

    // Test 2: WAIT_ALL — Wait() blocks until all requested bits are simultaneously set
    if (RunTest<SetWaitAllTask<ACCESS_PRIVILEGED>>("SetWaitAll") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: Clear() returns pre-clear value; cleared bits block subsequent waiters
    if (RunTest<ClearTask<ACCESS_PRIVILEGED>>("Clear") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: NO_CLEAR option — matched flags are not consumed; next Wait() still succeeds
    if (RunTest<NoClearTask<ACCESS_PRIVILEGED>>("NoClear") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Wait() returns ERROR_TIMEOUT within the expected window when flags are never set
    if (RunTest<TimeoutTask<ACCESS_PRIVILEGED>>("Timeout") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: TryWait() returns immediately; auto-clears on success; fails when flags absent
    if (RunTest<TryWaitTask<ACCESS_PRIVILEGED>>("TryWait") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Get() is non-destructive; reflects Set() and Clear() without consuming flags
    if (RunTest<GetTask<ACCESS_PRIVILEGED>>("Get") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 8: Multiple concurrent WAIT_ANY waiters, each with a unique flag bit
    if (RunTest<MultiWaiterAnyTask<ACCESS_PRIVILEGED>>("MultiWaiterAny") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 9: Multiple concurrent WAIT_ALL waiters — all must eventually receive the full mask
    if (RunTest<MultiWaiterAllTask<ACCESS_PRIVILEGED>>("MultiWaiterAll", _STK_EF_TEST_TASKS_MAX - 1) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 10: Constructor initial_flags — fast-path Wait() succeeds; cleared flag then times out
    if (RunTest<InitialFlagsTask<ACCESS_PRIVILEGED>>("InitialFlags", 0, FLAG_A) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 11: And() — flags &= mask, returns the pre-mask word, never sets flags
    if (RunTest<AndTask<ACCESS_PRIVILEGED>>("And", 0, FLAG_A | FLAG_B | FLAG_C | FLAG_D) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 12: And() withdrawing a partially matched flag keeps a WAIT_ALL waiter blocked (until timeout)
    if (RunTest<AndWaiterTask<ACCESS_PRIVILEGED>>("AndWaiter") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 13: WaitEx() — EWaitResult status plus full flags snapshot (ANY, ALL, NO_CLEAR, timeout)
    if (RunTest<WaitExTask<ACCESS_PRIVILEGED>>("WaitEx") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 14: TryWaitEx() and argument validation of WaitEx()
    if (RunTest<PollExTask<ACCESS_PRIVILEGED>>("PollEx", 0, FLAG_C) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 15: Sync() — 3-task rendezvous over 2 rounds (all participants released, context reused)
    if (RunTest<SyncTask<ACCESS_PRIVILEGED>>("Sync") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 16: Sync() — timeout (set flag stays), NO_WAIT, immediate completion, argument validation
    if (RunTest<SyncTimeoutTask<ACCESS_PRIVILEGED>>("SyncTimeout") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 17: Sync() — two independent rendezvous groups (separate contexts) on one object
    if (RunTest<SyncGroupsTask<ACCESS_PRIVILEGED>>("SyncGroups") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Tests 18-31: cancelability of EventFlags waits (tasks 0-2 only), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        struct CancelCase { const char *name; int32_t param; uint32_t initial_flags; };

        static const CancelCase cases[] =
        {
            { "CancelWait_Infinite",             CW_INFINITE,                              0U     },
            { "CancelWait_NotTimeout",           CW_NOT_TIMEOUT,                           0U     },
            { "CancelWait_NoStaleWaiter",        CW_NO_STALE_WAITER,                       0U     },
            { "CancelWait_FlagsUntouched",       CW_FLAGS_UNTOUCHED,                       FLAG_B },
            { "CancelWait_TwoWaiters",           CW_TWO_WAITERS,                           0U     },
            { "CancelWait_SignalWins",           CW_SIGNAL_WINS,                           0U     },
            { "CancelWait_WaitAllPartial",       CW_WAIT_ALL_PARTIAL,                      0U     },
            { "CancelWait_SleepCancel",          CW_VIA_SLEEPCANCEL + CW_INFINITE,         0U     },
            { "CancelWait_SleepCancelNoStale",   CW_VIA_SLEEPCANCEL + CW_NO_STALE_WAITER,  0U     },
            { "CancelWait_SleepCancelTwo",       CW_VIA_SLEEPCANCEL + CW_TWO_WAITERS,      0U     },
            { "CancelWait_SleepCancelSignal",    CW_VIA_SLEEPCANCEL + CW_SIGNAL_WINS,      0U     },
            { "CancelWait_SleepCancelAllPartial",CW_VIA_SLEEPCANCEL + CW_WAIT_ALL_PARTIAL, 0U     },
            { "CancelWait_ExCanceled",           CW_EX_CANCELED,                           FLAG_B },
            { "CancelWait_SleepCancelEx",        CW_VIA_SLEEPCANCEL + CW_EX_CANCELED,      FLAG_B }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_CancelParam = cases[i].param;

            if (RunTest<CancelWaitTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param, cases[i].initial_flags)
                != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

#endif // __ARM_ARCH_6M__

    int32_t final_result = (total_failures == 0 ? TestContext::SUCCESS_EXIT_CODE : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
