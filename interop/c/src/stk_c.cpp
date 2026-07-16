/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <cstddef> // for std::size_t

#include "stk.h"
#include "sync/stk_sync.h"
#include "memory/stk_memory.h"

#include "stk_c.h"
#include "stk_c_time.h"

// Override STK_TIMER_COUNT_MAX with STK_C_TIMER_MAX.
#undef STK_TIMER_COUNT_MAX
#define STK_TIMER_COUNT_MAX (STK_C_TIMER_MAX)
#include "time/stk_time.h"

// Check correctness of stk_config.h
#ifndef STK_C_KERNEL_TYPE_CPU_0
#error "Missing STK_C_KERNEL_TYPE_CPU_0: Kernel type for CPU0 must be defined via stk_config.h or compiler flags."
#endif
#ifndef STK_C_CPU_COUNT
#error "Missing STK_C_CPU_COUNT: CPU count must be defined via stk_config.h or compiler flags."
#endif
#ifndef STK_C_KERNEL_MAX_TASKS
#error "Missing STK_C_KERNEL_MAX_TASKS: max task count must be defined via stk_config.h or compiler flags."
#endif

using namespace stk;

#define STK_C_TASKS_MAX (STK_C_KERNEL_MAX_TASKS)

// Forward decl.
struct stk_task_t;

// Cast from stk_kernel_t to IKernel without a warning.
static __stk_forceinline IKernel *CastCToKernelInterface(stk_kernel_t *const k)
{
    return reinterpret_cast<IKernel *>(reinterpret_cast<void *>(k));
}

// Cast from const stk_kernel_t to const IKernel without a warning.
static __stk_forceinline const IKernel *CastCToKernelInterfaceConst(const stk_kernel_t *const k)
{
    return reinterpret_cast<const IKernel *>(reinterpret_cast<const void *>(k));
}

// Cast from IKernel to stk_kernel_t without a warning.
static __stk_forceinline stk_kernel_t *CastCppKernelInterfaceToC(IKernel *const k)
{
    return reinterpret_cast<stk_kernel_t *>(reinterpret_cast<void *>(k));
}

// Interop-private helpers
namespace stk {
namespace interop_c_helper {

extern void InitializeTimerHost(stk_kernel_t *kernel, stk::time::TimerHost *th, EAccessMode amode)
{
    th->Initialize(CastCToKernelInterface(kernel), amode);
}

} // interop_c_helper
} // stk

static void FreeTask(const stk_task_t *tsk);

class TaskWrapper final : public ITask
{
public:
    explicit TaskWrapper() : m_func(nullptr), m_user_data(nullptr), m_stack(nullptr), 
        m_stack_size(0U), m_mode(ACCESS_USER), m_weight(DEFAULT_WEIGHT), m_tname(nullptr)
    {}
    
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~TaskWrapper() = default;
  
    // ITask
    EAccessMode GetAccessMode()              const override { return m_mode; }
    void OnDeadlineMissed(uint32_t duration)       override { (void)duration; }
    int32_t GetWeight()                      const override { return m_weight; }
    const char *GetTraceName()               const override { return m_tname; }

    // IStackMemory
    const Word *GetStack() const override { return m_stack; }
    size_t GetStackSize()  const override { return m_stack_size; }

    void Initialize(stk_task_entry_t const func, void *user_data, stk_word_t *stack,
        size_t stack_size, EAccessMode mode)
    {
        m_func       = func;
        m_user_data  = user_data;
        m_stack      = stack;
        m_stack_size = stack_size;
        m_mode       = mode;
        m_weight     = DEFAULT_WEIGHT;
    }

    void SetWeight(Weight weight) { m_weight = weight; }
    void SetName(const char *tname) { m_tname = tname; }

private:
    STK_NONCOPYABLE_CLASS(TaskWrapper);
  
    void Run() override { m_func(m_user_data); }
    void OnExit() override;

    stk_task_entry_t m_func;
    void            *m_user_data;
    stk_word_t      *m_stack;
    size_t           m_stack_size;
    EAccessMode      m_mode;
    Weight           m_weight;
    const char      *m_tname;
};

struct stk_task_t
{
    TaskWrapper handle;
};

static struct TaskSlot
{
    TaskSlot() : busy(false), task()
    {}

    bool       busy;
    stk_task_t task;
}
s_Tasks[STK_C_TASKS_MAX];

// Cast from stk_task_t to TaskWrapper without a warning.
static __stk_forceinline stk_task_t *CastCppTaskInterfaceToC(ITask *const t)
{
    return reinterpret_cast<stk_task_t *>(reinterpret_cast<void *>(t));
}

void TaskWrapper::OnExit() 
{
    FreeTask(CastCppTaskInterfaceToC(this)); 
}

// -----------------------------------------------------------------------------
// EventOverriderWrapper - bridges stk_event_overrider_t callbacks into the
// C++ IPlatform::IEventOverrider interface.
//
// One instance is embedded per kernel slot (indexed by core_nr).  The wrapper
// is stateless when m_c == nullptr, which makes it safe to construct at file
// scope.  SetC(nullptr) is equivalent to removing the overrider.
// -----------------------------------------------------------------------------

class EventOverrider final : public IPlatform::IEventOverrider
{
public:
    EventOverrider() : m_cb(nullptr)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~EventOverrider() = default;

    /*! \brief     Bind or unbind a C-level overrider struct.
        \param[in] c: Pointer to a caller-owned stk_event_overrider_t, or nullptr
                   to deactivate this wrapper.
    */
    void SetCallback(stk_event_overrider_t *c) { m_cb = c; }

    /*! \brief     Returns true when a C-level struct is currently bound.
    */
    bool IsActive() const { return (m_cb != nullptr); }

    // IPlatform::IEventOverrider
    bool OnSleep(Timeout sleep_ticks) override
    {
        bool is_handled = false;
      
        if ((m_cb != nullptr) && (m_cb->on_sleep != nullptr))
        {
            is_handled = m_cb->on_sleep(static_cast<stk_timeout_t>(sleep_ticks), m_cb->user_data);
        }

        return is_handled;
    }
    bool OnHardFault() override
    {
        bool is_handled = false;
      
        if ((m_cb != nullptr) && (m_cb->on_hard_fault != nullptr))
        {
            is_handled = m_cb->on_hard_fault(m_cb->user_data);
        }

        return is_handled;
    }

private:
    stk_event_overrider_t *m_cb;
};

struct KernelRegistryEntry
{
    KernelRegistryEntry() : kernel(nullptr), event_cb()
    {}

    IKernel       *kernel;
    EventOverrider event_cb;
};
static KernelRegistryEntry s_KernelMap[STK_C_CPU_COUNT];

static void RegisterKernel(IKernel *k, uint8_t core_nr)
{
    STK_ASSERT(s_KernelMap[core_nr].kernel == nullptr);

    s_KernelMap[core_nr].kernel = k;
}

static void UnregisterKernel(const IKernel *k)
{
    for (uint32_t i = 0U; i < STK_C_CPU_COUNT; ++i)
    {
        if (s_KernelMap[i].kernel == k)
        {
            s_KernelMap[i].event_cb.SetCallback(nullptr);
            s_KernelMap[i].kernel = nullptr;
            break;
        }
    }
}

static void SetEventOverrider(IKernel *k, stk_event_overrider_t *overrider)
{
    bool found = false;

    for (uint32_t i = 0U; (i < STK_C_CPU_COUNT) && (!found); ++i)
    {
        if (s_KernelMap[i].kernel == k)
        {
            s_KernelMap[i].event_cb.SetCallback(overrider);
            k->GetPlatform()->SetEventOverrider(
                (overrider != nullptr) ? &s_KernelMap[i].event_cb : nullptr);
            found = true;
        }
    }

    // Kernel not found: stk_kernel_set_event_overrider() called before
    // stk_kernel_create() or after stk_kernel_destroy().
    STK_ASSERT(found);
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static stk_task_t *AllocateTask(stk_task_entry_t  entry,
                                void             *arg,
                                stk_word_t       *stack,
                                uint32_t          stack_size,
                                EAccessMode       amode)
{
    stk_task_t *tsk = nullptr;

    const sync::ScopedCriticalSection __cs;

    for (uint32_t i = 0U; i < STK_C_TASKS_MAX; ++i)
    {
        if (!s_Tasks[i].busy)
        {
            s_Tasks[i].busy = true;

            tsk = &s_Tasks[i].task;
            tsk->handle.Initialize(entry, arg, stack, stack_size, amode);
            break;
        }
    }

    STK_ASSERT(tsk != nullptr);
    return tsk;
}

void FreeTask(const stk_task_t *tsk)
{
    bool found = false;
    
    const sync::ScopedCriticalSection __cs;

    for (uint32_t i = 0U; ((i < STK_C_TASKS_MAX) && !found); ++i)
    {
        if (s_Tasks[i].busy && (tsk == &s_Tasks[i].task))
        {
            s_Tasks[i].busy = false;
            found = true;
        }
    }

    STK_ASSERT(found);
}

// =============================================================================
// C-interface
// =============================================================================
extern "C" {
 
// -----------------------------------------------------------------------------
// Kernel create/destroy wrappers
// -----------------------------------------------------------------------------
#define STK_PP_CAT(A, B)        A##B
#define STK_PP_CAT_EXPAND(A, B) STK_PP_CAT(A, B)
#define STK_KERNEL_TYPE(X)      STK_PP_CAT(STK_C_KERNEL_TYPE_CPU_, X)
#define STK_KERNEL_MEM(X)       STK_PP_CAT_EXPAND(kernel_, STK_PP_CAT_EXPAND(X, _mem))
#define STK_KERNEL_CASE(X) \
    case X: \
    { \
        using KernelType_ = STK_KERNEL_TYPE(X); \
        STK_STATIC_ASSERT_N(((sizeof(KernelType_) % sizeof(Word)) == 0U), \
                            "Kernel memory size must be multiple of Word"); \
        alignas(alignof(KernelType_)) \
        static Word STK_KERNEL_MEM(X)[sizeof(KernelType_) / sizeof(Word)]; \
        k = new (STK_KERNEL_MEM(X)) KernelType_(); \
        RegisterKernel(k, static_cast<uint8_t>(X)); \
        break; \
    }

stk_kernel_t *stk_kernel_create(uint8_t core_nr)
{
    STK_STATIC_ASSERT((STK_C_CPU_COUNT <= 8U)); // switch (core_nr) below handles cases 0..7; STK_C_KERNEL_TYPE_CPU_N is only defined up to N=7
    STK_ASSERT(core_nr < STK_C_CPU_COUNT);

    IKernel *k;

    switch (core_nr)
    {
#ifdef STK_C_KERNEL_TYPE_CPU_0
    STK_KERNEL_CASE(0)
#endif
#ifdef STK_C_KERNEL_TYPE_CPU_1
    STK_KERNEL_CASE(1)
#endif
#ifdef STK_C_KERNEL_TYPE_CPU_2
    STK_KERNEL_CASE(2)
#endif
#ifdef STK_C_KERNEL_TYPE_CPU_3
    STK_KERNEL_CASE(3)
#endif
#ifdef STK_C_KERNEL_TYPE_CPU_4
    STK_KERNEL_CASE(4)
#endif
#ifdef STK_C_KERNEL_TYPE_CPU_5
    STK_KERNEL_CASE(5)
#endif
#ifdef STK_C_KERNEL_TYPE_CPU_6
    STK_KERNEL_CASE(6)
#endif
#ifdef STK_C_KERNEL_TYPE_CPU_7
    STK_KERNEL_CASE(7)
#endif
    default: {
        k = nullptr;
        break; }
    }

    return CastCppKernelInterfaceToC(k);
}

void stk_kernel_destroy(stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    // Detach the event overrider and clear the registry entry BEFORE calling
    // the destructor: the platform teardown inside ~IKernel() may still invoke
    // GetPlatform() paths, and we must not leave a dangling overrider pointer
    // registered at that point.
    UnregisterKernel(CastCToKernelInterface(k));
}

// -----------------------------------------------------------------------------
// Kernel control wrappers
// -----------------------------------------------------------------------------
void stk_kernel_init(stk_kernel_t *k, uint32_t tick_period_us)
{
    STK_ASSERT(k != nullptr);

    CastCToKernelInterface(k)->Initialize(tick_period_us);
}

void stk_kernel_start(stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    CastCToKernelInterface(k)->Start();
}

stk_kernel_state_t stk_kernel_get_state(const stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    return static_cast<stk_kernel_state_t>(CastCToKernelInterfaceConst(k)->GetState());
}

bool stk_kernel_is_schedulable(const stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    return SchedulabilityCheck::IsSchedulableWCRT<STK_C_KERNEL_MAX_TASKS>(
        const_cast<IKernel *>(CastCToKernelInterfaceConst(k))->GetSwitchStrategy());
}

void stk_kernel_add_task(stk_kernel_t *k, stk_task_t *tsk)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(tsk != nullptr);

    CastCToKernelInterface(k)->AddTask(&tsk->handle);
}

void stk_kernel_remove_task(stk_kernel_t *k, stk_task_t *tsk)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(tsk != nullptr);

    CastCToKernelInterface(k)->RemoveTask(&tsk->handle);
}

bool stk_kernel_is_started(const stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    const stk::IKernel::EKernelState st = CastCToKernelInterfaceConst(k)->GetState();
    return ((st == stk::IKernel::KSTATE_RUNNING) || (st == stk::IKernel::KSTATE_SUSPENDED));
}

void stk_kernel_schedule_task_removal(stk_kernel_t *k, stk_task_t *tsk)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(tsk != nullptr);

    CastCToKernelInterface(k)->ScheduleTaskRemoval(&tsk->handle);
}

void stk_kernel_suspend_task(stk_kernel_t *k, stk_task_t *tsk, bool *suspended)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(tsk != nullptr);
    STK_ASSERT(suspended != nullptr);

    CastCToKernelInterface(k)->SuspendTask(&tsk->handle, *suspended);
}

void stk_kernel_resume_task(stk_kernel_t *k, stk_task_t *tsk)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(tsk != nullptr);

    CastCToKernelInterface(k)->ResumeTask(&tsk->handle);
}

size_t stk_kernel_enumerate_tasks(stk_kernel_t *k, stk_task_t **tasks, size_t max_count)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(tasks != nullptr);

    stk::ITask *itasks[STK_C_TASKS_MAX] = {};
    
    // Determine the safe upper bound for the temporary buffer
    const size_t requested_size = (max_count < static_cast<size_t>(STK_C_TASKS_MAX)) ? max_count : 
        static_cast<size_t>(STK_C_TASKS_MAX);

    // Pass via ArrayView temporary object
    const size_t ret_count = CastCToKernelInterface(k)->EnumerateTasks(
        ArrayView<stk::ITask *>(itasks, requested_size));

    ArrayView<stk_task_t *> output_view(tasks, max_count);
    for (size_t i = 0U; i < ret_count; ++i)
    {
        output_view[i] = CastCppTaskInterfaceToC(itasks[i]);
    }

    return ret_count;
}

stk_timeout_t stk_kernel_suspend(stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    return static_cast<stk_timeout_t>(CastCToKernelInterface(k)->GetPlatform()->Suspend());
}

void stk_kernel_resume(stk_kernel_t *k, stk_timeout_t elapsed_ticks)
{
    STK_ASSERT(k != nullptr);

    CastCToKernelInterface(k)->GetPlatform()->Resume(static_cast<stk::Timeout>(elapsed_ticks));
}

void stk_kernel_process_tick(stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    CastCToKernelInterface(k)->GetPlatform()->ProcessTick();
}

void stk_kernel_process_hard_fault(stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    CastCToKernelInterface(k)->GetPlatform()->ProcessHardFault();
}

void stk_kernel_set_event_overrider(stk_kernel_t *k, stk_event_overrider_t *overrider)
{
    STK_ASSERT(k != nullptr);

    SetEventOverrider(CastCToKernelInterface(k), overrider);
}

void stk_kernel_add_task_hrt(stk_kernel_t *k,
                             stk_task_t   *tsk,
                             int32_t       periodicity_ticks,
                             int32_t       deadline_ticks,
                             int32_t       start_delay_ticks)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(tsk != nullptr);

    CastCToKernelInterface(k)->AddTask(
        &tsk->handle,
        periodicity_ticks,
        deadline_ticks,
        start_delay_ticks);
}

// -----------------------------------------------------------------------------
// Task creation
// -----------------------------------------------------------------------------
stk_task_t *stk_task_create_privileged(stk_task_entry_t  entry,
                                       void             *arg,
                                       stk_word_t       *stack,
                                       uint32_t          stack_size)
{
    STK_ASSERT(entry != nullptr);
    STK_ASSERT(stack != nullptr);
    STK_ASSERT(stack_size != 0);

    return AllocateTask(entry, arg, stack, stack_size, ACCESS_PRIVILEGED);
}

stk_task_t *stk_task_create_user(stk_task_entry_t  entry,
                                 void             *arg,
                                 stk_word_t       *stack,
                                 uint32_t          stack_size)
{
    STK_ASSERT(entry != nullptr);
    STK_ASSERT(stack != nullptr);
    STK_ASSERT(stack_size != 0);

    return AllocateTask(entry, arg, stack, stack_size, ACCESS_USER);
}

void stk_task_set_weight(stk_task_t *tsk, stk_weight_t weight)
{
    STK_ASSERT(tsk != nullptr);

    tsk->handle.SetWeight(weight);
}

void stk_task_set_priority(stk_task_t *tsk, uint8_t priority)
{
    STK_ASSERT(priority <= 31U);
    
    const stk_weight_t weight = static_cast<stk_weight_t>(priority);

    stk_task_set_weight(tsk, weight);
}

void stk_task_set_name(stk_task_t *tsk, const char *tname)
{
    STK_ASSERT(tsk != nullptr);

    tsk->handle.SetName(tname);
}

const char *stk_task_get_name(const stk_task_t *tsk)
{
    STK_ASSERT(tsk != nullptr);

    return tsk->handle.GetTraceName();
}

stk_tid_t stk_task_get_id(const stk_task_t *tsk)
{
    STK_ASSERT(tsk != nullptr);

    return tsk->handle.GetId();
}

void stk_task_destroy(stk_task_t *tsk)
{
    STK_ASSERT(tsk != nullptr);

    FreeTask(tsk);
}

// -----------------------------------------------------------------------------
// Kernel services (available inside tasks)
// -----------------------------------------------------------------------------
stk_tid_t   stk_tid(void)                      { return stk::GetTid(); }
stk_tick_t  stk_ticks(void)                    { return stk::GetTicks(); }
uint32_t    stk_tick_resolution(void)          { return stk::GetTickResolution(); }
stk_time_t  stk_time_now_ms(void)              { return stk::GetTimeNowMs(); }
stk_tick_t  stk_ticks_from_ms(stk_time_t msec) { return stk_ticks_from_ms_r(msec, stk::GetTickResolution()); }
stk_cycle_t stk_sys_timer_count(void)          { return stk::GetSysTimerCount(); }
uint32_t    stk_sys_timer_frequency(void)      { return stk::GetSysTimerFrequency(); }
stk_cycle_t stk_hires_cycles(void)             { return stk::hw::HiResClock::GetCycles(); }
uint32_t    stk_hires_frequency(void)          { return stk::hw::HiResClock::GetFrequency(); }
stk_tick_t  stk_hires_time_us(void)            { return stk::hw::HiResClock::GetTimeUs(); }
void        stk_delay(stk_timeout_t ticks)     { stk::Delay(ticks); }
void        stk_sleep(stk_timeout_t ticks)     { stk::Sleep(ticks); }
void        stk_delay_ms(stk_timeout_t ms)     { stk::DelayMs(ms); }
void        stk_sleep_ms(stk_timeout_t ms)     { stk::SleepMs(ms); }
bool        stk_sleep_until(stk_tick_t ts)     { return stk::SleepUntil(ts); }
void        stk_sleep_cancel(stk_tid_t tid)    { stk::SleepCancel(tid); }
void        stk_yield(void)                    { stk::Yield(); }

// -----------------------------------------------------------------------------
// Thread-Local Storage (TLS) API
// -----------------------------------------------------------------------------
#if STK_TLS
void *stk_tls_get(void)
{
    return hw::GetTlsPtr<void *>();
}

void stk_tls_set(void *ptr)
{
    hw::SetTlsPtr(ptr);
}
#endif

// -----------------------------------------------------------------------------
// Critical Section
// -----------------------------------------------------------------------------

struct stk_cs_t
{
    hw::CriticalSection handle;
};

stk_cs_t *stk_cs_create(stk_cs_mem_t *const membuf, uint32_t membuf_size)
{
    STK_ASSERT(membuf != nullptr);
    STK_ASSERT(membuf_size >= sizeof(stk_cs_mem_t));
    STK_STATIC_ASSERT_N((sizeof(stk_cs_t) <= sizeof(stk_cs_mem_t)),
        "stk_cs_mem_t is too small to hold stk_cs_t");

    return new (membuf) stk_cs_t();
}

void stk_cs_destroy(stk_cs_t *cs)
{
    STK_ASSERT(cs != nullptr);

    cs->~stk_cs_t();
}

void stk_cs_enter(stk_cs_t *cs)
{
    STK_ASSERT(cs != nullptr);

    cs->handle.Enter();
}

void stk_cs_exit(stk_cs_t *cs)
{
    STK_ASSERT(cs != nullptr);

    cs->handle.Exit();
}

// =============================================================================
} // extern "C"
// =============================================================================
