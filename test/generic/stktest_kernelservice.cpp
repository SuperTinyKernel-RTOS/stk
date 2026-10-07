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
// =+========================= KernelService ================================== //
// ============================================================================ //

TEST_GROUP(KernelService)
{
    void setup() {}
    void teardown()
    {
        g_RelaxCpuHandler = NULL;

        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
    }
};

TEST(KernelService, GetMsecToTicks)
{
    KernelServiceMock mock;
    mock.m_ticks = 1;

    mock.m_resolution = 1000;
    CHECK_EQUAL(10, (int32_t)GetMsFromTicks(10, mock.GetTickResolution()));

    mock.m_resolution = 10000;
    CHECK_EQUAL(100, (int32_t)GetMsFromTicks(10, mock.GetTickResolution()));
}

static struct DelayContext
{
    DelayContext() : platform(NULL)
    {
        Clear();
    }

    IPlatform *platform;

    void Clear()
    {
        platform = nullptr;
    }

    void Process()
    {
        platform->ProcessTick();
    }
}
g_DelayContext;

static void DelayRelaxCpu()
{
    g_DelayContext.Process();
}

TEST(KernelService, Delay)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    g_RelaxCpuHandler = DelayRelaxCpu;
    g_DelayContext.platform = kernel.GetPlatform();

    Delay(10);

    g_RelaxCpuHandler = NULL;

    CHECK_EQUAL(10, (int32_t)g_KernelService->GetTicks());
}

TEST(KernelService, DelayMs)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize(2000); // decrease resolution to 1 tick = 2 ms
    kernel.AddTask(&task);
    kernel.Start();

    g_RelaxCpuHandler = DelayRelaxCpu;
    g_DelayContext.platform = kernel.GetPlatform();

    DelayMs(10);

    g_RelaxCpuHandler = NULL;

    CHECK_EQUAL(5, (int32_t)g_KernelService->GetTicks());
}

TEST(KernelService, GetTid)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    platform->ProcessTick();

    // task/thread id is a pointer to the user task
    size_t tid = stk::GetTid();
    CHECK_EQUAL(tid, (size_t)&task);
}

TEST(KernelService, GetTickResolution)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    const uint32_t periodicity = PERIODICITY_DEFAULT + 1;

    kernel.Initialize(periodicity);
    kernel.AddTask(&task);
    kernel.Start();

    CHECK_EQUAL(periodicity, g_KernelService->GetTickResolution());
    CHECK_EQUAL(periodicity, stk::GetTickResolution());
}

TEST(KernelService, GetTicks)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize(PERIODICITY_DEFAULT);
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // ISR calls OnSysTick 1-st time
    platform->ProcessTick();
    CHECK_EQUAL(1, (int32_t)g_KernelService->GetTicks());
    CHECK_EQUAL(1, (int32_t)stk::GetTicks());

    // ISR calls OnSysTick 2-nd time
    platform->ProcessTick();
    CHECK_EQUAL(2, (int32_t)g_KernelService->GetTicks());
    CHECK_EQUAL(2, (int32_t)stk::GetTicks());
}

TEST(KernelService, GetTimeNowMs)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize(PERIODICITY_DEFAULT);
    kernel.AddTask(&task1);
    kernel.Start();

    CHECK_EQUAL(0, (int32_t)stk::GetTimeNowMs());

    // make 1000 ticks
    for (int32_t i = 0; i < 1000; ++i)
        platform->ProcessTick();

    // 1000 usec * 1000 ticks = 1000 ms
    CHECK_EQUAL(1000, (int32_t)stk::GetTimeNowMs());
}

TEST(KernelService, GetTimeNowMsWith10UsecTick)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    // set periodicity to 10 microsecond
    kernel.Initialize(10);
    kernel.AddTask(&task1);
    kernel.Start();

    CHECK_EQUAL(0, (int32_t)stk::GetTimeNowMs());

    // make 1000 ticks
    for (int32_t i = 0; i < 1000; ++i)
        platform->ProcessTick();

    // 10 usec * 1000 ticks = 10 ms
    CHECK_EQUAL(10, (int32_t)stk::GetTimeNowMs());
}

static struct SwitchToNextRelaxCpuContext
{
    SwitchToNextRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        platform = NULL;
        task1    = NULL;
        task2    = NULL;
    }

    uint32_t               counter;
    PlatformTestMock      *platform;
    TaskMock<ACCESS_USER> *task1, *task2;

    void Process()
    {
        Stack *&active = platform->m_stack_active;

        platform->ProcessTick();

        if ((task1 != nullptr) && (task2 != nullptr))
        {
            // ISR calls OnSysTick (task1 = active, task2 = idle)
            if (counter == 0)
            {
                CHECK_EQUAL(active->SP, (size_t)task1->GetStack());
            }
            else
            // ISR calls OnSysTick (task1 = idle, task2 = active)
            if (counter == 1)
            {
                CHECK_EQUAL(active->SP, (size_t)task2->GetStack());
            }
            else
            // ISR calls OnSysTick (task1 = active, task2 = idle)
            if (counter == 2)
            {
                CHECK_EQUAL(active->SP, (size_t)task1->GetStack());
            }
            else
            // ISR calls OnSysTick (task1 = idle, task2 = active)
            if (counter == 3)
            {
                CHECK_EQUAL(active->SP, (size_t)task2->GetStack());
            }
        }

        ++counter;
    }
}
g_SwitchToNextRelaxCpuContext;

static void SwitchToNextRelaxCpu()
{
    g_SwitchToNextRelaxCpuContext.Process();
}

TEST(KernelService, SwitchToNext)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is scheduled first by RR
    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    // ISR calls OnSysTick (task1 = idle, task2 = active)
    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

    g_RelaxCpuHandler = SwitchToNextRelaxCpu;
    g_SwitchToNextRelaxCpuContext.Clear();
    g_SwitchToNextRelaxCpuContext.platform = platform;
    g_SwitchToNextRelaxCpuContext.task1    = &task1;
    g_SwitchToNextRelaxCpuContext.task2    = &task2;

    // task1 calls SwitchToNext (to test path: IKernelService::SwitchToNext -> IPlatform::SwitchToNext -> Kernel::SwitchToNext)
    Yield();
    CHECK_EQUAL(1, platform->m_switch_to_next_nr);

    // task2 is active again
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

    // task2 calls SwitchToNext
    platform->EventTaskSwitch(active->SP);

    // task1 calls SwitchToNext (task1 = active, task2 = idle)
    platform->EventTaskSwitch(active->SP + 1); // add shift to test IsMemoryOfSP

    // after a switch task 2 is active again
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());
}

TEST(KernelService, SwitchToNextInactiveTask)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is scheduled first by RR
    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    // ISR calls OnSysTick (task1 = idle, task2 = active)
    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

    g_RelaxCpuHandler = SwitchToNextRelaxCpu;
    g_SwitchToNextRelaxCpuContext.Clear();
    g_SwitchToNextRelaxCpuContext.platform = platform;
    g_SwitchToNextRelaxCpuContext.task1    = nullptr;
    g_SwitchToNextRelaxCpuContext.task2    = nullptr;

    platform->EventTaskSwitch(platform->m_stack_idle->SP + 1); // add shift to test IsMemoryOfSP
}

static struct SleepRelaxCpuContext
{
    SleepRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        platform = NULL;
        task1    = NULL;
        task2    = NULL;
    }

    uint32_t               counter;
    PlatformTestMock      *platform;
    TaskMock<ACCESS_USER> *task1, *task2;

    void Process()
    {
        Stack *&active = platform->m_stack_active;

        platform->ProcessTick();

        // ISR calls OnSysTick (task1 = active, task2 = idle)
        if (counter == 0)
        {
            CHECK_EQUAL_TEXT(active->SP, (size_t)task1->GetStack(), "sleep: expecting task1");
        }
        else
        if (counter == 1)
        {
            // task2 still sleeping here
            CHECK_EQUAL_TEXT(active->SP, (size_t)task1->GetStack(), "sleep: expecting task1");
        }
        else
        // ISR calls OnSysTick (task1 = idle, task2 = active)
        if (counter == 2)
        {
            CHECK_EQUAL_TEXT(active->SP, (size_t)task2->GetStack(), "sleep: expecting task2");
        }

        ++counter;
    }
}
g_SleepRelaxCpuContext;

static void SleepRelaxCpu()
{
    g_SleepRelaxCpuContext.Process();
}

template <class _SwitchStrategy>
static void TestTaskSleep(bool until)
{
    Kernel<KERNEL_STATIC, 2, _SwitchStrategy, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // on start Round-Robin selects the very first task
    CHECK_EQUAL_TEXT(active->SP, (size_t)task1.GetStack(), "expecting task1");

    // ISR calls OnSysTick (task1 = idle, task2 = active)
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (size_t)task2.GetStack(), "expecting task2");

    g_RelaxCpuHandler = SleepRelaxCpu;
    g_SleepRelaxCpuContext.Clear();
    g_SleepRelaxCpuContext.platform = platform;
    g_SleepRelaxCpuContext.task1    = &task1;
    g_SleepRelaxCpuContext.task2    = &task2;

    // task2 calls Sleep to become idle
    if (until)
    {
        SleepUntil(GetTicks() + 2);
    }
    else
    {
        Sleep(2);
    }

    // task2 slept 2 ticks and became active again when became a tail of previously active task1
    CHECK_EQUAL_TEXT(active->SP, (size_t)task2.GetStack(), "expecting task2 after sleep");

    // ISR calls OnSysTick (task1 = active, task2 = idle)
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (size_t)task1.GetStack(), "expecting task1 after next tick");
}

TEST(KernelService, SleepRR)
{
    TestTaskSleep<SwitchStrategyRR>(false);
}

TEST(KernelService, SleepSWRR)
{
    TestTaskSleep<SwitchStrategySWRR>(false);
}

TEST(KernelService, SleepFP31)
{
    TestTaskSleep<SwitchStrategyFP32>(false);
}

TEST(KernelService, SleepUntilRR)
{
    TestTaskSleep<SwitchStrategyRR>(true);
}

TEST(KernelService, SleepUntilSWRR)
{
    TestTaskSleep<SwitchStrategySWRR>(true);
}

TEST(KernelService, SleepUntilFP31)
{
    TestTaskSleep<SwitchStrategyFP32>(true);
}

TEST(KernelService, SleepMsRR)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // on start Round-Robin selects the very first task
    CHECK_EQUAL_TEXT(active->SP, (size_t)task1.GetStack(), "expecting task1");

    // ISR calls OnSysTick (task1 = idle, task2 = active)
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (size_t)task2.GetStack(), "expecting task2");

    g_RelaxCpuHandler = SleepRelaxCpu;
    g_SleepRelaxCpuContext.Clear();
    g_SleepRelaxCpuContext.platform = platform;
    g_SleepRelaxCpuContext.task1    = &task1;
    g_SleepRelaxCpuContext.task2    = &task2;

    // task2 calls Sleep to become idle
    SleepMs(2);

    // task2 slept 2 ticks and became active again when became a tail of previously active task1
    CHECK_EQUAL_TEXT(active->SP, (size_t)task2.GetStack(), "expecting task2 after sleep");

    // ISR calls OnSysTick (task1 = active, task2 = idle)
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (size_t)task1.GetStack(), "expecting task1 after next tick");
}

TEST(KernelService, SleepUntilMissDeadline)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // on start Round-Robin selects the very first task
    CHECK_EQUAL_TEXT(active->SP, (size_t)task1.GetStack(), "expecting task1");

    // ISR calls OnSysTick (task1 = idle, task2 = active)
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(active->SP, (size_t)task2.GetStack(), "expecting task2");

    g_RelaxCpuHandler = SleepRelaxCpu;
    g_SleepRelaxCpuContext.Clear();
    g_SleepRelaxCpuContext.platform = platform;
    g_SleepRelaxCpuContext.task1    = &task1;
    g_SleepRelaxCpuContext.task2    = &task2;

    Ticks now = GetTicks();

    // task2 calls Sleep to become idle
    SleepUntil(now);

    CHECK_EQUAL((int)now, (int)GetTicks());

    // task2 is still active as it did not sleep
    CHECK_EQUAL_TEXT(active->SP, (size_t)task2.GetStack(), "expecting task2 after sleep");
}

static struct SleepAllAndWakeRelaxCpuContext
{
    SleepAllAndWakeRelaxCpuContext()
    {
        counter  = 0;
        platform = NULL;
        task1    = NULL;
    }

    uint32_t               counter;
    PlatformTestMock      *platform;
    TaskMock<ACCESS_USER> *task1;

    void Process()
    {
        Stack *&idle = platform->m_stack_idle, *&active = platform->m_stack_active;

        platform->ProcessTick();

        // ISR calls OnSysTick (task1 = idle, sleep_trap = active)
        if (counter == 0)
        {
            CHECK_EQUAL(idle->SP, (size_t)task1->GetStack());
            CHECK_EQUAL(active->SP, (size_t)platform->m_stack_info[STACK_SLEEP_TRAP].stack->SP);
        }
        else
        if ((counter == 1) || (counter == 2))
        {
            // to check FSM_STATE_NONE case
        }
        else
        // ISR calls OnSysTick (task1 = active, sleep_trap = idle)
        if (counter == 3)
        {
            CHECK_EQUAL(active->SP, (size_t)task1->GetStack());
            CHECK_EQUAL(idle->SP, (size_t)platform->m_stack_info[STACK_SLEEP_TRAP].stack->SP);
        }

        ++counter;
    }
}
g_SleepAllAndWakeRelaxCpuContext;

static void SleepAllAndWakeRelaxCpu()
{
    g_SleepAllAndWakeRelaxCpuContext.Process();
}

TEST(KernelService, SleepAllAndWake)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    g_RelaxCpuHandler = SleepAllAndWakeRelaxCpu;
    g_SleepAllAndWakeRelaxCpuContext.platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    g_SleepAllAndWakeRelaxCpuContext.task1 = &task;

    // task1 calls Sleep
    Sleep(3);
}

#if STK_TICKLESS_IDLE
static struct SleepAndWakeTicklessRelaxCpuContext
{
    SleepAndWakeTicklessRelaxCpuContext()
    {
        counter  = 0;
        platform = NULL;
    }

    uint32_t         counter;
    PlatformTestMock *platform;

    void Process()
    {
        platform->ProcessTick();

        if (counter == 0)
        {
            // expecting 3 sleep ticks
            CHECK_EQUAL(3, platform->m_ticks_count);
        }

        ++counter;
    }
}
g_SleepAndWakeTicklessRelaxCpuContext;

static void SleepAndWakeTicklessRelaxCpu()
{
    g_SleepAndWakeTicklessRelaxCpuContext.Process();
}

TEST(KernelService, SleepAndWakeTickless)
{
    Kernel<KERNEL_STATIC | KERNEL_TICKLESS, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    g_RelaxCpuHandler = SleepAndWakeTicklessRelaxCpu;
    g_SleepAndWakeTicklessRelaxCpuContext.platform = platform;

    // task1 calls Sleep
    Sleep(3);

    // expect 4 ticks (1 + 3 sleep)
    CHECK_EQUAL(4, platform->m_ticks_count);

    // expect only 2 context switches (1st: Task=Idle, SleepCtx=Active, 2nd: Task=Active, SleepCtx=Idle)
    CHECK_EQUAL(2, platform->m_context_switch_nr);
}
#endif // STK_TICKLESS_IDLE

static struct SleepCancelRelaxCpuContext
{
    SleepCancelRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        platform = NULL;
        task1    = NULL;
    }

    uint32_t               counter;
    PlatformTestMock      *platform;
    TaskMock<ACCESS_USER> *task1;

    void Process()
    {
        platform->ProcessTick();

        if (counter == 0)
        {
            SleepCancel(GetTidFromUserTask(task1));
        }

        ++counter;
    }
}
g_SleepCancelRelaxCpuContext;

static void SleepCancelRelaxCpu()
{
    g_SleepCancelRelaxCpuContext.Process();
}

TEST(KernelService, SleepCancel)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    // on start Round-Robin selects the very first task
    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    g_RelaxCpuHandler = SleepCancelRelaxCpu;
    g_SleepCancelRelaxCpuContext.Clear();
    g_SleepCancelRelaxCpuContext.platform = platform;
    g_SleepCancelRelaxCpuContext.task1    = &task1;

    CHECK_EQUAL(0, platform->m_ticks_count);

    // task1 calls Sleep to become idle, inside SleepCancelRelaxCpu it will call SleepCancel
    Sleep(10);

    // 2 = 1 tick for going to sleep, +1 tick for going out of sleep
    CHECK_EQUAL(2, platform->m_ticks_count);
}

// ---------------------------------------------------------------------------- //
// Yield with SwitchStrategyRR_NSY (NOSLEEP_YIELD_API = 1):                      //
// Kernel::OnTaskSwitch() -> Strategy::OnTaskYield() == true -> ScheduleYield()  //
// -> BusyWaitWhileYielding(). The yielding task stays runnable (is NOT put to   //
// sleep) and the strategy cursor is rotated to the task's successor.            //
// ---------------------------------------------------------------------------- //

static struct YieldNsyRelaxCpuContext
{
    YieldNsyRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        platform = NULL;
    }

    uint32_t          counter;
    PlatformTestMock *platform;

    // invoked from the busy-wait loop of BusyWaitWhileYielding(): the tick consumes STATE_YIELD_PENDING
    // (UpdateTaskState) which releases the yielding task from the loop
    void Process()
    {
        ++counter;
        platform->ProcessTick();
    }
}
g_YieldNsyRelaxCpuContext;

static void YieldNsyRelaxCpu()
{
    g_YieldNsyRelaxCpuContext.Process();
}

TEST(KernelService, YieldNoSleepYieldSwitchesToNextTask)
{
    Kernel<KERNEL_STATIC, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.Start();

    // task1 is scheduled first by RR
    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    g_RelaxCpuHandler = YieldNsyRelaxCpu;
    g_YieldNsyRelaxCpuContext.Clear();
    g_YieldNsyRelaxCpuContext.platform = platform;

    // task1 calls Yield: IKernelService::SwitchToNext -> Kernel::OnTaskSwitch -> OnTaskYield (handled)
    // -> ScheduleYield -> BusyWaitWhileYielding (must return, pending yield consumed)
    Yield();
    CHECK_EQUAL(1, platform->m_switch_to_next_nr);

    // CPU is handed over to the next task in the rotation
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());
}

TEST(KernelService, YieldNoSleepYieldKeepsTaskRunnable)
{
    Kernel<KERNEL_STATIC, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.Start();

    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    g_RelaxCpuHandler = YieldNsyRelaxCpu;
    g_YieldNsyRelaxCpuContext.Clear();
    g_YieldNsyRelaxCpuContext.platform = platform;

    // task1 yields: it must stay in the runnable set (legacy yield would put it to sleep for YIELD_TICKS)
    Yield();
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

    g_RelaxCpuHandler = NULL;

    // rotation continues over all 3 tasks, task1 is not skipped or delayed by a sleep period
    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (size_t)task3.GetStack());

    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());
}

TEST(KernelService, YieldNoSleepYieldSingleTask)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    CHECK_EQUAL(active->SP, (size_t)task.GetStack());

    g_RelaxCpuHandler = YieldNsyRelaxCpu;
    g_YieldNsyRelaxCpuContext.Clear();
    g_YieldNsyRelaxCpuContext.platform = platform;

    // alone task yields: strategy hands the CPU back to the same task, the kernel must not fall into the sleep trap
    Yield();
    CHECK_EQUAL(1, platform->m_switch_to_next_nr);
    CHECK_EQUAL(active->SP, (size_t)task.GetStack());

    g_RelaxCpuHandler = NULL;

    // task is still runnable (a legacy yield would have put it to sleep)
    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (size_t)task.GetStack());
}

TEST(KernelService, YieldNoSleepYieldRepeated)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    g_RelaxCpuHandler = YieldNsyRelaxCpu;
    g_YieldNsyRelaxCpuContext.Clear();
    g_YieldNsyRelaxCpuContext.platform = platform;

    // each Yield() passes the CPU to the other task: task1 -> task2 -> task1 -> task2
    Yield();
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

    Yield();
    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    Yield();
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

    CHECK_EQUAL(3, platform->m_switch_to_next_nr);
}

TEST(KernelService, YieldNoSleepYieldHrtSingleTask)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task, 2, 1, 0);
    kernel.Start();

    g_YieldNsyRelaxCpuContext.Clear();
    g_YieldNsyRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = YieldNsyRelaxCpu;

    platform->ProcessTick();

    // in HRT mode Yield() signals work completion: Kernel::OnTaskSwitch must NOT take the NOSLEEP_YIELD path
    // but sleep the task until its next period (same as for non-NSY strategies)
    Yield();
    CHECK_EQUAL(1, platform->m_switch_to_next_nr);

    // the task has been put to sleep: the busy-wait must have been released by the tick handler
    CHECK_TRUE(g_YieldNsyRelaxCpuContext.counter != 0);

    // next tick continues scheduling normally
    platform->ProcessTick();

    CHECK_FALSE(platform->m_hard_fault);
    CHECK_EQUAL(0, task.m_deadline_missed);
}

static struct YieldNsyTraceRelaxCpuContext
{
    YieldNsyTraceRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        count    = 0;
        platform = NULL;

        for (size_t i = 0; i < STK_STATIC_ARRAY_SIZE(active_sp); ++i)
            active_sp[i] = 0;
    }

    enum { TRACE_MAX = 16 };

    uint32_t          count;
    PlatformTestMock *platform;
    size_t            active_sp[TRACE_MAX]; //!< SP of the active task observed after each tick

    // the mock simulates time passing while the yielding task is blocked in the busy-wait loop:
    // every call is one tick, record who is running after it
    void Process()
    {
        platform->ProcessTick();

        if (count < TRACE_MAX)
            active_sp[count] = platform->m_stack_active->SP;

        ++count;
    }
}
g_YieldNsyTraceRelaxCpuContext;

static void YieldNsyTraceRelaxCpu()
{
    g_YieldNsyTraceRelaxCpuContext.Process();
}

TEST(KernelService, YieldNoSleepYieldHrtHandsOverToNextTask)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 2, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1, 2, 2, 0);
    kernel.AddTask(&task2, 10, 10, 0);
    kernel.Start();

    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    g_YieldNsyTraceRelaxCpuContext.Clear();
    g_YieldNsyTraceRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = YieldNsyTraceRelaxCpu;

    // task1 completes its job and blocks inside Yield() until it is rescheduled (the mock spins ticks
    // in the busy-wait loop, exactly as the real time passes while the task is blocked)
    Yield();
    CHECK_EQUAL(1, platform->m_switch_to_next_nr);

    // the first tick (sleep entry) removes task1 from the runnable set: task2 gets the CPU
    CHECK_TRUE(g_YieldNsyTraceRelaxCpuContext.count != 0);
    CHECK_EQUAL(g_YieldNsyTraceRelaxCpuContext.active_sp[0], (size_t)task2.GetStack());

    // task1 woke up after its period, was appended behind task2 and then was scheduled again, this is
    // why Yield() returned (the task is active again)
    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    CHECK_FALSE(platform->m_hard_fault);
    CHECK_EQUAL(0, task1.m_deadline_missed);
    CHECK_EQUAL(0, task2.m_deadline_missed);
}

static struct YieldNsySleepingPeerRelaxCpuContext
{
    YieldNsySleepingPeerRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        platform         = NULL;
        phase            = 0;
        nested           = false;
        sp_after_sleep   = 0;
        sp_after_yield1  = 0;
        sp_after_yield2  = 0;
        sleeping_active  = false;
        ticks            = 0;
    }

    PlatformTestMock *platform;
    uint32_t          phase;
    bool              nested;
    size_t            sp_after_sleep;  //!< active task after the sleep-entry tick
    size_t            sp_after_yield1; //!< active task after the 1st Yield() issued while the peer sleeps
    size_t            sp_after_yield2; //!< active task after the 2nd Yield() issued while the peer sleeps
    size_t            sleeper_sp;      //!< SP of the sleeping task (must never be active while it sleeps)
    bool              sleeping_active; //!< set if the sleeping task was ever observed active before it woke
    uint32_t          ticks;

    // Called from the busy-wait of Sleep() issued by the sleeping peer (outer level) and from the
    // busy-wait of Yield() issued by running tasks (nested level).
    void Process()
    {
        if (nested)
        {
            // nested level: busy-wait of Yield(), one tick consumes the pending yield
            platform->ProcessTick();
            ++ticks;
            return;
        }

        // outer level: busy-wait of Sleep(), every call is a tick of time passing
        if (phase == 0)
        {
            // sleep-entry tick: the sleeping task leaves the runnable set
            platform->ProcessTick();
            ++ticks;
            sp_after_sleep = platform->m_stack_active->SP;
            phase = 1;
        }
        else
        if (phase == 1)
        {
            // the running task yields while its peer is sleeping
            nested = true;
            Yield();
            sp_after_yield1 = platform->m_stack_active->SP;

            // and the next running task yields too
            Yield();
            sp_after_yield2 = platform->m_stack_active->SP;
            nested = false;

            phase = 2;
        }
        else
        {
            platform->ProcessTick();
            ++ticks;

            // 1 entry tick + 10 sleep ticks: before that the sleeping task must never be scheduled
            if ((platform->m_stack_active->SP == sleeper_sp) && (ticks < 11))
            {
                sleeping_active = true;
            }
        }
    }
}
g_YieldNsySleepingPeerRelaxCpuContext;

static void YieldNsySleepingPeerRelaxCpu()
{
    g_YieldNsySleepingPeerRelaxCpuContext.Process();
}

TEST(KernelService, YieldNoSleepYieldSkipsSleepingPeer)
{
    Kernel<KERNEL_STATIC, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.Start();

    CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

    // task2 is scheduled: it will be the sleeping peer sitting between task1 and task3 in the rotation
    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

    g_YieldNsySleepingPeerRelaxCpuContext.Clear();
    g_YieldNsySleepingPeerRelaxCpuContext.platform   = platform;
    g_YieldNsySleepingPeerRelaxCpuContext.sleeper_sp = (size_t)task2.GetStack();
    g_RelaxCpuHandler = YieldNsySleepingPeerRelaxCpu;

    // task2 sleeps (the handler spins ticks until it wakes and, meanwhile, drives Yield() of the other tasks)
    Sleep(10);

    // sleep-entry tick removed task2 from the runnable set, task3 got the CPU
    CHECK_EQUAL(g_YieldNsySleepingPeerRelaxCpuContext.sp_after_sleep, (size_t)task3.GetStack());

    // task3 yields: rotation wraps to task1
    CHECK_EQUAL(g_YieldNsySleepingPeerRelaxCpuContext.sp_after_yield1, (size_t)task1.GetStack());

    // task1 yields: sleeping task2 (its former successor) is skipped, the CPU goes to task3
    CHECK_EQUAL(g_YieldNsySleepingPeerRelaxCpuContext.sp_after_yield2, (size_t)task3.GetStack());

    CHECK_EQUAL(2, platform->m_switch_to_next_nr);
    CHECK_FALSE(g_YieldNsySleepingPeerRelaxCpuContext.sleeping_active);

    g_RelaxCpuHandler = NULL;

    // after waking up task2 returns to the rotation
    bool task2_scheduled = (active->SP == (size_t)task2.GetStack());
    for (int i = 0; (i < 3) && !task2_scheduled; ++i)
    {
        platform->ProcessTick();
        task2_scheduled = (active->SP == (size_t)task2.GetStack());
    }
    CHECK_TRUE(task2_scheduled);
}

TEST(KernelService, SetWeightUnsupportedStrategy)
{
    // SwitchStrategyRR has no WEIGHT_API / PRIORITY_INHERITANCE_API: it does not handle OnTaskWeightChange()
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        g_KernelService->SetWeight(g_KernelService->GetTid(), 1);
        CHECK_TEXT(false, "SetWeight must assert if strategy does not handle OnTaskWeightChange()");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(KernelService, SetWeightUnsupportedStrategyReturnsNoWeight)
{
    // SwitchStrategyRR has no WEIGHT_API / PRIORITY_INHERITANCE_API: it does not handle OnTaskWeightChange()
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    // expect the assertion but do not throw from it, so execution continues to the return statement
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);

    const Weight result = g_KernelService->SetWeight(g_KernelService->GetTid(), 1);

    // restore (teardown does it too)
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);

    CHECK_EQUAL(NO_WEIGHT, result);
}

// ============================================================================ //
// =+==================== KernelServiceIsrSafety ============================== //
// ============================================================================ //

TEST_GROUP(KernelServiceIsrSafety)
{
    void setup()
    {
        g_RelaxCpuHandler = DelayRelaxCpu;
        g_InsideISR = true;
    }
    void teardown()
    {
        g_InsideISR = false;
        g_DelayContext.Clear();
        g_RelaxCpuHandler = NULL;

        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
    }
};

TEST(KernelServiceIsrSafety, Common)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    g_DelayContext.platform = kernel.GetPlatform();

    try
    {
        g_TestContext.ExpectAssert(true);
        Sleep(10);
        CHECK_TEXT(false, "Sleep is not allowed inside ISR");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        SleepUntil(GetTicks() + 10);
        CHECK_TEXT(false, "SleepUntil is not allowed inside ISR");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        Delay(10);
        CHECK_TEXT(false, "Delay is not allowed inside ISR");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        Yield();
        CHECK_TEXT(false, "Yield is not allowed inside ISR");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(KernelService, SysTimer)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    platform->m_systimer_count = 99;
    platform->m_systimer_freq  = 1000;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    uint32_t freq = stk::GetSysTimerFrequency();
    uint64_t count = stk::GetSysTimerCount();

    CHECK_EQUAL(1000, freq);
    CHECK_EQUAL(99, (int)count);
}

} // namespace stk
} // namespace test
