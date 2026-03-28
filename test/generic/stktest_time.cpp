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

//! Mock of GetTimeNowMsec().
namespace stk
{
    static struct KernelServiceMock : public IKernelService
    {
        int64_t ticks;
        int32_t resolution;

        static IKernelService *GetInstance() { return NULL; }
        TId GetTid() const { return 0; }
        Ticks GetTicks() const { return ticks; }
        int32_t GetTickResolution() const { return resolution; }
        void Delay(Timeout msec) { (void)msec; }
        void Sleep(Timeout msec) { (void)msec; }
        void SwitchToNext() {}
        IWaitObject *Wait(ISyncObject *sobj, IMutex *mutex, Timeout timeout)
        {
            (void)sobj;
            (void)mutex;
            (void)timeout;
            return nullptr;
        }
    }
    s_KernelServiceMock;

    void SetTimeNowMsec(int64_t now)
    {
        test::g_KernelService = &s_KernelServiceMock;

        s_KernelServiceMock.resolution = 1000;
        s_KernelServiceMock.ticks = GetTicksFromMsec(now, s_KernelServiceMock.resolution);
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
