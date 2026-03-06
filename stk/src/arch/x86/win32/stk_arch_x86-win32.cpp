/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

// note: If missing, this header must be customized (get it in the root of the source folder) and
//       copied to the /include folder manually.
#include "stk_config.h"

#ifdef _STK_ARCH_X86_WIN32

#include "stk_arch.h"
#include "arch/stk_arch_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <list>
#include <vector>

#ifndef WINAPI
#define WINAPI __stdcall
#endif

typedef UINT MMRESULT;
typedef MMRESULT (WINAPI * timeBeginPeriodF)(UINT uPeriod);
static timeBeginPeriodF timeBeginPeriod = nullptr;

#define STK_X86_WIN32_CRITICAL_SECTION CRITICAL_SECTION
#define STK_X86_WIN32_CRITICAL_SECTION_INIT(SES) ::InitializeCriticalSection(SES)
#define STK_X86_WIN32_CRITICAL_SECTION_START(SES) ::EnterCriticalSection(SES)
#define STK_X86_WIN32_CRITICAL_SECTION_END(SES) ::LeaveCriticalSection(SES)
#define STK_X86_WIN32_MIN_RESOLUTION (1000)
#define STK_X86_WIN32_GET_SP(STACK) (STACK + 2) // +2 to overcome stack filler check inside Kernel (adjusting to +2 preserves 8-byte alignment)
#define SLK_UNLOCKED hw::SpinLock::UNLOCKED
#define SLK_LOCKED hw::SpinLock::LOCKED
#define STK_X86_WIN32_SPIN_LOCK_TRYLOCK(LOCK) \
    InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&(LOCK)), SLK_LOCKED, SLK_UNLOCKED)
#define STK_X86_WIN32_SPIN_LOCK_UNLOCK(LOCK) \
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&(LOCK)), SLK_UNLOCKED);

struct Win32ScopedCriticalSection
{
    STK_X86_WIN32_CRITICAL_SECTION &m_sec;

    explicit Win32ScopedCriticalSection(STK_X86_WIN32_CRITICAL_SECTION &sec) : m_sec(sec)
    {
        STK_X86_WIN32_CRITICAL_SECTION_START(&sec);
    }
    ~Win32ScopedCriticalSection()
    {
        STK_X86_WIN32_CRITICAL_SECTION_END(&m_sec);
    }
};

using namespace stk;

//! Internal context.
static struct Context : public PlatformContext
{
    void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap, int32_t resolution_us)
    {
        PlatformContext::Initialize(handler, service, exit_trap, resolution_us);

        m_overrider    = nullptr;
        m_sleep_trap   = nullptr;
        m_exit_trap    = nullptr;
        m_winmm_dll    = nullptr;
        m_timer_thread = nullptr;
        m_started      = false;
        m_stop_signal  = false;
        m_csu_nesting  = 0;
        m_timer_tid    = 0;

        if ((m_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES)
        {
            assert(false);
            return;
        }

        STK_X86_WIN32_CRITICAL_SECTION_INIT(&m_cs);

        LoadWindowsAPI();
    }
    ~Context()
    {
        if (m_tls != TLS_OUT_OF_INDEXES)
            TlsFree(m_tls);

        UnloadWindowsAPI();
    }

    void LoadWindowsAPI()
    {
        HMODULE winmm = GetModuleHandleA("Winmm");
        if (winmm == nullptr)
            m_winmm_dll = winmm = LoadLibraryA("Winmm.dll");
        assert(winmm != nullptr);

        timeBeginPeriod = (timeBeginPeriodF)GetProcAddress(winmm, "timeBeginPeriod");
        assert(timeBeginPeriod != nullptr);

        timeBeginPeriod(1);
    }

    void UnloadWindowsAPI()
    {
        if (m_winmm_dll != nullptr)
        {
            FreeLibrary(m_winmm_dll);
            m_winmm_dll = nullptr;
        }
    }

    struct TaskContext
    {
        TaskContext() : m_task(nullptr), m_stack(nullptr), m_thread(nullptr), m_thread_id(0)
        { }

        void Initialize(ITask *task, Stack *stack)
        {
            m_task      = task;
            m_stack     = stack;
            m_thread    = nullptr;
            m_thread_id = 0;

            InitThread();
        }

        void InitThread()
        {
            // simulate stack size limitation
            uint32_t stack_size = m_task->GetStackSize() * sizeof(Word);

            m_thread = CreateThread(nullptr, stack_size, &OnTaskRun, this, CREATE_SUSPENDED, &m_thread_id);
        }

        static DWORD WINAPI OnTaskRun(LPVOID param)
        {
            ((TaskContext *)param)->m_task->Run();
            return 0;
        }

        ITask *m_task;      //!< user task
        Stack *m_stack;     //!< user tasks's stack
        HANDLE m_thread;    //!< task's thread handle
        DWORD  m_thread_id; //!< task's thread id
    };

    bool InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task);
    void ConfigureTime();
    void StartActiveTask();
    void CreateTimerThreadAndJoin();
    void Cleanup();
    void ProcessTick();
    void SwitchContext();
    void SwitchToNext();
    void Sleep(Timeout ticks);
    IWaitObject *Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout);
    void Stop();
    Word GetCallerSP() const;
    TId GetTid() const;
    
    __stk_forceinline uintptr_t GetTls() 
    { 
        return reinterpret_cast<uintptr_t>(TlsGetValue(m_tls)); 
    }

    __stk_forceinline void SetTls(uintptr_t tp) 
    { 
        TlsSetValue(m_tls, reinterpret_cast<void *>(tp));
    }
    
    __stk_forceinline void EnterCriticalSection()
    {
        STK_X86_WIN32_CRITICAL_SECTION_START(&m_cs);

        if (m_csu_nesting == 0)
        {
            // avoid suspending self
            if (GetCurrentThreadId() != m_timer_tid)
                SuspendThread(m_timer_thread);
        }

        ++m_csu_nesting;
    }
    
    __stk_forceinline void ExitCriticalSection()
    {
        STK_ASSERT(m_csu_nesting != 0);

        --m_csu_nesting;

        if (m_csu_nesting == 0)
        {
            // suspending self is not supported
            if (GetCurrentThreadId() != m_timer_tid)
                ResumeThread(m_timer_thread);
        }

        STK_X86_WIN32_CRITICAL_SECTION_END(&m_cs);
    }

    IPlatform::IEventOverrider    *m_overrider;
    Stack                         *m_sleep_trap;
    Stack                         *m_exit_trap;
    HMODULE                        m_winmm_dll;     //!< Winmm.dll (loaded with LoadLibrary)
    HANDLE                         m_timer_thread;  //!< timer thread handle
    DWORD                          m_tls;           //!< TLS
    std::list<TaskContext *>       m_tasks;         //!< list of task internal contexts
    std::vector<HANDLE>            m_task_threads;  //!< task threads
    STK_X86_WIN32_CRITICAL_SECTION m_cs;            //!< critical session
    DWORD                          m_csu_nesting;   //!< depth of user critical session nesting
    DWORD                          m_timer_tid;     //!< timer thread id
    bool                           m_started;       //!< started state's flag
    volatile bool                  m_stop_signal;   //!< stop signal for a timer thread
}
g_Context;

__stk_attr_noinline  // keep out of inlining to preserve stack frame
__stk_attr_noreturn  // never returns - a trap
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    (void)id;

    // spin forever: without a watchdog, a debugger can attach and inspect 'id'
    for (;;)
    {
        __stk_relax_cpu();
    }
}

static DWORD WINAPI TimerThread(LPVOID param)
{
    (void)param;

    DWORD wait_ms = g_Context.m_tick_resolution / 1000;
    g_Context.m_timer_tid = GetCurrentThreadId();

    while (WaitForSingleObject(g_Context.m_timer_thread, wait_ms) == WAIT_TIMEOUT)
    {
        if (g_Context.m_stop_signal)
            break;

        g_Context.ProcessTick();
    }

    return 0;
}

void Context::ConfigureTime()
{
    // Windows timers are jittery, so make resolution more coarse
    if (m_tick_resolution < STK_X86_WIN32_MIN_RESOLUTION)
        m_tick_resolution = STK_X86_WIN32_MIN_RESOLUTION;

    // increase precision of ticks to at least 1 ms (although Windows timers will still be quite coarse and have jitter of +1 ms)
    timeBeginPeriod(1);
}

void Context::StartActiveTask()
{
    STK_ASSERT(m_stack_active != nullptr);
    TaskContext *active_task = hw::WordToPtr<TaskContext>(m_stack_active->SP);
    STK_ASSERT(active_task != nullptr);

    ResumeThread(active_task->m_thread);
}

void Context::CreateTimerThreadAndJoin()
{
    m_started = true;

    m_handler->OnStart(&m_stack_active);

    StartActiveTask();

    // create tick thread with highest priority
    m_timer_thread = CreateThread(nullptr, 0, &TimerThread, nullptr, 0, nullptr);
    SetThreadPriority(m_timer_thread, THREAD_PRIORITY_TIME_CRITICAL);

    while (!m_task_threads.empty())
    {
        DWORD result = WaitForMultipleObjects((DWORD)m_task_threads.size(), m_task_threads.data(), FALSE, INFINITE);
        STK_ASSERT(result != WAIT_TIMEOUT);
        STK_ASSERT(result != WAIT_ABANDONED);
        STK_ASSERT(result != WAIT_FAILED);

        Win32ScopedCriticalSection __cs(m_cs);

        uint32_t i = 0;
        for (std::vector<HANDLE>::iterator itr = m_task_threads.begin(); itr != m_task_threads.end(); ++itr)
        {
            if (result == (WAIT_OBJECT_0 + i))
            {
                TaskContext *exiting_task = nullptr;
                for (std::list<TaskContext *>::iterator titr = m_tasks.begin(); titr != m_tasks.end(); ++titr)
                {
                    if ((*titr)->m_thread == (*itr))
                    {
                        exiting_task = (*titr);
                        break;
                    }
                }
                STK_ASSERT(exiting_task != nullptr);

                if (exiting_task != nullptr)
                    m_handler->OnTaskExit(exiting_task->m_stack);

                m_task_threads.erase(itr);
                break;
            }

            ++i;
        }
    }

    // join (never returns to the caller from here unless thread is terminated, see KERNEL_DYNAMIC),
    // a stop signal is sent by IPlatform::Stop() by the last exiting task
    if (m_timer_thread != nullptr)
        WaitForSingleObject(m_timer_thread, INFINITE);
}

void Context::Cleanup()
{
    // close thread handles of all tasks
    for (std::list<TaskContext *>::iterator itr = m_tasks.begin(); itr != m_tasks.end(); ++itr)
    {
        if ((*itr)->m_thread != nullptr)
        {
            CloseHandle((*itr)->m_thread);
            (*itr)->m_thread = nullptr;
        }
    }
    m_tasks.clear();

    // close timer thread
    if (m_timer_thread != nullptr)
    {
        CloseHandle(m_timer_thread);
        m_timer_thread = nullptr;
    }

    // reset stop signal
    m_stop_signal = false;
}

void Context::ProcessTick()
{
    Win32ScopedCriticalSection __cs(m_cs);

    if (m_handler->OnTick(&m_stack_idle, &m_stack_active))
        g_Context.SwitchContext();
}

void Context::SwitchContext()
{
    // suspend Idle thread
    if ((m_stack_idle != m_sleep_trap) && (m_stack_idle != m_exit_trap))
    {
        TaskContext *idle_task = hw::WordToPtr<TaskContext>(m_stack_idle->SP);
        STK_ASSERT(idle_task != nullptr);

        SuspendThread(idle_task->m_thread);
    }

    // resume Active thread
    if (m_stack_active == m_sleep_trap)
    {
        if ((m_overrider == nullptr) || !m_overrider->OnSleep())
        {
            // pass
        }
    }
    else
    if (m_stack_active == g_Context.m_exit_trap)
    {
        // pass
    }
    else
    {
        TaskContext *active_task = hw::WordToPtr<TaskContext>(m_stack_active->SP);
        STK_ASSERT(active_task != nullptr);

        ResumeThread(active_task->m_thread);
    }
}

Word Context::GetCallerSP() const
{
    Word caller_sp = 0;
    DWORD calling_tid = GetCurrentThreadId();

    Win32ScopedCriticalSection __cs(const_cast<STK_X86_WIN32_CRITICAL_SECTION &>(m_cs));

    for (std::list<TaskContext *>::const_iterator itr = m_tasks.begin(), end = m_tasks.end(); itr != end; ++itr)
    {
        if ((*itr)->m_thread_id == calling_tid)
        {
            caller_sp = hw::PtrToWord(STK_X86_WIN32_GET_SP((*itr)->m_task->GetStack()));
            break;
        }
    }

    // expect to find the calling task inside  m_tasks
    STK_ASSERT(caller_sp != 0);

    return caller_sp;
}

TId Context::GetTid() const
{
    return m_handler->OnGetTid(GetCallerSP());
}

void Context::SwitchToNext()
{
    m_handler->OnTaskSwitch(GetCallerSP());
}

void Context::Sleep(Timeout ticks)
{
    m_handler->OnTaskSleep(GetCallerSP(), ticks);
}

IWaitObject *Context::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return m_handler->OnTaskWait(GetCallerSP(), sync_obj, mutex, timeout);
}

void Context::Stop()
{
    m_stop_signal = true;
    m_started = false;
}

bool Context::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    InitStackMemory(stack_memory);

    TaskContext *ctx = reinterpret_cast<TaskContext *>(STK_X86_WIN32_GET_SP(stack_memory->GetStack()));

    switch (stack_type)
    {
    case STACK_USER_TASK: {
        ctx->Initialize(user_task, stack);

        m_tasks.push_back(ctx);
        m_task_threads.push_back(ctx->m_thread);
        break; }

    case STACK_SLEEP_TRAP: {
        g_Context.m_sleep_trap = stack;
        break; }

    case STACK_EXIT_TRAP: {
        g_Context.m_exit_trap = stack;
        break; }
    }

    stack->SP = hw::PtrToWord(ctx);

    return true;
}

void PlatformX86Win32::Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us,
    Stack *exit_trap)
{
    g_Context.Initialize(event_handler, service, exit_trap, resolution_us);
}

void PlatformX86Win32::Start()
{
    g_Context.ConfigureTime();
    g_Context.CreateTimerThreadAndJoin();
    g_Context.Cleanup();
}

void PlatformX86Win32::Stop()
{
    g_Context.Stop();
}

bool PlatformX86Win32::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    return g_Context.InitStack(stack_type, stack, stack_memory, user_task);
}

int32_t PlatformX86Win32::GetTickResolution() const
{
    return g_Context.m_tick_resolution;
}

void PlatformX86Win32::SwitchToNext()
{
    g_Context.SwitchToNext();
}

void PlatformX86Win32::Sleep(Timeout ticks)
{
    g_Context.Sleep(ticks);
}

IWaitObject *PlatformX86Win32::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return g_Context.Wait(sync_obj, mutex, timeout);
}

void PlatformX86Win32::ProcessTick()
{
    g_Context.ProcessTick();
}

void PlatformX86Win32::ProcessHardFault()
{
    if ((g_Context.m_overrider == nullptr) || !g_Context.m_overrider->OnHardFault())
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_HRT_HARD_FAULT);
    }
}

void PlatformX86Win32::SetEventOverrider(IEventOverrider *overrider)
{
    STK_ASSERT(!g_Context.m_started);
    g_Context.m_overrider = overrider;
}

Word PlatformX86Win32::GetCallerSP() const
{
    return g_Context.GetCallerSP();
}

TId PlatformX86Win32::GetTid() const
{
    return g_Context.GetTid();
}

uintptr_t stk::hw::GetTls()
{
    return g_Context.GetTls();
}

void stk::hw::SetTls(uintptr_t tp)
{
    return g_Context.SetTls(tp);
}

IKernelService *IKernelService::GetInstance()
{
    return g_Context.m_service;
}

void stk::hw::CriticalSection::Enter()
{
    g_Context.EnterCriticalSection();
}

void stk::hw::CriticalSection::Exit()
{
    g_Context.ExitCriticalSection();
}

void stk::hw::SpinLock::Lock()
{
    uint8_t sleep_time = 0;

test:
    while (STK_X86_WIN32_SPIN_LOCK_TRYLOCK(m_lock) != SLK_UNLOCKED)
    {
        for (volatile int32_t spin = 100; (spin != 0); spin--)
        {
            __stk_relax_cpu();

            // check if became unlocked then try locking atomically again
            if (m_lock == SLK_UNLOCKED)
                goto test;
        }

        // avoid priority inversion
        ::Sleep(sleep_time);
        sleep_time ^= 1;
    }
}

void stk::hw::SpinLock::Unlock()
{
    STK_X86_WIN32_SPIN_LOCK_UNLOCK(m_lock);
}

bool stk::hw::SpinLock::TryLock()
{
    return (STK_X86_WIN32_SPIN_LOCK_TRYLOCK(m_lock) == SLK_UNLOCKED);
}

bool stk::hw::IsInsideISR()
{
    return false;
}

#endif // _STK_ARCH_X86_WIN32
