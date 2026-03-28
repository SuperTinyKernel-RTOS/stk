# STK x86 Win32 Simulation Port

**SuperTinyKernel RTOS** — Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.

This document is the reference for the x86 Win32 simulation port of STK (`stk\src\arch\x86\win32`). This port is **not intended for production deployment** — it exists to let developers run, test, and debug STK-based application logic on a Windows PC before deploying to an embedded target.

---

## Table of Contents

- [Overview](#overview)
- [Supported Toolchains](#supported-toolchains)
- [How the Simulation Works](#how-the-simulation-works)
- [Minimum Requirements](#minimum-requirements)
- [Quick Start](#quick-start)
- [Configuration Reference](#configuration-reference)
  - [Architecture Enable](#architecture-enable)
  - [Tick Resolution](#tick-resolution)
  - [Kernel Tuning](#kernel-tuning)
- [Timing Behaviour and Limitations](#timing-behaviour-and-limitations)
- [High-Resolution Clock](#high-resolution-clock)
- [Critical Sections and Spin-Locks](#critical-sections-and-spin-locks)
- [Thread-Local Storage (TLS)](#thread-local-storage-tls)
- [Stack Allocation](#stack-allocation)
- [Features Not Available in Simulation](#features-not-available-in-simulation)
- [Project Setup](#project-setup)
  - [MSVC (Visual Studio)](#msvc-visual-studio)
  - [MinGW / GCC](#mingw--gcc)
  - [Clang (LLVM for Windows)](#clang-llvm-for-windows)
- [Frequently Asked Questions](#frequently-asked-questions)

---

## Overview

The Win32 port maps the STK kernel API onto native Windows OS primitives:

- Each STK task becomes a **Windows thread** (`CreateThread`), created suspended and resumed/suspended by the scheduler.
- The scheduler tick is driven by a dedicated **timer thread** running at `THREAD_PRIORITY_TIME_CRITICAL`, which fires at the configured tick interval using `WaitForSingleObject` with a millisecond timeout.
- Context switching is implemented by **suspending the outgoing task's thread** (`SuspendThread`) and **resuming the incoming task's thread** (`ResumeThread`).
- Critical sections use a **Windows `CRITICAL_SECTION`** object combined with suspend/resume of the timer thread to block preemption.
- High-resolution time is provided by **`QueryPerformanceCounter`** (QPC).
- Thread-local storage is backed by a **Windows TLS slot** (`TlsAlloc` / `TlsGetValue` / `TlsSetValue`).

This design means the full STK task model — task creation, sleeping, waiting on synchronization objects, critical sections, and the `HiResClock` API — all work correctly in simulation with the same application code that runs on embedded targets.

---

## Supported Toolchains

| Toolchain | Compiler | Notes |
|-----------|----------|-------|
| Visual Studio 2019 / 2022 | MSVC (`cl.exe`) | Primary development environment. x86 and x64 both supported. |
| MinGW-w64 | GCC | `g++ -std=c++11` or higher. Requires linking `-lwinmm` if not auto-linked. |
| Clang for Windows (LLVM) | Clang/LLVM | Both MSVC-compatible (`clang-cl`) and GCC-compatible (`clang++`) modes work. |

C++11 or later is required. The port uses `std::list` and `std::vector` from the standard library.

---

## How the Simulation Works

Understanding the simulation model helps when interpreting timing behaviour and debugging.

### Task model

Each task is a real Windows thread created with `CreateThread` at `Initialize()` / `InitStack()` time, with the stack size set to the task's configured stack size. All task threads start in the **suspended** state. The scheduler activates the first task by calling `ResumeThread` on it at `Start()`.

### Tick and context switching

A dedicated **timer thread** is created at `THREAD_PRIORITY_TIME_CRITICAL` when `Start()` is called. It loops on `WaitForSingleObject` with a `wait_ms` timeout derived from the tick resolution. On each timeout it calls `ProcessTick()`, which:

1. Acquires the Win32 `CRITICAL_SECTION`.
2. Calls the STK scheduler (`OnTick`).
3. If the scheduler selects a new active task, calls `SwitchContext()`:
   - **Suspends** the outgoing task's thread (`SuspendThread`).
   - **Resumes** the incoming task's thread (`ResumeThread`).

`Start()` blocks (joins) until all task threads have exited, then cleans up all thread handles.

### Winmm.dll and timer resolution

On initialization, STK dynamically loads `Winmm.dll` and calls `timeBeginPeriod(1)` to request 1 ms timer resolution from the Windows multimedia timer system. This reduces the default 15 ms timer granularity to approximately 1 ms, making the simulation tick more responsive. The DLL is unloaded when the context is destroyed.

---

## Minimum Requirements

- Windows 7 or later (Win32 API: `CreateThread`, `SuspendThread`, `ResumeThread`, `WaitForSingleObject`, `QueryPerformanceCounter`, `TlsAlloc`)
- C++11 compatible compiler (MSVC, GCC, or Clang)
- No additional libraries required — all Win32 APIs used are in `kernel32.dll` and `winmm.dll` (loaded dynamically)

---

## Quick Start

Create `stk_config.h` in your project — this is the entire configuration needed for the Win32 simulation port:

```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#define _STK_ARCH_X86_WIN32

#endif /* STK_CONFIG_H_ */
```

That is all. There are no platform-specific addresses, ISR handler names, or clock defines to set. Everything is handled automatically using Windows OS APIs.

Your application code then compiles and runs on Windows identically to how it will run on the embedded target, subject to the [timing limitations](#timing-behaviour-and-limitations) described below.

---

## Configuration Reference

### Architecture Enable

| Define | Description |
|--------|-------------|
| `_STK_ARCH_X86_WIN32` | **Required.** Selects the Win32 simulation port. Must be defined in `stk_config.h`. |

---

### Tick Resolution

The tick resolution is passed to `PlatformX86Win32::Initialize()` as the `resolution_us` parameter, the same as on embedded targets. However, Windows imposes a hard floor:

| Behaviour | Details |
|-----------|---------|
| Minimum tick resolution | **1000 µs (1 ms).** If a smaller value is requested (e.g. `resolution_us = 500`), it is silently clamped to 1000 µs inside `ConfigureTime()`. |
| Typical accuracy | ±1–2 ms jitter due to Windows scheduler granularity, even with `timeBeginPeriod(1)` active. |
| Timer thread priority | `THREAD_PRIORITY_TIME_CRITICAL` — the highest available Windows thread priority, minimising but not eliminating jitter. |

> **Implication for tests:** If your embedded target uses a 100 µs or 500 µs tick, the simulation will run at 1 ms per tick. Relative timing ratios between tasks are preserved; absolute durations will differ.

---

### Kernel Tuning

These defines are shared with all STK ports and apply normally in simulation:

| Define | Default | Description |
|--------|---------|-------------|
| `STK_CRITICAL_SECTION_NESTINGS_MAX` | `16` | Maximum nesting depth for `hw::CriticalSection`. Exceeding this triggers `KERNEL_PANIC_CS_NESTING_OVERFLOW`. |
| `STK_TICKLESS_IDLE` | `0` | Tickless idle is **not supported** on Win32. The timer thread always fires at the configured interval. Setting this to `1` has no effect. |
| `STK_SEGGER_SYSVIEW` | `0` | SEGGER SystemView integration. Can be enabled on Win32 for trace-level testing if the SystemView host application is connected. |
| `STK_SYNC_DEBUG_NAMES` | `0` | Attach string names to synchronization primitives. Useful for debugging simulated task interactions. |

---

## Timing Behaviour and Limitations

The Win32 port is a **functional simulation**, not a real-time simulation. Timing behaviour differs from embedded targets in the following ways:

**Minimum tick resolution is 1 ms.** Windows is not a real-time OS. Even with `timeBeginPeriod(1)`, the multimedia timer and thread scheduler introduce jitter of approximately ±1 ms per tick. Do not use the Win32 port to validate hard real-time timing behaviour.

**Ticks are not cycle-accurate.** The timer thread uses `WaitForSingleObject` with a millisecond timeout. The actual elapsed time between ticks depends on the Windows scheduler load and other running processes.

**`HiResClock::GetTimeUs()` is accurate.** The `QueryPerformanceCounter`-based clock runs independently of the tick mechanism and provides sub-microsecond resolution on modern hardware (typically ~100 ns). It is suitable for measuring relative durations within the simulation.

**Task preemption is cooperative within a tick.** Between ticks, a running task cannot be preempted by another task — `SuspendThread` is only called from the timer thread context. A task that spins in a tight loop without yielding will hold the CPU until the next tick fires.

**`hw::IsInsideISR()` always returns `false`.** There is no ISR context in the simulation. Code that checks `IsInsideISR()` will always see the thread-mode path.

---

## High-Resolution Clock

`stk::hw::HiResClock` is backed by Windows `QueryPerformanceCounter` (QPC):

| Method | Implementation | Notes |
|--------|---------------|-------|
| `GetCycles()` | `QueryPerformanceCounter` relative to simulation start | Returns elapsed QPC ticks since `Initialize()`. |
| `GetFrequency()` | `QueryPerformanceFrequency` | Returns the hardware QPC frequency (typically 10 MHz on modern hardware, up to ~10 GHz on some CPUs). |
| `GetTimeUs()` | `(GetCycles() * 1000000ULL) / GetFrequency()` | Accurate to ~100 ns on modern Windows hardware. |

QPC is monotonic and not affected by wall-clock adjustments. It is safe to call from any task thread. The clock origin is set at `Initialize()` time, so `GetCycles()` returns 0 at start and increases from there.

---

## Critical Sections and Spin-Locks

**`hw::CriticalSection`** is implemented using a Win32 `CRITICAL_SECTION` combined with `SuspendThread` / `ResumeThread` on the timer thread:

- On `Enter()`: acquires the `CRITICAL_SECTION`, then suspends the timer thread (preventing tick preemption). Nesting is tracked; the timer thread is only suspended on the first (outermost) entry.
- On `Exit()`: decrements the nesting counter; on the last (outermost) exit, resumes the timer thread and releases the `CRITICAL_SECTION`.

The timer thread never suspends itself — if it is the caller, the suspend/resume is skipped to avoid deadlock.

**`hw::SpinLock`** uses `InterlockedCompareExchange` (atomic test-and-set) and `InterlockedExchange` (atomic clear). The spin-lock includes a priority-inversion avoidance mechanism: after 100 spin iterations without acquiring the lock, it calls `::Sleep(0)` or `::Sleep(1)` alternately to yield the CPU and allow the lock holder to run.

---

## Thread-Local Storage (TLS)

TLS (`stk::hw::GetTls()` / `stk::hw::SetTls()`) is backed by a single Windows TLS slot allocated with `TlsAlloc()` during `Initialize()`. Each task thread has an independent TLS value. The slot is freed with `TlsFree()` when the context is destroyed.

This matches the per-task TLS semantics on embedded targets (where TLS is stored in a CPU register such as `tp` on RISC-V or `r9` on ARM).

---

## Stack Allocation

On embedded targets, STK manages a fixed memory buffer as the task stack. On Win32, the task stack is managed by Windows itself — the stack size passed to `CreateThread` is derived from `ITask::GetStackSize() * sizeof(Word)`. The STK stack memory buffer is still allocated (and filled with `STK_STACK_MEMORY_FILLER`) but is used only to store the `TaskContext` pointer, not as an actual call stack.

This means:
- Stack overflow detection via the filler watermark does **not** apply to Win32 — Windows manages the actual stack and will throw a stack overflow exception if the thread overflows.
- `STK_STACK_SIZE_MIN` is not enforced as a hard lower bound for task execution on Win32, but the allocation must still be large enough to hold the `TaskContext` struct (two `Word`-size slots minimum).
- The stack size you configure still controls the Windows thread stack size, so size it appropriately for your task's call depth.

---

## Features Not Available in Simulation

| Feature | Status | Reason |
|---------|--------|--------|
| Tickless idle (`STK_TICKLESS_IDLE`) | Not supported | Timer thread always fires at the fixed interval; no WFI equivalent. |
| `hw::IsInsideISR()` | Always `false` | No ISR context exists in the simulation. |
| Privilege modes | Not applicable | Win32 has no equivalent of Cortex-M unprivileged thread mode or RISC-V U-mode. |
| TrustZone / security states | Not applicable | x86 Win32 only. |
| Multi-core SMP | Not supported | `STK_ARCH_CPU_COUNT` is fixed at `1` for the Win32 port. Multiple tasks run as Windows threads but are scheduled by a single STK kernel instance. |
| `mcycle` / DWT cycle counter | Not applicable | Replaced by QPC. |
| FPU context save/restore | Not applicable | Windows manages FPU context automatically per thread. |

---

## Project Setup

### MSVC (Visual Studio)

1. Add the STK source directory to your include paths.
2. Add `stk_arch_x86-win32.cpp` and the shared STK kernel sources to your project.
3. Create `stk_config.h` with `#define _STK_ARCH_X86_WIN32` and place it in your include path.
4. Set the language standard to C++11 or later (`/std:c++14` or `/std:c++17` recommended).
5. No additional linker dependencies are needed — `kernel32.lib` is linked by default and `Winmm.dll` is loaded dynamically at runtime.

**Recommended project settings:**

```
Configuration Properties → C/C++ → Language → C++ Language Standard: ISO C++17 (/std:c++17)
Configuration Properties → C/C++ → General → Additional Include Directories: $(SolutionDir)\stk\include
```

### MinGW / GCC

```bash
g++ -std=c++17 -O2 -I stk/include \
    stk/src/arch/x86/win32/stk_arch_x86-win32.cpp \
    your_app.cpp \
    -o simulation.exe
```

Winmm is loaded dynamically by STK — no `-lwinmm` flag is needed.

### Clang (LLVM for Windows)

MSVC-compatible mode (recommended for Visual Studio integration):
```bash
clang-cl /std:c++17 /I stk/include stk_arch_x86-win32.cpp your_app.cpp /Fe:simulation.exe
```

GCC-compatible mode:
```bash
clang++ -std=c++17 -I stk/include stk_arch_x86-win32.cpp your_app.cpp -o simulation.exe
```

---

## Frequently Asked Questions

**Tasks never switch / scheduler appears to be stuck**

The timer thread uses `WaitForSingleObject` with a millisecond timeout. If `resolution_us` is less than 1000, it is clamped to 1 ms. Check that `Start()` has been called — it is a blocking call that launches the timer thread and joins until all tasks exit.

**Simulation runs much slower or faster than the embedded target**

Expected — Windows is not real-time. Absolute tick timing differs from embedded. Use the simulation for functional correctness testing (task interactions, synchronization logic, scheduling order), not for timing validation. `HiResClock::GetTimeUs()` can measure relative durations accurately within the simulation.

**`assert` fires in `Initialize()` saying TLS allocation failed**

Windows has a limited number of TLS slots per process (at least 64 guaranteed, up to 1088 with `TLS_MINIMUM_AVAILABLE`). If your process exhausts TLS slots (e.g. due to many DLLs each allocating TLS), STK cannot allocate its slot. This is extremely rare in practice. Reducing the number of DLLs or static TLS users resolves it.

**`STK_KERNEL_PANIC` spins forever and the debugger cannot break in**

`STK_PANIC_HANDLER_DEFAULT` loops on `__stk_relax_cpu()` (which maps to `_mm_pause()` on x86). Set a breakpoint on `STK_PANIC_HANDLER_DEFAULT` before running, or add a `__debugbreak()` call at the top of the function by providing a custom panic handler via `_STK_ASSERT_REDIRECT`.

**Critical section deadlock in simulation**

If a task holds a `hw::CriticalSection` and then blocks on a synchronization object (mutex, event) that can only be signalled from another task, that other task will never run because the timer thread is suspended. Never block inside a critical section on Win32 (or on any STK target). Critical sections are intended for short, non-blocking code regions only.

**Compilation error: `'list' is not a member of 'std'`**

The Win32 port uses `<list>` and `<vector>` from the C++ standard library. Ensure you are compiling with C++11 or later and that standard library headers are available. On MinGW, verify your installation includes the standard library headers.

**Large stack sizes cause `CreateThread` to fail**

`CreateThread` is called with the task's configured stack size in bytes. Very large values (hundreds of MB) may fail on 32-bit builds due to address space limits. Keep task stack sizes realistic — the Win32 thread stack does not need to be as large as an embedded target's reserved memory region.
