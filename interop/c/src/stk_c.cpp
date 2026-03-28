/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <cstddef> // for std::size_t

#include <stk_config.h>
#include <stk.h>
#include <sync/stk_sync.h>
#include "stk_c.h"

#define STK_C_TASKS_MAX (STK_C_KERNEL_MAX_TASKS)

inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }

using namespace stk;

class TaskWrapper : public ITask
{
public:
    // ITask
    EAccessMode GetAccessMode() const { return m_mode; }
    void OnDeadlineMissed(uint32_t duration) { (void)duration; }
    int32_t GetWeight() const { return m_weight; }
    stk_tid_t GetId() const  { return m_tid; }
    const char *GetTraceName() const  { return m_tname; }

    // IStackMemory
    stk_word_t *GetStack() const { return m_stack; }
    size_t GetStackSize() const { return m_stack_size; }
    size_t GetStackSizeBytes() const { return m_stack_size * sizeof(stk_word_t); }

    void Initialize(stk_task_entry_t func,
                    void       *user_data,
                    stk_word_t  *stack,
                    size_t      stack_size,
                    EAccessMode mode)
    {
        m_func       = func;
        m_user_data  = user_data;
        m_stack      = stack;
        m_stack_size = stack_size;
        m_mode       = mode;
        m_weight     = 1;
    }

    void SetWeight(int32_t weight) { m_weight = weight; }
    void SetId(stk_tid_t tid) { m_tid = tid; }
    void SetName(const char *tname) { m_tname = tname; }

private:
    void Run() { m_func(m_user_data); }

    stk_task_entry_t m_func;
    void            *m_user_data;
    stk_word_t       *m_stack;
    size_t           m_stack_size;
    EAccessMode      m_mode;
    int32_t          m_weight;
    stk_tid_t        m_tid;
    const char      *m_tname;
};

struct stk_task_t
{
    TaskWrapper handle;
};

struct TaskSlot
{
    TaskSlot() : busy(false), task()
    {}

    bool       busy;
    stk_task_t task;
};

// Static vars
static volatile bool s_TaskPoolLock = false;
static TaskSlot s_Tasks[STK_C_TASKS_MAX];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static stk_task_t *AllocateTask(stk_task_entry_t entry,
                                void *arg,
                                stk_word_t *stack,
                                uint32_t stack_size,
                                EAccessMode mode)
{
    stk_task_t *task = nullptr;

    sync::ScopedCriticalSection __cs;

    for (uint32_t i = 0; i < STK_C_TASKS_MAX; ++i)
    {
        if (!s_Tasks[i].busy)
        {
            s_Tasks[i].busy = true;

            task = &s_Tasks[i].task;
            task->handle.Initialize(entry, arg, stack, stack_size, mode);
            break;
        }
    }

    STK_ASSERT(task != nullptr);
    return task;
}

static void FreeTask(const stk_task_t *task)
{
    sync::ScopedCriticalSection __cs;

    for (uint32_t i = 0; i < STK_C_TASKS_MAX; ++i)
    {
        if (s_Tasks[i].busy && (task == &s_Tasks[i].task))
        {
            s_Tasks[i].busy = false;
            return;
        }
    }

    STK_ASSERT(false);
}

// ---------------------------------------------------------------------------
// C-interface
// ---------------------------------------------------------------------------
extern "C" {

// ---------------------------------------------------------------------------
// Kernel create/destroy wrappers
// ---------------------------------------------------------------------------
#define STK_KERNEL_CASE(X) \
    case X: \
    { \
        static_assert(sizeof(STK_C_KERNEL_TYPE_CPU_##X) % sizeof(stk_word_t) == 0, \
                      "Kernel memory size must be multiple of stk_word_t"); \
        alignas(alignof(STK_C_KERNEL_TYPE_CPU_##X)) /* instead of __stk_c_stack_attr */ \
        static stk_word_t kernel_##X##_mem[sizeof(STK_C_KERNEL_TYPE_CPU_##X) / sizeof(stk_word_t)]; \
        IKernel *kernel = new (kernel_##X##_mem) STK_C_KERNEL_TYPE_CPU_##X(); \
        return reinterpret_cast<stk_kernel_t *>(kernel); \
    }

stk_kernel_t *stk_kernel_create(uint8_t core_nr)
{
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
    default:
        return nullptr;
    }
}

void stk_kernel_destroy(stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    reinterpret_cast<IKernel *>(k)->~IKernel();
}

// ---------------------------------------------------------------------------
// Kernel control wrappers
// ---------------------------------------------------------------------------
void stk_kernel_init(stk_kernel_t *k, uint32_t tick_period_us)
{
    STK_ASSERT(k != nullptr);

    reinterpret_cast<stk::IKernel *>(k)->Initialize(tick_period_us);
}

void stk_kernel_start(stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    reinterpret_cast<stk::IKernel *>(k)->Start();
}

EKernelState stk_kernel_get_state(const stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    return static_cast<EKernelState>(reinterpret_cast<const stk::IKernel *>(k)->GetState());
}

bool stk_kernel_is_schedulable(const stk_kernel_t *k)
{
    STK_ASSERT(k != nullptr);

    return SchedulabilityCheck::IsSchedulableWCRT<STK_C_KERNEL_MAX_TASKS>(
            reinterpret_cast<stk::IKernel *>(const_cast<stk_kernel_t *>(k))->GetSwitchStrategy());
}

void stk_kernel_add_task(stk_kernel_t *k, stk_task_t *task)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(task != nullptr);

    reinterpret_cast<stk::IKernel *>(k)->AddTask(&task->handle);
}

void stk_kernel_remove_task(stk_kernel_t *k, stk_task_t *task)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(task != nullptr);

    reinterpret_cast<stk::IKernel *>(k)->RemoveTask(&task->handle);
}

void stk_kernel_add_task_hrt(stk_kernel_t *k,
                             stk_task_t *task,
                             int32_t periodicity_ticks,
                             int32_t deadline_ticks,
                             int32_t start_delay_ticks)
{
    STK_ASSERT(k != nullptr);
    STK_ASSERT(task != nullptr);

    reinterpret_cast<stk::IKernel *>(k)->AddTask(
        &task->handle,
        periodicity_ticks,
        deadline_ticks,
        start_delay_ticks);
}

// ---------------------------------------------------------------------------
// Task creation
// ---------------------------------------------------------------------------
stk_task_t *stk_task_create_privileged(stk_task_entry_t entry,
                                       void *arg,
                                       stk_word_t *stack,
                                       uint32_t stack_size)
{
    STK_ASSERT(entry != nullptr);
    STK_ASSERT(stack != nullptr);
    STK_ASSERT(stack_size != 0);

    return reinterpret_cast<stk_task_t *>(AllocateTask(entry, arg, stack, stack_size, ACCESS_PRIVILEGED));
}

stk_task_t *stk_task_create_user(stk_task_entry_t entry,
                                 void *arg,
                                 stk_word_t *stack,
                                 uint32_t stack_size)
{
    STK_ASSERT(entry != nullptr);
    STK_ASSERT(stack != nullptr);
    STK_ASSERT(stack_size != 0);

    return reinterpret_cast<stk_task_t *>(AllocateTask(entry, arg, stack, stack_size, ACCESS_USER));
}

void stk_task_set_weight(stk_task_t *task, uint32_t weight)
{
    STK_ASSERT(task != nullptr);
    STK_ASSERT(weight != 0);

    task->handle.SetWeight(weight);
}

void stk_task_set_priority(stk_task_t *task, uint8_t priority)
{
    STK_ASSERT(priority <= 31);

    stk_task_set_weight(task, priority);
}

void stk_task_set_id(stk_task_t *task, uint32_t tid)
{
    STK_ASSERT(task != nullptr);

    task->handle.SetId(tid);
}

void stk_task_set_name(stk_task_t *task, const char *tname)
{
    STK_ASSERT(task != nullptr);

    task->handle.SetName(tname);
}

void stk_task_destroy(stk_task_t *task)
{
    STK_ASSERT(task != nullptr);

    FreeTask(task);
}

// ---------------------------------------------------------------------------
// Kernel services (available inside tasks)
// ---------------------------------------------------------------------------
stk_tid_t stk_tid(void)             { return stk::GetTid(); }
int64_t   stk_ticks(void)           { return stk::GetTicks(); }
int32_t   stk_tick_resolution(void) { return stk::GetTickResolution(); }
int64_t   stk_time_now_ms(void)     { return stk::GetTimeNowMsec(); }
int64_t   stk_ticks_from_ms(int64_t msec) { return stk_ticks_from_ms_r(msec, stk_tick_resolution()); }
void      stk_delay_ms(uint32_t ms) { stk::Delay(ms); }
void      stk_sleep_ms(uint32_t ms) { stk::Sleep(ms); }
void      stk_yield(void)           { stk::Yield(); }

// ---------------------------------------------------------------------------
// Thread-Local Storage (TLS) API
// ---------------------------------------------------------------------------
void *stk_tls_get(void)
{
    return hw::GetTlsPtr<void *>();
}

void stk_tls_set(void *ptr)
{
    hw::SetTlsPtr(ptr);
}

// ---------------------------------------------------------------------------
// Critical Section - Manual Enter/Exit
// ---------------------------------------------------------------------------
void stk_critical_section_enter(void)
{
    hw::CriticalSection::Enter();
}

void stk_critical_section_exit(void)
{
    hw::CriticalSection::Exit();
}

} // extern "C"
