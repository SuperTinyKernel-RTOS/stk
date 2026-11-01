# STK POSIX API

The POSIX API is a minimal, POSIX-named layer on top of STK. It lets code written against POSIX
threads and the real-time extensions (threads, mutexes, condition variables, read-write locks,
barriers, thread-specific data, semaphores, message queues, clocks, timers, cancellation, thread
priorities, C11 `<threads.h>`) run on STK with little or no change. The public headers are plain C
(no C++ headers), so they can be used from C; the implementation calls the STK C++ API directly and
**does not depend on the STK C interface** (`stk_c.h`, `stk_c.cpp`, ...).

This is deliberately a **subset** of POSIX, limited to what maps cleanly onto STK's task and
synchronization model. See [What Is Supported](#what-is-supported) and
[What Is Not Supported](#what-is-not-supported).

> The layer lives in `interop/posix` (formerly `interop/pthread`). Files and documentation that
> still refer to the old folder name should be updated to the new path; the C symbols and
> headers keep their names, so existing application code does not change.

---

## Contents

- [Headers and Sources](#headers-and-sources)
- [What Is Supported](#what-is-supported)
- [What Is Not Supported](#what-is-not-supported)
- [Configuration](#configuration)
- [Quick Start](#quick-start)
- [Example Project](#example-project)
- [Step-by-Step Setup](#step-by-step-setup)
  - [1. Add the Include Path and Sources](#1-add-the-include-path-and-sources)
  - [2. Configure the Kernel Type](#2-configure-the-kernel-type)
  - [3. Size the Task Table](#3-size-the-task-table)
  - [4. Create, Bind, and Start the Kernel](#4-create-bind-and-start-the-kernel)
  - [5. Create Threads from the Bootstrap Task](#5-create-threads-from-the-bootstrap-task)
- [Threads](#threads)
  - [Creating and Joining](#creating-and-joining)
  - [Thread Attributes](#thread-attributes)
  - [Detached Threads](#detached-threads)
  - [Exiting a Thread](#exiting-a-thread)
  - [Thread Identity and Adoption](#thread-identity-and-adoption)
  - [Thread Names](#thread-names)
  - [Stacks and Memory](#stacks-and-memory)
- [Scheduling and Priorities](#scheduling-and-priorities)
- [Cancellation](#cancellation)
  - [Terminating a Thread](#terminating-a-thread)
- [Mutex](#mutex)
- [Condition Variable](#condition-variable)
- [Timed Waits and abstime](#timed-waits-and-abstime)
- [Reader-Writer Lock](#reader-writer-lock)
- [Spinlock](#spinlock)
- [Barrier](#barrier)
- [One-Time Initialization](#one-time-initialization)
- [Thread-Specific Data (Keys)](#thread-specific-data-keys)
- [Semaphores](#semaphores)
- [Message Queues](#message-queues)
- [Clocks, Sleeping and Wall Time](#clocks-sleeping-and-wall-time)
- [Timers](#timers)
- [System Configuration](#system-configuration)
- [C11 Threads](#c11-threads)
- [Static Initializers](#static-initializers)
- [Context Restrictions (ISR)](#context-restrictions-isr)
- [Return Codes and errno](#return-codes-and-errno)
- [How It Maps to STK](#how-it-maps-to-stk)
- [Toolchain Integration Notes](#toolchain-integration-notes)
- [Common Pitfalls](#common-pitfalls)
- [Configuration Reference](#configuration-reference)

---

## Headers and Sources

| File | Purpose |
|---|---|
| `pthread.h` | Entry point for `<pthread.h>`, forwards to `stk_c_pthread.h` |
| `stk_c_pthread.h` | Pthread types, constants and declarations (threads, sync objects, cancellation, attributes) |
| `sched.h` | `struct sched_param`, `SCHED_*`, `sched_yield()`, `sched_get_priority_min/max()` |
| `semaphore.h` | POSIX unnamed semaphores: `sem_t`, `sem_*` |
| `mqueue.h` | POSIX message queues: `mqd_t`, `mq_*` |
| `time.h` | Extends the toolchain's `<time.h>`: `clock_*`, `nanosleep()`, `usleep()`, `sleep()`, `timer_*`, `timespec_get()` |
| `sys/time.h` | Extends the toolchain's `<sys/time.h>`: `gettimeofday()` |
| `unistd.h` | Extends the toolchain's `<unistd.h>`: `sysconf()`, `getpid()` |
| `signal.h` | Wraps the toolchain's `<signal.h>`: makes `struct sigevent` / `pthread_kill()` declarations usable next to the layer's pthread types (no signal is implemented) |
| `threads.h` | C11 `<threads.h>` on top of the pthread layer |
| `stk_c_pthread.cpp` | Implementation of everything above except C11 threads |
| `stk_c_threads.cpp` | Implementation of C11 `<threads.h>` (needs only the public pthread/time headers) |

`time.h`, `sys/time.h`, `unistd.h` and `signal.h` first include the toolchain's own header
(`#include_next`) and then add to it, so they sit in front of the toolchain on the include path
without hiding anything. See [Toolchain Integration Notes](#toolchain-integration-notes).

Compile the two `.cpp` files with your project (the layer needs only the STK C++ API):

```
stk_c_pthread.cpp
stk_c_threads.cpp      # only if you use <threads.h>
```

No STK C-interface source (`stk_c.cpp`, `stk_c_sync.cpp`, `stk_c_memory.cpp`, ...) is required by
this layer. If your application itself uses the C interface it can of course still link it.

Include `pthread.h` (or `stk_c_pthread.h`) **instead of** the toolchain's `<pthread.h>`. The header
neutralizes the toolchain's own pthread type definitions so the two do not clash. Error codes are
standard `errno` values (`EINVAL`, `EAGAIN`, `ETIMEDOUT`, ...); include `<errno.h>` in files that
test for them.

---

## What Is Supported

| POSIX area | Support |
|---|---|
| `pthread_create` / `join` / `detach` / `exit` / `self` / `equal` | Yes |
| `pthread_tryjoin_np` / `pthread_timedjoin_np` | Yes (GNU-style extensions) |
| `pthread_yield` / `sched_yield` | Yes |
| `pthread_attr_t` (stack size, external stack, detach state) | Yes |
| `pthread_attr_set/get` scheduling: `schedparam`, `schedpolicy`, `inheritsched`, `scope`, `guardsize` | Yes (priority = task weight, see [Scheduling and Priorities](#scheduling-and-priorities)) |
| `pthread_getschedparam` / `pthread_setschedparam` / `pthread_setschedprio` | Yes (run-time change through `stk::SetWeight()`, needs `SwitchStrategyFixedPriority`) |
| `pthread_attr_set/getprivileged_np` | Yes (non-portable STK extension) |
| `pthread_set/getname_np`, `pthread_attr_set/getname_np`, `pthread_getattr_np` | Yes (GNU-style extensions) |
| `pthread_cancel`, `setcancelstate`, `setcanceltype`, `testcancel`, `cleanup_push/pop` | Yes (deferred cancellation only) |
| `pthread_kill(t, 0 / SIGKILL / SIGTERM)` | Yes (existence check and thread termination, see [Terminating a Thread](#terminating-a-thread)) |
| `pthread_atfork`, `pthread_sigmask` | Accepted, no effect (no `fork()`, no signals) |
| `sched_getscheduler` / `setscheduler` / `getparam` / `setparam` / `sched_rr_get_interval` | Yes (the "process" is the calling thread) |
| `pthread_mutex_t` (normal / default) | Yes: recursive in practice, see [Mutex](#mutex) |
| `pthread_mutex_t` (recursive, errorcheck) | Yes |
| `pthread_mutexattr_set/getprotocol`, `setpshared/getpshared` | Yes (`PRIO_NONE` / `PRIO_INHERIT`, `PROCESS_PRIVATE` only) |
| `pthread_cond_t` (`wait` / `timedwait` / `clockwait` / `signal` / `broadcast`) | Yes |
| `pthread_condattr_set/getclock`, `setpshared/getpshared` | Yes |
| `pthread_rwlock_t` (read/write, try, timed, clock) | Yes |
| `pthread_spin_*` | Yes (`PTHREAD_PROCESS_PRIVATE` only) |
| `pthread_barrier_t` | Yes |
| `pthread_once` | Yes |
| `pthread_key_t` (create / delete / set / get, destructors) | Yes |
| `pthread_set/getconcurrency` | Yes (hint, ignored) |
| `sem_*` (`<semaphore.h>`), unnamed semaphores | Yes |
| `mq_*` (`<mqueue.h>`), named queues with priorities | Yes (`mq_notify` returns `ENOSYS`) |
| `clock_gettime` / `clock_getres` / `timespec_get` | Yes |
| `nanosleep` / `clock_nanosleep` / `usleep` / `sleep` | Yes |
| `gettimeofday` / `settimeofday` / `time` | Yes (kernel time + optional offset) |
| `timer_create` / `settime` / `gettime` / `delete` / `getoverrun` | Yes (`SIGEV_THREAD` / `SIGEV_NONE`, on a `TimerHost` you provide) |
| `sysconf` / `getpid` / `getppid` / `gettid` / `getpagesize` / `getuid` / `geteuid` / `getgid` / `getegid` | Yes (limits and constants) |
| C11 `thrd_*`, `mtx_*`, `cnd_*`, `tss_*`, `call_once` | Yes (`<threads.h>`) |
| `stk_pthread_bind_kernel()`, `stk_posix_timer_bind_host()`, `stk_posix_set_wall_time()` | Yes: STK-specific, not POSIX |

---

## What Is Not Supported

| Area | Why |
|---|---|
| Signals: `sigaction`, `kill`, `raise`, `sigwait`, `alarm`, `setitimer`, `pthread_kill` with any signal other than `0` / `SIGKILL` / `SIGTERM` (`ENOSYS`), `SIGEV_SIGNAL`, `mq_notify` | STK has no signals (`pthread_sigmask` is a no-op) |
| Processes: `fork`, `exec*`, `wait*`, `posix_spawn`, `pthread_atfork`, process-shared objects | One firmware image, no processes |
| Named semaphores: `sem_open`, `sem_close`, `sem_unlink` | STK has no named kernel objects (use [message queues](#message-queues) for named objects) |
| Asynchronous cancellation | A running task cannot be stopped at an arbitrary instruction (`PTHREAD_CANCEL_ASYNCHRONOUS` returns `ENOTSUP`) |
| `pthread_setaffinity_np`, `pthread_getcpuclockid` | A kernel instance is bound to one core; no per-thread CPU clocks |
| Robust mutexes, priority ceiling (`pthread_mutex_setprioceiling`) | No owner-death notification; only priority inheritance exists |
| `pthread_rwlockattr_setkind_np` | `RWMutex` has a fixed writer-preference policy |
| `SCHED_FIFO` / `SCHED_RR` enforcement | The kernel switch strategy defines scheduling; the policy value is only remembered |
| Calendar time without help | No RTC: `time()` / `gettimeofday()` need `stk_posix_set_wall_time()` |
| File descriptors: `poll`, `select`, `epoll`, `pipe()`, sockets, `mmap`, `aio_*` | No descriptor layer |

---

## Configuration

All compile-time limits are set with `#define` before including any STK header, or more typically in
your project's `stk_config.h`. All of them have defaults, see the
[Configuration Reference](#configuration-reference) for the complete list.

| Macro | Default | Meaning |
|---|---|---|
| `STK_C_PTHREAD_MAX_THREADS` | `8` | Max concurrently-alive `pthread_t`'s (created threads **plus** adopted tasks) |
| `STK_C_PTHREAD_DEFAULT_STACK_WORDS` | `1024` | Default per-thread stack size, in machine words |
| `STK_C_PTHREAD_REAPER_STACK_WORDS` | `256` | Stack words for the internal reaper task |
| `STK_C_PTHREAD_KEYS_MAX` | `8` | Max concurrently-alive `pthread_key_t`'s (also `PTHREAD_KEYS_MAX`) |
| `STK_C_PTHREAD_NAME_MAX` | `16` | Thread name capacity including the NUL |
| `STK_C_PTHREAD_CLEANUP_MAX` | `8` | Cleanup-handler nesting depth per thread |
| `STK_C_SCHED_PRIORITY_MAX` | `31` | Highest priority level (`MAX_PRIORITIES - 1` of your `SwitchStrategyFixedPriority`) |
| `STK_C_PTHREAD_RUNTIME_PRIORITY` | `1` | Allow priority changes of running threads (needs `SwitchStrategyFixedPriority`) |
| `STK_C_MQ_MAX_QUEUES` / `STK_C_MQ_MAX_DESCRIPTORS` | `4` / `8` | Message queue limits |
| `STK_C_TIMERS_MAX` | `8` | POSIX timers alive at the same time |

The kernel itself must have room for the tasks of the layer:

| Limit | Why it matters here |
|---|---|
| Kernel task count (`Kernel<..., N, ...>` template argument, or `STK_C_KERNEL_MAX_TASKS` if you configure the kernel through the C interface) | Must hold your own tasks + the reaper task + all live pthreads. See [Size the Task Table](#3-size-the-task-table) |

The default-size stack pool is a static C++ `memory::BlockMemoryPool` owned by the layer: it does not
consume a slot of any C-interface pool limit (`STK_C_BLOCKPOOL_MAX` is no longer involved).

---

## Quick Start

Two threads incrementing a shared counter under a mutex, started from a bootstrap task. The kernel is
set up with the STK C++ API:

```cpp
#include <stk.h>            // C++ API
#include <sync/stk_sync.h>
#include <pthread.h>        // the layer's header (not the toolchain's)

using namespace stk;

#define MAIN_STACK_WORDS 512

// 1 bootstrap task + 1 reaper + up to 4 worker threads (+ spare)
typedef Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, 8,
               SwitchStrategyFP32, PlatformDefault> KernelType;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_counter = 0;

static void *worker(void *arg) {
    for (int i = 0; i < 100; ++i) {
        pthread_mutex_lock(&g_lock);
        ++g_counter;
        pthread_mutex_unlock(&g_lock);
    }
    return arg;
}

class MainTask : public Task<MAIN_STACK_WORDS, ACCESS_USER> {
    void Run() override {
        pthread_t t[2];
        for (int i = 0; i < 2; ++i) { pthread_create(&t[i], NULL, worker, NULL); }
        for (int i = 0; i < 2; ++i) { pthread_join(t[i], NULL); }
        // g_counter == 200 here
        for (;;) { Sleep(1000); }
    }
};

int main() {
    static KernelType kernel;
    static MainTask   main_task;

    kernel.Initialize();
    stk_pthread_bind_kernel(static_cast<IKernel *>(&kernel)); // once, before pthread_create()
    kernel.AddTask(&main_task);
    kernel.Start();                                            // never returns
}
```

If your application uses the STK C interface instead (`stk_kernel_create()` and friends), pass the
`stk_kernel_t *` it returns:

```c
#include <stk_c.h>          /* for stk_kernel_*, stk_word_t, stk_sleep_ms ... */
#include <pthread.h>

stk_kernel_t *k = stk_kernel_create(0);               /* core 0 */
stk_kernel_init(k, STK_PERIODICITY_DEFAULT);          /* 1 ms tick */
stk_pthread_bind_kernel(k);                           /* once, before pthread_create() */
/* ... add the bootstrap task, then: */
stk_kernel_start(k);
```

`stk_pthread_bind_kernel()` takes a `void *` that must be the `stk::IKernel *` of the kernel (this is
exactly what `stk_kernel_create()` returns). From C++ pass `static_cast<stk::IKernel *>(&kernel)`,
**not** a pointer to a derived type that has been converted to `void *`.

---

## Example Project

The repository contains a dedicated, buildable example in
[`build/example/blinky_pthread`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky_pthread).
It shows the layer in a complete project, including kernel setup, `stk_pthread_bind_kernel()` and
thread creation, and is a good reference for project-level configuration.

---

## Step-by-Step Setup

The layer never creates or starts a kernel itself. You set up and start an STK kernel exactly as you
would without it, then bind it once with `stk_pthread_bind_kernel()`.

### 1. Add the Include Path and Sources

Add `interop/posix/include` to the compiler include path **before** the toolchain's own directories,
and compile `stk_c_pthread.cpp` (and `stk_c_threads.cpp` for C11 threads). Keeping the layer's
directory first is what lets `time.h`, `sys/time.h` and `unistd.h` extend the toolchain headers and
lets `pthread.h`, `semaphore.h`, `sched.h`, `mqueue.h` and `threads.h` replace missing or conflicting
ones. See [Toolchain Integration Notes](#toolchain-integration-notes).

### 2. Configure the Kernel Type

The kernel type of the bound core must include **both** `KERNEL_DYNAMIC` and `KERNEL_SYNC`:

```cpp
typedef Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, 8,
               SwitchStrategyFP32, PlatformDefault> KernelType;
```

(With the C interface define `STK_C_KERNEL_TYPE_CPU_0` to the same `Kernel<...>` type.)

| Requirement | Why |
|---|---|
| `KERNEL_DYNAMIC` | Threads finish by returning from their entry function or calling `pthread_exit()`; STK supports that only for dynamic kernels |
| `KERNEL_SYNC` | Enables the Mutex / Event / Semaphore / ConditionVariable primitives the layer is built on |
| Not `KERNEL_HRT` | `pthread_create()` has no way to supply a period and deadline; sleeping functions are unavailable in HRT |

Use `SwitchStrategyFixedPriority` (for example `SwitchStrategyFP32`) if you want POSIX thread
priorities, including changing them at run time: they are the fixed priority levels of that strategy.
Other non-HRT strategies (`SwitchStrategyRR`, `SwitchStrategySWRR`) work for everything else; with them
set `STK_C_PTHREAD_RUNTIME_PRIORITY` to `0`. The layer cannot verify the kernel type at run time: a
mismatch is a build-configuration error and is **not** reported through an error code.

### 3. Size the Task Table

The kernel task count must be large enough for **all** of:

```
your own pre-existing tasks (including the bootstrap task)
+ 1 internal reaper task
+ the maximum number of concurrently-alive pthreads
+ 0 (timers run on the TimerHost you provide, accounted by you)
```

A pthread stays "alive" from `pthread_create()` until it is reclaimed: by `pthread_join()`, or by the
reaper for detached threads once they finish. A finished-but-not-yet-joined thread still counts.

```cpp
/* 1 bootstrap task + 1 reaper + up to 4 worker threads */
typedef Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, 6, SwitchStrategyFP32, PlatformDefault> KernelType;

/* 4 workers + 1 adopted bootstrap task (see "Thread Identity and Adoption") */
#define STK_C_PTHREAD_MAX_THREADS   5
```

> The default `STK_C_PTHREAD_MAX_THREADS` is `8`: make sure the kernel task count (or
> `STK_C_KERNEL_MAX_TASKS` when configuring through the C interface, default `4`) matches it.

### 4. Create, Bind, and Start the Kernel

```cpp
kernel.Initialize();                                       // tick = 1000 µs by default
stk_pthread_bind_kernel(static_cast<IKernel *>(&kernel));  // once, before pthread_create()

/* ... add your bootstrap task(s) ... */
kernel.Start();
```

`stk_pthread_bind_kernel()`:

- must be called **exactly once**, before the first `pthread_create()`;
- needs the kernel to be created and initialized, but not yet started;
- also adds the internal **reaper task** to the kernel (always, whether or not you ever detach a
  thread), so the task table must have room for it;
- does not support binding a second, different kernel (the reaper stays with the first).

POSIX timers additionally need a timer host, see [Timers](#timers).

### 5. Create Threads from the Bootstrap Task

Once the kernel is running, any task may call `pthread_create()`. Typically the application's
bootstrap task (the one added before `Start()`) creates the worker threads, as in the
[Quick Start](#quick-start).

---

## Threads

### Creating and Joining

```c
static void *worker(void *arg) {
    int id = *(int *)arg;
    /* ... */
    return (void *)(intptr_t)id;          /* delivered to pthread_join() */
}

pthread_t t;
static int id = 7;

int rc = pthread_create(&t, NULL, worker, &id);
if (rc != 0) {
    /* EINVAL: no kernel bound, or the kernel rejected the task (not retryable)
       EAGAIN: no free thread slot / stack allocation failed / kernel task table full (retryable) */
}

void *result;
rc = pthread_join(t, &result);            /* blocks until worker returns */
```

`pthread_join()` blocks until the thread finishes, then reclaims its resources (stack and slot). It
returns `EINVAL` if the thread is `NULL`, already joined, detached or adopted, and `EDEADLK` if a
thread tries to join itself. `pthread_join()` is a [cancellation point](#cancellation).

Non-blocking and timed joins (GNU-style):

```c
rc = pthread_tryjoin_np(t, &result);                 /* 0, or EBUSY if still running */
rc = pthread_timedjoin_np(t, &result, &abstime);     /* 0, or ETIMEDOUT (thread stays joinable) */
```

> Every joinable thread must be joined (or detached) eventually. An un-joined thread keeps its
> `STK_C_PTHREAD_MAX_THREADS` slot and its task-table entry.

### Thread Attributes

```c
pthread_attr_t attr;
pthread_attr_init(&attr);                 /* default stack, joinable, user mode */

pthread_attr_setstacksize(&attr, 2048 * sizeof(pthread_stk_word_t));   /* BYTES, not words */
pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
pthread_attr_setprivileged_np(&attr, PTHREAD_CREATE_PRIVILEGED_NP);

pthread_create(&t, &attr, worker, NULL);
pthread_attr_destroy(&attr);              /* no-op; no owned resources */
```

| Function | Effect |
|---|---|
| `pthread_attr_init()` | Defaults: `STK_C_PTHREAD_DEFAULT_STACK_WORDS` stack, no external stack, joinable, user mode, normal priority, name `"pthread"` |
| `pthread_attr_setstacksize()` / `getstacksize()` | Stack size in **bytes**. `getstacksize()` reports the default size if none was set |
| `pthread_attr_setstack()` / `getstack()` | Use a caller-supplied stack buffer (must be word-aligned; `EINVAL` otherwise) |
| `pthread_attr_setdetachstate()` / `getdetachstate()` | `PTHREAD_CREATE_JOINABLE` (default) or `PTHREAD_CREATE_DETACHED` |
| `pthread_attr_setprivileged_np()` / `getprivileged_np()` | **Non-portable.** `PTHREAD_CREATE_USER_NP` (default) or `PTHREAD_CREATE_PRIVILEGED_NP` (e.g. for direct peripheral access) |
| `pthread_attr_setschedparam()` / `setschedpolicy()` / `setinheritsched()` / `setscope()` | Priority and scheduling, see [Scheduling and Priorities](#scheduling-and-priorities) |
| `pthread_attr_setguardsize()` / `getguardsize()` | Accepted and stored; STK has no guard pages, so it has no effect |
| `pthread_attr_setname_np()` / `getname_np()` | Thread name, see [Thread Names](#thread-names) |

`pthread_getattr_np(thread, &attr)` fills an attribute object from a **running** thread (stack, detach
state, priority, policy, name). Do not create a new thread from such an object: it carries the running
thread's stack address.

An external stack:

```c
static __stk_c_stack pthread_stk_word_t g_worker_stack[512];   /* alignment macro from stk_c.h, or your own */

pthread_attr_setstack(&attr, g_worker_stack, sizeof(g_worker_stack));
```

The buffer is never freed by the layer; it must stay valid for the whole life of the thread (until it
is joined or reaped).

### Detached Threads

A detached thread cannot be joined. Its resources are reclaimed automatically by the internal reaper
task once it finishes.

```c
pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);   /* at creation */
pthread_create(&t, &attr, worker, NULL);

/* or later: */
pthread_detach(t);                        /* EINVAL if already joined or detached */
```

### Exiting a Thread

A thread ends by returning from its entry function or by calling `pthread_exit()`. Both deliver the
value to a joiner and run the thread's key destructors; `pthread_exit()` (and a cancellation) also run
the pushed [cleanup handlers](#cancellation) first.

```c
static void *worker(void *arg) {
    if (fatal_error) {
        pthread_exit((void *)-1);         /* does not return */
    }
    return NULL;
}
```

### Thread Identity and Adoption

`pthread_self()` is backed by STK's per-task TLS slot, which the layer fills in for every thread it
creates. A task the layer did **not** create, typically the bootstrap task added before `Start()`, is
**adopted** on first use: the first call to `pthread_self()`, `pthread_setspecific()`,
`pthread_setcancelstate()` or `pthread_cleanup_push()` from it gives it a `pthread_t`.

```c
pthread_t me = pthread_self();            /* adopts the bootstrap task on first call */
```

Consequences of adoption:

- each adopted task permanently consumes one `STK_C_PTHREAD_MAX_THREADS` slot, so size that limit as
  *(created threads + adopted tasks)*;
- an adopted thread is not joinable and never reaped: `pthread_join()` and `pthread_detach()` return
  `EINVAL` for it;
- the task's TLS slot now belongs to the layer: do **not** call the STK TLS API (`stk_tls_set()` /
  `hw::SetTlsPtr()`) in a task that uses the pthread API;
- `pthread_exit()` from an adopted task runs its cleanup handlers and key destructors, frees its slot and ends
  the task (`stk::ExitTask()`);
- `pthread_getschedparam()` of an adopted task reports the default priority until the priority is set through the
  layer (its real weight is unknown to it); `pthread_setschedparam()` works for it;
- `pthread_getspecific()` never adopts, so merely reading does not use a slot.

`pthread_self()` returns `NULL` only if no slot is left to adopt into. `pthread_equal(a, b)` compares
two handles.

### Thread Names

```c
pthread_setname_np(t, "sensor");          /* ERANGE if longer than STK_C_PTHREAD_NAME_MAX - 1 */
char buf[STK_C_PTHREAD_NAME_MAX];
pthread_getname_np(t, buf, sizeof(buf));  /* ERANGE if the buffer is too small */

pthread_attr_setname_np(&attr, "sensor"); /* name for threads created with this attribute */
```

The name is the kernel's trace name of the task (shown by trace/debug tools). The default is
`"pthread"`.

### Stacks and Memory

| Stack source | Heap use | Freed when |
|---|---|---|
| Default size (`STK_C_PTHREAD_DEFAULT_STACK_WORDS`, no attr or default attr) | None: served from a static pool of `STK_C_PTHREAD_MAX_THREADS` blocks | Thread is reclaimed (block returned to the pool) |
| Any other size via `pthread_attr_setstacksize()` | `malloc()` | Thread is reclaimed (`free()`) |
| Caller buffer via `pthread_attr_setstack()` | None | Never: caller owns it |

The internal reaper task uses a static stack of `STK_C_PTHREAD_REAPER_STACK_WORDS` words. Message
queues allocate their message storage with `malloc()` when a queue is created. If you want a fully
heap-free system, use only the default stack size or supply your own stacks, and avoid message queues.

---

## Scheduling and Priorities

POSIX thread priorities are the **fixed priority levels** of `SwitchStrategyFixedPriority`:

- `sched_priority` is the task weight: `0` is the lowest level and `MAX_PRIORITIES - 1` the highest
  (`SwitchStrategyFP32`: `0..31`). Set `STK_C_SCHED_PRIORITY_MAX` to match your strategy.
- The default level is `PRIORITY_NORMAL` = `MAX_PRIORITIES / 2` (`STK_C_SCHED_PRIORITY_DEFAULT`, 16 for
  FP32). Threads created without attributes, and the internal reaper, run at that level.
- Threads of equal priority are scheduled round-robin by the strategy; higher priorities preempt lower.
- Mutexes use priority inheritance automatically when the strategy supports it, which is also what
  `PTHREAD_PRIO_INHERIT` selects (`pthread_mutexattr_setprotocol()`); `PTHREAD_PRIO_PROTECT` returns
  `ENOTSUP`.

```c
pthread_attr_t attr;
struct sched_param sp = { .sched_priority = 24 };

pthread_attr_init(&attr);
pthread_attr_setschedparam(&attr, &sp);                    /* explicit priority 24 */
/* or: pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);  copy the creator's priority */
pthread_create(&t, &attr, worker, NULL);

/* later, from any task: */
sp.sched_priority = 28;
pthread_setschedparam(t, SCHED_FIFO, &sp);                 /* applied immediately via stk::SetWeight() */
pthread_setschedprio(t, 10);
```

Notes:

- The priority of a running thread is changed with `stk::SetWeight()`. This requires a strategy with
  `WEIGHT_API` and `PRIORITY_INHERITANCE_API` (`SwitchStrategyFixedPriority`); with another strategy set
  `STK_C_PTHREAD_RUNTIME_PRIORITY` to `0`, a change then returns `ENOTSUP` (an unchanged value still
  succeeds).
- If a thread currently inherits a higher priority through a mutex, the new base priority takes effect
  once the inheritance ends.
- `SCHED_OTHER`, `SCHED_FIFO` and `SCHED_RR` are accepted and remembered (`pthread_getschedparam()`
  returns them) but do not change scheduling: the switch strategy defines it.
- `pthread_setschedparam()` / `pthread_setschedprio()` return `ESRCH` for a thread that has finished and
  `EINVAL` for a priority outside `sched_get_priority_min()` .. `sched_get_priority_max()`.
- `sched_yield()` (and `pthread_yield()`) yields the CPU to another ready task.
- The process-level calls act on the **calling thread** (`pid` must be `0` or the value of `getpid()`, otherwise `ESRCH`):
  `sched_getscheduler()`, `sched_setscheduler()` (returns the previous policy), `sched_getparam()`,
  `sched_setparam()`. `sched_rr_get_interval()` returns the time slice of the kernel: one tick
  (`stk::GetTickResolution()`).

---

## Cancellation

Cancellation is **cooperative (deferred)**: `pthread_cancel()` records a request and wakes the target if
it is blocked; the target terminates as if it called `pthread_exit(PTHREAD_CANCELED)` the next time it
reaches a cancellation point with cancellation enabled. Pushed cleanup handlers and key destructors run,
and `pthread_join()` returns `PTHREAD_CANCELED`.

```c
static void unlock_it(void *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }

static void *worker(void *arg) {
    for (;;) {
        pthread_mutex_lock(&g_lock);
        pthread_cleanup_push(unlock_it, &g_lock);     /* runs if cancelled inside the wait */
        while (!g_ready) {
            pthread_cond_wait(&g_cond, &g_lock);      /* cancellation point */
        }
        pthread_cleanup_pop(1);                        /* run the handler: unlock */
    }
}

pthread_cancel(t);
pthread_join(t, &result);                              /* result == PTHREAD_CANCELED */
```

Cancellation points: `pthread_testcancel()`, `pthread_cond_wait` / `timedwait` / `clockwait`,
`pthread_join` / `timedjoin_np`, `sem_wait` / `timedwait` / `clockwait`, `mq_send` / `receive` and their
timed forms, `nanosleep`, `clock_nanosleep`, `usleep`, `sleep`, `thrd_sleep`, `thrd_join`. Mutex, read-write
lock, spinlock and barrier waits are **not** cancellation points (as in POSIX).

| Function | Notes |
|---|---|
| `pthread_cancel(t)` | `0`, or `ESRCH` if the thread has already finished |
| `pthread_setcancelstate()` | `PTHREAD_CANCEL_ENABLE` / `DISABLE`; a pending request stays pending while disabled |
| `pthread_setcanceltype()` | Only `PTHREAD_CANCEL_DEFERRED`; `PTHREAD_CANCEL_ASYNCHRONOUS` returns `ENOTSUP` |
| `pthread_testcancel()` | Explicit cancellation point |
| `pthread_cleanup_push()` / `pop()` | Function form of the POSIX macros: LIFO, at most `STK_C_PTHREAD_CLEANUP_MAX` nested, calls must be balanced (no lexical pairing needed) |

Limits: a thread that never reaches a cancellation point cannot be cancelled. A cancelled
`pthread_cond_wait()` re-acquires the mutex before the handlers run (as in POSIX), so the handler is
expected to unlock it.

### Terminating a Thread

`pthread_kill()` understands three values; there are no other signals:

| `sig` | Effect |
|---|---|
| `0` | Existence check: `0` if the thread is alive, `ESRCH` if it has finished |
| `SIGKILL` | Terminate immediately. The caller ends itself with `stk::ExitTask()`; another thread is removed with `IKernel::ScheduleTaskRemoval()`. **No** cleanup handler and **no** key destructor runs |
| `SIGTERM` | The caller: same as `pthread_exit(PTHREAD_CANCELED)` (handlers and destructors run). Another thread: same as `SIGKILL`, a running task cannot be made to execute code |

`pthread_join()` returns `PTHREAD_CANCELED` for a terminated thread, and detached threads are reclaimed as usual.
Any other signal number returns `ENOSYS`, a negative one `EINVAL`; an adopted task other than the caller cannot be
removed (`ENOTSUP`).

> Terminating another thread cannot release what it holds: a mutex it locked stays locked and memory it allocated
> is not freed. Prefer [cancellation](#cancellation) for a cooperative stop, and use `pthread_kill()` for threads
> which must be stopped unconditionally.

---

## Mutex

Normal (default), recursive and error-checking mutexes are supported.

```c
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_lock(&g_lock);               /* blocks until available */
/* ... protected region ... */
pthread_mutex_unlock(&g_lock);

if (pthread_mutex_trylock(&g_lock) == 0) { /* EBUSY if currently locked */
    pthread_mutex_unlock(&g_lock);
}
```

Dynamic initialization and destruction:

```c
pthread_mutex_t m;
pthread_mutex_init(&m, NULL);
/* ... */
pthread_mutex_destroy(&m);                 /* must be unlocked, no waiters */
```

A mutex initialized with `PTHREAD_MUTEX_INITIALIZER` has its underlying STK mutex created lazily on
first `lock` / `trylock` / `timedlock`. `pthread_mutex_unlock()` never creates it, so unlocking a
never-used static mutex returns `EINVAL`.

Mutex types:

```c
pthread_mutexattr_t ma;
pthread_mutexattr_init(&ma);
pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);   /* or _ERRORCHECK / _NORMAL */
pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);  /* accepted; inheritance is built in */

pthread_mutex_t m;
pthread_mutex_init(&m, &ma);
pthread_mutexattr_destroy(&ma);
```

The underlying `stk::sync::Mutex` is recursive, so the types behave as follows:

| Type | Relock by the owner | Unlock by a non-owner |
|---|---|---|
| `PTHREAD_MUTEX_NORMAL` / `PTHREAD_MUTEX_DEFAULT` | Succeeds (recursive in practice) | `EPERM` |
| `PTHREAD_MUTEX_RECURSIVE` | Succeeds; must be unlocked the same number of times | `EPERM` |
| `PTHREAD_MUTEX_ERRORCHECK` | `EDEADLK` (`trylock`: `EBUSY`) | `EPERM` |

Notes:

- A `NORMAL` mutex does **not** deadlock when relocked by its owner. POSIX leaves that case undefined, so
  portable code should not rely on it either way. If you want recursion, request
  `PTHREAD_MUTEX_RECURSIVE` explicitly.
- The maximum nesting depth is `0xFFFE` (`stk::sync::Mutex::RECURSION_MAX`); one more `lock` / `trylock` /
  `timedlock` returns `EAGAIN`.
- `pthread_mutexattr_settype()` and `pthread_mutex_init()` return `EINVAL` for a type other than the three
  above. `pthread_mutexattr_setpshared()` accepts only `PTHREAD_PROCESS_PRIVATE` (`ENOTSUP` otherwise).
- A mutex from `PTHREAD_MUTEX_INITIALIZER` is of type `NORMAL`.
- Do not pass a mutex that is locked more than once to `pthread_cond_wait()` / `pthread_cond_timedwait()`
  (undefined, as in POSIX): only one level is released, so other threads still cannot acquire it. For
  `ERRORCHECK` mutexes, waiting without owning the mutex returns `EPERM`.

For a deadline-based lock see [Timed Waits and abstime](#timed-waits-and-abstime).

---

## Condition Variable

Always paired with a locked mutex that protects the guarded state. Re-check the predicate in a loop:
spurious wakeups are possible, as in POSIX.

```c
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ready = PTHREAD_COND_INITIALIZER;
static int g_available = 0;

/* Consumer: */
pthread_mutex_lock(&g_lock);
while (g_available == 0) {
    pthread_cond_wait(&g_ready, &g_lock);   /* unlocks while waiting, re-locks before return */
}
--g_available;
pthread_mutex_unlock(&g_lock);

/* Producer: */
pthread_mutex_lock(&g_lock);
++g_available;
pthread_cond_signal(&g_ready);              /* wake one waiter */
pthread_mutex_unlock(&g_lock);
/* pthread_cond_broadcast(&g_ready) wakes all waiters */
```

Signals are not queued: one sent while nobody waits is lost, which is why the predicate is tested under
the mutex. The mutex passed to `pthread_cond_wait()` must be locked by the caller (and therefore already
initialized/used), otherwise the call returns `EINVAL`.

`pthread_cond_init()` / `pthread_cond_destroy()` work as for mutexes; no thread may be waiting when a
condition variable is destroyed. `pthread_condattr_setclock()` accepts `CLOCK_REALTIME` and
`CLOCK_MONOTONIC`; both select the same kernel timeline, so it makes no difference to the wait.

---

## Timed Waits and abstime

`pthread_mutex_timedlock()`, `pthread_cond_timedwait()`, `pthread_rwlock_timedrdlock()`,
`pthread_rwlock_timedwrlock()`, `pthread_timedjoin_np()`, `sem_timedwait()`, `mq_timedsend()` /
`mq_timedreceive()` and the `*_clock*` variants take an **absolute** deadline, as in POSIX. STK has no
wall clock: it has a single monotonic **kernel timeline** (the tick count since the kernel started), and
both `CLOCK_REALTIME` and `CLOCK_MONOTONIC` report it.

So derive `abstime` with `clock_gettime()` (either clock) plus a duration. Never derive it from
`time()` / `gettimeofday()` once you have called `stk_posix_set_wall_time()`: those include a wall-time
offset, see [Clocks, Sleeping and Wall Time](#clocks-sleeping-and-wall-time).

```c
#include <errno.h>
#include <time.h>

static void abstime_after_ms(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

struct timespec abstime;
abstime_after_ms(&abstime, 500);            /* 500 ms from now */

pthread_mutex_lock(&g_lock);
while (g_available == 0) {
    int rc = pthread_cond_clockwait(&g_ready, &g_lock, CLOCK_MONOTONIC, &abstime);
    if (rc == ETIMEDOUT) {
        break;                              /* mutex is still re-locked here */
    }
}
pthread_mutex_unlock(&g_lock);
```

Behaviour of the conversion:

- the remaining time is rounded **up** to whole kernel ticks, so a call never times out before `abstime`;
- a deadline **already in the past** gives a zero timeout, so the call behaves like a non-blocking attempt
  (an available lock or token is still taken);
- very distant deadlines are clamped just below the kernel's "infinite" value, so they stay timed waits;
- an invalid `tv_nsec` returns `EINVAL` from the `*_clock*` variants, `pthread_timedjoin_np()`,
  `sem_timedwait()` and the timed message queue functions;
- on timeout the functions return `ETIMEDOUT`; `pthread_cond_timedwait()` re-locks the mutex before
  returning in every case.

---

## Reader-Writer Lock

Multiple readers can hold the lock concurrently; a writer is exclusive. The underlying implementation uses
a **writer-preference** policy, and there is no setting to change it.

```c
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;

/* Reader: */
pthread_rwlock_rdlock(&g_rw);
/* ... read shared data ... */
pthread_rwlock_unlock(&g_rw);

/* Writer: */
pthread_rwlock_wrlock(&g_rw);
/* ... modify shared data ... */
pthread_rwlock_unlock(&g_rw);
```

Non-blocking and timed variants:

```c
if (pthread_rwlock_tryrdlock(&g_rw) == 0) { /* EBUSY if a writer is active or waiting */
    pthread_rwlock_unlock(&g_rw);
}
if (pthread_rwlock_trywrlock(&g_rw) == 0) { /* EBUSY otherwise */
    pthread_rwlock_unlock(&g_rw);
}

pthread_rwlock_timedrdlock(&g_rw, &abstime);   /* 0 or ETIMEDOUT */
pthread_rwlock_timedwrlock(&g_rw, &abstime);
pthread_rwlock_clockrdlock(&g_rw, CLOCK_MONOTONIC, &abstime);
pthread_rwlock_clockwrlock(&g_rw, CLOCK_MONOTONIC, &abstime);
```

As in POSIX, a single `pthread_rwlock_unlock()` releases either a read or a write hold. The layer records
inside the `pthread_rwlock_t` whether the lock is held for writing (safe because a write hold is
exclusive) and calls the matching STK unlock. Calling it on a lock you do not hold is undefined behaviour
and is not detected.

---

## Spinlock

A busy-waiting lock for very short regions. Not recursive: relocking from the owner deadlocks. Only
`PTHREAD_PROCESS_PRIVATE` is accepted (`PTHREAD_PROCESS_SHARED` returns `ENOTSUP`; STK has no
cross-process concept).

```c
pthread_spinlock_t sl;
pthread_spin_init(&sl, PTHREAD_PROCESS_PRIVATE);

pthread_spin_lock(&sl);
/* ... very short protected region ... */
pthread_spin_unlock(&sl);

if (pthread_spin_trylock(&sl) == 0) {       /* EBUSY if held */
    pthread_spin_unlock(&sl);
}

pthread_spin_destroy(&sl);
```

Unlike the other objects, spinlocks have **no static initializer** (matching POSIX);
`pthread_spin_init()` must be called first.

---

## Barrier

Blocks a fixed number of threads until all have arrived, then releases them together; the barrier resets
itself for reuse.

```c
static pthread_barrier_t g_bar;

pthread_barrier_init(&g_bar, NULL, 3);      /* 3 participating threads; count must not be 0 */

/* In each of the 3 threads: */
int rc = pthread_barrier_wait(&g_bar);
if (rc == PTHREAD_BARRIER_SERIAL_THREAD) {
    /* exactly one arbitrary thread per round gets this value */
}                                           /* all others get 0 */

pthread_barrier_destroy(&g_bar);            /* no thread may be waiting */
```

Barriers have **no static initializer**: the trip count is supplied to `pthread_barrier_init()`, which also
returns `EINVAL` for a count of `0`. `pthread_barrier_wait()` is not a cancellation point.

---

## One-Time Initialization

```c
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void init_subsystem(void) {
    /* runs exactly once */
}

void use_subsystem(void) {
    pthread_once(&g_once, init_subsystem);
}
```

A thread that calls `pthread_once()` while another thread's init routine is still running **waits** for it
to finish, rather than racing ahead. The routine must not call `pthread_once()` on the same
`pthread_once_t` (self-deadlock, as in POSIX).

---

## Thread-Specific Data (Keys)

Each thread can hold one `void *` value per key.

```c
static pthread_key_t g_key;

static void free_buffer(void *p) {
    /* called with a thread's non-NULL value when that thread finishes */
}

pthread_key_create(&g_key, free_buffer);    /* destructor may be NULL */

/* In any thread: */
pthread_setspecific(g_key, my_buffer);
void *buf = pthread_getspecific(g_key);     /* NULL if never set by this thread */

pthread_key_delete(g_key);                  /* does not run destructors */
```

- at most `STK_C_PTHREAD_KEYS_MAX` keys exist at a time (`EAGAIN` when exhausted);
- destructors run when a thread finishes (return, `pthread_exit()` or cancellation), for at most
  `PTHREAD_DESTRUCTOR_ITERATIONS` passes: a destructor that stores a new non-NULL value for its own key
  causes another pass;
- they do **not** run for an adopted task that never exits;
- `pthread_setspecific()` from an un-adopted foreign task adopts it, and returns `ENOMEM` if no slot is left.

---

## Semaphores

POSIX unnamed counting semaphores (`<semaphore.h>`), backed by `stk::sync::SemaphoreT`. There is no
static initializer (as in POSIX): call `sem_init()`.

```c
#include <semaphore.h>

static sem_t g_items;

sem_init(&g_items, 0, 0);                  /* pshared must be 0, initial count 0 */

/* Producer (task or ISR): */
sem_post(&g_items);

/* Consumer: */
while (sem_wait(&g_items) != 0) {          /* EINTR: wait was cancelled, retry */
    if (errno != EINTR) { break; }
}

struct timespec abstime;
abstime_after_ms(&abstime, 100);
if (sem_timedwait(&g_items, &abstime) != 0 && errno == ETIMEDOUT) { /* no item in 100 ms */ }

int n;
sem_getvalue(&g_items, &n);                /* current count (never negative) */

sem_destroy(&g_items);                     /* no task may be waiting */
```

| Function | Notes |
|---|---|
| `sem_init(sem, pshared, value)` | `ENOSYS` if `pshared != 0`; `EINVAL` if `value > SEM_VALUE_MAX` |
| `sem_wait()` / `sem_timedwait()` / `sem_clockwait()` | Cancellation points; `EINTR` if the wait is cancelled without terminating the thread |
| `sem_trywait()` | `EAGAIN` if the count is zero |
| `sem_post()` | `EOVERFLOW` if the count is already `SEM_VALUE_MAX` and nobody waits |
| `sem_getvalue()` | Count at the time of the call; 0 when tasks are waiting |

`SEM_VALUE_MAX` is `INT32_MAX`. A token posted while a task waits is handed to it directly, so the count is
unchanged. Named semaphores (`sem_open`, ...) are not available.

---

## Message Queues

POSIX named message queues (`<mqueue.h>`), with priorities, built on `stk::sync::Mutex` and
`stk::sync::ConditionVariable`. Messages are delivered **highest priority first, FIFO within a
priority** (`0 .. MQ_PRIO_MAX-1`, `MQ_PRIO_MAX` is 32).

```c
#include <mqueue.h>

struct mq_attr attr = { .mq_flags = 0, .mq_maxmsg = 8, .mq_msgsize = 32 };
mqd_t q = mq_open("/events", O_CREAT | O_RDWR, 0, &attr);   /* (mqd_t)-1 and errno on failure */

mq_send(q, "start", 6, 5);                                   /* priority 5 */
mq_send(q, "stop",  5, 20);                                  /* higher priority: delivered first */

char buf[32];                                                /* must be >= mq_msgsize */
unsigned prio;
ssize_t n = mq_receive(q, buf, sizeof(buf), &prio);          /* "stop", prio 20 */

mq_close(q);
mq_unlink("/events");                                        /* destroyed with its last descriptor */
```

- **Capacity:** `STK_C_MQ_MAX_QUEUES` queues and `STK_C_MQ_MAX_DESCRIPTORS` open descriptors at the same
  time (`ENOSPC` / `EMFILE` when exhausted). Names are up to `STK_C_MQ_NAME_MAX - 1` characters.
- **Memory:** the message storage (`mq_maxmsg * (mq_msgsize + 8)` bytes) is allocated with `malloc()` when
  the queue is created and released when the last descriptor of an unlinked queue is closed. With no
  attributes `STK_C_MQ_DEFAULT_MAXMSG` / `STK_C_MQ_DEFAULT_MSGSIZE` apply.
- **Blocking:** `mq_send()` waits while the queue is full and `mq_receive()` while it is empty, unless the
  descriptor has `O_NONBLOCK` (`EAGAIN`). `mq_timedsend()` / `mq_timedreceive()` take an absolute deadline
  (`ETIMEDOUT`). All four are cancellation points.
- **Attributes:** `mq_getattr()`, and `mq_setattr()` for `O_NONBLOCK` only.
- **Not supported:** `mq_notify()` returns `ENOSYS` (STK has no signals). Poll with `mq_timedreceive()` or
  use a thread that blocks in `mq_receive()`.

---

## Clocks, Sleeping and Wall Time

```c
#include <time.h>

struct timespec ts;
clock_gettime(CLOCK_MONOTONIC, &ts);       /* time since the kernel started, tick resolution */
clock_getres(CLOCK_MONOTONIC, &ts);        /* one kernel tick */

nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 5000000 }, NULL);   /* 5 ms, rounded up to ticks */
usleep(2000);
sleep(1);
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abstime, NULL);          /* sleep until a deadline */
```

- `CLOCK_REALTIME` and `CLOCK_MONOTONIC` are the **same** kernel timeline with the resolution of one tick.
  There is no `clock_settime()`.
- The sleep functions round the requested time **up** to whole ticks, are cancellation points, and can be
  woken early by `pthread_cancel()` or the kernel's sleep-cancel call (`nanosleep()` then returns `-1` with
  `EINTR` and the unslept time in `rem`; `sleep()` returns the unslept seconds). They are task-context only
  and unavailable in HRT mode.
- `timespec_get(ts, TIME_UTC)` (C11) reports the same timeline.

### Wall time: `time()` and `gettimeofday()`

STK has no real-time clock. `time()` and `gettimeofday()` return the kernel time plus an offset which is `0`
until you set the current Unix time:

```c
stk_posix_set_wall_time(1735689600);       /* e.g. after reading an RTC or from network time */
/* or, with microseconds: settimeofday(&tv, NULL); */

struct timeval tv;
gettimeofday(&tv, NULL);                   /* tv_sec = Unix time, microsecond fields */
time_t now = time(NULL);
```

The offset affects **only** `time()` and `gettimeofday()`. `clock_gettime()`, every absolute deadline and
`TIMER_ABSTIME` stay on the kernel timeline, so never build a deadline from `time()` / `gettimeofday()`.
These two functions replace the C library's versions; see [Toolchain Integration Notes](#toolchain-integration-notes).

---

## Timers

POSIX per-process timers (`timer_create()`, `timer_settime()`, `timer_gettime()`, `timer_delete()`,
`timer_getoverrun()`) run on a `stk::time::TimerHost` that **you** create and initialize, because the
application owns the timer task configuration (weights, stacks, queue size). Give it to the layer once:

```cpp
static stk::time::TimerHost g_timer_host;    // see the Timers documentation for its configuration
/* ... g_timer_host.Initialize(kernel, ...) ... */
stk_posix_timer_bind_host(&g_timer_host);    // before the first timer_create()
```

```c
static void on_timer(union sigval v) {       /* runs in a timer handler task: keep it short */
    pthread_cond_signal((pthread_cond_t *)v.sival_ptr);
}

struct sigevent sev = { 0 };                 /* needs <signal.h> */
sev.sigev_notify          = SIGEV_THREAD;
sev.sigev_notify_function = on_timer;
sev.sigev_value.sival_ptr = &g_cond;

timer_t timer;
timer_create(CLOCK_MONOTONIC, &sev, &timer);

struct itimerspec its = {
    .it_value    = { .tv_sec = 0, .tv_nsec = 100000000 },   /* first expiration in 100 ms */
    .it_interval = { .tv_sec = 0, .tv_nsec = 100000000 },   /* then every 100 ms (0 = one-shot) */
};
timer_settime(timer, 0, &its, NULL);         /* an all-zero it_value disarms the timer */
/* ... */
timer_delete(timer);
```

- `SIGEV_THREAD` calls the function from a handler task of the host; `SIGEV_NONE` (also used for a NULL
  `sevp`) creates a timer you poll with `timer_gettime()`. `SIGEV_SIGNAL` returns `ENOTSUP`.
- Times are rounded up to whole ticks; `TIMER_ABSTIME` takes a deadline on the kernel timeline.
- `timer_settime()` / `timer_delete()` wait for the host to execute the command (the host's synchronous
  API), so do not call them from a timer notification of the same host.
- `timer_create()` fails with `EAGAIN` if no host is bound or `STK_C_TIMERS_MAX` timers exist. Overruns are
  not tracked: `timer_getoverrun()` always returns `0`.

---

## System Configuration

```c
#include <unistd.h>

long hz     = sysconf(_SC_CLK_TCK);          /* kernel ticks per second */
long cpus   = sysconf(_SC_NPROCESSORS_ONLN); /* STK_C_SYSCONF_NPROCESSORS, default 1 */
long page   = sysconf(_SC_PAGESIZE);         /* STK_C_SYSCONF_PAGESIZE, default 4096 */
long maxthr = sysconf(_SC_THREAD_THREADS_MAX);
pid_t pid   = getpid();                      /* constant STK_C_POSIX_PID, default 1 */
pid_t tid   = gettid();                      /* id of the calling task */
int   psz   = getpagesize();                 /* STK_C_SYSCONF_PAGESIZE */
```

`getppid()` returns `STK_C_POSIX_PPID` (0) and `getuid()` / `geteuid()` / `getgid()` / `getegid()` return
`STK_C_POSIX_UID` / `STK_C_POSIX_GID` (0): there are no parents or users.

`sysconf()` reports the limits of this layer (threads, keys, stack, semaphore, message queue and timer
limits), the POSIX option version (`200809L`) for supported options and `-1` for unsupported ones such as
`_SC_THREAD_PROCESS_SHARED` (errno unchanged). An unknown name returns `-1` with `errno = EINVAL`. Only the
`_SC_*` names defined by your toolchain's `<unistd.h>` are handled.

---

## C11 Threads

`<threads.h>` (`thrd_*`, `mtx_*`, `cnd_*`, `tss_*`, `call_once`) is a thin wrapper over the pthread layer:

```c
#include <threads.h>

static int worker(void *arg) { return 0; }

thrd_t t;
thrd_create(&t, worker, NULL);
int res;
thrd_join(t, &res);

mtx_t m;
mtx_init(&m, mtx_plain);
mtx_lock(&m);
mtx_unlock(&m);
mtx_destroy(&m);
```

- all mutexes are recursive in practice (`mtx_plain` / `mtx_timed` behave like `mtx_recursive`);
- `thrd_create()` takes its start record from a static table of `STK_C_PTHREAD_MAX_THREADS` entries, so it
  needs no heap, and returns `thrd_nomem` when the table or the thread slots are exhausted;
- `thrd_sleep()` uses `nanosleep()` (cancellation point); `thrd_yield()` uses `sched_yield()`;
- `thread_local` / `_Thread_local` is a compiler feature, not part of this layer (use `tss_*` or
  `pthread_key_*`).

---

## Static Initializers

| Object | Static initializer | Lazily creates underlying STK object |
|---|---|---|
| `pthread_mutex_t` | `PTHREAD_MUTEX_INITIALIZER` | Yes, on first lock |
| `pthread_cond_t` | `PTHREAD_COND_INITIALIZER` | Yes, on first use |
| `pthread_rwlock_t` | `PTHREAD_RWLOCK_INITIALIZER` | Yes, on first use |
| `pthread_once_t` | `PTHREAD_ONCE_INIT` | Yes (guard mutex), on first use |
| `pthread_spinlock_t` | none: call `pthread_spin_init()` | n/a |
| `pthread_barrier_t` | none: call `pthread_barrier_init()` | n/a |
| `sem_t` | none: call `sem_init()` | n/a |

---

## Context Restrictions (ISR)

The **blocking** calls, which include `pthread_join()`, the lock/wait functions of mutexes, condition
variables, read-write locks and semaphores (including the timed variants, which block until the deadline),
`pthread_barrier_wait()`, `pthread_once()`, `mq_send()` / `mq_receive()`, and the sleeping functions, must not
be called from an ISR: the underlying STK primitives are ISR-unsafe for blocking operations.

STK's own non-blocking operations (try-lock, unlock, notify) are ISR-safe, but the layer adds lazy
initialization and bookkeeping around most of them, so treat the **whole POSIX API as task-context only**.
The exceptions are `sem_post()`, `sem_trywait()` and `sem_getvalue()`: they map directly to ISR-safe
semaphore operations (they set `errno` only on failure). Time functions read the kernel tick counter and
are task-context only.

---

## Return Codes and errno

The `pthread_*` functions return `0` or an error number. The `sem_*`, `mq_*`, `clock_*`, `nanosleep`,
`gettimeofday`, `timer_*` and `sysconf` functions follow POSIX: they return `-1` (or `(mqd_t)-1`) and set
`errno`. Whether `errno` is per-thread depends on your C library and its locking hooks (for newlib the
reentrancy structure must be switched per task); check this if several threads use these functions
concurrently.

| Code | Typical cause |
|---|---|
| `EINVAL` | `NULL` argument; invalid or never-initialized object; no kernel bound; kernel rejected the task; invalid attribute value; invalid `abstime`; thread already joined/detached/adopted |
| `EAGAIN` | No free thread slot (`STK_C_PTHREAD_MAX_THREADS`); stack allocation failed; kernel task table full; key slots exhausted; mutex recursion depth exceeded; `sem_trywait()` / `mq_*` non-blocking would block; no timer host bound / no free timer |
| `ENOTSUP` | `PTHREAD_PROCESS_SHARED`, `PTHREAD_PRIO_PROTECT`, `PTHREAD_SCOPE_PROCESS`, `PTHREAD_CANCEL_ASYNCHRONOUS`, `SIGEV_SIGNAL`, a priority change with `STK_C_PTHREAD_RUNTIME_PRIORITY` = 0 |
| `ENOSYS` | `sem_init()` with `pshared != 0`; `mq_notify()`; `pthread_kill()` with a signal other than `0` / `SIGKILL` / `SIGTERM` |
| `ENOMEM` | `pthread_setspecific()`: no slot left to adopt the calling task into; message queue storage allocation failed |
| `ERANGE` | Thread or attribute name too long, or the output buffer too small |
| `ESRCH` | `pthread_cancel()`, `pthread_setschedparam()` or `pthread_getattr_np()` on a thread that is not alive |
| `EBUSY` | A `try*` call could not take the lock (including `trylock` on an `ERRORCHECK` mutex the caller already owns); `pthread_tryjoin_np()` on a running thread |
| `EPERM` | `pthread_mutex_unlock()` by a thread that does not own the mutex; `pthread_cond_wait()` on an `ERRORCHECK` mutex the caller does not own |
| `ETIMEDOUT` | A timed wait's deadline passed first |
| `EDEADLK` | `pthread_join()` on the calling thread itself; relocking an `ERRORCHECK` mutex from its owner |
| `EINTR` | `sem_wait()` / `mq_*` / `nanosleep()` ended early by a cancellation request which did not terminate the thread, or by a sleep cancel |
| `EOVERFLOW` | `sem_post()` on a semaphore at `SEM_VALUE_MAX` |
| `EMSGSIZE` | `mq_send()` message too long, or `mq_receive()` buffer smaller than `mq_msgsize` |
| `ENOENT` / `EEXIST` / `ENOSPC` / `EMFILE` / `EBADF` | `mq_open()` / `mq_unlink()` / `mq_*` as in POSIX |

`pthread_getspecific()` and `pthread_self()` return `NULL` instead of an error code.

---

## How It Maps to STK

| POSIX object | Underlying STK object |
|---|---|
| `pthread_t` | A task in the bound `KERNEL_DYNAMIC` kernel (plus a control block from the pthread slot pool) |
| `pthread_mutex_t` | `stk::sync::Mutex` |
| `pthread_cond_t` | `stk::sync::ConditionVariable` |
| `pthread_rwlock_t` | `stk::sync::RWMutex` |
| `pthread_spinlock_t` | `stk::hw::SpinLock` |
| `pthread_barrier_t` | `stk::sync::Barrier` |
| `pthread_once_t` | A guard `stk::sync::Mutex` |
| `sem_t` | `stk::sync::SemaphoreT<uint32_t>` |
| `mqd_t` / message queue | A descriptor table + a priority-ordered ring protected by a `stk::sync::Mutex` and two `ConditionVariable`s |
| `pthread_join()` / `pthread_timedjoin_np()` wait | A manual-reset `stk::sync::Event` per thread, signaled from the task's `OnExit()` |
| `pthread_cancel()` | A pending flag + the kernel's sleep/wait cancel (`stk::SleepCancel()`) |
| `pthread_setschedparam()` | `stk::SetWeight()` (task weight = fixed priority level) |
| `pthread_self()` / keys | The per-task TLS slot |
| `nanosleep()` / `sleep()` | `stk::Sleep()` |
| `clock_gettime()` / `gettimeofday()` | `stk::GetTicks()` + `stk::GetTickResolution()` |
| `timer_t` | A `stk::time::Timer` served by the bound `stk::time::TimerHost` |
| Default-size thread stacks | A static `stk::memory::BlockMemoryPool` |
| Internal reaper | A static `ITask` which reclaims finished detached threads |

The opaque `__mem` blobs embedded in the POSIX types are used as in-place storage for these C++ objects (their
size is verified at compile time), so no heap allocation is needed for the synchronization primitives.

---

## Toolchain Integration Notes

**Include path order.** Put `interop/posix/include` before the toolchain's include directories. `time.h`,
`sys/time.h` and `unistd.h` of the layer include the toolchain's own header first (`#include_next` on
GCC/Clang; on other compilers define `STK_C_SYSTEM_TIME_H`, `STK_C_SYSTEM_SYS_TIME_H`,
`STK_C_SYSTEM_UNISTD_H` and `STK_C_SYSTEM_SIGNAL_H` to the toolchain header path) and then add what is
missing.

**newlib (arm-none-eabi-gcc).** Detected automatically:

- the layer does not redefine `timer_t` and `struct itimerspec`, which newlib already provides;
- `union sigval` / `struct sigevent` / `SIGEV_*` come from newlib's `<signal.h>` (the layer does not define
  its own copy). Newlib declares the members `sigev_notify_function` / `sigev_notify_attributes` only when
  `_POSIX_THREADS` is defined, so the layer's `signal.h` defines it while newlib's header is parsed (and
  restores it afterwards). Include `<signal.h>` in code which fills a `struct sigevent` for `timer_create()`;
- newlib's `<signal.h>` declares `pthread_kill(pthread_t, int)` and refers to `pthread_attr_t`, so the layer's
  pthread types must exist before it is read. The layer's `signal.h` guarantees that; any file that includes
  `<signal.h>` (for example a `syscalls.c`) therefore compiles without including `pthread.h` itself;
- every layer header installs the guard `_SYS__PTHREADTYPES_H_` before it reads a toolchain header, so
  `<sys/types.h>` does not define newlib's own pthread types. If the toolchain's `<sys/types.h>`,
  `<pthread.h>` or `<signal.h>` is read **before** any header of the layer (for example because the layer's
  include directory is behind the toolchain's), the types are defined twice and `stk_c_pthread.h` stops with an
  explicit `#error` telling so.

**Declaration clashes.** If your toolchain declares one of the functions with a different spelling, skip the
layer's declaration with one of these macros (define them in the build system):

| Macro | Skips |
|---|---|
| `STK_C_NO_TIME_DECLS` | `clock_gettime`, `clock_getres`, `clock_nanosleep`, `nanosleep`, `timespec_get` |
| `STK_C_NO_SLEEP_DECLS` | `usleep`, `sleep` |
| `STK_C_NO_TIMER_DECLS` | `timer_create` ... `timer_getoverrun` |
| `STK_C_NO_ITIMERSPEC` / `STK_C_NO_TIMER_T` | `struct itimerspec` / `typedef ... timer_t` |
| `STK_C_DEFINE_SIGEVENT` | `1` makes `time.h` define `union sigval`, `struct sigevent`, `SIGEV_*` itself (toolchains without their own); `0` is the default for newlib/glibc, where `<signal.h>` provides them |
| `STK_C_NO_GETTIMEOFDAY_DECL` / `STK_C_NO_SYSCONF_DECLS` | `gettimeofday` / `sysconf`, `getpid` |

**Replacing C library functions.** `time()`, `gettimeofday()`, `sleep()`, `usleep()`, `nanosleep()`, `clock_*`
and `sysconf()` of this layer replace the C library's versions: make sure `stk_c_pthread.cpp` is linked so its
definitions are used (an object file given explicitly on the link line is always preferred over the library).

---

## Common Pitfalls

**Kernel task count too small** — You need your own tasks + the reaper + every live pthread. Running out makes
`pthread_create()` fail (`EAGAIN`) or the system misbehave. See [Size the Task Table](#3-size-the-task-table).

**Wrong kernel type** — The kernel must include `KERNEL_DYNAMIC | KERNEL_SYNC` and must not be `KERNEL_HRT`. This
cannot be detected at run time and is not reported through an error code.

**Priorities without `SwitchStrategyFixedPriority`** — `pthread_setschedparam()` calls `stk::SetWeight()`, which
asserts when the strategy has no weight/priority-inheritance API. Use `SwitchStrategyFixedPriority`, or set
`STK_C_PTHREAD_RUNTIME_PRIORITY` to `0`. Also match `STK_C_SCHED_PRIORITY_MAX` to the number of levels.

**Passing the wrong pointer to `stk_pthread_bind_kernel()`** — It takes the `stk::IKernel *`. From C++ use
`static_cast<stk::IKernel *>(&kernel)`; a pointer to a more derived kernel type converted to `void *` may have a
different address.

**Forgetting `stk_pthread_bind_kernel()`** — Call it once before the first `pthread_create()`. Without it
`pthread_create()` asserts in debug builds and returns `EINVAL`.

**Stack size in words instead of bytes** — `pthread_attr_setstacksize()` and `pthread_attr_setstack()` take
**bytes**. Multiply words by `sizeof(pthread_stk_word_t)`.

**Non-default stack sizes need the heap** — Only exactly `STK_C_PTHREAD_DEFAULT_STACK_WORDS` words come from the
static pool; any other size calls `malloc()` at `pthread_create()` time. Message queues also use `malloc()`.

**Never joining or detaching** — An un-joined, non-detached thread keeps its `STK_C_PTHREAD_MAX_THREADS` slot and
its task-table entry forever.

**Building `abstime` from `time()`** — Absolute deadlines are on the kernel timeline. Use `clock_gettime()`; do not
use `time()` / `gettimeofday()` after `stk_posix_set_wall_time()`. See [Timed Waits and abstime](#timed-waits-and-abstime).

**Cancelling a thread that never reaches a cancellation point** — Mutex, rwlock, spinlock and barrier waits and
plain computation are not cancellation points; the request stays pending. Add `pthread_testcancel()` to long loops.

**Calling the TLS API in a pthread task** — The TLS slot belongs to the layer in any task that uses the pthread
API; overwriting it replaces the thread's handle and breaks `pthread_self()` and thread-specific data.

**Relying on a normal mutex's relock behaviour** — The underlying mutex is recursive, so relocking a
`PTHREAD_MUTEX_NORMAL` mutex from its owner succeeds here instead of deadlocking as it may on other systems.
Request `PTHREAD_MUTEX_RECURSIVE` explicitly if you need recursion and `PTHREAD_MUTEX_ERRORCHECK` to catch
accidental relocks (`EDEADLK`). Never wait on a condition variable with a mutex locked more than once.

**Not testing the predicate in a loop** — `pthread_cond_wait()` can wake spuriously and signals are not queued;
always loop on the guarded condition.

**Holding a spinlock across a blocking call** — Spinlocks busy-wait and are not recursive. Keep the protected
region tiny and never block while holding one.

**Joining an adopted task or yourself** — Adopted tasks are not joinable (`EINVAL`), and joining the calling thread
returns `EDEADLK`.

**Killing a thread which holds a lock** — `pthread_kill(t, SIGKILL)` / `SIGTERM` of another thread removes it without
running its cleanup handlers: mutexes it owns stay locked forever. Use cancellation, or make sure the target holds
no locks.

**Blocking from an ISR** — The whole API is task-context only, except `sem_post()` / `sem_trywait()` /
`sem_getvalue()`. See [Context Restrictions (ISR)](#context-restrictions-isr).

**Releasing an external stack too early** — A buffer passed to `pthread_attr_setstack()` must remain valid until the
thread has been joined or reaped.

**Deleting or re-arming a timer from its own notification** — `timer_settime()` / `timer_delete()` wait for the
timer host, which is busy running the notification: use a flag or another thread.

**Shared `errno`** — `sem_*`, `mq_*` and the time functions report errors through `errno`. If your C library keeps
one global `errno` (no per-task reentrancy structure), concurrent failures can overwrite each other.

---

## Configuration Reference

Place these in `stk_config.h` (or the build system) before including any STK header. Every macro has a default.

```c
/* --- Kernel --- */
/* Kernel type: DYNAMIC + SYNC, not HRT; task count = own tasks + 1 reaper + live pthreads.
   Use SwitchStrategyFixedPriority (e.g. FP32) for priorities. With the C interface define
   STK_C_KERNEL_MAX_TASKS and STK_C_KERNEL_TYPE_CPU_0 instead. */

/* --- Threads --- */
#define STK_C_PTHREAD_MAX_THREADS          8      /* created threads + adopted tasks */
#define STK_C_PTHREAD_DEFAULT_STACK_WORDS  1024   /* default per-thread stack, in words */
#define STK_C_PTHREAD_REAPER_STACK_WORDS   256    /* internal reaper task stack, in words */
#define STK_C_PTHREAD_KEYS_MAX             8      /* thread-specific data keys */
#define STK_C_PTHREAD_NAME_MAX             16     /* thread name capacity incl. NUL */
#define STK_C_PTHREAD_CLEANUP_MAX          8      /* cleanup handler nesting depth */

/* --- Scheduling --- */
#define STK_C_SCHED_PRIORITY_MIN           0
#define STK_C_SCHED_PRIORITY_MAX           31     /* MAX_PRIORITIES - 1 of the strategy */
#define STK_C_SCHED_PRIORITY_DEFAULT       16     /* (MAX + 1) / 2 = PRIORITY_NORMAL */
#define STK_C_PTHREAD_RUNTIME_PRIORITY     1      /* 0: no stk::SetWeight() (strategies without it) */

/* --- Message queues --- */
#define STK_C_MQ_MAX_QUEUES                4
#define STK_C_MQ_MAX_DESCRIPTORS           8
#define STK_C_MQ_NAME_MAX                  32
#define STK_C_MQ_DEFAULT_MAXMSG            8
#define STK_C_MQ_DEFAULT_MSGSIZE           64

/* --- Timers, system configuration --- */
#define STK_C_TIMERS_MAX                   8
#define STK_C_POSIX_PID                    1
#define STK_C_POSIX_PPID                   0
#define STK_C_POSIX_UID                    0
#define STK_C_POSIX_GID                    0
#define STK_C_SYSCONF_NPROCESSORS          1L
#define STK_C_SYSCONF_PAGESIZE             4096L

/* --- Toolchain integration (build system) --- */
/* STK_C_NO_TIME_DECLS, STK_C_NO_SLEEP_DECLS, STK_C_NO_TIMER_DECLS, STK_C_NO_ITIMERSPEC, STK_C_NO_TIMER_T,
   STK_C_DEFINE_SIGEVENT, STK_C_NO_GETTIMEOFDAY_DECL, STK_C_NO_SYSCONF_DECLS,
   STK_C_SYSTEM_TIME_H / STK_C_SYSTEM_SYS_TIME_H / STK_C_SYSTEM_UNISTD_H / STK_C_SYSTEM_SIGNAL_H (non-GCC) */
```
