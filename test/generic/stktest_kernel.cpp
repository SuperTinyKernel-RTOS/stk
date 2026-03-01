/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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

TEST_GROUP(Kernel)
{
    void setup() {}
    void teardown() {}
};

TEST(Kernel, MaxTasks)
{
    const int32_t TASKS = 2;
    Kernel<KERNEL_STATIC, TASKS, SwitchStrategyRR, PlatformTestMock> kernel;
    const int32_t result = Kernel<KERNEL_STATIC, TASKS, SwitchStrategyRR, PlatformTestMock>::TASKS_MAX;

    CHECK_EQUAL(TASKS, result);
}

TEST(Kernel, Init)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    CHECK_TRUE(platform != NULL);

    kernel.Initialize();

    CHECK_TRUE(((IKernel *)&kernel)->IsInitialized());

    CHECK_TRUE(IKernelService::GetInstance() != NULL);
    CHECK_TRUE(IKernelService::GetInstance() == platform->m_service);
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
    const ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    CHECK_EQUAL_TEXT(0, strategy->GetSize(), "Expecting none kernel tasks");

    kernel.AddTask(&task);

    CHECK_EQUAL_TEXT(1, strategy->GetSize(), "Expecting 1 kernel task");

    IKernelTask *ktask = strategy->GetFirst();
    CHECK_TRUE_TEXT(ktask != NULL, "Expecting one kernel task");

    CHECK_TRUE_TEXT(ktask->GetUserTask() == &task, "Expecting just added user task");
}

TEST(Kernel, AddTaskInitStack)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task);

    CHECK_EQUAL(&task, platform->m_stack_info[STACK_USER_TASK].task);
    CHECK_EQUAL((size_t)task.GetStack(), platform->m_stack_info[STACK_USER_TASK].stack->SP);
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
    const ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

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
    const ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

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
    CHECK_EQUAL((size_t)task.GetStack(), platform->m_stack_active->SP);
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
    CHECK_EQUAL(ACCESS_PRIVILEGED, platform->m_stack_active->mode);
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
        CHECK_EQUAL(idle->SP, (size_t)task1.GetStack());

        // 2-nd task becomes Active
        CHECK_EQUAL(active->SP, (size_t)task2.GetStack());

        // context switch requested
        CHECK_EQUAL(1, platform->m_context_switch_nr);
    }

    // ISR calls OnSysTick 2-nd time
    {
        platform->ProcessTick();

        CHECK_TRUE(idle != NULL);
        CHECK_TRUE(active != NULL);

        // 2-st task is switched from Active and becomes Idle
        CHECK_EQUAL(idle->SP, (size_t)task2.GetStack());

        // 1-nd task becomes Active
        CHECK_EQUAL(active->SP, (size_t)task1.GetStack());

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
    CHECK_EQUAL(ACCESS_USER, platform->m_stack_active->mode);

    // ISR calls OnSysTick
    platform->ProcessTick();

    // 2-st task
    CHECK_EQUAL(ACCESS_PRIVILEGED, platform->m_stack_active->mode);
}

TEST(Kernel, AbiCompatibility)
{
    class TaskMockAbiCheck : public Task<32, ACCESS_USER>
    {
    public:
        TaskMockAbiCheck() : m_result(0) {}
        RunFuncType GetFunc() { return forced_cast<stk::RunFuncType>(&TaskMockAbiCheck::Run); }
        void *GetFuncUserData() { return this; }
        uint32_t m_result;
    private:
        void Run() { ++m_result; }
    };

    TaskMockAbiCheck mock;

    mock.GetFunc()(mock.GetFuncUserData());
    mock.GetFunc()(mock.GetFuncUserData());

    CHECK_EQUAL(2, mock.m_result);
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

    try
    {
        g_TestContext.ExpectAssert(true);
        platform->EventTaskExit(active);
        CHECK_TEXT(false, "task exiting unsupported in KERNEL_STATIC mode");
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
        CHECK_TEXT(false, "non existent task must not succeed");
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
        CHECK_TEXT(false, "exited task must be successfully skipped by FindTaskBySP()");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
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
    CHECK_EQUAL(platform->m_stack_active->SP, (size_t)task2.GetStack());

    platform->ProcessTick();
    CHECK_EQUAL(platform->m_stack_active->SP, (size_t)task1.GetStack());
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
        CHECK_TEXT(false, "non-HRT AddTask not supported in HRT mode");
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
        CHECK_TEXT(false, "HRT-related AddTask not supported in non-HRT mode");
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
        CHECK_TEXT(false, "IKernelService::Sleep not allowed in HRT mode");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, HrtTaskCompleted)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    const ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task, 1, 1, 0);
    kernel.Start();

    CHECK_TRUE(strategy->GetSize() != 0);

    platform->EventTaskExit(platform->m_stack_active);
    platform->ProcessTick();

    platform->ProcessTick();

    CHECK_EQUAL(0, strategy->GetSize());
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

    CHECK_EQUAL(platform->m_stack_active->SP, (size_t)task2.GetStack());
    platform->ProcessTick();
    CHECK_EQUAL(platform->m_stack_active->SP, (size_t)task2.GetStack());
    platform->ProcessTick();
    Yield();
    CHECK_EQUAL(platform->m_stack_active->SP, (size_t)task1.GetStack());

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
    CHECK_EQUAL((size_t)task1.GetStack(), platform->m_stack_active->SP);

    // task returns (exiting) without calling SwitchToNext
    platform->EventTaskExit(platform->m_stack_active);

    platform->ProcessTick(); // schedules task removal but task2 is still sleeping

    // here scheduler is sleeping because task1 was sent to infinite sleep until removal and task2 is still pending

    platform->ProcessTick(); // task2 is still sleeping
    platform->ProcessTick(); // switched to task2

    CHECK_EQUAL((size_t)task2.GetStack(), platform->m_stack_active->SP);

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
    CHECK_EQUAL(platform->m_stack_active->SP, (size_t)task.GetStack());
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
        CHECK_TEXT(false, "kernel does not support waiting without KERNEL_SYNC");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // test return NULL
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    IWaitObject *wo = IKernelService::GetInstance()->Wait(nullptr, nullptr, 0);
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
    CHECK_TRUE_TEXT(wo == nullptr, "expect NULL");

    try
    {
        g_TestContext.ExpectAssert(true);
        platform->EventTaskWait(0, nullptr, nullptr, 0);
        CHECK_TEXT(false, "kernel does not support waiting without KERNEL_SYNC");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // test return NULL
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    wo = platform->EventTaskWait(0, nullptr, nullptr, 0);
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
    CHECK_TRUE_TEXT(wo == nullptr, "expect NULL");
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

static struct SyncWaitRelaxCpuContext
{
    SyncWaitRelaxCpuContext()
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
g_SyncWaitRelaxCpuContext;

static void SyncWaitRelaxCpu()
{
    g_SyncWaitRelaxCpuContext.Process();
}

TEST(Kernel, SyncWait)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SyncWaitRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = SyncWaitRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    IWaitObject *wo = IKernelService::GetInstance()->Wait(&sobj, &mutex, 2);

    CHECK_EQUAL(GetTid(), wo->GetTid()); // expect the same thread id as WaitObject belongs to the caller's task
    CHECK_TRUE(wo != nullptr); // expect wait object in return after timeout
    CHECK_TRUE(wo->IsTimeout()); // expect timeout
    CHECK_EQUAL(2, g_SyncWaitRelaxCpuContext.counter); // expect 2 ticks after timeout
    CHECK_EQUAL(true, mutex.m_locked); // expect locked mutex after Wait return
}

} // namespace stk
} // namespace test
