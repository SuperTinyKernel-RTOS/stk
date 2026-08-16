/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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

using namespace stk;

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <list>
#include <vector>

using namespace stk;

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

/*! \brief Attempt to lock a spin-lock.
*/
static __stk_forceinline bool HW_SpinLockTryLock(volatile LONG &lock)
{
    return (InterlockedCompareExchange(
        reinterpret_cast<volatile LONG *>(&lock), SLK_LOCKED, SLK_UNLOCKED) == SLK_UNLOCKED);
}

/*! \brief Lock a spin-lock.
*/
static __stk_forceinline void HW_SpinLockLock(volatile LONG &lock)
{
    uint8_t sleep_time = 0;
    uint32_t timeout = 0xFFFFFF;

test:
    while (!HW_SpinLockTryLock(lock))
    {
        if (--timeout == 0)
        {
            // invariant violated: the lock owner exited without releasing
            STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK);
        }

        for (volatile int32_t spin = 100; (spin != 0); spin--)
        {
            __stk_relax_cpu();

            // check if became unlocked then try locking atomically again
            if (lock == SLK_UNLOCKED)
                goto test;
        }

        // avoid priority inversion
        ::Sleep(sleep_time);
        sleep_time ^= 1;
    }
}

/*! \brief Unlock a spin-lock.
*/
static __stk_forceinline void HW_SpinLockUnlock(volatile LONG &lock)
{
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&lock), SLK_UNLOCKED);
}

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

class HiResClockQPC
{
    LARGE_INTEGER m_freq;
    LARGE_INTEGER m_start;

public:
    explicit HiResClockQPC()
    {
        QueryPerformanceFrequency(&m_freq);
        QueryPerformanceCounter(&m_start);
    }

    static HiResClockQPC *GetInstance()
    {
        // keep declaration function-local to allow compiler stripping it from the binary if
        // it is unused by the user code
        static HiResClockQPC clock;
        return &clock;
    }

    Cycles GetCycles()
    {
        LARGE_INTEGER current;
        QueryPerformanceCounter(&current);

        // relative cycles since simulation start
        return static_cast<Cycles>(current.QuadPart - m_start.QuadPart);
    }

    uint32_t GetFrequency()
    {
        return static_cast<uint32_t>(m_freq.QuadPart);
    }
};

//! Internal context.
static struct Context final : public PlatformContext
{
    Context()
    : m_overrider(nullptr),
        m_sleep_trap(nullptr),
        m_exit_trap(nullptr),
        m_winmm_dll(nullptr),
        m_timer_thread(nullptr),
        m_tls(TLS_OUT_OF_INDEXES),
        m_tasks(),
        m_task_threads(),
        m_timer_tid(0),
    #if STK_TICKLESS_IDLE
        m_sleep_ticks(0),
    #endif
        m_cs(),
        m_csu_nesting(0),
        m_started(false),
        m_stop_signal(false)
    {}

    void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap,
        uint32_t resolution_us) override
    {
        PlatformContext::Initialize(handler, service, exit_trap, resolution_us);

        m_sleep_trap   = nullptr; // set by Context::InitStack
        m_exit_trap    = nullptr; // set by Context::InitStack
        m_winmm_dll    = nullptr;
        m_timer_thread = nullptr;
        m_started      = false;
        m_stop_signal  = false;
        m_csu_nesting  = 0;
        m_timer_tid    = 0;
    #if STK_TICKLESS_IDLE
        m_sleep_ticks  = 0;
    #endif

    #if STK_TLS
        if ((m_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES)
        {
            assert(false);
            return;
        }
    #endif

        STK_X86_WIN32_CRITICAL_SECTION_INIT(&m_cs);

        LoadWindowsAPI();
    }

    virtual ~Context()
    {
    #if STK_TLS
        if (m_tls != TLS_OUT_OF_INDEXES)
            TlsFree(m_tls);
    #endif

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
        {}

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
            const size_t stack_size = m_task->GetStackSize() * sizeof(Word);

            m_thread = CreateThread(nullptr, stack_size, &OnTaskRun, this, CREATE_SUSPENDED, &m_thread_id);
        }

        static DWORD WINAPI OnTaskRun(LPVOID param)
        {
            ((TaskContext *)param)->m_task->Run();
            return 0;
        }

        ITask  *m_task;       //!< user task
        Stack  *m_stack;      //!< user tasks's stack
        HANDLE  m_thread;     //!< task's thread handle
        DWORD   m_thread_id;  //!< task's thread id
    };

    void InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task);
    void ConfigureTime();
    void StartActiveTask();
    void CreateTimerThreadAndJoin();
    void Cleanup();
    void ProcessTick();
    void SwitchContext();
    void SwitchToNext();
    void Sleep(Timeout ticks);
    bool SleepUntil(Ticks timestamp);
    EWaitResult Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout);
    void Stop();
    Word GetCallerSP() const;
    TId GetTid() const;
    
#if STK_TLS
    __stk_forceinline Word GetTls() 
    { 
        return hw::PtrToWord(TlsGetValue(m_tls)); 
    }

    __stk_forceinline void SetTls(Word tp) 
    { 
        TlsSetValue(m_tls, hw::WordToPtr<void>(tp));
    }
#endif
    
    __stk_forceinline void EnterCriticalSection()
    {
        STK_X86_WIN32_CRITICAL_SECTION_START(&m_cs);

        if (m_csu_nesting == 0)
        {
            // avoid suspending self
            if (GetCurrentThreadId() != m_timer_tid)
            {
                SuspendThread(m_timer_thread);
            }
        }

        // increase nesting count within a limit
        if (++m_csu_nesting > STK_CS_NESTINGS_MAX)
        {
            // invariant violated: exceeded max allowed number of recursions
            STK_KERNEL_PANIC(KERNEL_PANIC_CS_NESTING_OVERFLOW);
        }
    }
    
    __stk_forceinline void ExitCriticalSection()
    {
        STK_ASSERT(m_csu_nesting != 0);

        --m_csu_nesting;

        if (m_csu_nesting == 0)
        {
            // suspending self is not supported
            if (GetCurrentThreadId() != m_timer_tid)
            {
                ResumeThread(m_timer_thread);
            }
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
    DWORD                          m_timer_tid;     //!< timer thread id
#if STK_TICKLESS_IDLE
    Timeout                        m_sleep_ticks;   //!< sleep ticks of the current session
#endif
    STK_X86_WIN32_CRITICAL_SECTION m_cs;            //!< critical session
    uint8_t                        m_csu_nesting;   //!< depth of user critical session nesting
    bool                           m_started;       //!< started state's flag
    volatile bool                  m_stop_signal;   //!< stop signal for a timer thread
}
s_StkPlatformContext[1];

//! Panic id cache for post-mortem inspection.
static volatile EKernelPanicId g_LastPanicId = KERNEL_PANIC_NONE;

__stk_attr_noinline  // keep out of inlining to preserve stack frame
__stk_attr_noreturn  // never returns - a trap
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    g_LastPanicId = id;

    // spin forever: without a watchdog, a debugger can attach and inspect 'id'
    for (;;)
    {
        __stk_relax_cpu();
    }
}

static __stk_forceinline DWORD TicksToMs(uint64_t ticks)
{
    return static_cast<DWORD>((ticks * GetContext().m_tick_resolution) / 1000U);
}

static DWORD WINAPI TimerThread(LPVOID param)
{
    (void)param;

    DWORD wait_ms = TicksToMs(1U);
    GetContext().m_timer_tid = GetCurrentThreadId();

    while (WaitForSingleObject(GetContext().m_timer_thread, wait_ms) == WAIT_TIMEOUT)
    {
        if (GetContext().m_stop_signal)
        {
            break;
        }

        GetContext().ProcessTick();

    #if STK_TICKLESS_IDLE
        wait_ms = TicksToMs(GetContext().m_sleep_ticks);
    #endif
    }

    return 0;
}

void Context::ConfigureTime()
{
    // Windows timers are jittery, so make resolution more coarse
    if (m_tick_resolution < STK_X86_WIN32_MIN_RESOLUTION)
    {
        m_tick_resolution = STK_X86_WIN32_MIN_RESOLUTION;
    }

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

#if STK_TICKLESS_IDLE
    m_sleep_ticks = 1;
#endif

    m_handler->OnStart(m_stack_active);

    StartActiveTask();

    // create tick thread with highest priority
    m_timer_thread = CreateThread(nullptr, 0, &TimerThread, nullptr, 0, nullptr);
    STK_ASSERT(m_timer_thread != nullptr);
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
                {
                    m_handler->OnTaskExit(exiting_task->m_stack);
                }

                m_task_threads.erase(itr);
                break;
            }

            ++i;
        }
    }

    // join (never returns to the caller from here unless thread is terminated, see KERNEL_DYNAMIC),
    // a stop signal is sent by IPlatform::Stop() by the last exiting task
    if (m_timer_thread != nullptr)
    {
        WaitForSingleObject(m_timer_thread, INFINITE);
    }
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

    // notify kernel about a full stop
    m_handler->OnStop();
}

void Context::ProcessTick()
{
    Win32ScopedCriticalSection __cs(m_cs);

#if STK_TICKLESS_IDLE
    Timeout ticks = m_sleep_ticks;
#endif

    if (m_handler->OnTick(m_stack_idle, m_stack_active
    #if STK_TICKLESS_IDLE
        , ticks
    #endif
    ))
    {
        GetContext().SwitchContext();
    }

#if STK_TICKLESS_IDLE
    m_sleep_ticks = ticks;
#endif
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
    #if STK_TICKLESS_IDLE
        const Timeout sleep_ticks = m_sleep_ticks;
    #else
        const Timeout sleep_ticks = 1;
    #endif

        if ((m_overrider == nullptr) || !m_overrider->OnSleep(sleep_ticks))
        {
            // pass
        }
    }
    else
    if (m_stack_active == GetContext().m_exit_trap)
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

bool Context::SleepUntil(Ticks timestamp)
{
    return m_handler->OnTaskSleepUntil(GetCallerSP(), timestamp);
}

EWaitResult Context::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return m_handler->OnTaskWait(GetCallerSP(), sync_obj, mutex, timeout);
}

void Context::Stop()
{
    m_stop_signal = true;
    m_started = false;
}

void Context::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    InitStackMemory(stack_memory);

    Word *const stack_mem = const_cast<Word *>(stack_memory->GetStack());
    TaskContext *const ctx = reinterpret_cast<TaskContext *>(STK_X86_WIN32_GET_SP(stack_mem));

    switch (stack_type)
    {
    case STACK_USER_TASK: {
        ctx->Initialize(user_task, stack);

        m_tasks.push_back(ctx);
        m_task_threads.push_back(ctx->m_thread);
        break; }

    case STACK_SLEEP_TRAP: {
        GetContext().m_sleep_trap = stack;
        break; }

    case STACK_EXIT_TRAP: {
        GetContext().m_exit_trap = stack;
        break; }

    default: {
        STK_ASSERT(false);
        break; }
    }

    stack->SP = hw::PtrToWord(ctx);
}

void PlatformX86Win32::Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us,
    Stack *exit_trap)
{
    GetContext().Initialize(event_handler, service, exit_trap, resolution_us);
}

void PlatformX86Win32::Start()
{
    GetContext().ConfigureTime();
    GetContext().CreateTimerThreadAndJoin();
    GetContext().Cleanup();
}

void PlatformX86Win32::Stop()
{
    GetContext().Stop();
}

Timeout PlatformX86Win32::Suspend()
{
    STK_ASSERT(false); // unsupported
    return 0;
}

void PlatformX86Win32::Resume(Timeout elapsed_ticks)
{
    STK_ASSERT(false); // unsupported
}

void PlatformX86Win32::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    GetContext().InitStack(stack_type, stack, stack_memory, user_task);
}

uint32_t PlatformX86Win32::GetTickResolution() const
{
    return GetContext().m_tick_resolution;
}

Cycles PlatformX86Win32::GetSysTimerCount() const
{
    return HiResClockQPC::GetInstance()->GetCycles();
}

uint32_t PlatformX86Win32::GetSysTimerFrequency() const
{
    return HiResClockQPC::GetInstance()->GetFrequency();
}

void PlatformX86Win32::SwitchToNext()
{
    GetContext().SwitchToNext();
}

void PlatformX86Win32::Sleep(Timeout ticks)
{
    GetContext().Sleep(ticks);
}

bool PlatformX86Win32::SleepUntil(Ticks timestamp)
{
    return GetContext().SleepUntil(timestamp);
}

EWaitResult PlatformX86Win32::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return GetContext().Wait(sync_obj, mutex, timeout);
}

void PlatformX86Win32::ProcessTick()
{
    GetContext().ProcessTick();
}

void PlatformX86Win32::ProcessHardFault()
{
    if ((GetContext().m_overrider == nullptr) || !GetContext().m_overrider->OnHardFault())
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_HRT_HARD_FAULT);
    }
}

void PlatformX86Win32::SetEventOverrider(IEventOverrider *overrider, bool non_secure)
{
    STK_UNUSED(non_secure);

    STK_ASSERT(!GetContext().m_started);
    GetContext().m_overrider = overrider;
}

Word PlatformX86Win32::GetCallerSP() const
{
    return GetContext().GetCallerSP();
}

TId PlatformX86Win32::GetTid() const
{
    return GetContext().GetTid();
}

#if STK_TLS
Word stk::hw::GetTls()
{
    return GetContext().GetTls();
}

void stk::hw::SetTls(Word tp)
{
    return GetContext().SetTls(tp);
}
#endif

IKernelService *IKernelService::GetInstance()
{
    return GetContext().m_service;
}

stk::hw::CriticalSection::Session stk::hw::CriticalSection::Enter(CriticalSection::Session is_npriv)
{
    STK_UNUSED(is_npriv);
    GetContext().EnterCriticalSection();
    return DEFAULT_SESSION;
}

void stk::hw::CriticalSection::Exit(CriticalSection::Session is_npriv)
{
    STK_UNUSED(is_npriv);
    GetContext().ExitCriticalSection();
}

void stk::hw::SpinLock::Lock()
{
    HW_SpinLockLock(m_lock);
}

void stk::hw::SpinLock::Unlock()
{
    HW_SpinLockUnlock(m_lock);
}

bool stk::hw::SpinLock::TryLock()
{
    return HW_SpinLockTryLock(m_lock);
}

bool stk::hw::IsInsideISR()
{
    return false;
}

bool stk::hw::IsPrivilegedContext()
{
    return true;
}

Cycles stk::hw::HiResClock::GetCycles()
{
    return HiResClockQPC::GetInstance()->GetCycles();
}

uint32_t stk::hw::HiResClock::GetFrequency()
{
    return HiResClockQPC::GetInstance()->GetFrequency();
}

#endif // _STK_ARCH_X86_WIN32
