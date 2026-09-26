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
// ============================== Kernel ====================================== //
// ============================================================================ //

template <uint8_t TMode, uint32_t TSize, class TStrategy, class TPlatform>
class KernelMock : public Kernel<TMode, TSize, TStrategy, TPlatform>
{
    typedef Kernel<TMode, TSize, TStrategy, TPlatform> BaseType;

public:
    uint8_t m_fsm_state_mock = KernelMock::FSM_STATE_NONE;

    // Override the getter to bypass the ROM table during tests
    typename BaseType::EFsmState GetNewFsmState(typename BaseType::KernelTask *&next) override
    {
        if (m_fsm_state_mock != (uint8_t)KernelMock::FSM_STATE_NONE)
            return static_cast<typename BaseType::EFsmState>(m_fsm_state_mock);

        return BaseType::GetNewFsmState(next);
    }

    void ForceUpdateInvalidFsmState(bool max_val)
    {
        m_fsm_state_mock = KernelMock::FSM_STATE_MAX + (max_val ? 0 : 1);

        Stack *idle = nullptr, *active = nullptr;
        KernelMock::UpdateFsmState(idle, active);
    }

    // Expose protected Kernel::OnForceContextSwitch() so it can be driven directly,
    // simulating the ISR callback issued by the platform driver's ForceContextSwitch()
    bool CallForceContextSwitch(TId id, Stack *&idle, Stack *&active)
    {
        return KernelMock::OnForceContextSwitch(id, idle, active);
    }

    // Put the currently running task into a pending-sleep state directly (bypasses
    // KernelTask::BusyWaitWhileSleeping()'s spin-loop, which is not applicable in this
    // single-threaded test harness)
    void ScheduleSleepOnActiveTask(Timeout ticks)
    {
        BaseType::ScheduleSleepOnActiveTask(ticks);
    }
};

TEST_GROUP(Kernel)
{
    void setup() {}
    void teardown()
    {
        g_TestContext.RethrowAssertException(true);
        g_TestContext.ExpectAssert(false);
        g_TestContext.ExpectPanic(false);
        test::g_PanicValue = KERNEL_PANIC_NONE;
    }
};

TEST(Kernel, MaxTasks)
{
    const int32_t TASKS = 2;
    Kernel<KERNEL_STATIC, TASKS, SwitchStrategyRR, PlatformTestMock> kernel;
    const int32_t result = Kernel<KERNEL_STATIC, TASKS, SwitchStrategyRR, PlatformTestMock>::TASKS_MAX;

    CHECK_EQUAL(TASKS, result);
}

TEST(Kernel, State)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    CHECK_TRUE(platform != NULL);

    CHECK_TRUE(kernel.GetState() == IKernel::KSTATE_INACTIVE);

    kernel.Initialize();

    CHECK_TRUE(kernel.GetState() == IKernel::KSTATE_READY);

    CHECK_TRUE(IKernelService::GetInstance() != NULL);
    CHECK_TRUE(IKernelService::GetInstance() == platform->m_service);

    kernel.AddTask(&task);
    kernel.Start();

    CHECK_TRUE(kernel.GetState() == IKernel::KSTATE_RUNNING);
}

TEST(Kernel, InitDoubleFail)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.Initialize();
        kernel.Initialize();
        CHECK_TEXT(false, "duplicate Kernel::Initialize() did not fail");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, AddTaskNoInit)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task);
        CHECK_TEXT(false, "AddTask() did not fail");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, AddTask)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();

    CHECK_EQUAL(0, strategy->GetSize()); // Expecting none kernel tasks

    kernel.AddTask(&task);

    CHECK_EQUAL(1, strategy->GetSize()); // Expecting 1 kernel task

    IKernelTask *ktask = strategy->GetFirst();
    CHECK_TRUE(ktask != NULL); // Expecting added kernel task

    CHECK_EQUAL(&task, ktask->GetUserTask()); // Expecting just added user task

    // stack info must belong to this task
    CHECK_EQUAL(platform->m_stack_info[STACK_USER_TASK].stack->SP, ktask->GetUserStack().SP);
    CHECK_EQUAL(ACCESS_USER, ktask->GetUserStack().access_mode);
}

TEST(Kernel, AddTaskInitStack)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task);

    CHECK_EQUAL(&task, platform->m_stack_info[STACK_USER_TASK].task);
    CHECK_EQUAL((Word)task.GetStack(), platform->m_stack_info[STACK_USER_TASK].stack->SP);
}

TEST(Kernel, AddTaskFailMaxOut)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task1);
        kernel.AddTask(&task2);
        kernel.AddTask(&task3);
        CHECK_TEXT(false, "expecting to fail adding task because max is 2 but adding 3-rd");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, AddTaskFailSameTask)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task);
        kernel.AddTask(&task);
        CHECK_TEXT(false, "expecting to fail adding the same task");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

static struct AddTaskWhenStartedRelaxCpuContext
{
    AddTaskWhenStartedRelaxCpuContext()
    {
        counter  = 0;
        platform = NULL;
        strategy = NULL;
    }

    uint32_t                   counter;
    PlatformTestMock          *platform;
    const ITaskSwitchStrategy *strategy;

    void Process()
    {
        platform->ProcessTick();

        if (counter >= 1)
        {
            CHECK_EQUAL_TEXT(2, strategy->GetSize(), "task2 must be added within 1 tick");
        }

        ++counter;
    }
}
g_AddTaskWhenStartedRelaxCpuContext;

static void AddTaskWhenStartedRelaxCpu()
{
    g_AddTaskWhenStartedRelaxCpuContext.Process();
}

TEST(Kernel, AddTaskWhenStarted)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    CHECK_EQUAL_TEXT(1, strategy->GetSize(), "expecting task1 be added at this stage");

    g_AddTaskWhenStartedRelaxCpuContext.platform = (PlatformTestMock *)kernel.GetPlatform();
    g_AddTaskWhenStartedRelaxCpuContext.strategy = strategy;
    g_RelaxCpuHandler = AddTaskWhenStartedRelaxCpu;

    kernel.AddTask(&task2);

    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "task2 must be added");

    // AddTask is calling Yield/SwitchToNext which takes 2 ticks
    CHECK_EQUAL_TEXT(2, g_AddTaskWhenStartedRelaxCpuContext.counter, "should complete within 2 ticks");
}

TEST(Kernel, AddTaskFailStaticStarted)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task2);
        CHECK_TEXT(false, "expecting to AddTask to fail when non KERNEL_DYNAMIC");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, AddTaskFailHrtStarted)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1, 1, 1, 0);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task2, 1, 1, 0);
        CHECK_TEXT(false, "expecting to AddTask to fail when KERNEL_HRT");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, RemoveTask)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    kernel.RemoveTask(&task1);
    CHECK_EQUAL_TEXT(&task2, strategy->GetFirst()->GetUserTask(), "Expecting task2 as first");

    kernel.RemoveTask(&task1);
    CHECK_EQUAL_TEXT(&task2, strategy->GetFirst()->GetUserTask(), "Expecting task2 as first (duplicate task1 removal attempt)");

    kernel.RemoveTask(&task2);
    CHECK_EQUAL_TEXT(0, strategy->GetSize(), "Expecting none tasks");
}

TEST(Kernel, RemoveTaskFailNull)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.RemoveTask((ITask *)NULL);
        CHECK_TEXT(false, "expecting to fail with NULL argument");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, RemoveTaskFailUnsupported)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.RemoveTask(&task);
        CHECK_TEXT(false, "expecting to fail in KERNEL_STATIC mode");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, RemoveTaskFailStarted)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.RemoveTask(&task);
        CHECK_TEXT(false, "expecting to fail when Kernel has started");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, StartInvalidPeriodicity)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.Initialize(0);
        CHECK_TEXT(false, "expecting to fail with 0 periodicity");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.Initialize(PERIODICITY_MAX + 1);
        CHECK_TEXT(false, "expecting to fail with too large periodicity");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, StartNotIntialized)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.Start();
        CHECK_TEXT(false, "expecting to fail when not initialized");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, StartNoTasks)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.Start();
        CHECK_TEXT(false, "expecting to fail without tasks");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, Start)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    const uint32_t periodicity = PERIODICITY_MAX - 1;

    kernel.Initialize(periodicity);
    kernel.AddTask(&task);

    kernel.Start();

    CHECK_TRUE(platform->m_started);
    CHECK_TRUE(g_KernelService != NULL);
    CHECK_TRUE(platform->m_stack_active != NULL);
    CHECK_EQUAL((Word)task.GetStack(), platform->m_stack_active->SP);
    CHECK_EQUAL(periodicity, platform->GetTickResolution());
}

TEST(Kernel, StartBeginISR)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    // expect that first task's access mode is requested by kernel
    CHECK_EQUAL(ACCESS_PRIVILEGED, platform->m_stack_active->access_mode);
}

TEST(Kernel, ContextSwitchOnSysTickISR)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&idle = platform->m_stack_idle, *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // ISR calls OnSysTick 1-st time
    {
        platform->ProcessTick();

        CHECK_TRUE(idle != NULL);
        CHECK_TRUE(active != NULL);

        // 1-st task is switched from Active and becomes Idle
        CHECK_EQUAL(idle->SP, (Word)task1.GetStack());

        // 2-nd task becomes Active
        CHECK_EQUAL(active->SP, (Word)task2.GetStack());

        // context switch requested
        CHECK_EQUAL(1, platform->m_context_switch_nr);
    }

    // ISR calls OnSysTick 2-nd time
    {
        platform->ProcessTick();

        CHECK_TRUE(idle != NULL);
        CHECK_TRUE(active != NULL);

        // 2-st task is switched from Active and becomes Idle
        CHECK_EQUAL(idle->SP, (Word)task2.GetStack());

        // 1-nd task becomes Active
        CHECK_EQUAL(active->SP, (Word)task1.GetStack());

        // context switch requested
        CHECK_EQUAL(2, platform->m_context_switch_nr);
    }
}

TEST(Kernel, ContextSwitchAccessModeChange)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    TaskMock<ACCESS_PRIVILEGED> task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // 1-st task
    CHECK_EQUAL(ACCESS_USER, platform->m_stack_active->access_mode);

    // ISR calls OnSysTick
    platform->ProcessTick();

    // 2-st task
    CHECK_EQUAL(ACCESS_PRIVILEGED, platform->m_stack_active->access_mode);
}

TEST(Kernel, ContextSwitchCorruptedFsmMode)
{
    KernelMock<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    // ISR calls OnSysTick
    platform->ProcessTick();

    g_TestContext.ExpectPanic(true);

    kernel.ForceUpdateInvalidFsmState(true);
    platform->ProcessTick();
    CHECK_EQUAL(KERNEL_PANIC_BAD_STATE, test::g_PanicValue);

    test::g_PanicValue = KERNEL_PANIC_NONE;

    kernel.ForceUpdateInvalidFsmState(false);
    platform->ProcessTick();
    CHECK_EQUAL(KERNEL_PANIC_BAD_STATE, test::g_PanicValue);
}

TEST(Kernel, ForceContextSwitch)
{
    KernelMock<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *idle = nullptr, *active = nullptr;
    const TId task1_id = GetTidFromUserTask(&task1);

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is the running task after Start()
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);

    // task1 called Sleep()/SleepUntil(), which set it into a pending-sleep state and
    // is now busy-waiting inside BusyWaitWhileSleeping() for the ISR to de-schedule it
    kernel.ScheduleSleepOnActiveTask(5);

    // ISR handles the platform driver's ForceContextSwitch(task1_id) request
    const bool result = kernel.CallForceContextSwitch(task1_id, idle, active);

    CHECK_TRUE(result);
    CHECK_TRUE(idle != NULL);
    CHECK_TRUE(active != NULL);

    // task1 (now sleeping) is switched out to Idle
    CHECK_EQUAL((Word)task1.GetStack(), idle->SP);

    // task2 becomes Active
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    // task1 is no longer the running task (task2 is, after the switch above), so a
    // further ISR call reporting task1's id no longer matches and is a no-op
    idle = nullptr;
    active = nullptr;

    CHECK_FALSE(kernel.CallForceContextSwitch(task1_id, idle, active));
    CHECK_TRUE(idle == NULL);
    CHECK_TRUE(active == NULL);
}

TEST(Kernel, ForceContextSwitchTidMismatch)
{
    KernelMock<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    Stack *idle = nullptr, *active = nullptr;
    const TId task2_id = GetTidFromUserTask(&task2);

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is running and pending-sleep, but the ISR reports a force context switch
    // for task2's id, which is not the currently running task
    kernel.ScheduleSleepOnActiveTask(5);

    CHECK_FALSE(kernel.CallForceContextSwitch(task2_id, idle, active));
    CHECK_TRUE(idle == NULL);
    CHECK_TRUE(active == NULL);
}

TEST(Kernel, ForceContextSwitchNotSleeping)
{
    KernelMock<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    Stack *idle = nullptr, *active = nullptr;
    const TId task1_id = GetTidFromUserTask(&task1);

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is running but never called Sleep()/SleepUntil(): there is nothing pending
    // for the ISR to de-schedule
    CHECK_FALSE(kernel.CallForceContextSwitch(task1_id, idle, active));
    CHECK_TRUE(idle == NULL);
    CHECK_TRUE(active == NULL);
}

TEST(Kernel, SingleTask)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&idle = platform->m_stack_idle, *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    // ISR calls OnSysTick
    platform->ProcessTick();

    // expect that with single task nothing changes
    CHECK_EQUAL((Stack *)NULL, idle);
    CHECK_EQUAL((Stack *)platform->m_stack_info[STACK_USER_TASK].stack, active);
}

template <class _SwitchStrategy>
static void TestTaskExit()
{
    Kernel<KERNEL_DYNAMIC, 2, _SwitchStrategy, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&idle = platform->m_stack_idle, *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // ISR calls OnSysTick (task1 = idle, task2 = active)
    platform->ProcessTick();

    // task2 exited (will schedule its removal)
    platform->EventTaskExit(active);

    // ISR calls OnSysTick (task2 = idle, task1 = active)
    platform->ProcessTick();

    // task1 exited (will schedule its removal)
    platform->EventTaskExit(active);

    // ISR calls OnSysTick
    platform->ProcessTick();

    // last task is removed
    platform->ProcessTick();
    platform->ProcessTick();

    // no Idle tasks left
    CHECK_EQUAL((Stack *)NULL, idle);

    // Exit trap stack is provided for a long jump to the end of Kernel::Start()
    CHECK_EQUAL(platform->m_exit_trap, active);
}

TEST(Kernel, OnTaskExitRR)
{
    TestTaskExit<SwitchStrategyRR>();
}

TEST(Kernel, OnTaskExitSWRR)
{
    TestTaskExit<SwitchStrategySWRR>();
}

TEST(Kernel, OnTaskExitFP31)
{
    TestTaskExit<SwitchStrategyFP32>();
}

TEST(Kernel, OnTaskExitUnknownOrNull)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack unk_stack;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    // ISR calls OnSysTick (task1 = idle, task2 = active)
    platform->ProcessTick();

    try
    {
        g_TestContext.ExpectAssert(true);
        platform->EventTaskExit(&unk_stack);
        CHECK_TEXT(false, "expecting to fail on unknown stack");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        platform->EventTaskExit(NULL);
        CHECK_TEXT(false, "expecting to fail on NULL");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, OnTaskExitUnsupported)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    // ISR calls OnSysTick
    platform->ProcessTick();

    g_TestContext.ExpectPanic(true);
    platform->EventTaskExit(active);
    CHECK_EQUAL(KERNEL_PANIC_BAD_MODE, test::g_PanicValue);
}

TEST(Kernel, OnTaskExitScheduleDynamicOnly)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.ScheduleTaskRemoval(&task1);
        CHECK(false); // dynamic only
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, OnTaskExitSchedule)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is active after Start
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    kernel.ScheduleTaskRemoval(&task1);

    // ScheduleTaskRemoval does not change anything, just schedules removal on the next tick
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // task2 will become active now but kernel could not remove task1 because it was current one,
    // it will be removed on a next tick
    platform->ProcessTick();

    // task2 became the active
    CHECK_EQUAL(2, kernel.GetSwitchStrategy()->GetSize());
    CHECK_EQUAL(active->SP, (Word)task2.GetStack());

    platform->ProcessTick();

    // task1 was removed by the tick, task2 is the only active
    CHECK_EQUAL(1, kernel.GetSwitchStrategy()->GetSize());
    CHECK_EQUAL(active->SP, (Word)task2.GetStack());
}

static struct SyncWaitingTaskExitScheduleContext
{
    SyncWaitingTaskExitScheduleContext()
    {
        Reset();
    }

    void Reset()
    {
        kernel   = NULL;
        task1    = NULL;
        platform = NULL;
    }

    IKernel          *kernel;
    ITask            *task1;
    PlatformTestMock *platform;

    void Process()
    {
        platform->ProcessTick();

        // Wait object affects sleep_ticks, not a task
        {
            kernel->ScheduleTaskRemoval(task1);
        }
    }
}
g_SyncWaitingTaskExitScheduleContext;

static void SyncWaitingTaskExitScheduleRelaxCpu()
{
    g_SyncWaitingTaskExitScheduleContext.Process();
}

TEST(Kernel, OnWaitingTaskExitSchedule)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncWaitingTaskExitScheduleContext.Reset();
    g_SyncWaitingTaskExitScheduleContext.kernel   = &kernel;
    g_SyncWaitingTaskExitScheduleContext.platform = platform;
    g_SyncWaitingTaskExitScheduleContext.task1    = &task1;
    g_RelaxCpuHandler = SyncWaitingTaskExitScheduleRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is active after Start
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    {
        MutexMock::ScopedLock guard(mutex);

        EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 2);

        CHECK_EQUAL(WAIT_RESULT_CANCELED, wresult);
    }

    platform->ProcessTick();

    // task1 was removed by the tick, task2 is the only active
    CHECK_EQUAL(1, kernel.GetSwitchStrategy()->GetSize());
    CHECK_EQUAL(active->SP, (Word)task2.GetStack());
}

static struct SyncCancelWaitContext
{
    SyncCancelWaitContext()
    {
        Reset();
    }

    void Reset()
    {
        kernel   = NULL;
        task1    = NULL;
        platform = NULL;
    }

    IKernel          *kernel;
    ITask            *task1;
    PlatformTestMock *platform;

    void Process()
    {
        platform->ProcessTick();

        // Wait object affects sleep_ticks, not a task
        {
            kernel->CancelTaskWait(task1);
        }
    }
}
g_SyncCancelWaitContext;

static void SyncCancelWaitContextRelaxCpu()
{
    g_SyncCancelWaitContext.Process();
}

TEST(Kernel, OnCancelWait)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncCancelWaitContext.Reset();
    g_SyncCancelWaitContext.kernel   = &kernel;
    g_SyncCancelWaitContext.platform = platform;
    g_SyncCancelWaitContext.task1    = &task1;
    g_RelaxCpuHandler = SyncCancelWaitContextRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is active after Start
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    {
        MutexMock::ScopedLock guard(mutex);

        EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 2);

        CHECK_EQUAL(WAIT_RESULT_CANCELED, wresult);
    }
}

TEST(Kernel, OnCancelWaitNonSync)
{
    Kernel<KERNEL_DYNAMIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    // task1 is active after Start
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.CancelTaskWait(&task1);
        CHECK(false);
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, OnTaskNotFoundBySP)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    platform->ProcessTick();

    try
    {
        g_TestContext.ExpectAssert(true);
        platform->EventTaskSwitch(0xdeadbeef);
        CHECK(false); // non existent task must not succeed
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, OnTaskSkipFreedTask)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 exited (will schedule its removal)
    platform->EventTaskExit(active);

    // 2 ticks to remove exited task1 from scheduling (1st switched to task2, 2nd cleans up task1 exit)
    platform->ProcessTick();
    platform->ProcessTick();

    try
    {
        g_TestContext.ExpectAssert(true);

        // we loop through all tasks in attempt to find non existent SP (0xdeadbeef)
        // by this FindTaskBySP() is invoked and will loop thorugh the exited task1's
        // slot
        platform->EventTaskSwitch(0xdeadbeef);
        CHECK(false); // exited task must be successfully skipped by FindTaskBySP()
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

static struct TaskSuspendContext
{
    TaskSuspendContext()
    {
        Clear();
    }

    IPlatform *platform;
    IKernel   *kernel;
    ITask     *task1;
    ITask     *task2;
    uint32_t   count;

    void Clear()
    {
        platform = nullptr;
        kernel   = nullptr;
        task1    = nullptr;
        task2    = nullptr;
        count    = 0;
    }

    void Process()
    {
        platform->ProcessTick();

        if (count > 2)
        {
            // on first attempt we resume self, then calling Resume for a non-suspended task is noop
            kernel->ResumeTask(task1);
        }

        ++count;
    }
}
g_TaskSuspendContext;

static void TaskSuspendRelaxCpu()
{
    g_TaskSuspendContext.Process();
}

TEST(Kernel, TaskSuspend)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&idle = platform->m_stack_idle, *&active = platform->m_stack_active;
    bool suspended = false;

    g_TaskSuspendContext.Clear();
    g_TaskSuspendContext.platform = platform;
    g_TaskSuspendContext.kernel   = &kernel;
    g_TaskSuspendContext.task1    = &task1;
    g_TaskSuspendContext.task2    = &task2;
    g_RelaxCpuHandler = TaskSuspendRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is active after Start
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // task1 is calling SuspendTask to suspend self
    kernel.SuspendTask(&task1, suspended);
    CHECK_TRUE(suspended);

    // task1 became active after we resumed it TaskSuspendContext::Process
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // task1 is calling SuspendTask for task2
    kernel.SuspendTask(&task2, suspended);
    CHECK_TRUE(suspended);

    // task2 is suspended
    platform->ProcessTick();
    CHECK_EQUAL(idle->SP, (Word)task2.GetStack());

    // task2 is suspended
    platform->ProcessTick();
    CHECK_EQUAL(idle->SP, (Word)task2.GetStack());

    // task1 is calling ResumeTask for task2
    kernel.ResumeTask(&task2);

    // task2 becomes active
    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (Word)task2.GetStack());
}

TEST(Kernel, Hrt)
{
    Kernel<KERNEL_STATIC | KERNEL_HRT, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1, 1, 1, 0);
    kernel.AddTask(&task2, 2, 1, 0);
    kernel.Start();

    platform->ProcessTick();
    CHECK_EQUAL(platform->m_stack_active->SP, (Word)task2.GetStack());

    platform->ProcessTick();
    CHECK_EQUAL(platform->m_stack_active->SP, (Word)task1.GetStack());
}

TEST(Kernel, HrtApiForNonHrtTask)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    IKernelTask *ktask = strategy->GetFirst();
    CHECK_TRUE(ktask != NULL); // Expecting added kernel task

    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);

    CHECK_EQUAL(0, ktask->GetHrtPeriodicity());
    CHECK_EQUAL(0, ktask->GetHrtDeadline());
    CHECK_EQUAL(0, ktask->GetHrtRelativeDeadline());

    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
}

TEST(Kernel, HrtAddNonHrt)
{
    Kernel<KERNEL_STATIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task);
        CHECK(false); // non-HRT AddTask not supported in HRT mode"
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, HrtAddNotAllowedForNonHrtMode)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task, 1, 1, 0);
        CHECK(false); // HRT-related AddTask not supported in non-HRT mode
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, HrtSleepNotAllowed)
{
    Kernel<KERNEL_STATIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task, 1, 1, 0);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        Sleep(1);
        CHECK(false); // IKernelService::Sleep not allowed in HRT mode
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        SleepUntil(GetTicks() + 1);
        CHECK(false); // IKernelService::SleepUntil not allowed in HRT mode
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    CHECK_EQUAL(false, SleepUntil(GetTicks() + 1));
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
}

TEST(Kernel, HrtTaskCompleted)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task, 1, 1, 0);
    kernel.Start();

    CHECK_TRUE(strategy->GetSize() != 0);

    platform->EventTaskExit(platform->m_stack_active);
    platform->ProcessTick();

    platform->ProcessTick();

    CHECK_EQUAL(0, strategy->GetSize());

    CHECK_EQUAL(IKernel::KSTATE_READY, kernel.GetState());
}

static struct HrtTaskDeadlineMissedRelaxCpuContext
{
    HrtTaskDeadlineMissedRelaxCpuContext()
    {
        counter  = 0;
        platform = NULL;
    }

    uint32_t          counter;
    PlatformTestMock *platform;

    void Process()
    {
        platform->ProcessTick();
        ++counter;
    }
}
g_HrtTaskDeadlineMissedRelaxCpuContext;

static void HrtTaskDeadlineMissedRelaxCpu()
{
    g_HrtTaskDeadlineMissedRelaxCpuContext.Process();
}

TEST(Kernel, HrtTaskDeadlineMissedRR)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task, 2, 1, 0);
    kernel.Start();

    g_HrtTaskDeadlineMissedRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = HrtTaskDeadlineMissedRelaxCpu;

    platform->ProcessTick();

    // task does not Yield() and thus next tick will overcome the deadline
    g_TestContext.ExpectAssert(true);

    // 2-nd tick goes outside the deadline
    platform->ProcessTick();

    CHECK_TRUE(platform->m_hard_fault);
    CHECK_EQUAL(2, task.m_deadline_missed);
}

TEST(Kernel, HrtTaskDeadlineNotMissedRR)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task, 2, 1, 0);
    kernel.Start();

    g_HrtTaskDeadlineMissedRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = HrtTaskDeadlineMissedRelaxCpu;

    platform->ProcessTick();

    // task completes its work and yields to kernel, its workload is 1 ticks now that is within deadline 1
    Yield();

    // 2-nd tick continues scheduling normally
    platform->ProcessTick();

    CHECK_FALSE(platform->m_hard_fault);
    CHECK_EQUAL(0, task.m_deadline_missed);
}

TEST(Kernel, HrtSkipSleepingNextRM)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 2, SwitchStrategyRM, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1, 2, 2, 3);
    kernel.AddTask(&task2, 3, 3, 0);
    kernel.Start();

    g_HrtTaskDeadlineMissedRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = HrtTaskDeadlineMissedRelaxCpu;

    CHECK_EQUAL(platform->m_stack_active->SP, (Word)task2.GetStack());
    platform->ProcessTick();
    CHECK_EQUAL(platform->m_stack_active->SP, (Word)task2.GetStack());
    platform->ProcessTick();
    Yield();
    CHECK_EQUAL(platform->m_stack_active->SP, (Word)task1.GetStack());

    CHECK_FALSE(platform->m_hard_fault);
    CHECK_EQUAL(0, task1.m_deadline_missed);
    CHECK_EQUAL(0, task2.m_deadline_missed);
}

template <class _SwitchStrategy>
static void TestHrtTaskExitDuringSleepState()
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 2, _SwitchStrategy, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    const _SwitchStrategy *strategy = static_cast<const _SwitchStrategy *>(kernel.GetSwitchStrategy());

    kernel.Initialize();
    kernel.AddTask(&task1, 1, 2, 0);
    kernel.AddTask(&task2, 1, 2, 2);
    kernel.Start();

    // task1 is the first
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);

    // task returns (exiting) without calling SwitchToNext
    platform->EventTaskExit(platform->m_stack_active);

    platform->ProcessTick(); // schedules task removal but task2 is still sleeping

    // here scheduler is sleeping because task1 was sent to infinite sleep until removal and task2 is still pending

    platform->ProcessTick(); // task2 is still sleeping
    platform->ProcessTick(); // switched to task2

    CHECK_EQUAL((Word)task2.GetStack(), platform->m_stack_active->SP);

    CHECK_EQUAL(1, strategy->GetSize());
}

TEST(Kernel, HrtTaskExitDuringSleepStateRR)
{
    TestHrtTaskExitDuringSleepState<SwitchStrategyRR>();
}

TEST(Kernel, HrtTaskExitDuringSleepStateRM)
{
    TestHrtTaskExitDuringSleepState<SwitchStrategyRM>();
}

TEST(Kernel, HrtTaskExitDuringSleepStateDM)
{
    TestHrtTaskExitDuringSleepState<SwitchStrategyDM>();
}

TEST(Kernel, HrtTaskExitDuringSleepStateEDF)
{
    TestHrtTaskExitDuringSleepState<SwitchStrategyEDF>();
}

TEST(Kernel, HrtSleepingAwakeningStateChange)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task, 1, 1, 1);
    kernel.Start();

    // due to 1 tick delayed start of the task Kernel enters into a SLEEPING state
    CHECK_EQUAL(platform->m_stack_active, platform->m_stack_info[STACK_SLEEP_TRAP].stack);

    platform->ProcessTick();

    // after a tick task become active and Kernel enters into a AWAKENING state
    CHECK_EQUAL(platform->m_stack_idle, platform->m_stack_info[STACK_SLEEP_TRAP].stack);
    CHECK_EQUAL(platform->m_stack_active->SP, (Word)task.GetStack());
}

TEST(Kernel, HrtOnlyAPI)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    // Obtain kernel task
    IKernelTask *ktask = kernel.GetSwitchStrategy()->GetFirst();
    CHECK_TRUE_TEXT(ktask != nullptr, "Kernel task must exist");

    try
    {
        g_TestContext.ExpectAssert(true);
        ktask->GetHrtRelativeDeadline();
        CHECK_TEXT(false, "HRT API can't be called in non-HRT mode");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        ktask->GetHrtPeriodicity();
        CHECK_TEXT(false, "HRT API can't be called in non-HRT mode");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        ktask->GetHrtDeadline();
        CHECK_TEXT(false, "HRT API can't be called in non-HRT mode");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SyncNotEnabledFailsOnWait)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->Wait(nullptr, nullptr, 0);
        CHECK_TRUE(false); // kernel does not support waiting without KERNEL_SYNC
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // test return NULL
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    EWaitResult wresult = IKernelService::GetInstance()->Wait(nullptr, nullptr, 0);
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
    CHECK_EQUAL(WAIT_RESULT_FAIL, wresult);

    try
    {
        g_TestContext.ExpectAssert(true);
        platform->EventTaskWait(0, nullptr, nullptr, 0);
        CHECK_TRUE(false); // kernel does not support waiting without KERNEL_SYNC;
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // test return NULL
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    wresult = platform->EventTaskWait(0, nullptr, nullptr, 0);
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
    CHECK_EQUAL(WAIT_RESULT_FAIL, wresult);
}

TEST(Kernel, SyncNotEnabledFailsOnWake)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->Wake(nullptr, false);
        CHECK_TRUE(false); // kernel does not support waiting without KERNEL_SYNC
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SyncNoNullSyncObj)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->Wait(nullptr, &mutex, 10);
        CHECK_TEXT(false, "sync object must not be NULL");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SyncNoNullMutex)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    SyncObjectMock sobj;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->Wait(&sobj, nullptr, 10);
        CHECK_TEXT(false, "mutex must not be NULL");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SyncNoZeroWait)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->Wait(&sobj, &mutex, 0);
        CHECK_TEXT(false, "must not be zero wait");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SyncMutexMustBeLocked)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->Wait(&sobj, &mutex, 10);
        CHECK_TEXT(false, "mutex must be locked");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SyncTaskExitAfterWait)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    {
        //MutexMock::ScopedLock guard(mutex);

        //IKernelService::GetInstance()->Wait(&sobj, &mutex, 10);
    }

    // task1 exited (will schedule its removal)
    platform->EventTaskExit(platform->m_stack_active);

    platform->ProcessTick();

    // should be still running here, next tick will result in task exit and kernel stop
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());

    platform->ProcessTick();

    // should be stopped here
    CHECK_EQUAL(IKernel::KSTATE_READY, kernel.GetState());
}

static struct SyncWaitRelaxCpuContext
{
    SyncWaitRelaxCpuContext()
    {
        Reset();
    }

    void Reset()
    {
        counter        = 0;
        platform       = NULL;
        check_tickless = ~0;
    }

    uint32_t          counter;
    PlatformTestMock *platform;
    uint32_t          check_tickless;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        // Wait object affects sleep_ticks, not a task
        if (counter == check_tickless)
        {
            CHECK_EQUAL(2, platform->m_ticks_count);
        }
    }
}
g_SyncWaitRelaxCpuContext;

static void SyncWaitRelaxCpu()
{
    g_SyncWaitRelaxCpuContext.Process();
}

template <bool TTickless>
void Test_SyncWait()
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC | (TTickless ? KERNEL_TICKLESS : 0), 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncWaitRelaxCpuContext.Reset();
    g_SyncWaitRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = SyncWaitRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 2);

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, wresult); // expect timeout
    CHECK_EQUAL(2, g_SyncWaitRelaxCpuContext.counter); // expect 2 ticks after timeout
    CHECK_EQUAL(true, mutex.m_locked); // expect locked mutex after Wait return
}

TEST(Kernel, SyncWait)
{
    Test_SyncWait<false>();
}

#if STK_TICKLESS_IDLE
TEST(Kernel, SyncWaitTickless)
{
    Test_SyncWait<true>();
}
#endif

#if STK_TICKLESS_IDLE
TEST(Kernel, SyncWaitTicklessDuration)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC | KERNEL_TICKLESS, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncWaitRelaxCpuContext.Reset();
    g_SyncWaitRelaxCpuContext.platform       = platform;
    g_SyncWaitRelaxCpuContext.check_tickless = 0; // check first tick
    g_RelaxCpuHandler = SyncWaitRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    // sleep_ticks should be equal to 3 on first OnTick call
    EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 3);

    // in total 3 ticks must be elapsed, including sleep ticks
    CHECK_EQUAL(3, platform->m_ticks_count);

    // at this stage test should pass successfully by validating sleep_ticks in SyncWaitRelaxCpuContext::Process

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, wresult); // expect timeout
    CHECK_EQUAL(2, g_SyncWaitRelaxCpuContext.counter);
    CHECK_EQUAL(true, mutex.m_locked);
}
#endif

static struct SyncWaitWakeInOneTickRelaxCpuContext
{
    SyncWaitWakeInOneTickRelaxCpuContext()
    {
        Reset();
    }

    void Reset()
    {
        counter        = 0;
        platform       = NULL;
        check_tickless = ~0;
        sobj           = NULL;
    }

    uint32_t          counter;
    PlatformTestMock *platform;
    uint32_t          check_tickless;
    SyncObjectMock   *sobj;

    void Process()
    {
        IKernelService::GetInstance()->Wake(sobj, true);

        platform->ProcessTick();
        ++counter;
    }
}
g_SyncWaitWakeInOneTickRelaxCpuContext;

static void SyncWaitWakeInOneTickRelaxCpu()
{
    g_SyncWaitWakeInOneTickRelaxCpuContext.Process();
}

TEST(Kernel, SyncWaitWakeInOneTick)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncWaitWakeInOneTickRelaxCpuContext.Reset();
    g_SyncWaitWakeInOneTickRelaxCpuContext.platform       = platform;
    g_SyncWaitWakeInOneTickRelaxCpuContext.check_tickless = 0; // check first tick
    g_SyncWaitWakeInOneTickRelaxCpuContext.sobj           = &sobj;
    g_RelaxCpuHandler = SyncWaitWakeInOneTickRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    // sleep_ticks should be equal to 3 on first OnTick call
    EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 3);

    // in total 1 ticks must be elapsed, including sleep ticks because Wake was
    // called in the same tick, so task did not really fall asleep
    CHECK_EQUAL(1, platform->m_ticks_count);
    CHECK_EQUAL(1, g_SyncWaitWakeInOneTickRelaxCpuContext.counter);

    // at this stage test should pass successfully by validating sleep_ticks in SyncWaitRelaxCpuContext::Process

    CHECK_EQUAL(WAIT_RESULT_SIGNAL, wresult); // expect Wake signal
    CHECK_EQUAL(true, mutex.m_locked);
}

TEST(Kernel, CheckWeightLessApi)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    IKernelTask *ktask = strategy->GetFirst();
    CHECK_TRUE(ktask != NULL); // Expecting added kernel task

    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);

    CHECK_EQUAL(DEFAULT_WEIGHT, ktask->GetWeight());
    CHECK_EQUAL(DEFAULT_WEIGHT, ktask->GetCurrentWeight());

    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
}

static struct SyncFindWeightHigherThanContext
{
    SyncFindWeightHigherThanContext()
    {
        Reset();
    }

    void Reset()
    {
        counter  = 0;
        platform = NULL;
        sobj     = NULL;
    }

    uint32_t          counter;
    PlatformTestMock *platform;
    SyncObjectMock   *sobj;

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (counter == 1)
        {
            CHECK_EQUAL(2, sobj->FindWeightHigherThan(0));
        }
    }
}
g_SyncFindWeightHigherThan;

static void SyncFindWeightHigherThanRelaxCpu()
{
    g_SyncFindWeightHigherThan.Process();
}

TEST(Kernel, SyncFindWeightHigherThan)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 2, SwitchStrategyFP32, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMockW<1, ACCESS_USER> task1;
    TaskMockW<2, ACCESS_USER> task2;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncFindWeightHigherThan.Reset();
    g_SyncFindWeightHigherThan.platform = platform;
    g_SyncFindWeightHigherThan.sobj     = &sobj;
    g_RelaxCpuHandler = SyncFindWeightHigherThanRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // no task is waiting
    CHECK_EQUAL(NO_WEIGHT, sobj.FindWeightHigherThan(0));

    MutexMock::ScopedLock guard(mutex);

    // sleep_ticks should be equal to 2 on first OnTick call
    EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 5);

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, wresult);
    CHECK_EQUAL(true, mutex.m_locked);

    // no task is waiting here
    CHECK_EQUAL(NO_WEIGHT, sobj.FindWeightHigherThan(0));
}

TEST(Kernel, SyncInheritWeight)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 2, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    TaskMockW<2, ACCESS_USER> task2;

    MutexMock mutex;
    SyncObjectMock sobj;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    IKernelTask *ktasks[2];
    ArrayView<IKernelTask *> ktasks_view(ktasks, 2);
    kernel.EnumerateKernelTasks(ktasks_view);

    // current weight is dynamic value used by strategy with PRIORITY_INHERITANCE_API, if not overridden
    // by InheritWeight should be NO_WEIGHT
    CHECK_EQUAL(NO_WEIGHT, ktasks[0]->GetCurrentWeight());

    // original weight
    CHECK_EQUAL(1, ktasks[0]->GetWeight());

    const TId task1_id = GetTidFromUserTask(&task1);

    IKernelService::GetInstance()->InheritWeight(task1_id, 2);

    // boosted weight
    CHECK_EQUAL(2, ktasks[0]->GetCurrentWeight());
    CHECK_EQUAL(2, ktasks[0]->GetWeight()); // using GetCurrentWeight

    IKernelService::GetInstance()->RestoreWeight(task1_id);

    // dynamic is back to NO_WEIGHT
    CHECK_EQUAL(NO_WEIGHT, ktasks[0]->GetCurrentWeight());

    // back to own weight
    CHECK_EQUAL(1, ktasks[0]->GetWeight());
}

TEST(Kernel, EnumTasks)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    ITask *tasks[2] = {};
    uint32_t i = 0;

    size_t count = kernel.EnumerateTasksT<2>([&](ITask *t) {
        CHECK_COMPARE(i, <, 2);
        tasks[i++] = t;
        return true; // continue
    });

    // must enumerate all
    CHECK_EQUAL(2, count);

    // check ordering
    CHECK_EQUAL(tasks[0], &task1);
    CHECK_EQUAL(tasks[1], &task2);

    // expect break of enumeration
    count = kernel.EnumerateTasksT<2>([&](ITask */*t*/) {
        return false; // break on first entry
    });
    CHECK_EQUAL(1, count);

    // expect only 1 iteration
    count = kernel.EnumerateTasksT<1>([&](ITask */*t*/) {
        return true;
    });
    CHECK_EQUAL(1, count);
}

TEST(Kernel, SuspendResumeTicklessOnly)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        stk::IKernelService::GetInstance()->Suspend();
        CHECK_TEXT(false, "tickless only");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // shall return 0 in release build
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    CHECK_EQUAL(0, stk::IKernelService::GetInstance()->Suspend());
    g_TestContext.ExpectAssert(false);
    g_TestContext.RethrowAssertException(true);

    try
    {
        g_TestContext.ExpectAssert(true);
        stk::IKernelService::GetInstance()->Resume(0);
        CHECK_TEXT(false, "tickless only");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SuspendResume)
{
    Kernel<KERNEL_STATIC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_PRIVILEGED> task1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    platform->m_sleep_ticks = 9;

    Timeout ticks = stk::IKernelService::GetInstance()->Suspend();
    CHECK_EQUAL(9, ticks);

    CHECK_EQUAL(IKernel::KSTATE_SUSPENDED, kernel.GetState());

    stk::IKernelService::GetInstance()->Resume(ticks);
}

} // namespace stk
} // namespace test
