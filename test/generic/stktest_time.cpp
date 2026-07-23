/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

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
        void Sleep(Timeout ticks) override { (void)ticks; }
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
        void Wake(ISyncObject *sobj, bool all)
        {
            STK_UNUSED(sobj);
            STK_UNUSED(all);
        }
        Timeout Suspend() override { return 1; }
        void Resume(Timeout elapsed_ticks) override { (void)elapsed_ticks; }
        void InheritWeight(TId tid, Weight weight) override { (void)tid; (void)weight; }
        void RestoreWeight(TId tid, ISyncObject *sobj) override { (void)tid; (void)sobj; }
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

} // namespace stk
} // namespace test
