# STK Source Tree — Code Organization Manual

**Path:** `stk/`

This document is the code organization reference for [SuperTinyKernel™ RTOS](https://github.com/SuperTinyKernel-RTOS) — a lightweight, high-performance, deterministic C++ RTOS for embedded systems.

It covers the layout and purpose of every header and source file under `stk/`, the dependency order between them, and how to locate the right file for a given task.

---

## Table of Contents

- [Directory Overview](#directory-overview)
- [Top-Level Include Headers](#top-level-include-headers)
  - [stk_defs.h — Compiler & Configuration Foundations](#stk_defsh--compiler--configuration-foundations)
  - [stk_linked_list.h — Intrusive List](#stk_linked_listh--intrusive-list)
  - [stk_common.h — Core Interfaces & Types](#stk_commonh--core-interfaces--types)
  - [stk_arch.h — Hardware Abstraction Layer](#stk_archh--hardware-abstraction-layer)
  - [stk_helper.h — User-Facing Task Helpers](#stk_helperh--user-facing-task-helpers)
  - [stk.h — Top-Level Single Include](#stkh--top-level-single-include)
- [include/arch — Architecture Back-Ends](#includearch--architecture-back-ends)
  - [stk_arch_common.h — Shared Platform Context](#stk_arch_commonh--shared-platform-context)
  - [ARM Cortex-M Port](#arm-cortex-m-port)
  - [RISC-V Port](#risc-v-port)
  - [x86 Win32 Port (Simulation)](#x86-win32-port-simulation)
- [include/strategy — Scheduling Strategies](#includestrategy--scheduling-strategies)
- [include/sync — Synchronization Primitives](#includesync--synchronization-primitives)
- [include/memory — Memory Allocation](#includememory--memory-allocation)
- [include/time — Time Utilities](#includetime--time-utilities)
- [src/arch — Architecture Implementations](#srcarch--architecture-implementations)
- [Dependencies](#dependencies)
- [Quick Start](#quick-start)
- [Configuration Reference (stk_config.h)](#configuration-reference-stk_configh)

---

## Directory Overview

```
stk/
├── CMakeLists.txt
├── include/
│   ├── stk.h                     ← Top-level user include (start here)
│   ├── stk_arch.h                ← HAL: arch selection, hw:: namespace
│   ├── stk_common.h              ← Core interfaces, types, kernel modes
│   ├── stk_defs.h                ← Compiler macros, config defaults
│   ├── stk_helper.h              ← Task<>, TaskW<>, free-function helpers
│   ├── stk_linked_list.h         ← Intrusive doubly-linked list (internal)
│   ├── arch/
│   │   ├── stk_arch_common.h     ← PlatformContext base class
│   │   ├── arm/cortex-m/         ← ARM Cortex-M port header
│   │   ├── risc-v/               ← RISC-V port header
│   │   └── x86/win32/            ← Windows simulation port header
│   ├── memory/
│   │   ├── stk_memory.h           ← Umbrella: includes all memory headers
│   │   └── stk_memory_blockpool.h ← Fixed-size block pool allocator
│   ├── strategy/
│   │   ├── stk_strategy_rrobin.h    ← Round-Robin
│   │   ├── stk_strategy_swrrobin.h  ← Smooth Weighted Round-Robin
│   │   ├── stk_strategy_fpriority.h ← Fixed Priority
│   │   ├── stk_strategy_edf.h       ← Earliest Deadline First
│   │   └── stk_strategy_monotonic.h ← Rate/Deadline Monotonic
│   ├── sync/
│   │   ├── stk_sync.h            ← Umbrella: includes all sync headers
│   │   ├── stk_sync_cs.h         ← ScopedCriticalSection
│   │   ├── stk_sync_cv.h         ← ConditionVariable
│   │   ├── stk_sync_mutex.h      ← Mutex (recursive)
│   │   ├── stk_sync_spinlock.h   ← SpinLock (recursive)
│   │   ├── stk_sync_rwmutex.h    ← RWMutex (reader-writer)
│   │   ├── stk_sync_semaphore.h  ← Semaphore (counting)
│   │   ├── stk_sync_event.h      ← Event (binary, auto/manual reset)
│   │   ├── stk_sync_eventflags.h ← EventFlags (32-bit multi-flag)
│   │   ├── stk_sync_pipe.h       ← Pipe<T, N> (typed FIFO)
│   │   └── stk_sync_msgqueue.h   ← MessageQueue / MessageQueueT<N,MSG>
│   └── time/
│       ├── stk_time.h            ← Umbrella: includes all time headers
│       ├── stk_time_util.h       ← PeriodicTrigger
│       └── stk_time_timer.h      ← TimerHost + Timer
└── src/
    └── arch/
        ├── arm/cortex-m/         ← ARM Cortex-M implementation (.cpp)
        ├── risc-v/               ← RISC-V implementation (.cpp)
        └── x86/win32/            ← Windows simulation implementation (.cpp)
```

The `include/` tree is the public API. The `src/` tree contains the architecture-specific `.cpp` files that implement the platform back-ends — one file compiled per target, guarded by `#ifdef _STK_ARCH_*`.

---

## Top-Level Include Headers

The six headers directly inside `include/` form the kernel's foundation layer. They are included in dependency order by `stk.h`.

### stk_defs.h — Compiler & Configuration Foundations

**Included by:** everything (transitively via `stk_common.h`)

This is the lowest-level header. It includes `stk_config.h` first, making all user-supplied configuration visible before any STK definition is processed. It then defines:

- **Compiler portability macros** — `__stk_forceinline`, `__stk_aligned(x)`, `__stk_attr_naked`, `__stk_attr_noreturn`, `__stk_attr_unused`, `__stk_attr_used` for GCC, Clang/LLVM, IAR, and MSVC.
- **Assertion macros** — `STK_ASSERT(cond)` (debug), `STK_STATIC_ASSERT(cond)`, `STK_STATIC_ASSERT_DESC(cond, msg)`.
- **Feature-flag defaults** — `STK_TICKLESS_IDLE` (0), `STK_TICKLESS_USE_ARM_DWT` (1), `STK_TICKLESS_TICKS_MAX` (1000), `STK_ARCH_CPU_COUNT` (1), `STK_SEGGER_SYSVIEW` (0), `STK_SYNC_DEBUG_NAMES` (0).
- **Stack constants** — `STK_STACK_SIZE_MIN` (arch-dependent: 32 on Cortex-M, 256–512+ on RISC-V), `STK_SLEEP_TRAP_STACK_SIZE`, `STK_STACK_MEMORY_ALIGN`, `STK_STACK_MEMORY_FILLER`.
- **Critical section limit** — `STK_CRITICAL_SECTION_NESTINGS_MAX` (16).
- **Utility macros** — `STK_NONCOPYABLE_CLASS(TYPE)`, `STK_UNUSED(X)`, `STK_ALLOCATE_COUNT(MODE,FLAG,ONTRUE,ONFALSE)`, endian index macros `STK_ENDIAN_IDX_HI`/`STK_ENDIAN_IDX_LO`.
- **`stk::Min`/`Max`** — constexpr compile-time min/max templates.

All user configuration belongs in `stk_config.h`, which is picked up automatically by `stk_defs.h`.

---

### `stk_linked_list.h` — Intrusive List

**Used by:** kernel internals only — scheduler lists, sync object wait queues, strategy task lists.

Provides a generic intrusive doubly-linked circular list in `stk::util`. Objects join a list by inheriting the node type rather than being pointed to by a separately allocated node, eliminating all dynamic allocation from the scheduler's hot paths. Not part of the public API.

---

### `stk_common.h` — Core Interfaces & Types

**Included by:** `stk_arch.h` → `stk_helper.h` → `stk.h`

Defines every kernel interface, type alias, and enumeration. Key contents:

**Type aliases:**

| Type      | Underlying  | Purpose                                              |
|-----------|-------------|------------------------------------------------------|
| `Word`    | `uintptr_t` | Native CPU word; used for stack, registers, pointers |
| `TId`     | `Word`      | Task/thread identifier                               |
| `Timeout` | `int32_t`   | Tick-based timeout (`WAIT_INFINITE`, `NO_WAIT`)      |
| `Ticks`   | `int64_t`   | Elapsed tick counter                                 |
| `Cycles`  | `uint64_t`  | High-resolution cycle counter                        |

**Key constants:** `TID_ISR_N` (upper 20-bit ISR sentinel), `TID_NONE` (0), `WAIT_INFINITE` (`INT32_MAX`), `NO_WAIT` (0).

**Enumerations:**

| Enum             | Values                                                                            |
|------------------|-----------------------------------------------------------------------------------|
| `EAccessMode`    | `ACCESS_USER`, `ACCESS_PRIVILEGED`                                                |
| `EKernelMode`    | `KERNEL_STATIC`, `KERNEL_DYNAMIC`, `KERNEL_HRT`, `KERNEL_SYNC`, `KERNEL_TICKLESS` |
| `EKernelPanicId` | 10 panic codes (stack corruption, deadlock, assert, hard fault, etc.)             |
| `EStackType`     | `STACK_USER_TASK`, `STACK_SLEEP_TRAP`, `STACK_EXIT_TRAP`                          |
| `EConsts`        | `PERIODICITY_MAX` (99 000 µs), `PERIODICITY_DEFAULT` (1 000 µs), `STACK_SIZE_MIN` |

**Interfaces defined here:**

- `ITask` — user task contract (`Run`, `GetStack`, `GetStackSize`, `GetWeight`, `GetId`, `OnDeadlineMissed`, `OnExit`, `GetTraceName`).
- `IStackMemory` — stack buffer abstraction.
- `IKernelTask` — kernel-side per-task descriptor (extends `ITask` with scheduling metadata).
- `ITaskSwitchStrategy` — scheduling strategy interface (`AddTask`, `RemoveTask`, `GetNext`, `GetFirst`, `GetSize`, `OnTaskSleep`, `OnTaskWake`, `OnTaskDeadlineMissed`).
- `IPlatform` — platform driver interface (`Initialize`, `Start`, `Stop`, `InitStack`, `SwitchToNext`, `Sleep`, `SleepUntil`, `Wait`, `ProcessTick`, `ProcessHardFault`, `GetTid`, `Suspend`, `Resume`, `GetCallerSP`, etc.).
- `IKernel` — kernel control interface (`Initialize`, `AddTask`, `RemoveTask`, `SuspendTask`, `ResumeTask`, `EnumerateTasks`, `EnumerateTasksT<N>`, `Start`, `GetState`, `GetPlatform`, `GetSwitchStrategy`).
- `IKernelService` — runtime service interface available to running tasks (`GetTid`, `GetTicks`, `GetTickResolution`, `Sleep`, `SleepUntil`, `Delay`, `SwitchToNext`, `Wait`, `Suspend`, `Resume`).
- `ISyncObject`, `IWaitObject`, `IMutex`, `ITraceable` — synchronization building blocks.
- `StackMemoryDef<N>` — stack memory array type helper.

---

### `stk_arch.h` — Hardware Abstraction Layer

**Included by:** `stk_helper.h`

This header performs two jobs. First, it selects exactly one architecture back-end header based on the active `_STK_ARCH_*` macro and sets `_STK_ARCH_DEFINED`:

| Macro                    | Back-end included                           |
|--------------------------|---------------------------------------------|
| `_STK_ARCH_ARM_CORTEX_M` | `arch/arm/cortex-m/stk_arch_arm-cortex-m.h` |
| `_STK_ARCH_RISC_V`       | `arch/risc-v/stk_arch_risc-v.h`             |
| `_STK_ARCH_X86_WIN32`    | `arch/x86/win32/stk_arch_x86-win32.h`       |

Second, it declares and implements the portable `stk::hw` namespace used throughout the kernel:

| Symbol                                         | Description                                                   |
|------------------------------------------------|---------------------------------------------------------------|
| `hw::PtrToWord<T>(ptr)`                        | Cast pointer to `Word` (register-width integer)               |
| `hw::WordToPtr<T>(word)`                       | Cast `Word` back to typed pointer                             |
| `hw::IsInsideISR()`                            | Returns `true` when called from interrupt context             |
| `hw::GetTls()` / `hw::SetTls(word)`            | Read/write raw thread-local storage register                  |
| `hw::GetTlsPtr<T>()` / `hw::SetTlsPtr<T>(ptr)` | Type-safe TLS wrappers                                        |
| `hw::CriticalSection::Enter/Exit`              | Nestable interrupt-disable critical section                   |
| `hw::CriticalSection::ScopedLock`              | RAII wrapper for `CriticalSection`                            |
| `hw::SpinLock::Lock/Unlock/TryLock`            | Hardware atomic spinlock (SMP)                                |
| `hw::HiResClock::GetCycles()`                  | 64-bit cycle counter                                          |
| `hw::HiResClock::GetFrequency()`               | Counter frequency in Hz                                       |
| `hw::HiResClock::GetTimeUs()`                  | Elapsed microseconds                                          |
| `hw::ReadVolatile64<T>(addr)`                  | Lock-free 64-bit volatile read (hi-lo retry on 32-bit)        |
| `hw::WriteVolatile64<T>(addr, val)`            | Lock-free 64-bit volatile write (hi before lo)                |
| `STK_KERNEL_PANIC(id)`                         | Trigger hardware breakpoint then call `STK_PANIC_HANDLER(id)` |

`ISyncObject::Tick()` is also implemented here because it requires the `hw::CriticalSection` SMP guard.

---

### `stk_helper.h` — User-Facing Task Helpers

**Included by:** `stk.h`

Provides the concrete building blocks that application code uses directly:

**`Task<StackSize, AccessMode>`** — The standard base class for user tasks. Owns its stack memory array (16-byte aligned). Provides default no-op `OnDeadlineMissed`, `OnExit`, `GetWeight` (returns 1), `GetId` (returns `this` address), `GetTraceName` (returns `nullptr`). Override `Run()` (pure virtual from `ITask`) to implement task logic.

```cpp
template <stk::EAccessMode Mode>
class MyTask : public stk::Task<256, Mode>
{
    void Run() override
    {
        while (true) { /* work */ }
    }
};
```

**`TaskW<Weight, StackSize, AccessMode>`** — Variant of `Task` with a compile-time scheduling weight for use with `SwitchStrategySWRR`. Incompatible with `KERNEL_HRT`.

**`StackMemoryWrapper<StackSize>`** — Adapter that wraps an externally-owned stack array (e.g. placed in a specific linker section) as an `IStackMemory` for passing to the kernel.

**Free functions** (all delegate to `IKernelService::GetInstance()`):

| Function                     | ISR-safe  | Description                              |
|------------------------------|-----------|------------------------------------------|
| `GetTid()`                   | Yes       | Task identifier of the calling task      |
| `GetTicks()`                 | Yes       | Ticks elapsed since kernel start         |
| `GetTickResolution()`        | Yes       | Microseconds per tick                    |
| `GetTicksFromMs(ms)`         | Yes       | Convert ms → ticks (queries resolution)  |
| `GetTicksFromMs(ms, res)`    | Yes       | Convert ms → ticks (explicit resolution) |
| `GetMsFromTicks(ticks, res)` | Yes       | Convert ticks → ms                       |
| `GetTimeNowMs()`             | Yes       | Milliseconds since kernel start          |
| `GetSysTimerCount()`         | Yes       | Raw hardware timer counter value         |
| `GetSysTimerFrequency()`     | Yes       | Hardware timer frequency (Hz)            |
| `Sleep(ticks)`               | No        | Suspend calling task for N ticks         |
| `SleepMs(ms)`                | No        | Suspend calling task for N milliseconds  |
| `SleepUntil(timestamp)`      | No        | Suspend until absolute tick timestamp    |
| `Yield()`                    | No        | Cooperatively yield to the next task     |
| `Delay(ticks)`               | No        | Busy-wait for N ticks (spins, no sleep)  |
| `DelayMs(ms)`                | No        | Busy-wait for N milliseconds             |

---

### `stk.h` — Top-Level Single Include

**This is the only header most application code needs to include.**

It pulls in `stk_helper.h` and all five strategy headers, then defines the `Kernel` class template.

**`Kernel<TMode, TSize, TStrategy, TPlatform>`:**

| Parameter   | Description                                         |
|-------------|-----------------------------------------------------|
| `TMode`     | Bitmask of `EKernelMode` flags (see table below)    |
| `TSize`     | Maximum concurrent tasks (must be > 0)              |
| `TStrategy` | Scheduling strategy class (e.g. `SwitchStrategyRR`) |
| `TPlatform` | Platform driver class (e.g. `PlatformDefault`)      |

**`TMode` flag combinations:**

| Flag              | Meaning                                                                                                                                                                    |
|-------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `KERNEL_STATIC`   | Task list fixed before `Start()`; tasks cannot exit                                                                                                                        |
| `KERNEL_DYNAMIC`  | Tasks may be added/removed at runtime; exit is allowed                                                                                                                     |
| `KERNEL_HRT`      | Hard Real-Time mode — tasks have deadlines and periods. Must combine with `KERNEL_STATIC` or `KERNEL_DYNAMIC`. Incompatible with `KERNEL_TICKLESS` and weighted strategies |
| `KERNEL_SYNC`     | Enables `sync::` primitives (Mutex, Event, Semaphore, etc.)                                                                                                                |
| `KERNEL_TICKLESS` | Low-power tickless idle. Requires `STK_TICKLESS_IDLE=1` in `stk_config.h`. Incompatible with `KERNEL_HRT`                                                                  |

All illegal combinations are caught at compile time by `STK_STATIC_ASSERT`.

**Typical usage:**

```cpp
#include <stk.h>

// Define tasks
class Task1 : public stk::Task<512, stk::ACCESS_USER>
{
    void Run() override { while (true) { stk::SleepMs(100); } }
};

// Instantiate kernel with Round-Robin, 3 tasks, default platform
static stk::Kernel<stk::KERNEL_STATIC | stk::KERNEL_SYNC,
                   3,
                   stk::SwitchStrategyRR,
                   stk::PlatformDefault> kernel;

static Task1 task1, task2, task3;

int main()
{
    kernel.Initialize();
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);
    kernel.Start(); // never returns
}
```

---

## `include/arch` — Architecture Back-Ends

### `stk_arch_common.h` — Shared Platform Context

Defines `PlatformContext` — the base class inherited by every concrete platform class. Contains the fields shared by all platforms: `m_handler` (kernel event handler), `m_service` (kernel service), `m_stack_idle`, `m_stack_active`, `m_tick_resolution`. Also defines the `GetContext()` macro (SMP-indexed per-CPU context access) and `ConvertTimeUsToClockCycles()`.

---

### ARM Cortex-M Port

**Headers:** `arch/arm/cortex-m/stk_arch_arm-cortex-m.h`  
**Implementation:** `src/arch/arm/cortex-m/stk_arch_arm-cortex-m.cpp`  
**Enable with:** `#define _STK_ARCH_ARM_CORTEX_M` in `stk_config.h`

**Supported cores:**

| Core                 | ISA                | FPU      | DWT  | Privilege  | TrustZone  |
|----------------------|--------------------|----------|------|------------|------------|
| Cortex-M0 / M0+ / M1 | ARMv6-M            | No       | No   | No         | No         |
| Cortex-M3            | ARMv7-M            | No       | Yes  | Yes        | No         |
| Cortex-M4            | ARMv7-M            | Optional | Yes  | Yes        | No         |
| Cortex-M7            | ARMv7-M            | Optional | Yes  | Yes        | No         |
| Cortex-M23           | ARMv8-M Baseline   | No       | No   | Yes        | Optional   |
| Cortex-M33           | ARMv8-M Mainline   | Optional | Yes  | Yes        | Optional   |
| Cortex-M35P          | ARMv8-M Mainline   | Optional | Yes  | Yes        | Optional   |
| Cortex-M52           | ARMv8.1-M Mainline | Optional | Yes  | Yes        | Optional   |
| Cortex-M55           | ARMv8.1-M Mainline | Optional | Yes  | Yes        | Optional   |
| Cortex-M85           | ARMv8.1-M Mainline | Optional | Yes  | Yes        | Optional   |

Context switching uses **SysTick** (tick source) → **PendSV** (lowest-priority, deferred context switch) → **SVC** (scheduler start, privileged critical section entry/exit). The core variant is auto-detected from the CMSIS `__CORTEX_M` macro.

**Provides:** `PlatformArmCortexM` class, `typedef PlatformArmCortexM PlatformDefault`, TLS via `r9`, `__stk_dmb()` = `dmb sy`.

**Key configuration macros** (set in `stk_config.h` before including STK):

| Macro                      | Default           | Description                                     |
|----------------------------|-------------------|-------------------------------------------------|
| `STK_SYSTICK_HANDLER`      | `SysTick_Handler` | SysTick ISR name                                |
| `STK_PENDSV_HANDLER`       | `PendSV_Handler`  | PendSV ISR name                                 |
| `STK_SVC_HANDLER`          | `SVC_Handler`     | SVC ISR name                                    |
| `STK_TICKLESS_IDLE`        | `0`               | Enable tickless low-power idle                  |
| `STK_TICKLESS_USE_ARM_DWT` | `1`               | Use DWT for tickless drift correction (M3+)     |
| `STK_ARCH_CPU_COUNT`       | `1`               | Number of cores (2 for RP2040/RP2350 dual-core) |
| `STK_ARCH_GET_CPU_ID()`    | `0`               | Expression returning current core index         |

`SystemCoreClock` must be set to the correct CPU frequency before `Initialize()` is called.

> See `include/arch/arm/cortex-m/README.md` for the full configuration reference, ISR override examples, platform-specific configs (RP2350, RP2040, STM32), and an FAQ.

---

### RISC-V Port

**Headers:** `arch/risc-v/stk_arch_risc-v.h`  
**Implementation:** `src/arch/risc-v/stk_arch_risc-v.cpp`  
**Enable with:** `#define _STK_ARCH_RISC_V` in `stk_config.h`

Supports RV32I, RV32E, RV64I, with optional F/D FPU extensions. Context switching uses the **CLINT MTIMER** as the tick source and machine-mode exception/interrupt handlers. An optional `_STK_RISCV_USE_PENDSV` mode emulates ARM-style deferred context switching using the MSIP (Machine Software Interrupt) for platforms with a preemptible interrupt controller (CLIC); for standard M-mode the single-handler path is preferred.

**Provides:** `PlatformRiscV` class with `ISpecificEventHandler` for custom exception callbacks, `typedef PlatformRiscV PlatformDefault`, TLS via `tp` (x4) register, `__stk_dmb()` = `fence rw,rw`.

**Key configuration macros:**

| Macro                               | Default                 | Description                          |
|-------------------------------------|-------------------------|--------------------------------------|
| `STK_SYSTICK_HANDLER`               | `riscv_mtvec_mti`       | Machine timer interrupt handler name |
| `STK_SVC_HANDLER`                   | `riscv_mtvec_exception` | Machine exception handler name       |
| `STK_RISCV_CLINT_BASE_ADDR`         | `0x2000000`             | CLINT base address                   |
| `STK_TIMER_CLOCK_FREQUENCY`         | `1 000 000`             | MTIME clock frequency (Hz)           |
| `STK_RISCV_CLINT_MTIMECMP_PER_HART` | `1`                     | Per-hart MTIMECMP (set 0 for shared) |
| `STK_ARCH_GET_CPU_ID()`             | `read_csr(mhartid)`     | Hart ID expression                   |
| `STK_SYSTEM_CORE_CLOCK_FREQUENCY`   | `150 000 000`           | CPU frequency (Hz)                   |

> See `include/arch/risc-v/README.md` for full porting details.

---

### x86 Win32 Port (Simulation)

**Headers:** `arch/x86/win32/stk_arch_x86-win32.h`  
**Implementation:** `src/arch/x86/win32/stk_arch_x86-win32.cpp`  
**Enable with:** `#define _STK_ARCH_X86_WIN32` in `stk_config.h`

This port simulates the RTOS on Windows for development and testing. Each task maps to a Win32 thread (`CreateThread`). The scheduler tick is driven by a dedicated timer thread using `timeBeginPeriod(1)`. `Suspend`/`Resume` tickless mode is not supported on this platform.

**Provides:** `PlatformX86Win32` class, `typedef PlatformX86Win32 PlatformDefault`, `__stk_dmb()` = `_mm_mfence()` (MSVC) / `__sync_synchronize()` (GCC/Clang). TLS uses Win32 `TlsAlloc`/`TlsGetValue`/`TlsSetValue`.

> See `include/arch/x86/win32/README.md` for further details.

---

## `include/strategy` — Scheduling Strategies

> **Full reference:** [`include/strategy/README.md`](include/strategy/README.md)

All strategies are **header-only** classes implementing `ITaskSwitchStrategy`. The `Kernel` is templated on the strategy type — selection happens at compile time with zero runtime overhead. Each strategy maintains two intrusive lists: `m_tasks` (runnable) and `m_sleep` (blocked), and reports two capability flags (`WEIGHT_API`, `SLEEP_EVENT_API`) as compile-time `enum EConfig` values.

**Available strategies at a glance:**

| Header                     | Class / Alias                           | `WEIGHT_API` | `SLEEP_EVENT_API` | Requires     | License       |
|----------------------------|-----------------------------------------|:------------:|:-----------------:|--------------|---------------|
| `stk_strategy_rrobin.h`    | `SwitchStrategyRR`                      |      0       |         1         | —            | MIT           |
| `stk_strategy_swrrobin.h`  | `SwitchStrategySWRR`                    |      1       |         1         | —            | MIT           |
| `stk_strategy_fpriority.h` | `SwitchStrategyFP32`                    |      1       |         1         | —            | MIT           |
| `stk_strategy_edf.h`       | `SwitchStrategyEDF`                     |      0       |         1         | `KERNEL_HRT` | MIT           |
| `stk_strategy_monotonic.h` | `SwitchStrategyRM` / `SwitchStrategyDM` |      0       |         0         | `KERNEL_HRT` | MIT           |
| `stk_strategy_mcas.h`      | `SwitchStrategyMCAS`                    |      —       |         —         | —            | 🔒 Commercial |
| `stk_strategy_mcas4.h`     | `SwitchStrategyMCAS4`                   |      —       |         —         | —            | 🔒 Commercial |

**Quick selection guide:**

```
Is CPU time equal for all tasks?
├── Yes → SwitchStrategyRR
└── No
    ├── Static weights, proportional fairness  → SwitchStrategySWRR
    ├── Hard priorities, classic RTOS model    → SwitchStrategyFP32
    ├── Periodic tasks, formal guarantees      → SwitchStrategyRM / SwitchStrategyDM
    ├── Dynamic deadlines, max utilisation     → SwitchStrategyEDF
    ├── Mixed-criticality (2-level)            → SwitchStrategyMCAS   [commercial]
    └── Mixed-criticality (4-level)            → SwitchStrategyMCAS4  [commercial]
```

For algorithm details, capability flag semantics, `KERNEL_HRT` requirements, offline schedulability analysis (`SchedulabilityCheck::IsSchedulableWCRT()`), and commercial licensing, see [`include/strategy/README.md`](include/strategy/README.md).

---

## `include/sync` — Synchronization Primitives

> **Full reference:** [`include/sync/README.md`](include/sync/README.md)

Include the umbrella header to access all primitives:

```cpp
#include <sync/stk_sync.h>
```

Requires `KERNEL_SYNC` in the kernel mode bitmask for most primitives. `ScopedCriticalSection` is always available regardless of kernel mode.

**Primitives at a glance:**

| Class                         | Header                  | Description                                                                                |
|-------------------------------|-------------------------|--------------------------------------------------------------------------------------------|
| `sync::ScopedCriticalSection` | `stk_sync_cs.h`         | RAII interrupt-disable critical section. Always available, ISR-safe                        |
| `sync::ConditionVariable`     | `stk_sync_cv.h`         | Monitor-style wait/notify. Foundation for Pipe, MessageQueue, EventFlags, BlockMemoryPool  |
| `sync::Mutex`                 | `stk_sync_mutex.h`      | Recursive mutex. `Lock`, `TryLock`, `TimedLock`, `Unlock`. FIFO direct handover on unlock  |
| `sync::SpinLock`              | `stk_sync_spinlock.h`   | Recursive spinlock for ultra-short sections. ISR-unsafe                                    |
| `sync::RWMutex`               | `stk_sync_rwmutex.h`    | Non-recursive reader-writer lock with writer-preference policy. RAII guards included       |
| `sync::Semaphore`             | `stk_sync_semaphore.h`  | Counting semaphore with direct handover on `Signal()`                                      |
| `sync::Event`                 | `stk_sync_event.h`      | Binary signal. Auto-reset or manual-reset. `Set`, `Reset`, `Wait`, `TryWait`, `Pulse`      |
| `sync::EventFlags`            | `stk_sync_eventflags.h` | 32-bit multi-flag group. `OPT_WAIT_ANY` / `OPT_WAIT_ALL` / `OPT_NO_CLEAR`. Error sentinels |
| `sync::Pipe<T, N>`            | `stk_sync_pipe.h`       | Typed FIFO ring-buffer. Single-element and bulk read/write. `memcpy` fast-path for scalars |
| `sync::MessageQueue`          | `stk_sync_msgqueue.h`   | Opaque-message FIFO over external buffer. `Put/Get`, `TryPut/TryGet`, `Reset`              |
| `sync::MessageQueueT<N, MSG>` | `stk_sync_msgqueue.h`   | `MessageQueue` with compile-time capacity and internal storage                             |

**ISR safety at a glance:**

| Primitive                        | ISR-safe operations                                               |
|----------------------------------|-------------------------------------------------------------------|
| `ScopedCriticalSection`          | All                                                               |
| `Event`                          | `Set()`, `Pulse()`, `Reset()`, `TryWait()`                        |
| `EventFlags`                     | `Set()`, `Clear()`, `Get()`, `TryWait()`, `Wait(NO_WAIT)`         |
| `Semaphore`                      | `Signal()`, `TryWait()`                                           |
| `ConditionVariable`              | `NotifyOne()`, `NotifyAll()`, `Wait(NO_WAIT)`                     |
| `Pipe`                           | All `Try*` and `NO_WAIT` variants                                 |
| `MessageQueue`                   | `Put(NO_WAIT)`, `TryPut()`, `Get(NO_WAIT)`, `TryGet()`, `Reset()` |
| `SpinLock` / `Mutex` / `RWMutex` | None                                                              |

> Calling a blocking method from an ISR is undefined behaviour. In debug builds `STK_ASSERT` halts execution if an ISR-unsafe method is called from interrupt context.

For full API reference, C API (`stk_c.h`), C++ and C usage examples, and Eclipse project links, see [`include/sync/README.md`](include/sync/README.md).

---

## `include/memory` — Memory Allocation

> **Full reference:** [`include/memory/README.md`](include/memory/README.md)

Include the umbrella header:

```cpp
#include <memory/stk_memory.h>
```

**`memory::BlockMemoryPool`** provides O(1) alloc/free via an intrusive singly-linked free-list with zero heap fragmentation. Two storage modes are available: caller-supplied static buffer (zero heap, preferred for embedded) or heap-owned buffer (check `IsStorageValid()` after construction when exceptions are disabled).

**Key operations:**

| Method                          | Blocking            | ISR-safe  |
|---------------------------------|---------------------|-----------|
| `TryAlloc()` / `TryAllocT<T>()` | No                  | Yes       |
| `Free(ptr)`                     | No                  | Yes       |
| `TimedAlloc(NO_WAIT)`           | No                  | Yes       |
| `Alloc()` / `AllocT<T>()`       | Yes — indefinite    | No        |
| `TimedAlloc(timeout > 0)`       | Yes — up to timeout | No        |

Blocking paths (`Alloc`, `TimedAlloc` with non-zero timeout) require `KERNEL_SYNC`. `TryAlloc` and `Free` are always available.

```cpp
// External static storage (zero heap)
alignas(sizeof(void*)) static uint8_t
    g_Buf[N * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(MyType))];
stk::memory::BlockMemoryPool pool(N, sizeof(MyType), g_Buf, sizeof(g_Buf));
```

For constructor signatures, typed API (`AllocT<T>`, `TryAllocT<T>`), utility/query methods, ISR safety table, and usage examples (ISR receiver, typed blocking alloc, timed alloc), see [`include/memory/README.md`](include/memory/README.md).

---

## `include/time` — Time Utilities

> **Full reference:** [`include/time/README.md`](include/time/README.md)

Include the umbrella header:

```cpp
#include <time/stk_time.h>
```

Two classes cover the full range of timing needs:

**`time::PeriodicTrigger`** (`stk_time_util.h`) — Lightweight polling trigger requiring no kernel involvement. Stores an absolute next-fire tick and advances it by the configured period on each `true` return, keeping the long-term rate stable regardless of call jitter. Not thread-safe; intended for a single task or ISR.

```cpp
stk::time::PeriodicTrigger trigger(stk::GetTicksFromMs(500), /*start=*/true);
// Inside task loop:
if (trigger.Poll()) { ReadSensor(); }
```

**`time::TimerHost` + `time::TimerHost::Timer`** (`stk_time_timer.h`) — Kernel-backed software timer multiplexer. Fixed overhead of one tick task plus N handler tasks (`STK_TIMER_THREADS_COUNT`, default 1) regardless of how many timers are active. Subclass `Timer` and override `OnExpired()`. Key commands: `Start`, `Stop`, `Reset`, `Restart` (atomic stop+start), `StartOrReset` (TOCTOU-safe), `SetPeriod`, `Shutdown`.

**When to use which:**

| Criterion           |   `PeriodicTrigger`    |       `TimerHost::Timer`       |
|---------------------|:----------------------:|:------------------------------:|
| Kernel required     |           No           |              Yes               |
| Task overhead       |     None (polling)     |        1+ shared tasks         |
| Timers per instance |           1            |              Many              |
| Callback model      | Inline in calling task |     Separate handler task      |
| ISR-safe            |  Yes (single context)  | Command API: task context only |

For full API tables, watchdog/debounce/runtime-period-change examples, and configuration macros (`STK_TIMER_THREADS_COUNT`, `STK_TIMER_HANDLER_STACK_SIZE`, `STK_TIMER_COUNT_MAX`), see [`include/time/README.md`](include/time/README.md).

---

## `src/arch` — Architecture Implementations

Each file under `src/arch/` implements the `Platform*` class declared in the corresponding header under `include/arch/`. All files are guarded by `#ifdef _STK_ARCH_*` so only the selected back-end is compiled.

| File                                              | Class implemented    | Guard                    |
|---------------------------------------------------|----------------------|--------------------------|
| `src/arch/arm/cortex-m/stk_arch_arm-cortex-m.cpp` | `PlatformArmCortexM` | `_STK_ARCH_ARM_CORTEX_M` |
| `src/arch/risc-v/stk_arch_risc-v.cpp`             | `PlatformRiscV`      | `_STK_ARCH_RISC_V`       |
| `src/arch/x86/win32/stk_arch_x86-win32.cpp`       | `PlatformX86Win32`   | `_STK_ARCH_X86_WIN32`    |

Every implementation file begins with:

```cpp
#include "stk_config.h"   // must be customized and copied to /include by the user
#ifdef _STK_ARCH_*
#include "stk_arch.h"
#include "arch/stk_arch_common.h"
```

The `stk_config.h` note is a reminder: the config file lives in the repository root and must be copied to `include/` and customized for your target before building.

All three files implement the same `IPlatform` interface — `Initialize`, `Start`, `Stop`, `InitStack`, `SwitchToNext`, `Sleep`, `SleepUntil`, `Wait`, `ProcessTick`, `ProcessHardFault`, `GetTid`, `GetCallerSP`, `Suspend`, `Resume` — and the `hw::` namespace definitions (`CriticalSection::Enter/Exit`, `SpinLock::Lock/Unlock/TryLock`, `IsInsideISR`, `HiResClock::GetCycles/GetFrequency`).

---

## Dependencies

```
User application code
        │
        ▼
    stk.h  ◄────────────────────────────────────────────────────────┐
        │                                                           │
        ├── stk_helper.h  (Task<>, TaskW<>, free functions)         │
        │       └── stk_arch.h  (hw:: namespace, arch selection)    │
        │               └── stk_common.h  (IKernel, ITask, etc.)    │
        │                       └── stk_defs.h  (macros, stk_config.h)
        │
        ├── strategy/stk_strategy_*.h  (compile-time strategy selection)
        │
        └── Kernel<TMode, TSize, TStrategy, TPlatform>
                │
                ├── TStrategy  (one of the stk_strategy_*.h classes)
                │
                └── TPlatform  (PlatformDefault from the active arch header)
                        │
                        └── src/arch/**/*.cpp  (compiled once per build)

Optional modules (included independently by user code):
    sync/stk_sync.h      → 10 synchronization primitives
    memory/stk_memory.h  → BlockMemoryPool
    time/stk_time.h      → PeriodicTrigger, TimerHost
```

The kernel itself allocates no heap. All storage — `KernelTask` slots, the sleep trap stack, the optional exit trap stack, the sync object list, and the strategy and platform objects — lives inside the `Kernel` template instance, which is typically declared as a global static.

---

## Quick Start

**Step 1 — Copy and customize `stk_config.h`**

Get the template from the repository root, place it in `include/`, and set your architecture macro:

```cpp
// stk_config.h
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <your_device.h>       // provides SystemCoreClock, __CORTEX_M, SysTick, etc.

#define _STK_ARCH_ARM_CORTEX_M // or _STK_ARCH_RISC_V / _STK_ARCH_X86_WIN32

// Optional overrides:
// #define STK_SYSTICK_HANDLER  isr_systick   // override if SDK uses non-CMSIS names
// #define STK_PENDSV_HANDLER   isr_pendsv
// #define STK_SVC_HANDLER      isr_svcall

#endif
```

**Step 2 — Define your tasks**

```cpp
#include <stk.h>
#include <sync/stk_sync.h>

stk::sync::Mutex g_Mutex;

class SensorTask : public stk::Task<512, stk::ACCESS_USER>
{
    void Run() override
    {
        while (true)
        {
            g_Mutex.Lock();
            ReadSensor();
            g_Mutex.Unlock();
            stk::SleepMs(100);
        }
    }
};
```

**Step 3 — Instantiate kernel and start**

```cpp
static stk::Kernel<stk::KERNEL_STATIC | stk::KERNEL_SYNC,
                   2,
                   stk::SwitchStrategyRR,
                   stk::PlatformDefault> kernel;

static SensorTask sensor;
static LogTask    logger;

int main()
{
    SystemCoreClockUpdate(); // ARM: must be called before Initialize()

    kernel.Initialize();
    kernel.AddTask(&sensor);
    kernel.AddTask(&logger);
    kernel.Start();          // never returns
}
```

---

## Configuration Reference (stk_config.h)

All macros below can be defined in `stk_config.h`. Macros marked **required** must be set; all others have the shown defaults.

| Macro                               | Default              | Description                                                 |
|-------------------------------------|:--------------------:|-------------------------------------------------------------|
| `_STK_ARCH_ARM_CORTEX_M`            | —                    | **Required (one arch)** — select ARM Cortex-M port          |
| `_STK_ARCH_RISC_V`                  | —                    | **Required (one arch)** — select RISC-V port                |
| `_STK_ARCH_X86_WIN32`               | —                    | **Required (one arch)** — select x86 Win32 simulation port  |
| `STK_TICKLESS_IDLE`                 | `0`                  | `1` = enable tickless low-power idle                        |
| `STK_TICKLESS_USE_ARM_DWT`          | `1`                  | `1` = use DWT for tickless drift correction (Cortex-M3+)    |
| `STK_TICKLESS_TICKS_MAX`            | `1000`               | Maximum ticks the timer may be suppressed per idle interval |
| `STK_ARCH_CPU_COUNT`                | `1`                  | Number of CPU cores (SMP)                                   |
| `STK_ARCH_GET_CPU_ID()`             | `0`                  | Expression returning the calling core's index               |
| `STK_STACK_SIZE_MIN`                | arch-dependent       | Minimum stack size in `Word` elements                       |
| `STK_SLEEP_TRAP_STACK_SIZE`         | `STK_STACK_SIZE_MIN` | Sleep trap stack size                                       |
| `STK_STACK_MEMORY_ALIGN`            | arch-dependent       | Required stack buffer alignment (bytes)                     |
| `STK_CRITICAL_SECTION_NESTINGS_MAX` | `16`                 | Maximum critical section nesting depth                      |
| `STK_SEGGER_SYSVIEW`                | `0`                  | `1` = enable SEGGER SystemView tracing                      |
| `STK_SYNC_DEBUG_NAMES`              | `0`                  | `1` = enable debug names for sync objects                   |
| `STK_SYSTICK_HANDLER`               | `SysTick_Handler`    | ARM: SysTick ISR symbol name                                |
| `STK_PENDSV_HANDLER`                | `PendSV_Handler`     | ARM: PendSV ISR symbol name                                 |
| `STK_SVC_HANDLER`                   | `SVC_Handler`        | ARM/RISC-V: SVC/exception handler symbol name               |
| `STK_TIMER_THREADS_COUNT`           | `1`                  | TimerHost: number of callback handler tasks                 |
| `STK_TIMER_HANDLER_STACK_SIZE`      | `256`                | TimerHost: handler task stack size (words)                  |
| `STK_TIMER_COUNT_MAX`               | `32`                 | TimerHost: max concurrently active timers                   |
| `STK_PANIC_HANDLER(id)`             | spin loop            | Override with a platform-specific fault handler             |
