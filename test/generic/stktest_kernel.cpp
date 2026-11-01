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

    // Expose protected Kernel::TryAllocateNewTask() so it can be driven directly (both AddTask() overloads use it,
    // it always validates the collision with the already added tasks).
    // Returns the result, 'bound' receives the user task bound to the allocated slot (nullptr if none).
    EAddResult CallTryAllocateNewTask(ITask *user_task, ITask **bound = nullptr)
    {
        EAddResult result = ADD_FAIL_INVALID;

        typename BaseType::KernelTask *const task = BaseType::TryAllocateNewTask(user_task, result);

        if (bound != nullptr)
        {
            *bound = (task != nullptr) ? task->GetUserTask() : nullptr;
        }

        return result;
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

// ---------------------------------------------------------------------------- //
// AddTask() after Start() (KERNEL_DYNAMIC): the task is added immediately under a //
// critical section, without a yield, a busy-wait or a tick. Failures are returned.  //
// ---------------------------------------------------------------------------- //

static uint32_t g_AddTaskWhenStartedRelaxCpuCount = 0;

static void AddTaskWhenStartedRelaxCpuCounter()
{
    ++g_AddTaskWhenStartedRelaxCpuCount;
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

    g_AddTaskWhenStartedRelaxCpuCount = 0;
    g_RelaxCpuHandler = AddTaskWhenStartedRelaxCpuCounter;

    kernel.AddTask(&task2);

    g_RelaxCpuHandler = NULL;

    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "task2 must be added immediately");
    CHECK_EQUAL_TEXT(0, g_AddTaskWhenStartedRelaxCpuCount, "AddTask must not yield or busy-wait");
}

typedef KernelMock<KERNEL_DYNAMIC, 2, SwitchStrategyRR_NSY, PlatformTestMock> KernelDynNsyMock;

TEST(Kernel, AddTaskWhenStartedNsy)
{
    KernelDynNsyMock kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    CHECK_EQUAL_TEXT(1, strategy->GetSize(), "expecting task1 be added at this stage");

    g_AddTaskWhenStartedRelaxCpuCount = 0;
    g_RelaxCpuHandler = AddTaskWhenStartedRelaxCpuCounter;

    kernel.AddTask(&task2);

    g_RelaxCpuHandler = NULL;

    // NOSLEEP_YIELD_API strategies must not busy-wait either
    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "task2 must be added immediately");
    CHECK_EQUAL_TEXT(0, g_AddTaskWhenStartedRelaxCpuCount, "AddTask must not busy-wait");
}

// KERNEL_DYNAMIC: failures are returned, never asserted (the caller handles them)
TEST(Kernel, AddTaskWhenStartedResultOk)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task1));
    kernel.Start();
    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task2));
}

TEST(Kernel, AddTaskWhenStartedResultSameTask)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();
    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task2));

    CHECK_EQUAL(ADD_FAIL_DUPLICATE, kernel.AddTask(&task2));
    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "failed add must not change the kernel");
}

TEST(Kernel, AddTaskWhenStartedResultPoolFull)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();
    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task2));

    CHECK_EQUAL(ADD_FAIL_POOL_FULL, kernel.AddTask(&task3));
    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "failed add must not change the kernel");
}

TEST(Kernel, AddTaskDynamicResultInvalid)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;

    // not initialized
    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(&task1));

    kernel.Initialize();

    // nullptr
    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(NULL));
}

// AddTask() from an ISR (KERNEL_DYNAMIC): immediate add, no yield/wait, failures are returned.
// g_InsideISR is reset before any check: a failing CHECK throws and must not leave the flag set.

TEST(Kernel, AddTaskFromIsr)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_RelaxCpuHandler = AddTaskWhenStartedRelaxCpuCounter;
    g_AddTaskWhenStartedRelaxCpuCount = 0;
    g_InsideISR = true;

    const EAddResult result = kernel.AddTask(&task2);

    g_InsideISR = false;
    g_RelaxCpuHandler = NULL;

    CHECK_EQUAL(ADD_OK, result);
    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "task2 must be added immediately");
    CHECK_EQUAL_TEXT(0, platform->m_switch_to_next_nr, "ISR must not switch out the interrupted task");
    CHECK_EQUAL_TEXT(0, g_AddTaskWhenStartedRelaxCpuCount, "ISR must not wait");
    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released");
}

TEST(Kernel, AddTaskFromIsrResultPoolFull)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_InsideISR = true;

    const EAddResult ok   = kernel.AddTask(&task2);
    const EAddResult full = kernel.AddTask(&task3); // must not assert, the ISR has to get the result

    g_InsideISR = false;

    CHECK_EQUAL(ADD_OK, ok);
    CHECK_EQUAL(ADD_FAIL_POOL_FULL, full);
    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "failed add must not change the kernel");
    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released on failure too");
}

TEST(Kernel, AddTaskFromIsrResultSameTask)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_InsideISR = true;

    const EAddResult ok   = kernel.AddTask(&task2);
    const EAddResult dupl = kernel.AddTask(&task2);

    g_InsideISR = false;

    CHECK_EQUAL(ADD_OK, ok);
    CHECK_EQUAL(ADD_FAIL_DUPLICATE, dupl);
    CHECK_EQUAL_TEXT(2, strategy->GetSize(), "failed add must not change the kernel");
    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released on failure too");
}

TEST(Kernel, AddTaskFromIsrBeforeStart)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();

    g_InsideISR = true;

    const EAddResult result = kernel.AddTask(&task1);

    g_InsideISR = false;

    CHECK_EQUAL(ADD_OK, result);
    CHECK_EQUAL(1, strategy->GetSize());
    CHECK_EQUAL(0, g_CriticalSectionState);
}

// TryAllocateNewTask(): always scans the whole pool, i.e. checks the collision with the already added tasks
// (also in a release build) and binds the first free slot, covered by driving the function directly.

TEST(Kernel, TryAllocateNewTaskBindsFirstFreeSlot)
{
    KernelMock<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITask *bound = nullptr;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    // slots 0 and 1 are busy, slot 2 is the first free slot
    CHECK_EQUAL(ADD_OK, kernel.CallTryAllocateNewTask(&task3, &bound));
    POINTERS_EQUAL(&task3, bound);
}

TEST(Kernel, TryAllocateNewTaskEmptyKernel)
{
    KernelMock<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITask *bound = nullptr;

    kernel.Initialize();

    // the very first slot is free
    CHECK_EQUAL(ADD_OK, kernel.CallTryAllocateNewTask(&task1, &bound));
    POINTERS_EQUAL(&task1, bound);
}

TEST(Kernel, TryAllocateNewTaskPoolFull)
{
    KernelMock<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    ITask *bound = &task1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    // no free slot: the whole pool is scanned and nothing is bound
    CHECK_EQUAL(ADD_FAIL_POOL_FULL, kernel.CallTryAllocateNewTask(&task3, &bound));
    POINTERS_EQUAL(nullptr, bound);
}

TEST(Kernel, TryAllocateNewTaskDuplicateBeforeFreeSlot)
{
    KernelMock<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;
    ITask *bound = &task1;

    kernel.Initialize();
    kernel.AddTask(&task1);

    // the busy slot in front of the first free one holds this very task
    CHECK_EQUAL(ADD_FAIL_DUPLICATE, kernel.CallTryAllocateNewTask(&task1, &bound));
    POINTERS_EQUAL(nullptr, bound);
}

TEST(Kernel, TryAllocateNewTaskFindsDuplicateAfterFreeSlot)
{
    KernelMock<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    ITask *bound = &task1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.RemoveTask(&task1); // slot 0 is free again, task2 stays in slot 1

    // the scan continues past the first free slot and finds task2; the failed call must not bind a slot
    CHECK_EQUAL(ADD_FAIL_DUPLICATE, kernel.CallTryAllocateNewTask(&task2, &bound));
    POINTERS_EQUAL(nullptr, bound);

    // the free slot 0 is still available to another task
    TaskMock<ACCESS_USER> task3;
    CHECK_EQUAL(ADD_OK, kernel.CallTryAllocateNewTask(&task3, &bound));
    POINTERS_EQUAL(&task3, bound);
}

// ---------------------------------------------------------------------------- //
// IKernelService::ExitTask(): the calling task schedules its removal, is switched  //
// out and never returns. The test harness has no context switch, so the relax-CPU  //
// handler (called while the task busy-waits to be switched out) drives the ticks   //
// and leaves ExitTask() by an exception once the kernel has removed the task.      //
// ---------------------------------------------------------------------------- //

struct ExitTaskDone {};

struct ExitTaskTaskMock : public TaskMock<ACCESS_USER>
{
    ExitTaskTaskMock() : m_exited(false) {}

    bool m_exited;

private:
    void OnExit() override { m_exited = true; }
};

static PlatformTestMock          *g_ExitTaskPlatform = NULL;
static const ITaskSwitchStrategy *g_ExitTaskStrategy = NULL;
static uint32_t                   g_ExitTaskTicks    = 0;

static void ExitTaskRelaxCpu()
{
    g_ExitTaskPlatform->ProcessTick();
    ++g_ExitTaskTicks;

    // the exiting task has been removed by the tick: leave ExitTask() (it would loop forever otherwise)
    if (g_ExitTaskStrategy->GetSize() == 1)
    {
        throw ExitTaskDone();
    }
}

TEST(Kernel, ExitTask)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    ExitTaskTaskMock task1;
    TaskMock<ACCESS_USER> task2;
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start(); // task1 is the running one

    g_ExitTaskPlatform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    g_ExitTaskStrategy = strategy;
    g_ExitTaskTicks    = 0;
    g_RelaxCpuHandler  = ExitTaskRelaxCpu;

    bool left_by_exception = false;
    try
    {
        stk::ExitTask();
    }
    catch (ExitTaskDone &)
    {
        left_by_exception = true;
    }

    g_RelaxCpuHandler = NULL;

    CHECK_TEXT(left_by_exception, "ExitTask() must not return, the harness leaves it by the exception");
    CHECK_EQUAL_TEXT(1, strategy->GetSize(), "calling task must be removed from the strategy");
    CHECK_TEXT(task1.m_exited, "OnExit() must be called when the removal is completed");
    CHECK_TRUE_TEXT(g_ExitTaskTicks >= 1, "the removal is completed by the tick");

    // formalized restart: a removed task can be added again
    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task1));
    CHECK_EQUAL(2, strategy->GetSize());
}

TEST(Kernel, ExitTaskReAddBeforeRemovalCompletedFails)
{
    Kernel<KERNEL_DYNAMIC, 3, SwitchStrategyRR, PlatformTestMock> kernel;
    ExitTaskTaskMock task1;
    TaskMock<ACCESS_USER> task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // the task is only marked: OnExit() has not been called yet, the slot is still referenced
    kernel.ScheduleTaskRemoval(&task1);

    CHECK_FALSE(task1.m_exited);
    CHECK_EQUAL(ADD_FAIL_DUPLICATE, kernel.AddTask(&task1));
}

TEST(Kernel, ExitTaskNotSupported)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    ExitTaskTaskMock task1;
    TaskMock<ACCESS_USER> task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    g_TestContext.ExpectPanic(true);

    stk::ExitTask();
    CHECK_EQUAL(KERNEL_PANIC_BAD_MODE, test::g_PanicValue);
}

TEST(Kernel, ExitTaskFromIsrAsserts)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    g_InsideISR = true;

    try
    {
        g_TestContext.ExpectAssert(true);
        stk::ExitTask();
        g_InsideISR = false;
        CHECK_TEXT(false, "expecting to fail calling ExitTask() from an ISR");
    }
    catch (TestAssertPassed &pass)
    {
        g_InsideISR = false;
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, AddTaskWhenStartedFailStatic)
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
        CHECK_TEXT(false, "expecting to fail adding a task after Start() in KERNEL_STATIC");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

static struct YieldPendingTaskExitRelaxCpuContext
{
    YieldPendingTaskExitRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        platform = NULL;
        strategy = NULL;
    }

    uint32_t                   counter;
    PlatformTestMock          *platform;
    const ITaskSwitchStrategy *strategy;

    // busy-wait of BusyWaitWhileYielding(): STATE_YIELD_PENDING is set at this moment
    void Process()
    {
        if (counter == 0)
        {
            // the yielding task exits before the pending yield is consumed by a tick
            platform->EventTaskExit(platform->m_stack_active);

            CHECK_EQUAL_TEXT(2, strategy->GetSize(), "removal is deferred until the next tick");
        }

        platform->ProcessTick();

        ++counter;
    }
}
g_YieldPendingTaskExitRelaxCpuContext;

static void YieldPendingTaskExitRelaxCpu()
{
    g_YieldPendingTaskExitRelaxCpuContext.Process();
}

TEST(Kernel, TaskExitWhileYieldPending)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);
    CHECK_EQUAL(2, strategy->GetSize());

    g_YieldPendingTaskExitRelaxCpuContext.Clear();
    g_YieldPendingTaskExitRelaxCpuContext.platform = platform;
    g_YieldPendingTaskExitRelaxCpuContext.strategy = strategy;
    g_RelaxCpuHandler = YieldPendingTaskExitRelaxCpu;

    // task1 yields (strategy handles it: STATE_YIELD_PENDING is set) and exits while the yield is pending;
    // Yield() must return: the tick consumes the pending yield even though the task is leaving
    Yield();

    CHECK_EQUAL(1, g_YieldPendingTaskExitRelaxCpuContext.counter);
    CHECK_EQUAL(1, platform->m_switch_to_next_nr);

    // the exiting task is switched out, task2 gets the CPU (removal itself is processed on the next tick)
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
    CHECK_EQUAL(2, strategy->GetSize());

    // next tick removes the exited task, no stale yield state is left behind
    platform->ProcessTick();
    CHECK_EQUAL(1, strategy->GetSize());
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    // the remaining task still yields normally (to itself, as the only runnable task)
    Yield();
    CHECK_EQUAL(2, platform->m_switch_to_next_nr);
    CHECK_EQUAL(1, strategy->GetSize());
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
}

#if STK_TICKLESS_IDLE
static struct YieldTicklessRelaxCpuContext
{
    YieldTicklessRelaxCpuContext()
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

    // busy-wait of BusyWaitWhileYielding(): one tick (of platform->m_sleep_ticks elapsed ticks) consumes the pending yield
    void Process()
    {
        platform->ProcessTick();
        ++counter;
    }
}
g_YieldTicklessRelaxCpuContext;

static void YieldTicklessRelaxCpu()
{
    g_YieldTicklessRelaxCpuContext.Process();
}

TEST(Kernel, YieldNsyTickless)
{
    Kernel<KERNEL_STATIC | KERNEL_TICKLESS, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    g_YieldTicklessRelaxCpuContext.Clear();
    g_YieldTicklessRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = YieldTicklessRelaxCpu;

    // pending yield is consumed by a regular (1 elapsed tick) tick: rotation moves to the next task
    Yield();

    CHECK_EQUAL(1, g_YieldTicklessRelaxCpuContext.counter);
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    // yields keep rotating in tickless mode
    Yield();
    CHECK_EQUAL((Word)task3.GetStack(), active->SP);

    Yield();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    CHECK_EQUAL(3, platform->m_switch_to_next_nr);
}

TEST(Kernel, YieldNsyTicklessCoalescedTick)
{
    Kernel<KERNEL_STATIC | KERNEL_TICKLESS, 3, SwitchStrategyRR_NSY, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    g_YieldTicklessRelaxCpuContext.Clear();
    g_YieldTicklessRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = YieldTicklessRelaxCpu;

    const int64_t ticks_before = IKernelService::GetInstance()->GetTicks();

    // the next tick is delivered with several elapsed ticks (as after a suppressed tick interval)
    platform->m_sleep_ticks = 5;

    Yield();

    // the tick consumed the pending yield (Yield returned after a single handler call)
    CHECK_EQUAL(1, g_YieldTicklessRelaxCpuContext.counter);

    // all elapsed ticks are accounted for by the kernel
    // note: PlatformTestMock::m_ticks_count is not used here, the mock adds the value returned by OnTick (next sleep
    // interval) to it, not the elapsed ticks
    CHECK_EQUAL(5, (int)(IKernelService::GetInstance()->GetTicks() - ticks_before));

    // but the rotation advanced by a single position only: task1 -> task2
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    // the next regular tick continues the rotation normally
    platform->m_sleep_ticks = 1;
    platform->ProcessTick();
    CHECK_EQUAL((Word)task3.GetStack(), active->SP);
}
#endif

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

TEST(Kernel, AddTaskFailHrtStartedDynamicReturnsResult)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task1, 1, 1, 0));
    kernel.Start();

    // KERNEL_DYNAMIC: the failure is only returned, never asserted
    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(&task2, 1, 1, 0));
}

TEST(Kernel, AddTaskHrtResults)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, task3;

    kernel.Initialize();

    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(nullptr, 1, 1, 0));
    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(&task1, 0, 1, 0));          // periodicity
    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(&task1, 1, 0, 0));          // deadline
    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(&task1, 1, 1, -1));         // start delay
    CHECK_EQUAL(ADD_FAIL_INVALID, kernel.AddTask(&task1, INT32_MAX, 1, 0));  // periodicity upper bound

    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task1, 1, 1, 0));
    CHECK_EQUAL(ADD_FAIL_DUPLICATE, kernel.AddTask(&task1, 1, 1, 0));
    CHECK_EQUAL(ADD_OK, kernel.AddTask(&task2, 1, 1, 0));
    CHECK_EQUAL(ADD_FAIL_POOL_FULL, kernel.AddTask(&task3, 1, 1, 0));
}

TEST(Kernel, AddTaskFailHrtStarted)
{
    Kernel<KERNEL_STATIC | KERNEL_HRT, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1, 1, 1, 0);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.AddTask(&task2, 1, 1, 0);
        CHECK_TEXT(false, "expecting to AddTask to fail when KERNEL_HRT is started");
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

typedef KernelMock<KERNEL_STATIC, 2, SwitchStrategyRR_NSY, PlatformTestMock> KernelStaticNsyMock;

static struct ForceContextSwitchYieldRelaxCpuContext
{
    ForceContextSwitchYieldRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter = 0;
        kernel  = NULL;
        tid     = TID_NONE;
        result  = false;
        idle    = NULL;
        active  = NULL;
    }

    uint32_t             counter;
    KernelStaticNsyMock *kernel;
    TId                  tid;    //!< id of the yielding task
    bool                 result; //!< result of Kernel::OnForceContextSwitch()
    Stack               *idle;
    Stack               *active;

    // busy-wait of KernelTask::BusyWaitWhileYielding(): the platform driver's ForceContextSwitch(tid)
    // arrives in the ISR as OnForceContextSwitch() and must consume STATE_YIELD_PENDING
    void Process()
    {
        if (counter == 0)
        {
            result = kernel->CallForceContextSwitch(tid, idle, active);
        }

        ++counter;
    }
}
g_ForceContextSwitchYieldRelaxCpuContext;

static void ForceContextSwitchYieldRelaxCpu()
{
    g_ForceContextSwitchYieldRelaxCpuContext.Process();
}

TEST(Kernel, ForceContextSwitchPendingYield)
{
    KernelStaticNsyMock kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is the running task after Start()
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);

    g_ForceContextSwitchYieldRelaxCpuContext.Clear();
    g_ForceContextSwitchYieldRelaxCpuContext.kernel = &kernel;
    g_ForceContextSwitchYieldRelaxCpuContext.tid    = task1.GetTid();
    g_RelaxCpuHandler = ForceContextSwitchYieldRelaxCpu;

    // task1 calls Yield(): strategy handles it (task stays runnable) and the task sets STATE_YIELD_PENDING
    // then spins in BusyWaitWhileYielding() until the ISR processes ForceContextSwitch(task1_id)
    Yield();

    CHECK_EQUAL(1, platform->m_switch_to_next_nr);

    // the yield is consumed by OnForceContextSwitch (the busy-wait was released without any tick)
    CHECK_EQUAL(1, g_ForceContextSwitchYieldRelaxCpuContext.counter);
    CHECK_TRUE(g_ForceContextSwitchYieldRelaxCpuContext.result);
    CHECK_TRUE(g_ForceContextSwitchYieldRelaxCpuContext.idle != NULL);
    CHECK_TRUE(g_ForceContextSwitchYieldRelaxCpuContext.active != NULL);

    // task1 (still runnable) is switched out to Idle
    CHECK_EQUAL((Word)task1.GetStack(), g_ForceContextSwitchYieldRelaxCpuContext.idle->SP);

    // strategy picked the next task: task2 becomes Active
    CHECK_EQUAL((Word)task2.GetStack(), g_ForceContextSwitchYieldRelaxCpuContext.active->SP);

    // task1 is no longer the running task, a repeated ISR call for its id is a no-op
    Stack *idle = NULL, *active = NULL;
    CHECK_FALSE(kernel.CallForceContextSwitch(task1.GetTid(), idle, active));
    CHECK_TRUE(idle == NULL);
    CHECK_TRUE(active == NULL);
}

TEST(Kernel, ForceContextSwitchNoPendingYield)
{
    KernelStaticNsyMock kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *idle = NULL, *active = NULL;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);

    // task1 is running, not sleeping and has not yielded: STATE_YIELD_PENDING is not set, nothing to process
    CHECK_FALSE(kernel.CallForceContextSwitch(task1.GetTid(), idle, active));
    CHECK_TRUE(idle == NULL);
    CHECK_TRUE(active == NULL);

    // task1 is still the running one
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);
}

static struct YieldSleepingTaskRelaxCpuContext
{
    YieldSleepingTaskRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter  = 0;
        platform = NULL;
        sp_first = 0;
    }

    uint32_t          counter;
    PlatformTestMock *platform;
    size_t            sp_first; //!< active task after the first tick

    // busy-wait of KernelTask::BusyWaitWhileSleeping(): every call is a tick of time passing
    void Process()
    {
        platform->ProcessTick();

        if (counter == 0)
        {
            sp_first = platform->m_stack_active->SP;
        }

        ++counter;
    }
}
g_YieldSleepingTaskRelaxCpuContext;

static void YieldSleepingTaskRelaxCpu()
{
    g_YieldSleepingTaskRelaxCpuContext.Process();
}

TEST(Kernel, TaskSwitchNsyDeclinedYieldSleeps)
{
    KernelStaticNsyMock kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);

    g_YieldSleepingTaskRelaxCpuContext.Clear();
    g_YieldSleepingTaskRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = YieldSleepingTaskRelaxCpu;

    // task1 is already in a sleeping state when it calls Yield() (e.g. it was put to sleep and is waiting to be
    // de-scheduled): Kernel::OnTaskSwitch must not consult the strategy (OnTaskYield) and falls back to the legacy
    // yield, i.e. OnTaskSleep(caller_SP, YIELD_TICKS)
    kernel.ScheduleSleepOnActiveTask(5);

    Yield();

    CHECK_EQUAL(1, platform->m_switch_to_next_nr);

    // legacy yield: task1 slept for YIELD_TICKS (1 sleep-entry tick + 1 tick to wake up), whereas a yield
    // handled by the strategy would be released by the very first tick
    CHECK_EQUAL(2, g_YieldSleepingTaskRelaxCpuContext.counter);

    // while task1 was sleeping task2 was running
    CHECK_EQUAL((Word)task2.GetStack(), g_YieldSleepingTaskRelaxCpuContext.sp_first);

    // task1 woke up (sleep time was reduced to YIELD_TICKS) and is scheduled again
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);
}

TEST(Kernel, TaskSwitchNsyUnknownSP)
{
    KernelStaticNsyMock kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // expect the assertion for a caller which is not known to the kernel but do not throw from it,
    // so that execution continues to the defensive "null task" branch of Kernel::OnTaskSwitch
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);

    platform->EventTaskSwitch(0xdeadbeef);

    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);

    // nothing was scheduled by the bogus request: no pending yield, rotation is intact
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    platform->ProcessTick();
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);
}

static struct YieldNsyWakeRelaxCpuContext
{
    YieldNsyWakeRelaxCpuContext()
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

    // busy-wait of BusyWaitWhileYielding(): one tick consumes the pending yield
    void Process()
    {
        platform->ProcessTick();
        ++counter;
    }
}
g_YieldNsyWakeRelaxCpuContext;

static void YieldNsyWakeRelaxCpu()
{
    g_YieldNsyWakeRelaxCpuContext.Process();
}

TEST(Kernel, YieldNsyAfterWakeFromSleepTrap)
{
    KernelStaticNsyMock kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    ITaskSwitchStrategy *strategy = kernel.GetSwitchStrategy();
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // task1 goes to sleep for a long time, task2 takes over
    kernel.ScheduleSleepOnActiveTask(6);
    platform->ProcessTick();
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    // task2 goes to sleep for a shorter time: no runnable tasks left, strategy cursor is reset (nullptr)
    kernel.ScheduleSleepOnActiveTask(2);
    platform->ProcessTick();
    CHECK_EQUAL(2, strategy->GetSize());

    // sleep trap: neither of the tasks is active
    CHECK_TRUE(active->SP != (Word)task1.GetStack());
    CHECK_TRUE(active->SP != (Word)task2.GetStack());

    // wait for task2 to wake up (task1 is still sleeping): the strategy cursor is restored from nullptr
    int guard = 0;
    while ((active->SP != (Word)task2.GetStack()) && (++guard < 10))
    {
        platform->ProcessTick();
    }
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    g_YieldNsyWakeRelaxCpuContext.Clear();
    g_YieldNsyWakeRelaxCpuContext.platform = platform;
    g_RelaxCpuHandler = YieldNsyWakeRelaxCpu;

    // task2 is the only runnable task: its yield is handled by the strategy using the restored cursor and
    // returns to itself, the kernel must not fall into the sleep trap
    Yield();

    CHECK_EQUAL(1, platform->m_switch_to_next_nr);
    CHECK_EQUAL(1, g_YieldNsyWakeRelaxCpuContext.counter);
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    g_RelaxCpuHandler = NULL;

    // task1 wakes up later and is appended behind task2: rotation continues task2 -> task1
    guard = 0;
    while ((active->SP != (Word)task1.GetStack()) && (++guard < 10))
    {
        platform->ProcessTick();
    }
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    g_RelaxCpuHandler = YieldNsyWakeRelaxCpu;

    // task1 yields to the other task
    Yield();

    CHECK_EQUAL(2, platform->m_switch_to_next_nr);
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
}

TEST(Kernel, ForceContextSwitch)
{
    KernelMock<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *idle = nullptr, *active = nullptr;
    const TId task1_id = task1.GetTid();

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
    const TId task2_id = task2.GetTid();

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
    const TId task1_id = task1.GetTid();

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

TEST(Kernel, OnTaskExitScheduleNotCurrent)
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

    // task2 is not the current task and is not sleeping yet: ScheduleRemoval() puts it to sleep
    // (STATE_SLEEP_PENDING) and must deliver the sleep entry to the strategy immediately
    kernel.ScheduleTaskRemoval(&task2);

    // nothing changes right away: task1 keeps running, task2 is not removed yet
    CHECK_EQUAL(2, kernel.GetSwitchStrategy()->GetSize());
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // the tick removes task2 (it is not the current task) and the strategy never hands it the CPU
    platform->ProcessTick();

    CHECK_EQUAL(1, kernel.GetSwitchStrategy()->GetSize());
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // task1 is the only task left and keeps running
    platform->ProcessTick();
    CHECK_EQUAL(1, kernel.GetSwitchStrategy()->GetSize());
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());
}

TEST(Kernel, OnTaskExitScheduleNotCurrentAlreadySleeping)
{
    Kernel<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;
    bool suspended = false;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // put task2 to sleep and let the tick consume its STATE_SLEEP_PENDING
    kernel.SuspendTask(&task2, suspended);
    CHECK_TRUE(suspended);

    platform->ProcessTick();
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // task2 is already sleeping (the strategy knows about it): STATE_SLEEP_PENDING is not set again,
    // so ScheduleRemoval() must not report the sleep entry a second time
    kernel.ScheduleTaskRemoval(&task2);

    CHECK_EQUAL(2, kernel.GetSwitchStrategy()->GetSize());

    // next tick removes task2
    platform->ProcessTick();

    CHECK_EQUAL(1, kernel.GetSwitchStrategy()->GetSize());
    CHECK_EQUAL(active->SP, (Word)task1.GetStack());
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

// ============================================================================ //
// ======================== IKernel::GetTaskState ============================= //
// ============================================================================ //

static TaskStateInfo GetTaskState(IKernel *kernel, ITask *user_task)
{
    TaskStateInfo info = {
        .state     = TASK_STATE_REMOVED,
        .wait_obj  = nullptr,
        .remaining = WAIT_INFINITE
    };

    kernel->GetTaskState(user_task, info);

    return info;
}

static void CheckTaskState(const TaskStateInfo &info, ETaskState state)
{
    CHECK_EQUAL(state, info.state);

    // wait object is reported for TASK_STATE_WAITING only
    if (state != TASK_STATE_WAITING)
    {
        CHECK_TRUE(info.wait_obj == nullptr);
    }
}

TEST(Kernel, GetTaskStateNotAddedIsRemoved)
{
    Kernel<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1);

    // task2 was never added: not bound to a kernel slot, the query itself succeeds
    TaskStateInfo query = { TASK_STATE_READY, nullptr, 0 };
    CHECK_TRUE(kernel.GetTaskState(&task2, query));

    const TaskStateInfo info = GetTaskState(&kernel, &task2);

    CheckTaskState(query, TASK_STATE_REMOVED);
    CHECK_EQUAL(WAIT_INFINITE, query.remaining);
    CheckTaskState(info, TASK_STATE_REMOVED);
    CHECK_EQUAL(WAIT_INFINITE, info.remaining);
}

TEST(Kernel, GetTaskStateNullTask)
{
    Kernel<KERNEL_STATIC | KERNEL_TELEMETRY, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    ISyncObject *const marker = reinterpret_cast<ISyncObject *>(static_cast<uintptr_t>(0x1000));

    kernel.Initialize();

    // invalid argument: reported by the result, info is not touched
    TaskStateInfo info = { TASK_STATE_EXITING, marker, 1234 };
    CHECK_FALSE(kernel.GetTaskState(nullptr, info));

    CHECK_EQUAL(TASK_STATE_EXITING, info.state);
    CHECK_TRUE(info.wait_obj == marker);
    CHECK_EQUAL(1234, info.remaining);
}

TEST(Kernel, GetTaskStateTelemetryDisabled)
{
    // KERNEL_TELEMETRY is not set: the introspection is compiled out and reports failure
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    ISyncObject *const marker = reinterpret_cast<ISyncObject *>(static_cast<uintptr_t>(0x1000));

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    TaskStateInfo info = { TASK_STATE_EXITING, marker, 1234 };
    CHECK_FALSE(kernel.GetTaskState(&task, info));

    CHECK_EQUAL(TASK_STATE_EXITING, info.state);
    CHECK_TRUE(info.wait_obj == marker);
    CHECK_EQUAL(1234, info.remaining);

    // the helper then reports the defaults, which is the same as for a task which is not bound
    CheckTaskState(GetTaskState(&kernel, &task), TASK_STATE_REMOVED);
}

TEST(Kernel, GetTaskStateReady)
{
    Kernel<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    // bound to a slot, not started yet
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);

    kernel.Start();

    // running (task1) and waiting for the CPU (task2) are both runnable
    const TaskStateInfo info1 = GetTaskState(&kernel, &task1);
    const TaskStateInfo info2 = GetTaskState(&kernel, &task2);

    CheckTaskState(info1, TASK_STATE_READY);
    CheckTaskState(info2, TASK_STATE_READY);
    CHECK_EQUAL(WAIT_INFINITE, info1.remaining);
    CHECK_EQUAL(WAIT_INFINITE, info2.remaining);

    // stays ready across context switches
    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_READY);
}

TEST(Kernel, GetTaskStateSleeping)
{
    KernelMock<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is the active task: it goes to sleep for 5 ticks
    kernel.ScheduleSleepOnActiveTask(5);

    const TaskStateInfo info = GetTaskState(&kernel, &task1);

    CheckTaskState(info, TASK_STATE_SLEEPING);
    CHECK_EQUAL(5, info.remaining);

    // other task is not affected
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_READY);

    // the sleep expires after a few ticks, never earlier than the requested time
    uint32_t ticks = 0;
    Timeout prev_remaining = info.remaining;

    while ((GetTaskState(&kernel, &task1).state == TASK_STATE_SLEEPING) && (ticks < 10))
    {
        const Timeout remaining = GetTaskState(&kernel, &task1).remaining;
        CHECK_TRUE(remaining <= prev_remaining); // counts down
        prev_remaining = remaining;

        platform->ProcessTick();
        ++ticks;
    }

    CHECK_TRUE(ticks >= 4);
    CHECK_TRUE(ticks <= 7);

    const TaskStateInfo awake = GetTaskState(&kernel, &task1);
    CheckTaskState(awake, TASK_STATE_READY);
    CHECK_EQUAL(WAIT_INFINITE, awake.remaining);
}

TEST(Kernel, GetTaskStateSuspended)
{
    Kernel<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    bool suspended = false;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    kernel.SuspendTask(&task2, suspended);
    CHECK_TRUE(suspended);

    // a suspended task sleeps infinitely inside the kernel, but is reported as suspended, not as sleeping
    const TaskStateInfo info = GetTaskState(&kernel, &task2);
    CheckTaskState(info, TASK_STATE_SUSPENDED);
    CHECK_EQUAL(WAIT_INFINITE, info.remaining);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);

    // stays suspended across ticks
    platform->ProcessTick();
    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_SUSPENDED);

    // leaves the suspended state immediately, becomes runnable on the next tick
    kernel.ResumeTask(&task2);
    CHECK_TRUE(GetTaskState(&kernel, &task2).state != TASK_STATE_SUSPENDED);

    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_READY);
}

static struct GetTaskStateSuspendSelfContext
{
    GetTaskStateSuspendSelfContext()
    {
        Clear();
    }

    void Clear()
    {
        platform        = nullptr;
        kernel          = nullptr;
        task1           = nullptr;
        count           = 0;
        sampled         = false;
        sampled_state   = TASK_STATE_REMOVED;
        sampled_wait    = nullptr;
    }

    IPlatform   *platform;
    IKernel     *kernel;
    ITask       *task1;
    uint32_t     count;
    bool         sampled;
    ETaskState   sampled_state;
    ISyncObject *sampled_wait;

    void Process()
    {
        platform->ProcessTick();

        if (count == 1)
        {
            // task1 is suspended and spinning inside SuspendTask()
            const TaskStateInfo info = GetTaskState(kernel, task1);

            sampled       = true;
            sampled_state = info.state;
            sampled_wait  = info.wait_obj;
        }

        if (count > 2)
        {
            kernel->ResumeTask(task1);
        }

        ++count;
    }
}
g_GetTaskStateSuspendSelfContext;

static void GetTaskStateSuspendSelfRelaxCpu()
{
    g_GetTaskStateSuspendSelfContext.Process();
}

TEST(Kernel, GetTaskStateSuspendedSelf)
{
    Kernel<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    bool suspended = false;

    g_GetTaskStateSuspendSelfContext.Clear();
    g_GetTaskStateSuspendSelfContext.platform = platform;
    g_GetTaskStateSuspendSelfContext.kernel   = &kernel;
    g_GetTaskStateSuspendSelfContext.task1    = &task1;

    decltype(g_RelaxCpuHandler) prev_handler = g_RelaxCpuHandler;
    g_RelaxCpuHandler = GetTaskStateSuspendSelfRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is calling SuspendTask to suspend self, returns when the handler resumes it
    kernel.SuspendTask(&task1, suspended);
    CHECK_TRUE(suspended);

    g_RelaxCpuHandler = prev_handler;

    CHECK_TRUE(g_GetTaskStateSuspendSelfContext.sampled);
    CHECK_EQUAL(TASK_STATE_SUSPENDED, g_GetTaskStateSuspendSelfContext.sampled_state);
    CHECK_TRUE(g_GetTaskStateSuspendSelfContext.sampled_wait == nullptr);

    // resumed
    CHECK_TRUE(GetTaskState(&kernel, &task1).state != TASK_STATE_SUSPENDED);
}

static struct GetTaskStateWaitContext
{
    GetTaskStateWaitContext()
    {
        Reset();
    }

    void Reset()
    {
        platform        = nullptr;
        kernel          = nullptr;
        task            = nullptr;
        sobj            = nullptr;
        wake            = false;
        counter         = 0;
        sampled         = false;
        suspend_result  = SUSPEND_FAIL;
        lift_result     = RESUME_FAIL;
        waiting.state   = TASK_STATE_REMOVED;
        waiting.wait_obj = nullptr;
        waiting.remaining = 0;
        after_suspend   = waiting;
        after_wake      = waiting;
    }

    IPlatform    *platform;
    IKernel      *kernel;
    ITask        *task;
    ISyncObject  *sobj;
    bool          wake;           // end the wait with Wake() on the 3rd tick, otherwise let it expire
    uint32_t      counter;
    bool          sampled;
    ESuspendResult suspend_result; // result of SuspendTask() attempted on the waiting task
    EResumeResult  lift_result;    // result of ResumeTask() which withdraws the deferred suspension
    TaskStateInfo waiting;        // state while the task is blocked
    TaskStateInfo after_suspend;  // state right after SuspendTask() was attempted on the blocked task
    TaskStateInfo after_wake;     // state right after Wake(), before the next tick

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (counter == 2)
        {
            waiting = GetTaskState(kernel, task);
            sampled = true;

            // SuspendTask() does not touch a task which is already blocked: the suspension is deferred
            suspend_result = kernel->SuspendTask(task);
            after_suspend  = GetTaskState(kernel, task);

            // withdraw it, otherwise the task would be parked when the wait ends and never finish
            lift_result = kernel->ResumeTask(task);
        }
        else
        if ((counter == 3) && wake)
        {
            IKernelService::GetInstance()->Wake(sobj, false);
            after_wake = GetTaskState(kernel, task);
        }
    }
}
g_GetTaskStateWaitContext;

static void GetTaskStateWaitRelaxCpu()
{
    g_GetTaskStateWaitContext.Process();
}

static void Test_GetTaskStateWaiting(Timeout wait_ticks, bool wake, EWaitResult expected)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC | KERNEL_TELEMETRY, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_GetTaskStateWaitContext.Reset();
    g_GetTaskStateWaitContext.platform = platform;
    g_GetTaskStateWaitContext.kernel   = &kernel;
    g_GetTaskStateWaitContext.task     = &task;
    g_GetTaskStateWaitContext.sobj     = &sobj;
    g_GetTaskStateWaitContext.wake     = wake;

    decltype(g_RelaxCpuHandler) prev_handler = g_RelaxCpuHandler;
    g_RelaxCpuHandler = GetTaskStateWaitRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    CheckTaskState(GetTaskState(&kernel, &task), TASK_STATE_READY);

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, wait_ticks);

    g_RelaxCpuHandler = prev_handler;

    CHECK_EQUAL(expected, wresult);
    CHECK_TRUE(g_GetTaskStateWaitContext.sampled);

    // blocked on the sync object: reported as waiting (not as sleeping, although it sleeps inside the kernel)
    const TaskStateInfo &waiting = g_GetTaskStateWaitContext.waiting;
    CHECK_EQUAL(TASK_STATE_WAITING, waiting.state);
    CHECK_TRUE(waiting.wait_obj == &sobj);

    if (wait_ticks == WAIT_INFINITE)
    {
        CHECK_EQUAL(WAIT_INFINITE, waiting.remaining);
    }
    else
    {
        CHECK_TRUE(waiting.remaining > 0);
        CHECK_TRUE(waiting.remaining <= wait_ticks);
    }

    // SuspendTask() defers the suspension of the blocked task: it keeps waiting, and the deferred
    // suspension can be withdrawn
    CHECK_EQUAL(SUSPEND_DEFERRED, g_GetTaskStateWaitContext.suspend_result);
    CHECK_EQUAL(RESUME_LIFTED, g_GetTaskStateWaitContext.lift_result);
    CHECK_EQUAL(TASK_STATE_WAITING, g_GetTaskStateWaitContext.after_suspend.state);
    CHECK_TRUE(g_GetTaskStateWaitContext.after_suspend.wait_obj == &sobj);

    if (wake)
    {
        // Wake() detaches the task from the sync object immediately
        CHECK_TRUE(g_GetTaskStateWaitContext.after_wake.state != TASK_STATE_WAITING);
        CHECK_TRUE(g_GetTaskStateWaitContext.after_wake.wait_obj == nullptr);
    }

    // the wait is over
    const TaskStateInfo done = GetTaskState(&kernel, &task);
    CheckTaskState(done, TASK_STATE_READY);
    CHECK_EQUAL(WAIT_INFINITE, done.remaining);
}

TEST(Kernel, GetTaskStateWaitingTimeout)
{
    Test_GetTaskStateWaiting(4, false, WAIT_RESULT_TIMEOUT);
}

TEST(Kernel, GetTaskStateWaitingSignaled)
{
    Test_GetTaskStateWaiting(10, true, WAIT_RESULT_SIGNAL);
}

TEST(Kernel, GetTaskStateWaitingInfinite)
{
    Test_GetTaskStateWaiting(WAIT_INFINITE, true, WAIT_RESULT_SIGNAL);
}

TEST(Kernel, GetTaskStateExitingAndRemoved)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task2 is not the current task: it is removed by the next tick
    kernel.ScheduleTaskRemoval(&task2);

    const TaskStateInfo info = GetTaskState(&kernel, &task2);
    CheckTaskState(info, TASK_STATE_EXITING);
    CHECK_EQUAL(WAIT_INFINITE, info.remaining);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);

    platform->ProcessTick();

    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_REMOVED);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);
}

TEST(Kernel, GetTaskStateExitingCurrentTask)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is the current task: removal is only scheduled
    kernel.ScheduleTaskRemoval(&task1);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_EXITING);

    // the kernel can not remove the task it was running, the slot is released one tick later
    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_EXITING);

    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_REMOVED);
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_READY);
}

TEST(Kernel, GetTaskStateExitingHasPriorityOverSuspended)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    bool suspended = false;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    kernel.SuspendTask(&task2, suspended);
    CHECK_TRUE(suspended);

    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_SUSPENDED);

    // a suspended task which is being removed is reported as exiting
    kernel.ScheduleTaskRemoval(&task2);
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_EXITING);

    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_REMOVED);
}

// ============================================================================ //
// ============== IKernel::SuspendTask / ResumeTask (results, deferred) ======= //
// ============================================================================ //

TEST(Kernel, SuspendResumeResultCodes)
{
    Kernel<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2, unknown;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // not suspended: nothing to resume
    CHECK_EQUAL(RESUME_NOT_SUSPENDED, kernel.ResumeTask(&task2));

    // the legacy overload reports an immediate suspension only
    bool suspended = false;
    kernel.SuspendTask(&task2, suspended);
    CHECK_TRUE(suspended);

    // nesting is not supported
    CHECK_EQUAL(SUSPEND_ALREADY, kernel.SuspendTask(&task2));
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_SUSPENDED);

    CHECK_EQUAL(RESUME_DONE, kernel.ResumeTask(&task2));
    CHECK_EQUAL(RESUME_NOT_SUSPENDED, kernel.ResumeTask(&task2));

    // the resumed task is still asleep until the next tick: suspending it now is deferred, then withdrawn
    CHECK_EQUAL(SUSPEND_DEFERRED, kernel.SuspendTask(&task2));
    CHECK_EQUAL(RESUME_LIFTED, kernel.ResumeTask(&task2));

    // task is not bound to the kernel
    CHECK_EQUAL(SUSPEND_FAIL, kernel.SuspendTask(&unknown));
    CHECK_EQUAL(RESUME_FAIL, kernel.ResumeTask(&unknown));
}

TEST(Kernel, SuspendResumeExitingTaskFails)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_PRIVILEGED> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    kernel.ScheduleTaskRemoval(&task2);
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_EXITING);

    // a task which is being removed can be neither suspended nor resumed
    CHECK_EQUAL(SUSPEND_FAIL, kernel.SuspendTask(&task2));
    CHECK_EQUAL(RESUME_FAIL, kernel.ResumeTask(&task2));

    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_REMOVED);

    // and neither after the slot is released
    CHECK_EQUAL(SUSPEND_FAIL, kernel.SuspendTask(&task2));
    CHECK_EQUAL(RESUME_FAIL, kernel.ResumeTask(&task2));
}

TEST(Kernel, SuspendDeferredWhileSleeping)
{
    KernelMock<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(active->SP, (Word)task1.GetStack());

    // task1 (active) goes to sleep for 4 ticks
    kernel.ScheduleSleepOnActiveTask(4);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_SLEEPING);

    // the suspension is deferred (the call does not block, a sleeping task is not spun on)
    CHECK_EQUAL(SUSPEND_DEFERRED, kernel.SuspendTask(&task1));
    CHECK_EQUAL(SUSPEND_ALREADY, kernel.SuspendTask(&task1));

    // the sleep itself is left untouched: still reported as sleeping with the same time left
    const TaskStateInfo info = GetTaskState(&kernel, &task1);
    CheckTaskState(info, TASK_STATE_SLEEPING);
    CHECK_EQUAL(4, info.remaining);

    // when the sleep expires the task is parked (suspended) instead of being woken
    uint32_t ticks = 0;
    while ((GetTaskState(&kernel, &task1).state == TASK_STATE_SLEEPING) && (ticks < 10))
    {
        platform->ProcessTick();
        ++ticks;
    }

    CHECK_TRUE(ticks >= 3);
    CHECK_TRUE(ticks <= 7);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_SUSPENDED);

    // it is never scheduled while suspended, task2 owns the CPU
    for (uint32_t i = 0; i < 4; ++i)
    {
        platform->ProcessTick();
        CHECK_EQUAL(active->SP, (Word)task2.GetStack());
        CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_SUSPENDED);
    }

    // resumed: becomes runnable and gets the CPU again
    CHECK_EQUAL(RESUME_DONE, kernel.ResumeTask(&task1));
    platform->ProcessTick();
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);

    bool ran = false;
    for (uint32_t i = 0; i < 3; ++i)
    {
        ran = ran || (active->SP == (Word)task1.GetStack());
        platform->ProcessTick();
    }
    CHECK_TRUE(ran);
}

TEST(Kernel, SuspendDeferredLiftedBeforeSleepEnds)
{
    KernelMock<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    kernel.ScheduleSleepOnActiveTask(4);

    CHECK_EQUAL(SUSPEND_DEFERRED, kernel.SuspendTask(&task1));

    // the request has not taken effect yet: it is simply withdrawn
    CHECK_EQUAL(RESUME_LIFTED, kernel.ResumeTask(&task1));
    CHECK_EQUAL(RESUME_NOT_SUSPENDED, kernel.ResumeTask(&task1));

    // the sleep still counts down and ends as a normal wake-up
    uint32_t ticks = 0;
    while ((GetTaskState(&kernel, &task1).state == TASK_STATE_SLEEPING) && (ticks < 10))
    {
        platform->ProcessTick();
        ++ticks;
    }

    CHECK_TRUE(ticks >= 3);
    CHECK_TRUE(ticks <= 7);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);

    // and the task is scheduled again
    bool ran = false;
    for (uint32_t i = 0; i < 3; ++i)
    {
        ran = ran || (active->SP == (Word)task1.GetStack());
        platform->ProcessTick();
    }
    CHECK_TRUE(ran);
    CheckTaskState(GetTaskState(&kernel, &task1), TASK_STATE_READY);
}

TEST(Kernel, CancelTaskWaitDoesNotWakeSuspendedTask)
{
    Kernel<KERNEL_STATIC | KERNEL_TELEMETRY, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(SUSPEND_DONE, kernel.SuspendTask(&task2));
    platform->ProcessTick();

    // cancelling must not silently break the suspension (SleepCancel() maps to the same call)
    kernel.CancelTaskWait(&task2);

    for (uint32_t i = 0; i < 3; ++i)
    {
        platform->ProcessTick();
        CHECK_EQUAL(active->SP, (Word)task1.GetStack());
        CheckTaskState(GetTaskState(&kernel, &task2), TASK_STATE_SUSPENDED);
    }

    CHECK_EQUAL(RESUME_DONE, kernel.ResumeTask(&task2));
}

static struct SuspendDeferredWaitContext
{
    SuspendDeferredWaitContext()
    {
        Reset();
    }

    void Reset()
    {
        platform       = nullptr;
        kernel         = nullptr;
        task           = nullptr;
        sobj           = nullptr;
        wake           = false;
        cancel         = false;
        counter        = 0;
        suspend_result = SUSPEND_FAIL;
        resume_result  = RESUME_FAIL;
        waiting.state     = TASK_STATE_REMOVED;
        waiting.wait_obj  = nullptr;
        waiting.remaining = 0;
        parked         = waiting;
        parked_seen    = false;
        parked_at      = 0;
        still_parked   = false;
    }

    IPlatform     *platform;
    IKernel       *kernel;
    ITask         *task;
    ISyncObject   *sobj;
    bool           wake;           // end the wait with Wake() on the 3rd tick
    bool           cancel;         // end the wait with CancelTaskWait() on the 3rd tick
    uint32_t       counter;
    ESuspendResult suspend_result; // result of SuspendTask() on the blocked task
    EResumeResult  resume_result;  // result of ResumeTask() on the parked task
    TaskStateInfo  waiting;        // state right after SuspendTask(), the task is still blocked
    TaskStateInfo  parked;         // state sampled when the wait is over
    bool           parked_seen;    // the task became TASK_STATE_SUSPENDED after its wait ended
    uint32_t       parked_at;      // tick at which that was observed
    bool           still_parked;   // it stayed suspended for a few more ticks

    void Process()
    {
        platform->ProcessTick();
        ++counter;

        if (counter == 2)
        {
            suspend_result = kernel->SuspendTask(task);
            waiting        = GetTaskState(kernel, task);
        }
        else
        if ((counter == 3) && wake)
        {
            IKernelService::GetInstance()->Wake(sobj, false);
        }
        else
        if ((counter == 3) && cancel)
        {
            kernel->CancelTaskWait(task);
        }
        else
        {
            // noop
        }

        if (counter > 2)
        {
            const TaskStateInfo info = GetTaskState(kernel, task);

            if (!parked_seen)
            {
                if (info.state == TASK_STATE_SUSPENDED)
                {
                    parked_seen = true;
                    parked      = info;
                    parked_at   = counter;
                }
            }
            else
            if (counter == (parked_at + 3))
            {
                // nobody has resumed the task for 3 ticks: it must still be parked
                still_parked  = (info.state == TASK_STATE_SUSPENDED);
                resume_result = kernel->ResumeTask(task);
            }
            else
            {
                // noop
            }
        }
    }
}
g_SuspendDeferredWaitContext;

static void SuspendDeferredWaitRelaxCpu()
{
    g_SuspendDeferredWaitContext.Process();
}

static void Test_SuspendDeferredWaiting(Timeout wait_ticks, bool wake, bool cancel, EWaitResult expected)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC | KERNEL_TELEMETRY, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_USER> task;

    MutexMock mutex;
    SyncObjectMock sobj;

    g_SuspendDeferredWaitContext.Reset();
    g_SuspendDeferredWaitContext.platform = platform;
    g_SuspendDeferredWaitContext.kernel   = &kernel;
    g_SuspendDeferredWaitContext.task     = &task;
    g_SuspendDeferredWaitContext.sobj     = &sobj;
    g_SuspendDeferredWaitContext.wake     = wake;
    g_SuspendDeferredWaitContext.cancel   = cancel;

    decltype(g_RelaxCpuHandler) prev_handler = g_RelaxCpuHandler;
    g_RelaxCpuHandler = SuspendDeferredWaitRelaxCpu;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    MutexMock::ScopedLock guard(mutex);

    // returns only after the wait is over, the task is parked and then resumed by the handler
    const EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, wait_ticks);

    g_RelaxCpuHandler = prev_handler;

    const SuspendDeferredWaitContext &ctx = g_SuspendDeferredWaitContext;

    // the suspension was deferred and left the wait untouched
    CHECK_EQUAL(SUSPEND_DEFERRED, ctx.suspend_result);
    CHECK_EQUAL(TASK_STATE_WAITING, ctx.waiting.state);
    CHECK_TRUE(ctx.waiting.wait_obj == &sobj);

    // the wait ended (timeout, signal or cancel), the task did not run on but was parked
    CHECK_TRUE(ctx.parked_seen);
    CHECK_TRUE(ctx.parked_at > 2);
    CHECK_TRUE(ctx.parked.wait_obj == nullptr);
    CHECK_TRUE(ctx.still_parked);
    CHECK_EQUAL(RESUME_DONE, ctx.resume_result);

    // the result of the wait is preserved across the suspension
    CHECK_EQUAL(expected, wresult);

    CheckTaskState(GetTaskState(&kernel, &task), TASK_STATE_READY);
}

TEST(Kernel, SuspendDeferredWaitingTimeout)
{
    Test_SuspendDeferredWaiting(4, false, false, WAIT_RESULT_TIMEOUT);
}

TEST(Kernel, SuspendDeferredWaitingSignaled)
{
    Test_SuspendDeferredWaiting(10, true, false, WAIT_RESULT_SIGNAL);
}

TEST(Kernel, SuspendDeferredWaitingInfiniteSignaled)
{
    Test_SuspendDeferredWaiting(WAIT_INFINITE, true, false, WAIT_RESULT_SIGNAL);
}

TEST(Kernel, SuspendDeferredWaitingCanceled)
{
    Test_SuspendDeferredWaiting(WAIT_INFINITE, false, true, WAIT_RESULT_CANCELED);
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

/*! \class StrategyDeadlineMissedMock
    \brief Round-Robin strategy (by delegation) which reports DEADLINE_MISSED_API = 1 and whose
           OnTaskDeadlineMissed() result is controlled by the test.
*/
class StrategyDeadlineMissedMock final : public ITaskSwitchStrategy
{
public:
    enum EConfig
    {
        WEIGHT_API               = 0,
        SLEEP_EVENT_API          = 1,
        DEADLINE_MISSED_API      = 1,
        PRIORITY_INHERITANCE_API = 0,
        NOSLEEP_YIELD_API        = 0,
        TIMESLICE_API            = 0
    };

    StrategyDeadlineMissedMock() : m_rr(), m_recover(false), m_missed_calls(0), m_missed_task(nullptr)
    {}

    void         AddTask(IKernelTask *task) override     { m_rr.AddTask(task); }
    void         RemoveTask(IKernelTask *task) override  { m_rr.RemoveTask(task); }
    IKernelTask *GetFirst() override                     { return m_rr.GetFirst(); }
    IKernelTask *GetNext() override                      { return m_rr.GetNext(); }
    size_t       GetSize() const override                { return m_rr.GetSize(); }
    void         OnTaskSleep(IKernelTask *task) override { m_rr.OnTaskSleep(task); }
    void         OnTaskWake(IKernelTask *task) override  { m_rr.OnTaskWake(task); }

    bool OnTaskDeadlineMissed(IKernelTask *task) override
    {
        ++m_missed_calls;
        m_missed_task = task;
        return m_recover;
    }

    SwitchStrategyRR m_rr;
    bool             m_recover;      //!< value returned by OnTaskDeadlineMissed()
    uint32_t         m_missed_calls; //!< number of OnTaskDeadlineMissed() invocations
    IKernelTask     *m_missed_task;  //!< task passed to the last OnTaskDeadlineMissed()
};

// strategy could not absorb the overrun: the kernel must fall back to the HRT hard fault
TEST(Kernel, HrtTaskDeadlineMissedStrategyNotRecovered)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, StrategyDeadlineMissedMock, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    StrategyDeadlineMissedMock *strategy = static_cast<StrategyDeadlineMissedMock *>(kernel.GetSwitchStrategy());

    strategy->m_recover = false;

    kernel.Initialize();
    kernel.AddTask(&task, 2, 1, 0);
    kernel.Start();

    // 1-st tick: duration (1) is within the deadline (1), strategy is not consulted
    platform->ProcessTick();
    CHECK_EQUAL(0, strategy->m_missed_calls);
    CHECK_FALSE(platform->m_hard_fault);

    // task does not Yield(): 2-nd tick goes outside the deadline
    g_TestContext.ExpectAssert(true);
    platform->ProcessTick();

    // strategy was asked first, refused, therefore the kernel hard-failed the task
    CHECK_EQUAL(1, strategy->m_missed_calls);
    CHECK_TRUE(strategy->m_missed_task != NULL);
    CHECK_EQUAL(&task, strategy->m_missed_task->GetUserTask());
    CHECK_TRUE(platform->m_hard_fault);
    CHECK_EQUAL(2, task.m_deadline_missed);
}

// strategy absorbed the overrun: no hard fault, OnDeadlineMissed() of the task is not called
TEST(Kernel, HrtTaskDeadlineMissedStrategyRecovered)
{
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT, 1, StrategyDeadlineMissedMock, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    StrategyDeadlineMissedMock *strategy = static_cast<StrategyDeadlineMissedMock *>(kernel.GetSwitchStrategy());

    strategy->m_recover = true;

    kernel.Initialize();
    kernel.AddTask(&task, 2, 1, 0);
    kernel.Start();

    platform->ProcessTick();
    CHECK_EQUAL(0, strategy->m_missed_calls);

    // 2-nd tick goes outside the deadline
    platform->ProcessTick();

    CHECK_EQUAL(1, strategy->m_missed_calls);
    CHECK_EQUAL(&task, strategy->m_missed_task->GetUserTask());
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

    const TId task1_id = task1.GetTid();

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

TEST(Kernel, SetWeightClearsInheritedWeight)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 2, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    TaskMockW<2, ACCESS_USER> task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    IKernelTask *ktasks[2];
    ArrayView<IKernelTask *> ktasks_view(ktasks, 2);
    kernel.EnumerateKernelTasks(ktasks_view);

    const TId task1_id = task1.GetTid();

    // task1 inherits a higher weight (priority inheritance boost)
    IKernelService::GetInstance()->InheritWeight(task1_id, 2);
    CHECK_EQUAL(2, ktasks[0]->GetCurrentWeight());
    CHECK_EQUAL(2, ktasks[0]->GetWeight());

    // new base weight (3) exceeds the inherited weight (2): inherited weight is no longer a boost and must be dropped
    const Weight prev_base = IKernelService::GetInstance()->SetWeight(task1_id, 3);

    CHECK_EQUAL(1, prev_base);
    CHECK_EQUAL(NO_WEIGHT, ktasks[0]->GetCurrentWeight());
    CHECK_EQUAL(3, ktasks[0]->GetBaseWeight());
    CHECK_EQUAL(3, ktasks[0]->GetWeight());
}

TEST(Kernel, SetWeightClearsInheritedWeightEqual)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 2, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    TaskMockW<2, ACCESS_USER> task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    IKernelTask *ktasks[2];
    ArrayView<IKernelTask *> ktasks_view(ktasks, 2);
    kernel.EnumerateKernelTasks(ktasks_view);

    const TId task1_id = task1.GetTid();

    IKernelService::GetInstance()->InheritWeight(task1_id, 2);
    CHECK_EQUAL(2, ktasks[0]->GetCurrentWeight());

    // new base weight equals the inherited weight: the boost is dropped too (<=)
    IKernelService::GetInstance()->SetWeight(task1_id, 2);

    CHECK_EQUAL(NO_WEIGHT, ktasks[0]->GetCurrentWeight());
    CHECK_EQUAL(2, ktasks[0]->GetWeight());
}

TEST(Kernel, SetWeightKeepsHigherInheritedWeight)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 2, SwitchStrategyFP32, PlatformTestMock> kernel;
    TaskMockW<1, ACCESS_USER> task1;
    TaskMockW<2, ACCESS_USER> task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    IKernelTask *ktasks[2];
    ArrayView<IKernelTask *> ktasks_view(ktasks, 2);
    kernel.EnumerateKernelTasks(ktasks_view);

    const TId task1_id = task1.GetTid();

    IKernelService::GetInstance()->InheritWeight(task1_id, 5);
    CHECK_EQUAL(5, ktasks[0]->GetCurrentWeight());

    // new base weight (3) is below the inherited weight (5): the boost stays active
    const Weight prev_base = IKernelService::GetInstance()->SetWeight(task1_id, 3);

    CHECK_EQUAL(1, prev_base);
    CHECK_EQUAL(5, ktasks[0]->GetCurrentWeight());
    CHECK_EQUAL(3, ktasks[0]->GetBaseWeight());
    CHECK_EQUAL(5, ktasks[0]->GetWeight());
}

// ---------------------------------------------------------------------------- //
// IKernelService::SetTimeSlice() (KERNEL_TIMESLICE)                             //
// ---------------------------------------------------------------------------- //

// task mock with a configurable time-slice quantum (ITask::GetTimeSlice)
template <Timeout TQuantum>
class TaskMockTS : public TaskMock<ACCESS_USER>
{
public:
    Timeout GetTimeSlice() const override { return TQuantum; }

    // priority == threshold: the task is not "held" by preemption-threshold (held tasks are never
    // time-sliced), all TaskMockTS tasks share one priority level and therefore rotate
    Weight GetWeight() const override { return SwitchStrategyPT32_TS::MakeWeight(1, 1); }
};

typedef Kernel<KERNEL_STATIC | KERNEL_TIMESLICE, 2, SwitchStrategyPT32_TS, PlatformTestMock> KernelTsType;

TEST(Kernel, SetTimeSliceReturnsPreviousQuantum)
{
    KernelTsType kernel;
    TaskMockTS<5> task1;
    TaskMockTS<NO_TIME_SLICE> task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    const TId task1_id = task1.GetTid();
    const TId task2_id = task2.GetTid();

    // initial quantum comes from ITask::GetTimeSlice()
    CHECK_EQUAL(5, IKernelService::GetInstance()->SetTimeSlice(task1_id, 3));

    // previous value is the one set last (can be passed back to restore)
    CHECK_EQUAL(3, IKernelService::GetInstance()->SetTimeSlice(task1_id, NO_TIME_SLICE));
    CHECK_EQUAL(NO_TIME_SLICE, IKernelService::GetInstance()->SetTimeSlice(task1_id, 5));

    // task without time-slicing: previous quantum is NO_TIME_SLICE
    CHECK_EQUAL(NO_TIME_SLICE, IKernelService::GetInstance()->SetTimeSlice(task2_id, 2));
    CHECK_EQUAL(2, IKernelService::GetInstance()->SetTimeSlice(task2_id, NO_TIME_SLICE));
}

TEST(Kernel, SetTimeSliceHelperCallingTask)
{
    KernelTsType kernel;
    TaskMockTS<4> task1;
    TaskMockTS<NO_TIME_SLICE> task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // task1 is the running task after Start(): stk::SetTimeSlice(ticks) resolves it via GetTid()
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);
    CHECK_EQUAL(4, stk::SetTimeSlice(7));
    CHECK_EQUAL(7, stk::SetTimeSlice(task1.GetTid(), 4));
}

TEST(Kernel, SetTimeSliceRotatesOnExpiry)
{
    KernelTsType kernel;
    TaskMockTS<NO_TIME_SLICE> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // no slicing: task1 keeps the CPU
    platform->ProcessTick();
    platform->ProcessTick();
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // enable a 2 tick quantum at run-time
    CHECK_EQUAL(NO_TIME_SLICE, IKernelService::GetInstance()->SetTimeSlice(task1.GetTid(), 2));

    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP); // 1 tick left

    platform->ProcessTick();
    CHECK_EQUAL((Word)task2.GetStack(), active->SP); // slice expired: rotated behind peer
}

TEST(Kernel, SetTimeSliceReloadsSlice)
{
    KernelTsType kernel;
    TaskMockTS<3> task1;
    TaskMockTS<NO_TIME_SLICE> task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    // consume 2 of 3 ticks of the slice
    platform->ProcessTick();
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // setting the quantum (even to the same value) restarts the slice
    CHECK_EQUAL(3, IKernelService::GetInstance()->SetTimeSlice(task1.GetTid(), 3));

    platform->ProcessTick();
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP); // would have rotated without the reload

    platform->ProcessTick();
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
}

TEST(Kernel, SetTimeSliceDisableStopsRotation)
{
    KernelTsType kernel;
    TaskMockTS<2> task1;
    TaskMockTS<NO_TIME_SLICE> task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    platform->ProcessTick();

    // disable slicing before the slice expires
    CHECK_EQUAL(2, IKernelService::GetInstance()->SetTimeSlice(task1.GetTid(), NO_TIME_SLICE));

    platform->ProcessTick();
    platform->ProcessTick();
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);
}

TEST(Kernel, SetTimeSliceNotSupportedWithoutMode)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    const TId tid = task.GetTid();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->SetTimeSlice(tid, 1);
        CHECK_TEXT(false, "SetTimeSlice requires KERNEL_TIMESLICE");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    // release build behavior: returns NO_TIME_SLICE
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);
    CHECK_EQUAL(NO_TIME_SLICE, IKernelService::GetInstance()->SetTimeSlice(tid, 1));
    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
}

TEST(Kernel, SetTimeSliceNegativeFails)
{
    KernelTsType kernel;
    TaskMockTS<2> task1;
    TaskMockTS<2> task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->SetTimeSlice(task1.GetTid(), -1);
        CHECK_TEXT(false, "negative quantum must not be accepted");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}

TEST(Kernel, SetTimeSliceUnknownTaskFails)
{
    KernelTsType kernel;
    TaskMockTS<2> task1, task2;
    TaskMockTS<2> not_added;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->SetTimeSlice(not_added.GetTid(), 1);
        CHECK_TEXT(false, "task is not known to the kernel");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    try
    {
        g_TestContext.ExpectAssert(true);
        IKernelService::GetInstance()->SetTimeSlice(TID_NONE, 1);
        CHECK_TEXT(false, "TID_NONE is not a valid task id");
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }
}


TEST(Kernel, SetTimeSliceSlotReuseAfterRemoveTask)
{
    // RemoveTask() unbinds the slot (TsInfo::Clear()), a new task must get the slot with its own quantum
    Kernel<KERNEL_DYNAMIC | KERNEL_TIMESLICE, 2, SwitchStrategyPT32_TS, PlatformTestMock> kernel;
    TaskMockTS<3> task1;
    TaskMockTS<NO_TIME_SLICE> task2, task3;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);

    // free the slot of task1 (had a 3 tick quantum) and re-use it for task3 (no time-slicing)
    kernel.RemoveTask(&task1);
    CHECK_EQUAL(1, kernel.GetSwitchStrategy()->GetSize());

    kernel.AddTask(&task3);
    CHECK_EQUAL(2, kernel.GetSwitchStrategy()->GetSize());

    kernel.Start();

    // either of the two tasks may run first (depends on the order of the strategy), find which one
    CHECK_TRUE((active->SP == (Word)task2.GetStack()) || (active->SP == (Word)task3.GetStack()));
    TaskMockTS<NO_TIME_SLICE> &running = ((active->SP == (Word)task2.GetStack()) ? task2 : task3);
    TaskMockTS<NO_TIME_SLICE> &other   = ((&running == &task2) ? task3 : task2);

    // nobody has a quantum: no rotation, nothing is inherited from the removed task1
    for (int32_t i = 0; i < 6; ++i)
        platform->ProcessTick();

    CHECK_EQUAL((Word)running.GetStack(), active->SP);

    // slots of both tasks (including the re-used one) report NO_TIME_SLICE as the previous quantum
    CHECK_EQUAL(NO_TIME_SLICE, IKernelService::GetInstance()->SetTimeSlice(other.GetTid(), 2));
    CHECK_EQUAL(NO_TIME_SLICE, IKernelService::GetInstance()->SetTimeSlice(running.GetTid(), 2));

    // the running task now rotates to the other one after 2 ticks
    platform->ProcessTick();
    CHECK_EQUAL((Word)running.GetStack(), active->SP);

    platform->ProcessTick();
    CHECK_EQUAL((Word)other.GetStack(), active->SP);
}

typedef KernelMock<KERNEL_STATIC | KERNEL_TIMESLICE, 2, SwitchStrategyPT32_TS, PlatformTestMock> KernelTsMock;

static struct TimesliceYieldRelaxCpuContext
{
    TimesliceYieldRelaxCpuContext()
    {
        Clear();
    }

    void Clear()
    {
        counter = 0;
        kernel  = NULL;
        tid     = TID_NONE;
        idle    = NULL;
        active  = NULL;
    }

    uint32_t       counter;
    KernelTsMock  *kernel;
    TId            tid;    //!< id of the yielding task
    Stack         *idle;   //!< stack switched out by the consumed yield
    Stack         *active; //!< stack switched in by the consumed yield

    // busy-wait of KernelTask::BusyWaitWhileYielding(): the pending yield is consumed by the ISR
    // ForceContextSwitch path WITHOUT a tick, thus the yield itself does not charge the time slice
    void Process()
    {
        if (counter == 0)
        {
            CHECK_TRUE(kernel->CallForceContextSwitch(tid, idle, active));
        }

        ++counter;
    }
}
g_TimesliceYieldRelaxCpuContext;

static void TimesliceYieldRelaxCpu()
{
    g_TimesliceYieldRelaxCpuContext.Process();
}

TEST(Kernel, SetTimeSliceYieldRestartsSlice)
{
    // KernelTask::ScheduleYield() reloads the slice (tx_thread_relinquish semantics): a task which yielded
    // starts its next run with a full quantum
    KernelTsMock kernel;
    TaskMockTS<3> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&idle = platform->m_stack_idle, *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // consume 2 of 3 ticks of task1 slice: 1 tick is left
    platform->ProcessTick();
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    g_TimesliceYieldRelaxCpuContext.Clear();
    g_TimesliceYieldRelaxCpuContext.kernel = &kernel;
    g_TimesliceYieldRelaxCpuContext.tid    = task1.GetTid();
    g_RelaxCpuHandler = TimesliceYieldRelaxCpu;

    // task1 yields to its peer: the slice of task1 is restarted
    Yield();
    CHECK_EQUAL(1, g_TimesliceYieldRelaxCpuContext.counter);

    // the yield is consumed by the ISR path which returns the stacks (the mock platform is not updated by it)
    CHECK_EQUAL((Word)task1.GetStack(), g_TimesliceYieldRelaxCpuContext.idle->SP);
    CHECK_EQUAL((Word)task2.GetStack(), g_TimesliceYieldRelaxCpuContext.active->SP);

    // apply the switch to the mock platform (a real platform driver does it on the context switch), otherwise
    // the next Yield() would be attributed to task1 by its stack pointer
    idle   = g_TimesliceYieldRelaxCpuContext.idle;
    active = g_TimesliceYieldRelaxCpuContext.active;

    // task2 yields back to task1 (this does not touch the slice of task1)
    g_TimesliceYieldRelaxCpuContext.Clear();
    g_TimesliceYieldRelaxCpuContext.kernel = &kernel;
    g_TimesliceYieldRelaxCpuContext.tid    = task2.GetTid();

    Yield();
    CHECK_EQUAL(1, g_TimesliceYieldRelaxCpuContext.counter);
    CHECK_EQUAL((Word)task2.GetStack(), g_TimesliceYieldRelaxCpuContext.idle->SP);
    CHECK_EQUAL((Word)task1.GetStack(), g_TimesliceYieldRelaxCpuContext.active->SP);

    idle   = g_TimesliceYieldRelaxCpuContext.idle;
    active = g_TimesliceYieldRelaxCpuContext.active;

    g_RelaxCpuHandler = NULL;

    // task1 has a full 3 tick slice again (without the reload on yield it would expire on the 1st tick)
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // slice expired: rotated behind the peer
    platform->ProcessTick();
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
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

// ---------------------------------------------------------------------------- //
// IKernel::SetSchedulerLock(), IKernelService::LockScheduler() / UnlockScheduler() //
// / RestoreSchedulerLock() (CMSIS-RTOS2 osKernelLock family)                       //
// ---------------------------------------------------------------------------- //

// returns true if the call raised an assertion (debug-build contract violation)
template <typename TCall>
static bool LockCallAsserts(TCall call)
{
    bool asserted = false;

    try
    {
        g_TestContext.ExpectAssert(true);
        call();
    }
    catch (TestAssertPassed &pass)
    {
        asserted = true;
    }

    g_TestContext.ExpectAssert(false);

    return asserted;
}

// runs the call the way a release build does: the assertion is swallowed and the call returns
template <typename TCall>
static void LockCallNoRethrow(TCall call)
{
    g_TestContext.ExpectAssert(true);
    g_TestContext.RethrowAssertException(false);

    call();

    g_TestContext.RethrowAssertException(true);
    g_TestContext.ExpectAssert(false);
}

TEST(Kernel, LockUnlockResultAndState)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());

    // unlocked -> locked
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

    // not nested: locking again reports the previous (locked) state and changes nothing
    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->LockScheduler());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

    // a single Unlock() releases the lock
    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());

    // unlocking an unlocked kernel is harmless and reports the previous (unlocked) state
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->UnlockScheduler());
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());

    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released");
}

TEST(Kernel, LockUnlockScoped)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());

    // scope
    {
        const stk::ScopedSchedulerLock lock;

        // unlocked -> locked on construction
        CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

        // do critical stuff on memory shared between tasks
    }

    // became unlocked on scope leave
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());

    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released");
}

// scopes nest: the outer lock stays held when an inner scope is left
TEST(Kernel, LockUnlockScopedNested)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    {
        const stk::ScopedSchedulerLock outer;
        CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

        {
            const stk::ScopedSchedulerLock inner; // found the scheduler locked
            CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
        }

        // inner scope left: the outer lock is still held
        CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
    }

    // outermost scope left: unlocked
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());
    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released");
}

// a scope inside a manually taken lock (LockScheduler) must not release that lock
TEST(Kernel, LockUnlockScopedInsideManualLock)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    IKernelService *const svc = IKernelService::GetInstance();

    const ELockResult prev = svc->LockScheduler();
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, prev);

    {
        const stk::ScopedSchedulerLock scope;
        CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
    }

    CHECK_EQUAL_TEXT(IKernel::KSTATE_LOCKED, kernel.GetState(), "scope must not release the outer manual lock");

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, svc->RestoreSchedulerLock(prev == LOCK_WAS_LOCKED));
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());
}

// a scope which could not take the lock (kernel not running) must not break the state afterwards
TEST(Kernel, LockUnlockScopedLockFailed)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);

    // KSTATE_READY: the lock can not be taken, the destructor restores "unlocked" which fails the same way
    {
        const stk::ScopedSchedulerLock scope;
        CHECK_EQUAL(IKernel::KSTATE_READY, kernel.GetState());
    }

    CHECK_EQUAL(IKernel::KSTATE_READY, kernel.GetState());
    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released");
}

TEST(Kernel, LockFailsWhenKernelNotRunning)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    // KSTATE_INACTIVE
    CHECK_EQUAL(LOCK_FAIL, kernel.SetSchedulerLock(true));
    CHECK_EQUAL(LOCK_FAIL, kernel.SetSchedulerLock(false));
    CHECK_EQUAL(IKernel::KSTATE_INACTIVE, kernel.GetState());

    kernel.Initialize();
    kernel.AddTask(&task);

    IKernelService *const svc = IKernelService::GetInstance();

    // KSTATE_READY (not started)
    CHECK_EQUAL(LOCK_FAIL, svc->LockScheduler());
    CHECK_EQUAL(LOCK_FAIL, svc->UnlockScheduler());
    CHECK_EQUAL(LOCK_FAIL, svc->RestoreSchedulerLock(false));
    CHECK_EQUAL(IKernel::KSTATE_READY, kernel.GetState());
}

TEST(Kernel, LockFailsFromIsr)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    IKernelService *const svc = IKernelService::GetInstance();

    // g_InsideISR is reset before any check: a failing CHECK throws and must not leave the flag set
    g_InsideISR = true;

    const ELockResult lock    = svc->LockScheduler();
    const ELockResult unlock  = svc->UnlockScheduler();
    const ELockResult restore = svc->RestoreSchedulerLock(true);

    g_InsideISR = false;

    CHECK_EQUAL(LOCK_FAIL_ISR, lock);
    CHECK_EQUAL(LOCK_FAIL_ISR, unlock);
    CHECK_EQUAL(LOCK_FAIL_ISR, restore);
    CHECK_EQUAL_TEXT(IKernel::KSTATE_RUNNING, kernel.GetState(), "ISR must not change the lock state");

    // a lock taken in task context cannot be released from an ISR
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    g_InsideISR = true;
    const ELockResult isr_unlock = IKernelService::GetInstance()->UnlockScheduler();
    g_InsideISR = false;

    CHECK_EQUAL(LOCK_FAIL_ISR, isr_unlock);
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released");
}

TEST(Kernel, LockFailsInHrtMode)
{
    Kernel<KERNEL_STATIC | KERNEL_HRT, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task, 1, 1, 0);
    kernel.Start();

    IKernelService *const svc = IKernelService::GetInstance();

    // holding the CPU would break periodic deadlines
    CHECK_EQUAL(LOCK_FAIL, svc->LockScheduler());
    CHECK_EQUAL(LOCK_FAIL, svc->UnlockScheduler());
    CHECK_EQUAL(LOCK_FAIL, svc->RestoreSchedulerLock(true));
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());
}

TEST(Kernel, RestoreLock)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    IKernelService *const svc = IKernelService::GetInstance();

    // RestoreSchedulerLock() reports the NEW state (unlike LockScheduler/UnlockScheduler which report the previous one)
    CHECK_EQUAL(LOCK_WAS_LOCKED, svc->RestoreSchedulerLock(true));
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

    CHECK_EQUAL(LOCK_WAS_LOCKED, svc->RestoreSchedulerLock(true)); // idempotent
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, svc->RestoreSchedulerLock(false));
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, svc->RestoreSchedulerLock(false)); // idempotent
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());
}

// nesting emulation as done with CMSIS osKernelLock()/osKernelRestoreLock()
TEST(Kernel, RestoreLockNesting)
{
    Kernel<KERNEL_STATIC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    IKernelService *const svc = IKernelService::GetInstance();

    // outer section: kernel was unlocked
    const ELockResult outer = svc->LockScheduler();
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, outer);

    // inner section (e.g. a callee): kernel was already locked
    const ELockResult inner = svc->LockScheduler();
    CHECK_EQUAL(LOCK_WAS_LOCKED, inner);

    // leaving the inner section restores "locked": the outer section stays protected
    CHECK_EQUAL(LOCK_WAS_LOCKED, svc->RestoreSchedulerLock(inner == LOCK_WAS_LOCKED));
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

    // leaving the outer section restores "unlocked"
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, svc->RestoreSchedulerLock(outer == LOCK_WAS_LOCKED));
    CHECK_EQUAL(IKernel::KSTATE_RUNNING, kernel.GetState());
}

TEST(Kernel, LockedTickDoesNotSwitchContext)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    const Ticks ticks_before = IKernelService::GetInstance()->GetTicks();
    const uint32_t switches_before = platform->m_context_switch_nr;

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    // RR would rotate on every tick: while locked task1 keeps the CPU
    platform->ProcessTick();
    platform->ProcessTick();
    platform->ProcessTick();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);
    CHECK_EQUAL(switches_before, platform->m_context_switch_nr);

    // time is not stopped by the lock
    CHECK_TRUE(IKernelService::GetInstance()->GetTicks() >= (ticks_before + 3));

    // released: the very next tick switches to the peer
    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());

    platform->ProcessTick();

    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
    CHECK_EQUAL(switches_before + 1, platform->m_context_switch_nr);
}

static PlatformTestMock *g_LockDelayPlatform = NULL;

static void LockDelayRelaxCpu()
{
    g_LockDelayPlatform->ProcessTick();
}

// Delay() is a busy-wait: it is allowed under the lock, ticks keep running and the holder keeps the CPU
TEST(Kernel, LockedDelayAllowed)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    g_LockDelayPlatform = platform;
    g_RelaxCpuHandler   = LockDelayRelaxCpu;

    const Ticks ticks_before = IKernelService::GetInstance()->GetTicks();

    stk::Delay(3);

    g_RelaxCpuHandler   = NULL;
    g_LockDelayPlatform = NULL;

    CHECK_TRUE(IKernelService::GetInstance()->GetTicks() >= (ticks_before + 3));
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);
}

TEST(Kernel, LockedSleepIsNoop)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    IKernelTask *ktask1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    ktask1 = kernel.GetSwitchStrategy()->GetFirst();
    CHECK_TRUE(ktask1 != NULL);
    CHECK_EQUAL(&task1, ktask1->GetUserTask());

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    // debug build: contract violation asserts
    CHECK_TRUE(LockCallAsserts([]() { stk::Sleep(5); }));
    CHECK_TRUE(LockCallAsserts([]() { stk::SleepMs(5); }));

    // release build: no-op, the holder is not put to sleep and keeps the lock
    LockCallNoRethrow([]() { stk::Sleep(5); });
    LockCallNoRethrow([]() { stk::SleepMs(5); });

    CHECK_FALSE(ktask1->IsSleeping());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);
}

TEST(Kernel, HolderShallNotSelfSuspendWhenKernelLocked)
{
    KernelMock<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *idle = nullptr, *active = nullptr;
    const TId task1_id = task1.GetTid();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

    // task1 (lock holder) is going away: pending sleep, busy-waiting for the ISR to de-schedule it
    try
    {
        g_TestContext.ExpectAssert(true);
        kernel.SuspendTask(&task1);
        CHECK(false);
    }
    catch (TestAssertPassed &pass)
    {
        CHECK(true);
        g_TestContext.ExpectAssert(false);
    }

    const bool result = kernel.CallForceContextSwitch(task1_id, idle, active);

    CHECK_FALSE(result);
    CHECK_EQUAL_TEXT(IKernel::KSTATE_LOCKED, kernel.GetState(), "lock must remain locked");
    CHECK_TRUE(idle == NULL);
    CHECK_TRUE(active == NULL);

    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());

    platform->ProcessTick();
    CHECK_EQUAL((Word)task2.GetStack(), platform->m_stack_active->SP);
}

TEST(Kernel, LockedSleepUntilIsNoop)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    IKernelTask *ktask1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    ktask1 = kernel.GetSwitchStrategy()->GetFirst();
    CHECK_TRUE(ktask1 != NULL);

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    CHECK_TRUE(LockCallAsserts([]() { stk::SleepUntil(stk::GetTicks() + 5); }));

    // release build: reported as "did not sleep"
    bool slept = true;
    LockCallNoRethrow([&]() { slept = stk::SleepUntil(stk::GetTicks() + 5); });

    CHECK_FALSE(slept);
    CHECK_FALSE(ktask1->IsSleeping());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
}

TEST(Kernel, LockedYieldIsNoop)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    IKernelTask *ktask1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    ktask1 = kernel.GetSwitchStrategy()->GetFirst();
    CHECK_TRUE(ktask1 != NULL);

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    CHECK_TRUE(LockCallAsserts([]() { stk::Yield(); }));

    // release build: the platform is never asked to switch, task is not put to sleep
    LockCallNoRethrow([]() { stk::Yield(); });

    CHECK_EQUAL_TEXT(0, platform->m_switch_to_next_nr, "Yield must not reach the platform while locked");
    CHECK_FALSE(ktask1->IsSleeping());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
}

TEST(Kernel, LockedWaitFails)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task;
    MutexMock mutex;
    SyncObjectMock sobj;
    IKernelTask *ktask;

    kernel.Initialize();
    kernel.AddTask(&task);
    kernel.Start();

    ktask = kernel.GetSwitchStrategy()->GetFirst();
    CHECK_TRUE(ktask != NULL);

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    CHECK_TRUE(LockCallAsserts([&]() { IKernelService::GetInstance()->Wait(&sobj, &mutex, 5); }));

    // release build: fails without blocking (nobody else could ever wake the holder)
    EWaitResult wresult = WAIT_RESULT_SIGNAL;
    LockCallNoRethrow([&]() { wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 5); });

    CHECK_EQUAL(WAIT_RESULT_FAIL, wresult);
    CHECK_FALSE(ktask->IsSleeping());
    CHECK_FALSE(mutex.m_locked); // mutex untouched: not unlocked/re-locked by a wait
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
}

TEST(Kernel, LockedWaitAllowedAfterUnlock)
{
    Kernel<KERNEL_STATIC | KERNEL_SYNC, 1, SwitchStrategyRR, PlatformTestMock> kernel;
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

    // lock/unlock pair leaves no trace: blocking works again
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());
    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());

    MutexMock::ScopedLock guard(mutex);

    const EWaitResult wresult = IKernelService::GetInstance()->Wait(&sobj, &mutex, 2);

    g_RelaxCpuHandler = NULL;

    CHECK_EQUAL(WAIT_RESULT_TIMEOUT, wresult);
}

// The only way the lock holder can leave the CPU is to exit (ExitTask() or returning from Run()): Sleep(), SleepUntil(),
// Yield(), Wait() and SuspendTask(self) are refused while locked. The exit must release the lock, not leak it.
TEST(Kernel, LockReleasedWhenHolderExits)
{
    KernelMock<KERNEL_DYNAMIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *idle = nullptr, *active = nullptr;
    const TId task1_id = task1.GetTid();

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());

    // task1 (lock holder) exits: it is marked sleeping + pending for removal and then busy-waits for the ISR
    // to de-schedule it (same path as ExitTask() / returning from ITask::Run())
    Stack *const holder_stack = platform->m_stack_active;

    // debug build: exiting while holding the lock is a contract violation. Unlike Sleep/Yield/Wait it can not be
    // refused (ExitTask() does not return), the assertion fires before the exit is recorded
    CHECK_TRUE(LockCallAsserts([&]() { platform->EventTaskExit(holder_stack); }));

    // release build: the exit proceeds
    LockCallNoRethrow([&]() { platform->EventTaskExit(holder_stack); });

    CHECK_EQUAL_TEXT(IKernel::KSTATE_LOCKED, kernel.GetState(), "lock is released only when the task leaves the CPU");

    const bool result = kernel.CallForceContextSwitch(task1_id, idle, active);

    CHECK_TRUE(result);
    CHECK_EQUAL_TEXT(IKernel::KSTATE_RUNNING, kernel.GetState(), "lock must not leak to other tasks");
    CHECK_TRUE(idle != NULL);
    CHECK_TRUE(active != NULL);
    CHECK_EQUAL((Word)task1.GetStack(), idle->SP);
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);

    // scheduling is alive again (not stuck behind a stale lock): the tick completes the removal of task1
    // (note: its Stack slot is unbound by the removal, so platform->m_stack_active must not be dereferenced here)
    platform->ProcessTick();
    CHECK_EQUAL_TEXT(1, kernel.GetSwitchStrategy()->GetSize(), "exited holder must be removed");
    CHECK_EQUAL(&task2, kernel.GetSwitchStrategy()->GetFirst()->GetUserTask());

    // the leaked state is gone, the lock can be taken again by the new running task
    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());
}

// a forced switch request of the lock holder which is NOT leaving the CPU keeps the lock
TEST(Kernel, ForceContextSwitchNotSleepingKeepsLock)
{
    KernelMock<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    Stack *idle = nullptr, *active = nullptr;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    CHECK_FALSE(kernel.CallForceContextSwitch(task1.GetTid(), idle, active));
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
}

// the lock holder must not suspend itself: asserts in debug, no-op in release (the task keeps the CPU and the lock)
TEST(Kernel, LockedSuspendSelfIsNoop)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    IKernelTask *ktask1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    ktask1 = kernel.GetSwitchStrategy()->GetFirst();
    CHECK_TRUE(ktask1 != NULL);
    CHECK_EQUAL(&task1, ktask1->GetUserTask());

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    // task1 is the running task and the lock holder
    CHECK_TRUE(LockCallAsserts([&]() { kernel.SuspendTask(&task1); }));

    // release build: nothing happens and the call does not spin waiting to be switched out
    ESuspendResult result = SUSPEND_DONE;
    LockCallNoRethrow([&]() { result = kernel.SuspendTask(&task1); });

    CHECK_EQUAL(SUSPEND_FAIL, result);
    CHECK_FALSE(ktask1->IsSleeping());
    CHECK_EQUAL(IKernel::KSTATE_LOCKED, kernel.GetState());
    CHECK_EQUAL((Word)task1.GetStack(), platform->m_stack_active->SP);
    CHECK_EQUAL_TEXT(0, g_CriticalSectionState, "critical section must be released");

    // the task was not left half-suspended: after Unlock() it is an ordinary runnable task
    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());
    CHECK_EQUAL(SUSPEND_DONE, kernel.SuspendTask(&task2)); // task2 can still be suspended, i.e. the kernel state is sane
}

// the holder may suspend ANOTHER task: the lock stays, the suspension takes effect once task switching is possible
TEST(Kernel, LockedSuspendOtherTaskAllowed)
{
    Kernel<KERNEL_STATIC, 2, SwitchStrategyRR, PlatformTestMock> kernel;
    TaskMock<ACCESS_USER> task1, task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    CHECK_EQUAL(SUSPEND_DONE, kernel.SuspendTask(&task2));
    CHECK_EQUAL_TEXT(IKernel::KSTATE_LOCKED, kernel.GetState(), "suspending another task must not drop the lock");
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());

    // task2 is suspended: task1 stays the only runnable task
    platform->ProcessTick();
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    CHECK_EQUAL(RESUME_DONE, kernel.ResumeTask(&task2));
}

#if STK_TICKLESS_IDLE
// tickless Suspend() while locked: scheduling suspension takes over, lock can not be taken while suspended
TEST(Kernel, LockSuspendedKernel)
{
    Kernel<KERNEL_STATIC | KERNEL_TICKLESS, 1, SwitchStrategyRR, PlatformTestMock> kernel;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    TaskMock<ACCESS_PRIVILEGED> task1;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.Start();

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    platform->m_sleep_ticks = 9;

    const Timeout ticks = stk::IKernelService::GetInstance()->Suspend();

    CHECK_EQUAL(IKernel::KSTATE_SUSPENDED, kernel.GetState());

    // KSTATE_SUSPENDED: Lock/Unlock are not available
    CHECK_EQUAL(LOCK_FAIL, IKernelService::GetInstance()->LockScheduler());
    CHECK_EQUAL(LOCK_FAIL, IKernelService::GetInstance()->UnlockScheduler());

    stk::IKernelService::GetInstance()->Resume(ticks);
}
#endif

TEST(Kernel, LockedTimeSliceIsChargedButNotRotated)
{
    KernelTsType kernel;
    TaskMockTS<2> task1;
    TaskMockTS<NO_TIME_SLICE> task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    // 3 ticks > 2 tick quantum: the slice is expired but rotation is deferred while locked
    platform->ProcessTick();
    platform->ProcessTick();
    platform->ProcessTick();

    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // elapsed ticks were not lost: on the first tick after Unlock() the expired slice rotates the task
    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());

    platform->ProcessTick();

    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
}

TEST(Kernel, LockedTimeSliceNotExpiredKeepsRunningAfterUnlock)
{
    KernelTsType kernel;
    TaskMockTS<4> task1;
    TaskMockTS<NO_TIME_SLICE> task2;
    PlatformTestMock *platform = static_cast<PlatformTestMock *>(kernel.GetPlatform());
    Stack *&active = platform->m_stack_active;

    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.Start();

    CHECK_EQUAL(LOCK_WAS_UNLOCKED, IKernelService::GetInstance()->LockScheduler());

    platform->ProcessTick();
    platform->ProcessTick();

    CHECK_EQUAL(LOCK_WAS_LOCKED, IKernelService::GetInstance()->UnlockScheduler());

    // 2 of 4 ticks were consumed under the lock: 1 tick left after the next one, no rotation yet
    platform->ProcessTick();
    CHECK_EQUAL((Word)task1.GetStack(), active->SP);

    // slice (4) expires on the 4-th charged tick in total
    platform->ProcessTick();
    CHECK_EQUAL((Word)task2.GetStack(), active->SP);
}

} // namespace stk
} // namespace test
