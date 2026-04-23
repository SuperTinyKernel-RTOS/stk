/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STKTEST_H_
#define STKTEST_H_

#include <stdio.h>
#include <exception>

// lib: cpputest
#include <CppUTest/TestHarness.h>

//! __stk_relax_cpu handler.
extern void (* g_RelaxCpuHandler)();

//! __stk_relax_cpu interceptor.
static inline void __stktest_relax_cpu()
{
    if (g_RelaxCpuHandler != NULL)
        g_RelaxCpuHandler();
}

// lib: stk
#ifndef _STK_UNDER_TEST
	#define _STK_UNDER_TEST
#endif
#define __stk_relax_cpu __stktest_relax_cpu
#include <stk_config.h>
#undef _STK_ARCH_ARM_CORTEX_M
#undef _STK_ARCH_RISC_V
#undef _STK_ARCH_X86_WIN32
#include <stk.h>
#include <arch/stk_arch_common.h>
#include <sync/stk_sync.h>
#include <time/stk_time.h>

#include "stktest_context.h"

namespace stk {

/*! \namespace stk::test
    \brief     Namespace of the test inventory.
 */
namespace test {

extern IKernelService *g_KernelService;

//! Critical section state.
extern int32_t g_CriticalSectionState;

//! ISR state.
extern bool g_InsideISR;

//! Panic value.
extern EKernelPanicId g_PanicValue;

/*! \class TestAssertPassed
    \brief Throwable class for catching assertions from STK_ASSERT_HANDLER().
*/
struct TestAssertPassed : public std::exception
{
    const char *what() const noexcept { return "STK test suite exception (TestAssertPassed) thrown!"; }
};

/*! \class PlatformTestMock
    \brief IPlatform mock.
*/
class PlatformTestMock : public IPlatform
{
public:
    struct StackInfo
    {
        Stack        *stack;
        IStackMemory *memory;
        ITask        *task;
    };

    explicit PlatformTestMock()
    {
        m_event_handler     = NULL;
        m_service           = NULL;
        m_started           = false;
        m_hard_fault        = false;
        m_switch_to_next_nr = 0;
        m_exit_trap         = NULL;
        m_fail_InitStack    = false;
        m_resolution        = 0;
        m_context_switch_nr = 0;
        m_ticks_count       = 0;
        m_stack_idle        = NULL;
        m_stack_active      = NULL;
        m_overrider         = NULL;
        m_systimer_count    = 0;
        m_systimer_freq     = 0;
        m_sleep_ticks       = 0;
    }

    virtual ~PlatformTestMock()
    {
        STK_ASSERT(m_service == g_KernelService);
        g_KernelService = NULL;
    }

    void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap) override
    {
        m_event_handler = event_handler;
        m_service       = service;
        m_started       = false;
        m_resolution    = resolution_us;
        m_exit_trap     = exit_trap;

        g_KernelService = service;
    }

    void Start() override
    {
        m_started = true;

        EventStart();
    }

    void Stop() override
    {
    	m_started = false;

    	m_event_handler->OnStop();
    }

    bool InitStack(EStackType type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) override
    {
        if (m_fail_InitStack)
            return false;

        // if NULL then it is Exit trap is being initialized
        m_stack_info[type].stack  = stack;
        m_stack_info[type].memory = stack_memory;
        m_stack_info[type].task   = user_task;

        // required to pass assertion checks when switching tasks
        PlatformContext::InitStackMemory(stack_memory);

        stack->SP = (size_t)stack_memory->GetStack();
        return true;
    }

    uint32_t GetTickResolution() const override
    {
        return m_resolution;
    }

    Cycles GetSysTimerCount() const override
    {
        return m_systimer_count;
    }

    uint32_t GetSysTimerFrequency() const override
    {
        return m_systimer_freq;
    }

    void SwitchToNext() override
    {
        m_event_handler->OnTaskSwitch(GetCallerSP());
        ++m_switch_to_next_nr;
    }

    void Sleep(Timeout ticks) override
    {
        m_event_handler->OnTaskSleep(GetCallerSP(), ticks);
    }

    void SleepUntil(Ticks timestamp) override
    {
        m_event_handler->OnTaskSleepUntil(GetCallerSP(), timestamp);
    }

    IWaitObject *Wait(ISyncObject *sobj, IMutex *mutex, Timeout timeout)
    {
        return m_event_handler->OnTaskWait(GetCallerSP(), sobj, mutex, timeout);
    }

    void ProcessHardFault() override
    {
        m_hard_fault = true;
    }

    void ProcessTick() override
    {
        Timeout ticks = 1;

        if (m_event_handler->OnTick(m_stack_idle, m_stack_active
        #if STK_TICKLESS_IDLE
            , ticks
        #endif
        ))
        {
            ++m_context_switch_nr;
        }

        m_ticks_count += ticks;
    }

    void SetEventOverrider(IEventOverrider *overrider) override
    {
        m_overrider = overrider;
    }

    // Events generated by test cases:

    void EventStart()
    {
        m_event_handler->OnStart(m_stack_active);
    }

    void EventTaskExit(Stack *stack)
    {
        m_event_handler->OnTaskExit(stack);
    }

    void EventTaskSwitch(size_t caller_SP)
    {
        m_event_handler->OnTaskSwitch(caller_SP);
    }

    void EventTaskSleep(size_t caller_SP, uint32_t sleep_ticks)
    {
        m_event_handler->OnTaskSleep(caller_SP, sleep_ticks);
    }

    IWaitObject *EventTaskWait(size_t caller_SP, ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
    {
        return m_event_handler->OnTaskWait(caller_SP, sync_obj, mutex, timeout);
    }

    size_t GetCallerSP() const override
    {
        return m_stack_active->SP;
    }

    virtual TId GetTid() const override
    {
        return m_event_handler->OnGetTid(GetCallerSP());
    }

    Timeout Suspend() override
    {
        m_event_handler->OnSuspend(true);
        return m_sleep_ticks;
    }

    void Resume(Timeout elapsed_ticks) override
    {
        m_event_handler->OnSuspend(false);
    }

    IKernelService  *m_service;
    Stack           *m_exit_trap;
    bool             m_fail_InitStack;
    int32_t          m_resolution;
    uint32_t         m_context_switch_nr;
    uint32_t         m_ticks_count;
    bool             m_started;
    bool             m_hard_fault;
    uint32_t         m_switch_to_next_nr;
    IEventOverrider *m_overrider;
    Stack           *m_stack_idle;
    Stack           *m_stack_active;
    StackInfo        m_stack_info[STACK_EXIT_TRAP + 1];
    uint64_t         m_systimer_count;
    uint32_t         m_systimer_freq;
    uint32_t         m_sleep_ticks;

protected:
    IEventHandler *m_event_handler;
};

/*! \class KernelServiceMock
    \brief IKernelService mock.
*/
class KernelServiceMock : public IKernelService
{
public:
    KernelServiceMock()
    {
        m_inc_ticks      = false;
        m_switch_to_next = false;
        m_ticks          = 0;
        m_resolution     = 0;
        m_tid            = 0;
        m_systimer_count = 0;
        m_systimer_freq  = 0;
    }
    virtual ~KernelServiceMock()
    {}

    size_t GetTid() const override
    {
        return m_tid;
    }

    int64_t GetTicks() const override
    {
        if (m_inc_ticks)
            const_cast<int64_t &>(m_ticks) = m_ticks + 1;

        return m_ticks;
    }

    uint32_t GetTickResolution() const override
    {
        return m_resolution;
    }

    Cycles GetSysTimerCount() const override
    {
        return m_systimer_count;
    }

    uint32_t GetSysTimerFrequency() const override
    {
        return m_systimer_freq;
    }

    void Delay(Timeout ticks) override
    {
        (void)ticks;
    }

    void Sleep(Timeout ticks) override
    {
        (void)ticks;
    }

    void SleepUntil(Ticks timestamp) override
    {
        (void)timestamp;
    }

    void SwitchToNext() override
    {
        m_switch_to_next = true;
    }

    IWaitObject *Wait(ISyncObject *sobj, IMutex *mutex, Timeout timeout) override
    {
        (void)sobj;
        (void)mutex;
        (void)timeout;
        return nullptr;
    }

    Timeout Suspend() override
    {
        return 1;
    }

    void Resume(Timeout elapsed_ticks) override
    {
        (void)elapsed_ticks;
    }

    void InheritWeight(TId tid, Weight weight) override
    {
        (void)tid;
        (void)weight;
    }

    void RestoreWeight(TId tid, ISyncObject *sobj) override
    {
        (void)tid;
        (void)sobj;
    }

    bool     m_inc_ticks;
    bool     m_switch_to_next;
    int64_t  m_ticks;
    int32_t  m_resolution;
    size_t   m_tid;
    uint64_t m_systimer_count;
    uint32_t m_systimer_freq;
};

/*! \class TaskMock
    \brief Task mock.
    \note  QEMU allocates small stack for the function, therefore stack size is limited to STACK_SIZE_MIN for tests to pass (256 was causing a hard fault).
*/
template <EAccessMode _AccessMode>
class TaskMock : public Task<STACK_SIZE_MIN, _AccessMode>
{
public:
    TaskMock() : m_deadline_missed(0)
    {}

    uint32_t m_deadline_missed; //!< duration of workload if deadline is missed in HRT mode

private:
    void Run() override {}

    void OnDeadlineMissed(uint32_t duration) override
    {
        // call base (to achieve full coverage)
        Task<STACK_SIZE_MIN, _AccessMode>::OnDeadlineMissed(duration);

        m_deadline_missed = duration;
    }
};

/*! \class TaskMockW
    \brief Task mock for SwitchStrategySmoothWeightedRoundRobin and similar algorithms.
    \note  QEMU allocates small stack for the function, therefore stack size is limited to STACK_SIZE_MIN for tests to pass (256 was causing a hard fault).
*/
template <Weight _Weight, EAccessMode _AccessMode>
class TaskMockW : public TaskW<_Weight, STACK_SIZE_MIN, _AccessMode>
{
private:
    void Run() override {}
};

struct MutexMock : public IMutex
{
    explicit MutexMock() : m_nesting(0), m_locked(false)
    {}

    uint32_t m_nesting;
    bool     m_locked;

    void Lock() override
    {
        if (m_nesting == 0)
            m_locked = true;

        ++m_nesting;
    }

    void Unlock() override
    {
        STK_ASSERT(m_nesting != 0);

        if (--m_nesting == 0)
            m_locked = false;
    }
};

struct SyncObjectMock : public ISyncObject
{
    void WakeOne() { ISyncObject::WakeOne(); }
    void WakeAll() { ISyncObject::WakeAll(); }
};

} // namespace test
} // namespace stk

#endif /* STKTEST_H_ */
