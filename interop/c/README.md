# STK C Interface

The C interface provides full access to the C++ version of STK from plain C code.
All synchronization, memory, timing, and scheduling features are exposed through a
pure C API with no C++ headers required in your source files.

---

## Contents

- [Headers and Sources](#headers-and-sources)
- [Configuration](#configuration)
- [Version Information](#version-information)
- [Quick Start](#quick-start)
- [Step-by-Step Setup](#step-by-step-setup)
  - [1. Configure the Kernel Type](#1-configure-the-kernel-type)
  - [2. Create and Start the Kernel](#2-create-and-start-the-kernel)
  - [3. Create Tasks](#3-create-tasks)
  - [Alignment and Sizing Helpers](#alignment-and-sizing-helpers)
- [Kernel Modes](#kernel-modes)
  - [Choosing a Scheduling Strategy](#choosing-a-scheduling-strategy)
  - [Hard Real-Time (HRT)](#hard-real-time-hrt)
  - [Tickless / Low-Power](#tickless--low-power)
- [Task Lifecycle](#task-lifecycle)
  - [Static Kernel](#static-kernel)
  - [Dynamic Kernel](#dynamic-kernel)
  - [Adding Tasks at Runtime](#adding-tasks-at-runtime)
  - [Leaving and Restarting a Task](#leaving-and-restarting-a-task)
  - [Task Naming and Priority](#task-naming-and-priority)
  - [Suspend and Resume](#suspend-and-resume)
- [Timing Services](#timing-services)
  - [Sleep vs Delay](#sleep-vs-delay)
  - [Time Conversion Helpers](#time-conversion-helpers)
  - [Raw System Timer](#raw-system-timer)
  - [High-Resolution Clock](#high-resolution-clock)
- [Kernel Introspection and Control](#kernel-introspection-and-control)
  - [Kernel State](#kernel-state)
  - [Enumerating Tasks](#enumerating-tasks)
  - [Manual Tick Injection](#manual-tick-injection)
  - [Tickless Scheduling Suspend/Resume](#tickless-scheduling-suspendresume)
  - [Platform Event Overrider](#platform-event-overrider)
  - [Dynamic Kernel/Task Cleanup](#dynamic-kerneltask-cleanup)
  - [Access to the C++ Objects](#access-to-the-c-objects)
- [Synchronization Primitives](#synchronization-primitives)
  - [Critical Section](#critical-section)
  - [Mutex](#mutex)
  - [FastMutex](#fastmutex)
  - [SpinLock](#spinlock)
  - [Condition Variable](#condition-variable)
  - [Semaphore](#semaphore)
  - [Event](#event)
  - [EventFlags](#eventflags)
  - [Pipe](#pipe)
  - [Message Queue](#message-queue)
  - [Reader-Writer Lock](#reader-writer-lock)
  - [Barrier](#barrier)
- [Memory: Block Pool](#memory-block-pool)
- [Memory: Byte Pool](#memory-byte-pool)
- [Software Timers](#software-timers)
- [Thread-Local Storage (TLS)](#thread-local-storage-tls)
- [Common Pitfalls](#common-pitfalls)
- [Configuration Reference](#configuration-reference)

---

## Headers and Sources

| File | Purpose |
|---|---|
| `stk_c.h` | Kernel, tasks, timing, synchronization, and version information |
| `stk_c_memory.h` | Block memory pool and byte (variable-size) memory pool |
| `stk_c_time.h` | Software timers and periodic triggers |

Compile the matching `.cpp` files alongside your project:

```
stk_c.cpp
stk_c_sync.cpp
stk_c_memory.cpp
stk_c_time.cpp
```

---

## Configuration

All compile-time limits are set with `#define` before including any STK header,
or more typically in your project's `stk_config.h`.

| Macro | Default | Meaning |
|---|---|---|
| `STK_C_KERNEL_MAX_TASKS` | `4` | Max tasks per kernel instance |
| `STK_C_CPU_COUNT` | `1` | Number of CPU cores / kernel instances |
| `STK_C_BLOCKPOOL_MAX` | `8` | Max concurrent block pools |
| `STK_C_BYTEPOOL_MAX` | `8` | Max concurrent byte pools |
| `STK_C_TIMER_COMMAND_QUEUE_MAX` | `32` | Capacity of each timer host's command queue (does **not** limit the number of timers; must be identical in every translation unit). The deprecated `STK_C_TIMER_MAX` is still honored if this macro is not defined |
| `STK_C_TIMER_HANDLER_STACK_SIZE` | `256` | Stack words for the timer handler task |
| `STK_C_TIMER_THREADS_COUNT` | `1` | Number of handler tasks serving expired timers in task mode (must be identical in every translation unit) |
| `STK_C_SEM_VALUE_TYPE` | `uint16_t` | Semaphore counter type: `uint8_t`, `uint16_t`, `uint32_t` or `uint64_t` (see [Semaphore](#semaphore)) |
| `STK_C_EF_VALUE_TYPE` | `uint32_t` | EventFlags word type: `uint8_t`, `uint16_t`, `uint32_t` or `uint64_t` (see [EventFlags](#eventflags)) |

The most important configuration step is declaring the kernel type. See
[Step 1](#1-configure-the-kernel-type) below.

---

## Version Information

`stk_get_version()` returns the version of the STK kernel this C binding was built with:

```c
#include "stk_c.h"

stk_version_info_t v = stk_get_version();
printf("STK %s (code 0x%08lX, rev %lu, %s)\n",
       v.string, (unsigned long)v.code, (unsigned long)v.revision, v.git_hash);
```

| Field | Description |
|---|---|
| `major`, `minor`, `patch` | Numeric version parts |
| `string` | Version string, e.g. `1.08.0`, or `1.08.0-rc1` for a pre-release |
| `code` | Packed version `0x00MMmmpp` (same as `STK_VERSION_CODE`), for numeric comparison |
| `revision` | Build metadata: git commit count, `0` if unknown |
| `git_hash` | Build metadata: short git hash, `<hash>-dirty` if the working tree was modified, `"unknown"` if unavailable |

`stk_get_version()` is ISR-safe: it returns compile-time constants and makes no kernel call.
`stk_version_info_t` is layout-compatible with `stk::VersionInfo`.

For compile-time checks use the `STK_VERSION_*` macros from `stk_version.h`, which can be
included from C:

```c
#include "stk_version.h"

#if !STK_VERSION_AT_LEAST(1, 8, 0)
    #error "STK 1.8.0 or newer is required"
#endif
```

> `STK_C_VERSION` (a date-based number such as `0x20261028`) is the version of the C interface
> itself, not of the kernel. The revision and hash are generated at build time by CMake and fall
> back to `0` / `"unknown"` without git history (see `stk_version.h`).

---

## Quick Start

A minimal two-task program on a single-core target:

```c
#include <stk_c.h>

#define STACK_WORDS 256
static stk_word_t g_stack0[STACK_WORDS];
static stk_word_t g_stack1[STACK_WORDS];

void task_led(void *arg) {
    while (1) {
        /* toggle LED */
        stk_sleep_ms(500);
    }
}

void task_sensor(void *arg) {
    while (1) {
        /* read sensor */
        stk_sleep_ms(100);
    }
}

int main(void) {
    stk_kernel_t *k = stk_kernel_create(0);           /* core 0 */
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);       /* 1 ms tick */

    stk_task_t *t0 = stk_task_create_privileged(task_led,    NULL, g_stack0, STACK_WORDS);
    stk_task_t *t1 = stk_task_create_privileged(task_sensor, NULL, g_stack1, STACK_WORDS);

    /* call outside of the assert: it must not vanish if asserts are compiled out */
    stk_add_result_t r = stk_kernel_add_task(k, t0);
    STK_C_ASSERT(r == STK_ADD_OK);
    r = stk_kernel_add_task(k, t1);
    STK_C_ASSERT(r == STK_ADD_OK);
    (void)r;

    stk_kernel_start(k);   /* never returns for KERNEL_STATIC */

    STK_C_ASSERT(false);   /* should not reach here */
}
```

A working example for x86 (MinGW) is in:
`build/example/project/eclipse/x86/blinky_c-mingw32`

---

## Step-by-Step Setup

### 1. Configure the Kernel Type

Define `STK_C_KERNEL_TYPE_CPU_0` (and `_CPU_1` … `_CPU_7` for multicore) in
your `stk_config.h` **before** any STK header is included:

```c
/* Soft real-time, static tasks, round-robin */
#define STK_C_KERNEL_TYPE_CPU_0 \
    Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>

/* Hard real-time, dynamic tasks, EDF scheduling, with sync primitives */
#define STK_C_KERNEL_TYPE_CPU_0 \
    Kernel<KERNEL_DYNAMIC | KERNEL_HRT | KERNEL_SYNC, \
           STK_C_KERNEL_MAX_TASKS, SwitchStrategyEDF, PlatformDefault>
```

The kernel flags can be OR-combined, subject to these rules:

| Flag | Meaning | Constraints |
|---|---|---|
| `KERNEL_STATIC` | Fixed task list set before `stk_kernel_start()` | Cannot combine with `KERNEL_DYNAMIC` |
| `KERNEL_DYNAMIC` | Tasks may be added/removed at runtime and may return | Cannot combine with `KERNEL_STATIC` |
| `KERNEL_HRT` | Hard real-time; tasks have period and deadline | Requires `KERNEL_STATIC` or `KERNEL_DYNAMIC` |
| `KERNEL_SYNC` | Enables mutex, semaphore, event, message queue | — |
| `KERNEL_TICKLESS` | Suppresses SysTick when all tasks sleep | Requires `STK_TICKLESS_IDLE=1`; **incompatible** with `KERNEL_HRT` |

### 2. Create and Start the Kernel

```c
stk_kernel_t *k = stk_kernel_create(0);          /* 0 = core 0 */
stk_kernel_init(k, STK_PERIODICITY_DEFAULT);      /* tick = 1000 µs */
/* ... add tasks ... */
stk_kernel_start(k);
```

`stk_kernel_init()` takes the tick period in **microseconds**.
`STK_PERIODICITY_DEFAULT` equals `1000` (1 ms). Smaller values increase
scheduling resolution but also ISR overhead.

### 3. Create Tasks

```c
/* Privileged mode (full hardware access) */
stk_task_t *t = stk_task_create_privileged(my_func, arg, stack, STACK_WORDS);

/* User mode (MPU restricted — use when KERNEL_SYNC is enabled and MPU is present) */
stk_task_t *t = stk_task_create_user(my_func, arg, stack, STACK_WORDS);
```

`stack` is a `stk_word_t[]` array you declare. Apply `__stk_c_stack` to ensure
correct alignment:

```c
static __stk_c_stack stk_word_t g_stack[256];
```

### Alignment and Sizing Helpers

`__stk_c_align(x)` aligns a variable to `x` bytes (a no-op on compilers without
`__attribute__((aligned))` support). `__stk_c_stack` and `__stk_c_aligned` are built on it,
and you can use it directly for your own buffers:

```c
static uint8_t g_buf[128] __stk_c_align(8);
```

Small compile-time-friendly helpers are also available in `stk_c.h`:

| Macro | Result |
|---|---|
| `STK_MIN(a, b)` / `STK_MAX(a, b)` | Smaller / larger of two values |
| `STK_C_CEIL_DIV(size, unit)` | Integer division rounded up (e.g. bytes → words) |
| `STK_ROUND_UP(x, a)` | `x` rounded up to a multiple of `a` |
| `STK_CALC_ALIGNED_BLOCKS(base, val_size, count)` | Size in `stk_word_t` blocks of a `base`-byte header followed by `count` values of `val_size` bytes, with alignment padding (used to size the semaphore container) |

The arguments are evaluated more than once, so do not pass expressions with side effects.

---

## Kernel Modes

### Choosing a Scheduling Strategy

| Strategy | Macro | Best for |
|---|---|---|
| Round Robin | `SwitchStrategyRR` | Equal time-slicing, simplest |
| Smooth Weighted RR | `SwitchStrategySWRR` | Proportional CPU share per task |
| Fixed Priority (32 levels) | `SwitchStrategyFP32` | Priority-driven soft real-time |
| Rate Monotonic | `SwitchStrategyRM` | HRT: shorter period = higher priority |
| Deadline Monotonic | `SwitchStrategyDM` | HRT: shorter deadline = higher priority |
| Earliest Deadline First | `SwitchStrategyEDF` | HRT: optimal utilization |

### Hard Real-Time (HRT)

With `KERNEL_HRT`, each task must be added with timing parameters:

```c
/* period = 10 ticks, deadline = 8 ticks, start delay = 0 ticks */
stk_add_result_t r = stk_kernel_add_task_hrt(k, task, 10, 8, 0);
STK_C_ASSERT(r == STK_ADD_OK);   /* keep the call itself outside of the assert */
(void)r;
```

`stk_kernel_add_task_hrt()` returns a `stk_add_result_t` (see
[Adding Tasks at Runtime](#adding-tasks-at-runtime)). Period and deadline must
be > 0 and < `INT32_MAX`, the start delay must be >= 0.

Inside an HRT task, call `stk_yield()` when the work for the current period is
done. The kernel then suspends the task until the next period begins.

> If the task overruns its deadline, the `OnDeadlineMissed()` callback fires
> (no-op in the C binding by default). The application will assert in debug
> builds.

Check schedulability before starting:

```c
if (!stk_kernel_is_schedulable(k)) {
    /* task set cannot meet all deadlines — adjust periods or deadlines */
}
```

### Tickless / Low-Power

Enable `KERNEL_TICKLESS` and set `STK_TICKLESS_IDLE=1` in `stk_config.h`. When
all tasks are sleeping the SysTick interrupt is suppressed and the MCU enters
WFI, waking only for the nearest deadline. No application code changes are
needed.

---

## Task Lifecycle

### Static Kernel

Tasks must **never return** from their entry function. A typical task body:

```c
void my_task(void *arg) {
    /* one-time init */
    while (1) {
        /* periodic work */
        stk_sleep_ms(100);
    }
}
```

### Dynamic Kernel

Tasks may return from the entry function, or call `stk_exit_task()` from any call
depth (it does not return; local objects are not unwound). Either way the kernel
completes the removal on the next tick and releases the task handle
automatically — do not call `stk_task_destroy()` on it afterwards. You can also
remove a running task from another task or ISR:

```c
/* Schedule removal on the next tick (safe to call from any context) */
stk_kernel_schedule_task_removal(k, target_task);

/* Or remove a task that has already returned */
stk_kernel_remove_task(k, finished_task);
```

```c
void worker(void *arg) {
    if (!init_ok()) {
        stk_exit_task();     /* ISR-unsafe, KERNEL_DYNAMIC only, never returns */
    }
    /* ... */
}
```

### Adding Tasks at Runtime

`stk_kernel_add_task()` returns a `stk_add_result_t`. It adds the task
immediately inside a critical section (no yield, no wait); the scheduler picks
it up on the next tick. In a `KERNEL_DYNAMIC` kernel it can be called before or
after `stk_kernel_start()`, from a task **or from an ISR**.

| Result | Meaning |
|---|---|
| `STK_ADD_OK` | Task was added |
| `STK_ADD_FAIL_POOL_FULL` | No free kernel slot (`STK_C_KERNEL_MAX_TASKS`); retry after a task has exited |
| `STK_ADD_FAIL_DUPLICATE` | The task or its stack is already added (also returned while a removed task has not finished leaving, see below) |
| `STK_ADD_FAIL_INVALID` | NULL task, kernel not initialized, adding is not allowed in this mode/state (HRT kernel, or static kernel after `stk_kernel_start()`), or (HRT only) invalid timing parameters or a call on a non-HRT kernel |

Error handling depends on the kernel mode:

- **`KERNEL_DYNAMIC`** — a failure is only *returned*, never asserted. Always check the result, otherwise the task is silently not added.
- **`KERNEL_STATIC`** and other kernels — a failure asserts; in a release build the failure is only returned and the task is not added.

The task and stack collision check is always performed, also in a release build.

On failure the kernel is unchanged and the task handle stays allocated: retry
later or release it with `stk_task_destroy()`.

```c
stk_task_t *t = stk_task_create_privileged(worker, NULL, stack, STACK_WORDS);
stk_add_result_t r = stk_kernel_add_task(k, t);
if (r != STK_ADD_OK) {
    if (r == STK_ADD_FAIL_POOL_FULL) { /* retry later */ }
    else                             { stk_task_destroy(t); }
}
```

Restrictions when called from an ISR:

- The whole operation (stack initialization, MPU setup) runs with interrupts masked: keep the task stack modest and call from a low-priority ISR.
- On Cortex-M the ISR priority must be numerically above (less urgent than) the SVCall priority.
- In a tickless kernel, a task added while all tasks sleep is first scheduled when the already armed timer expires, not on the next tick.

> HRT: `stk_kernel_add_task_hrt()` returns a `stk_add_result_t` as well, with the
> same error handling, and must be called **before** `stk_kernel_start()`. Adding
> an HRT task after the start is not supported, not even from an ISR or in a
> `KERNEL_DYNAMIC` kernel: the HRT schedule is built from the complete task set
> and is not recomputed at runtime. The kernel is unchanged if the result is not
> `STK_ADD_OK`.

### Leaving and Restarting a Task

A task that has left the kernel (returned, `stk_exit_task()`, or was removed)
can be added again once its removal has completed, e.g. to restart a worker. Until
then `stk_kernel_add_task()` returns `STK_ADD_FAIL_DUPLICATE`, so a restart
loop should treat that result as "try again later".

### Task Naming and Priority

```c
stk_task_set_name(t, "sensor");          /* shown in SEGGER SystemView trace */

/* Fixed Priority scheduler only: 0 = lowest, 31 = highest */
stk_task_set_priority(t, 10);

/* Smooth Weighted Round Robin only */
stk_task_set_weight(t, 3);              /* gets 3x CPU share vs weight-1 tasks */
```

Both `stk_task_set_priority()` and `stk_task_set_weight()` must be called
**before** `stk_kernel_add_task()`.

### Suspend and Resume

```c
stk_suspend_result_t sr = stk_kernel_suspend_task(k, t);   /* returns after resume if self */
/* ... later ... */
stk_resume_result_t  rr = stk_kernel_resume_task(k, t);
```

| `stk_suspend_result_t` | Meaning |
|---|---|
| `STK_SUSPEND_DONE` | Task was runnable and is suspended now |
| `STK_SUSPEND_DEFERRED` | Task is sleeping/blocked: it is parked when the sleep/wait ends (the wait result is preserved) |
| `STK_SUSPEND_ALREADY` | Already suspended, or a deferred suspension is pending |
| `STK_SUSPEND_FAIL` | Task is not bound to the kernel, or is exiting |

| `stk_resume_result_t` | Meaning |
|---|---|
| `STK_RESUME_DONE` | Suspended task was resumed |
| `STK_RESUME_LIFTED` | Deferred suspension was cancelled before it took effect |
| `STK_RESUME_NOT_SUSPENDED` | Task was not suspended (no-op) |
| `STK_RESUME_FAIL` | Task is not bound to the kernel, or is exiting |

Both calls are ISR-safe. A task blocked on a synchronization object is not woken by
`stk_kernel_resume_task()`: a kernel wait is not a suspension.

> Do not hold a critical section when suspending the calling task — it will
> deadlock.

---

## Timing Services

### Sleep vs Delay

| Function | Behaviour | HRT compatible? |
|---|---|---|
| `stk_sleep_ms(ms)` | Yields CPU; low-power friendly | No |
| `stk_sleep(ticks)` | Yields CPU | No |
| `stk_sleep_until(ts)` | Sleeps until absolute tick timestamp | No |
| `stk_delay_ms(ms)` | Busy-waits (other tasks still run) | Yes |
| `stk_delay(ticks)` | Busy-waits | Yes |
| `stk_yield()` | Yields to another ready task | Yes (required in HRT) |

Prefer `stk_sleep_ms()` in normal tasks; use `stk_yield()` in HRT tasks.

Wake a sleeping task early from another task or an ISR:

```c
stk_sleep_cancel(stk_task_get_id(t));   /* no-op if the task isn't sleeping */
```

### Time Conversion Helpers

```c
uint32_t res  = stk_tick_resolution();     /* µs per tick */
int64_t ticks = stk_ticks_from_ms(250);    /* 250 ms → ticks */
int64_t ms    = stk_ms_from_ticks(ticks);  /* ticks → ms */
int64_t now   = stk_time_now_ms();         /* ms since kernel start */
int64_t t     = stk_ticks();               /* raw tick counter */

/* When a value is passed straight to a `timeout` argument, clamp instead of wrapping: */
stk_timeout_t to = stk_ticks_from_ms_clamped_to_timeout(ms);   /* ms → ticks, max STK_WAIT_INFINITE */

/* With a cached resolution (no kernel service call, ISR-safe arithmetic): */
int64_t ticks2 = stk_ticks_from_ms_r(250, res);
int64_t ms2    = stk_ms_from_ticks_r(ticks2, res);
```

### Raw System Timer

```c
stk_cycle_t t0 = stk_sys_timer_count();     /* raw 64-bit hardware counter, ISR-safe */
uint32_t freq  = stk_sys_timer_frequency(); /* counter frequency in Hz, ISR-safe */
```

### High-Resolution Clock

For sub-tick measurements (profiling, precise intervals):

```c
uint64_t t0 = stk_hires_cycles();
/* ... work ... */
uint64_t t1 = stk_hires_cycles();
uint32_t freq = stk_hires_frequency();     /* CPU clock in Hz */
double elapsed_us = (double)(t1 - t0) * 1e6 / freq;

/* Or directly: */
int64_t us = stk_hires_time_us();
```

---

## Kernel Introspection and Control

### Kernel State

```c
stk_kernel_state_t st = stk_kernel_get_state(k);   /* INACTIVE, READY, RUNNING, SUSPENDED */
bool running          = stk_kernel_is_started(k);  /* first context switch has occurred */
```

`STK_KERNEL_STATE_SUSPENDED` is only seen in a tickless kernel between
`stk_kernel_suspend()` and `stk_kernel_resume()`.

### Enumerating Tasks

```c
stk_task_t *tasks[STK_C_KERNEL_MAX_TASKS];
size_t n = stk_kernel_enumerate_tasks(k, tasks, STK_C_KERNEL_MAX_TASKS);
for (size_t i = 0; i < n; i++) {
    printf("%s\n", stk_task_get_name(tasks[i]));
}
```

`stk_kernel_enumerate_tasks()` is ISR-safe.

### Manual Tick Injection

If the platform driver's built-in SysTick handler is disabled
(`STK_SYSTICK_HANDLER = _STK_SYSTICK_HANDLER_DISABLE` in `stk_config.h`), drive
the scheduler from your own tick ISR instead:

```c
void my_tick_isr(void) {
    stk_kernel_process_tick(k);   /* ISR-safe; call at the configured tick rate */
}
```

`stk_kernel_process_hard_fault(k)` triggers the kernel's safe-state handler
(normally invoked automatically when an HRT task misses its deadline) and
never returns. It is exposed for custom fault handlers or test harnesses.

### Tickless Scheduling Suspend/Resume

These pair together for `KERNEL_TICKLESS` kernels and are distinct from
`stk_kernel_suspend_task()` / `stk_kernel_resume_task()`, which suspend a
single task rather than the whole scheduler:

```c
/* Entering low-power state: */
stk_timeout_t sleep_ticks = stk_kernel_suspend(k); /* ISR-safe */
/* Program a hardware timer for sleep_ticks, enter WFI ... */

/* On wake: */
stk_kernel_resume(k, elapsed_ticks);               /* ISR-safe */
```

### Platform Event Overrider

Intercept the kernel's idle-sleep and hard-fault handling before it reaches
the platform driver's default behaviour:

```c
static bool on_sleep(stk_timeout_t sleep_ticks, void *user_data) {
    /* return true if handled (kernel skips its own sleep logic) */
    return false;
}

static bool on_hard_fault(void *user_data) {
    return false; /* let the platform driver halt the system */
}

static stk_event_overrider_t overrider = {
    .on_sleep = on_sleep,
    .on_hard_fault = on_hard_fault,
    .user_data = NULL,
};

/* After stk_kernel_init(), before stk_kernel_start(): */
stk_kernel_set_event_overrider(k, &overrider);
```

Pass `NULL` to remove a previously installed overrider. Not ISR-safe; the
struct must remain valid for the kernel's lifetime (static/global storage).

### Dynamic Kernel/Task Cleanup

```c
stk_task_destroy(task);            /* task that was never added, or whose add failed */
stk_kernel_destroy(k);             /* kernel must not be running */
```

Only valid for dynamically created kernels/tasks. A task that has exited or been
removed is released by the kernel itself, so `stk_task_destroy()` is for handles
that never entered (or failed to enter) the kernel.

### Access to the C++ Objects

C++ translation units that link against the C binding can reach the wrapped
objects (declared only under `__cplusplus`, not part of the C ABI):

| Function | Returns |
|---|---|
| `stk_kernel_get_instance(core)` | `stk::IKernel *` |
| `stk_task_get_instance(tsk)` | `stk::ITask *` |
| `stk_mutex_get_instance`, `stk_fastmutex_get_instance`, `stk_spinlock_get_instance`, `stk_cv_get_instance`, `stk_event_get_instance`, `stk_ef_get_instance`, `stk_pipe_get_instance`, `stk_msgq_get_instance`, `stk_rwmutex_get_instance`, `stk_barrier_get_instance` | the matching `stk::sync::*` object |
| `stk_sem_get_instance` | `stk::sync::SemaphoreT<stk_sem_val_t> *` |
| `stk_blockpool_get_instance`, `stk_bytepool_get_instance` | `stk::memory::BlockMemoryPool` / `BytePool` |
| `stk_timerhost_get_instance`, `stk_timer_get_instance`, `stk_periodic_trigger_get_instance` | `stk::time::*` objects |

---

## Synchronization Primitives

All primitives require `KERNEL_SYNC` in the kernel flags. Memory for each
primitive is supplied by the caller — no heap allocation occurs.

### Critical Section

Disables context switches on the current core. Supports nesting (one `exit`
per `enter`). There is no handle or backing memory to manage — the critical
section is a global, per-core resource accessed through free functions:

```c
stk_critical_section_enter();
/* protected region */
stk_critical_section_exit();
```

The plain `stk_critical_section_enter()` / `stk_critical_section_exit()` pair may be used from
a **Privileged** thread only. From a User thread the section cannot be left properly (the exit
has no session to restore, and User privilege may not clear the interrupt masking set on entry).
Misuse is caught by an assertion when assertions are enabled; otherwise the behavior is undefined.

Use the `_ex()` variants (the preferred API) in any code that may run in a User thread, such as
shared library or middleware code. They work from both privilege levels: the returned session
carries the state that the matching exit must restore. Pass the session returned by *that* call
to its exit, and leave nested sections in reverse (LIFO) order:

```c
stk_cs_session_t ses = stk_critical_section_enter_ex(STK_DEFAULT_CS_SESSION);
/* protected region */
stk_critical_section_exit_ex(ses);
```

> Critical sections protect against context switches only, not hardware
> interrupts. For ISR safety use primitives with the `ISR-safe` note.

### Mutex

```c
static stk_mutex_mem_t mtx_mem;
stk_mutex_t *mtx = stk_mutex_create(&mtx_mem);

stk_mutex_lock(mtx);                        /* blocks until available */
bool ok = stk_mutex_trylock(mtx);           /* non-blocking */
bool ok = stk_mutex_timed_lock(mtx, 100);   /* ticks timeout */
stk_mutex_unlock(mtx);

stk_mutex_destroy(mtx);
```

`stk_mutex_timed_lock_ex()` returns a `stk_wait_result_t` instead of a bool, to tell a
timeout from a cancellation. The same `_ex` form exists for FastMutex, Semaphore, Event,
EventFlags, Reader-Writer Lock, Pipe and Message Queue calls.

### FastMutex

Non-recursive (binary) mutex: smaller and faster than `stk_mutex_t`, but the owner must not
lock it again. Re-locking is an unrecoverable contract violation: the kernel panics with
`KERNEL_PANIC_SYNC_DEADLOCK` in all build configurations. Ownership
is handed directly to the first waiter (FIFO) on unlock.

```c
static stk_fastmutex_mem_t fmtx_mem;
stk_fastmutex_t *fmtx = stk_fastmutex_create(&fmtx_mem);

stk_fastmutex_lock(fmtx);                        /* blocks until available */
bool ok = stk_fastmutex_trylock(fmtx);           /* non-blocking */
bool ok = stk_fastmutex_timed_lock(fmtx, 100);   /* ticks timeout */
stk_wait_result_t r = stk_fastmutex_timed_lock_ex(fmtx, 100);
stk_fastmutex_unlock(fmtx);

stk_fastmutex_destroy(fmtx);
```

### SpinLock

Suitable for very short critical regions and ISR-to-task handoff.

```c
static stk_spinlock_mem_t sl_mem;
stk_spinlock_t *sl = stk_spinlock_create(&sl_mem);   /* recursive */

stk_spinlock_lock(sl);
stk_spinlock_unlock(sl);
bool ok = stk_spinlock_trylock(sl);                  /* non-blocking */

stk_spinlock_destroy(sl);
```

### Condition Variable

Always paired with a locked mutex protecting the guarded state.

```c
static stk_cv_mem_t cv_mem;
stk_cv_t *cv = stk_cv_create(&cv_mem);

/* Waiter: */
stk_mutex_lock(mtx);
while (!condition_met) {
    bool ok = stk_cv_wait(cv, mtx, STK_WAIT_INFINITE); /* atomically unlocks mtx while
                                                            waiting, re-locks before return */
}
stk_mutex_unlock(mtx);

/* Signaler: */
stk_cv_notify_one(cv);   /* wake one waiter */
stk_cv_notify_all(cv);   /* wake all waiters */

stk_cv_destroy(cv);
```

`stk_cv_wait()` collapses timeout and cancellation into a single `false`
result. To tell them apart, use `stk_cv_wait_ex()`:

```c
stk_wait_result_t r = stk_cv_wait_ex(cv, mtx, 100 /* ticks */);
switch (r) {
case STK_WAIT_RESULT_SIGNAL:   /* woken by notify */          break;
case STK_WAIT_RESULT_TIMEOUT:  /* timeout expired */          break;
case STK_WAIT_RESULT_CANCELED: /* wait was cancelled */        break;
case STK_WAIT_RESULT_FAIL:     /* kernel error, did not wait */ break;
}
```

`stk_wait_result_t` is shared by other cancellable timed waits in the API
(e.g. `stk_barrier_wait_ex()`).

A condition variable can also be paired with a `stk_fastmutex_t` instead of a
`stk_mutex_t`, using the `_fastmutex` variants (same semantics; the calling task
must own the FastMutex exactly once):

```c
stk_fastmutex_lock(fmtx);
while (!condition_met) {
    bool ok = stk_cv_wait_fastmutex(cv, fmtx, STK_WAIT_INFINITE);
}
stk_fastmutex_unlock(fmtx);

stk_wait_result_t r = stk_cv_wait_ex_fastmutex(cv, fmtx, 100 /* ticks */);
```

> ISR-safe only with `timeout = STK_NO_WAIT`; ISR-unsafe otherwise.

### Semaphore

The counter type is `stk_sem_val_t`, selected at build time by `STK_C_SEM_VALUE_TYPE`
(default `uint16_t`; `uint8_t`, `uint32_t` or `uint64_t` are also supported. Define it before
including `stk_c.h`, and use the same value in every translation unit). `stk_sem_create()`,
`stk_sem_trysignal_ceiling()` and `stk_sem_get_count()` all use this type. Passing `max_count = 0`
to `stk_sem_create()` selects the maximum value of `stk_sem_val_t` (65535 for the default). The size and alignment of `stk_sem_mem_t` adapt automatically, so a
counter wider than a pointer (e.g. `uint64_t` on a 32-bit MCU) works; on such targets
`stk_sem_get_count()` reads under a critical section to avoid a torn value.

```c
static stk_sem_mem_t sem_mem;
/* initial value = 0, max value = 1 → binary semaphore */
stk_sem_t *sem = stk_sem_create(&sem_mem, 0, 1);

stk_sem_signal(sem);                    /* post / give — ISR-safe */
bool ok = stk_sem_wait(sem, STK_WAIT_INFINITE); /* blocks */
bool ok = stk_sem_trywait(sem);         /* non-blocking poll, ISR-safe */
bool ok = stk_sem_wait(sem, 50);        /* ticks timeout */
stk_wait_result_t r = stk_sem_wait_ex(sem, 50);   /* distinguishes timeout / cancel */

/* Signal without exceeding max_count (returns false instead of overflowing): */
bool signaled = stk_sem_trysignal(sem);  /* ISR-safe */

/* Same, but with a per-call ceiling (effective limit = min(ceiling, max_count),
   e.g. ThreadX tx_semaphore_ceiling_put). Check and increment are atomic: */
bool signaled2 = stk_sem_trysignal_ceiling(sem, 5);  /* ISR-safe */

stk_sem_val_t n = stk_sem_get_count(sem);   /* current resource counter, ISR-safe */

stk_sem_destroy(sem);
```

### Event

A binary signal (signaled / non-signaled). Supports auto-reset and manual-reset modes.

```c
static stk_event_mem_t ev_mem;
/* false = auto-reset, true = manual-reset */
stk_event_t *ev = stk_event_create(&ev_mem, false);

/* From ISR or another task: */
bool changed = stk_event_set(ev);   /* signal — ISR-safe; true if it was non-signaled */
changed = stk_event_reset(ev);      /* clear — ISR-safe; true if it was signaled */
stk_event_pulse(ev);                /* signal then immediately reset */

/* Waiting task: */
bool ok = stk_event_wait(ev, STK_WAIT_INFINITE);  /* blocks */
bool ok = stk_event_wait(ev, 100);                /* ticks timeout */
bool ok = stk_event_trywait(ev);                  /* non-blocking, ISR-safe */
stk_wait_result_t r = stk_event_wait_ex(ev, 100); /* distinguishes timeout / cancel */

stk_event_destroy(ev);
```

### EventFlags

A flags word of type `stk_ef_val_t`, selected at build time by `STK_C_EF_VALUE_TYPE` (default
`uint32_t`; `uint8_t`, `uint16_t` or `uint64_t` are also supported. Define it before including
`stk_c.h`, and use the same value in every translation unit). The most significant bit is reserved
as the error indicator (`STK_EF_ERROR_MASK`), leaving `8 * sizeof(stk_ef_val_t) - 1` usable flags
(7, 15, 31 or 63); `STK_EF_FLAGS_MASK` covers the usable bits. The size and alignment of
`stk_ef_mem_t` adapt automatically, and a word wider than a pointer (e.g. `uint64_t` on a 32-bit
MCU) is read under a critical section by `stk_ef_get()`. Tasks can wait for any subset (OR) or all
bits (AND). The examples below use the default `uint32_t`.

```c
static stk_ef_mem_t ef_mem;
stk_ef_t *ef = stk_ef_create(&ef_mem, 0);

#define EVT_BUTTON  (1u << 0)
#define EVT_UART_RX (1u << 1)

/* From ISR or another task: */
stk_ef_set(ef, EVT_BUTTON);                 /* ISR-safe */

/* Wait for any of the bits (clears them on return unless STK_EF_OPT_NO_CLEAR is OR-ed in): */
uint32_t fired = stk_ef_wait(ef, EVT_BUTTON | EVT_UART_RX,
                             STK_EF_OPT_WAIT_ANY, STK_WAIT_INFINITE);

/* Wait for ALL bits: */
uint32_t fired = stk_ef_wait(ef, EVT_BUTTON | EVT_UART_RX,
                             STK_EF_OPT_WAIT_ALL, STK_WAIT_INFINITE);

if (stk_ef_is_error(fired)) { /* timeout or invalid flags */ }

/* Non-blocking poll (ISR-safe), read and clear without waiting: */
fired = stk_ef_trywait(ef, EVT_BUTTON, STK_EF_OPT_WAIT_ANY);
uint32_t snapshot = stk_ef_get(ef);                 /* read only, ISR-safe */
uint32_t before   = stk_ef_clear(ef, EVT_UART_RX);  /* returns flags before clearing, ISR-safe */

/* Keep only the masked bits (flags &= mask), the inverse of stk_ef_clear(); returns flags before masking.
   Any mask is valid (0 clears everything). Never wakes waiters. ISR-safe. */
before = stk_ef_and(ef, EVT_BUTTON);

stk_ef_destroy(ef);
```

`stk_ef_wait()` reports errors as sentinels in the most significant bit of the returned word (bit 31 for
the default type), so a status and the flags share one value. Sentinels: `STK_EF_ERROR_PARAMETER`
(flags is 0 or has the error bit set), `STK_EF_ERROR_TIMEOUT`, `STK_EF_ERROR_ISR` (blocking wait from an
ISR), `STK_EF_ERROR_CANCELED` and `STK_EF_ERROR_FAIL` (kernel rejected the wait, e.g. `KERNEL_SYNC` is not
enabled). `stk_ef_wait_ex()` and `stk_ef_trywait_ex()` return a `stk_wait_result_t` and give the flags
separately through an out parameter (pass `NULL` if the snapshot is not needed):

```c
uint32_t actual;
stk_wait_result_t r = stk_ef_wait_ex(ef, EVT_BUTTON | EVT_UART_RX,
                                     STK_EF_OPT_WAIT_ANY, 100 /* ticks */, &actual);

if (r == STK_WAIT_RESULT_SIGNAL)
{
    /* actual is the WHOLE flags word taken before auto-clear;
       matched bits: actual & (EVT_BUTTON | EVT_UART_RX) for WAIT_ANY, the requested mask for WAIT_ALL */
}
else if (r == STK_WAIT_RESULT_TIMEOUT)  { /* condition not met in time */ }
else if (r == STK_WAIT_RESULT_CANCELED) { /* wait was cancelled by another task */ }
else                                    { /* STK_WAIT_RESULT_FAIL: flags is 0 / has bit 31 set, or blocking wait from ISR */ }

r = stk_ef_trywait_ex(ef, EVT_BUTTON, STK_EF_OPT_WAIT_ANY, &actual);  /* non-blocking, ISR-safe */
```

Unlike `stk_ef_wait()`, the `_ex` forms do not distinguish invalid arguments from an ISR misuse (both are
`STK_WAIT_RESULT_FAIL`).

`stk_ef_sync()` is a multi-task rendezvous (barrier): it sets this task's bits and waits until **all** of
`wait_flags` are set. The task which completes the condition clears `wait_flags` once and **every** task
blocked on the same rendezvous context is released, which a `stk_ef_set()` followed by
`stk_ef_wait(STK_EF_OPT_WAIT_ALL)` cannot do (the first task to run would clear the bits and the others
would block again). The context (`stk_ef_sync_ctx_t`) holds the state of one rendezvous group and is shared
by all of its participants:

```c
#define EVT_A (1u << 0)
#define EVT_B (1u << 1)

static stk_ef_sync_ctx_mem_t sync_mem;
stk_ef_sync_ctx_t *sync = stk_ef_sync_ctx_create(&sync_mem);   /* shared by task A and task B */

/* Task A: */
uint32_t actual;
stk_wait_result_t r = stk_ef_sync(ef, sync, EVT_A, EVT_A | EVT_B, STK_WAIT_INFINITE, &actual);
/* Task B: */
r = stk_ef_sync(ef, sync, EVT_B, EVT_A | EVT_B, STK_WAIT_INFINITE, &actual);
/* both return STK_WAIT_RESULT_SIGNAL once both have arrived */

stk_ef_sync_ctx_destroy(sync);
```

All participants sharing one context must pass the same `wait_flags`; independent groups on the same
EventFlags object use separate contexts.

### Pipe

A fixed-size-element FIFO, simpler than the Message Queue (no front-insert or
peek), with bulk and threshold-triggered read support.

```c
typedef struct { uint32_t id; uint8_t data[16]; } Sample;

#define PIPE_CAP 16

static stk_pipe_mem_t s_pipe_mem;
static uint8_t        s_pipe_buf[STK_PIPE_BUF_SIZE(PIPE_CAP, sizeof(Sample))];

stk_pipe_t *pipe = stk_pipe_create(&s_pipe_mem,
                                    s_pipe_buf, sizeof(s_pipe_buf),
                                    PIPE_CAP, sizeof(Sample));

/* Single element: */
Sample s = { .id = 1 };
stk_pipe_write(pipe, &s, STK_WAIT_INFINITE);      /* blocks if full */
bool ok = stk_pipe_trywrite(pipe, &s);            /* ISR-safe, non-blocking */

Sample in;
stk_pipe_read(pipe, &in, STK_WAIT_INFINITE);      /* blocks if empty */
bool got = stk_pipe_tryread(pipe, &in);           /* ISR-safe, non-blocking */

/* Bulk transfer: */
Sample batch[4];
size_t n = stk_pipe_write_bulk(pipe, batch, 4, STK_WAIT_INFINITE);
n = stk_pipe_read_bulk(pipe, batch, 4, STK_WAIT_INFINITE);

/* Block until at least `trigger` elements are available, then drain up to max_count: */
n = stk_pipe_read_bulk_triggered(pipe, batch, /*trigger=*/2, /*max_count=*/4,
                                  STK_WAIT_INFINITE);

/* Non-blocking bulk (ISR-safe): */
n = stk_pipe_trywrite_bulk(pipe, batch, 4);
n = stk_pipe_tryread_bulk(pipe, batch, 4);
n = stk_pipe_tryread_bulk_triggered(pipe, batch, /*max_count=*/4);

/* The _ex variants return stk_wait_result_t and the transferred count via an out parameter: */
size_t done;
stk_wait_result_t r = stk_pipe_read_bulk_ex(pipe, batch, 4, &done, 100);

stk_pipe_reset(pipe);   /* discard all elements, ISR-safe */

/* Query (all ISR-safe): */
stk_pipe_get_capacity(pipe);
stk_pipe_get_element_size(pipe);
stk_pipe_get_count(pipe);
stk_pipe_get_space(pipe);
stk_pipe_is_empty(pipe);
stk_pipe_is_full(pipe);
stk_pipe_is_storage_valid(pipe);

stk_pipe_destroy(pipe);
```

> ISR-safe only with `timeout = STK_NO_WAIT` (or the `try*` variants, which
> are always ISR-safe).

### Message Queue

Fixed-size message queue (the payload is copied in and out). Unlike Pipe, it
also supports front-insertion (priority messages) and non-destructive peek.

```c
typedef struct { uint32_t id; uint8_t data[16]; } Msg;

static stk_msgq_mem_t mq_mem;
static uint8_t        mq_buf[8 * sizeof(Msg)];   /* capacity × message size, or
                                                     use STK_MSGQ_BUF_SIZE(8, sizeof(Msg)) */

stk_msgq_t *mq = stk_msgq_create(&mq_mem,
                                  mq_buf, sizeof(mq_buf),
                                  8, sizeof(Msg));

/* Producer (ISR-safe with STK_NO_WAIT): */
Msg out = { .id = 42 };
stk_msgq_put(mq, &out, STK_NO_WAIT);
stk_msgq_putfront(mq, &out, STK_NO_WAIT);   /* priority insert — becomes the next Get() */

/* Consumer: */
Msg in;
if (stk_msgq_get(mq, &in, STK_WAIT_INFINITE)) {
    /* process in */
}

/* Non-destructive peek (leaves the message in the queue): */
stk_msgq_peek(mq, &in, STK_NO_WAIT);        /* peeks the oldest (next-to-Get) message */
stk_msgq_peekfront(mq, &in, STK_NO_WAIT);   /* peeks the most recently front-inserted one */

stk_msgq_reset(mq);   /* discard all messages, ISR-safe */

stk_msgq_destroy(mq);
```

Non-blocking `try*` variants exist for every blocking call above
(`stk_msgq_tryput`, `stk_msgq_tryputfront`, `stk_msgq_tryget`,
`stk_msgq_trypeek`, `stk_msgq_trypeekfront`) and are all ISR-safe. Query
helpers: `stk_msgq_get_capacity()`, `stk_msgq_get_msg_size()`,
`stk_msgq_get_count()`, `stk_msgq_get_space()`, `stk_msgq_is_empty()`,
`stk_msgq_is_full()`, `stk_msgq_is_storage_valid()`, and
`stk_msgq_get_buffer()` (raw pointer to the backing byte buffer).

### Reader-Writer Lock

Multiple readers can hold the lock concurrently; writers are exclusive.
The implementation uses writer preference.

```c
static stk_rwmutex_mem_t rw_mem;
stk_rwmutex_t *rw = stk_rwmutex_create(&rw_mem);

/* Reader: */
stk_rwmutex_read_lock(rw);
/* ... read shared data ... */
stk_rwmutex_read_unlock(rw);

/* Writer: */
stk_rwmutex_lock(rw);
/* ... modify shared data ... */
stk_rwmutex_unlock(rw);

/* Non-blocking and timed variants (bool; the _ex forms return stk_wait_result_t): */
bool ok = stk_rwmutex_try_read_lock(rw);
ok = stk_rwmutex_timed_read_lock(rw, 100);
ok = stk_rwmutex_trylock(rw);
ok = stk_rwmutex_timed_lock(rw, 100);

stk_rwmutex_destroy(rw);
```

### Barrier

Blocks a fixed number of tasks until they have all arrived, then releases
them together and resets itself for reuse.

```c
static stk_barrier_mem_t bar_mem;
stk_barrier_t *bar = stk_barrier_create(&bar_mem, 3 /* task count */);

/* In each of the 3 participating tasks: */
bool was_last = stk_barrier_wait(bar);   /* ISR-unsafe */
if (was_last) {
    /* this task tripped the barrier */
}

stk_barrier_get_threshold(bar);   /* construction-time count, ISR-safe */

stk_barrier_destroy(bar);
```

`stk_barrier_wait()` collapses a normal release and a cancellation into a
single `false`. Use `stk_barrier_wait_ex()` to distinguish all three outcomes:

```c
stk_barrier_result_t r = stk_barrier_wait_ex(bar);
switch (r) {
case STK_BARRIER_LAST_ARRIVAL: /* this task tripped the barrier */ break;
case STK_BARRIER_RELEASED:     /* released along with the others */ break;
case STK_BARRIER_CANCELED:     /* wait cancelled; arrival rolled back */ break;
}
```

---

## Memory: Block Pool

A deterministic, fragmentation-free fixed-size block allocator. All blocks have
the same size, so allocation and deallocation are O(1).

### Static storage (no heap, preferred for embedded)

```c
#include <stk_c_memory.h>

#define PKT_COUNT 8
#define PKT_SIZE  sizeof(Packet)

STK_BLOCKPOOL_STORAGE_DECL(g_pkt_storage, PKT_COUNT, PKT_SIZE);

stk_blockpool_t *pool = stk_blockpool_create_static(
    PKT_COUNT, PKT_SIZE,
    (uint8_t *)g_pkt_storage, sizeof(g_pkt_storage),
    "pkt_pool");

/* In ISR or task: */
Packet *pkt = (Packet *)stk_blockpool_try_alloc(pool);  /* ISR-safe */
if (pkt) {
    fill_packet(pkt);
    /* hand to consumer, consumer calls stk_blockpool_free() */
}

bool ok = stk_blockpool_free(pool, pkt);   /* ISR-safe; wakes a blocked allocator.
                                               Returns false if ptr is NULL, out of
                                               range, or misaligned. */
pkt = NULL;   /* avoid double-free */
```

### Heap storage

```c
stk_blockpool_t *pool = stk_blockpool_create(PKT_COUNT, PKT_SIZE, "pkt_pool");
if (!stk_blockpool_is_storage_valid(pool)) { /* allocation failed */ }
```

### Destroying a pool

```c
stk_blockpool_destroy(pool);   /* frees heap storage if owned; not ISR-safe.
                                   Asserts if any task is still blocked in
                                   stk_blockpool_alloc()/timed_alloc(). */
pool = NULL;
```

### Blocking allocation

```c
void *blk = stk_blockpool_alloc(pool);                    /* blocks until available */
void *blk = stk_blockpool_timed_alloc(pool, 100);         /* ticks timeout, NULL on timeout */
void *blk = stk_blockpool_try_alloc(pool);                /* non-blocking, ISR-safe */
```

### Query

```c
stk_blockpool_get_capacity(pool);    /* total blocks */
stk_blockpool_get_block_size(pool);  /* aligned per-block size in bytes */
stk_blockpool_get_used_count(pool);  /* currently allocated */
stk_blockpool_get_free_count(pool);  /* available */
stk_blockpool_is_full(pool);
stk_blockpool_is_empty(pool);
```

---

## Memory: Byte Pool

A variable-size, first-fit allocator with block coalescing, for payloads of
different sizes (messages, strings) where a fixed block size would waste memory.
Every allocation is 8-byte aligned and costs its payload (rounded up to 8) plus an
8-byte header. Allocation and free are O(n) in the number of blocks, unlike the
O(1) block pool.

### Static storage (no heap, preferred for embedded)

```c
#include <stk_c_memory.h>

/* Space for four 128-byte and one 512-byte allocation alive at the same time */
STK_BYTEPOOL_STORAGE_DECL(g_heap,
    STK_BYTEPOOL_REQUIRED_SIZE(128) * 4 + STK_BYTEPOOL_REQUIRED_SIZE(512));

stk_bytepool_t *bp = stk_bytepool_create_static((uint8_t *)g_heap, sizeof(g_heap), "msg_pool");

char *msg = (char *)stk_bytepool_timed_alloc(bp, 64, stk_ticks_from_ms(100)); /* NULL on timeout */
if (msg) {
    /* ... use msg ... */
    bool ok = stk_bytepool_free(bp, msg);   /* false if NULL, foreign, misaligned or double free */
    msg = NULL;
}
```

`STK_BYTEPOOL_STORAGE_DECL()` declares a `uint64_t` array so the start address is
8-byte aligned on every target. `STK_BYTEPOOL_REQUIRED_SIZE(payload)` is the pool
space one allocation consumes; sum it over all blocks that must coexist.

### Heap storage

```c
stk_bytepool_t *bp = stk_bytepool_create(1024, "msg_pool");
if (bp == NULL || !stk_bytepool_is_storage_valid(bp)) { /* slot pool exhausted or heap allocation failed */ }
```

### Allocation

```c
void *p = stk_bytepool_alloc(bp, 64);                 /* blocks; NULL only on invalid size or cancel */
void *p = stk_bytepool_timed_alloc(bp, 64, 100);      /* ticks timeout, NULL on timeout */
void *p = stk_bytepool_try_alloc(bp, 64);             /* non-blocking, ISR-safe (O(n)) */
```

A request larger than `stk_bytepool_get_max_request()` fails immediately instead
of blocking forever. A free wakes all blocked allocators, because a coalesced block may
satisfy several of them. `stk_bytepool_timed_alloc()` is ISR-safe only with
`STK_NO_WAIT`.

### Query

```c
size_t available, largest, fragments;
stk_bytepool_get_info(bp, &available, &largest, &fragments);  /* any pointer may be NULL;
                                                                  fragments == 1: no external
                                                                  fragmentation; ISR-safe, O(n) */
stk_bytepool_get_size(bp);          /* managed area in bytes, headers included */
stk_bytepool_get_max_request(bp);   /* largest request that can ever succeed */
```

### Destroying a pool

```c
stk_bytepool_destroy(bp);   /* frees heap storage if owned; not ISR-safe */
bp = NULL;
```

Do not destroy a pool while a task is blocked in `stk_bytepool_alloc()` /
`stk_bytepool_timed_alloc()`.

---

## Software Timers

Software timers are managed by `stk_timerhost_t`. One host is pre-allocated per CPU core
and can run in one of two modes:

- **Task mode** (`stk_timerhost_init()` / `stk_timerhost_init_ex()`): the host adds a tick task
  plus `STK_C_TIMER_THREADS_COUNT` handler task(s) to the kernel, and callbacks run inside a
  handler task. It occupies `1 + STK_C_TIMER_THREADS_COUNT` kernel task slots, so size
  `STK_C_KERNEL_MAX_TASKS` accordingly.
- **Polled mode** (`stk_timerhost_init_polled()`): no kernel tasks are created and you drive the
  host by calling `stk_timerhost_process()`; callbacks run inline from that call. This is the
  mode to use with an HRT kernel, where task-mode initialization is rejected. See
  [Polled mode](#polled-mode).

The two modes are mutually exclusive: call exactly one init function, before any other timer API.

Timers (`stk_timer_t`) are standalone objects constructed in caller-supplied memory
(`stk_timer_mem_t`), exactly like the synchronization primitives. There is no internal timer pool,
so the number of timers is limited only by the memory you provide. A timer is linked into the host
only while it is active.

Control calls (`stk_timer_start()`, `stk_timer_stop()`, ...) are **asynchronous**: they place a
command in the host's command queue (capacity `STK_C_TIMER_COMMAND_QUEUE_MAX`), and the tick task
executes it later. They return a `stk_timer_result_t`, where `STK_TIMER_RES_OK` means only that the
command was *accepted*. Note that `STK_TIMER_RES_OK` is `0`, so `if (stk_timer_start(...))` means
"failed": compare against `STK_TIMER_RES_OK` explicitly. See [Synchronous API](#synchronous-api) for
calls that report the exact outcome.

| `stk_timer_result_t` | Meaning |
|---|---|
| `STK_TIMER_RES_OK` | Command accepted into the command queue |
| `STK_TIMER_RES_ACTIVE` | Timer is active but the command requires it to be inactive |
| `STK_TIMER_RES_INACTIVE` | Timer is not active but the command requires it to be |
| `STK_TIMER_RES_NOT_PERIODIC` | Timer is not periodic but the command requires it to be |
| `STK_TIMER_RES_QUEUED` | Synchronous API only: the caller cannot wait, the command was queued asynchronously and its outcome is unknown |
| `STK_TIMER_RES_FAIL` | Unspecified failure |
| `STK_TIMER_RES_NOT_READY` | Host is not initialized (or its initialization failed), or it has been shut down |
| `STK_TIMER_RES_QUEUE_FULL` | Command queue is full and the caller cannot wait (ISR, kernel not started, polled mode, tick task) |
| `STK_TIMER_RES_INVALID_PARAM` | An argument is out of range (e.g. zero period for `stk_timer_set_period()`) |
| `STK_TIMER_RES_CANCELLED` | The wait for a free command queue slot was cancelled; nothing is queued |

The `ACTIVE` / `INACTIVE` / `NOT_PERIODIC` checks of the asynchronous calls are advisory: they are
evaluated from the state applied by the tick task so far. When the queue is full, a caller that can
wait (task context, task mode) blocks until a slot is free; any other caller gets
`STK_TIMER_RES_QUEUE_FULL`.

```c
#include <stk_c_time.h>

/* --- Initialization (before stk_kernel_start) --- */

stk_timerhost_t *host = stk_timerhost_get(0);   /* core 0 */
stk_timerhost_init(host, k, true);              /* privileged handler task */

/* --- Create a timer --- */

void on_timer(stk_timerhost_t *host, stk_timer_t *timer, void *user_data) {
    /* called from the timer handler task — do not call blocking APIs here
       unless you increased STK_C_TIMER_HANDLER_STACK_SIZE accordingly */
}

static stk_timer_mem_t tmr_mem;                      /* must outlive the timer: static or stack, not heap */
stk_timer_t *tmr = stk_timer_create(&tmr_mem, on_timer, NULL);   /* NULL only if membuf is NULL */

/* --- Start: delay=10 ticks before first fire, period=50 ticks (repeating) ---
   Every control call below returns stk_timer_result_t: STK_TIMER_RES_OK if the
   command was accepted, otherwise the reason (ACTIVE, NOT_READY, QUEUE_FULL, ...). */
stk_timer_result_t res = stk_timer_start(host, tmr, 10, 50);
if (res != STK_TIMER_RES_OK) { /* handle */ }

/* --- One-shot: period=0 means fire once --- */
res = stk_timer_start(host, tmr, 100, 0);

/* --- Control --- */
res = stk_timer_stop(host, tmr);
res = stk_timer_reset(host, tmr);                    /* restart the current delay */
res = stk_timer_restart(host, tmr, 20, 50);          /* atomic stop+start with new params */
res = stk_timer_start_or_reset(host, tmr, 20, 50);   /* start if idle, reset if active */
res = stk_timer_set_period(host, tmr, 100);          /* new period takes effect next reload;
                                                          follow with stk_timer_reset() to
                                                          apply it immediately */

/* --- Query --- */
stk_timer_is_active(tmr);
stk_timer_get_period(tmr);
stk_timer_get_deadline(tmr);           /* absolute expiration tick of the next fire */
stk_timer_get_timestamp(tmr);          /* tick count at which it last expired */
stk_timer_get_remaining_ticks(tmr);

/* --- Cleanup: stop synchronously first, then destroy --- */
stk_timer_stop_sync(host, tmr);   /* returns when the host holds no reference to the timer
                                     and no callback of it is running */
stk_timer_destroy(tmr);           /* NULL-safe; tmr_mem may be reused afterwards */
```

### Synchronous API

The asynchronous functions only report whether a request was accepted. The `_sync` functions queue
the same command together with a completion point and block the caller until the tick task has
executed it, so the result is the exact outcome (e.g. `STK_TIMER_RES_ACTIVE` for a timer that was
already active).

```c
stk_timer_result_t r;
r = stk_timer_start_sync(host, tmr, 10, 50);      /* OK, or ACTIVE (nothing changed) */
r = stk_timer_activate_sync(host, tmr);           /* start with the delay/period stored in the timer */
r = stk_timer_stop_sync(host, tmr);               /* OK if it was active, INACTIVE if not */
r = stk_timer_restart_sync(host, tmr, 20, 50);    /* atomic stop+start */
r = stk_timer_reset_sync(host, tmr);              /* OK, INACTIVE or NOT_PERIODIC */
r = stk_timer_set_period_sync(host, tmr, 100);    /* OK, INACTIVE, NOT_PERIODIC or INVALID_PARAM */
r = stk_timer_change_sync(host, tmr, 20, 50);     /* change delay/period of an INACTIVE timer, does not start it */

stk_timer_info_t info;
r = stk_timer_query_sync(host, tmr, &info);       /* consistent snapshot: active, remaining, delay, period */
```

Rules:

- A call waits only where waiting is possible: task context, kernel running, task mode. From an ISR,
  before `stk_kernel_start()`, or in polled mode the command is queued asynchronously and
  `STK_TIMER_RES_QUEUED` is returned (`STK_TIMER_RES_QUEUE_FULL` if the queue is full).
- A sync call blocks while the command queue is full instead of failing.
- The tick task must be at least as urgent as every task that uses this API (see the weights in
  `stk_timerhost_config_t`).
- Callbacks may use this API in task mode (they run in handler tasks).
- `stk_timer_stop_sync()` also waits until a callback of this timer that a handler task has already
  started has returned, so after `OK` / `INACTIVE` the timer memory may be released. Called from a
  timer callback it does not wait (deadlock guard): it only guarantees that no further callback of
  the timer starts. Do not release a timer from a callback of another timer.

### Custom Weights and Stacks

`stk_timerhost_init_ex()` is like `stk_timerhost_init()` but accepts a configuration and reports
failures. Always fill the configuration with `stk_timerhost_config_init()` first, then override
the fields you need:

```c
static __stk_c_stack stk_word_t tick_stack[128];
static __stk_c_stack stk_word_t handler_stack[STK_C_TIMER_THREADS_COUNT][256];

stk_timerhost_config_t cfg;
stk_timerhost_config_init(&cfg);               /* default weights, internal stacks */

cfg.handler_weight = 2;                        /* strategy-specific, passed to the kernel unchanged */
cfg.tick_stack = tick_stack;
cfg.tick_stack_size = 128;                     /* in stk_word_t elements */
cfg.handler_stack[0] = handler_stack[0];       /* a NULL entry selects the internal buffer */
cfg.handler_stack_size = 256;                  /* applies to every non-NULL handler_stack entry */

stk_add_result_t r = stk_timerhost_init_ex(host, k, true, &cfg);   /* cfg may be NULL for defaults */
if (r != STK_ADD_OK) { /* STK_ADD_FAIL_POOL_FULL, _DUPLICATE (shared stack), _INVALID (e.g. HRT kernel) */ }
```

Notes:

- Weights are strategy-specific. With the preemption-threshold strategy they must be set
  explicitly; the default weight is not neutral for it.
- Externally supplied stacks must outlive the host, be distinct from each other, and meet the
  platform's alignment and memory-protection requirements.
- On failure the host is unusable, and tasks added before the failure stay registered with the kernel.
- The tick task always runs in user mode; `privileged` applies to the handler tasks.

### Polled mode

```c
stk_timerhost_init_polled(host);   /* cannot fail; needs no kernel, task slots, weights or stacks */

static stk_timer_mem_t tmr_mem;
stk_timer_t *tmr = stk_timer_create(&tmr_mem, on_timer, NULL);
stk_timer_start(host, tmr, 10, 50);

/* In one periodic context (e.g. an HRT job): */
stk_timeout_t next = stk_timerhost_process(host);
/* next == STK_NO_WAIT       : a timer is already overdue, call again without delay
   next == STK_WAIT_INFINITE : no timer is active
   otherwise                 : ticks until the earliest deadline */
```

`stk_timerhost_process()` executes pending commands, evaluates deadlines and invokes the callback
of every expired timer inline. It never blocks. Call it from a single context only: not
concurrently and not from a timer callback. `stk_timer_start()` and the other control functions
remain callable from any task and take effect at the next `stk_timerhost_process()` call.

A timer fires on the first call at or after its deadline, so the calling period is the timer
resolution, and callback execution time counts against the caller (for example, against the WCET of
an HRT job).

### TimerHost Management

```c
stk_timerhost_is_empty(host);        /* true if no timers are currently active */
stk_timerhost_get_size(host);        /* number of currently active timers */
stk_timerhost_get_time_now(host);    /* host's last tick-count snapshot */

stk_timer_result_t res = stk_timerhost_shutdown(host);  /* stops all active timers; OK if the request was
                                                            accepted, else QUEUE_FULL / NOT_READY (not
                                                            initialized or already shut down) */
```

After `stk_timerhost_shutdown()` in task mode, the host must not be used until it is initialized
again. In polled mode the host stays usable.

### Periodic Trigger (polling alternative)

If you prefer a polling style inside a task instead of a callback:

```c
#include <stk_c_time.h>

static stk_periodic_trigger_mem_t trig_mem;
stk_periodic_trigger_t *trig = stk_periodic_trigger_create(
    &trig_mem,
    50,    /* period in ticks, must be > 0 */
    true); /* started; false = stopped until stk_periodic_trigger_restart() */

while (1) {
    if (stk_periodic_trigger_poll(trig)) {
        /* runs every 50 ticks; absolute-time scheduling keeps long-term
           frequency stable even if a call is delayed */
    }
    stk_yield();
}

/* Change period while preserving phase progress toward the next firing: */
stk_periodic_trigger_set_period(trig, 100);

/* Reset and (re)start counting from the current tick, without changing period: */
stk_periodic_trigger_restart(trig);

stk_periodic_trigger_get_period(trig);   /* currently configured period in ticks */

stk_periodic_trigger_destroy(trig);      /* NULL-safe no-op */
```

---

## Thread-Local Storage (TLS)

Each task has one pointer-sized TLS slot backed by a CPU register (zero overhead).

```c
typedef struct {
    int  counter;
    void *context;
} my_tls_t;

static my_tls_t my_data = { 0, NULL };

void my_task(void *arg) {
    STK_TLS_SET_T(&my_data);          /* store pointer into TLS slot */

    while (1) {
        my_tls_t *tls = STK_TLS_GET_T(my_tls_t);
        tls->counter++;
        stk_sleep_ms(100);
    }
}
```

`STK_TLS_GET_T(type)` expands to `((type *)stk_tls_get())`.
`STK_TLS_SET_T(ptr)` expands to `stk_tls_set((void *)(ptr))`.
The untyped `stk_tls_get()` / `stk_tls_set(ptr)` functions underneath these
macros are also available directly.

---

## Common Pitfalls

**Ignoring the `stk_kernel_add_task()` / `stk_kernel_add_task_hrt()` result** — In a `KERNEL_DYNAMIC` kernel a
failure (`STK_ADD_FAIL_POOL_FULL`, `STK_ADD_FAIL_DUPLICATE`, ...) is only returned,
never asserted, so an unchecked call silently leaves the task unstarted. The handle
also stays allocated: retry or `stk_task_destroy()` it.

**Calling `stk_task_destroy()` on an exited task** — A task that returned from its
entry function, called `stk_exit_task()` or was removed is released by the kernel.
Destroying it again is a double free.

**Stack size too small** — Start with 256 words and increase if you observe
hard faults or corrupted data. If in doubt, use a stack watermarking tool and
add at least 20–30% margin.

**`stk_sleep_ms()` in an HRT task** — HRT tasks must use `stk_yield()` to
signal end-of-period. Calling `stk_sleep_ms()` in an HRT task is not supported
and will cause misbehaviour.

**Blocking call from a timer callback** — In task mode the timer callback executes inside the
timer handler task. Calling a blocking API (e.g. `stk_mutex_lock()` with a
non-zero timeout) requires a stack large enough to support it. Increase
`STK_C_TIMER_HANDLER_STACK_SIZE` if needed. In polled mode the callback runs inline in the
caller of `stk_timerhost_process()`, so it uses that task's stack and time budget.

**Mixing timer host modes** — `stk_timerhost_init_polled()` and
`stk_timerhost_init()` / `stk_timerhost_init_ex()` are mutually exclusive. A host in task
mode on an HRT kernel is rejected (`STK_ADD_FAIL_INVALID`): use polled mode there.

**Mismatched `STK_C_SEM_VALUE_TYPE` / `STK_C_EF_VALUE_TYPE` / `STK_C_TIMER_THREADS_COUNT` /
`STK_C_TIMER_COMMAND_QUEUE_MAX`** — These change the layout or sizing of types in the public headers. Define them identically in every translation unit (and in the
`.cpp` files of the binding), otherwise structure sizes will disagree.

**Forgetting `KERNEL_SYNC`** — Mutex, semaphore, event, and message queue are
compiled in only when `KERNEL_SYNC` is part of the kernel flags. Without it,
the sync API will link but the kernel has no scheduler hooks, leading to
undefined behaviour.

**Destroying an active timer** — `stk_timer_destroy()` asserts that the timer
is not active. Call `stk_timer_stop_sync()` first: it returns only when the host holds no
reference to the timer and no callback of it is running, so the timer memory can be reused safely.
The asynchronous `stk_timer_stop()` only queues the request and gives no such guarantee.

**Treating `STK_TIMER_RES_OK` as "true"** — Timer control calls return `stk_timer_result_t`, and
`STK_TIMER_RES_OK` is `0`. `if (stk_timer_start(...))` therefore means *failed*. Compare with
`STK_TIMER_RES_OK` explicitly. Also remember that `OK` from an asynchronous call only means the
command was queued; use the `_sync` variants for the exact outcome.

**Releasing timer memory too early** — The `stk_timer_mem_t` container must stay valid (not reused,
not freed, not a dead stack frame) from `stk_timer_create()` until the timer is stopped and destroyed.

**Using the plain critical section from a User thread** — `stk_critical_section_enter()` /
`stk_critical_section_exit()` are for Privileged threads only. Use the `_ex()` variants in code that
may run in a User thread.

**Suspending self while holding a critical section** — Calling
`stk_kernel_suspend_task()` on the currently running task blocks until the
scheduler switches it out. If a critical section is held at that point, the
system will deadlock.

**Destroying a Pool/Pipe/MessageQueue with blocked tasks** — `stk_blockpool_destroy()`,
`stk_pipe_destroy()`, and `stk_msgq_destroy()` all assert in debug builds if
any task is still blocked in a call on that object. Make sure producers and
consumers have stopped before tearing one down.

**Destroying a Byte Pool with blocked tasks** — `stk_bytepool_destroy()` while a task is blocked in
`stk_bytepool_alloc()`/`stk_bytepool_timed_alloc()` is a logic error.

**`stk_kernel_suspend()`/`stk_kernel_resume()` vs `stk_kernel_suspend_task()`/
`stk_kernel_resume_task()`** — The former pair suspends and resumes the whole
scheduler (used for tickless idle entry/exit); the latter pair suspends and
resumes one specific task. They are not interchangeable.

---

## Configuration Reference

Place these in `stk_config.h` before including any STK header.

```c
/* Maximum tasks tracked by the C binding (shared across all kernel instances) */
#define STK_C_KERNEL_MAX_TASKS   8

/* Number of independent CPU cores / kernel instances */
#define STK_C_CPU_COUNT          1

/* Maximum concurrent block pools */
#define STK_C_BLOCKPOOL_MAX      4

/* Maximum concurrent byte pools */
#define STK_C_BYTEPOOL_MAX       4

/* Capacity of each timer host's command queue (timers themselves live in
   caller-supplied memory and are not limited by this value).
   The deprecated STK_C_TIMER_MAX is still honored if this is not defined. */
#define STK_C_TIMER_COMMAND_QUEUE_MAX   16

/* Stack size (words) for the timer handler task */
#define STK_C_TIMER_HANDLER_STACK_SIZE  512

/* Number of timer handler tasks in task mode (host uses 1 + this many task slots) */
#define STK_C_TIMER_THREADS_COUNT       1

/* Semaphore counter type: uint8_t, uint16_t, uint32_t or uint64_t */
#define STK_C_SEM_VALUE_TYPE            uint32_t

/* EventFlags word type: uint8_t, uint16_t, uint32_t or uint64_t (MSB is reserved as error bit) */
#define STK_C_EF_VALUE_TYPE             uint32_t

/* Kernel type for core 0 — required */
#define STK_C_KERNEL_TYPE_CPU_0 \
    Kernel<KERNEL_STATIC | KERNEL_SYNC, \
           STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>
```
