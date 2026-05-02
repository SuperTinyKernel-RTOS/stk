/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_H_
#define STK_C_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>

/*! \file     stk_c.h
    \brief    C language binding/interface for SuperTinyKernel RTOS.

    This header provides a pure C API to create, configure and run STK kernel
    from C code.

    \defgroup c_api STK C API
    \brief    Pure C interface for C++ API of SuperTinyKernel RTOS.
    @{
*/

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Configuration macros (can be overridden before including this file)
// =============================================================================

/*! \def       STK_C_KERNEL_MAX_TASKS
    \brief     Maximum number of tasks per kernel instance (default: 4).
    \note      Increase this value if you need more tasks.
               Has direct impact on RAM and FLASH usage.
*/
#ifndef STK_C_KERNEL_MAX_TASKS
    #define STK_C_KERNEL_MAX_TASKS (4)
#endif

/*! \def       STK_C_CPU_COUNT
    \brief     Number of kernel instances / CPU cores supported (default: 1)
    \note      Each core usually gets its own independent kernel instance.
*/
#ifndef STK_C_CPU_COUNT
    #define STK_C_CPU_COUNT (1)
#endif

/*! \def       STK_SYNC_DEBUG_NAMES
    \brief     Enable names for synchronization primitives for debugging/tracing purpose.
*/
#if !defined(STK_SYNC_DEBUG_NAMES) && STK_SEGGER_SYSVIEW
    #define STK_SYNC_DEBUG_NAMES (1)
#elif !defined(STK_SYNC_DEBUG_NAMES)
    #define STK_SYNC_DEBUG_NAMES (0)
#endif

/*! \def       STK_C_ASSERT
    \brief     Assertion macro used inside STK C bindings
*/
#define STK_C_ASSERT(e) assert(e)

// =============================================================================
// Types
// =============================================================================

/*! \brief     CPU register type.
*/
typedef uintptr_t stk_word_t;

/*! \brief     Task id.
*/
typedef stk_word_t stk_tid_t;

/*! \brief     Ticks value.
    \see       stk_ticks
*/
typedef int64_t stk_tick_t;

/*! \brief     Time value.
    \see       stk_time_now_ms
*/
typedef int64_t stk_time_t;

/*! \brief     Timeout value.
    \see       stk_sleep, stk_delay
*/
typedef int32_t stk_timeout_t;

/*! \brief     CPU cycles value.
    \see       stk_sys_timer_count, stk_hires_cycles
*/
typedef uint64_t stk_cycle_t;

/*! \brief     Opaque handle to a kernel instance.
*/
typedef struct stk_kernel_t stk_kernel_t;

/*! \brief     Opaque handle to a task instance.
*/
typedef struct stk_task_t stk_task_t;

/*! \brief     Default tick period (1 ms).
*/
#define STK_PERIODICITY_DEFAULT (1000U) /*!< in microseconds */

/*! \brief     Task entry point function type
    \param[in] arg: User-supplied argument (may be NULL)
    \note      If \a KERNEL_STATIC, the function must never return.
               If \a KERNEL_DYNAMIC, it may return and then task will be considered as finished.
    \note      \a KERNEL_TICKLESS is compatible with both \a KERNEL_STATIC and \a KERNEL_DYNAMIC,
               but is incompatible with \a KERNEL_HRT (see kernel type definitions below).
*/
typedef void (*stk_task_entry_t)(void *arg);

/*! \brief     Infinite timeout constant.
*/
#define STK_WAIT_INFINITE ((stk_timeout_t)(INT32_MAX))

/*! \brief     No timeout constant.
*/
#define STK_NO_WAIT ((stk_timeout_t)(0))

/*! \brief     Memory buffer alignment.
*/
#define STK_ALIGN_SIZE sizeof(stk_word_t)

/*! \brief     Alignment mask.
*/
#define STK_ALIGN_MASK (STK_ALIGN_SIZE - 1)

/*! \def       STK_STACK_MEMORY_ALIGN
    \brief     Stack memory alignment.
*/
#ifndef STK_STACK_MEMORY_ALIGN
    #if defined(__riscv)
        #define STK_STACK_MEMORY_ALIGN 16U
    #elif defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
        #define STK_STACK_MEMORY_ALIGN 8U
    #else // ARM, others
        #define STK_STACK_MEMORY_ALIGN 4U
    #endif
#endif

// =============================================================================
// Attributes
// =============================================================================

/*! \def       __stk_c_stack
    \brief     Stack attribute (applies required alignment for a stack memory).
*/
#if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__)
    #define __stk_c_stack __attribute__((aligned(STK_STACK_MEMORY_ALIGN)))
#else
    #define __stk_c_stack
#endif

/*! \def       __stk_c_aligned
    \brief     Memory buffer alignment attribute.
*/
#if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__)
    #define __stk_c_aligned __attribute__((aligned(STK_ALIGN_SIZE)))
#else
    #define __stk_c_aligned
#endif

// =============================================================================
// Kernel factory functions
// =============================================================================

/* Available kernel type definitions:

   Kernel mode flags (may be OR-combined, subject to the constraints listed below):
     KERNEL_STATIC   - fixed task list; tasks must never return from their entry function.
     KERNEL_DYNAMIC  - tasks may be added/removed at runtime and may return when done.
     KERNEL_HRT      - Hard Real-Time mode; must be combined with KERNEL_STATIC or KERNEL_DYNAMIC.
     KERNEL_SYNC     - enables synchronization primitives (Mutex, Event, Semaphore, etc.).
     KERNEL_TICKLESS - tickless low-power idle; suppresses the SysTick when all tasks sleep.
                       Requires STK_TICKLESS_IDLE=1 in stk_config.h.
                       INCOMPATIBLE with KERNEL_HRT (HRT requires a continuous tick).

// Standard variants
Kernel<KERNEL_STATIC,  STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
Kernel<KERNEL_DYNAMIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
Kernel<KERNEL_STATIC,  STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_DYNAMIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_STATIC,  STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>
Kernel<KERNEL_DYNAMIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>

// HRT variants (KERNEL_TICKLESS must NOT be combined with any of these)
Kernel<KERNEL_STATIC  | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRM, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRM, PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyDM, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyDM, PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyEDF, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT, STK_C_KERNEL_MAX_TASKS, SwitchStrategyEDF, PlatformDefault>

// Standard variants with KERNEL_SYNC
Kernel<KERNEL_STATIC  | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,   PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,   PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>

// HRT variants with KERNEL_SYNC
Kernel<KERNEL_STATIC  | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,  PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,  PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRM,  PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRM,  PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyDM,  PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyDM,  PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyEDF, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_HRT | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyEDF, PlatformDefault>

// Tickless variants (low-power; KERNEL_TICKLESS requires STK_TICKLESS_IDLE=1 in stk_config.h)
Kernel<KERNEL_STATIC  | KERNEL_TICKLESS, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,   PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_TICKLESS, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,   PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_TICKLESS, STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_TICKLESS, STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_TICKLESS, STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_TICKLESS, STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>

// Tickless variants with KERNEL_SYNC
Kernel<KERNEL_STATIC  | KERNEL_TICKLESS | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,   PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_TICKLESS | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR,   PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_TICKLESS | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_TICKLESS | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategySWRR, PlatformDefault>
Kernel<KERNEL_STATIC  | KERNEL_TICKLESS | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>
Kernel<KERNEL_DYNAMIC | KERNEL_TICKLESS | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>
*/

/*! \def       STK_C_KERNEL_TYPE_CPU_X
    \brief     Kernel type definition per CPU core.
    \note      STK_C_KERNEL_TYPE_CPU_X type will be assigned to X core.
    \note      Supported mode flags: KERNEL_STATIC, KERNEL_DYNAMIC, KERNEL_HRT, KERNEL_SYNC,
               KERNEL_TICKLESS. See the "Available kernel type definitions" block above for
               valid combinations. KERNEL_TICKLESS requires STK_TICKLESS_IDLE=1 in stk_config.h
               and must NOT be combined with KERNEL_HRT.

    \code
    // Example of kernel type definition for 8 cores.
    #define STK_C_KERNEL_TYPE_CPU_0 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    #define STK_C_KERNEL_TYPE_CPU_1 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    #define STK_C_KERNEL_TYPE_CPU_2 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    #define STK_C_KERNEL_TYPE_CPU_3 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    #define STK_C_KERNEL_TYPE_CPU_4 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    #define STK_C_KERNEL_TYPE_CPU_5 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    #define STK_C_KERNEL_TYPE_CPU_6 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    #define STK_C_KERNEL_TYPE_CPU_7 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>

    // Tickless example - CPU enters low-power state when all tasks are sleeping:
    #define STK_C_KERNEL_TYPE_CPU_0 Kernel<KERNEL_STATIC | KERNEL_TICKLESS, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
    \endcode
 */

/*! \brief     Create kernel.
    \note      At least \a STK_C_KERNEL_TYPE_CPU_0 must be defined with the type of the kernel.
               Place STK_C_KERNEL_TYPE_CPU_X defines inside the stk_config.h file which is per project.
               STK_C_KERNEL_TYPE_CPU_X type will be assigned to X core.
    \param[in] core_nr: CPU core number (starts with 0). Max: 7 (for 8 cores).
*/
stk_kernel_t *stk_kernel_create(uint8_t core_nr);

// =============================================================================
// Kernel control
// =============================================================================

/*! \brief     Initialize kernel with given tick period.
    \param[in] k:  Kernel handle.
    \param[in] tick_period_us: System tick period in microseconds (usually 100–10000).
    \note      Must be called exactly once before adding tasks or starting scheduler.
*/
void stk_kernel_init(stk_kernel_t *k, uint32_t tick_period_us);

/*! \brief     Add task to non-HRT kernel (static or dynamic).
    \param[in] k: Kernel handle.
    \param[in] task: Task handle created with one of stk_task_create_* functions.
    \note      For static kernels this must be done before stk_kernel_start().
*/
void stk_kernel_add_task(stk_kernel_t *k, stk_task_t *task);

/*! \brief     Add task with HRT timing parameters (HRT kernels only).
    \param[in] k: Kernel handle.
    \param[in] task: Task handle.
    \param[in] periodicity_ticks: Period in ticks.
    \param[in] deadline_ticks: Relative deadline in ticks.
    \param[in] start_delay_ticks: Initial offset / phase in ticks (>= 0).
    \note      Must be called after stk_kernel_init() and before stk_kernel_start().
*/
void stk_kernel_add_task_hrt(stk_kernel_t *k,
                             stk_task_t *task,
                             int32_t periodicity_ticks,
                             int32_t deadline_ticks,
                             int32_t start_delay_ticks);

/*! \brief     Remove finished task from dynamic kernel.
    \param[in] k: Kernel handle.
    \param[in] task: Task that has already returned from its entry function.
    \note      Only valid in dynamic kernels. Task must have exited (returned from entry function).
*/
void stk_kernel_remove_task(stk_kernel_t *k, stk_task_t *task);

/*! \brief     Start the scheduler - never returns.
    \param[in] k: Kernel handle.
    \note      Transfers control to the scheduler and a first ready task. May return if all tasks are
               finished and kernel is dynamic.
*/
void stk_kernel_start(stk_kernel_t *k);

/*! \brief     Kernel state.
    \note      It is a direct match for IKernel::EState enum.
    \see       stk_kernel_get_state()
*/
typedef enum _EKernelState {
    STK_KERNEL_STATE_INACTIVE  = 0, //!< not ready, stk_kernel_init() must be called
    STK_KERNEL_STATE_READY     = 1, //!< ready to start, stk_kernel_start() must be called
    STK_KERNEL_STATE_RUNNING   = 2, //!< initialized and running, stk_kernel_start() was called successfully
    STK_KERNEL_STATE_SUSPENDED = 3  //!< scheduling suspended via stk_kernel_service_suspend() (tickless idle)
} EKernelState;

/*! \brief     Get state of the scheduler.
    \param[in] k: Kernel handle.
    \return    State value, see \a EKernelState.
*/
EKernelState stk_kernel_get_state(const stk_kernel_t *k);

/*! \brief     Test whether currently configured task set is schedulable.
    \param[in] k: Kernel handle.
    \return    True if task set passes schedulability test, False otherwise.
    \note      Only meaningful for HRT RM/DM kernels.
*/
bool stk_kernel_is_schedulable(const stk_kernel_t *k);

/*! \brief     Check whether the scheduler is currently running (first task switch has occurred).
    \param[in] k: Kernel handle.
    \return    True if stk_kernel_start() has been called and the first context switch has occurred,
               False before stk_kernel_start() or after all tasks have exited.
*/
bool stk_kernel_is_started(const stk_kernel_t *k);

/*! \brief     Schedule removal of a running task from the kernel on the next tick.
    \param[in] k: Kernel handle.
    \param[in] task: Task to remove. Must be currently scheduled.
    \note      KERNEL_DYNAMIC only. The task is removed on the next scheduling tick and its
               OnExit() callback is invoked automatically.
    \note      To remove a task before stk_kernel_start(), use stk_kernel_remove_task() instead.
    \see       stk_kernel_remove_task
*/
void stk_kernel_schedule_task_removal(stk_kernel_t *k, stk_task_t *task);

/*! \brief     Suspend a task (prevent it from being scheduled).
    \param[in]  k: Kernel handle.
    \param[in]  task: Task to suspend.
    \param[out] suspended: Set to true if the task was successfully suspended (was awake),
                false if the task was already sleeping (e.g. blocked on a mutex or timed Sleep).
    \note      Do not hold a critical section when suspending the calling task - this will deadlock.
    \note      If the task suspends itself, the call blocks until the kernel switches it out.
*/
void stk_kernel_suspend_task(stk_kernel_t *k, stk_task_t *task, bool *suspended);

/*! \brief     Resume a previously suspended task.
    \param[in] k: Kernel handle.
    \param[in] task: Task to resume. Must have been suspended with stk_kernel_suspend_task().
*/
void stk_kernel_resume_task(stk_kernel_t *k, stk_task_t *task);

/*! \brief     Suspend scheduling (tickless idle entry point).
    \param[in] k: Kernel handle.
    \return    Number of ticks available for the suspension period, determined by the
               nearest pending wake-up deadline across all sleeping tasks. The caller
               may program a hardware timer with this value to suppress SysTick wakeups.
    \note      ISR-safe. Pair every call with stk_kernel_resume().
    \note      Only meaningful when the kernel was created with KERNEL_TICKLESS mode.
    \see       stk_kernel_resume, STK_KERNEL_STATE_SUSPENDED
*/
int32_t stk_kernel_suspend(stk_kernel_t *k);

/*! \brief     Resume scheduling after a prior stk_kernel_suspend() call.
    \param[in] k: Kernel handle.
    \param[in] elapsed_ticks: Number of ticks that elapsed during the suspended period.
               The kernel uses this value to advance internal time counters and wake tasks
               whose sleep deadlines have expired.
    \note      ISR-safe.
    \see       stk_kernel_suspend
*/
void stk_kernel_resume(stk_kernel_t *k, int32_t elapsed_ticks);

/*! \brief     Enumerate all currently active tasks.
    \param[in]  k: Kernel handle.
    \param[out] tasks: Caller-allocated array of opaque task pointers.  Each element is a
                stk_task_t* that can be passed to other stk_task_* / stk_kernel_* functions.
    \param[in]  max_count: Capacity of the \a tasks array (number of elements).
    \return     Number of active tasks written into \a tasks (0 .. max_count).
    \note       ISR-safe.
*/
size_t stk_kernel_enumerate_tasks(stk_kernel_t *k, stk_task_t **tasks, size_t max_count);

/*! \brief     Manually deliver one scheduler tick to the kernel.
    \param[in] k: Kernel handle.
    \note      Use this when the platform driver's built-in SysTick handler is disabled
               (STK_SYSTICK_HANDLER = _STK_SYSTICK_HANDLER_DISABLE in stk_config.h) and
               the application provides its own tick source. Call from your custom tick ISR
               at the rate matching the tick period configured in stk_kernel_init().
    \note      ISR-safe.
*/
void stk_kernel_process_tick(stk_kernel_t *k);

/*! \brief     Trigger a kernel hard fault (safe-state handler).
    \param[in] k: Kernel handle.
    \note      Normally invoked automatically by the kernel when an HRT task misses its
               deadline. Exposed here for custom fault handlers or test harnesses that need
               to force a controlled system halt via the platform driver's fault path.
    \note      This call does not return.
*/
void stk_kernel_process_hard_fault(stk_kernel_t *k);

// =============================================================================
// Tasks
// =============================================================================

/*! \brief     Create privileged-mode (kernel-mode) task.
    \param[in] entry: Task entry function.
    \param[in] arg: Argument passed to entry function.
    \param[in] stack: Pointer to stack buffer (array of stk_word_t).
    \param[in] stack_size: Number of elements (words) in the stack buffer.
    \return    Task handle (static storage in static kernels, heap in dynamic).
*/
stk_task_t *stk_task_create_privileged(stk_task_entry_t entry,
                                       void *arg,
                                       stk_word_t *stack,
                                       uint32_t stack_size);

/*! \brief     Create user-mode task.
    \param[in] entry: Task entry function.
    \param[in] arg: Argument passed to entry function.
    \param[in] stack: Pointer to stack buffer (array of stk_word_t).
    \param[in] stack_size: Number of elements (words) in the stack buffer.
    \return    Task handle.
*/
stk_task_t *stk_task_create_user(stk_task_entry_t entry,
                                 void *arg,
                                 stk_word_t *stack,
                                 uint32_t stack_size);

/*! \brief     Set task weight (used only by Smooth Weighted Round Robin).
    \param[in] task: Task handle.
    \param[in] weight: Positive weight value (recommended 1–16777215).
    \note      Must be called before adding task to kernel.
    \see       SwitchStrategySmoothWeightedRoundRobin.
*/
void stk_task_set_weight(stk_task_t *task, uint32_t weight);

/*! \brief     Set task priority (used only by Fixed Priority scheduler).
    \param[in] task: Task handle.
    \param[in] priority: Priority level [0 = lowest … 31 = highest].
    \note      Must be called before adding task to kernel.
*/
void stk_task_set_priority(stk_task_t *task, uint8_t priority);

/*! \brief     Assign human-readable task name (for tracing/debugging).
    \param[in] task: Task handle.
    \param[in] tname: Null-terminated string (may be NULL).
*/
void stk_task_set_name(stk_task_t *task, const char *tname);

/*! \brief     Get human-readable task name previously set with stk_task_set_name().
    \param[in] task: Task handle.
    \return    Null-terminated name string, or NULL if not set.
    \note      ISR-safe (reads a stored pointer, no kernel call).
*/
const char *stk_task_get_name(const stk_task_t *task);

/*! \brief     Get the unique identifier of a task.
    \param[in] task: Task handle.
    \return    Task identifier (stk_tid_t). Equivalent to the value stk_tid() returns
               when called from within that task.
    \note      ISR-safe.
    \see       stk_tid
*/
stk_tid_t stk_task_get_id(const stk_task_t *task);

// =============================================================================
// Services available from inside tasks
// =============================================================================

/*! \brief     Returns current task/thread ID (the value set by stk_task_set_id).
    \return    Task identifier (0 if not set).
*/
stk_tid_t stk_tid(void);

/*! \brief     Returns number of ticks elapsed since kernel start.
    \return    Tick count (monotonically increasing).
*/
stk_tick_t stk_ticks(void);

/*! \brief     Returns how many microseconds correspond to one kernel tick.
    \return    Tick resolution in microseconds.
*/
int32_t stk_tick_resolution(void);

/*! \brief     Get ticks from milliseconds using current kernel tick resolution.
    \param[in] msec: Milliseconds to convert.
    \return    Ticks.
    \note      Equivalent to stk::GetTicksFromMsec(msec).
               Requires the kernel to be initialized before calling
               (stk_tick_resolution() must return a valid non-zero value).
*/
stk_tick_t stk_ticks_from_ms(stk_time_t msec);

/*! \brief     Get ticks from milliseconds using an explicit tick resolution.
    \param[in] msec: Milliseconds to convert.
    \param[in] resolution: Microseconds per tick (see stk_tick_resolution()).
    \return    Ticks.
    \note      Equivalent to stk::GetTicksFromMsec(msec, resolution).
               Use this overload when the resolution is already cached to avoid
               a repeated call to stk_tick_resolution().
*/
static inline stk_tick_t stk_ticks_from_ms_r(stk_time_t msec, int32_t resolution)
{
    return msec * 1000 / resolution;
}

/*! \brief     Returns current time in milliseconds since kernel start.
    \return    Time in milliseconds.
*/
stk_time_t stk_time_now_ms(void);

/*! \brief     Convert ticks to milliseconds using an explicit tick resolution.
    \param[in] ticks: Tick count to convert.
    \param[in] resolution: Microseconds per tick (see stk_tick_resolution()).
    \return    Equivalent time in milliseconds.
    \note      ISR-safe (arithmetic only).
*/
static inline stk_time_t stk_ms_from_ticks_r(stk_tick_t ticks, int32_t resolution)
{
    return (ticks * resolution) / 1000;
}

/*! \brief     Convert ticks to milliseconds using the current kernel tick resolution.
    \param[in] ticks: Tick count to convert.
    \return    Equivalent time in milliseconds.
    \note      Requires the kernel to be initialized before calling.
*/
static inline stk_time_t stk_ms_from_ticks(stk_tick_t ticks)
{
    return stk_ms_from_ticks_r(ticks, stk_tick_resolution());
}

/*! \brief     Get raw system timer counter value.
    \return    64-bit hardware counter value. Useful for sub-tick timing measurements.
    \note      ISR-safe.
*/
uint64_t stk_sys_timer_count(void);

/*! \brief     Get system timer frequency in Hz.
    \return    Timer frequency (Hz). Divide stk_sys_timer_count() differences by this value
               to obtain elapsed time in seconds.
    \note      ISR-safe.
*/
uint32_t stk_sys_timer_frequency(void);

// =============================================================================
// High-Resolution Clock  (see hw::HiResClock)
// =============================================================================

/*! \brief     Get raw CPU cycle counter.
    \return    64-bit cycle count. Resolution is one clock cycle.
    \note      ISR-safe. Use stk_hires_frequency() to convert to real time.
*/
uint64_t stk_hires_cycles(void);

/*! \brief     Get CPU clock frequency in Hz.
    \return    Clock frequency in Hz.
    \note      ISR-safe.
*/
uint32_t stk_hires_frequency(void);

/*! \brief     Get elapsed time in microseconds from the high-resolution clock.
    \return    Microseconds since an arbitrary but fixed epoch (typically reset).
    \note      ISR-safe. Equivalent to stk_hires_cycles() * 1000000 / stk_hires_frequency().
*/
stk_tick_t stk_hires_time_us(void);

/*! \brief     Busy-wait delay (other tasks continue to run).
    \param[in] ticks: Ticks to delay.
*/
void stk_delay(stk_timeout_t ticks);

/*! \brief     Busy-wait delay (other tasks continue to run).
    \param[in] ms: Milliseconds to delay.
*/
void stk_delay_ms(stk_timeout_t ms);

/*! \brief     Put current task to sleep (non-HRT kernels only).
    \param[in] ms: Ticks to sleep.
    \note      Unlike stk_delay_ms(), this function does not spin and allows the kernel to
               idle the CPU. When \a KERNEL_TICKLESS is active and all tasks are sleeping,
               the SysTick is suppressed and the CPU enters a low-power WFI state until the
               nearest wake-up deadline.
    \note      Unsupported in \a KERNEL_HRT mode; in HRT mode tasks sleep automatically
               according to their periodicity and workload, use stk_yield instead.
    \see       stk_sleep_ms, stk_sleep_until, stk_ticks
*/
void stk_sleep(stk_timeout_t ticks);

/*! \brief     Put current task to sleep (non-HRT kernels only).
    \param[in] ms: Milliseconds to sleep.
    \note      Unlike stk_delay_ms(), this function does not spin and allows the kernel to
               idle the CPU. When \a KERNEL_TICKLESS is active and all tasks are sleeping,
               the SysTick is suppressed and the CPU enters a low-power WFI state until the
               nearest wake-up deadline.
    \note      Unsupported in \a KERNEL_HRT mode; in HRT mode tasks sleep automatically
               according to their periodicity and workload, use stk_yield instead.
    \see       stk_sleep, stk_sleep_until, stk_ticks
*/
void stk_sleep_ms(stk_timeout_t ms);

/*! \brief     Put current task to sleep (non-HRT kernels only).
    \param[in] ts: Absolute time, a deadline for a sleep period.
    \note      Unlike stk_delay_ms(), this function does not spin and allows the kernel to
               idle the CPU. When \a KERNEL_TICKLESS is active and all tasks are sleeping,
               the SysTick is suppressed and the CPU enters a low-power WFI state until the
               nearest wake-up deadline.
    \note      Unsupported in \a KERNEL_HRT mode; in HRT mode tasks sleep automatically
               according to their periodicity and workload, use stk_yield instead.
    \see       stk_sleep, stk_sleep_ms, stk_ticks
*/
void stk_sleep_until(stk_tick_t ts);

/*! \brief     Voluntarily give up CPU to another ready task (cooperative yield).
*/
void stk_yield(void);

/*! \brief     Cancel the sleep of a task, waking it immediately.
    \param[in] tid: Identifier of the task to wake (obtained from stk_tid() or
               stk_task_get_id()).
    \note      No-op if the target task is not currently sleeping.
    \note      ISR-safe.
    \see       stk_sleep, stk_sleep_ms, stk_sleep_until
*/
void stk_sleep_cancel(stk_tid_t tid);

// =============================================================================
// Dynamic cleanup
// =============================================================================

/*! \brief     Destroy dynamic kernel instance (only when not running).
    \param[in] k: Kernel handle.
    \note      Kernel must not be running (all tasks must have exited or been removed).
               Only valid for kernels created with dynamic factory functions.
*/
void stk_kernel_destroy(stk_kernel_t *k);

/*! \brief     Destroy dynamically created task object.
    \param[in] task: Task handle.
    \note      Only valid for tasks created with dynamic creation functions.
               Task must no longer be scheduled (must have exited or been removed).
*/
void stk_task_destroy(stk_task_t *task);

// =============================================================================
// Thread-Local Storage (TLS)
// =============================================================================

/*! \example
    \code
    typedef struct {
        int task_counter;
        void *user_context;
    } my_task_local_t;

    // In task code:
    static my_task_local_t my_data = { .task_counter = 0 };
    STK_TLS_SET(&my_data);

    // Later in the same task:
    my_task_local_t *tls = STK_TLS_GET(my_task_local_t);
    tls->task_counter++;
    \endcode
*/

/*! \brief     Get thread-local pointer (platform-specific slot).
    \return    Pointer previously stored with stk_tls_set() (NULL if never set).
*/
void *stk_tls_get(void);

/*! \brief     Set thread-local pointer.
    \param[in] ptr: Pointer value to store for the current task/thread.
*/
void stk_tls_set(void *ptr);

/*! \brief     Typed helper for getting TLS value.
    \note      Expands to ((type *)stk_tls_get())
*/
#define STK_TLS_GET(type) ((type *)stk_tls_get())

/*! \brief     Typed helper for setting TLS value.
    \note      Expands to stk_tls_set((void *)(ptr))
*/
#define STK_TLS_SET(ptr) stk_tls_set((void *)(ptr))

// =============================================================================
// Synchronization Primitives
// =============================================================================

// ----- Critical Section ------------------------------------------------------

/*! \brief     Enter critical section - disable context switches on current core.
    \note      Supports nesting (number of enter calls must match number of exit calls).
*/
void stk_critical_section_enter(void);

/*! \brief     Leave critical section - re-enable context switches.
    \note      Must be called once for each previous stk_critical_section_enter().
*/
void stk_critical_section_exit(void);

// ----- Mutex -----------------------------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for Mutex instance.
*/
#define STK_MUTEX_IMPL_SIZE (10 + (STK_SYNC_DEBUG_NAMES ? 1 : 0))

/*! \brief     Opaque memory container for a Mutex instance.
*/
typedef struct stk_mutex_mem_t {
    stk_word_t data[STK_MUTEX_IMPL_SIZE] __stk_c_aligned;
} stk_mutex_mem_t;

/*! \brief     Opaque handle to a Mutex instance.
*/
typedef struct stk_mutex_t stk_mutex_t;

/*! \brief     Create a Mutex (using provided memory).
    \param[in] memory: Pointer to static memory container.
    \param[in] memory_size: Size of the container (must be >= sizeof(stk_mutex_mem_t)).
    \return    Mutex handle.
*/
stk_mutex_t *stk_mutex_create(stk_mutex_mem_t *memory, uint32_t memory_size);

/*! \brief     Destroy a Mutex.
    \param[in] mtx: Mutex handle.
*/
void stk_mutex_destroy(stk_mutex_t *mtx);

/*! \brief     Lock the mutex. Blocks until available.
    \param[in] mtx: Mutex handle.
*/
void stk_mutex_lock(stk_mutex_t *mtx);

/*! \brief     Try locking the mutex. Does not block if already locked.
    \param[in] mtx: Mutex handle.
    \return    True if locked successfully, False on timeout.
*/
bool stk_mutex_trylock(stk_mutex_t *mtx);

/*! \brief     Unlock the mutex.
    \param[in] mtx: Mutex handle.
*/
void stk_mutex_unlock(stk_mutex_t *mtx);

/*! \brief     Try to lock the mutex with a timeout.
    \param[in] mtx: Mutex handle.
    \param[in] timeout: Max time to wait in milliseconds.
    \return    True if locked successfully, False on timeout.
*/
bool stk_mutex_timed_lock(stk_mutex_t *mtx, stk_timeout_t timeout);

// ----- SpinLock --------------------------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for SpinLock instance.
*/
#define STK_SPINLOCK_IMPL_SIZE (1)

/*! \struct    stk_spinlock_mem_t
    \brief     Opaque memory container for SpinLock object.
*/
typedef struct {
    stk_word_t data[STK_SPINLOCK_IMPL_SIZE];
} stk_spinlock_mem_t;

/*! \brief     Opaque handle to a SpinLock instance.
*/
typedef struct stk_spinlock_t stk_spinlock_t;

/*! \brief     Create a recursive SpinLock.
    \param[in] memory: Pointer to static memory container.
    \param[in] memory_size: Size of the container (must be >= sizeof(stk_spinlock_mem_t)).
    \return    SpinLock handle.
*/
stk_spinlock_t *stk_spinlock_create(stk_spinlock_mem_t *memory, uint32_t memory_size);

/*! \brief     Destroy the SpinLock.
*/
void stk_spinlock_destroy(stk_spinlock_t *lock);

/*! \brief     Acquire the SpinLock (recursive).
*/
void stk_spinlock_lock(stk_spinlock_t *lock);

/*! \brief     Attempt to acquire the SpinLock immediately.
    \return    True if locked successfully, False otherwise.
*/
bool stk_spinlock_trylock(stk_spinlock_t *lock);

/*! \brief     Release the SpinLock.
*/
void stk_spinlock_unlock(stk_spinlock_t *lock);

// ----- Condition Variable ----------------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for ConditionVariable instance.
*/
#define STK_CV_IMPL_SIZE (7 + (STK_SYNC_DEBUG_NAMES ? 1 : 0))

/*! \brief     Opaque memory container for a ConditionVariable instance.
*/
typedef struct stk_cv_mem_t {
    stk_word_t data[STK_CV_IMPL_SIZE] __stk_c_aligned;
} stk_cv_mem_t;

/*! \brief     Opaque handle to a Condition Variable instance.
*/
typedef struct stk_cv_t stk_cv_t;

/*! \brief     Create a Condition Variable (using provided memory).
    \param[in] memory:      Pointer to static memory container.
    \param[in] memory_size: Size of the container (must be >= sizeof(stk_cv_mem_t)).
    \return    CV handle.
*/
stk_cv_t *stk_cv_create(stk_cv_mem_t *memory, uint32_t memory_size);

/*! \brief     Destroy a Condition Variable.
    \param[in] cv: CV handle.
*/
void stk_cv_destroy(stk_cv_t *cv);

/*! \brief     Wait for a signal on the condition variable.
    \details   Atomically releases the mutex and suspends the task.
               The mutex is re-acquired before returning.
    \param[in] cv: CV handle.
    \param[in] mtx: Locked mutex handle protecting the state.
    \param[in] timeout: Max time to wait (or \a STK_WAIT_INFINITE).
    \return    True if signaled, False on timeout.
*/
bool stk_cv_wait(stk_cv_t *cv, stk_mutex_t *mtx, stk_timeout_t timeout);

/*! \brief     Wake one task waiting on the condition variable.
    \param[in] cv: CV handle.
*/
void stk_cv_notify_one(stk_cv_t *cv);

/*! \brief     Wake all tasks waiting on the condition variable.
    \param[in] cv: CV handle.
*/
void stk_cv_notify_all(stk_cv_t *cv);

// ----- Event -----------------------------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for Event instance.
*/
#define STK_EVENT_IMPL_SIZE (8 + (STK_SYNC_DEBUG_NAMES ? 1 : 0))

/*! \brief     Opaque memory container for an Event instance.
*/
typedef struct stk_event_mem_t {
    stk_word_t data[STK_EVENT_IMPL_SIZE] __stk_c_aligned;
} stk_event_mem_t;

/*! \brief     Opaque handle to an Event instance.
*/
typedef struct stk_event_t stk_event_t;

/*! \brief     Create an Event (using provided memory).
    \param[in] memory: Pointer to static memory container.
    \param[in] memory_size: Size of the container (must be >= sizeof(stk_event_mem_t)).
    \param[in] manual_reset: True for manual-reset, False for auto-reset.
    \return    Event handle.
*/
stk_event_t *stk_event_create(stk_event_mem_t *memory, uint32_t memory_size, bool manual_reset);

/*! \brief     Destroy an Event.
    \param[in] ev: Event handle.
*/
void stk_event_destroy(stk_event_t *ev);

/*! \brief     Wait for the event to become signaled.
    \param[in] ev:      Event handle.
    \param[in] timeout: Max time to wait in milliseconds.
    \return    True if signaled, False on timeout.
*/
bool stk_event_wait(stk_event_t *ev, stk_timeout_t timeout);

/*! \brief     Wait for the event to become signaled.
    \param[in] ev: Event handle.
    \return    True if signaled, False on timeout.
*/
bool stk_event_trywait(stk_event_t *ev);

/*! \brief     Set the event to signaled state.
    \param[in] ev: Event handle.
*/
void stk_event_set(stk_event_t *ev);

/*! \brief     Reset the event to non-signaled state.
    \param[in] ev: Event handle.
*/
void stk_event_reset(stk_event_t *ev);

/*! \brief     Pulse the event (signal then immediately reset).
    \param[in] ev: Event handle.
*/
void stk_event_pulse(stk_event_t *ev);

// ----- Semaphore -------------------------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for Semaphore instance.
*/
#define STK_SEM_IMPL_SIZE (8 + (STK_SYNC_DEBUG_NAMES ? 1 : 0))

/*! \brief     Opaque memory container for a Semaphore instance.
*/
typedef struct stk_sem_mem_t {
    stk_word_t data[STK_SEM_IMPL_SIZE] __stk_c_aligned;
} stk_sem_mem_t;

/*! \brief     Opaque handle to a Semaphore instance.
*/
typedef struct stk_sem_t stk_sem_t;

/*! \brief     Create a Semaphore (using provided memory).
    \param[in] memory: Pointer to static memory container.
    \param[in] memory_size: Size of the container (must be >= sizeof(stk_sem_mem_t)).
    \param[in] initial_count: Starting value of the resource counter.
    \param[in] max_count: Maximum value the counter is allowed to reach.
               Pass 0 to use the default maximum (65534). Must be > initial_count.
    \return    Semaphore handle.
*/
stk_sem_t *stk_sem_create(stk_sem_mem_t *memory, uint32_t memory_size,
                           uint32_t initial_count, uint32_t max_count);

/*! \brief     Destroy a Semaphore.
    \param[in] sem: Semaphore handle.
*/
void stk_sem_destroy(stk_sem_t *sem);

/*! \brief     Wait for a semaphore resource.
    \param[in] sem: Semaphore handle.
    \param[in] timeout: Max time to wait (ticks).
    \return    True if resource acquired, False on timeout.
*/
bool stk_sem_wait(stk_sem_t *sem, stk_timeout_t timeout);

/*! \brief     Poll the semaphore without blocking.
    \details   Acquires a resource token if one is immediately available; returns
               \c false instantly if the counter is zero.
    \param[in] sem: Semaphore handle.
    \return    True if a token was acquired, False if count was zero.
    \warning   ISR-safe.
*/
bool stk_sem_trywait(stk_sem_t *sem);

/*! \brief     Signal/Release a semaphore resource.
    \param[in] sem: Semaphore handle.
*/
void stk_sem_signal(stk_sem_t *sem);

/*! \brief     Get the current counter value.
    \param[in] sem: Semaphore handle.
    \return    Advisory snapshot of the counter. May be stale by the time
               the caller acts on it.
    \note      ISR-safe on targets where a 16-bit aligned read is atomic.
*/
uint16_t stk_sem_get_count(const stk_sem_t *sem);

// ----- EventFlags ------------------------------------------------------------

/*! \brief     Options bitmask constants for stk_ef_wait() / stk_ef_trywait().
*/
#define STK_EF_OPT_WAIT_ANY (0x00000000U) /*!< Unblock when ANY requested bit is set (OR semantics, default) */
#define STK_EF_OPT_WAIT_ALL (0x00000001U) /*!< Unblock when ALL requested bits are simultaneously set (AND semantics) */
#define STK_EF_OPT_NO_CLEAR (0x00000002U) /*!< Do not clear matched bits on a successful return */

/*! \brief     Return-value error sentinels (bit 31 set indicates an error).
*/
#define STK_EF_ERROR_PARAMETER (0x80000001U) /*!< flags argument is 0 or has bit 31 set */
#define STK_EF_ERROR_TIMEOUT   (0x80000002U) /*!< Timeout expired before the flag condition was met */
#define STK_EF_ERROR_ISR       (0x80000004U) /*!< Wait called from an ISR with a blocking timeout */
#define STK_EF_ERROR_MASK      (0x80000000U) /*!< Mask for testing any error; bit 31 set means error */

/*! \brief     Returns true if a value returned by stk_ef_set(), stk_ef_clear(),
               stk_ef_wait(), or stk_ef_trywait() is an error sentinel (bit 31 set).
*/
static inline bool stk_ef_is_error(uint32_t result) { return ((result & STK_EF_ERROR_MASK) != 0U); }

/*! \brief     A memory size (multiples of stk_word_t) required for EventFlags instance.
    \note      EventFlags contains one ConditionVariable (STK_CV_IMPL_SIZE words)
               plus one 32-bit flags word and alignment padding.
*/
#define STK_EF_IMPL_SIZE (STK_CV_IMPL_SIZE + 1 + (STK_SYNC_DEBUG_NAMES ? 1 : 0))

/*! \brief     Opaque memory container for an EventFlags instance.
*/
typedef struct stk_ef_mem_t {
    stk_word_t data[STK_EF_IMPL_SIZE] __stk_c_aligned;
} stk_ef_mem_t;

/*! \brief     Opaque handle to an EventFlags instance.
*/
typedef struct stk_ef_t stk_ef_t;

/*! \brief     Create an EventFlags object (using provided memory).
    \param[in] memory: Pointer to static memory container.
    \param[in] memory_size: Size of the container (must be >= sizeof(stk_ef_mem_t)).
    \param[in] initial_flags: Initial value of the 32-bit flags word (bits 0..30 only;
               bit 31 is reserved and must not be set).
    \return    EventFlags handle, or NULL if memory is too small.
*/
stk_ef_t *stk_ef_create(stk_ef_mem_t *memory, uint32_t memory_size, uint32_t initial_flags);

/*! \brief     Destroy an EventFlags object.
    \param[in] ef: EventFlags handle.
*/
void stk_ef_destroy(stk_ef_t *ef);

/*! \brief     Set one or more flags.
    \details   Atomically OR-sets the specified bits and wakes all current waiters
               so each can re-evaluate its own predicate.
    \param[in] ef:EventFlags handle.
    \param[in] flags: Bitmask of bits to set. Must not be 0 and must not have bit 31 set.
    \return    Flags word value after setting, or \c STK_EF_ERROR_PARAMETER on invalid input.
    \note      ISR-safe.
*/
uint32_t stk_ef_set(stk_ef_t *ef, uint32_t flags);

/*! \brief     Clear one or more flags.
    \details   Atomically clears the specified bits.
    \param[in] ef: EventFlags handle.
    \param[in] flags: Bitmask of bits to clear. Must not be 0 and must not have bit 31 set.
    \return    Flags word value before clearing, or \c STK_EF_ERROR_PARAMETER on invalid input.
    \note      ISR-safe.
*/
uint32_t stk_ef_clear(stk_ef_t *ef, uint32_t flags);

/*! \brief     Read the current flags word without modifying it.
    \param[in] ef: EventFlags handle.
    \return    Point-in-time snapshot of the 32-bit flags word.
    \note      ISR-safe. Never blocks or modifies state.
*/
uint32_t stk_ef_get(stk_ef_t *ef);

/*! \brief     Wait for one or more flags to be set.
    \details   Suspends the calling task until the requested flag condition is satisfied
               or the timeout expires. On success, matched bits are atomically cleared
               unless \c STK_EF_OPT_NO_CLEAR is set in options.
    \param[in] ef: EventFlags handle.
    \param[in] flags: Bitmask of flag bits to watch. Must not be 0 and must not have bit 31 set.
    \param[in] options: Combination of \c STK_EF_OPT_WAIT_ANY / \c STK_EF_OPT_WAIT_ALL
               and optionally \c STK_EF_OPT_NO_CLEAR.
    \param[in] timeout: Maximum time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking poll.
    \return    Bitmask of the matched flags on success, or a \c STK_EF_ERROR_* sentinel
               on failure. Always check \c stk_ef_is_error() before using the return
               value as a flags mask.
    \note      If the predicate becomes satisfied in the same tick that the deadline
               expires, the wait succeeds and returns the matched flags.
    \warning   ISR-safe only with timeout = \c STK_NO_WAIT, ISR-unsafe otherwise.
*/
uint32_t stk_ef_wait(stk_ef_t *ef, uint32_t flags, uint32_t options, stk_timeout_t timeout);

/*! \brief     Non-blocking flag poll.
    \details   Checks immediately whether the flag condition is satisfied.
               Clears matched bits on success unless \c STK_EF_OPT_NO_CLEAR is set.
    \param[in] ef: EventFlags handle.
    \param[in] flags: Bitmask of flag bits to watch.
    \param[in] options: \c STK_EF_OPT_WAIT_ANY (default) or \c STK_EF_OPT_WAIT_ALL,
               optionally OR-ed with \c STK_EF_OPT_NO_CLEAR.
    \return    Matched flags bitmask on success, or \c STK_EF_ERROR_TIMEOUT immediately
               if the condition is not met.
    \note      ISR-safe.
*/
uint32_t stk_ef_trywait(stk_ef_t *ef, uint32_t flags, uint32_t options);

// ----- Pipe (FIFO) -----------------------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for a Pipe control-block.
    \details   Covers the fixed-overhead fields of stk::sync::Pipe:
               one uint8_t pointer plus five size_t members (capacity, element_size,
               count, head, tail), two ConditionVariable objects (cv_not_empty,
               cv_not_full) and an optional debug-name word.
    \note      The backing data buffer is allocated separately by the caller and
               passed to stk_pipe_create() via the \a buf / \a buf_size parameters.
*/
#define STK_PIPE_IMPL_SIZE (6 + (2 * STK_CV_IMPL_SIZE) + (STK_SYNC_DEBUG_NAMES ? 1 : 0))

/*! \brief     Opaque memory container for a Pipe control-block.
*/
typedef struct stk_pipe_mem_t {
    stk_word_t data[STK_PIPE_IMPL_SIZE] __stk_c_aligned;
} stk_pipe_mem_t;

/*! \brief     Opaque handle to a Pipe instance.
*/
typedef struct stk_pipe_t stk_pipe_t;

/*! \def       STK_PIPE_BUF_SIZE(capacity, element_size)
    \brief     Compute the required data-buffer size (in bytes) for a Pipe.
    \param[in] capacity: Maximum number of elements the pipe can hold.
    \param[in] element_size: Size of a single element in bytes.
    \note      Use this macro when declaring the \c uint8_t buffer passed to stk_pipe_create():

    \code
    #define MY_PIPE_CAP   16
    #define MY_ELEM_SIZE  sizeof(MyMsg_t)

    static uint8_t        s_pipe_buf[STK_PIPE_BUF_SIZE(MY_PIPE_CAP, MY_ELEM_SIZE)] __stk_c_aligned;
    static stk_pipe_mem_t s_pipe_mem;
    stk_pipe_t *g_pipe = stk_pipe_create(&s_pipe_mem, sizeof(s_pipe_mem),
                                          s_pipe_buf, sizeof(s_pipe_buf),
                                          MY_PIPE_CAP, MY_ELEM_SIZE);
    \endcode
*/
#define STK_PIPE_BUF_SIZE(capacity, element_size) \
    ((((capacity) * (element_size)) + STK_ALIGN_MASK) & ~STK_ALIGN_MASK)

/*! \brief     Create a Pipe (using provided memory).
    \details   Constructs a stk::sync::Pipe in-place inside \a memory. The pipe will
               hold up to \a capacity elements, each \a element_size bytes wide.
               The backing ring-buffer storage must be supplied by the caller via
               \a buf / \a buf_size.
    \param[in] memory: Pointer to static memory container for the Pipe control-block.
               Must be at least sizeof(stk_pipe_mem_t) bytes.
    \param[in] memory_size: Size of \a memory in bytes.
    \param[in] buf: Pointer to the element data buffer.
               Must be at least \a capacity * \a element_size bytes.
    \param[in] buf_size: Size of \a buf in bytes (used for the safety assertion;
               must equal \a capacity * \a element_size).
    \param[in] capacity: Maximum number of elements [1, 65534].
    \param[in] element_size: Size of each individual element in bytes (>= 1).
    \return    Pipe handle, or NULL if any size assertion fails.
    \note      Convenience macro STK_PIPE_BUF_SIZE(capacity, element_size) computes
               the required \a buf_size.
    \note      Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
stk_pipe_t *stk_pipe_create(stk_pipe_mem_t *memory,
                            uint32_t        memory_size,
                            uint8_t        *buf,
                            uint32_t        buf_size,
                            size_t          capacity,
                            size_t          element_size);

/*! \brief     Destroy a Pipe.
    \param[in] pipe: Pipe handle.
    \note      Any tasks still blocked on Write/Read at destruction time are considered
               a logic error; an assertion is triggered in debug builds.
*/
void stk_pipe_destroy(stk_pipe_t *pipe);

/*! \brief     Write a single element to the pipe.
    \details   Copies \a element_size bytes from \a data into the next available slot.
               Blocks if the pipe is full until space becomes available or the timeout
               expires.
    \param[in] pipe: Pipe handle.
    \param[in] data: Pointer to the element payload (must be >= element_size bytes).
    \param[in] timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return    True if the element was written, False on timeout.
    \warning   ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
bool stk_pipe_write(stk_pipe_t *pipe, const void *data, stk_timeout_t timeout);

/*! \brief     Attempt to write a single element to the pipe without blocking.
    \param[in] pipe: Pipe handle.
    \param[in] data: Pointer to the element payload.
    \return    True if written, False if the pipe was full.
    \warning   ISR-safe.
*/
bool stk_pipe_trywrite(stk_pipe_t *pipe, const void *data);

/*! \brief     Read a single element from the pipe.
    \details   Copies \a element_size bytes from the oldest slot into the buffer
               pointed to by \a data. Blocks if the pipe is empty until data is
               produced or the timeout expires.
    \param[in] pipe: Pipe handle.
    \param[out] data: Destination buffer (must be >= element_size bytes).
    \param[in] timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return    True if an element was read, False on timeout.
    \warning   ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
bool stk_pipe_read(stk_pipe_t *pipe, void *data, stk_timeout_t timeout);

/*! \brief     Attempt to read a single element from the pipe without blocking.
    \param[in] pipe: Pipe handle.
    \param[out] data: Destination buffer (must be >= element_size bytes).
    \return    True if an element was read, False if the pipe was empty.
    \warning   ISR-safe.
*/
bool stk_pipe_tryread(stk_pipe_t *pipe, void *data);

/*! \brief     Write multiple elements to the pipe.
    \details   Copies a block of \a count elements. Blocks until the full amount is
               written or the timeout expires.
    \param[in] pipe: Pipe handle.
    \param[in] src: Pointer to the source array (must hold at least \a count
               elements of \a element_size bytes each).
    \param[in] count: Number of elements to write.
    \param[in] timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return    Number of elements actually written. Equal to \c count unless a timeout
               occurred.
    \warning   ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
size_t stk_pipe_write_bulk(stk_pipe_t *pipe, const void *src, size_t count, stk_timeout_t timeout);

/*! \brief     Attempt to write multiple elements to the pipe without blocking.
    \details   Copies as many elements as possible. Elements that do not fit are discarded.
    \param[in] pipe: Pipe handle.
    \param[in] src: Pointer to the source array.
    \param[in] count: Number of elements to write.
    \return    Number of elements actually written.
    \warning   ISR-safe.
*/
size_t stk_pipe_trywrite_bulk(stk_pipe_t *pipe, const void *src, size_t count);

/*! \brief     Read multiple elements from the pipe.
    \details   Attempts to retrieve \a count elements from the FIFO. Blocks until the
               full amount is read or the timeout expires.
    \param[in] pipe: Pipe handle.
    \param[out] dst: Pointer to the destination array (must hold at least \a count
               elements of \a element_size bytes each).
    \param[in] count: Number of elements to read.
    \param[in] timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return    Number of elements actually read. Equal to \c count unless a timeout occurred.
    \warning   ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
size_t stk_pipe_read_bulk(stk_pipe_t *pipe, void *dst, size_t count, stk_timeout_t timeout);

/*! \brief     Attempt to read multiple elements from the pipe without blocking.
    \details   Reads as many elements as are currently available without blocking.
    \param[in] pipe: Pipe handle.
    \param[out] dst: Pointer to the destination array.
    \param[in] count: Number of elements to read.
    \return    Number of elements actually read.
    \warning   ISR-safe.
*/
size_t stk_pipe_tryread_bulk(stk_pipe_t *pipe, void *dst, size_t count);

/*! \brief     Read at least \a trigger elements, then drain up to \a max_count without
               blocking.
    \details   Blocks until at least \a trigger elements are simultaneously available
               in the pipe, or the timeout expires. Once the threshold is reached the
               call dequeues min(max_count, available) elements in a single atomic pass.
    \param[in] pipe: Pipe handle.
    \param[out] dst: Destination buffer; must hold at least \a max_count elements
               of \a element_size bytes.
    \param[in] trigger: Minimum number of elements that must be available before any
               data is dequeued. Clamped to [1, max_count] internally.
    \param[in] max_count: Maximum number of elements to return in total.
    \param[in] timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return    Number of elements actually read (0 if timeout fired before trigger was reached).
    \warning   ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
size_t stk_pipe_read_bulk_triggered(stk_pipe_t *pipe,
                                    void       *dst,
                                    size_t      trigger,
                                    size_t      max_count,
                                    int32_t     timeout);

/*! \brief     Non-blocking variant of stk_pipe_read_bulk_triggered.
    \details   Returns immediately with however many elements are available, up to
               \a max_count. The trigger threshold is not enforced.
    \param[in] pipe: Pipe handle.
    \param[out] dst: Destination buffer.
    \param[in] max_count: Maximum number of elements to read.
    \return    Number of elements actually read.
    \warning   ISR-safe.
*/
size_t stk_pipe_tryread_bulk_triggered(stk_pipe_t *pipe, void *dst, size_t max_count);

/*! \brief     Discard all elements and reset the pipe to the empty state.
    \details   Any tasks blocked in Write() are woken so they can re-evaluate.
    \param[in] pipe: Pipe handle.
    \warning   Elements that were in the pipe are silently discarded.
    \warning   ISR-safe.
*/
void stk_pipe_reset(stk_pipe_t *pipe);

/*! \brief     Get the maximum number of elements the pipe can hold.
    \param[in] pipe: Pipe handle.
    \return    Construction-time capacity.
    \note      ISR-safe.
*/
size_t stk_pipe_get_capacity(const stk_pipe_t *pipe);

/*! \brief     Get the size of each element in bytes.
    \param[in] pipe: Pipe handle.
    \return    Construction-time element size.
    \note      ISR-safe.
*/
size_t stk_pipe_get_element_size(const stk_pipe_t *pipe);

/*! \brief     Get the current number of elements in the pipe.
    \param[in] pipe: Pipe handle.
    \return    Point-in-time snapshot of the element count.
    \note      ISR-safe on targets where a size_t-aligned read is atomic.
*/
size_t stk_pipe_get_count(const stk_pipe_t *pipe);

/*! \brief     Get the number of free slots currently available.
    \param[in] pipe: Pipe handle.
    \return    Point-in-time snapshot of the free-slot count.
    \note      ISR-safe.
*/
size_t stk_pipe_get_space(const stk_pipe_t *pipe);

/*! \brief     Check whether the pipe is currently empty.
    \param[in] pipe: Pipe handle.
    \return    True if the pipe contains no elements.
    \note      ISR-safe.
*/
bool stk_pipe_is_empty(const stk_pipe_t *pipe);

/*! \brief     Check whether the pipe is currently full.
    \param[in] pipe: Pipe handle.
    \return    True if the pipe contains \a capacity elements.
    \note      ISR-safe.
*/
bool stk_pipe_is_full(const stk_pipe_t *pipe);

/*! \brief     Verify that the backing storage is valid and the pipe is ready for use.
    \param[in] pipe: Pipe handle.
    \return    True if the pipe is ready for use.
    \note      ISR-safe.
*/
bool stk_pipe_is_storage_valid(const stk_pipe_t *pipe);

// ----- MessageQueue ----------------------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for a MessageQueue instance.
    \details   Covers the fixed-overhead fields of stk::sync::MessageQueue:
               six size_t members (buffer ptr, capacity, msg_size, count, head, tail)
               plus two ConditionVariable objects (cv_not_empty, cv_not_full) and
               optional debug-name word.
    \note      The backing data buffer is allocated separately by the caller and
               passed to stk_msgq_create() via the \a buf / \a buf_size parameters.
*/
#define STK_MSGQ_IMPL_SIZE (6 + (2 * STK_CV_IMPL_SIZE) + (STK_SYNC_DEBUG_NAMES ? 1 : 0))

/*! \brief     Opaque memory container for a MessageQueue instance.
*/
typedef struct stk_msgq_mem_t {
    stk_word_t data[STK_MSGQ_IMPL_SIZE] __stk_c_aligned;
} stk_msgq_mem_t;

/*! \brief     Opaque handle to a MessageQueue instance.
*/
typedef struct stk_msgq_t stk_msgq_t;

/*! \def       STK_MSGQ_BUF_SIZE(capacity, msg_size)
    \brief     Compute the required data-buffer size (in bytes) for a MessageQueue.
    \param[in] capacity: Maximum number of messages.
    \param[in] msg_size: Size of each message in bytes.
    \note      Use this macro when declaring the \c uint8_t buffer passed to
               stk_msgq_create():
    \code
    #define MY_QUEUE_CAP     8
    #define MY_MSG_SIZE      sizeof(MyMsg_t)

    static uint8_t        s_msgq_buf[STK_MSGQ_BUF_SIZE(MY_QUEUE_CAP, MY_MSG_SIZE)];
    static stk_msgq_mem_t s_msgq_mem;
    stk_msgq_t *g_queue = stk_msgq_create(&s_msgq_mem, sizeof(s_msgq_mem),
                                           s_msgq_buf,  sizeof(s_msgq_buf),
                                           MY_QUEUE_CAP, MY_MSG_SIZE);
    \endcode
*/
#define STK_MSGQ_BUF_SIZE(capacity, msg_size) ((capacity) * (msg_size))

/*! \brief     Create a MessageQueue (using provided memory).
    \details   Constructs a stk::sync::MessageQueue in-place inside \a memory.
               The queue will hold up to \a capacity messages, each \a msg_size bytes
               wide.  The backing storage for messages must be supplied by the caller
               via \a buf / \a buf_size.

    \param[in] memory: Pointer to static memory container for the queue object.
               Must be at least sizeof(stk_msgq_mem_t) bytes.
    \param[in] memory_size: Size of \a memory in bytes (must be >= sizeof(stk_msgq_mem_t)).
    \param[in] buf: Pointer to the message data buffer.
               Must be at least \a capacity * \a msg_size bytes.
    \param[in] buf_size: Size of \a buf in bytes (used only for the safety assertion; must equal
               \a capacity * \a msg_size).
    \param[in] capacity: Maximum number of messages [1, 65534].
    \param[in] msg_size: Size of each individual message in bytes (>= 1).
    \return    MessageQueue handle, or NULL if any size assertion fails.

    \note      Convenience macro STK_MSGQ_BUF_SIZE(capacity, msg_size) computes
               the required \a buf_size.
    \note      Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
stk_msgq_t *stk_msgq_create(stk_msgq_mem_t *memory,
                             uint32_t        memory_size,
                             uint8_t        *buf,
                             uint32_t        buf_size,
                             size_t          capacity,
                             size_t          msg_size);

/*! \brief     Destroy a MessageQueue.
    \param[in] mq: MessageQueue handle.
    \note      Any tasks still blocked on Put/Get at destruction time are considered
               a logic error; an assertion is triggered in debug builds.
*/
void stk_msgq_destroy(stk_msgq_t *mq);

/*! \brief     Put a message into the queue.
    \details   Copies \a msg_size bytes from \a msg into the next available slot.
               Blocks if the queue is full until space becomes available or the
               timeout expires.
    \param[in] mq: MessageQueue handle.
    \param[in] msg: Pointer to the message payload (must be >= msg_size bytes).
    \param[in] timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return    True if the message was enqueued, False on timeout.
    \warning   ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
bool stk_msgq_put(stk_msgq_t *mq, const void *msg, stk_timeout_t timeout);

/*! \brief     Attempt to put a message into the queue without blocking.
    \param[in] mq: MessageQueue handle.
    \param[in] msg: Pointer to the message payload.
    \return    True if enqueued, False if the queue was full.
    \warning   ISR-safe.
*/
bool stk_msgq_tryput(stk_msgq_t *mq, const void *msg);

/*! \brief     Put a message into the front of the queue (priority insert).
    \details   Copies \a msg_size bytes from \a msg into the slot immediately
               before the current read pointer, making it the next message
               that stk_msgq_get() will return. Blocks if the queue is full
               until space becomes available or the timeout expires.
    \param[in] mq: MessageQueue handle.
    \param[in] msg: Pointer to the message payload (must be >= msg_size bytes).
    \param[in] timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
               indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return    True if the message was enqueued at the front, False on timeout.
    \warning   ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
bool stk_msgq_putfront(stk_msgq_t *mq, const void *msg, stk_timeout_t timeout);

/*! \brief     Attempt to put a message into the front of the queue without blocking.
    \param[in] mq: MessageQueue handle.
    \param[in] msg: Pointer to the message payload.
    \return    True if enqueued at the front, False if the queue was full.
    \warning   ISR-safe.
*/
bool stk_msgq_tryputfront(stk_msgq_t *mq, const void *msg);

/*! \brief      Get a message from the queue.
    \details    Copies the oldest message into \a msg. Blocks if the queue is
                empty until a message arrives or the timeout expires.
    \param[in]  mq: MessageQueue handle.
    \param[out] msg: Destination buffer (must be >= msg_size bytes).
    \param[in]  timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
                indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return     True if a message was retrieved, False on timeout.
    \warning    ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
bool stk_msgq_get(stk_msgq_t *mq, void *msg, stk_timeout_t timeout);

/*! \brief      Attempt to get a message from the queue without blocking.
    \param[in]  mq: MessageQueue handle.
    \param[out] msg: Destination buffer.
    \return     True if a message was retrieved, False if the queue was empty.
    \warning    ISR-safe.
*/
bool stk_msgq_tryget(stk_msgq_t *mq, void *msg);

/*! \brief      Peek at the next message to be delivered without removing it.
    \details    Copies \a msg_size bytes from the oldest slot into \a msg,
                leaving the message in place so that a subsequent
                stk_msgq_get() returns the same message.  Blocks if the
                queue is empty until a message is available or the timeout
                expires.
    \param[in]  mq: MessageQueue handle.
    \param[out] msg: Destination buffer (must be >= msg_size bytes).
    \param[in]  timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
                indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return     True if a message was peeked, False on timeout.
    \warning    ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
bool stk_msgq_peek(stk_msgq_t *mq, void *msg, stk_timeout_t timeout);

/*! \brief      Attempt to peek at the next message without blocking.
    \param[in]  mq: MessageQueue handle.
    \param[out] msg: Destination buffer (must be >= msg_size bytes).
    \return     True if a message was peeked, False if the queue was empty.
    \warning    ISR-safe.
*/
bool stk_msgq_trypeek(stk_msgq_t *mq, void *msg);

/*! \brief      Peek at the most recently front-inserted message without removing it.
    \details    Copies \a msg_size bytes from the front slot (i.e. the message
                that stk_msgq_putfront() most recently placed) into \a msg,
                leaving the message in the queue.  Blocks if the queue is
                empty until a message is available or the timeout expires.
    \param[in]  mq: MessageQueue handle.
    \param[out] msg: Destination buffer (must be >= msg_size bytes).
    \param[in]  timeout: Max time to wait (ticks). Use \c STK_WAIT_INFINITE to block
                indefinitely, \c STK_NO_WAIT for a non-blocking attempt.
    \return     True if a message was peeked, False on timeout.
    \warning    ISR-safe only with \a timeout = \c STK_NO_WAIT.
*/
bool stk_msgq_peekfront(stk_msgq_t *mq, void *msg, stk_timeout_t timeout);

/*! \brief      Attempt to peek at the front message without blocking.
    \param[in]  mq: MessageQueue handle.
    \param[out] msg: Destination buffer (must be >= msg_size bytes).
    \return     True if a message was peeked, False if the queue was empty.
    \warning    ISR-safe.
*/
bool stk_msgq_trypeekfront(stk_msgq_t *mq, void *msg);

/*! \brief     Discard all messages and reset the queue to the empty state.
    \details   Tasks blocked in Put() are woken so they can re-enqueue into the
               now-empty queue.
    \param[in] mq: MessageQueue handle.
    \warning   Messages that were in the queue are silently discarded.
    \warning   ISR-safe.
*/
void stk_msgq_reset(stk_msgq_t *mq);

/*! \brief     Get the maximum number of messages the queue can hold.
    \param[in] mq: MessageQueue handle.
    \return    Construction-time capacity.
    \note      ISR-safe.
*/
size_t stk_msgq_get_capacity(const stk_msgq_t *mq);

/*! \brief     Get the size of each message in bytes.
    \param[in] mq: MessageQueue handle.
    \return    Construction-time message size.
    \note      ISR-safe.
*/
size_t stk_msgq_get_msg_size(const stk_msgq_t *mq);

/*! \brief     Get the current number of messages waiting in the queue.
    \param[in] mq: MessageQueue handle.
    \return    Point-in-time snapshot of the message count.
    \note      ISR-safe on targets where a size_t-aligned read is atomic.
*/
size_t stk_msgq_get_count(const stk_msgq_t *mq);

/*! \brief     Get the number of free slots currently available.
    \param[in] mq: MessageQueue handle.
    \return    Point-in-time snapshot of the free-slot count.
    \note      ISR-safe.
*/
size_t stk_msgq_get_space(const stk_msgq_t *mq);

/*! \brief     Check whether the queue is currently empty.
    \param[in] mq: MessageQueue handle.
    \return    True if the queue contains no messages.
    \note      ISR-safe.
*/
bool stk_msgq_is_empty(const stk_msgq_t *mq);

/*! \brief     Get a pointer to the raw message data buffer.
    \param[in] mq: MessageQueue handle.
    \return    Pointer to the beginning of the backing byte buffer supplied
               at construction time.
    \note      ISR-safe.
*/
uint8_t *stk_msgq_get_buffer(stk_msgq_t *mq);

/*! \brief     Check whether the queue is currently full.
    \param[in] mq: MessageQueue handle.
    \return    True if the queue contains \a capacity messages.
    \note      ISR-safe.
*/
bool stk_msgq_is_full(const stk_msgq_t *mq);

/*! \brief     Verify that the backing storage is valid and the queue is ready for use.
    \details   Always true for queues constructed with a non-NULL buffer.
               Useful as a post-construction sanity check in no-exceptions
               environments; stk_msgq_create() returning NULL already covers
               the primary failure path.
    \param[in] mq: MessageQueue handle.
    \return    True if the queue is ready for use.
    \note      ISR-safe.
*/
bool stk_msgq_is_storage_valid(const stk_msgq_t *mq);

// ----- RWMutex (Reader-Writer Lock) ------------------------------------------

/*! \brief     A memory size (multiples of stk_word_t) required for RWMutex instance.
*/
#define STK_RWMUTEX_IMPL_SIZE (17 + (STK_SYNC_DEBUG_NAMES ? 3 : 0))

/*! \brief     Opaque memory container for an RWMutex instance.
*/
typedef struct stk_rwmutex_mem_t {
    stk_word_t data[STK_RWMUTEX_IMPL_SIZE] __stk_c_aligned;
} stk_rwmutex_mem_t;

/*! \brief     Opaque handle to an RWMutex instance.
*/
typedef struct stk_rwmutex_t stk_rwmutex_t;

/*! \brief     Create an RWMutex (using provided memory).
    \param[in] memory: Pointer to static memory container.
    \param[in] memory_size: Size of the container (must be >= sizeof(stk_rwmutex_mem_t)).
    \return    RWMutex handle, or NULL if memory is too small.
*/
stk_rwmutex_t *stk_rwmutex_create(stk_rwmutex_mem_t *memory, uint32_t memory_size);

/*! \brief     Destroy an RWMutex.
    \param[in] rw: RWMutex handle.
*/
void stk_rwmutex_destroy(stk_rwmutex_t *rw);

/*! \brief     Acquire the lock for shared reading. Blocks until available.
    \details   Blocks if a writer is currently active or writers are waiting
               (Writer Preference Policy).
    \param[in] rw: RWMutex handle.
*/
void stk_rwmutex_read_lock(stk_rwmutex_t *rw);

/*! \brief     Try to acquire the read lock without blocking.
    \param[in] rw: RWMutex handle.
    \return    True if the read lock was acquired, False if a writer is active or waiting.
*/
bool stk_rwmutex_try_read_lock(stk_rwmutex_t *rw);

/*! \brief     Try to acquire the read lock with a timeout.
    \param[in] rw: RWMutex handle.
    \param[in] timeout: Max time to wait (ticks). Use STK_NO_WAIT for non-blocking.
    \return    True if acquired, False on timeout.
*/
bool stk_rwmutex_timed_read_lock(stk_rwmutex_t *rw, stk_timeout_t timeout);

/*! \brief     Release the shared reader lock.
    \details   If this is the last active reader, waiting writers are notified.
    \param[in] rw: RWMutex handle.
*/
void stk_rwmutex_read_unlock(stk_rwmutex_t *rw);

/*! \brief     Acquire the lock for exclusive writing. Blocks until available.
    \details   Blocks until all active readers have released their locks and no
               other writer is active.
    \param[in] rw: RWMutex handle.
*/
void stk_rwmutex_lock(stk_rwmutex_t *rw);

/*! \brief     Try to acquire the write lock without blocking.
    \param[in] rw: RWMutex handle.
    \return    True if the exclusive lock was acquired, False otherwise.
*/
bool stk_rwmutex_trylock(stk_rwmutex_t *rw);

/*! \brief     Try to acquire the write lock with a timeout.
    \param[in] rw: RWMutex handle.
    \param[in] timeout: Max time to wait (ticks). Use STK_NO_WAIT for non-blocking.
    \return    True if acquired, False on timeout.
*/
bool stk_rwmutex_timed_lock(stk_rwmutex_t *rw, stk_timeout_t timeout);

/*! \brief     Release the exclusive writer lock.
    \details   Prioritizes waking waiting writers. If none are waiting, all
               waiting readers are woken (Writer Preference Policy).
    \param[in] rw: RWMutex handle.
*/
void stk_rwmutex_unlock(stk_rwmutex_t *rw);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* STK_C_H_ */
