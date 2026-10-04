# STK Pthread API

The Pthread API is a minimal, POSIX-named pthreads-style layer on top of STK.
It lets code written against pthreads (threads, mutexes, condition variables,
read-write locks, barriers, thread-specific data) run on STK with little or no
change. The public header needs only `stk_c.h` (no C++ headers), so it can be
used from plain C; the implementation calls the STK C++ API directly.

This is deliberately a **subset** of POSIX threads, limited to what maps cleanly
onto STK's task and synchronization model. See [What Is Supported](#what-is-supported).

---

## Contents

- [Headers and Sources](#headers-and-sources)
- [What Is Supported](#what-is-supported)
- [Configuration](#configuration)
- [Quick Start](#quick-start)
- [Example Project](#example-project)
- [Step-by-Step Setup](#step-by-step-setup)
  - [1. Configure the Kernel Type](#1-configure-the-kernel-type)
  - [2. Size the Task Table](#2-size-the-task-table)
  - [3. Create, Bind, and Start the Kernel](#3-create-bind-and-start-the-kernel)
  - [4. Create Threads from the Bootstrap Task](#4-create-threads-from-the-bootstrap-task)
- [Threads](#threads)
  - [Creating and Joining](#creating-and-joining)
  - [Thread Attributes](#thread-attributes)
  - [Detached Threads](#detached-threads)
  - [Exiting a Thread](#exiting-a-thread)
  - [Thread Identity and Adoption](#thread-identity-and-adoption)
  - [Stacks and Memory](#stacks-and-memory)
- [Mutex](#mutex)
- [Condition Variable](#condition-variable)
- [Timed Waits and abstime](#timed-waits-and-abstime)
- [Reader-Writer Lock](#reader-writer-lock)
- [Spinlock](#spinlock)
- [Barrier](#barrier)
- [One-Time Initialization](#one-time-initialization)
- [Thread-Specific Data (Keys)](#thread-specific-data-keys)
- [Static Initializers](#static-initializers)
- [Context Restrictions (ISR)](#context-restrictions-isr)
- [Return Codes](#return-codes)
- [How It Maps to STK](#how-it-maps-to-stk)
- [Common Pitfalls](#common-pitfalls)
- [Configuration Reference](#configuration-reference)

---

## Headers and Sources

| File | Purpose |
|---|---|
| `stk_c_pthread.h` | Pthread types, constants, and function declarations |
| `stk_c_pthread.cpp` | Implementation |

Compile the matching `.cpp` files alongside your project. The Pthread layer is
built on the C interface, so its sources are required too (the block pool is
used for default-size thread stacks):

```
stk_c.cpp
stk_c_sync.cpp
stk_c_memory.cpp
stk_c_pthread.cpp
```

Include `stk_c_pthread.h` **instead of** the toolchain's `<pthread.h>`. The
header neutralizes the toolchain's own pthread type definitions so the two do
not clash. Error codes are standard `errno` values (`EINVAL`, `EAGAIN`,
`ETIMEDOUT`, ...); include `<errno.h>` in files that test for them.

---

## What Is Supported

| POSIX area | Support |
|---|---|
| `pthread_create` / `join` / `detach` / `exit` / `self` / `equal` | Yes |
| `pthread_yield` | Yes (non-POSIX GNU/BSD-style extension; POSIX uses `sched_yield()`) |
| `pthread_attr_t` (stack size, external stack, detach state) | Yes |
| `pthread_attr_set/getprivileged_np` | Yes (non-portable STK extension) |
| `pthread_mutex_t` (normal / default) | Yes — recursive in practice, see [Mutex](#mutex) |
| `pthread_mutex_t` (recursive, errorcheck) | Yes |
| `pthread_cond_t` (`wait` / `timedwait` / `signal` / `broadcast`) | Yes |
| `pthread_rwlock_t` (read/write, try, timed, unlock) | Yes |
| `pthread_spin_*` | Yes (`PTHREAD_PROCESS_PRIVATE` only) |
| `pthread_barrier_t` | Yes |
| `pthread_once` | Yes |
| `pthread_key_t` (create / delete / set / get, destructors) | Yes |
| `stk_pthread_bind_kernel()` | Yes — STK-specific bootstrap, not POSIX |
| Cancellation (`pthread_cancel`, ...) | No |
| Scheduling policy / priority get-set | No — `pthread_t` is opaque, so `stk_task_set_priority()` cannot be applied to it |

---

## Configuration

All compile-time limits are set with `#define` before including any STK header,
or more typically in your project's `stk_config.h`.

| Macro | Default | Meaning |
|---|---|---|
| `STK_C_PTHREAD_MAX_THREADS` | `8` | Max concurrently-alive `pthread_t`'s (created threads **plus** adopted tasks) |
| `STK_C_PTHREAD_DEFAULT_STACK_WORDS` | `1024` | Default per-thread stack size, in `stk_word_t` words |
| `STK_C_PTHREAD_REAPER_STACK_WORDS` | `256` | Stack words for the internal reaper task |
| `STK_C_PTHREAD_KEYS_MAX` | `8` | Max concurrently-alive `pthread_key_t`'s (also available as `PTHREAD_KEYS_MAX`) |

Two C-interface limits matter as well:

| Macro | Why it matters here |
|---|---|
| `STK_C_KERNEL_MAX_TASKS` | Must hold your own tasks + the reaper task + all live pthreads. See [Size the Task Table](#2-size-the-task-table) |
| `STK_C_BLOCKPOOL_MAX` | The default-size stack pool takes one block-pool slot (created on the first default-size `pthread_create()`) |

---

## Quick Start

Two threads incrementing a shared counter under a mutex, started from a
bootstrap task:

```c
#include <stk_c_pthread.h>

#define MAIN_STACK_WORDS 512
static __stk_c_stack stk_word_t g_main_stack[MAIN_STACK_WORDS];

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

static void main_entry(void *arg) {
    pthread_t t[2];

    for (int i = 0; i < 2; ++i) {
        pthread_create(&t[i], NULL, worker, NULL);
    }
    for (int i = 0; i < 2; ++i) {
        pthread_join(t[i], NULL);
    }
    /* g_counter == 200 here */

    while (1) {
        stk_sleep_ms(1000);
    }
}

int main(void) {
    stk_kernel_t *k = stk_kernel_create(0);               /* core 0 */
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);          /* 1 ms tick */
    stk_pthread_bind_kernel(k);                           /* once, before pthread_create() */

    stk_task_t *main_task = stk_task_create_user(main_entry, NULL,
                                                 g_main_stack, MAIN_STACK_WORDS);
    stk_kernel_add_task(k, main_task);

    stk_kernel_start(k);   /* never returns; main_entry() may now call pthread_create() */

    STK_C_ASSERT(false);   /* should not reach here */
}
```

The kernel type and `STK_C_KERNEL_MAX_TASKS` must be configured for this to
work — see the steps below.

---

## Example Project

The repository contains a dedicated, buildable pthread example in
[`build/example/blinky_pthread`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky_pthread).
It shows the Pthread API in a complete project, including kernel setup,
`stk_pthread_bind_kernel()` and thread creation, and is a good reference for
project-level configuration.

---

## Step-by-Step Setup

The Pthread layer never creates or starts a kernel itself. You set up and start
an STK kernel exactly as you would without pthreads, then bind it once with
`stk_pthread_bind_kernel()`.

### 1. Configure the Kernel Type

Define `STK_C_KERNEL_TYPE_CPU_0` in your `stk_config.h` **before** any STK
header is included. The kernel type for the bound core must include **both**
`KERNEL_DYNAMIC` and `KERNEL_SYNC`:

```c
#define STK_C_KERNEL_TYPE_CPU_0 \
    Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, \
           STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
```

| Requirement | Why |
|---|---|
| `KERNEL_DYNAMIC` | Threads finish by returning from their entry function or calling `pthread_exit()`; STK supports that only for dynamic kernels |
| `KERNEL_SYNC` | Enables the Mutex / Event / Semaphore primitives the layer is built on |
| Not `KERNEL_HRT` | `pthread_create()` has no way to supply a period and deadline |

Any non-HRT scheduling strategy works (`SwitchStrategyRR`, `SwitchStrategySWRR`,
`SwitchStrategyFP32`). The layer cannot verify the kernel type at run time: a
mismatch is a build-configuration error and is **not** reported through an error
code.

### 2. Size the Task Table

`STK_C_KERNEL_MAX_TASKS` must be large enough for **all** of:

```
your own pre-existing tasks (including the bootstrap task)
+ 1 internal reaper task
+ the maximum number of concurrently-alive pthreads
```

A pthread stays "alive" from `pthread_create()` until it is reclaimed: by
`pthread_join()`, or by the reaper for detached threads once they finish. A
finished-but-not-yet-joined thread still counts.

```c
/* 1 bootstrap task + 1 reaper + up to 4 worker threads */
#define STK_C_KERNEL_MAX_TASKS      6

/* 4 workers + 1 adopted bootstrap task (see "Thread Identity and Adoption") */
#define STK_C_PTHREAD_MAX_THREADS   5
```

> The C interface default of `STK_C_KERNEL_MAX_TASKS` is `4`, and the default
> `STK_C_PTHREAD_MAX_THREADS` is `8`. The defaults are **not** consistent with
> each other — raise `STK_C_KERNEL_MAX_TASKS` to match.

### 3. Create, Bind, and Start the Kernel

```c
stk_kernel_t *k = stk_kernel_create(0);            /* 0 = core 0 */
stk_kernel_init(k, STK_PERIODICITY_DEFAULT);        /* tick = 1000 µs */
stk_pthread_bind_kernel(k);                         /* once, before pthread_create() */

/* ... add your bootstrap task(s) ... */
stk_kernel_start(k);
```

`stk_pthread_bind_kernel()`:

- must be called **exactly once**, before the first `pthread_create()`;
- needs the kernel to be created and initialized, but not yet started;
- also adds the internal **reaper task** to the kernel (always, whether or not
  you ever detach a thread), so `STK_C_KERNEL_MAX_TASKS` must have room for it;
- does not support binding a second, different kernel (the reaper stays with the
  first).

### 4. Create Threads from the Bootstrap Task

Once the kernel is running, any task may call `pthread_create()`. Typically the
application's bootstrap task (the one added before `stk_kernel_start()`) creates
the worker threads, as in the [Quick Start](#quick-start).

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
    /* EINVAL: no kernel bound; EAGAIN: no free thread slot / stack allocation
       failed / underlying task could not be created */
}

void *result;
rc = pthread_join(t, &result);            /* blocks until worker returns */
```

`pthread_join()` blocks until the thread finishes, then reclaims its resources
(stack and slot). It returns `EINVAL` if the thread is `NULL`, already joined,
detached or adopted, and `EDEADLK` if a thread tries to join itself.

> Every joinable thread must be joined (or detached) eventually. An un-joined
> thread keeps its `STK_C_PTHREAD_MAX_THREADS` slot and its task-table entry.

### Thread Attributes

```c
pthread_attr_t attr;
pthread_attr_init(&attr);                 /* default stack, joinable, user mode */

pthread_attr_setstacksize(&attr, 2048 * sizeof(stk_word_t));   /* BYTES, not words */
pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
pthread_attr_setprivileged_np(&attr, PTHREAD_CREATE_PRIVILEGED_NP);

pthread_create(&t, &attr, worker, NULL);
pthread_attr_destroy(&attr);              /* no-op; no owned resources */
```

| Function | Effect |
|---|---|
| `pthread_attr_init()` | Defaults: `STK_C_PTHREAD_DEFAULT_STACK_WORDS` stack, no external stack, joinable, user mode |
| `pthread_attr_setstacksize()` / `getstacksize()` | Stack size in **bytes**. `getstacksize()` reports the default size if none was set |
| `pthread_attr_setstack()` / `getstack()` | Use a caller-supplied stack buffer (must be `stk_word_t`-aligned; `EINVAL` otherwise) |
| `pthread_attr_setdetachstate()` / `getdetachstate()` | `PTHREAD_CREATE_JOINABLE` (default) or `PTHREAD_CREATE_DETACHED` |
| `pthread_attr_setprivileged_np()` / `getprivileged_np()` | **Non-portable.** `PTHREAD_CREATE_USER_NP` (default, `stk_task_create_user()`) or `PTHREAD_CREATE_PRIVILEGED_NP` (`stk_task_create_privileged()`, e.g. for direct peripheral access) |

An external stack:

```c
static __stk_c_stack stk_word_t g_worker_stack[512];

pthread_attr_setstack(&attr, g_worker_stack, sizeof(g_worker_stack));
```

The buffer is never freed by the layer; it must stay valid for the whole life of
the thread (until it is joined or reaped).

### Detached Threads

A detached thread cannot be joined. Its resources are reclaimed automatically by
the internal reaper task once it finishes.

```c
pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);   /* at creation */
pthread_create(&t, &attr, worker, NULL);

/* or later: */
pthread_detach(t);                        /* EINVAL if already joined or detached */
```

### Exiting a Thread

A thread ends by returning from its entry function or by calling
`pthread_exit()`. Both deliver the value to a joiner and run the thread's key
destructors.

```c
static void *worker(void *arg) {
    if (fatal_error) {
        pthread_exit((void *)-1);         /* does not return */
    }
    return NULL;
}
```

### Thread Identity and Adoption

`pthread_self()` is backed by STK's per-task TLS slot, which the layer fills in
for every thread it creates. A task the layer did **not** create — typically the
bootstrap task added before `stk_kernel_start()` — is **adopted** on first use:
the first call to `pthread_self()` or `pthread_setspecific()` from it gives it a
`pthread_t`.

```c
pthread_t me = pthread_self();            /* adopts the bootstrap task on first call */
```

Consequences of adoption:

- each adopted task permanently consumes one `STK_C_PTHREAD_MAX_THREADS` slot,
  so size that limit as *(created threads + adopted tasks)*;
- an adopted thread is not joinable and never reaped — `pthread_join()` and
  `pthread_detach()` return `EINVAL` for it;
- the task's TLS slot now belongs to the layer: do **not** call `stk_tls_set()`
  / `STK_TLS_SET_T()` in a task that uses the pthread API;
- `pthread_exit()` from an adopted task runs its key destructors and then parks
  the task forever;
- `pthread_getspecific()` never adopts, so merely reading does not use a slot.

`pthread_self()` returns `NULL` only if no slot is left to adopt into.
`pthread_equal(a, b)` compares two handles.

### Stacks and Memory

| Stack source | Heap use | Freed when |
|---|---|---|
| Default size (`STK_C_PTHREAD_DEFAULT_STACK_WORDS`, no attr or default attr) | None — served from a static pool of `STK_C_PTHREAD_MAX_THREADS` blocks | Thread is reclaimed (block returned to the pool) |
| Any other size via `pthread_attr_setstacksize()` | `malloc()` | Thread is reclaimed (`free()`) |
| Caller buffer via `pthread_attr_setstack()` | None | Never — caller owns it |

The internal reaper task uses a static stack of `STK_C_PTHREAD_REAPER_STACK_WORDS`
words. If you want a fully heap-free system, use only the default stack size or
supply your own stacks.

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

A mutex initialized with `PTHREAD_MUTEX_INITIALIZER` has its underlying STK mutex
created lazily on first `lock` / `trylock` / `timedlock`. `pthread_mutex_unlock()`
never creates it, so unlocking a never-used static mutex returns `EINVAL`.

Mutex types:

```c
pthread_mutexattr_t ma;
pthread_mutexattr_init(&ma);
pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);   /* or _ERRORCHECK / _NORMAL */

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

- A `NORMAL` mutex does **not** deadlock when relocked by its owner. POSIX leaves
  that case undefined, so portable code should not rely on it either way. If you
  want recursion, request `PTHREAD_MUTEX_RECURSIVE` explicitly.
- The maximum nesting depth is `0xFFFE` (`stk::sync::Mutex::RECURSION_MAX`); one
  more `lock` / `trylock` / `timedlock` returns `EAGAIN`.
- `pthread_mutexattr_settype()` and `pthread_mutex_init()` return `EINVAL` for a
  type other than the three above.
- A mutex from `PTHREAD_MUTEX_INITIALIZER` is of type `NORMAL`.
- Do not pass a mutex that is locked more than once to `pthread_cond_wait()` /
  `pthread_cond_timedwait()` (undefined, as in POSIX): only one level is released,
  so other threads still cannot acquire it. For `ERRORCHECK` mutexes, waiting
  without owning the mutex returns `EPERM`.

For a deadline-based lock see [Timed Waits and abstime](#timed-waits-and-abstime).

---

## Condition Variable

Always paired with a locked mutex that protects the guarded state. Re-check the
predicate in a loop — spurious wakeups are possible, as in POSIX.

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

Signals are not queued: one sent while nobody waits is lost, which is why the
predicate is tested under the mutex. The mutex passed to `pthread_cond_wait()`
must be locked by the caller (and therefore already initialized/used), otherwise
the call returns `EINVAL`.

`pthread_cond_init()` / `pthread_cond_destroy()` work as for mutexes; no thread
may be waiting when a condition variable is destroyed.

---

## Timed Waits and abstime

`pthread_mutex_timedlock()`, `pthread_cond_timedwait()`,
`pthread_rwlock_timedrdlock()` and `pthread_rwlock_timedwrlock()` take an
**absolute** deadline, as in POSIX — but STK has no wall clock
(`CLOCK_REALTIME`). It only has a monotonic tick count since `stk_kernel_init()`.

`abstime` is therefore a point on the `stk_time_now_ms()` timeline. Derive it by
adding a duration to `stk_time_now_ms()`, **never** from a time-of-day clock:

```c
#include <errno.h>

static void abstime_after_ms(struct timespec *ts, int32_t ms) {
    stk_time_t deadline_ms = stk_time_now_ms() + ms;
    ts->tv_sec  = (time_t)(deadline_ms / 1000);
    ts->tv_nsec = (long)((deadline_ms % 1000) * 1000000);
}

struct timespec abstime;
abstime_after_ms(&abstime, 500);            /* 500 ms from now */

pthread_mutex_lock(&g_lock);
while (g_available == 0) {
    int rc = pthread_cond_timedwait(&g_ready, &g_lock, &abstime);
    if (rc == ETIMEDOUT) {
        break;                              /* mutex is still re-locked here */
    }
}
pthread_mutex_unlock(&g_lock);
```

Behaviour of the conversion:

- the deadline is converted to a relative timeout in milliseconds, then to
  kernel ticks;
- a deadline **already in the past** gives a zero timeout, so the call behaves
  like a non-blocking attempt;
- very distant deadlines are clamped to `INT32_MAX` ms;
- on timeout the functions return `ETIMEDOUT`; `pthread_cond_timedwait()`
  re-locks the mutex before returning in every case.

---

## Reader-Writer Lock

Multiple readers can hold the lock concurrently; a writer is exclusive. The
underlying implementation uses a **writer-preference** policy, and there is no
setting to change it.

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
```

As in POSIX, a single `pthread_rwlock_unlock()` releases either a read or a
write hold. The layer records inside the `pthread_rwlock_t` whether the lock is
held for writing (safe because a write hold is exclusive) and calls the matching
STK unlock. Calling it on a lock you do not hold is undefined behaviour and is
not detected.

---

## Spinlock

A busy-waiting lock for very short regions. Not recursive: relocking from the
owner deadlocks. Only `PTHREAD_PROCESS_PRIVATE` is accepted (`PTHREAD_PROCESS_SHARED`
returns `ENOTSUP`; STK has no cross-process concept).

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

Unlike the other objects, spinlocks have **no static initializer** (matching
POSIX); `pthread_spin_init()` must be called first.

---

## Barrier

Blocks a fixed number of threads until all have arrived, then releases them
together; the barrier resets itself for reuse.

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

Barriers have **no static initializer**: the trip count is supplied to
`pthread_barrier_init()`, which also returns `EINVAL` for a count of `0`.

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

A thread that calls `pthread_once()` while another thread's init routine is
still running **waits** for it to finish, rather than racing ahead. The routine
must not call `pthread_once()` on the same `pthread_once_t` (self-deadlock, as in
POSIX).

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
- destructors run when a thread finishes (return or `pthread_exit()`), for at
  most `PTHREAD_DESTRUCTOR_ITERATIONS` passes — a destructor that stores a new
  non-NULL value for its own key causes another pass;
- they do **not** run for an adopted task that never exits;
- `pthread_setspecific()` from an un-adopted foreign task adopts it, and returns
  `ENOMEM` if no slot is left.

---

## Static Initializers

| Object | Static initializer | Lazily creates underlying STK object |
|---|---|---|
| `pthread_mutex_t` | `PTHREAD_MUTEX_INITIALIZER` | Yes, on first lock |
| `pthread_cond_t` | `PTHREAD_COND_INITIALIZER` | Yes, on first use |
| `pthread_rwlock_t` | `PTHREAD_RWLOCK_INITIALIZER` | Yes, on first use |
| `pthread_once_t` | `PTHREAD_ONCE_INIT` | Yes (guard mutex), on first use |
| `pthread_spinlock_t` | none — call `pthread_spin_init()` | — |
| `pthread_barrier_t` | none — call `pthread_barrier_init()` | — |

---

## Context Restrictions (ISR)

The **blocking** calls — `pthread_join()`, the lock/wait functions of mutexes,
condition variables and read-write locks (including the timed variants, which
block until the deadline), `pthread_barrier_wait()` and `pthread_once()` — must
not be called from an ISR: the underlying STK primitives are ISR-unsafe for
blocking operations.

STK's own non-blocking operations (try-lock, unlock, notify) are ISR-safe, but
the Pthread layer adds lazy initialization and bookkeeping around them, so treat
the **whole pthread API as task-context only**. For ISR-to-task hand-off use the
C interface directly (see the C interface README).

---

## Return Codes

| Code | Typical cause |
|---|---|
| `EINVAL` | `NULL` argument; invalid or never-initialized object; no kernel bound; invalid attribute value; thread already joined/detached/adopted |
| `EAGAIN` | No free thread slot (`STK_C_PTHREAD_MAX_THREADS`); stack allocation failed; key slots exhausted (`STK_C_PTHREAD_KEYS_MAX`); mutex recursion depth exceeded |
| `ENOTSUP` | `PTHREAD_PROCESS_SHARED` for a spinlock |
| `ENOMEM` | `pthread_setspecific()`: no slot left to adopt the calling task into |
| `EBUSY` | A `try*` call could not take the lock (including `trylock` on an `ERRORCHECK` mutex the caller already owns) |
| `EPERM` | `pthread_mutex_unlock()` by a thread that does not own the mutex; `pthread_cond_wait()` / `pthread_cond_timedwait()` on an `ERRORCHECK` mutex the caller does not own |
| `ETIMEDOUT` | A timed wait's deadline passed first |
| `EDEADLK` | `pthread_join()` on the calling thread itself; relocking an `ERRORCHECK` mutex from its owner |

`pthread_getspecific()` and `pthread_self()` return `NULL` instead of an error
code.

---

## How It Maps to STK

| Pthread object | Underlying STK object |
|---|---|
| `pthread_t` | A task in the bound `KERNEL_DYNAMIC` kernel (plus a control block from the pthread slot pool) |
| `pthread_mutex_t` | `stk::sync::Mutex` |
| `pthread_cond_t` | `stk::sync::ConditionVariable` |
| `pthread_rwlock_t` | `stk::sync::RWMutex` |
| `pthread_spinlock_t` | `stk::hw::SpinLock` |
| `pthread_barrier_t` | `stk::sync::Barrier` |
| `pthread_once_t` | A guard `stk::sync::Mutex` |
| `pthread_join()` wait | A manual-reset `stk::sync::Event` per thread |
| `pthread_self()` / keys | The per-task TLS slot |

The `stk_*_mem_t` blobs embedded in the `pthread_*` types are used as in-place
storage for these C++ objects, so no heap allocation is needed for the
synchronization primitives.

---

## Common Pitfalls

**`STK_C_KERNEL_MAX_TASKS` too small** — The default is `4`, but you need your
own tasks + the reaper + every live pthread. Running out makes
`pthread_create()` fail (`EAGAIN`) or the system misbehave. See
[Size the Task Table](#2-size-the-task-table).

**Wrong kernel type** — The kernel must include `KERNEL_DYNAMIC | KERNEL_SYNC`
and must not be `KERNEL_HRT`. This cannot be detected at run time and is not
reported through an error code.

**Forgetting `stk_pthread_bind_kernel()`** — Call it once before the first
`pthread_create()`. Without it `pthread_create()` asserts in debug builds and
returns `EINVAL`.

**Stack size in words instead of bytes** — `pthread_attr_setstacksize()` and
`pthread_attr_setstack()` take **bytes**. Multiply words by `sizeof(stk_word_t)`.

**Non-default stack sizes need the heap** — Only exactly
`STK_C_PTHREAD_DEFAULT_STACK_WORDS` words come from the static pool; any other
size calls `malloc()` at `pthread_create()` time.

**Never joining or detaching** — An un-joined, non-detached thread keeps its
`STK_C_PTHREAD_MAX_THREADS` slot and its task-table entry forever.

**Wall-clock `abstime`** — Building `abstime` from `clock_gettime(CLOCK_REALTIME)`
or similar produces a deadline on the wrong timeline. Always start from
`stk_time_now_ms()`. See [Timed Waits and abstime](#timed-waits-and-abstime).

**Calling `stk_tls_set()` in a pthread task** — The TLS slot belongs to the
Pthread layer in any task that uses the pthread API; overwriting it replaces the
thread's handle and breaks `pthread_self()` and thread-specific data.

**Relying on a normal mutex's relock behaviour** — The underlying mutex is
recursive, so relocking a `PTHREAD_MUTEX_NORMAL` mutex from its owner succeeds
here instead of deadlocking as it may on other systems. Request
`PTHREAD_MUTEX_RECURSIVE` explicitly if you need recursion and
`PTHREAD_MUTEX_ERRORCHECK` to catch accidental relocks (`EDEADLK`). Never wait on
a condition variable with a mutex locked more than once.

**Not testing the predicate in a loop** — `pthread_cond_wait()` can wake
spuriously and signals are not queued; always loop on the guarded condition.

**Holding a spinlock across a blocking call** — Spinlocks busy-wait and are not
recursive. Keep the protected region tiny and never block while holding one.

**Joining an adopted task or yourself** — Adopted tasks are not joinable
(`EINVAL`), and joining the calling thread returns `EDEADLK`.

**Blocking from an ISR** — The whole pthread API is task-context only. See
[Context Restrictions (ISR)](#context-restrictions-isr).

**Releasing an external stack too early** — A buffer passed to
`pthread_attr_setstack()` must remain valid until the thread has been joined or
reaped.

**Block-pool slots** — The default-size stack pool takes one of the
`STK_C_BLOCKPOOL_MAX` slots; if you use many block pools of your own, raise that
limit by one.

---

## Configuration Reference

Place these in `stk_config.h` before including any STK header.

```c
/* --- C interface limits that the Pthread layer depends on --- */

/* Own tasks + 1 reaper + concurrently-alive pthreads */
#define STK_C_KERNEL_MAX_TASKS          12

/* Default-size stack pool uses one slot */
#define STK_C_BLOCKPOOL_MAX             4

/* --- Pthread layer --- */

/* Max concurrently-alive pthread_t's (created threads + adopted tasks) */
#define STK_C_PTHREAD_MAX_THREADS       8

/* Default per-thread stack, in stk_word_t words */
#define STK_C_PTHREAD_DEFAULT_STACK_WORDS  1024

/* Stack words for the internal reaper task */
#define STK_C_PTHREAD_REAPER_STACK_WORDS   256

/* Max concurrently-alive thread-specific data keys */
#define STK_C_PTHREAD_KEYS_MAX          8

/* Kernel type for core 0 — required: DYNAMIC + SYNC, not HRT */
#define STK_C_KERNEL_TYPE_CPU_0 \
    Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, \
           STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
```
