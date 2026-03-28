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
// ================================ Sync ====================================== //
// ============================================================================ //

TEST_GROUP(Sync)
{
    void setup() {}
    void teardown() {}
};

TEST(Sync, CriticalSection)
{
    CHECK_EQUAL(0, test::g_CriticalSectionState);
    {
        sync::ScopedCriticalSection cs;
        CHECK_EQUAL(1, test::g_CriticalSectionState);
    }
    CHECK_EQUAL(0, test::g_CriticalSectionState);
}

TEST(Sync, IMutex_ScopedLock)
{
    MutexMock mutex;
    {
        MutexMock::ScopedLock guard(mutex);
        CHECK_TRUE(mutex.m_locked);
    }
    CHECK_FALSE(mutex.m_locked);
}

static struct SyncWaitWakeRelaxCpuContext
{
    SyncWaitWakeRelaxCpuContext()
    {
        wake_all = false;
        counter  = 0;
        platform = NULL;
        sobj     = NULL;
    }

    bool              wake_all;
    uint32_t          counter;
    PlatformTestMock *platform;
    SyncObjectMock   *sobj;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (!wake_all && (counter >= 5))
            sobj->WakeOne();
        else
        if (wake_all && (counter >= 7))
            sobj->WakeAll();
    }
}
g_SyncWaitWakeRelaxCpuContext;

static void SyncWaitWakeRelaxCpu()
{
    g_SyncWaitWakeRelaxCpuContext.Process();
}

TEST(Sync, SyncWait_Wake)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncWaitWakeRelaxCpuContext.platform = platform;
    g_SyncWaitWakeRelaxCpuContext.sobj     = &sobj;
    g_RelaxCpuHandler = SyncWaitWakeRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    // short wait in order to get wait object for a Wake testing
    IWaitObject *wo = IKernelService::GetInstance()->Wait(&sobj, &mutex, 10);
    CHECK_TRUE(wo != nullptr); // expect wait object in return after timeout

    // woken by WakeOne() on 5th tick, became active on 6th (see SyncWaitWakeRelaxCpuContext::Process)
    CHECK_EQUAL(6, g_SyncWaitWakeRelaxCpuContext.counter);
    CHECK_FALSE(wo->IsTimeout()); // expect no timeout
    CHECK_EQUAL(true, mutex.m_locked); // expect locked mutex after Wait return

    g_SyncWaitWakeRelaxCpuContext.counter  = 0;
    g_SyncWaitWakeRelaxCpuContext.wake_all = true;

    // repeat for WakeAll()
    wo = IKernelService::GetInstance()->Wait(&sobj, &mutex, 10);
    CHECK_TRUE(wo != nullptr); // expect wait object in return after timeout

    // woken by WakeAll() on 7th tick, became active on 8th (see SyncWaitWakeRelaxCpuContext::Process)
    CHECK_EQUAL(8, g_SyncWaitWakeRelaxCpuContext.counter);
    CHECK_FALSE(wo->IsTimeout()); // expect no timeout
    CHECK_EQUAL(true, mutex.m_locked); // expect locked mutex after Wait return
}

} // namespace stk
} // namespace test
