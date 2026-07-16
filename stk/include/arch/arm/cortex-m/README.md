# STK ARM Cortex-M Architecture Port

**SuperTinyKernel RTOS** — Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.

This document is the configuration and porting reference for the ARM Cortex-M architecture port of STK (`stk\src\arch\arm\cortex-m`). It covers all available configuration defines, ISR handler names, clock and timer setup, high-resolution timing, privilege modes, TrustZone, multi-core support, and ready-to-use platform examples.

---

## Table of Contents

- [Overview](#overview)
- [Supported Cores](#supported-cores)
- [Minimum Requirements](#minimum-requirements)
- [Quick Start](#quick-start)
- [Configuration Reference](#configuration-reference)
  - [Architecture Enable](#architecture-enable)
  - [ISR Handler Names](#isr-handler-names)
  - [Clock Configuration](#clock-configuration)
  - [High-Resolution Clock](#high-resolution-clock)
  - [Tickless Idle](#tickless-idle)
  - [Privilege Modes](#privilege-modes)
  - [TrustZone (Cortex-M33 / ARMv8-M)](#trustzone-cortex-m33--armv8-m)
  - [Multi-Core (SMP)](#multi-core-smp)
  - [Stack Configuration](#stack-configuration)
  - [Kernel Tuning](#kernel-tuning)
  - [STM32 HAL Integration](#stm32-hal-integration)
- [Platform Examples](#platform-examples)
  - [RP2350 (Raspberry Pi Pico 2, Cortex-M33)](#rp2350-raspberry-pi-pico-2-cortex-m33)
  - [RP2040 (Raspberry Pi Pico, dual-core M0+)](#rp2040-raspberry-pi-pico-dual-core-m0)
  - [STM32 (any series)](#stm32-any-series)
  - [Generic Cortex-M bare-metal](#generic-cortex-m-bare-metal)
- [Frequently Asked Questions](#frequently-asked-questions)

---

## Overview

The Cortex-M port targets both privileged and unprivileged thread modes. Context switching is performed by the PendSV interrupt (lowest hardware priority), driven by a SysTick timer tick. Scheduler startup is triggered via SVC. The port supports:

- ARMv6-M (Cortex-M0, M0+, M1) — Thumb-1 ISA, no hardware privilege separation
- ARMv7-M (Cortex-M3, M4, M7) — Thumb-2, full privilege/unprivilege separation, DWT cycle counter
- ARMv8-M (Cortex-M23, M33) — ARMv8-M Baseline and Mainline, optional TrustZone
- Optional FPU (single and double precision, auto-detected via `__FPU_PRESENT` / `__FPU_USED`)
- Single-core and SMP (dual-core) configurations
- Tickless idle with DWT-based drift correction on M3+
- Two high-resolution clock backends: DWT CYCCNT (M3+) and SysTick-based (M0/M0+)

---

## Supported Cores

| Core | ISA | FPU | DWT | Privilege | TrustZone |
|------|-----|-----|-----|-----------|-----------|
| Cortex-M0 / M0+ / M1 | ARMv6-M (Thumb-1) | No | No | No | No |
| Cortex-M3 | ARMv7-M | No | Yes | Yes | No |
| Cortex-M4 | ARMv7-M | Optional | Yes | Yes | No |
| Cortex-M7 | ARMv7-M | Optional | Yes | Yes | No |
| Cortex-M23 | ARMv8-M Baseline | No | No | Yes | Optional |
| Cortex-M33 | ARMv8-M Mainline | Optional | Yes | Yes | Optional |

The core variant is auto-detected from the `__CORTEX_M` macro provided by CMSIS. No manual selection is needed.

---

## Minimum Requirements

- GCC, Clang/LLVM (ARM Compiler), or IAR with Cortex-M support
- CMSIS device headers providing `__CORTEX_M`, `SysTick`, `SCB`, and optionally `DWT`
- `SystemCoreClock` variable set to the correct CPU frequency before `Initialize()` is called

---

## Quick Start

Create `stk_config.h` in your project and include it before any STK headers:

```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <your_device.h>  // provides __CORTEX_M, SysTick, SystemCoreClock

// Select ARM Cortex-M architecture port
#define _STK_ARCH_ARM_CORTEX_M

#endif /* STK_CONFIG_H_ */
```

For most Cortex-M3/M4/M7 targets with standard CMSIS headers this is all that is needed. ISR handler names, clock source, and DWT all have correct defaults.

---

## Configuration Reference

### Architecture Enable

| Define | Description |
|--------|-------------|
| `_STK_ARCH_ARM_CORTEX_M` | **Required.** Selects the Cortex-M port. Must be defined in `stk_config.h`. |

---

### ISR Handler Names

STK registers three interrupt handlers that must match the symbol names in your vector table. The defaults follow CMSIS naming conventions and work out of the box for most bare-metal and STM32 HAL projects.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_SYSTICK_HANDLER` | `SysTick_Handler` | SysTick interrupt. Fires at every scheduler tick, calls the scheduler, and pends PendSV if a context switch is needed. |
| `STK_PENDSV_HANDLER` | `PendSV_Handler` | PendSV interrupt. Performs the actual register save/restore and stack switch. Always configured at the lowest hardware priority. |
| `STK_SVC_HANDLER` | `SVC_Handler` | SVC (SuperVisor Call) interrupt. Used for scheduler startup and critical section entry/exit from unprivileged thread mode. |

Override when your SDK or RTOS uses non-standard names:
```cpp
// pico-sdk (RP2350 Cortex-M33)
#define STK_SYSTICK_HANDLER  isr_systick
#define STK_PENDSV_HANDLER   isr_pendsv
#define STK_SVC_HANDLER      isr_svcall
```

> **Important:** If any of these handlers are already defined elsewhere (e.g. by STM32 HAL's `stm32xx_it.c` template), either remove the duplicate or rename it — two definitions of the same weak symbol will cause a linker error or silent override.

---

### Clock Configuration

The scheduler tick period is derived from `SystemCoreClock` at the time `Initialize()` is called. The SysTick timer is programmed in CPU clock cycles using `SysTick_Config()`.

| Variable / Define | Description |
|--------|-------------|
| `SystemCoreClock` | **Required.** CPU core clock frequency in Hz. Must be set correctly before `PlatformArmCortexM::Initialize()` is called. Provided by CMSIS and typically updated by `SystemCoreClockUpdate()` or your clock init code. |

> **Common mistake:** Calling `Initialize()` before the PLL is configured, or before `SystemCoreClockUpdate()` is called. The tick period will be calculated from the wrong frequency and all sleep durations and timeouts will be proportionally wrong.

STM32 example:
```cpp
HAL_Init();                // configures SysTick at default frequency
SystemClock_Config();      // reconfigures PLL
SystemCoreClockUpdate();   // updates SystemCoreClock to match new PLL
kernel.Initialize(...);    // now safe to initialize STK
```

---

### High-Resolution Clock

`stk::hw::HiResClock` provides `GetCycles()`, `GetFrequency()`, and `GetTimeUs()` for high-precision measurements. The backend is selected automatically based on the core:

| Core | Backend | Source | Precision | Notes |
|------|---------|--------|-----------|-------|
| Cortex-M3 / M4 / M7 / M33 | `HiResClockDWT` | DWT `CYCCNT` | ~ns (CPU clock rate) | 32-bit CYCCNT with 64-bit software accumulator. DWT is enabled automatically in `Initialize()`. |
| Cortex-M0 / M0+ / M1 | `HiResClockM0` | SysTick + OS tick counter | ~tick resolution | Combines coarse OS ticks with the fine-grained SysTick down-counter to produce a monotonic cycle estimate. |

`GetFrequency()` returns `SystemCoreClock` on both backends. `GetTimeUs()` computes:
```cpp
(GetCycles() * 1000000ULL) / GetFrequency()
```

**DWT backend notes (M3+):**

- DWT `CYCCNT` is a 32-bit counter that wraps at 2³² cycles (~28 seconds at 150 MHz). STK maintains a 64-bit software accumulator by calling `Update()` inside `GetCycles()` to detect and accumulate wraps. Call `GetCycles()` at least once per wrap period (~28 s at 150 MHz) to avoid missing a wrap.
- On Cortex-M7, `DWT->LAR` must be unlocked with the CoreSight magic key (`0xC5ACCE55`) before `CYCCNT` can be written. STK handles this automatically in `HW_DWTEnableCounter()`.
- DWT is enabled by `Initialize()` when `STK_TICKLESS_USE_ARM_DWT` is set, and again independently by the `HiResClockDWT` constructor on first `GetCycles()` call.

**M0 backend notes:**

- No DWT is available on M0/M0+. The `HiResClockM0` backend synthesizes a cycle count from the kernel tick counter (`stk::GetTicks()`) multiplied by the tick resolution, plus the remaining SysTick down-counter value.
- Precision is bounded by the SysTick reload value (one tick period), typically 1 ms. Sub-millisecond measurements within a single tick are accurate; measurements spanning tick boundaries accumulate the tick granularity as error.

---

### Tickless Idle

Tickless idle suppresses SysTick interrupts during idle periods, reducing power consumption. On Cortex-M3+, DWT `CYCCNT` is used to measure the elapsed time during the idle period for accurate timer rearm without drift accumulation.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_TICKLESS_IDLE` | `0` | Set to `1` to enable tickless idle mode. |
| `STK_TICKLESS_TICKS_MAX` | `1000` | Maximum number of ticks the scheduler may skip in a single idle period. Must not exceed `100000`. |
| `STK_TICKLESS_USE_ARM_DWT` | `1` | When `1` and `__CORTEX_M > 1`: uses DWT `CYCCNT` to measure the actual elapsed time during tickless sleep and compensate for rearm overhead. Has no effect on Cortex-M0/M0+. Set to `0` to disable DWT use even on M3+ (not recommended). |

> `STK_TICKLESS_USE_ARM_DWT` has no effect on RISC-V targets.

---

### Privilege Modes

On Cortex-M3 and higher (where `CONTROL_nPRIV_Msk` is defined), STK supports per-task privilege levels. Each task stack carries an `ACCESS_PRIVILEGED` or `ACCESS_UNPRIVILEGED` flag that is applied to the `CONTROL` register on every context switch.

- **Privileged tasks** run with full access to all registers and peripherals.
- **Unprivileged tasks** are restricted by the MPU (if configured). Critical section entry/exit from unprivileged mode is performed via SVC to escalate privilege temporarily.

On **Cortex-M0/M0+** there is no privilege separation — all tasks run in privileged mode regardless of the stack flag.

No user-facing define is required to enable this feature. It is automatically active when `CONTROL_nPRIV_Msk` is present in your CMSIS headers.

---

### TrustZone (Cortex-M33 / ARMv8-M)

STK includes optional TrustZone-aware SVC handling for Cortex-M33 (ARMv8-M Mainline) and Cortex-M23 (ARMv8-M Baseline) targets.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_CORTEX_M_TRUSTZONE` | *(undefined / disabled)* | Define to enable TrustZone-aware SVC dispatch. When enabled, the SVC handler inspects `LR` bits to determine whether the call originated from Secure or Non-Secure state and reads the correct stack pointer (`MSP`/`PSP`/`MSP_NS`/`PSP_NS`) accordingly. |

```cpp
// In stk_config.h, or uncomment in stk_arch_arm-cortex-m.cpp:
#define STK_CORTEX_M_TRUSTZONE
```

> Only define `STK_CORTEX_M_TRUSTZONE` if your application genuinely uses TrustZone with Secure/Non-Secure state transitions. Enabling it on a non-TrustZone project (or a project where all code runs in the Secure state) adds unnecessary SVC dispatch overhead without benefit.

---

### Multi-Core (SMP)

STK allocates per-core kernel context instances indexed by core ID. On Cortex-M SMP targets the core ID is typically read from a dedicated hardware register rather than a CPU CSR.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_ARCH_CPU_COUNT` | `1` | Number of physical CPU cores. Set to `2` for dual-core targets (RP2350, RP2040). |
| `STK_ARCH_GET_CPU_ID` | `(0)` | Expression that returns the calling core's ID (0-based). **Must be overridden for multi-core builds** — the default always returns 0. |

**Spin-lock backend selection** is automatic based on included headers:

| Platform | Spin-lock implementation |
|----------|--------------------------|
| Cortex-M3/M4/M7 (general) | GCC `__atomic_test_and_set` / `__atomic_clear` with `dmb ishst` barrier |
| RP2040 (`RP2040_H` defined) | SIO hardware spinlock 31 (`SIO->SPINLOCK31`) with DMB — required because M0+ has no LDREX/STREX |
| Cortex-M0/M0+ (single-core fallback) | PRIMASK-based critical section around test-and-set |

> **RP2040 note:** STK reserves SIO hardware spinlock 31 (`SIO->SPINLOCK31`) for its internal critical section. Do not use `SPINLOCK31` anywhere else in your application.

> On RP2350 with the ARM Cortex-M33 cores, the generic atomic spin-lock (GCC builtins) is used — no SIO spinlock is needed because M33 provides LDREX/STREX.

---

### Stack Configuration

| Define | Default | Description |
|--------|---------|-------------|
| `STK_STACK_SIZE_MIN` | `32` (Cortex-M) | Minimum stack size in `Word` elements. Enforced at stack allocation. The default of 32 covers the full hardware exception frame (8 words) plus callee-saved registers and optional LR/FPU slots. |
| `STK_STACK_MEMORY_ALIGN` | `4` (Cortex-M) | Required stack alignment in bytes. AAPCS requires 8-byte alignment on function calls; the stack base itself is aligned to 4 bytes by STK. |
| `STK_STACK_MEMORY_FILLER` | `0xDEADBEEF` | Sentinel value written to the entire stack at init for overflow detection and peak usage watermarking. |
| `STK_SLEEP_TRAP_STACK_SIZE` | `STK_STACK_SIZE_MIN` | Stack size for the idle/sleep trap task. Increase if `IEventOverrider::OnSleep()` uses significant stack depth. |

---

### Kernel Tuning

| Define | Default | Description |
|--------|---------|-------------|
| `stk_cs_NESTINGS_MAX` | `16` | Maximum allowable nesting depth for `hw::CriticalSection`. Exceeding this triggers `KERNEL_PANIC_CS_NESTING_OVERFLOW`. |
| `STK_SEGGER_SYSVIEW` | `0` | Enable SEGGER SystemView trace integration. Automatically enables `STK_NEED_TASK_ID` and `STK_SYNC_DEBUG_NAMES`. When enabled, `Initialize()` calls `SEGGER_SYSVIEW_Init()` and the SysTick/PendSV handlers emit trace records. |
| `STK_SYNC_DEBUG_NAMES` | `0` | Attach string names to synchronization primitives for trace tools. |

---

### STM32 HAL Integration

When STM32 HAL is used (`HAL_MODULE_ENABLED` is defined), STK automatically integrates with the HAL tick mechanism:

- In **normal mode**: `HAL_IncTick()` is called from inside `STK_SYSTICK_HANDLER` so that HAL timing functions (`HAL_Delay`, `HAL_GetTick`) continue to work correctly.
- In **tickless mode**: `uwTick` is incremented by `sleep_ticks * tick_resolution` each time the scheduler wakes, preserving HAL tick accuracy across variable-length idle periods.
- The STM32 HAL initializes SysTick during `HAL_Init()`. STK detects this and guards the SysTick handler with a `m_started` check to avoid a crash on NULL handler before STK's `Start()` is called.

No additional defines are needed — the integration activates automatically when `HAL_MODULE_ENABLED` is present.

> **Important:** Call `SystemCoreClockUpdate()` after `SystemClock_Config()` and before `PlatformArmCortexM::Initialize()`. STM32 HAL's `HAL_Init()` configures SysTick at the default HSI frequency; your clock init changes the frequency without automatically updating `SystemCoreClock`.

---

## Platform Examples

### RP2350 (Raspberry Pi Pico 2, Cortex-M33)

The RP2350 contains two Cortex-M33 cores alongside two RISC-V Hazard3 cores. This example targets the M33 cores.

Ready-to-use project examples are available in:
```
build/example/project/eclipse/rpi
```

RP2350 Cortex-M33 characteristics relevant to STK:
- Core ID is read from the SIO peripheral (`SIO_BASE + SIO_CPUID_OFFSET`), equivalent to pico-sdk `get_core_num()`
- ISR handler names are defined in `crt0.S` of pico-sdk
- No SIO hardware spinlock is needed — M33 has LDREX/STREX for atomic operations
- Core 1 is started separately via `multicore_launch_core1()`
- `SystemCoreClock` is updated by pico-sdk during board init

**`stk_config.h` for RP2350 Cortex-M33 (single-core):**
```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <RP2350.h>
#include <pico.h>

// Select ARM Cortex-M port (M33 cores of RP2350)
#define _STK_ARCH_ARM_CORTEX_M

// ISR handler names from pico-sdk crt0.S
#define STK_SYSTICK_HANDLER  isr_systick
#define STK_PENDSV_HANDLER   isr_pendsv
#define STK_SVC_HANDLER      isr_svcall

#endif /* STK_CONFIG_H_ */
```

**`stk_config.h` for RP2350 Cortex-M33 (dual-core):**
```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <RP2350.h>
#include <pico.h>

// Select ARM Cortex-M port (M33 cores of RP2350)
#define _STK_ARCH_ARM_CORTEX_M

// Enable both cores
#define STK_ARCH_CPU_COUNT    2
#define STK_ARCH_GET_CPU_ID() (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET)) // equivalent to get_core_num()

// ISR handler names from pico-sdk crt0.S
#define STK_SYSTICK_HANDLER  isr_systick
#define STK_PENDSV_HANDLER   isr_pendsv
#define STK_SVC_HANDLER      isr_svcall

#endif /* STK_CONFIG_H_ */
```

---

### RP2040 (Raspberry Pi Pico, dual-core M0+)

The RP2040 has two Cortex-M0+ cores. Because M0+ has no LDREX/STREX, STK uses SIO hardware spinlock 31 for inter-core mutual exclusion when `RP2040_H` is detected.

```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <RP2040.h>
#include <pico.h>

// Select ARM Cortex-M port (M0+ cores of RP2040)
#define _STK_ARCH_ARM_CORTEX_M

// Enable both cores
#define STK_ARCH_CPU_COUNT    2
#define STK_ARCH_GET_CPU_ID() (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET))

// ISR handler names from pico-sdk crt0.S
#define STK_SYSTICK_HANDLER  isr_systick
#define STK_PENDSV_HANDLER   isr_pendsv
#define STK_SVC_HANDLER      isr_svcall

#endif /* STK_CONFIG_H_ */
```

> Do **not** use `SIO->SPINLOCK31` anywhere else in your application — STK reserves it for its inter-core critical section.

---

### STM32 (any series)

```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include "stm32xx_hal.h"  // provides __CORTEX_M, SystemCoreClock, HAL_MODULE_ENABLED

// Select ARM Cortex-M port
#define _STK_ARCH_ARM_CORTEX_M

// Default ISR names (SysTick_Handler, PendSV_Handler, SVC_Handler) match
// STM32 HAL's vector table — no overrides needed.

// Optional: enable tickless idle with DWT drift correction (M3/M4/M7 only)
// #define STK_TICKLESS_IDLE            1
// #define STK_TICKLESS_USE_ARM_DWT     1

#endif /* STK_CONFIG_H_ */
```

Initialization order for STM32 HAL projects:
```cpp
HAL_Init();             // init HAL, sets SysTick at default HSI frequency
SystemClock_Config();   // configure PLL
SystemCoreClockUpdate();// update SystemCoreClock to match new PLL — required before STK init
kernel.Initialize(...); // safe to initialize STK now
kernel.Start();
```

> If `SysTick_Handler`, `PendSV_Handler`, or `SVC_Handler` appear in your `stm32xx_it.c` from CubeMX, remove them — STK provides its own definitions.

---

### Generic Cortex-M bare-metal

Minimal config for any Cortex-M target with a CMSIS device header:

```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <your_device_cmsis.h>  // must define __CORTEX_M and SystemCoreClock

// Select ARM Cortex-M port
#define _STK_ARCH_ARM_CORTEX_M

#endif /* STK_CONFIG_H_ */
```

---

## Frequently Asked Questions

**Scheduler tick period is wrong / all timeouts are proportionally off**

`SystemCoreClock` was not updated before `Initialize()`. The SysTick period is calculated from `SystemCoreClock` at init time. Call `SystemCoreClockUpdate()` (or set `SystemCoreClock` manually) after your PLL configuration and before `PlatformArmCortexM::Initialize()`.

**Hard fault immediately on `Start()` or first context switch**

Check that `STK_PENDSV_HANDLER` is set to the lowest possible interrupt priority. STK sets this automatically via NVIC during `Initialize()`. If something else raises PendSV priority or if the symbol name is wrong and a different handler fires instead, a priority inversion hard fault will occur.

**Linker error: multiple definition of `SysTick_Handler` (or `PendSV_Handler`, `SVC_Handler`)**

Your project (e.g. STM32 CubeMX-generated `stm32xx_it.c`) already defines one of these. Remove the duplicate from your project — STK owns these handlers. Alternatively rename them in both places using the `STK_SYSTICK_HANDLER` / `STK_PENDSV_HANDLER` / `STK_SVC_HANDLER` defines.

**`HiResClock::GetCycles()` stops incrementing or wraps unexpectedly**

On M3+ the DWT `CYCCNT` is a 32-bit register that wraps every 2³² cycles (~28 seconds at 150 MHz). STK maintains a 64-bit accumulator but only updates it when `GetCycles()` is called. If more than one wrap period elapses between calls, wraps will be missed and the result will be incorrect. Ensure `GetCycles()` is called at least once every ~28 seconds (at 150 MHz; proportionally longer at lower frequencies).

**`GetTimeUs()` returns wrong values**

`SystemCoreClock` is incorrect. Both `HiResClockDWT` and `HiResClockM0` use `SystemCoreClock` as their frequency source. Verify it matches the actual running CPU frequency.

**On RP2040 dual-core: deadlock or crash in critical section**

Something else in your application is also using `SIO->SPINLOCK31`. STK reserves this spinlock for inter-core critical section exclusion. Use a different SIO spinlock (0–30) for application purposes.

**On Cortex-M0: `HiResClock::GetTimeUs()` is jittery**

Expected — the M0 backend combines the OS tick counter with the SysTick down-counter. Within a single tick the value is smooth; at tick boundaries there is a step of exactly one tick resolution. For finer resolution on M0 consider decreasing the tick period (increasing the tick frequency) at the cost of higher ISR overhead.

**SVC handler crashes when called from unprivileged thread mode**

Ensure `STK_SVC_HANDLER` has higher priority than `STK_PENDSV_HANDLER` (SVC must be able to pre-empt PendSV). STK configures this automatically. Do not manually lower SVC priority below PendSV.

**Stack overflow or memory corruption on first context switch**

On Cortex-M the minimum stack (`STK_STACK_SIZE_MIN = 32`) covers the hardware exception frame (8 words) plus callee-saved registers and optional EXC_RETURN / FPU slots. If your task stack is smaller than this, the first context switch will corrupt adjacent memory silently. Always allocate task stacks larger than `STK_STACK_SIZE_MIN`.
