# STK C Interface

The C interface provides full access to the C++ version of STK from plain C code.
All synchronization, memory, timing, and scheduling features are exposed through a
pure C API with no C++ headers required in your source files.

---

## Contents

- [Headers and Sources](#headers-and-sources)
- [Configuration](#configuration)
- [Quick Start](#quick-start)
- [Step-by-Step Setup](#step-by-step-setup)
  - [1. Configure the Kernel Type](#1-configure-the-kernel-type)
  - [2. Create and Start the Kernel](#2-create-and-start-the-kernel)
  - [3. Create Tasks](#3-create-tasks)
- [Kernel Modes](#kernel-modes)
  - [Choosing a Scheduling Strategy](#choosing-a-scheduling-strategy)
  - [Hard Real-Time (HRT)](#hard-real-time-hrt)
  - [Tickless / Low-Power](#tickless--low-power)
- [Task Lifecycle](#task-lifecycle)
  - [Static Kernel](#static-kernel)
  - [Dynamic Kernel](#dynamic-kernel)
  - [Task Naming and Priority](#task-naming-and-priority)
  - [Suspend and Resume](#suspend-and-resume)
- [Timing Services](#timing-services)
  - [Sleep vs Delay](#sleep-vs-delay)
  - [Time Conversion Helpers](#time-conversion-helpers)
  - [Raw System Timer](#raw-system-timer)
  - [High-Resolution Clock](#high-resolution-clock)
- [Kernel Introspection and Control](#kernel-introspection-and-control)
  - [Enumerating Tasks](#enumerating-tasks)
  - [Manual Tick Injection](#manual-tick-injection)
  - [Tickless Scheduling Suspend/Resume](#tickless-scheduling-suspendresume)
  - [Platform Event Overrider](#platform-event-overrider)
  - [Dynamic Kernel/Task Cleanup](#dynamic-kerneltask-cleanup)
- [Synchronization Primitives](#synchronization-primitives)
  - [Critical Section](#critical-section)
  - [Mutex](#mutex)
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
- [Software Timers](#software-timers)
- [Thread-Local Storage (TLS)](#thread-local-storage-tls)
- [Common Pitfalls](#common-pitfalls)
- [Configuration Reference](#configuration-reference)

---

## Headers and Sources

| File | Purpose |
|---|---|
| `stk_c.h` | Kernel, tasks, timing, and synchronization |
| `stk_c_memory.h` | Block memory pool |
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
| `STK_C_TIMER_MAX` | `32` | Max concurrent software timers per core |
| `STK_C_TIMER_HANDLER_STACK_SIZE` | `256` | Stack words for the timer handler task |

The most important configuration step is declaring the kernel type. See
[Step 1](#1-configure-the-kernel-type) below.

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

    stk_kernel_add_task(k, t0);
    stk_kernel_add_task(k, t1);

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
stk_kernel_add_task_hrt(k, task, 10, 8, 0);
```

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

Tasks may return. The kernel calls `stk_task_destroy()` automatically when a
task's entry function returns. You can also remove a running task from another
task or ISR:

```c
/* Schedule removal on the next tick (safe to call from any context) */
stk_kernel_schedule_task_removal(k, target_task);

/* Or remove a task that has already returned */
stk_kernel_remove_task(k, finished_task);
```

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
bool was_suspended;
stk_kernel_suspend_task(k, t, &was_suspended);   /* blocks until switch-out if self */
/* ... later ... */
stk_kernel_resume_task(k, t);
```

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
int32_t res   = stk_tick_resolution();     /* µs per tick */
int64_t ticks = stk_ticks_from_ms(250);    /* 250 ms → ticks */
int64_t ms    = stk_ms_from_ticks(ticks);  /* ticks → ms */
int64_t now   = stk_time_now_ms();         /* ms since kernel start */
int64_t t     = stk_ticks();               /* raw tick counter */
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
stk_task_destroy(finished_task);   /* task must have exited or been removed */
stk_kernel_destroy(k);             /* kernel must not be running */
```

Only valid for dynamically created kernels/tasks whose tasks have all exited
or been removed.

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

Use the `_ex()` variants when the calling context's privilege level cannot be
auto-detected (e.g. code that may run from either a privileged or user-mode
task) or when threading a session value through manually:

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
stk_mutex_t *mtx = stk_mutex_create(&mtx_mem, sizeof(mtx_mem));

stk_mutex_lock(mtx);                        /* blocks until available */
bool ok = stk_mutex_trylock(mtx);           /* non-blocking */
bool ok = stk_mutex_timed_lock(mtx, 100);   /* ticks timeout */
stk_mutex_unlock(mtx);

stk_mutex_destroy(mtx);
```

### SpinLock

Suitable for very short critical regions and ISR-to-task handoff.

```c
static stk_spinlock_mem_t sl_mem;
stk_spinlock_t *sl = stk_spinlock_create(&sl_mem, sizeof(sl_mem));

stk_spinlock_lock(sl);
stk_spinlock_unlock(sl);
```

### Condition Variable

Always paired with a locked mutex protecting the guarded state.

```c
static stk_cv_mem_t cv_mem;
stk_cv_t *cv = stk_cv_create(&cv_mem, sizeof(cv_mem));

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

> ISR-safe only with `timeout = STK_NO_WAIT`; ISR-unsafe otherwise.

### Semaphore

```c
static stk_sem_mem_t sem_mem;
/* initial value = 0, max value = 1 → binary semaphore */
stk_sem_t *sem = stk_sem_create(&sem_mem, sizeof(sem_mem), 0, 1);

stk_sem_signal(sem);                    /* post / give — ISR-safe */
bool ok = stk_sem_wait(sem, STK_WAIT_INFINITE); /* blocks */
bool ok = stk_sem_trywait(sem);         /* non-blocking poll, ISR-safe */
bool ok = stk_sem_wait(sem, 50);        /* ticks timeout */

uint16_t n = stk_sem_get_count(sem);    /* current resource counter */

stk_sem_destroy(sem);
```

### Event

A binary signal (signaled / non-signaled). Supports auto-reset and manual-reset modes.

```c
static stk_event_mem_t ev_mem;
/* false = auto-reset, true = manual-reset */
stk_event_t *ev = stk_event_create(&ev_mem, sizeof(ev_mem), false);

/* From ISR or another task: */
bool changed = stk_event_set(ev);   /* signal — ISR-safe; true if it was non-signaled */
changed = stk_event_reset(ev);      /* clear — ISR-safe; true if it was signaled */
stk_event_pulse(ev);                /* signal then immediately reset */

/* Waiting task: */
bool ok = stk_event_wait(ev, STK_WAIT_INFINITE);  /* blocks */
bool ok = stk_event_wait(ev, 100);                /* ticks timeout */
bool ok = stk_event_trywait(ev);                  /* non-blocking, ISR-safe */

stk_event_destroy(ev);
```

### EventFlags

A 32-bit flags word. Tasks can wait for any subset (OR) or all bits (AND).

```c
static stk_ef_mem_t ef_mem;
stk_ef_t *ef = stk_ef_create(&ef_mem, sizeof(ef_mem), 0);

#define EVT_BUTTON  (1u << 0)
#define EVT_UART_RX (1u << 1)

/* From ISR or another task: */
stk_ef_set(ef, EVT_BUTTON);                 /* ISR-safe */

/* Wait for any of the bits (clears them on return): */
uint32_t fired = stk_ef_wait(ef, EVT_BUTTON | EVT_UART_RX,
                             STK_EF_OPT_WAIT_ANY, STK_WAIT_INFINITE);

/* Wait for ALL bits: */
uint32_t fired = stk_ef_wait(ef, EVT_BUTTON | EVT_UART_RX,
                             STK_EF_OPT_WAIT_ALL, STK_WAIT_INFINITE);

if (stk_ef_is_error(fired)) { /* timeout or invalid flags */ }

stk_ef_destroy(ef);
```

### Pipe

A fixed-size-element FIFO, simpler than the Message Queue (no front-insert or
peek), with bulk and threshold-triggered read support.

```c
typedef struct { uint32_t id; uint8_t data[16]; } Sample;

#define PIPE_CAP 16

static stk_pipe_mem_t s_pipe_mem;
static uint8_t        s_pipe_buf[STK_PIPE_BUF_SIZE(PIPE_CAP, sizeof(Sample))];

stk_pipe_t *pipe = stk_pipe_create(&s_pipe_mem, sizeof(s_pipe_mem),
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

stk_pipe_reset(pipe);   /* discard all elements, ISR-safe */

/* Query (all ISR-safe): */
stk_pipe_get_capacity(pipe);
stk_pipe_get_count(pipe);
stk_pipe_get_space(pipe);
stk_pipe_is_empty(pipe);
stk_pipe_is_full(pipe);

stk_pipe_destroy(pipe);
```

> ISR-safe only with `timeout = STK_NO_WAIT` (or the `try*` variants, which
> are always ISR-safe).

### Message Queue

Zero-copy message queue that copies a fixed-size payload. Unlike Pipe, it
also supports front-insertion (priority messages) and non-destructive peek.

```c
typedef struct { uint32_t id; uint8_t data[16]; } Msg;

static stk_msgq_mem_t mq_mem;
static uint8_t        mq_buf[8 * sizeof(Msg)];   /* capacity × message size, or
                                                     use STK_MSGQ_BUF_SIZE(8, sizeof(Msg)) */

stk_msgq_t *mq = stk_msgq_create(&mq_mem, sizeof(mq_mem),
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
stk_rwmutex_t *rw = stk_rwmutex_create(&rw_mem, sizeof(rw_mem));

/* Reader: */
stk_rwmutex_read_lock(rw);
/* ... read shared data ... */
stk_rwmutex_read_unlock(rw);

/* Writer: */
stk_rwmutex_lock(rw);
/* ... modify shared data ... */
stk_rwmutex_unlock(rw);

stk_rwmutex_destroy(rw);
```

### Barrier

Blocks a fixed number of tasks until they have all arrived, then releases
them together and resets itself for reuse.

```c
static stk_barrier_mem_t bar_mem;
stk_barrier_t *bar = stk_barrier_create(&bar_mem, sizeof(bar_mem), 3 /* task count */);

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

## Software Timers

Software timers run their callback inside a dedicated kernel task managed by
`stk_timerhost_t`. One host is pre-allocated per CPU core.

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

stk_timer_t *tmr = stk_timer_create(on_timer, NULL);

/* --- Start: delay=10 ticks before first fire, period=50 ticks (repeating) ---
   Every control call below returns bool: true on success, false if the
   timer's preconditions weren't met or the internal command queue is full. */
bool ok = stk_timer_start(host, tmr, 10, 50);

/* --- One-shot: period=0 means fire once --- */
ok = stk_timer_start(host, tmr, 100, 0);

/* --- Control --- */
ok = stk_timer_stop(host, tmr);
ok = stk_timer_reset(host, tmr);                     /* restart the current delay */
ok = stk_timer_restart(host, tmr, 20, 50);           /* atomic stop+start with new params */
ok = stk_timer_start_or_reset(host, tmr, 20, 50);    /* start if idle, reset if active */
ok = stk_timer_set_period(host, tmr, 100);           /* new period takes effect next reload;
                                                          follow with stk_timer_reset() to
                                                          apply it immediately */

/* --- Query --- */
stk_timer_is_active(tmr);
stk_timer_get_period(tmr);
stk_timer_get_deadline(tmr);           /* absolute expiration tick of the next fire */
stk_timer_get_timestamp(tmr);          /* tick count at which it last expired */
stk_timer_get_remaining_ticks(tmr);

/* --- Cleanup --- */
stk_timer_stop(host, tmr);
stk_timer_destroy(tmr);
```

### TimerHost Management

```c
stk_timerhost_is_empty(host);        /* true if no timers are currently active */
stk_timerhost_get_size(host);        /* number of currently active timers */
stk_timerhost_get_time_now(host);    /* host's last tick-count snapshot */

bool ok = stk_timerhost_shutdown(host);  /* stops all active timers; host must
                                             not be used afterward */
```

### Periodic Trigger (polling alternative)

If you prefer a polling style inside a task instead of a callback:

```c
#include <stk_c_time.h>

static stk_periodic_trigger_mem_t trig_mem;
stk_periodic_trigger_t *trig = stk_periodic_trigger_create(
    &trig_mem, sizeof(trig_mem),
    50,    /* period in ticks */
    true); /* start immediately */

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

**Stack size too small** — Start with 256 words and increase if you observe
hard faults or corrupted data. If in doubt, use a stack watermarking tool and
add at least 20–30% margin.

**`stk_sleep_ms()` in an HRT task** — HRT tasks must use `stk_yield()` to
signal end-of-period. Calling `stk_sleep_ms()` in an HRT task is not supported
and will cause misbehaviour.

**Blocking call from a timer callback** — The timer callback executes inside the
timer handler task. Calling a blocking API (e.g. `stk_mutex_lock()` with a
non-zero timeout) requires a stack large enough to support it. Increase
`STK_C_TIMER_HANDLER_STACK_SIZE` if needed.

**Forgetting `KERNEL_SYNC`** — Mutex, semaphore, event, and message queue are
compiled in only when `KERNEL_SYNC` is part of the kernel flags. Without it,
the sync API will link but the kernel has no scheduler hooks, leading to
undefined behaviour.

**Destroying an active timer** — `stk_timer_destroy()` asserts that the timer
is not active. Always call `stk_timer_stop()` first.

**Suspending self while holding a critical section** — Calling
`stk_kernel_suspend_task()` on the currently running task blocks until the
scheduler switches it out. If a critical section is held at that point, the
system will deadlock.

**Destroying a Pool/Pipe/MessageQueue with blocked tasks** — `stk_blockpool_destroy()`,
`stk_pipe_destroy()`, and `stk_msgq_destroy()` all assert in debug builds if
any task is still blocked in a call on that object. Make sure producers and
consumers have stopped before tearing one down.

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

/* Maximum concurrent software timers per core */
#define STK_C_TIMER_MAX          16

/* Stack size (words) for the timer handler task */
#define STK_C_TIMER_HANDLER_STACK_SIZE  512

/* Kernel type for core 0 — required */
#define STK_C_KERNEL_TYPE_CPU_0 \
    Kernel<KERNEL_STATIC | KERNEL_SYNC, \
           STK_C_KERNEL_MAX_TASKS, SwitchStrategyFP32, PlatformDefault>
```
