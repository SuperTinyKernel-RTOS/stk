/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <functional>
#include <vector>

#include "stktest.h"

namespace stk {
namespace test {

// ============================================================================ //
// ============================= PeriodicTimer ================================ //
// ============================================================================ //

//! Mock of GetTimeNowMs().
namespace stk
{
    static struct KernelServiceMock : public IKernelService
    {
        int64_t ticks;
        int32_t resolution;

        static IKernelService *GetInstance() { return NULL; }
        TId GetTid() const override { return 0; }
        Ticks GetTicks() const override { return ticks; }
        uint32_t GetTickResolution() const override { return resolution; }
        Cycles GetSysTimerCount() const override { return 0; }
        uint32_t GetSysTimerFrequency() const override { return 0; }
        void Delay(Timeout ticks) override { (void)ticks; }
        bool Sleep(Timeout ticks) override { (void)ticks; return false; }
        bool SleepUntil(Ticks timestamp) override { (void)timestamp; return false; }
        void SleepCancel(TId task_id) override { (void)task_id; }
        void SwitchToNext() override {}
        EWaitResult Wait(ISyncObject *sobj, IMutex *mutex, Timeout timeout) override
        {
            STK_UNUSED(sobj);
            STK_UNUSED(mutex);
            STK_UNUSED(timeout);
            return WAIT_RESULT_FAIL;
        }
        TId Wake(ISyncObject *sobj, bool all) override
        {
            STK_UNUSED(sobj);
            STK_UNUSED(all);

            return TID_NONE;
        }
        Timeout Suspend() override { return 1; }
        void Resume(Timeout elapsed_ticks) override { (void)elapsed_ticks; }
        void InheritWeight(TId tid, TId donor_tid) override
        {
            (void)tid;
            (void)donor_tid;
        }
        void InheritWeight(TId tid, const ISyncObject *sobj) override
        {
            (void)tid;
            (void)sobj;
        }
        void RestoreWeight(TId tid, ISyncObject *sobj) override { (void)tid; (void)sobj; }
        Weight SetWeight(TId tid, Weight weight) override
        {
            (void)tid;
            (void)weight;
            return weight;
        }
        Weight GetWeight(TId tid) const
        {
            (void)tid;
            return NO_WEIGHT;
        }
        Timeout SetTimeSlice(TId tid, Timeout ticks) override
        {
            (void)tid;
            (void)ticks;
            return NO_TIME_SLICE;
        }
        void ExitTask() override {}
        ELockResult LockScheduler() override { return LOCK_FAIL; }
        ELockResult UnlockScheduler() override { return LOCK_FAIL; }
        ELockResult RestoreSchedulerLock(bool locked) override
        {
            STK_UNUSED(locked);
            return LOCK_FAIL;
        }
        EWaitOrder GetDefaultWaitOrder() const override
        {
            return WAIT_ORDER_FIFO;
        }
        IWaitObject *SelectWaiter(IWaitObject::ListHeadType &wlist, EWaitOrder order) override
        {
            return nullptr;
        }
        void WakeDeleted(ISyncObject *sobj) override
        {
            STK_UNUSED(sobj);
        }
    }
    s_KernelServiceMock;

    void SetTimeNowMsec(int64_t now)
    {
        test::g_KernelService = &s_KernelServiceMock;

        s_KernelServiceMock.resolution = 1000;
        s_KernelServiceMock.ticks = GetTicksFromMs(now, s_KernelServiceMock.resolution);
    }
}

TEST_GROUP(PeriodicTrigger)
{
    enum { PERIOD = 100 };

    void setup()
    {
        stk::SetTimeNowMsec(0);
    }
};

TEST(PeriodicTrigger, MustBeArmed)
{
    time::PeriodicTrigger trigger(PERIOD, false);

    stk::SetTimeNowMsec(50);

    try
    {
        g_TestContext.ExpectAssert(true);
        CHECK_FALSE(trigger.Poll());
        CHECK_TRUE(false); // Poll() must fail
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(PeriodicTrigger, DoesNotFireBeforePeriod)
{
    time::PeriodicTrigger trigger(PERIOD, true);

    stk::SetTimeNowMsec(50);
    CHECK_FALSE(trigger.Poll());
}

TEST(PeriodicTrigger, FiresAtExactPeriod)
{
    time::PeriodicTrigger trigger(PERIOD, true);

    stk::SetTimeNowMsec(100);
    CHECK_TRUE(trigger.Poll());
}

TEST(PeriodicTrigger, PreservesRemainderAfterFire)
{
    time::PeriodicTrigger trigger(PERIOD, true);

    // 1. Move to 150ms.
    // m_elapsed becomes 150. Poll() returns true.
    // m_elapsed becomes 150 - 100 = 50ms (the remainder).
    stk::SetTimeNowMsec(150);
    CHECK_TRUE(trigger.Poll());

    // 2. Move to 190ms.
    // Time delta is 40ms (190 - 150).
    // m_elapsed = 50 (remainder) + 40 (delta) = 90ms.
    stk::SetTimeNowMsec(190);
    CHECK_FALSE(trigger.Poll());

    // 3. Move to 205ms.
    // Time delta is 15ms (205 - 190).
    // m_elapsed = 90 + 15 = 105ms. Fires!
    stk::SetTimeNowMsec(205);
    CHECK_TRUE(trigger.Poll());
}

TEST(PeriodicTrigger, AccumulatesAcrossMultiplePolls)
{
    time::PeriodicTrigger trigger(PERIOD, true);

    stk::SetTimeNowMsec(40);
    CHECK_FALSE(trigger.Poll());

    stk::SetTimeNowMsec(80);
    CHECK_FALSE(trigger.Poll());

    stk::SetTimeNowMsec(110);
    CHECK_TRUE(trigger.Poll()); // Total 110ms accumulated
}

TEST(PeriodicTrigger, ManualResetSyncsToCurrentTime)
{
    time::PeriodicTrigger trigger(PERIOD, true);

    stk::SetTimeNowMsec(80);
    CHECK_FALSE(trigger.Poll()); // Accumulated 80ms

    // Re-constructing or manually resetting m_prev/m_elapsed
    // allows a "hard reset" to the current mock time.
    trigger = time::PeriodicTrigger(PERIOD, true);

    stk::SetTimeNowMsec(150);
    // Delta is only 70ms (150 - 80) since m_prev was set to 80 during reset/re-init.
    CHECK_FALSE(trigger.Poll());
}

// ============================================================================ //
// ================================ Stopwatch ================================= //
// ============================================================================ //

namespace
{
    //! Mock cycle counter for Stopwatch tests.
    Cycles s_MockCycles = 0;

    Cycles MockNow() { return s_MockCycles; }

    //! Functor time source.
    struct MockNowFunctor
    {
        Cycles operator()() const { return s_MockCycles; }
    };
}

TEST_GROUP(Stopwatch)
{
    void setup()
    {
        s_MockCycles = 0;
    }
};

TEST(Stopwatch, UpdateWithoutStartReturnsZero)
{
    time::Stopwatch sw;

    s_MockCycles = 1000;
    CHECK_EQUAL(0, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, UpdateWithoutStartBecomesReferencePoint)
{
    time::Stopwatch sw;

    s_MockCycles = 1000;
    CHECK_EQUAL(0, (int64_t)sw.Update(MockNow)); // implicit start at 1000

    s_MockCycles = 1250;
    CHECK_EQUAL(250, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, MeasuresElapsedSinceStart)
{
    time::Stopwatch sw;

    s_MockCycles = 100;
    sw.Start(MockNow);

    s_MockCycles = 175;
    CHECK_EQUAL(75, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, SuccessiveUpdatesMeasureIntervals)
{
    time::Stopwatch sw;

    s_MockCycles = 100;
    sw.Start(MockNow);

    s_MockCycles = 150;
    CHECK_EQUAL(50, (int64_t)sw.Update(MockNow));

    s_MockCycles = 170;
    CHECK_EQUAL(20, (int64_t)sw.Update(MockNow)); // since previous Update(), not Start()

    s_MockCycles = 400;
    CHECK_EQUAL(230, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, NoTimeElapsedReturnsZero)
{
    time::Stopwatch sw;

    s_MockCycles = 500;
    sw.Start(MockNow);

    CHECK_EQUAL(0, (int64_t)sw.Update(MockNow));
    CHECK_EQUAL(0, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, StartResetsReferencePoint)
{
    time::Stopwatch sw;

    s_MockCycles = 100;
    sw.Start(MockNow);

    s_MockCycles = 200;
    CHECK_EQUAL(100, (int64_t)sw.Update(MockNow));

    // Restart at a later moment: elapsed time must be measured from the new start.
    s_MockCycles = 1000;
    sw.Start(MockNow);

    s_MockCycles = 1030;
    CHECK_EQUAL(30, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, StartWithoutUpdateDiscardsPreviousReference)
{
    time::Stopwatch sw;

    s_MockCycles = 100;
    sw.Start(MockNow);

    s_MockCycles = 5000;
    sw.Start(MockNow); // no Update() in between

    s_MockCycles = 5010;
    CHECK_EQUAL(10, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, StartAtZeroCounterIsTreatedAsNotStarted)
{
    time::Stopwatch sw;

    // A counter value of exactly 0 is indistinguishable from "not started".
    s_MockCycles = 0;
    sw.Start(MockNow);

    s_MockCycles = 100;
    CHECK_EQUAL(0, (int64_t)sw.Update(MockNow)); // acts as the start point

    s_MockCycles = 130;
    CHECK_EQUAL(30, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, ZeroReadingInUpdateReArmsStartPoint)
{
    time::Stopwatch sw;

    // A zero reading in Update() is stored as m_prev, which then looks like "not started".
    s_MockCycles = 100;
    sw.Start(MockNow);

    s_MockCycles = 0;
    // 0 - 100 wraps around (unsigned); the following Update() must then see m_prev == 0
    // and treat it as not started, returning 0.
    (void)sw.Update(MockNow);

    s_MockCycles = 50;
    CHECK_EQUAL(0, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, HandlesCounterWrapAround)
{
    time::Stopwatch sw;

    // Start 10 cycles before the counter wraps (max - 9), end 5 cycles after wrap: 15 cycles.
    s_MockCycles = static_cast<Cycles>(0) - 10;
    sw.Start(MockNow);

    s_MockCycles = 5;
    CHECK_EQUAL(15, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, WrapAroundThenContinuesNormally)
{
    time::Stopwatch sw;

    s_MockCycles = static_cast<Cycles>(0) - 10;
    sw.Start(MockNow);

    s_MockCycles = 5;
    CHECK_EQUAL(15, (int64_t)sw.Update(MockNow));

    s_MockCycles = 25;
    CHECK_EQUAL(20, (int64_t)sw.Update(MockNow));
}

TEST(Stopwatch, AcceptsLambdaTimeSource)
{
    time::Stopwatch sw;
    Cycles counter = 10;
    auto now = [&counter]() { return counter; };

    sw.Start(now);

    counter = 60;
    CHECK_EQUAL(50, (int64_t)sw.Update(now));
}

TEST(Stopwatch, AcceptsFunctorTimeSource)
{
    time::Stopwatch sw;
    MockNowFunctor now;

    s_MockCycles = 40;
    sw.Start(now);

    s_MockCycles = 100;
    CHECK_EQUAL(60, (int64_t)sw.Update(now));
}

TEST(Stopwatch, AcceptsFunctionPointerTimeSource)
{
    time::Stopwatch sw;
    Cycles (*now)() = &MockNow;

    s_MockCycles = 7;
    sw.Start(now);

    s_MockCycles = 107;
    CHECK_EQUAL(100, (int64_t)sw.Update(now));
}

TEST(Stopwatch, TimeSourceCalledOncePerCall)
{
    time::Stopwatch sw;
    int calls = 0;
    auto now = [&calls]() { return static_cast<Cycles>(++calls * 10); };

    sw.Start(now);
    CHECK_EQUAL(1, calls);

    CHECK_EQUAL(10, (int64_t)sw.Update(now));
    CHECK_EQUAL(2, calls);

    CHECK_EQUAL(10, (int64_t)sw.Update(now));
    CHECK_EQUAL(3, calls);
}

TEST(Stopwatch, IndependentInstances)
{
    time::Stopwatch a, b;

    s_MockCycles = 100;
    a.Start(MockNow);

    s_MockCycles = 150;
    b.Start(MockNow);

    s_MockCycles = 200;
    CHECK_EQUAL(100, (int64_t)a.Update(MockNow));
    CHECK_EQUAL(50, (int64_t)b.Update(MockNow));
}

// ============================================================================ //
// ================================ TimerHost ================================= //
// ============================================================================ //

// Polled mode (InitializePolled() + Process()) runs the whole timer logic without tasks, on the tick source of
// stk::SetTimeNowMsec() (1 tick = 1 ms). Task mode is exercised on a mock kernel which is never started: the tasks
// are registered, the commands are accepted (queued) but not executed, which covers Initialize() and the producer side.

typedef time::TimerHost TH;

#define CHECK_RES(expected, actual) CHECK_EQUAL((int)(expected), (int)(actual))

//! Ids of the timers in the order of their callbacks (a fixed array: a growing container would be reported as a leak
//! by the next test, its capacity outlives clear()).
static struct TimerLog
{
    enum { MAX = 16 };

    void   Clear()            { count = 0; }
    void   Add(int id)        { if (count < MAX) ids[count++] = id; }
    size_t Size() const       { return count; }
    int    operator[](size_t i) const { return ids[i]; }

    int    ids[MAX];
    size_t count;
}
s_TimerLog = {};

//! Timer which counts its expirations and runs an optional hook from OnExpired().
struct TestTimer : public time::Timer
{
    explicit TestTimer(int timer_id = 0) : id(timer_id), count(0), last_host(NULL), last_now(0), hook()
    {}

    virtual ~TestTimer()
    {}

    void OnExpired(time::TimerHost *host) override
    {
        ++count;
        last_host = host;
        last_now  = host->GetTimeNow();
        s_TimerLog.Add(id);

        if (hook)
            hook(*this, host);
    }

    int                                           id;
    int                                           count;
    time::TimerHost                              *last_host;
    Ticks                                         last_now;
    std::function<void(TestTimer &, TH *)>        hook;
};

//! Host in polled mode: shuts everything down on destruction so no timer stays linked (declare timers before it).
struct PolledHost
{
    PolledHost()
    {
        stk::SetTimeNowMsec(0);
        host.InitializePolled();
    }

    ~PolledHost()
    {
        host.Shutdown();
        host.Process();
    }

    time::TimerHost host;
};

static void At(int64_t now) { stk::SetTimeNowMsec(now); }

template <class Fn>
static void TimerExpectAssert(Fn fn)
{
    try
    {
        g_TestContext.ExpectAssert(true);
        fn();
        CHECK(false); // should asssert in a DEBUG build
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST_GROUP(TimerHost)
{
    void setup()
    {
        s_TimerLog.Clear();
        stk::SetTimeNowMsec(0);
    }

    void teardown()
    {
        test::g_KernelService = NULL;
    }
};

// ---- Timer ----

TEST(TimerHost, Timer_InitialState)
{
    TestTimer t;

    CHECK_FALSE(t.IsActive());
    CHECK_EQUAL(0, t.GetDeadline());
    CHECK_EQUAL(0, t.GetTimestamp());
    CHECK_EQUAL(0U, t.GetPeriod());
    CHECK_EQUAL(0U, t.GetDelay());
    CHECK_EQUAL(0U, t.GetRemainingTicks());
}

// ---- not initialized ----

TEST(TimerHost, NotInitialized_CommandsAreRejected)
{
    TestTimer t;
    TH host;
    TH::TimerInfo info = {};

    CHECK_RES(TH::RES_NOT_READY, host.Start(t, 10));
    CHECK_RES(TH::RES_NOT_READY, host.Restart(t, 10));
    CHECK_RES(TH::RES_NOT_READY, host.StartOrReset(t, 10));
    CHECK_RES(TH::RES_NOT_READY, host.Shutdown());

    // refused by the state of the timer before the host is asked
    CHECK_RES(TH::RES_INACTIVE, host.Stop(t));
    CHECK_RES(TH::RES_INACTIVE, host.Reset(t));
    CHECK_RES(TH::RES_INACTIVE, host.SetPeriod(t, 10));
    CHECK_RES(TH::RES_INVALID_PARAM, host.SetPeriod(t, 0));

    CHECK_RES(TH::RES_NOT_READY, host.StartSync(t, 10));
    CHECK_RES(TH::RES_NOT_READY, host.ActivateSync(t));
    CHECK_RES(TH::RES_NOT_READY, host.StopSync(t));
    CHECK_RES(TH::RES_NOT_READY, host.RestartSync(t, 10));
    CHECK_RES(TH::RES_NOT_READY, host.ResetSync(t));
    CHECK_RES(TH::RES_NOT_READY, host.SetPeriodSync(t, 10));
    CHECK_RES(TH::RES_INVALID_PARAM, host.SetPeriodSync(t, 0));
    CHECK_RES(TH::RES_NOT_READY, host.ChangeSync(t, 10, 10));
    CHECK_RES(TH::RES_NOT_READY, host.QuerySync(t, info));

    CHECK_TRUE(host.IsEmpty());
    CHECK_EQUAL(0U, host.GetSize());
    CHECK_FALSE(host.IsHostTid(TID_NONE));
}

TEST(TimerHost, Process_RequiresPolledMode)
{
    TH host;

    TimerExpectAssert([&]() { STK_UNUSED(host.Process()); });
}

TEST(TimerHost, InvalidArguments_Assert)
{
    TestTimer t;
    PolledHost p;

    const uint32_t too_big = static_cast<uint32_t>(WAIT_INFINITE) + 1U;

    TimerExpectAssert([&]() { STK_UNUSED(p.host.Start(t, too_big)); });
    TimerExpectAssert([&]() { STK_UNUSED(p.host.Start(t, 10, too_big)); });
    TimerExpectAssert([&]() { STK_UNUSED(p.host.Restart(t, too_big)); });
    TimerExpectAssert([&]() { STK_UNUSED(p.host.Restart(t, 10, too_big)); });
    TimerExpectAssert([&]() { STK_UNUSED(p.host.StartOrReset(t, too_big)); });
    TimerExpectAssert([&]() { STK_UNUSED(p.host.StartOrReset(t, 10, too_big)); });
    TimerExpectAssert([&]() { STK_UNUSED(p.host.StartSync(t, too_big)); });
    TimerExpectAssert([&]() { STK_UNUSED(p.host.RestartSync(t, 10, too_big)); });
}

// ---- polled mode: one-shot and periodic timers ----

TEST(TimerHost, Polled_Process_Empty)
{
    PolledHost p;

    CHECK_EQUAL(WAIT_INFINITE, p.host.Process());
    CHECK_TRUE(p.host.IsEmpty());
    CHECK_FALSE(p.host.IsHostTid(1));
    CHECK_FALSE(p.host.IsHostTid(TID_NONE));
}

TEST(TimerHost, Polled_OneShot)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 50));
    CHECK_FALSE(t.IsActive()); // only queued

    CHECK_EQUAL(50, p.host.Process()); // executes the command, the deadline is 50 ticks away
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(50, t.GetDeadline());
    CHECK_EQUAL(50U, t.GetDelay());
    CHECK_EQUAL(0U, t.GetPeriod());
    CHECK_EQUAL(50U, t.GetRemainingTicks());
    CHECK_EQUAL(1U, p.host.GetSize());

    At(49);
    CHECK_EQUAL(1, p.host.Process());
    CHECK_EQUAL(0, t.count);
    CHECK_EQUAL(1U, t.GetRemainingTicks());

    At(50);
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process()); // a one-shot timer does not keep the host busy
    CHECK_EQUAL(1, t.count);
    CHECK_TRUE(t.last_host == &p.host);
    CHECK_EQUAL(50, t.last_now);
    CHECK_EQUAL(50, t.GetTimestamp());
    CHECK_EQUAL(50, p.host.GetTimeNow());
    CHECK_FALSE(t.IsActive());
    CHECK_EQUAL(0U, t.GetRemainingTicks());
    CHECK_TRUE(p.host.IsEmpty());

    // does not fire again
    At(500);
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process());
    CHECK_EQUAL(1, t.count);
}

TEST(TimerHost, Polled_Periodic)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 10, 20));
    CHECK_EQUAL(10, p.host.Process());
    CHECK_EQUAL(20U, t.GetPeriod());
    CHECK_EQUAL(10U, t.GetDelay());

    At(10);
    CHECK_EQUAL(20, p.host.Process()); // fired, reloaded: next deadline is 30
    CHECK_EQUAL(1, t.count);
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(30, t.GetDeadline());

    At(30);
    CHECK_EQUAL(20, p.host.Process());
    CHECK_EQUAL(2, t.count);

    At(45);
    CHECK_EQUAL(5, p.host.Process()); // nothing is due: 50 - 45
    CHECK_EQUAL(2, t.count);
    CHECK_EQUAL(5U, t.GetRemainingTicks());
}

// polled slower than the period: one callback per call until the schedule has caught up, no drift
TEST(TimerHost, Polled_Periodic_Overdue_CatchesUp)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 10, 20));
    CHECK_EQUAL(10, p.host.Process());

    At(75); // deadlines were 10, 30, 50, 70, the next is 90

    CHECK_EQUAL(NO_WAIT, p.host.Process());
    CHECK_EQUAL(1, t.count);
    CHECK_EQUAL(NO_WAIT, p.host.Process());
    CHECK_EQUAL(NO_WAIT, p.host.Process());
    CHECK_EQUAL(3, t.count);

    CHECK_EQUAL(15, p.host.Process()); // 4th and last one, 90 - 75
    CHECK_EQUAL(4, t.count);
    CHECK_EQUAL(90, t.GetDeadline());
}

TEST(TimerHost, Polled_MultipleTimers_OrderAndNextSleep)
{
    TestTimer a(1), b(2), c(3);
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(a, 30));
    CHECK_RES(TH::RES_OK, p.host.Start(b, 10));
    CHECK_RES(TH::RES_OK, p.host.Start(c, 20));

    CHECK_EQUAL(10, p.host.Process()); // the earliest one
    CHECK_EQUAL(3U, p.host.GetSize());

    At(10);
    CHECK_EQUAL(10, p.host.Process()); // b fired, c is due in 10
    CHECK_EQUAL(1U, s_TimerLog.Size());

    At(30); // a and c are due together: called in the order of activation
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process());

    CHECK_EQUAL(3U, s_TimerLog.Size());
    CHECK_EQUAL(2, s_TimerLog[0]);
    CHECK_EQUAL(1, s_TimerLog[1]);
    CHECK_EQUAL(3, s_TimerLog[2]);
    CHECK_TRUE(p.host.IsEmpty());
}

// ---- start / stop / reset ----

TEST(TimerHost, Polled_Start_ActiveTimer)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 50));
    CHECK_EQUAL(50, p.host.Process());

    CHECK_RES(TH::RES_ACTIVE, p.host.Start(t, 10)); // nothing is queued
    CHECK_EQUAL(50, p.host.Process());
    CHECK_EQUAL(50, t.GetDeadline());
}

// two Start() calls before the tick: the second one is dropped by the tick task (the timer is active by then)
TEST(TimerHost, Polled_Start_Twice_FirstWins)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 50));
    CHECK_RES(TH::RES_OK, p.host.Start(t, 10));

    CHECK_EQUAL(50, p.host.Process());
    CHECK_EQUAL(50, t.GetDeadline());
}

TEST(TimerHost, Polled_Stop)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_INACTIVE, p.host.Stop(t));

    CHECK_RES(TH::RES_OK, p.host.Start(t, 10));
    CHECK_EQUAL(10, p.host.Process());

    CHECK_RES(TH::RES_OK, p.host.Stop(t));
    CHECK_TRUE(t.IsActive()); // takes effect at the next Process()

    // the stop is executed before the deadlines are evaluated: the timer does not fire
    At(20);
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process());
    CHECK_FALSE(t.IsActive());
    CHECK_EQUAL(0, t.count);
    CHECK_TRUE(p.host.IsEmpty());

    // and can be started again
    CHECK_RES(TH::RES_OK, p.host.Start(t, 5));
    CHECK_EQUAL(5, p.host.Process());
    At(25);
    p.host.Process();
    CHECK_EQUAL(1, t.count);
}

TEST(TimerHost, Polled_Reset)
{
    TestTimer one_shot, periodic, idle;
    PolledHost p;

    CHECK_RES(TH::RES_INACTIVE, p.host.Reset(idle));

    CHECK_RES(TH::RES_OK, p.host.Start(one_shot, 100));
    CHECK_RES(TH::RES_OK, p.host.Start(periodic, 100, 100));
    p.host.Process();

    CHECK_RES(TH::RES_NOT_PERIODIC, p.host.Reset(one_shot)); // nothing is queued

    At(30);
    CHECK_RES(TH::RES_OK, p.host.Reset(periodic));
    CHECK_EQUAL(70, p.host.Process()); // one-shot: 100 - 30
    CHECK_EQUAL(130, periodic.GetDeadline()); // restarted from the call time with the period
}

TEST(TimerHost, Polled_Restart)
{
    TestTimer t;
    PolledHost p;

    // inactive: starts
    CHECK_RES(TH::RES_OK, p.host.Restart(t, 40, 15));
    CHECK_EQUAL(40, p.host.Process());
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(40U, t.GetDelay());
    CHECK_EQUAL(15U, t.GetPeriod());

    // active: re-armed with the new parameters (here a one-shot timer)
    At(10);
    CHECK_RES(TH::RES_OK, p.host.Restart(t, 100));
    CHECK_EQUAL(100, p.host.Process());
    CHECK_EQUAL(110, t.GetDeadline());
    CHECK_EQUAL(0U, t.GetPeriod());
    CHECK_EQUAL(0, t.count);
}

// Restart() calls of the same timer which are waiting in the queue are merged: the latest call wins
TEST(TimerHost, Polled_Restart_Coalesced_LatestCallWins)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Restart(t, 100));

    At(5);
    CHECK_RES(TH::RES_OK, p.host.Restart(t, 30, 7));

    CHECK_EQUAL(30, p.host.Process());
    CHECK_EQUAL(35, t.GetDeadline()); // call time and arguments of the last call
    CHECK_EQUAL(30U, t.GetDelay());
    CHECK_EQUAL(7U, t.GetPeriod());
}

// a merged call does not take a slot of the command queue
TEST(TimerHost, Polled_Restart_Coalesced_NoQueueSlot)
{
    TestTimer t;
    PolledHost p;

    for (uint32_t i = 0; i < 4U * STK_TIMER_COMMAND_QUEUE_SIZE; ++i)
        CHECK_RES(TH::RES_OK, p.host.Restart(t, 10 + i));

    p.host.Process();
    CHECK_EQUAL(10U + 4U * STK_TIMER_COMMAND_QUEUE_SIZE - 1U, t.GetDelay());
}

TEST(TimerHost, Polled_StartOrReset)
{
    TestTimer t, one_shot;
    PolledHost p;

    // inactive: starts
    CHECK_RES(TH::RES_OK, p.host.StartOrReset(t, 20, 10));
    CHECK_EQUAL(20, p.host.Process());
    CHECK_TRUE(t.IsActive());

    // active periodic: the deadline is moved by the stored period, the arguments are not used
    At(5);
    CHECK_RES(TH::RES_OK, p.host.StartOrReset(t, 99, 99));
    CHECK_EQUAL(10, p.host.Process());
    CHECK_EQUAL(15, t.GetDeadline());
    CHECK_EQUAL(20U, t.GetDelay());
    CHECK_EQUAL(10U, t.GetPeriod());

    // active one-shot: nothing happens (started at tick 5, so its deadline is 55)
    CHECK_RES(TH::RES_OK, p.host.Start(one_shot, 50));
    p.host.Process();
    CHECK_EQUAL(55, one_shot.GetDeadline());

    At(8);
    CHECK_RES(TH::RES_OK, p.host.StartOrReset(one_shot, 10));
    p.host.Process();
    CHECK_EQUAL(55, one_shot.GetDeadline());

    // calls of the same timer are merged like Restart()
    CHECK_RES(TH::RES_OK, p.host.StartOrReset(t, 5, 5));
    CHECK_RES(TH::RES_OK, p.host.StartOrReset(t, 6, 6));
    p.host.Process();
}

TEST(TimerHost, Polled_SetPeriod)
{
    TestTimer t, one_shot, idle;
    PolledHost p;

    CHECK_RES(TH::RES_INVALID_PARAM, p.host.SetPeriod(t, 0));
    CHECK_RES(TH::RES_INVALID_PARAM, p.host.SetPeriod(t, static_cast<uint32_t>(WAIT_INFINITE) + 1U));
    CHECK_RES(TH::RES_INACTIVE, p.host.SetPeriod(idle, 10));

    CHECK_RES(TH::RES_OK, p.host.Start(t, 10, 20));
    CHECK_RES(TH::RES_OK, p.host.Start(one_shot, 100));
    p.host.Process();

    CHECK_RES(TH::RES_NOT_PERIODIC, p.host.SetPeriod(one_shot, 10));

    At(10);
    p.host.Process(); // fires, next at 30
    CHECK_EQUAL(1, t.count);

    // the new period is used from the next reload: the current deadline is not changed
    CHECK_RES(TH::RES_OK, p.host.SetPeriod(t, 50));
    CHECK_EQUAL(20, p.host.Process());
    CHECK_EQUAL(30, t.GetDeadline());
    CHECK_EQUAL(50U, t.GetPeriod());

    At(30);
    p.host.Process();
    CHECK_EQUAL(2, t.count);
    CHECK_EQUAL(80, t.GetDeadline());
}

// ---- synchronous API: a polled host can not wait, the commands are queued ----

TEST(TimerHost, Polled_SyncCommands_AreQueued)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_QUEUED, p.host.StartSync(t, 30, 15));
    CHECK_FALSE(t.IsActive());
    CHECK_EQUAL(30, p.host.Process());
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(15U, t.GetPeriod());

    At(10);
    CHECK_RES(TH::RES_QUEUED, p.host.ResetSync(t));
    p.host.Process();
    CHECK_EQUAL(25, t.GetDeadline()); // 10 + period

    At(12);
    CHECK_RES(TH::RES_QUEUED, p.host.SetPeriodSync(t, 40));
    p.host.Process();
    CHECK_EQUAL(40U, t.GetPeriod());

    At(14);
    CHECK_RES(TH::RES_QUEUED, p.host.RestartSync(t, 100));
    p.host.Process();
    CHECK_EQUAL(114, t.GetDeadline());
    CHECK_EQUAL(0U, t.GetPeriod());

    CHECK_RES(TH::RES_QUEUED, p.host.StopSync(t));
    p.host.Process();
    CHECK_FALSE(t.IsActive());
    CHECK_TRUE(p.host.IsEmpty());
}

TEST(TimerHost, Polled_ChangeAndActivateSync)
{
    TestTimer t;
    PolledHost p;

    // Change stores the parameters of an inactive timer without starting it
    CHECK_RES(TH::RES_QUEUED, p.host.ChangeSync(t, 5, 9));
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process());
    CHECK_FALSE(t.IsActive());
    CHECK_EQUAL(5U, t.GetDelay());
    CHECK_EQUAL(9U, t.GetPeriod());

    // Activate starts it with the stored parameters, relative to the call
    At(100);
    CHECK_RES(TH::RES_QUEUED, p.host.ActivateSync(t));
    CHECK_EQUAL(5, p.host.Process());
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(105, t.GetDeadline());

    // an active timer is not changed (and the command is dropped)
    CHECK_RES(TH::RES_QUEUED, p.host.ChangeSync(t, 1, 1));
    CHECK_RES(TH::RES_QUEUED, p.host.ActivateSync(t));
    p.host.Process();
    CHECK_EQUAL(5U, t.GetDelay());
    CHECK_EQUAL(9U, t.GetPeriod());
    CHECK_EQUAL(105, t.GetDeadline());
}

TEST(TimerHost, Polled_SyncCommands_NotCoalesced)
{
    TestTimer t;
    PolledHost p;

    // every synchronous command takes its own slot, unlike the asynchronous Restart()
    for (uint32_t i = 0; i < STK_TIMER_COMMAND_QUEUE_SIZE; ++i)
        CHECK_RES(TH::RES_QUEUED, p.host.RestartSync(t, 10 + i));

    CHECK_RES(TH::RES_QUEUE_FULL, p.host.RestartSync(t, 1));

    p.host.Process();
    CHECK_EQUAL(10U + STK_TIMER_COMMAND_QUEUE_SIZE - 1U, t.GetDelay()); // the arguments of every call were applied in order
}

TEST(TimerHost, Polled_SetPeriodSync_InvalidParameter)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_INVALID_PARAM, p.host.SetPeriodSync(t, 0));
    CHECK_RES(TH::RES_INVALID_PARAM, p.host.SetPeriodSync(t, static_cast<uint32_t>(WAIT_INFINITE) + 1U));
}

// a polled host returns a snapshot taken at the call instead of waiting for the tick task
TEST(TimerHost, Polled_QuerySync_Snapshot)
{
    TestTimer t;
    PolledHost p;
    TH::TimerInfo info = {};

    CHECK_RES(TH::RES_OK, p.host.QuerySync(t, info));
    CHECK_FALSE(info.active);
    CHECK_EQUAL(0U, info.remaining);
    CHECK_EQUAL(0U, info.delay);
    CHECK_EQUAL(0U, info.period);

    CHECK_RES(TH::RES_OK, p.host.Start(t, 50, 10));
    p.host.Process();

    At(20);
    CHECK_RES(TH::RES_OK, p.host.QuerySync(t, info));
    CHECK_TRUE(info.active);
    CHECK_EQUAL(30U, info.remaining);
    CHECK_EQUAL(50U, info.delay);
    CHECK_EQUAL(10U, info.period);

    // overdue and not processed yet
    At(60);
    CHECK_RES(TH::RES_OK, p.host.QuerySync(t, info));
    CHECK_TRUE(info.active);
    CHECK_EQUAL(0U, info.remaining);
}

// ---- callbacks ----

// commands issued by a callback take effect at the next Process()
TEST(TimerHost, Polled_Callback_RearmsItsTimer)
{
    TestTimer t;
    PolledHost p;

    t.hook = [](TestTimer &self, TH *host)
    {
        if (self.count < 3)
            CHECK_RES(TH::RES_OK, host->Start(self, 10));
    };

    CHECK_RES(TH::RES_OK, p.host.Start(t, 10));
    CHECK_EQUAL(10, p.host.Process());

    At(10);
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process()); // the callback queued Start(), not executed yet
    CHECK_EQUAL(1, t.count);
    CHECK_FALSE(t.IsActive());

    CHECK_EQUAL(10, p.host.Process());
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(20, t.GetDeadline());

    At(20);
    p.host.Process();
    p.host.Process();
    CHECK_EQUAL(2, t.count);

    At(30);
    p.host.Process();
    CHECK_EQUAL(3, t.count);

    // the third callback does not re-arm
    At(40);
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process());
    CHECK_EQUAL(3, t.count);
    CHECK_TRUE(p.host.IsEmpty());
}

TEST(TimerHost, Polled_Callback_StopsOtherTimer_SyncCallIsQueued)
{
    TestTimer a(1), b(2);
    PolledHost p;

    TH::EResult stop_res = TH::RES_FAIL, sync_res = TH::RES_FAIL;

    a.hook = [&](TestTimer &, TH *host)
    {
        stop_res = host->Stop(b);       // asynchronous
        sync_res = host->StopSync(b);   // a callback of a polled host can not wait
    };

    CHECK_RES(TH::RES_OK, p.host.Start(a, 10));
    CHECK_RES(TH::RES_OK, p.host.Start(b, 20, 20));
    p.host.Process();

    At(10);
    p.host.Process(); // a fires, queues the stop of b
    CHECK_RES(TH::RES_OK, stop_res);
    CHECK_RES(TH::RES_QUEUED, sync_res);
    CHECK_TRUE(b.IsActive());

    At(20);
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process()); // the stop is executed before the deadline is evaluated
    CHECK_EQUAL(0, b.count);
    CHECK_FALSE(b.IsActive());
}

// ---- command queue ----

TEST(TimerHost, Polled_CommandQueueFull)
{
    const uint32_t cap = STK_TIMER_COMMAND_QUEUE_SIZE;

    std::vector<TestTimer> timers(cap + 1U); // declared before the host: destroyed after it
    PolledHost p;

    for (uint32_t i = 0; i < cap; ++i)
        CHECK_RES(TH::RES_OK, p.host.Start(timers[i], 100));

    // a polled host can not wait for a free slot
    CHECK_RES(TH::RES_QUEUE_FULL, p.host.Start(timers[cap], 100));
    CHECK_RES(TH::RES_QUEUE_FULL, p.host.Restart(timers[cap], 100));
    CHECK_RES(TH::RES_QUEUE_FULL, p.host.StartSync(timers[cap], 100));

    // the tick drains the queue
    CHECK_EQUAL(100, p.host.Process());
    CHECK_EQUAL(cap, p.host.GetSize());

    CHECK_RES(TH::RES_OK, p.host.Start(timers[cap], 100));
    p.host.Process();
    CHECK_EQUAL(cap + 1U, p.host.GetSize());
}

// ---- shutdown ----

TEST(TimerHost, Polled_Shutdown_StopsTimers_HostStaysUsable)
{
    TestTimer a(1), b(2);
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(a, 10));
    CHECK_RES(TH::RES_OK, p.host.Start(b, 20, 20));
    p.host.Process();
    CHECK_EQUAL(2U, p.host.GetSize());

    CHECK_RES(TH::RES_OK, p.host.Shutdown());
    CHECK_TRUE(a.IsActive()); // executed by the next Process()

    At(10);
    CHECK_EQUAL(WAIT_INFINITE, p.host.Process());
    CHECK_FALSE(a.IsActive());
    CHECK_FALSE(b.IsActive());
    CHECK_EQUAL(0, a.count);
    CHECK_TRUE(p.host.IsEmpty());

    // there are no tasks to exit: the host keeps working
    CHECK_RES(TH::RES_OK, p.host.Start(a, 5));
    CHECK_EQUAL(5, p.host.Process());
    At(15);
    p.host.Process();
    CHECK_EQUAL(1, a.count);
}

// ---- task mode: kernel which is not started ----

typedef Kernel<KERNEL_STATIC | KERNEL_SYNC, time::TimerHost::TASK_COUNT, sched::SwitchStrategyRR, PlatformTestMock> TimerTestKernel;

TEST(TimerHost, TaskMode_Initialize_AcceptsCommands)
{
    TimerTestKernel kernel; // declared first: destroyed after the host which owns its tasks
    TestTimer t;
    TH host;

    kernel.Initialize();
    CHECK_RES(ADD_OK, host.Initialize(&kernel, ACCESS_USER));

    CHECK_FALSE(host.IsHostTid(TID_NONE));

    // the tick task does not run yet: the commands only wait in the queue
    CHECK_RES(TH::RES_OK, host.Start(t, 10, 5));
    CHECK_FALSE(t.IsActive());
    CHECK_RES(TH::RES_OK, host.Start(t, 20)); // still inactive for the caller: queued too

    CHECK_RES(TH::RES_INACTIVE, host.Stop(t));
    CHECK_RES(TH::RES_INACTIVE, host.Reset(t));
    CHECK_RES(TH::RES_INACTIVE, host.SetPeriod(t, 10));
    CHECK_RES(TH::RES_INVALID_PARAM, host.SetPeriod(t, 0));

    CHECK_RES(TH::RES_OK, host.Restart(t, 10));
    CHECK_RES(TH::RES_OK, host.StartOrReset(t, 10));

    // a re-arm of the same timer which is waiting is merged
    for (int i = 0; i < 100; ++i)
        CHECK_RES(TH::RES_OK, host.Restart(t, 10 + i));
}

TEST(TimerHost, TaskMode_QueueFull_CallerCanNotWait)
{
    const uint32_t cap = STK_TIMER_COMMAND_QUEUE_SIZE;

    TimerTestKernel kernel;
    std::vector<TestTimer> timers(cap + 1U);
    TH host;

    kernel.Initialize();
    CHECK_RES(ADD_OK, host.Initialize(&kernel, ACCESS_USER));

    for (uint32_t i = 0; i < cap; ++i)
        CHECK_RES(TH::RES_OK, host.Start(timers[i], 100));

    // the kernel is not running: the caller can not wait for a free slot
    CHECK_RES(TH::RES_QUEUE_FULL, host.Start(timers[cap], 100));
    CHECK_RES(TH::RES_QUEUE_FULL, host.Restart(timers[cap], 100));
    CHECK_RES(TH::RES_QUEUE_FULL, host.StartOrReset(timers[cap], 100));
    CHECK_RES(TH::RES_QUEUE_FULL, host.Shutdown());
}

TEST(TimerHost, TaskMode_SyncCommands_AreQueued_WhenKernelIsNotRunning)
{
    TimerTestKernel kernel;
    TestTimer t;
    TH host;
    TH::TimerInfo info = {};

    kernel.Initialize();
    CHECK_RES(ADD_OK, host.Initialize(&kernel, ACCESS_USER));

    CHECK_RES(TH::RES_QUEUED, host.StartSync(t, 10, 5));
    CHECK_RES(TH::RES_QUEUED, host.ActivateSync(t));
    CHECK_RES(TH::RES_QUEUED, host.StopSync(t));
    CHECK_RES(TH::RES_QUEUED, host.RestartSync(t, 10));
    CHECK_RES(TH::RES_QUEUED, host.ResetSync(t));
    CHECK_RES(TH::RES_QUEUED, host.SetPeriodSync(t, 10));
    CHECK_RES(TH::RES_QUEUED, host.ChangeSync(t, 10, 10));
    CHECK_RES(TH::RES_INVALID_PARAM, host.SetPeriodSync(t, 0));

    // the query returns a snapshot
    CHECK_RES(TH::RES_OK, host.QuerySync(t, info));
    CHECK_FALSE(info.active);
    CHECK_EQUAL(0U, info.remaining);
}

TEST(TimerHost, TaskMode_Initialize_FailureLeavesHostNotReady)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, sched::SwitchStrategyRR, PlatformTestMock> kernel; // a slot short
    TestTimer t;
    TH host;

    kernel.Initialize();

    // a full kernel asserts in KERNEL_STATIC mode, the failure is still reported
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    const EAddResult result = host.Initialize(&kernel, ACCESS_USER);
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);

    CHECK_TRUE(result != ADD_OK);

    CHECK_RES(TH::RES_NOT_READY, host.Start(t, 10));
    CHECK_RES(TH::RES_NOT_READY, host.StartSync(t, 10));
    CHECK_RES(TH::RES_NOT_READY, host.Shutdown());
}

TEST(TimerHost, TaskMode_Initialize_ExternalStacks)
{
    TimerTestKernel kernel;
    TestTimer t;
    TH host;

    StackMemoryDef<TH::TASK_TICK_MEMORY_SIZE>::Type    tick_stack;
    StackMemoryDef<TH::TASK_HANDLER_STACK_SIZE>::Type  handler_stack;

    TH::Config cfg;
    cfg.tick_stack         = tick_stack;
    cfg.tick_stack_size    = TH::TASK_TICK_MEMORY_SIZE;
    cfg.handler_stack[0]   = handler_stack;
    cfg.handler_stack_size = TH::TASK_HANDLER_STACK_SIZE;
    cfg.tick_mode          = ACCESS_USER;

    kernel.Initialize();
    CHECK_RES(ADD_OK, host.Initialize(&kernel, ACCESS_USER, cfg));

    CHECK_RES(TH::RES_OK, host.Start(t, 10));
}

TEST(TimerHost, TaskMode_Initialize_InvalidArguments_Assert)
{
    TimerTestKernel kernel;
    TH host;

    kernel.Initialize();

    TimerExpectAssert([&]() { STK_UNUSED(host.Initialize(NULL, ACCESS_USER)); });

    TH::Config no_tick;
    no_tick.tick_weight = NO_WEIGHT;
    TimerExpectAssert([&]() { STK_UNUSED(host.Initialize(&kernel, ACCESS_USER, no_tick)); });

    TH::Config no_handler;
    no_handler.handler_weight = NO_WEIGHT;
    TimerExpectAssert([&]() { STK_UNUSED(host.Initialize(&kernel, ACCESS_USER, no_handler)); });
}

// ---- task mode: the bodies of the tick task and of the handler task ----

// The mock kernel does not execute tasks, so the tests run the task bodies themselves: the entry points are taken from
// the platform which sees every task of the kernel (ITask::Run() of the tick task is UpdateTime(), of the handler
// task is ProcessTimers()). The tick task is run from the test thread: whenever it blocks (a timed wait on the
// command queue) the relax-CPU hook advances the kernel time and plays the other tasks (commands, shutdown).

//! Platform which remembers the tasks of the kernel.
struct TimerRecPlatform : public PlatformTestMock
{
    TimerRecPlatform() : task_count(0)
    {}

    void InitStack(EStackType type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) override
    {
        if ((type == STACK_USER_TASK) && (user_task != NULL) && (task_count < 8))
            tasks[task_count++] = user_task;

        PlatformTestMock::InitStack(type, stack, stack_memory, user_task);
    }

    ITask *Find(const Word *stack) const
    {
        for (int i = 0; i < task_count; ++i)
        {
            if (tasks[i]->GetStack() == stack)
                return tasks[i];
        }
        return NULL;
    }

    ITask *tasks[8];
    int    task_count;
};

typedef Kernel<KERNEL_STATIC | KERNEL_SYNC, time::TimerHost::TASK_COUNT, sched::SwitchStrategyRR, TimerRecPlatform> TimerRecKernel;

static struct TimerRelaxContext
{
    PlatformTestMock         *platform = NULL;
    std::function<void()>     on_relax;
}
g_TimerRelax;

static void TimerRelaxCpu()
{
    if (g_TimerRelax.platform != NULL)
        g_TimerRelax.platform->ProcessTick(); // time advances while a task waits

    if (g_TimerRelax.on_relax)
        g_TimerRelax.on_relax();
}

//! Installs the relax-CPU hook for the scope of a test.
struct TimerRelaxScope
{
    template <class K>
    explicit TimerRelaxScope(K &kernel) : prev(g_RelaxCpuHandler)
    {
        g_TimerRelax.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
        g_TimerRelax.on_relax = nullptr;
        g_RelaxCpuHandler     = TimerRelaxCpu;
    }

    ~TimerRelaxScope()
    {
        g_RelaxCpuHandler     = prev;
        g_TimerRelax.on_relax = nullptr;
        g_TimerRelax.platform = NULL;
    }

    void (*prev)();
};

//! Kernel (started), host in task mode with known stacks, entry points of its tasks.
struct TimerTaskFixture
{
    TimerTaskFixture() : tick(NULL), handler(NULL)
    {
        TH::Config cfg;
        cfg.tick_stack         = tick_stack;
        cfg.tick_stack_size    = TH::TASK_TICK_MEMORY_SIZE;
        cfg.handler_stack[0]   = handler_stack;
        cfg.handler_stack_size = TH::TASK_HANDLER_STACK_SIZE;

        kernel.Initialize();
        CHECK_RES(ADD_OK, host.Initialize(&kernel, ACCESS_USER, cfg));
        kernel.Start();

        TimerRecPlatform *const platform = static_cast<TimerRecPlatform *>(kernel.GetPlatform());
        tick    = platform->Find(tick_stack);
        handler = platform->Find(handler_stack);

        CHECK_TRUE(tick != NULL);
        CHECK_TRUE(handler != NULL);
    }

    StackMemoryDef<TH::TASK_TICK_MEMORY_SIZE>::Type    tick_stack;    // memory is declared before the kernel which uses it
    StackMemoryDef<TH::TASK_HANDLER_STACK_SIZE>::Type  handler_stack;
    TimerRecKernel kernel;
    TH             host;
    ITask         *tick;
    ITask         *handler;
};

// the tick task evaluates the deadlines, hands the expired timer over to the handler task and, on shutdown, releases
// all timers (the expiration which was already handed over is still delivered by the handler task)
TEST(TimerHost, TaskMode_TickTask_ExpiresTimers_ShutdownReleasesTimers)
{
    TestTimer t1(1), t2(2), t3(3), t4(4);
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    CHECK_RES(TH::RES_OK, f.host.Start(t1, 100));
    CHECK_RES(TH::RES_OK, f.host.Start(t2, 40, 20));

    bool shut_down = false;
    g_TimerRelax.on_relax = [&]()
    {
        // the first deadline (40) has been evaluated: shut down, with one more command queued behind the shutdown
        if (!shut_down && (f.host.GetTimeNow() >= 40))
        {
            shut_down = true;
            CHECK_RES(TH::RES_OK, f.host.Shutdown());
            CHECK_RES(TH::RES_OK, f.host.Start(t3, 5));
        }
    };

    f.tick->Run(); // UpdateTime(): returns on the shutdown

    CHECK_TRUE(shut_down);

    // all timers are released, the command behind the shutdown is dropped
    CHECK_FALSE(t1.IsActive());
    CHECK_FALSE(t2.IsActive());
    CHECK_FALSE(t3.IsActive());
    CHECK_TRUE(f.host.IsEmpty());

    // nobody consumes the commands any more
    CHECK_RES(TH::RES_NOT_READY, f.host.Start(t4, 10));
    CHECK_RES(TH::RES_NOT_READY, f.host.StartSync(t4, 10));

    // the expiration which was handed to the handler task before the shutdown is still delivered
    CHECK_EQUAL(0, t2.count);
    f.handler->Run(); // ProcessTimers(): returns after the ready list is drained
    CHECK_EQUAL(1, t2.count);
    CHECK_EQUAL(0, t1.count);
    CHECK_EQUAL(0, t3.count);
    CHECK_TRUE(t2.last_host == &f.host);
}

// commands which are queued behind the shutdown are failed: a synchronous caller does not wait forever
TEST(TimerHost, TaskMode_TickTask_Shutdown_FailsQueuedSyncCommand)
{
    TestTimer t1;
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    CHECK_RES(TH::RES_OK, f.host.Shutdown());

    bool ran = false;
    g_TimerRelax.on_relax = [&]()
    {
        // the caller below is blocked until the tick task completes its command
        if (!ran)
        {
            ran = true;
            f.tick->Run();
        }
    };

    CHECK_RES(TH::RES_NOT_READY, f.host.StartSync(t1, 10));
    CHECK_TRUE(ran);
    CHECK_FALSE(t1.IsActive());
    CHECK_RES(TH::RES_NOT_READY, f.host.Start(t1, 10));
}

TEST(TimerHost, TaskMode_TickTask_Shutdown_FailsQueuedQuery_ClearsInfo)
{
    TestTimer t1;
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    CHECK_RES(TH::RES_OK, f.host.Shutdown());

    bool ran = false;
    g_TimerRelax.on_relax = [&]()
    {
        if (!ran)
        {
            ran = true;
            f.tick->Run();
        }
    };

    TH::TimerInfo info;
    info.active    = true;
    info.remaining = 7U;
    info.delay     = 8U;
    info.period    = 9U;

    CHECK_RES(TH::RES_NOT_READY, f.host.QuerySync(t1, info));
    CHECK_TRUE(ran);

    // the result of a failed command carries no data
    CHECK_FALSE(info.active);
    CHECK_EQUAL(0U, info.remaining);
    CHECK_EQUAL(0U, info.delay);
    CHECK_EQUAL(0U, info.period);
}

// ---- task mode: CMD_QUERY evaluated by the tick task ----

// A synchronous query is completed by the tick task, so that all fields belong to the same moment. The caller below
// is blocked on the completion, the hook then queues the shutdown behind the query (which ends UpdateTime() right
// after it, without a further wait) and runs the tick task. The kernel time is advanced first to \a advance_to.
static TH::EResult TimerQueryOnTickTask(TimerTaskFixture &f, TestTimer &tmr, TH::TimerInfo &info,
    Ticks advance_to, Ticks &now_at_query)
{
    bool ran = false;

    g_TimerRelax.on_relax = [&]()
    {
        if (ran)
            return;

        ran = true;

        CHECK_RES(TH::RES_OK, f.host.Shutdown());

        while (GetTicks() < advance_to)
            g_TimerRelax.platform->ProcessTick();

        now_at_query = GetTicks();

        f.tick->Run();
    };

    const TH::EResult result = f.host.QuerySync(tmr, info);

    g_TimerRelax.on_relax = nullptr;

    CHECK_TRUE(ran);
    return result;
}

TEST(TimerHost, TaskMode_TickTask_Query_ActiveTimer)
{
    TestTimer t;
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    const Ticks t0 = GetTicks();

    // executed by the tick task right before the query (the queue is ordered)
    CHECK_RES(TH::RES_OK, f.host.Start(t, 1000, 77));

    TH::TimerInfo info = {};
    Ticks now = 0;
    CHECK_RES(TH::RES_OK, TimerQueryOnTickTask(f, t, info, t0 + 10, now));

    CHECK_TRUE(now >= t0 + 10);

    CHECK_TRUE(info.active);
    CHECK_EQUAL(1000U, info.delay);
    CHECK_EQUAL(77U, info.period);
    CHECK_EQUAL((uint32_t)((t0 + 1000) - now), info.remaining); // time left to the deadline at the moment of the query

    // released by the shutdown which follows the query
    CHECK_FALSE(t.IsActive());
}

// the deadline has passed but the timer is not served yet: nothing is left, the remaining time is not negative
TEST(TimerHost, TaskMode_TickTask_Query_OverdueTimer)
{
    TestTimer t;
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    const Ticks t0 = GetTicks();

    CHECK_RES(TH::RES_OK, f.host.Start(t, 20));

    TH::TimerInfo info = {};
    info.remaining = 99U;
    Ticks now = 0;
    CHECK_RES(TH::RES_OK, TimerQueryOnTickTask(f, t, info, t0 + 50, now));

    CHECK_TRUE(info.active);
    CHECK_EQUAL(20U, info.delay);
    CHECK_EQUAL(0U, info.period);
    CHECK_EQUAL(0U, info.remaining);
}

TEST(TimerHost, TaskMode_TickTask_Query_InactiveTimer)
{
    TestTimer t;
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    TH::TimerInfo info = {};
    info.active    = true;
    info.remaining = 7U;
    info.delay     = 8U;
    info.period    = 9U;
    Ticks now = 0;
    CHECK_RES(TH::RES_OK, TimerQueryOnTickTask(f, t, info, 0, now));

    CHECK_FALSE(info.active);
    CHECK_EQUAL(0U, info.remaining); // an inactive timer has no deadline
    CHECK_EQUAL(0U, info.delay);
    CHECK_EQUAL(0U, info.period);
}

// ---- task mode: waiting for a callback which is being executed (WaitCallbackDone) ----

// m_running is private: the slot of the handler task is reached through an explicit instantiation (the only place
// where the language does not check the access), to play "a callback of the timer is being executed by a handler".
typedef time::Timer *volatile TimerRunningSlots[STK_TIMER_THREADS_COUNT];

struct TimerRunningTag
{
    typedef TimerRunningSlots TH::*type;
    friend type GetRunningSlots(TimerRunningTag);
};

template <class Tag, typename Tag::type Member>
struct TimerRunningRob
{
    friend typename Tag::type GetRunningSlots(Tag) { return Member; }
};

template struct TimerRunningRob<TimerRunningTag, &TH::m_running>;

static TimerRunningSlots &TimerRunning(TH &host)
{
    return host.*GetRunningSlots(TimerRunningTag());
}

typedef Kernel<KERNEL_STATIC | KERNEL_SYNC, time::TimerHost::TASK_COUNT + 1, sched::SwitchStrategyRR, TimerRecPlatform> TimerRecKernelEx;

//! Like TimerTaskFixture, with an application task added first: it is the task which runs the test body (the first
//! task is the one the mock kernel starts with), so the caller is not a task of the host.
struct TimerUserTaskFixture
{
    TimerUserTaskFixture() : tick(NULL)
    {
        TH::Config cfg;
        cfg.tick_stack         = tick_stack;
        cfg.tick_stack_size    = TH::TASK_TICK_MEMORY_SIZE;
        cfg.handler_stack[0]   = handler_stack;
        cfg.handler_stack_size = TH::TASK_HANDLER_STACK_SIZE;

        kernel.Initialize();
        CHECK_RES(ADD_OK, kernel.AddTask(&user));
        CHECK_RES(ADD_OK, host.Initialize(&kernel, ACCESS_USER, cfg));
        kernel.Start();

        tick = static_cast<TimerRecPlatform *>(kernel.GetPlatform())->Find(tick_stack);
        CHECK_TRUE(tick != NULL);
    }

    StackMemoryDef<TH::TASK_TICK_MEMORY_SIZE>::Type    tick_stack;
    StackMemoryDef<TH::TASK_HANDLER_STACK_SIZE>::Type  handler_stack;
    TimerRecKernelEx      kernel;
    TaskMock<ACCESS_USER> user;
    TH                    host;
    ITask                *tick;
};

// A synchronous stop completes when the callback which a handler task has already started (if any) has returned:
// the caller polls the running slots, sleeping a tick between the polls.
TEST(TimerHost, TaskMode_StopSync_WaitsForRunningCallback)
{
    TestTimer t;
    TimerUserTaskFixture f;
    TimerRelaxScope relax(f.kernel);

    CHECK_FALSE(f.host.IsHostTid(GetTid())); // the caller is an application task

    // a handler task is "executing the callback of the timer"
    TimerRunning(f.host)[0] = &t;

    int  phase         = 0;
    bool saw_running   = false;

    g_TimerRelax.on_relax = [&]()
    {
        if (phase == 0)
        {
            // the caller waits for the completion of its command: the tick task executes it (the shutdown
            // behind it ends the tick task, there is nothing else to do for it)
            phase = 1;
            CHECK_RES(TH::RES_OK, f.host.Shutdown());
            f.tick->Run();
        }
        else if (phase == 1)
        {
            // the command is completed, the caller is sleeping between the polls of the running slot
            saw_running = (TimerRunning(f.host)[0] == &t);

            // the callback returns
            TimerRunning(f.host)[0] = nullptr;
            phase = 2;
        }
    };

    // the timer is not active: stopped already (RES_INACTIVE), the result is reported after the callback is done
    CHECK_RES(TH::RES_INACTIVE, f.host.StopSync(t));

    CHECK_EQUAL(2, phase);
    CHECK_TRUE(saw_running);
    CHECK_TRUE(TimerRunning(f.host)[0] == nullptr);
}

// no callback is running: the poll finds nothing and completes at once
TEST(TimerHost, TaskMode_StopSync_NoRunningCallback)
{
    TestTimer t;
    TimerUserTaskFixture f;
    TimerRelaxScope relax(f.kernel);

    bool ran = false;
    g_TimerRelax.on_relax = [&]()
    {
        if (!ran)
        {
            ran = true;
            CHECK_RES(TH::RES_OK, f.host.Shutdown());
            f.tick->Run();
        }
    };

    CHECK_RES(TH::RES_INACTIVE, f.host.StopSync(t));
    CHECK_TRUE(ran);
}

// a callback must not wait for its own (or another timer's) callback: a stop from a task of the host returns at once
TEST(TimerHost, TaskMode_StopSync_FromHostTask_DoesNotWaitForCallback)
{
    TestTimer t;
    TimerTaskFixture f; // the test body runs as the first task: a handler task of the host
    TimerRelaxScope relax(f.kernel);

    CHECK_TRUE(f.host.IsHostTid(GetTid()));

    TimerRunning(f.host)[0] = &t; // would never be cleared: waiting for it would hang

    bool ran = false;
    g_TimerRelax.on_relax = [&]()
    {
        if (!ran)
        {
            ran = true;
            CHECK_RES(TH::RES_OK, f.host.Shutdown());
            f.tick->Run();
        }
    };

    CHECK_RES(TH::RES_INACTIVE, f.host.StopSync(t));
    CHECK_TRUE(ran);

    TimerRunning(f.host)[0] = nullptr;
}

// ---- coverage completion: rarely reachable paths of the timer host ----

// m_active of the timer and m_ready of the host are private: reached through explicit instantiations, to play "the
// timer was deactivated behind the host" and "the host was shut down by another core in the middle of a call".
struct TimerActiveTag
{
    typedef volatile bool time::Timer::*type;
    friend type GetActiveFlag(TimerActiveTag);
};

struct TimerReadyTag
{
    typedef volatile bool TH::*type;
    friend type GetReadyFlag(TimerReadyTag);
};

template <class Tag, typename Tag::type Member>
struct TimerFlagRob
{
    friend typename Tag::type GetActiveFlag(Tag) { return Member; }
};

template <class Tag, typename Tag::type Member>
struct TimerReadyRob
{
    friend typename Tag::type GetReadyFlag(Tag) { return Member; }
};

template struct TimerFlagRob<TimerActiveTag, &time::Timer::m_active>;
template struct TimerReadyRob<TimerReadyTag, &TH::m_ready>;

// the internal tasks of the host are plain ITask implementations: the defaults of the optional methods
TEST(TimerHost, TaskMode_WorkerTask_ItaskDefaults)
{
    TimerTaskFixture f;

    ITask *const tasks[2] = { f.tick, f.handler };

    for (int i = 0; i < 2; ++i)
    {
        tasks[i]->OnDeadlineMissed(0U);
        tasks[i]->OnExit();

        CHECK_EQUAL((int)DEFAULT_WEIGHT, (int)tasks[i]->GetWeight());
        CHECK_TRUE(tasks[i]->GetTraceName() == NULL);
    }
}

// a timer deactivated behind the back of the host is dropped from the list of the active timers by the next scan
TEST(TimerHost, Polled_Scan_UnlinksTimerDeactivatedBehindTheHost)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 50));
    CHECK_EQUAL(50, (int)p.host.Process());
    CHECK_FALSE(p.host.IsEmpty());

    (t.*GetActiveFlag(TimerActiveTag())) = false;

    CHECK_EQUAL((int)WAIT_INFINITE, (int)p.host.Process());
    CHECK_TRUE(p.host.IsEmpty());
}

// a periodic timer whose expiration is not claimed by a handler yet is polled every tick, the handler claims it later
TEST(TimerHost, TaskMode_TickTask_PeriodicPendingNotClaimed_PollsEveryTick)
{
    TestTimer t;
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    const Ticks t0 = GetTicks();

    CHECK_RES(TH::RES_OK, f.host.Start(t, 10, 10));

    bool shut_down = false;
    g_TimerRelax.on_relax = [&]()
    {
        if (!shut_down && (GetTicks() >= (t0 + 25)))
        {
            shut_down = true;
            CHECK_RES(TH::RES_OK, f.host.Shutdown());
        }
    };

    f.tick->Run(); // nobody runs the handler: the expiration stays pending while the periodic deadline is reloaded

    CHECK_TRUE(shut_down);
    CHECK_EQUAL(0, t.count);

    f.handler->Run(); // delivers the expiration, then exits on the shutdown

    CHECK_EQUAL(1, t.count);
}

// a stop cancels the expiration which has been handed over to the ready list but not claimed by a handler
TEST(TimerHost, TaskMode_TickTask_Stop_CancelsQueuedExpiration)
{
    TestTimer t;
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    const Ticks t0 = GetTicks();

    CHECK_RES(TH::RES_OK, f.host.Start(t, 10, 10));

    bool stopped = false;
    g_TimerRelax.on_relax = [&]()
    {
        if (!stopped && (GetTicks() >= (t0 + 15)))
        {
            stopped = true;
            CHECK_RES(TH::RES_OK, f.host.Stop(t));
            CHECK_RES(TH::RES_OK, f.host.Shutdown());
        }
    };

    f.tick->Run();

    CHECK_TRUE(stopped);

    f.handler->Run();

    CHECK_EQUAL(0, t.count);
}

// a handler task with nothing to deliver sleeps until the tick task shuts down
TEST(TimerHost, TaskMode_HandlerTask_WaitsForExpiration)
{
    TimerTaskFixture f;
    TimerRelaxScope  relax(f.kernel);

    bool ran = false;
    g_TimerRelax.on_relax = [&]()
    {
        if (!ran)
        {
            ran = true;
            CHECK_RES(TH::RES_OK, f.host.Shutdown());
            f.tick->Run();
        }
    };

    f.handler->Run();

    CHECK_TRUE(ran);
}

// the queue is full: an asynchronous re-arm waits for a free slot (the tick task drains the queue meanwhile) and
// completes its claimed record
TEST(TimerHost, TaskMode_QueueFull_CallerWaitsForFreeSlot_RestartClaimCompleted)
{
    const size_t cap = STK_TIMER_COMMAND_QUEUE_SIZE;
    std::vector<TestTimer *> timers;
    for (size_t i = 0; i <= cap; ++i)
        timers.push_back(new TestTimer((int)i));

    {
        TimerUserTaskFixture f;
        TimerRelaxScope      relax(f.kernel);

        for (size_t i = 0; i < (cap - 1); ++i)
            CHECK_RES(TH::RES_OK, f.host.Start(*timers[i], 100));
        CHECK_RES(TH::RES_OK, f.host.Shutdown()); // the last slot: the tick task stops after draining

        bool ran = false;
        g_TimerRelax.on_relax = [&]()
        {
            if (!ran)
            {
                ran = true;
                f.tick->Run();
            }
        };

        // full queue: waits, the hook drains it, the command is written
        CHECK_RES(TH::RES_OK, f.host.Restart(*timers[cap], 100));
        CHECK_TRUE(ran);
        CHECK_EQUAL(0U, (unsigned)f.host.GetSize());

        g_TimerRelax.on_relax = nullptr;
    }

    for (size_t i = 0; i <= cap; ++i)
        delete timers[i];
}

// the wait of a synchronous command for a free queue slot is cancelled: nothing is queued
TEST(TimerHost, TaskMode_SyncCommand_QueueFull_WaitCancelled)
{
    const size_t cap = STK_TIMER_COMMAND_QUEUE_SIZE;
    std::vector<TestTimer *> timers;
    for (size_t i = 0; i <= cap; ++i)
        timers.push_back(new TestTimer((int)i));

    {
        TimerUserTaskFixture f;
        TimerRelaxScope      relax(f.kernel);

        for (size_t i = 0; i < cap; ++i)
            CHECK_RES(TH::RES_OK, f.host.Start(*timers[i], 100));

        bool ran = false;
        g_TimerRelax.on_relax = [&]()
        {
            if (!ran)
            {
                ran = true;
                f.kernel.CancelTaskWait(&f.user);
            }
        };

        CHECK_RES(TH::RES_CANCELLED, f.host.StartSync(*timers[cap], 100));
        CHECK_TRUE(ran);

        g_TimerRelax.on_relax = nullptr;
    }

    for (size_t i = 0; i <= cap; ++i)
        delete timers[i];
}

// the host is shut down by "another core" between the check of the caller and the locked section which writes the command
static TH *s_RaceHost = NULL;

static void RaceShutdownHook()
{
    if (s_RaceHost != NULL)
        (s_RaceHost->*GetReadyFlag(TimerReadyTag())) = false;
}

TEST(TimerHost, PushCommand_ShutdownDuringCall_ReportsNotReady)
{
    TestTimer t;
    PolledHost p;

    s_RaceHost = &p.host;
    g_CriticalSectionEnterHook = RaceShutdownHook;

    const TH::EResult res = p.host.Start(t, 10);

    g_CriticalSectionEnterHook = NULL;
    s_RaceHost = NULL;
    (p.host.*GetReadyFlag(TimerReadyTag())) = true; // let the fixture shut the host down properly

    CHECK_RES(TH::RES_NOT_READY, res);
    CHECK_FALSE(t.IsActive());
}

// WaitCallbackDone() is private: called directly (explicit instantiation) from an application task, so the polling of
// the running slots is exercised without depending on how a synchronous stop reaches it.
struct TimerWaitDoneTag
{
    typedef void (TH::*type)(time::Timer &);
    friend type GetWaitCallbackDone(TimerWaitDoneTag);
};

template <class Tag, typename Tag::type Member>
struct TimerWaitDoneRob
{
    friend typename Tag::type GetWaitCallbackDone(Tag) { return Member; }
};

template struct TimerWaitDoneRob<TimerWaitDoneTag, &TH::WaitCallbackDone>;

// the callback of the timer is being executed: the caller polls the slot (sleeping a tick between the polls) until it is cleared
TEST(TimerHost, WaitCallbackDone_FromAppTask_PollsUntilCallbackReturns)
{
    TestTimer t, other;
    TimerUserTaskFixture f;
    TimerRelaxScope relax(f.kernel);

    CHECK_FALSE(f.host.IsHostTid(GetTid()));

    // the other slot holds another timer: it must not keep the caller waiting
    TimerRunning(f.host)[0] = &t;

    int polls = 0;
    g_TimerRelax.on_relax = [&]()
    {
        // called by the sleep between the polls: the callback returns on the 3rd one
        if (++polls >= 3)
            TimerRunning(f.host)[0] = nullptr;
    };

    (f.host.*GetWaitCallbackDone(TimerWaitDoneTag()))(t);

    CHECK_TRUE(polls >= 3); // the caller slept (more than once) until the slot was cleared
    CHECK_TRUE(TimerRunning(f.host)[0] == nullptr);
}

// no slot holds the timer (another timer is running): returns at once, without sleeping
TEST(TimerHost, WaitCallbackDone_FromAppTask_OtherTimerRunning_DoesNotWait)
{
    TestTimer t, other;
    TimerUserTaskFixture f;
    TimerRelaxScope relax(f.kernel);

    TimerRunning(f.host)[0] = &other;

    int polls = 0;
    g_TimerRelax.on_relax = [&]() { ++polls; };

    (f.host.*GetWaitCallbackDone(TimerWaitDoneTag()))(t);

    CHECK_EQUAL(0, polls);

    TimerRunning(f.host)[0] = nullptr;
}

// the sequence number of the re-arm record wraps around: 0 means "no record", so it is skipped
TEST(TimerHost, Polled_Restart_RearmSequenceWrapsAroundSkippingZero)
{
    TestTimer t;
    PolledHost p;

    t.SetRearmSeqForTest(0xFFFFFFFFU); // the next re-arm record wraps to 0

    CHECK_RES(TH::RES_OK, p.host.Restart(t, 10));

    CHECK_EQUAL(1U, (unsigned)t.GetRearmSeqForTest()); // 0 is skipped

    // the command which carries the record is executed normally
    p.host.Process();
    CHECK_TRUE(t.IsActive());
}

// ---- unknown command id ----

// PushCommand() is private and TimerCommand is a private type: both are reached through an explicit instantiation (which is
// exempt from access checking) to queue a command with an id which the tick task does not know, the only way to reach the
// defensive "default" branch of ProcessCommands().
template <class F>
struct TimerPushTraits;

template <class R, class C, class A>
struct TimerPushTraits<R (C::*)(A, Timeout, bool)>
{
    typedef A Command;
};

void TimerInjectCommand(TH &host, uint8_t id);

// the body is a plain function template (not defined inside the friend): GCC 11 can not handle a local of a dependent type
// declared in a friend function which is defined in a class template
template <class M, M Fn>
static void TimerPushUnknownCommand(TH &host, uint8_t id)
{
    typename TimerPushTraits<M>::Command cmd = {};

    cmd.cmd = static_cast<decltype(cmd.cmd)>(id); // no timer, no completion: nothing but the id is looked at
    STK_UNUSED((host.*Fn)(cmd, NO_WAIT, false));
}

template <class M, M Fn>
struct TimerPushRob
{
    friend void TimerInjectCommand(TH &host, uint8_t id)
    {
        TimerPushUnknownCommand<M, Fn>(host, id);
    }
};

template struct TimerPushRob<decltype(&TH::PushCommand), &TH::PushCommand>;

// an unknown command is asserted (debug build) and discarded (failed for a synchronous caller), the tick task goes on
TEST(TimerHost, Polled_Process_UnknownCommand_IsDiscarded)
{
    TestTimer t;
    PolledHost p;

    // out of the range of TimerCommand::EId: just past the last id and the largest possible one
    const uint8_t unknown_id = 0x0AU;
    const uint8_t max_id     = 0xFFU;

    TimerInjectCommand(p.host, unknown_id);
    TimerInjectCommand(p.host, max_id);

    // a valid command queued behind the unknown ones must still be executed
    CHECK_RES(TH::RES_OK, p.host.Start(t, 10));

    // the assertion is expected and must not interrupt the command loop (the handler returns instead of throwing)
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);

    const Timeout next = p.host.Process();

    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);

    CHECK_EQUAL(10, (int)next);
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(1U, p.host.GetSize());

    // the host is still fully operational
    At(10);
    p.host.Process();
    CHECK_EQUAL(1, t.count);
}

// ---- Reset / SetPeriod: the timer changes state between the call and the execution of the command ----

// Reset() and SetPeriod() check the timer when they are called, the tick task checks it again when the command is executed:
// a command queued ahead of them (Stop(), Restart()) can change the timer in between. The result is not visible to an
// asynchronous caller, so the timer must be left as the commands before it left it.

TEST(TimerHost, Polled_Reset_TimerStoppedBeforeExecution_IsNotReset)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 100, 20));
    p.host.Process();
    CHECK_TRUE(t.IsActive());

    At(5);
    CHECK_RES(TH::RES_OK, p.host.Stop(t));
    CHECK_RES(TH::RES_OK, p.host.Reset(t)); // still active when called: queued behind Stop()

    CHECK_EQUAL((int)WAIT_INFINITE, (int)p.host.Process()); // Stop() first, Reset() finds the timer inactive (RES_INACTIVE)
    CHECK_FALSE(t.IsActive());
    CHECK_TRUE(p.host.IsEmpty());
    CHECK_EQUAL(0, t.count);
}

TEST(TimerHost, Polled_Reset_TimerBecameOneShotBeforeExecution_IsNotReset)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 100, 20));
    p.host.Process();

    At(5);
    CHECK_RES(TH::RES_OK, p.host.Restart(t, 50)); // one-shot from 5: deadline 55
    CHECK_RES(TH::RES_OK, p.host.Reset(t));       // still periodic when called: queued behind Restart()

    CHECK_EQUAL(50, (int)p.host.Process()); // Restart() first, Reset() finds an active one-shot (RES_NOT_PERIODIC)
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(0U, t.GetPeriod());
    CHECK_EQUAL(55, t.GetDeadline()); // not moved by Reset()
}

TEST(TimerHost, Polled_SetPeriod_TimerStoppedBeforeExecution_IsNotChanged)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 100, 20));
    p.host.Process();

    At(5);
    CHECK_RES(TH::RES_OK, p.host.Stop(t));
    CHECK_RES(TH::RES_OK, p.host.SetPeriod(t, 40)); // still active when called: queued behind Stop()

    CHECK_EQUAL((int)WAIT_INFINITE, (int)p.host.Process()); // SetPeriod() finds the timer inactive (RES_INACTIVE)
    CHECK_FALSE(t.IsActive());
    CHECK_EQUAL(20U, t.GetPeriod()); // not changed
}

TEST(TimerHost, Polled_SetPeriod_TimerBecameOneShotBeforeExecution_IsNotChanged)
{
    TestTimer t;
    PolledHost p;

    CHECK_RES(TH::RES_OK, p.host.Start(t, 100, 20));
    p.host.Process();

    At(5);
    CHECK_RES(TH::RES_OK, p.host.Restart(t, 50));    // one-shot from 5: deadline 55
    CHECK_RES(TH::RES_OK, p.host.SetPeriod(t, 40));  // still periodic when called: queued behind Restart()

    CHECK_EQUAL(50, (int)p.host.Process()); // SetPeriod() finds an active one-shot (RES_NOT_PERIODIC)
    CHECK_TRUE(t.IsActive());
    CHECK_EQUAL(0U, t.GetPeriod()); // stays a one-shot

    At(55);
    p.host.Process();
    CHECK_EQUAL(1, t.count);
    CHECK_FALSE(t.IsActive());
}

} // namespace stk
} // namespace test
