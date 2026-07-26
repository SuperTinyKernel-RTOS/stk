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
  - [High-Resolution Clock](#high-resolution-clock)
- [Synchronization Primitives](#synchronization-primitives)
  - [Critical Section](#critical-section)
  - [Mutex](#mutex)
  - [SpinLock](#spinlock)
  - [Semaphore](#semaphore)
  - [Event](#event)
  - [EventFlags](#eventflags)
  - [Message Queue](#message-queue)
  - [Reader-Writer Lock](#reader-writer-lock)
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

### Time Conversion Helpers

```c
int32_t res   = stk_tick_resolution();     /* µs per tick */
int64_t ticks = stk_ticks_from_ms(250);    /* 250 ms → ticks */
int64_t ms    = stk_ms_from_ticks(ticks);  /* ticks → ms */
int64_t now   = stk_time_now_ms();         /* ms since kernel start */
int64_t t     = stk_ticks();               /* raw tick counter */
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

## Synchronization Primitives

All primitives require `KERNEL_SYNC` in the kernel flags. Memory for each
primitive is supplied by the caller — no heap allocation occurs.

### Critical Section

Disables context switches on the current core. Supports nesting.

```c
static stk_cs_mem_t cs_mem;
stk_cs_t *cs = stk_cs_create(&cs_mem, sizeof(cs_mem));

stk_cs_enter(cs);
/* protected region */
stk_cs_exit(cs);

stk_cs_destroy(cs);
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
stk_event_set(ev);                                /* signal — ISR-safe */
stk_event_reset(ev);                              /* clear — ISR-safe */
stk_event_pulse(ev);                              /* signal then immediately reset */

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

### Message Queue

Zero-copy message queue that copies a fixed-size payload.

```c
typedef struct { uint32_t id; uint8_t data[16]; } Msg;

static stk_msgq_mem_t mq_mem;
static uint8_t        mq_buf[8 * sizeof(Msg)];   /* capacity × message size */

stk_msgq_t *mq = stk_msgq_create(&mq_mem, sizeof(mq_mem),
                                  mq_buf, sizeof(mq_buf),
                                  8, sizeof(Msg));

/* Producer (ISR-safe with STK_NO_WAIT): */
Msg out = { .id = 42 };
stk_msgq_put(mq, &out, STK_NO_WAIT);

/* Consumer: */
Msg in;
if (stk_msgq_get(mq, &in, STK_WAIT_INFINITE)) {
    /* process in */
}

stk_msgq_destroy(mq);
```

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

stk_blockpool_free(pool, pkt);   /* ISR-safe; wakes blocked allocators */
```

### Heap storage

```c
stk_blockpool_t *pool = stk_blockpool_create(PKT_COUNT, PKT_SIZE, "pkt_pool");
if (!stk_blockpool_is_storage_valid(pool)) { /* allocation failed */ }
```

### Blocking allocation

```c
void *blk = stk_blockpool_alloc(pool);                    /* blocks until available */
void *blk = stk_blockpool_timed_alloc(pool, 100);         /* ticks timeout, NULL on timeout */
void *blk = stk_blockpool_try_alloc(pool);                /* non-blocking, ISR-safe */
```

### Query

```c
stk_blockpool_get_capacity(pool);   /* total blocks */
stk_blockpool_get_used_count(pool); /* currently allocated */
stk_blockpool_get_free_count(pool); /* available */
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

/* --- Start: delay=10 ticks before first fire, period=50 ticks (repeating) --- */
stk_timer_start(host, tmr, 10, 50);

/* --- One-shot: period=0 means fire once --- */
stk_timer_start(host, tmr, 100, 0);

/* --- Control --- */
stk_timer_stop(host, tmr);
stk_timer_reset(host, tmr);                     /* restart the current delay */
stk_timer_restart(host, tmr, 20, 50);           /* stop then start with new params */
stk_timer_start_or_reset(host, tmr, 20, 50);    /* start if idle, reset if active */
stk_timer_set_period(host, tmr, 100);           /* change period while running */

/* --- Query --- */
stk_timer_is_active(tmr);
stk_timer_get_period(tmr);
stk_timer_get_remaining_ticks(tmr);

/* --- Cleanup --- */
stk_timer_stop(host, tmr);
stk_timer_destroy(tmr);
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
        /* runs every 50 ticks */
    }
    stk_yield();
}
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
    STK_TLS_SET(&my_data);          /* store pointer into TLS slot */

    while (1) {
        my_tls_t *tls = STK_TLS_GET(my_tls_t);
        tls->counter++;
        stk_sleep_ms(100);
    }
}
```

`STK_TLS_GET(type)` expands to `((type *)stk_tls_get())`.
`STK_TLS_SET(ptr)` expands to `stk_tls_set((void *)(ptr))`.

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
