# Raspberry Pi Pico SDK port for SuperTinyKernel RTOS (STK)

STK-native backends for three Pico SDK libraries that normally assume
FreeRTOS or a bare polling loop: `pico_async_context`, `pico_cyw43_arch`,
and `pico_lwip`. Lives at `deps/port/pico` in the STK repo, laid out to
match the Pico SDK's own `<lib>/include/pico/...` convention so it can be
vendored straight into a Pico SDK tree (or pointed to via
`PICO_SDK_EXTRAS_PATH` / an extra `add_subdirectory`) without renaming
anything:

```
deps/port/pico
├── pico_async_context
│   ├── async_context_stk.cpp
│   └── include
│       └── pico
│               async_context_stk.h
│
├── pico_cyw43_arch
│   ├── cyw43_arch_stk.cpp
│   └── include
│       └── pico
│           ├── cyw43_arch_stk.h
│           └── cyw43_arch
│                   arch_stk.h
│
└── pico_lwip
    ├── lwip_stk.cpp
    └── include
        └── pico
                lwip_stk.h
```

**Every `.cpp` file here must be compiled as C++.** STK's API
(`stk::IKernel`, `stk::sync::*`, `stk::time::TimerHost`) is C++-only, so
unlike some of the Pico SDK's other `async_context`/`cyw43_arch` backends
(which are plain C), all three of these are genuinely C++ translation
units — `cyw43_arch_stk.cpp` in particular is a straight rename of the
upstream `cyw43_arch_*.c` file for exactly this reason. Their public
headers (`async_context_stk.h`, `cyw43_arch_stk.h`) are likewise C++-only,
since `async_context_stk_config_t` holds `stk::IKernel*`,
`stk::time::TimerHost*`, `stk::Weight`, and `stk::Word*` members directly.
`lwip_stk.h` is the exception — it's `extern "C"`-wrapped and safe to
include from lwIP's own plain-C sources.

## What each library does

### `pico_async_context` — `async_context_stk`

A task-based `async_context` implementation (`ASYNC_CONTEXT_STK`) that
schedules its worker directly on STK's own scheduler, with no polling
loop. Built entirely on STK primitives:

| Role | STK primitive |
|---|---|
| Context lock (recursive) | `stk::sync::Mutex` |
| Worker wakeup / `execute_sync()` rendezvous / "work pending" signal | three `stk::sync::Semaphore` (binary, `(0,1)`) |
| "Wake at next due time" | `stk::time::TimerHost::Timer` |
| Worker task | `stk::ITask` |

Unlike some other backends, this one does **not** lazily create a private
kernel or timer under the hood — it takes the application's own
already-`Initialize()`'d `stk::IKernel*` and `stk::time::TimerHost*` by
pointer via `async_context_stk_config_t`, so the application controls
scheduling and can share one `TimerHost` across many timers, as STK
intends. STK also has no dynamic task-creation path, so the caller always
owns the worker task's stack storage (`config.task_stack` /
`config.task_stack_size`).

`async_context_stk_t` contains only inline, default-constructed C++
objects (no heap allocation) — **never `memset()` it**; default-construct
it (`static async_context_stk_t ctx;` or equivalent) and pass it to
`async_context_stk_init()`.

### `pico_cyw43_arch` — `cyw43_arch_stk`

Wires the CYW43 Wi-Fi/Bluetooth driver's default `async_context` to
`async_context_stk`, requires `NO_SYS=0`, and additionally boots
`lwip_stk` (if `CYW43_LWIP`) and `btstack_cyw43` (if
`CYW43_ENABLE_BLUETOOTH`) from `cyw43_arch_init()`.

Because `async_context_stk` needs a kernel and `TimerHost` up front and
STK has no global registry to discover them from, call
`cyw43_arch_stk_preinit(kernel, timer_host)` once before `cyw43_arch_init()`
— unless you construct your own `async_context_stk_t` and hand it to
`cyw43_arch_set_async_context()` yourself first, in which case
`cyw43_arch_init()` will use that instead of building its own default
context (`cyw43_arch_stk_preinit()` is then unnecessary).

The worker task's stack is statically allocated
(`CYW43_TASK_STACK_SIZE` words) unless `CYW43_NO_DEFAULT_TASK_STACK` is
set, in which case you must supply `config.task_stack` yourself before
`async_context_stk_init()` runs.

**Call `cyw43_arch_init()` from inside an STK task, not from `main()`
before `kernel.Start()`.** It initializes `async_context_stk`, whose
locking/ownership checks (`async_context_stk_lock_check()`,
`async_context_stk_execute_sync()`, ...) call `stk::GetTid()` to identify
the calling task — which only returns a meaningful value once called from
inside a real, running STK task. `sys_arch_set_kernel()` and
`cyw43_arch_stk_preinit()` have no such restriction (they only stash
pointers), so those still belong in `main()`, before `kernel.Start()`; see
the wiring example below.

### `pico_lwip` — `lwip_stk`

Glue that boots lwIP's `NO_SYS=0` tcpip thread on top of the STK
`sys_arch` port (`sys_arch.h` / `sys_arch.cpp` — a separate component, not
part of this directory) and binds it to an `async_context`. Two
requirements this library depends on, both belonging to the `sys_arch`
port rather than to this directory:

- An `stk::IKernel` must already be registered via `sys_arch_set_kernel()`
  before `lwip_stk_init()` runs — `tcpip_init()` spawns lwIP's tcpip
  thread via `sys_thread_new()`, which needs a kernel to add that task to.
- `sys_arch.cpp` must be built with `LWIP_STK_CUSTOM_CORE_LOCKING=1` (and
  `LWIP_TCPIP_CORE_LOCKING=1`), which makes it delegate
  `sys_lock_tcpip_core()` / `sys_unlock_tcpip_core()` to this library's own
  `pico_lwip_custom_lock_tcpip_core()` / `pico_lwip_custom_unlock_tcpip_core()`
  instead of an independent `stk::sync::Mutex`. That makes lwIP's tcpip
  core lock *be* the async_context's own lock, so code already running
  inside an async_context callback doesn't deadlock re-taking it.

`lwip_stk.cpp` constructs its two long-/short-lived semaphores
(`s_tcpip_task_blocker`, `init_sem`) directly as `stk::sync::Semaphore`
rather than via `sys_sem_new()`/`sys_sem_free()`, since neither's lifetime
(one effectively permanent, one scoped to a single stack frame) fits
`sys_arch.cpp`'s fixed-capacity semaphore pool well; each is still wrapped
in a `sys_sem_t` so the file can reuse `sys_arch.cpp`'s own
`sys_sem_signal()` / `sys_arch_sem_wait()` for the wait/signal logic
itself (including the `timeout_ms == 0` → "wait forever" convention).

## Wiring it together

A typical `NO_SYS=0` CYW43 + lwIP application:

```cpp
#include "stk.h"
#include "sync/stk_sync.h"
#include "time/stk_time.h"
#include "pico/cyw43_arch_stk.h"

using MyKernel = stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC,
                              16 /* max tasks */,
                              stk::SwitchStrategySmoothWeightedRoundRobin,
                              stk::PlatformDefault>;
static MyKernel             g_kernel;
static stk::time::TimerHost g_timer_host;

// cyw43_arch_init() must run from inside a task's Run(), not from main() before
// kernel.Start(): it initializes async_context_stk, whose locking/ownership checks
// call stk::GetTid() to identify the calling task - only meaningful once we're
// actually running as a task under the kernel.
class WifiTask : public stk::Task<4096 /* stack words */, stk::ACCESS_PRIVILEGED>
{
private:
    void Run() override
    {
        if (cyw43_arch_init())
        {
            // handle error
        }
        cyw43_arch_enable_sta_mode();
        // ... connect, sockets, ...
        while (true) { /* ... */ }
    }
};
static WifiTask g_wifi_task;

int main()
{
    g_kernel.Initialize();
    g_timer_host.Initialize(&g_kernel);
    g_kernel.AddTask(&g_wifi_task);

    // sys_arch_set_kernel() / cyw43_arch_stk_preinit() only stash pointers, so - unlike
    // cyw43_arch_init() - they're fine to call here, before kernel.Start().
    sys_arch_set_kernel(&g_kernel);
    cyw43_arch_stk_preinit(&g_kernel, &g_timer_host);

    g_kernel.Start();                        // never returns; runs WifiTask::Run()
}
```

If you're using `pico_async_context`/`pico_lwip` without CYW43, skip
`cyw43_arch_stk_preinit()` and instead build an `async_context_stk_t`
directly with `async_context_stk_init_with_defaults()` (or
`async_context_stk_init()` for full control), then pass that context to
`lwip_stk_init()`.

## Compile-time configuration

| Macro | Default | Meaning |
|---|---|---|
| `ASYNC_CONTEXT_DEFAULT_STK_TASK_WEIGHT` | `stk::DEFAULT_WEIGHT + 4` | Scheduling weight for `async_context_stk`'s worker task when built via `async_context_stk_default_config()`. Meaning depends on the kernel's switch strategy — a fixed priority level for `SwitchStrategyFixedPriority`, a proportional CPU share for `SwitchStrategySmoothWeightedRoundRobin` |
| `ASYNC_CONTEXT_DEFAULT_STK_TASK_STACK_SIZE` | `1024` (words) | Default worker task stack size when built via `async_context_stk_default_config()` |
| `ASYNC_CONTEXT_STK` | `4` | `async_context_type_t` enum value identifying this backend. Fallback definition only — add a proper entry to your tree's `pico/async_context.h` alongside the other backends if you haven't already |
| `CYW43_NO_DEFAULT_TASK_STACK` | `0` | Disable the statically-allocated default CYW43 worker task stack; you must then supply `config.task_stack` yourself |
| `CYW43_TASK_STACK_SIZE` | `1024` (words) | Stack size for the CYW43 worker task |
| `CYW43_TASK_PRIORITY` | `0 + 4` | Scheduling weight (`task_weight`, not a priority in the traditional sense — see the async_context table above) for the CYW43 worker task |

## Requirements summary

- An `stk::IKernel` (`stk::KERNEL_SYNC`, `Initialize()`'d) and an
  `stk::time::TimerHost` (`Initialize()`'d against that same kernel) that
  the application owns and constructs itself — none of these libraries
  create either lazily.
- For `pico_lwip` / `pico_cyw43_arch` with `CYW43_LWIP`: the STK
  `sys_arch` port, with `sys_arch_set_kernel()` called and
  `LWIP_STK_CUSTOM_CORE_LOCKING=1` set.
- `NO_SYS=0` for `pico_cyw43_arch` (`cyw43_arch_stk.cpp` hard-errors
  otherwise) and for `pico_lwip` (`lwip_stk.cpp` hard-errors otherwise).
- A C++ toolchain for every `.cpp` file in this directory, even though
  most of the rest of a typical Pico SDK project stays plain C.
