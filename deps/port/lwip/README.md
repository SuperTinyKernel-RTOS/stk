# lwIP `sys_arch` port for SuperTinyKernel (STK)

Two files, dropped into your port's `arch/` directory next to your existing
`cc.h`:

- **`sys_arch.h`** — the opaque wrapper structs lwIP's core expects
  (`sys_mutex_t`, `sys_sem_t`, `sys_mbox_t`, `sys_thread_t`), each holding a
  `void*` handle so the header stays includable from both C and C++.
- **`sys_arch.cpp`** — the implementation. **Must be compiled as C++** (it
  uses `stk::sync::Mutex/Semaphore/MessageQueue`, `stk::hw::CriticalSection`,
  `stk::ITask`, `stk::IKernel`), but every function lwIP calls is `extern "C"`
  so it links cleanly against lwIP's plain-C `.c` files.

## One extra setup step: registering the kernel

STK doesn't presuppose "the one and only" kernel instance in the image: the
application instantiates its own `stk::Kernel<...>` object and drives its
lifecycle explicitly (`Initialize()` / `AddTask()` / `Start()`). Since
`sys_thread_new()` has no parameter through which lwIP could hand this port
a kernel reference, the application must register it once, before touching
lwIP:

```cpp
#include "stk.h"
#include "sync/stk_sync.h"
#include "arch/sys_arch.h"

using MyKernel = stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC,
                              16 /* max tasks */,
                              stk::SwitchStrategySmoothWeightedRoundRobin,
                              stk::PlatformDefault>;
static MyKernel g_kernel;

int main()
{
    g_kernel.Initialize();
    sys_arch_set_kernel(&g_kernel);   // <-- STK-specific, before tcpip_init()

    tcpip_init(NULL, NULL);          // spawns tcpip_thread via sys_thread_new()
    // ... g_kernel.AddTask(&my_app_task); for any of your own tasks ...

    g_kernel.Start();                // never returns
}
```

Requirements on the kernel instance:
- **`stk::KERNEL_SYNC`** is mandatory — the port is built entirely on
  `stk::sync::*`.
- **`stk::KERNEL_DYNAMIC`** is recommended so `AddTask()` for lwIP-spawned
  threads works the same way at any point in the program, and so a thread
  that unexpectedly returns can be retired cleanly.

## Primitive mapping

| lwIP type      | STK backing                              | Allocation |
|-----------------|-------------------------------------------|------------|
| `sys_mutex_t`   | `stk::sync::Mutex` (recursive)            | fixed-capacity pool, placement-new |
| `sys_sem_t`     | `stk::sync::Semaphore` (binary, 0/1)      | fixed-capacity pool, placement-new |
| `sys_mbox_t`    | `stk::sync::MessageQueue`, `msg_size = sizeof(void*)` | fixed-capacity pool (control block + ring buffer, two separate pools) |
| `sys_thread_t`  | small internal `stk::ITask` implementation | fixed-capacity pool (control block + stack, two separate pools) |
| `SYS_LIGHTWEIGHT_PROT` | `stk::hw::CriticalSection::Enter()/Exit()` | n/a |

**Every allocation in this port comes from its own fixed-capacity
`stk::memory::BlockMemoryPool` instances — never from lwIP's
`mem_malloc()`/`mem_free()`.** This is deliberate, not just a style choice:
lwIP's heap (`mem.c`) protects itself with a mutex it creates via
`sys_mutex_new()` the first time `mem_malloc()`/`mem_init()` runs (whenever
`!SYS_LIGHTWEIGHT_PROT`). If `sys_mutex_new()` itself called `mem_malloc()`
to get storage for that very mutex, the first allocation would recurse into
`sys_mutex_new() -> mem_malloc()` before the heap's own protection exists —
a bootstrapping deadlock/failure during `lwip_init()`. Using independent
pools for every primitive breaks that cycle unconditionally, regardless of
your `SYS_LIGHTWEIGHT_PROT` setting.

One practical consequence: this port's allocations are **not** reflected in
lwIP's own `MEM_STATS`/`MEMP_STATS`. Use the `LWIP_STK_*_POOL_SIZE` /
`*_BLOCK_*` macros below to size each pool for your application, and lwIP's
own `SYS_STATS` (mutex/sem/mbox `.used`/`.err` counters) to watch for
exhaustion at runtime.

## Notable implementation choices

- **Pool-allocated, not `mem_malloc()`'d.** See the callout above — every
  `sys_*_new()` in this port draws from its own `stk::memory::BlockMemoryPool`
  via a non-blocking `TryAlloc()` (fails fast with `ERR_MEM`/an assert on
  exhaustion, rather than suspending the caller), never from lwIP's heap.
- **`sys_sem_t` is strictly binary**: `sys_arch.h` defines `SYS_SEM_MAX_COUNT`
  as `1U`, and `sys_sem_new()` constructs the underlying
  `stk::sync::Semaphore` with that ceiling. `sys_sem_signal()` forwards
  straight to the semaphore's `TrySignal()`, which itself absorbs a redundant
  signal on an already-signaled semaphore, so this port doesn't need to track
  signaled/unsignaled state itself.
- **`LWIP_NETCONN_SEM_PER_THREAD`**: STK's `ITask` has no generic user-data/
  TLS slot of its own, so this is implemented as a small, bounded side-table
  keyed by `stk::TId` (size `LWIP_STK_NETCONN_SEM_MAX_THREADS`, default 16).
  Raise that macro if you assert out of slots.
- **`ERR_NEED_SCHED`** is defined for source compatibility but never actually
  returned: STK's switch strategies re-evaluate scheduling as part of
  `Signal()`/`Set()`/`Put()` itself, so there's no separate "please
  reschedule" step for `_fromisr()` variants to report.
- **`LWIP_STK_CUSTOM_CORE_LOCKING`**: when `LWIP_TCPIP_CORE_LOCKING` is on,
  you can opt out of this port's own `stk::sync::Mutex`-backed
  `sys_lock_tcpip_core()`/`sys_unlock_tcpip_core()` and delegate to an
  externally-provided `pico_lwip_custom_lock_tcpip_core()`/
  `..._unlock_tcpip_core()` pair instead — e.g. for an integration (such as
  the Pico SDK `async_context` glue) that needs the tcpip core lock to *be*
  its own lock, rather than an independent mutex layered on top. Ownership of
  that external lock isn't tracked by `sys_check_core_locking()`; only the
  ISR check still applies in that mode.

## Compile-time configuration (define before including, or via build flags)

| Macro | Default | Meaning |
|---|---|---|
| `LWIP_STK_THREAD_STACKSIZE_IS_STACKWORDS` | `1` | Interpret `sys_thread_new()`'s `stacksize` as bytes (0) or `stk::Word`s (1) |
| `LWIP_STK_THREAD_ACCESS_MODE` | `stk::ACCESS_PRIVILEGED` | Access mode for lwIP-spawned tasks |
| `LWIP_STK_SYS_ARCH_PROTECT_SANITY_CHECK` | `0` | Verify `sys_arch_protect`/`unprotect` nest correctly |
| `LWIP_STK_CHECK_QUEUE_EMPTY_ON_FREE` | `0` | Assert mailboxes are drained before `sys_mbox_free()` |
| `LWIP_STK_CHECK_CORE_LOCKING` | `1` | Enable `sys_mark_tcpip_thread()`/`sys_check_core_locking()` |
| `LWIP_STK_CUSTOM_CORE_LOCKING` | `0` | Delegate `sys_lock_tcpip_core()`/`unlock` to external `pico_lwip_custom_*` functions instead of this port's own mutex (only consulted when `LWIP_TCPIP_CORE_LOCKING` is on) |
| `LWIP_STK_SYS_NOW_FROM_STK` | `1` | Implement `sys_now()` via `stk::GetTimeNowMs()` (set 0 to supply your own) |
| `LWIP_STK_NETCONN_SEM_MAX_THREADS` | `16` | Side-table size for `LWIP_NETCONN_SEM_PER_THREAD` |
| `LWIP_STK_MUTEX_POOL_SIZE` | `4` | Max concurrent `sys_mutex_t` instances (`sys_mutex_new()` pool capacity) |
| `LWIP_STK_SEM_POOL_SIZE` | `16` | Max concurrent `sys_sem_t` instances (`sys_sem_new()` pool capacity, includes `LWIP_NETCONN_SEM_PER_THREAD` slots) |
| `LWIP_STK_MBOX_POOL_SIZE` | `8` | Max concurrent `sys_mbox_t` instances (`sys_mbox_new()` pool capacity) |
| `LWIP_STK_MBOX_BUF_BLOCK_BYTES` | `32 * sizeof(void*)` | Shared per-mbox ring-buffer block size in bytes — must cover the largest `capacity * sizeof(void*)` any `sys_mbox_new()` call in your build requests |
| `LWIP_STK_THREAD_POOL_SIZE` | `4` | Max concurrent `sys_thread_t` instances (`sys_thread_new()` pool capacity) |
| `LWIP_STK_THREAD_STACK_BLOCK_WORDS` | `1024` | Shared per-thread stack block size in `stk::Word`s — must cover the largest stack any `sys_thread_new()` call in your build requests |

## Relevant `lwipopts.h` settings

Nothing STK-specific is required beyond the usual lwIP options; this port
supports `LWIP_COMPAT_MUTEX`, `SYS_LIGHTWEIGHT_PROT`,
`LWIP_NETCONN_SEM_PER_THREAD`, `LWIP_TCPIP_CORE_LOCKING`, and `NO_SYS=0`.

`cc.h` is unchanged — it's platform/compiler configuration (assert macro,
section placement for your MCU), not RTOS-specific, so it works as-is with
this port.
