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
#include <sync/stk_sync_semaphore.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_SEM_TEST_TASKS_MAX   5
#define _STK_SEM_TEST_TIMEOUT     1000
#define _STK_SEM_TEST_SHORT_SLEEP 10
#define _STK_SEM_TEST_LONG_SLEEP  100
#ifdef __ARM_ARCH_6M__
#define _STK_SEM_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_SEM_STACK_SIZE       256
#define STK_TASK                  static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::semaphore
    \brief     Namespace of Semaphore test.
 */
namespace semaphore {

// Counter type limits: COUNT_MAX is the maximum value of the counter type minus one
static_assert(sync::SemaphoreT<uint8_t>::COUNT_MAX  == 0xFFU,       "uint8_t COUNT_MAX must be 0xFF");
static_assert(sync::SemaphoreT<uint16_t>::COUNT_MAX == 0xFFFFU,     "uint16_t COUNT_MAX must be 0xFFFF");
static_assert(sync::SemaphoreT<uint32_t>::COUNT_MAX == 0xFFFFFFFFU, "uint32_t COUNT_MAX must be 0xFFFFFFFF");
static_assert(sync::SemaphoreT<uint64_t>::COUNT_MAX == 0xFFFFFFFFFFFFFFFFULL, "uint64_t COUNT_MAX must be 0xFFFFFFFFFFFFFFFF");
static_assert(sync::Semaphore::COUNT_MAX            == 0xFFFFU,     "default Semaphore must keep a 16-bit counter");

// Test results storage
static volatile int32_t g_TestResult = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_AcquisitionOrder[_STK_SEM_TEST_TASKS_MAX] = {0};
static volatile int32_t g_OrderIndex = 0;
static volatile bool    g_TestComplete = false;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, _STK_SEM_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Test semaphore (re-constructed per test via ResetTestState)
static sync::Semaphore g_TestSemaphore;

// Cancellation test state (see CancelWaitTask)
static ITask * volatile g_Victim                 = nullptr;      // task whose wait gets cancelled (task 1)
static volatile TId     g_VictimTid              = TID_NONE;     // same, as TId (for SleepCancel)
static volatile int32_t g_VictimResult[2]        = {-100, -100}; // EWaitResult of task 1 / task 2 (-100 = not set)
static volatile int64_t g_VictimElapsed[2]       = {0, 0};       // ms spent in the wait by task 1 / task 2
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

/*! \class BasicSignalWaitTask
    \brief Tests basic Signal/Wait functionality.
    \note  Verifies that Wait() blocks until Signal() is called, and that every
           signal produces exactly one successful Wait().
*/
template <EAccessMode _AccessMode>
class BasicSignalWaitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    BasicSignalWaitTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Task 0: producer - signal once per iteration for each consumer
            for (int32_t i = 0; i < m_iterations * (_STK_SEM_TEST_TASKS_MAX - 1); ++i)
            {
                stk::Delay(1); // pace signals so consumers can catch up
                g_TestSemaphore.Signal();
            }

            stk::Sleep(_STK_SEM_TEST_LONG_SLEEP);

            int32_t expected = m_iterations * (_STK_SEM_TEST_TASKS_MAX - 1);

            printf("basic signal/wait: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)expected);

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: consumers - wait and increment counter per acquired signal
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestSemaphore.Wait();
                ++g_SharedCounter;
            }
        }
    }
};

/*! \class InitialCountTask
    \brief Tests semaphore constructed with a non-zero initial count.
    \note  Verifies that the first N Wait() calls succeed immediately (fast path)
           when the semaphore is initialized with count N, and that the count
           reaches zero afterwards.
*/
template <EAccessMode _AccessMode>
class InitialCountTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    InitialCountTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        // Semaphore is pre-loaded with (_STK_SEM_TEST_TASKS_MAX - 1) permits in ResetTestState.
        // Tasks 1-4 grab one permit each immediately; task 0 must block (count already at 0).
        if (m_task_id != 0)
        {
            // Should succeed on fast path - count is pre-loaded
            if (g_TestSemaphore.Wait(_STK_SEM_TEST_SHORT_SLEEP))
                ++g_SharedCounter;
        }
        else
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP / 2); // let consumers drain the count first

            // Count should now be 0; this Wait must block until a signal arrives
            bool acquired = g_TestSemaphore.Wait(_STK_SEM_TEST_SHORT_SLEEP);

            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            int32_t expected = _STK_SEM_TEST_TASKS_MAX - 1;

            printf("initial count: fast-path counter=%d (expected %d), blocked-wait acquired=%d (expected 0)\n",
                (int)g_SharedCounter, (int)expected, (int)acquired);

            if ((g_SharedCounter == expected) && !acquired)
                g_TestResult = 1;
        }
    }
};

/*! \class TimeoutWaitTask
    \brief Tests Wait() timeout behavior.
    \note  Verifies that Wait() respects the timeout and returns false when no
           signal arrives within the allotted time.
*/
template <EAccessMode _AccessMode>
class TimeoutWaitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
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
            // Task 0: hold the semaphore at zero; signal only after the test window
            stk::Sleep(200); // wait well past timeout window before signalling
            g_TestSemaphore.Signal();
        }
        else
        if (m_task_id == 1)
        {
            // Task 1: Wait with 50-tick timeout while semaphore stays at zero
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // let task 0 establish the zero state

            int64_t start   = GetTimeNowMs();
            bool acquired   = g_TestSemaphore.Wait(50); // 50-tick timeout
            int64_t elapsed = GetTimeNowMs() - start;

            // Should time out after ~50 ms and return false
            if (!acquired && elapsed >= 45 && elapsed <= 60)
                ++g_SharedCounter;

            if (acquired)
                g_TestSemaphore.Signal(); // return the token
        }
        else
        if (m_task_id == 2)
        {
            // Task 2: verify that a Wait with a generous timeout succeeds after task 0 signals
            stk::Sleep(210); // after task 0's signal

            if (g_TestSemaphore.Wait(100))
                ++g_SharedCounter;
        }

        if (m_task_id == 2)
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            printf("timeout wait: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class ZeroTimeoutTask
    \brief Tests Wait(0) (NO_WAIT) non-blocking behavior.
    \note  Verifies that Wait(0) returns immediately without blocking when the
           semaphore count is zero, and returns true immediately when count > 0.
*/
template <EAccessMode _AccessMode>
class ZeroTimeoutTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ZeroTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 1)
        {
            // Wait(0) on a zero-count semaphore must return false immediately
            int64_t start   = GetTimeNowMs();
            bool acquired   = g_TestSemaphore.Wait(NO_WAIT);
            int64_t elapsed = GetTimeNowMs() - start;

            if (!acquired && elapsed < _STK_SEM_TEST_SHORT_SLEEP)
                ++g_SharedCounter;
        }
        else
        if (m_task_id == 2)
        {
            // Pre-load one permit then Wait(0) must succeed immediately
            g_TestSemaphore.Signal();

            int64_t start   = GetTimeNowMs();
            bool acquired   = g_TestSemaphore.Wait(NO_WAIT);
            int64_t elapsed = GetTimeNowMs() - start;

            if (acquired && elapsed < _STK_SEM_TEST_SHORT_SLEEP)
                ++g_SharedCounter;
        }

        if (m_task_id == 0)
        {
            stk::Sleep(_STK_SEM_TEST_LONG_SLEEP);

            printf("zero-timeout wait: counter=%d (expected 2)\n", (int)g_SharedCounter);

            if (g_SharedCounter == 2)
                g_TestResult = 1;
        }
    }
};

/*! \class SignalBeforeWaitTask
    \brief Tests that Signal() before Wait() is remembered by the counter.
    \note  Verifies the "stateful" property: signals posted with no waiters
           increment m_count, and a subsequent Wait() drains them on the fast path.
*/
template <EAccessMode _AccessMode>
class SignalBeforeWaitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    SignalBeforeWaitTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Fire all signals upfront before any waiter is ready
            for (int32_t i = 0; i < m_iterations; ++i)
                g_TestSemaphore.Signal();

            // Verify count accumulated correctly
            uint32_t count_after_signals = g_TestSemaphore.GetCount();

            stk::Sleep(_STK_SEM_TEST_LONG_SLEEP);

            printf("signal-before-wait: count_after_signals=%d (expected %d), counter=%d (expected %d)\n",
                (int)count_after_signals, (int)m_iterations, (int)g_SharedCounter, (int)m_iterations);

            if ((count_after_signals == (uint32_t)m_iterations) && (g_SharedCounter == m_iterations))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Single consumer drains all pre-posted signals on the fast path
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // ensure signals are already posted

            for (int32_t i = 0; i < m_iterations; ++i)
            {
                if (g_TestSemaphore.Wait(_STK_SEM_TEST_SHORT_SLEEP))
                    ++g_SharedCounter;
            }
        }
    }
};

/*! \class FIFOOrderTask
    \brief Tests FIFO ordering of waiting tasks.
    \note  Verifies that tasks are woken in the order they blocked on Wait().
*/
template <EAccessMode _AccessMode>
class FIFOOrderTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
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
            // Task 0: producer - stagger signals so all waiters are queued before first wake
            stk::Sleep(50); // give consumers time to block in FIFO order

            for (int32_t i = 0; i < (_STK_SEM_TEST_TASKS_MAX - 1); ++i)
            {
                g_TestSemaphore.Signal();
                stk::Delay(1); // small pause between signals
            }

            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            // Check if tasks acquired in FIFO order (1, 2, 3, 4)
            bool ordered = true;
            for (int32_t i = 0; i < (_STK_SEM_TEST_TASKS_MAX - 1); ++i)
            {
                if (g_AcquisitionOrder[i] != (i + 1))
                {
                    ordered = false;
                    printf("order violation: position %d has task %d (expected %d)\n",
                        (int)i, (int)g_AcquisitionOrder[i], (int)(i + 1));
                    break;
                }
            }

            if (ordered)
                g_TestResult = 1;
        }
        else
        {
            // Tasks 1-4: stagger entry so they queue in ascending order
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * m_task_id);

            g_TestSemaphore.Wait();

            // Record acquisition order
            int32_t idx = g_OrderIndex++;
            g_AcquisitionOrder[idx] = m_task_id;
        }
    }
};

/*! \class StressTestTask
    \brief Stress test with many interleaved Signal/Wait cycles.
    \note  Verifies semaphore stability under heavy contention by checking that
           no signal is lost: every Signal() produces exactly one successful Wait().
*/
template <EAccessMode _AccessMode>
class StressTestTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    StressTestTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: fire signals continuously
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                g_TestSemaphore.Signal();

                if ((i % 10) == 0)
                    stk::Delay(1);
            }
        }
        else
        {
            // Consumers: each drains its share of signals with a generous timeout
            int32_t share = m_iterations / (_STK_SEM_TEST_TASKS_MAX - 1);

            for (int32_t i = 0; i < share; ++i)
            {
                if (g_TestSemaphore.Wait(_STK_SEM_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }
        }

        if (m_task_id == (_STK_SEM_TEST_TASKS_MAX - 1))
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            // Remaining permits in the semaphore plus consumed ones must equal total signals
            int32_t total_consumed = g_SharedCounter;
            int32_t remaining      = (int32_t)g_TestSemaphore.GetCount();

            printf("stress test: consumed=%d remaining=%d total=%d (expected %d)\n",
                (int)total_consumed, (int)remaining, (int)(total_consumed + remaining), (int)m_iterations);

            if ((total_consumed + remaining) == m_iterations)
                g_TestResult = 1;
        }
    }
};

/*! \class BoundedBufferTask
    \brief Tests classic producer/consumer synchronization pattern.
    \note  Verifies that a semaphore correctly gates a producer and consumer:
           the producer signals N times, the consumer waits N times, and the
           total transfer count matches exactly.
*/
template <EAccessMode _AccessMode>
class BoundedBufferTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    BoundedBufferTask(uint8_t task_id, int32_t iterations) : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Producer: write items and signal readiness
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                stk::Delay(1); // simulate work
                g_TestSemaphore.Signal();
            }
        }
        else
        if (m_task_id == 1)
        {
            // Consumer: wait for each item produced by task 0
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                if (g_TestSemaphore.Wait(_STK_SEM_TEST_TIMEOUT))
                    ++g_SharedCounter;
            }
        }

        // Task 1 is the verifier
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            printf("bounded buffer: counter=%d (expected %d)\n", (int)g_SharedCounter, (int)m_iterations);

            if (g_SharedCounter == m_iterations)
                g_TestResult = 1;
        }
    }
};

/*! \class AlreadySignaledOnInitTask
    \brief Tests a semaphore constructed with \c initial_count == \c max_count,
           e.g. \c sync::Semaphore(1, 1).
    \note  Verifies the semaphore starts in an already-saturated state without
           any \c Signal()/TrySignal() call: \c GetCount() reflects the initial
           count right after construction, a \c TrySignal() at that point
           correctly reports failure (saturated, no waiters) rather than
           silently succeeding or asserting, and \c Wait() still drains the
           pre-loaded permit(s) via the normal fast path.
*/
template <EAccessMode _AccessMode>
class AlreadySignaledOnInitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    AlreadySignaledOnInitTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Semaphore is constructed as sync::Semaphore(1, 1) - see RunTest call site.
            uint16_t count_at_init = g_TestSemaphore.GetCount(); // already 1, no Signal() ever called

            bool signal_while_saturated = g_TestSemaphore.TrySignal(); // saturated, no waiters: expect false

            uint16_t count_after_trysignal = g_TestSemaphore.GetCount(); // must be unchanged

            bool first_wait  = g_TestSemaphore.Wait(NO_WAIT); // drains the pre-loaded permit: expect true
            bool second_wait = g_TestSemaphore.Wait(NO_WAIT); // now empty: expect false

            printf("already-signaled on init: count_at_init=%d(expect 1) signal_while_saturated=%d(expect 0) "\
                "count_after_trysignal=%d(expect 1) first_wait=%d(expect 1) second_wait=%d(expect 0)\n",
                (int)count_at_init, (int)signal_while_saturated, (int)count_after_trysignal,
                (int)first_wait, (int)second_wait);

            if ((count_at_init == 1) && !signal_while_saturated && (count_after_trysignal == 1) &&
                first_wait && !second_wait)
                g_TestResult = 1;
        }
    }
};

/*! \class TrySignalSaturationTask
    \brief Tests TrySignal() at the max_count boundary.
    \note  Verifies two branches that Signal() can never exercise (it asserts
           instead): TrySignal() returns false, and leaves the counter
           unchanged, when the counter is already at max_count and no task is
           waiting; and TrySignal() still succeeds via direct hand-off to a
           waiting task even while the counter sits at max_count, without
           incrementing the counter.
*/
template <EAccessMode _AccessMode>
class TrySignalSaturationTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TrySignalSaturationTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Semaphore starts at count=0, max_count=1 (see RunTest call site).
            bool first_signal_ok  = g_TestSemaphore.TrySignal(); // 0 -> 1 (now saturated)
            bool second_signal_ok = g_TestSemaphore.TrySignal(); // saturated, no waiters: expect false

            uint16_t count_after_saturation = g_TestSemaphore.GetCount();

            bool drained = g_TestSemaphore.Wait(NO_WAIT); // 1 -> 0, resets for the hand-off case

            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // let task 1 block on Wait()

            bool handoff_ok = g_TestSemaphore.TrySignal(); // waiter present: hands off directly

            uint16_t count_after_handoff = g_TestSemaphore.GetCount(); // must stay 0 (not incremented)

            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            printf("trysignal saturation: first=%d second=%d(expect 0) count=%d(expect 1) "\
                "drained=%d handoff=%d(expect 1) count_after_handoff=%d(expect 0) waiter_acquired=%d(expect 1)\n",
                (int)first_signal_ok, (int)second_signal_ok, (int)count_after_saturation, (int)drained,
                (int)handoff_ok, (int)count_after_handoff, (int)g_SharedCounter);

            if (first_signal_ok && !second_signal_ok && (count_after_saturation == 1) &&
                drained && handoff_ok && (count_after_handoff == 0) && (g_SharedCounter == 1))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Blocks before task 0's hand-off TrySignal() so that call has a waiter to hand off to.
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP / 2);

            if (g_TestSemaphore.Wait(_STK_SEM_TEST_TIMEOUT))
                g_SharedCounter = 1;
        }
    }
};

/*! \class TrySignalCeilingTask
    \brief Tests TrySignal(ceiling) without waiters.
    \note  Verifies that the per-call ceiling limits the counter when it is below max_count, that it is
           clamped to max_count when it is above it, that a zero ceiling rejects the signal, that a ceiling
           below the current count rejects the signal and leaves the counter unchanged, and that the plain
           TrySignal() still works with the per-object maximum afterwards.
*/
template <EAccessMode _AccessMode>
class TrySignalCeilingTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TrySignalCeilingTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id != 0)
            return;

        // (1) ceiling below max_count: g_TestSemaphore is constructed as (0, 10) - see RunTest call site
        int32_t accepted = 0;
        for (int32_t i = 0; i < 5; ++i)
        {
            if (g_TestSemaphore.TrySignal(3U))
                ++accepted;
        }
        const int32_t count_below = (int32_t)g_TestSemaphore.GetCount();

        // (2) ceiling above max_count is clamped to max_count
        sync::Semaphore clamped(0, 2);
        int32_t accepted_clamped = 0;
        for (int32_t i = 0; i < 5; ++i)
        {
            if (clamped.TrySignal(100U))
                ++accepted_clamped;
        }
        const int32_t count_clamped = (int32_t)clamped.GetCount();

        // (3) zero ceiling on an empty semaphore: rejected, counter unchanged
        sync::Semaphore zero_ceiling(0, 2);
        const bool    zero_ok    = zero_ceiling.TrySignal(0U);
        const int32_t count_zero = (int32_t)zero_ceiling.GetCount();

        // (4) ceiling below the current count (3): rejected, counter unchanged
        const bool    lower_ok    = g_TestSemaphore.TrySignal(2U);
        const int32_t count_lower = (int32_t)g_TestSemaphore.GetCount();

        // (5) plain TrySignal() still uses the per-object maximum (10), not the earlier ceiling
        const bool    plain_ok    = g_TestSemaphore.TrySignal();
        const int32_t count_plain = (int32_t)g_TestSemaphore.GetCount();

        printf("trysignal ceiling: below=%d/%d(expect 3/3) clamped=%d/%d(expect 2/2) zero=%d/%d(expect 0/0) "\
            "lower=%d/%d(expect 0/3) plain=%d/%d(expect 1/4)\n",
            (int)accepted, (int)count_below, (int)accepted_clamped, (int)count_clamped,
            (int)zero_ok, (int)count_zero, (int)lower_ok, (int)count_lower, (int)plain_ok, (int)count_plain);

        if ((accepted == 3) && (count_below == 3) &&
            (accepted_clamped == 2) && (count_clamped == 2) &&
            !zero_ok && (count_zero == 0) &&
            !lower_ok && (count_lower == 3) &&
            plain_ok && (count_plain == 4))
            g_TestResult = 1;
    }
};

/*! \class TrySignalCeilingHandoffTask
    \brief Tests TrySignal(ceiling) with a waiting task.
    \note  Verifies that a waiter always gets the token by direct hand-off even if the ceiling is already
           exceeded (ceiling 0), that the counter is not incremented by the hand-off, and that the same
           call without a waiter is rejected.
*/
template <EAccessMode _AccessMode>
class TrySignalCeilingHandoffTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TrySignalCeilingHandoffTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id == 0)
        {
            // Semaphore starts at count=0, max_count=10 (see RunTest call site).
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // let task 1 block on Wait()

            const bool    handoff_ok          = g_TestSemaphore.TrySignal(0U); // waiter present: must hand off
            const int32_t count_after_handoff = (int32_t)g_TestSemaphore.GetCount(); // must stay 0

            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // let task 1 record the acquisition

            const bool    no_waiter_ok         = g_TestSemaphore.TrySignal(0U); // nobody waits, ceiling 0: rejected
            const int32_t count_after_no_waiter = (int32_t)g_TestSemaphore.GetCount();

            printf("trysignal ceiling handoff: handoff=%d(expect 1) count=%d(expect 0) waiter_acquired=%d(expect 1) "\
                "no_waiter=%d(expect 0) count=%d(expect 0)\n",
                (int)handoff_ok, (int)count_after_handoff, (int)g_SharedCounter,
                (int)no_waiter_ok, (int)count_after_no_waiter);

            if (handoff_ok && (count_after_handoff == 0) && (g_SharedCounter == 1) &&
                !no_waiter_ok && (count_after_no_waiter == 0))
                g_TestResult = 1;
        }
        else
        if (m_task_id == 1)
        {
            // Blocks before task 0's TrySignal(0) so that call has a waiter to hand off to.
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP / 2);

            if (g_TestSemaphore.Wait(_STK_SEM_TEST_TIMEOUT))
                g_SharedCounter = 1;
        }
    }
};

/*! \class WideCounter32Task
    \brief Tests SemaphoreT<uint32_t> beyond the 16-bit range.
    \note  Verifies that the initial count is not truncated, that the counter does not wrap at 65535,
           and that the top of the range is handled correctly: a semaphore constructed as
           (COUNT_MAX, COUNT_MAX) rejects TrySignal(), drops by one on Wait() and is restored by TrySignal().
*/
template <EAccessMode _AccessMode>
class WideCounter32Task : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WideCounter32Task(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id != 0)
            return;

        typedef sync::SemaphoreT<uint32_t> Sem32;

        // (1) initial count above the 16-bit range must not be truncated
        Sem32 wide(70000U);
        const uint32_t count_at_init = wide.GetCount();
        const bool     taken         = wide.Wait(NO_WAIT);
        const uint32_t count_after   = wide.GetCount();

        // (2) the counter must not wrap at 65535
        Sem32 crossing(0xFFFFU);
        const bool     crossing_ok    = crossing.TrySignal();
        const uint32_t count_crossing = crossing.GetCount();

        // (3) top of the range: constructed saturated
        Sem32 full(Sem32::COUNT_MAX, Sem32::COUNT_MAX);
        const bool     signal_full    = full.TrySignal();           // saturated, no waiters: expect false
        const bool     signal_ceiling = full.TrySignal(Sem32::COUNT_MAX); // same via the ceiling overload
        const bool     taken_full     = full.Wait(NO_WAIT);         // COUNT_MAX -> COUNT_MAX - 1
        const uint32_t count_minus    = full.GetCount();
        const bool     signal_restore = full.TrySignal();           // COUNT_MAX - 1 -> COUNT_MAX
        const uint32_t count_restored = full.GetCount();

        printf("wide counter 32: init=%lu(expect 70000) taken=%d after=%lu(expect 69999) crossing=%d count=%lu(expect 65536)\n",
            (unsigned long)count_at_init, (int)taken, (unsigned long)count_after,
            (int)crossing_ok, (unsigned long)count_crossing);
        printf("wide counter 32: full signal=%d ceiling=%d(expect 0/0) taken=%d minus=%lu(expect 4294967293) "\
            "restore=%d restored=%lu(expect 4294967294)\n",
            (int)signal_full, (int)signal_ceiling, (int)taken_full, (unsigned long)count_minus,
            (int)signal_restore, (unsigned long)count_restored);

        if ((count_at_init == 70000U) && taken && (count_after == 69999U) &&
            crossing_ok && (count_crossing == 0x10000U) &&
            !signal_full && !signal_ceiling && taken_full &&
            (count_minus == (Sem32::COUNT_MAX - 1U)) &&
            signal_restore && (count_restored == Sem32::COUNT_MAX))
            g_TestResult = 1;
    }
};

/*! \class NarrowCounter8Task
    \brief Tests SemaphoreT<uint8_t> at its counter limit.
    \note  Verifies that exactly COUNT_MAX (254) signals are accepted, the next one is rejected without
           wrapping the counter, and that draining yields exactly 254 successful waits.
*/
template <EAccessMode _AccessMode>
class NarrowCounter8Task : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    NarrowCounter8Task(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run()
    {
        if (m_task_id != 0)
            return;

        typedef sync::SemaphoreT<uint8_t> Sem8;

        Sem8 narrow; // default max_count = COUNT_MAX = 254

        int32_t accepted = 0;
        for (int32_t i = 0; i < 300; ++i)
        {
            if (narrow.TrySignal())
                ++accepted;
        }
        const int32_t count_full = (int32_t)narrow.GetCount();

        int32_t drained = 0;
        while (narrow.Wait(NO_WAIT))
            ++drained;
        const int32_t count_empty = (int32_t)narrow.GetCount();

        printf("narrow counter 8: accepted=%d(expect 255) count=%d(expect 255) drained=%d(expect 255) count=%d(expect 0)\n",
            (int)accepted, (int)count_full, (int)drained, (int)count_empty);

        if ((accepted == 255) && (count_full == 255) && (drained == 255) && (count_empty == 0))
            g_TestResult = 1;
    }
};

/*! \class WideCounter64Task
    \brief Tests SemaphoreT<uint64_t> beyond the 32-bit range.
    \note  Verifies that an initial count above 32 bits is not truncated, that the counter does not wrap at
           2^32, that a per-call ceiling above 32 bits works, and that the top of the range is handled
           correctly: a semaphore constructed as (COUNT_MAX, COUNT_MAX) rejects TrySignal(), drops by one on
           Wait() and is restored by TrySignal(). GetCount() of a counter wider than the native word is read
           under the critical section (see SemaphoreT::GetCount()), so on a 32-bit target it can not be torn.
           Values are printed as two 32-bit halves because a 64-bit printf conversion is not always available.
*/
template <EAccessMode _AccessMode>
class WideCounter64Task : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WideCounter64Task(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    static unsigned long Hi(uint64_t v) { return (unsigned long)(v >> 32); }
    static unsigned long Lo(uint64_t v) { return (unsigned long)(v & 0xFFFFFFFFULL); }

    void Run()
    {
        if (m_task_id != 0)
            return;

        typedef sync::SemaphoreT<uint64_t> Sem64;

        static const uint64_t BIT32 = 0x100000000ULL; // first value which does not fit 32 bits

        // (1) initial count above the 32-bit range must not be truncated
        Sem64 wide(BIT32 + 5U);
        const uint64_t count_at_init = wide.GetCount();
        const bool     taken         = wide.Wait(NO_WAIT);
        const uint64_t count_after   = wide.GetCount();

        // (2) the counter must not wrap at 2^32
        Sem64 crossing(BIT32 - 1U);
        const bool     crossing_ok    = crossing.TrySignal();
        const uint64_t count_crossing = crossing.GetCount();

        // (3) a per-call ceiling above the 32-bit range: accepted up to the ceiling, then rejected
        Sem64 ceil_sem(BIT32 - 1U);
        const bool     ceiling_ok     = ceil_sem.TrySignal(BIT32);  // BIT32 - 1 -> BIT32 (reaches the ceiling)
        const bool     ceiling_full   = ceil_sem.TrySignal(BIT32);  // at the ceiling: rejected
        const uint64_t count_ceiling  = ceil_sem.GetCount();

        // (4) top of the range: constructed saturated
        Sem64 full(Sem64::COUNT_MAX, Sem64::COUNT_MAX);
        const bool     signal_full    = full.TrySignal();                 // saturated, no waiters: expect false
        const bool     signal_ceiling = full.TrySignal(Sem64::COUNT_MAX); // same via the ceiling overload
        const bool     taken_full     = full.Wait(NO_WAIT);               // COUNT_MAX -> COUNT_MAX - 1
        const uint64_t count_minus    = full.GetCount();
        const bool     signal_restore = full.TrySignal();                 // COUNT_MAX - 1 -> COUNT_MAX
        const uint64_t count_restored = full.GetCount();

        printf("wide counter 64: init=0x%08lX%08lX(expect 0x0000000100000005) taken=%d after=0x%08lX%08lX(expect 0x0000000100000004)\n",
            Hi(count_at_init), Lo(count_at_init), (int)taken, Hi(count_after), Lo(count_after));
        printf("wide counter 64: crossing=%d count=0x%08lX%08lX(expect 0x0000000100000000) ceiling=%d/%d(expect 1/0) "\
            "count=0x%08lX%08lX(expect 0x0000000100000000)\n",
            (int)crossing_ok, Hi(count_crossing), Lo(count_crossing), (int)ceiling_ok, (int)ceiling_full,
            Hi(count_ceiling), Lo(count_ceiling));
        printf("wide counter 64: full signal=%d ceiling=%d(expect 0/0) taken=%d minus=0x%08lX%08lX(expect 0xFFFFFFFFFFFFFFFD) "\
            "restore=%d restored=0x%08lX%08lX(expect 0xFFFFFFFFFFFFFFFE)\n",
            (int)signal_full, (int)signal_ceiling, (int)taken_full, Hi(count_minus), Lo(count_minus),
            (int)signal_restore, Hi(count_restored), Lo(count_restored));

        if ((count_at_init == (BIT32 + 5U)) && taken && (count_after == (BIT32 + 4U)) &&
            crossing_ok && (count_crossing == BIT32) &&
            ceiling_ok && !ceiling_full && (count_ceiling == BIT32) &&
            !signal_full && !signal_ceiling && taken_full &&
            (count_minus == (Sem64::COUNT_MAX - 1U)) &&
            signal_restore && (count_restored == Sem64::COUNT_MAX))
            g_TestResult = 1;
    }
};

static const int32_t CW_INFINITE         = 1;
static const int32_t CW_NOT_TIMEOUT      = 2;
static const int32_t CW_NO_STALE_WAITER  = 3;
static const int32_t CW_TWO_WAITERS      = 4;
static const int32_t CW_SIGNAL_WINS      = 5;
static const int32_t CW_BOOL_WAIT        = 6;
static const int32_t CW_RETRY_AFTER      = 7;
static const int32_t CW_VIA_SLEEPCANCEL  = 100;

/*! \class CancelWaitTask
    \brief Tests cancelability of Semaphore waits (IKernel::CancelTaskWait() and stk::SleepCancel()).
    \note  Task 0 is the canceller/checker, task 1 is the victim (blocked in WaitEx()), task 2 is a second
           waiter used by the two-waiters scenario. The semaphore starts at count 0, so a Signal() either
           hands its token to a waiter (count stays 0, Direct Handover) or is remembered (count + 1): which
           of the two happened shows whether a cancelled task is still, wrongly, in the wait list. The
           scenario is selected by g_CancelParam: 1 = cancel an infinite wait (WAIT_RESULT_CANCELED),
           2 = cancel is not reported as a timeout, 3 = cancel leaves no stale waiter behind (the next
           Signal() calls are remembered, none is swallowed), 4 = cancelling one of two waiters hands the
           next token to the other one, 5 = a Signal() that arrived first wins over a late cancel (the token
           is neither lost nor duplicated), 6 = the boolean Wait() collapses a cancel into false, 7 = a
           cancelled task can wait on the same semaphore again and receives the next token. Adding 100 to
           g_CancelParam cancels via stk::SleepCancel() instead of IKernel::CancelTaskWait().
*/
template <EAccessMode _AccessMode>
class CancelWaitTask : public Task<_STK_SEM_STACK_SIZE, _AccessMode>
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

    // Task 1: block on the semaphore, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        const Timeout timeout = (m_scenario == CW_NOT_TIMEOUT ? 300 : WAIT_INFINITE);

        const int64_t start = GetTimeNowMs();
        int32_t result;

        if (m_scenario == CW_BOOL_WAIT)
        {
            // boolean variant: a cancel is reported as false, same as a timeout (an infinite wait can not
            // time out, so false means cancelled here); stored as 1 = acquired / 0 = not acquired
            result = g_TestSemaphore.Wait(timeout) ? 1 : 0;
        }
        else
        {
            result = static_cast<int32_t>(g_TestSemaphore.WaitEx(timeout));
        }

        g_VictimElapsed[0] = GetTimeNowMs() - start;
        g_VictimResult[0]  = result;

        if (m_scenario == CW_NO_STALE_WAITER)
        {
            // a stale wait entry left behind by the cancel would cut this sleep short
            const int64_t sleep_start = GetTimeNowMs();
            stk::Sleep(60);
            g_VictimSleepElapsed = GetTimeNowMs() - sleep_start;
        }
        else
        if ((m_scenario == CW_RETRY_AFTER) && (result == WAIT_RESULT_CANCELED))
        {
            // wait on the same semaphore again after having been cancelled
            const int64_t start2 = GetTimeNowMs();
            const EWaitResult r2 = g_TestSemaphore.WaitEx(WAIT_INFINITE);

            g_VictimElapsed[1] = GetTimeNowMs() - start2;
            g_VictimResult[1]  = static_cast<int32_t>(r2);
        }
    }

    // Task 2: second waiter, queued behind task 1
    void RunSecondWaiter()
    {
        if (m_scenario != CW_TWO_WAITERS)
            return;

        stk::Sleep(2 * _STK_SEM_TEST_SHORT_SLEEP); // block after task 1

        const int64_t start = GetTimeNowMs();
        const EWaitResult r = g_TestSemaphore.WaitEx(WAIT_INFINITE);

        g_VictimElapsed[1] = GetTimeNowMs() - start;
        g_VictimResult[1]  = static_cast<int32_t>(r);
    }

    // Task 0: cancel and verify
    void RunCanceller()
    {
        bool pass = false;

        stk::Sleep(60); // task 1 (and task 2) are blocked in the wait by now

        if (m_scenario == CW_INFINITE)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * 2);

            // started waiting at ~10 ms, cancelled at ~60 ms; the cancel must not create a permit
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   (g_TestSemaphore.GetCount() == 0U);
        }
        else
        if (m_scenario == CW_NOT_TIMEOUT)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * 2);

            // 300 ms timeout is far away: the wait must end early and as CANCELED, not TIMEOUT
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimElapsed[0] < 150) &&
                   (g_TestSemaphore.GetCount() == 0U);
        }
        else
        if (m_scenario == CW_NO_STALE_WAITER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP); // victim returns and starts its Sleep(60)

            // nobody waits: both tokens must be remembered; a stale wait entry would swallow the first one
            g_TestSemaphore.Signal();
            g_TestSemaphore.Signal();

            const int32_t count_after = (int32_t)g_TestSemaphore.GetCount();

            const bool take1 = g_TestSemaphore.Wait(NO_WAIT);
            const bool take2 = g_TestSemaphore.Wait(NO_WAIT);
            const bool take3 = g_TestSemaphore.Wait(NO_WAIT); // nothing left

            stk::Sleep(70); // let the victim finish its sleep

            printf("cancel no stale waiter: result=%d count=%d take=%d%d%d victim_sleep=%d ms\n",
                (int)g_VictimResult[0], (int)count_after, (int)take1, (int)take2, (int)take3,
                (int)g_VictimSleepElapsed);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (count_after == 2) && take1 && take2 && !take3 &&
                   (g_VictimSleepElapsed >= 55); // not woken early by a ghost wait entry
        }
        else
        if (m_scenario == CW_TWO_WAITERS)
        {
            CancelVictim(m_use_sleep_cancel);                // cancel the first waiter only
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            g_TestSemaphore.Signal();                        // must be handed over to the second waiter
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * 2);

            const int32_t count_after = (int32_t)g_TestSemaphore.GetCount(); // handed over, not remembered
            const bool    leftover    = g_TestSemaphore.Wait(NO_WAIT);

            printf("cancel two waiters: first=%d second=%d count=%d leftover=%d\n",
                (int)g_VictimResult[0], (int)g_VictimResult[1], (int)count_after, (int)leftover);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimResult[1] == WAIT_RESULT_SIGNAL) &&
                   (count_after == 0) && !leftover;
        }
        else
        if (m_scenario == CW_SIGNAL_WINS)
        {
            // back to back, no scheduling point in between: the waiter already received the token and is
            // removed from the wait list when the cancel arrives, so the cancel must be a no-op
            g_TestSemaphore.Signal();
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * 2);

            const int32_t count_after = (int32_t)g_TestSemaphore.GetCount();
            const bool    leftover    = g_TestSemaphore.Wait(NO_WAIT); // the token must not be duplicated

            pass = (g_VictimResult[0] == WAIT_RESULT_SIGNAL) && (count_after == 0) && !leftover;
        }
        else
        if (m_scenario == CW_BOOL_WAIT)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);

            g_TestSemaphore.Signal();                        // nobody waits: must be remembered

            const int32_t count_after = (int32_t)g_TestSemaphore.GetCount();
            const bool    take        = g_TestSemaphore.Wait(NO_WAIT);

            printf("cancel bool wait: acquired=%d elapsed=%d ms count=%d take=%d\n",
                (int)g_VictimResult[0], (int)g_VictimElapsed[0], (int)count_after, (int)take);

            // an infinite wait can not time out: false after ~50 ms means the cancel ended it
            pass = (g_VictimResult[0] == 0) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   (count_after == 1) && take;
        }
        else
        if (m_scenario == CW_RETRY_AFTER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * 2);

            // victim got CANCELED and is waiting again
            const bool waiting_again = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimResult[1] == -100);

            g_TestSemaphore.Signal();                        // must be handed over to the retrying victim
            stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP * 2);

            const int32_t count_after = (int32_t)g_TestSemaphore.GetCount();

            printf("cancel retry: first=%d second=%d count=%d\n",
                (int)g_VictimResult[0], (int)g_VictimResult[1], (int)count_after);

            pass = waiting_again && (g_VictimResult[1] == WAIT_RESULT_SIGNAL) && (count_after == 0);
        }

        // Safety net: if the cancel did not work a waiter is still blocked forever and the kernel would
        // never stop, release any such waiter so that the test fails instead of hanging.
        if (!pass)
        {
            for (int32_t i = 0; i < 3; ++i)
            {
                g_TestSemaphore.Signal();
                stk::Sleep(_STK_SEM_TEST_SHORT_SLEEP);
            }
        }

        printf("cancel wait (scenario=%d, via %s): result[0]=%d elapsed[0]=%d ms\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
            (int)g_VictimResult[0], (int)g_VictimElapsed[0]);

        if (pass)
            g_TestResult = 1;
    }
};

// Helper function to reset test state
static void ResetTestState(uint32_t initial_count = 0, uint32_t max_count = sync::Semaphore::COUNT_MAX)
{
    g_TestResult  = 0;
    g_SharedCounter = 0;
    g_OrderIndex  = 0;
    g_TestComplete = false;

    for (int32_t i = 0; i < _STK_SEM_TEST_TASKS_MAX; ++i)
        g_AcquisitionOrder[i] = 0;

    g_Victim             = nullptr;
    g_VictimTid          = TID_NONE;
    g_VictimSleepElapsed = 0;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]  = -100;
        g_VictimElapsed[i] = 0;
    }

    // Re-construct the semaphore in-place with the requested initial count and max count
    g_TestSemaphore.~SemaphoreT();
    new (&g_TestSemaphore) sync::Semaphore(initial_count, max_count);
}

} // namespace semaphore
} // namespace test
} // namespace stk

static bool NeedsExtendedTasks(const char *test_name)
{
    return (strcmp(test_name, "TimeoutWait") != 0) &&
           (strcmp(test_name, "ZeroTimeout") != 0) &&
           (strcmp(test_name, "SignalBeforeWait") != 0) &&
           (strcmp(test_name, "BoundedBuffer") != 0) &&
           (strcmp(test_name, "TrySignalSaturation") != 0) &&
           (strcmp(test_name, "AlreadySignaledOnInit") != 0) &&
           (strcmp(test_name, "TrySignalCeiling") != 0) &&
           (strcmp(test_name, "TrySignalCeilingHandoff") != 0) &&
           (strcmp(test_name, "WideCounter32") != 0) &&
           (strcmp(test_name, "NarrowCounter8") != 0) &&
           (strcmp(test_name, "WideCounter64") != 0) &&
           (strncmp(test_name, "CancelWait", 10) != 0); // cancel tests use tasks 0-2 only
}

/*! \fn    RunTest
    \brief Helper function to run a single test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0, uint32_t initial_count = 0,
    uint32_t max_count = sync::Semaphore::COUNT_MAX)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::semaphore;

    printf("Test: %s\n", test_name);

    ResetTestState(initial_count, max_count);

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

    using namespace stk::test::semaphore;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    g_Kernel.Initialize();

#ifndef __ARM_ARCH_6M__

    // Test 1: Basic Signal/Wait with producer-consumer mutual exclusion
    if (RunTest<BasicSignalWaitTask<ACCESS_PRIVILEGED>>("BasicSignalWait", 100) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 2: Non-zero initial count (fast-path drain, then block)
    if (RunTest<InitialCountTask<ACCESS_PRIVILEGED>>("InitialCount", 0, _STK_SEM_TEST_TASKS_MAX - 1) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 3: TimedWait timeout behavior
    if (RunTest<TimeoutWaitTask<ACCESS_PRIVILEGED>>("TimeoutWait") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 4: Wait(0) non-blocking behavior
    if (RunTest<ZeroTimeoutTask<ACCESS_PRIVILEGED>>("ZeroTimeout") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 5: Signal posted before Wait is remembered
    if (RunTest<SignalBeforeWaitTask<ACCESS_PRIVILEGED>>("SignalBeforeWait", 200) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 6: FIFO wakeup ordering
    if (RunTest<FIFOOrderTask<ACCESS_PRIVILEGED>>("FIFOOrder") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 7: Classic bounded-buffer producer/consumer
    if (RunTest<BoundedBufferTask<ACCESS_PRIVILEGED>>("BoundedBuffer", 200) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 8: TrySignal() at the max_count boundary - rejection without a waiter, hand-off with one
    if (RunTest<TrySignalSaturationTask<ACCESS_PRIVILEGED>>("TrySignalSaturation", 0, 0, 1) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 9: Semaphore(1, 1) - already-saturated state at construction time, no Signal() called
    if (RunTest<AlreadySignaledOnInitTask<ACCESS_PRIVILEGED>>("AlreadySignaledOnInit", 0, 1, 1) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 10: TrySignal(ceiling) without waiters - below/above max_count, zero ceiling, ceiling below count
    if (RunTest<TrySignalCeilingTask<ACCESS_PRIVILEGED>>("TrySignalCeiling", 0, 0, 10) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 11: TrySignal(ceiling) with a waiter - hand-off must succeed even if the ceiling is exceeded
    if (RunTest<TrySignalCeilingHandoffTask<ACCESS_PRIVILEGED>>("TrySignalCeilingHandoff", 0, 0, 10) != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 12: SemaphoreT<uint32_t> - values beyond 16 bits and the top of the range
    if (RunTest<WideCounter32Task<ACCESS_PRIVILEGED>>("WideCounter32") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 13: SemaphoreT<uint8_t> - exactly COUNT_MAX (254) signals accepted
    if (RunTest<NarrowCounter8Task<ACCESS_PRIVILEGED>>("NarrowCounter8") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Test 14: SemaphoreT<uint64_t> - values beyond 32 bits, ceiling above 32 bits and the top of the range
    if (RunTest<WideCounter64Task<ACCESS_PRIVILEGED>>("WideCounter64") != TestContext::SUCCESS_EXIT_CODE)
        total_failures++;
    else
        total_success++;

    // Tests 15-26: cancelability of Semaphore waits (tasks 0-2 only), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        struct CancelCase { const char *name; int32_t param; };

        static const CancelCase cases[] =
        {
            { "CancelWait_Infinite",            CW_INFINITE                              },
            { "CancelWait_NotTimeout",          CW_NOT_TIMEOUT                           },
            { "CancelWait_NoStaleWaiter",       CW_NO_STALE_WAITER                       },
            { "CancelWait_TwoWaiters",          CW_TWO_WAITERS                           },
            { "CancelWait_SignalWins",          CW_SIGNAL_WINS                           },
            { "CancelWait_BoolWait",            CW_BOOL_WAIT                             },
            { "CancelWait_RetryAfter",          CW_RETRY_AFTER                           },
            { "CancelWait_SleepCancel",         CW_VIA_SLEEPCANCEL + CW_INFINITE         },
            { "CancelWait_SleepCancelNoStale",  CW_VIA_SLEEPCANCEL + CW_NO_STALE_WAITER  },
            { "CancelWait_SleepCancelTwo",      CW_VIA_SLEEPCANCEL + CW_TWO_WAITERS      },
            { "CancelWait_SleepCancelSignal",   CW_VIA_SLEEPCANCEL + CW_SIGNAL_WINS      },
            { "CancelWait_SleepCancelBool",     CW_VIA_SLEEPCANCEL + CW_BOOL_WAIT        }
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

    // Test 27: Stress test (no signals lost)
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
