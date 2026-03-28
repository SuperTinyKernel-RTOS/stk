# STK RISC-V Architecture Port

**SuperTinyKernel RTOS** — Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.

This document is the configuration and porting reference for the RISC-V architecture port of STK (`stk\src\arch\risc-v`). It covers all available configuration defines, ISR handler names, timer setup, clock configuration, high-resolution timing, multi-core support, and ready-to-use platform examples.

---

## Table of Contents

- [Overview](#overview)
- [Minimum Requirements](#minimum-requirements)
- [Quick Start](#quick-start)
- [Configuration Reference](#configuration-reference)
  - [Architecture Enable](#architecture-enable)
  - [CLINT / Timer Registers](#clint--timer-registers)
  - [ISR Handler Names](#isr-handler-names)
  - [ISR Section Placement](#isr-section-placement)
  - [Clock Configuration](#clock-configuration)
  - [High-Resolution Clock](#high-resolution-clock)
  - [Tickless Idle](#tickless-idle)
  - [Multi-Core (SMP)](#multi-core-smp)
  - [PendSV Emulation](#pendsv-emulation)
  - [Stack Configuration](#stack-configuration)
  - [Kernel Tuning](#kernel-tuning)
- [Platform Examples](#platform-examples)
  - [RP2350 (Raspberry Pi Pico 2)](#rp2350-raspberry-pi-pico-2)
  - [ESP32-H2 / ESP32-C6](#esp32-h2--esp32-c6)
- [Frequently Asked Questions](#frequently-asked-questions)

---

## Overview

The RISC-V port targets M-mode bare-metal environments. Context switching is driven by the CLINT Machine Timer Interrupt (MTI), with the Machine Exception handler used for scheduler startup via `ecall`. The port supports:

- RV32I, RV32E, RV64I ISAs
- Optional FPU (single and double precision)
- Single-core and SMP (multi-hart) configurations
- Tickless idle with drift-free MTIME-based timer rearm
- Two high-resolution clock backends: MTIME (1 µs) and `mcycle` (sub-µs)

---

## Minimum Requirements

- GCC or Clang with RISC-V M-mode support (`-march=rv32i` or higher)
- A CLINT-compatible timer providing `mtime` and `mtimecmp` registers
- A vector table that routes MTI, MSI, and exception traps to STK's handlers

---

## Quick Start

Create `stk_config.h` in your project and include it before any STK headers. At minimum define the architecture and your CLINT addresses:

```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <risc-v/encoding.h>

// Select RISC-V architecture port
#define _STK_ARCH_RISC_V

// CLINT register addresses (check your MCU datasheet)
#define STK_RISCV_CLINT_BASE_ADDR      0x2000000U
#define STK_RISCV_CLINT_MTIME_ADDR     (STK_RISCV_CLINT_BASE_ADDR + 0xBFF8U)
#define STK_RISCV_CLINT_MTIMECMP_ADDR  (STK_RISCV_CLINT_BASE_ADDR + 0x4000U)

#endif /* STK_CONFIG_H_ */
```

All other defines have defaults and are optional.

---

## Configuration Reference

### Architecture Enable

| Define | Description |
|--------|-------------|
| `_STK_ARCH_RISC_V` | **Required.** Selects the RISC-V port. Must be defined in `stk_config.h`. |

---

### CLINT / Timer Registers

RISC-V does not fix CLINT register addresses in the specification — they vary by MCU. Check your datasheet or Technical Reference Manual.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_RISCV_CLINT_BASE_ADDR` | `0x2000000` | Base address of the CLINT peripheral. Used only as a base for the default `MTIME` and `MTIMECMP` address calculations. Not used when those two addresses are defined directly. |
| `STK_RISCV_CLINT_MTIME_ADDR` | `STK_RISCV_CLINT_BASE_ADDR + 0xBFF8` | Address of the 64-bit `mtime` free-running counter. May be a plain integer address or a typed pointer (e.g. `(volatile uint64_t *)`) depending on platform. |
| `STK_RISCV_CLINT_MTIMECMP_ADDR` | `STK_RISCV_CLINT_BASE_ADDR + 0x4000` | Base address of the `mtimecmp` register. May be a typed pointer. When `STK_RISCV_CLINT_MTIMECMP_PER_HART` is `1`, treated as an array indexed by hart ID. |
| `STK_RISCV_CLINT_MTIMECMP_PER_HART` | `1` | When `1`, STK indexes `mtimecmp` by hart ID (`mtimecmp[hartid]`) — the standard CLINT layout. Set to `0` when each hart has its own private `mtime`/`mtimecmp` pair at a fixed address (e.g. RP2350 SIO, where `SIO_BASE` is already per-core and no hart offset is needed). |

> **Important:** Always verify these addresses against your MCU's datasheet. Incorrect addresses will cause the scheduler timer to never fire or to corrupt unrelated peripherals.

---

### ISR Handler Names

STK's context-switch assembly must be linked under the exact symbol names your vector table expects. Override these if your platform uses different names.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_SYSTICK_HANDLER` | `riscv_mtvec_mti` | Machine Timer Interrupt handler. Fires at every scheduler tick and drives context switching. |
| `STK_SVC_HANDLER` | `riscv_mtvec_exception` | Machine Exception handler. Used for scheduler startup via `ecall` and for hard-fault processing. |
| `STK_MSI_HANDLER` | `riscv_mtvec_msi` | Machine Software Interrupt handler. Used as a PendSV equivalent when `_STK_RISCV_USE_PENDSV` is enabled. |

Example override:
```cpp
#define STK_SYSTICK_HANDLER  m_timer_interrupt_handler
#define STK_SVC_HANDLER      exception_handler
#define STK_MSI_HANDLER      software_interrupt_handler
```

---

### ISR Section Placement

By default STK places ISR handlers in the default `.text` section. For platforms that require ISR code to reside in RAM (e.g. to avoid XIP flash latency during interrupt handling) override the section attribute.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_RISCV_ISR_SECTION` | *(empty)* | Section attribute applied to all ISR handler functions. |

Example for pico-sdk (places handlers outside flash, required for correct interrupt behaviour with XIP):
```cpp
#define STK_RISCV_ISR_SECTION  __not_in_flash("stk")
```

Example for ESP32 (IRAM placement via ESP-IDF):
```cpp
#define STK_RISCV_ISR_SECTION  IRAM_ATTR
```

Example for a custom linker section:
```cpp
#define STK_RISCV_ISR_SECTION  __attribute__((section(".fast_ram")))
```

---

### Clock Configuration

STK uses two independent clocks that must be configured separately:

**Scheduler timer clock** — drives `mtimecmp` and controls the tick period. On most RISC-V MCUs (including RP2350 and ESP32) the CLINT `mtime` counter runs from a fixed-frequency reference clock that is independent of the CPU PLL, commonly 1 MHz.

**CPU core clock** — used only when the sub-microsecond `mcycle`-based high-resolution clock backend is selected (`STK_SUBMICORSECOND_PRECISION_TIMER 1`).

| Define | Default | Description |
|--------|---------|-------------|
| `STK_TIMER_CLOCK_FREQUENCY` | `1000000` | Frequency of the `mtime` reference clock in Hz. Used to calculate `mtimecmp` increments. **Must match the actual hardware timer frequency, not the CPU frequency.** |
| `STK_SYSTEM_CORE_CLOCK_VAR` | `SystemCoreClock` | Name of the variable holding the CPU core clock frequency. Used by `HiResClock::GetFrequency()` when `STK_SUBMICORSECOND_PRECISION_TIMER` is enabled. |
| `STK_SYSTEM_CORE_CLOCK_FREQUENCY` | `150000000` | Default CPU core frequency in Hz (150 MHz). Used to initialise `STK_SYSTEM_CORE_CLOCK_VAR` if `_STK_SYSTEM_CORE_CLOCK_EXTERNAL` is not defined. |
| `_STK_SYSTEM_CORE_CLOCK_EXTERNAL` | *(undefined)* | When defined, STK does not declare its own `STK_SYSTEM_CORE_CLOCK_VAR` storage. Define this when the variable is already provided by your SDK (e.g. CMSIS `SystemCoreClock`). |

> **Common mistake:** Using `SystemCoreClock` (CPU frequency) for `STK_TIMER_CLOCK_FREQUENCY`. On RP2350 and ESP32 the CLINT timer runs at 1 MHz regardless of the CPU PLL setting. Using the wrong frequency will cause all task sleep durations and timeouts to be proportionally wrong.

---

### High-Resolution Clock

`stk::hw::HiResClock` provides `GetCycles()`, `GetFrequency()`, and `GetTimeUs()` for high-precision measurements. Two backends are available, selected at compile time:

| Define | Default | Description |
|--------|---------|-------------|
| `STK_SUBMICORSECOND_PRECISION_TIMER` | `1` | When `1`: uses `mcycle`/`mcycleh` CSRs — sub-microsecond precision at CPU clock rate. When `0`: uses `mtime` — 1 µs precision, no CPU frequency dependency. |

**Backend comparison:**

| | `mtime` backend (`= 0`) | `mcycle` backend (`= 1`) |
|---|---|---|
| Precision | 1 µs | Sub-µs (e.g. ~6.7 ns at 150 MHz) |
| Frequency source | `STK_TIMER_CLOCK_FREQUENCY` | `STK_SYSTEM_CORE_CLOCK_VAR` |
| Cross-hart consistent | Yes (global counter) | No (per-hart counter) |
| Requires `SystemCoreClock` | No | Yes — must be accurate |
| `mcountinhibit` handling | Not needed | Auto-cleared in `Initialize()` |

> **RP2350 note:** The `mcycle` counter is inhibited by default (`mcountinhibit = 0x5`). STK automatically clears the inhibit bit during `PlatformRiscV::Initialize()` when `STK_SUBMICORSECOND_PRECISION_TIMER 1` is set. No manual action is required.

`GetTimeUs()` is computed as:
```cpp
(GetCycles() * 1000000ULL) / GetFrequency()
```
Both backends return a consistent value because `GetFrequency()` always matches the clock source used by `GetCycles()`.

---

### Tickless Idle

Tickless idle suppresses scheduler ticks during periods when no task is ready to run, reducing power consumption. On RISC-V, STK uses the CLINT `mtime` absolute timestamp for drift-free timer rearm — no DWT or separate correction mechanism is needed.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_TICKLESS_IDLE` | `0` | Set to `1` to enable tickless idle mode. |
| `STK_TICKLESS_TICKS_MAX` | `1000` | Maximum number of ticks the scheduler may skip in a single idle period. Must not exceed `100000`. |

> `STK_TICKLESS_USE_ARM_DWT` has no effect on RISC-V targets and can be ignored.

---

### Multi-Core (SMP)

STK allocates per-hart kernel context instances indexed by hart ID.

| Define | Default | Description |
|--------|---------|-------------|
| `STK_ARCH_CPU_COUNT` | `1` | Number of physical CPU cores (harts). Set to `2` for dual-core targets such as RP2350. |
| `STK_ARCH_GET_CPU_ID` | `read_csr(mhartid)` | Expression that returns the calling hart's ID (0-based). Override when `mhartid` is unavailable or when the platform provides a more direct mechanism. |

On most standard RISC-V MCUs the default `read_csr(mhartid)` is correct. On RP2350, the pico-sdk provides a dedicated per-core register via the SIO peripheral which is slightly faster and avoids a CSR read:

```cpp
#define STK_ARCH_GET_CPU_ID()  (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET))
```

This is equivalent to the pico-sdk `get_core_num()` function.

> On RP2350 dual-core builds, core 1 is started separately by the SDK (e.g. via `multicore_launch_core1()`). Each core must call `PlatformRiscV::Initialize()` with its own kernel instance before calling `Start()`. The `mcountinhibit` clear and per-hart timer programming are handled automatically per hart inside `Initialize()`.

---

### PendSV Emulation

By default STK performs context switching directly inside the MTI handler. Optionally it can emulate ARM's PendSV pattern using the Machine Software Interrupt (MSI), deferring the actual register swap to a lower-priority interrupt.

| Define | Default | Description |
|--------|---------|-------------|
| `_STK_RISCV_USE_PENDSV` | *(undefined / disabled)* | Define to enable PendSV-style context switching via CLINT MSIP. |

> **Recommendation:** Leave this disabled for standard M-mode deployments. The single-handler path has lower overhead and identical scheduling behaviour. Only enable if targeting a RISC-V core with a hardware preemptible interrupt controller (e.g. CLIC) where deferring to a lower MSI priority level provides a measurable benefit. When enabled, the two-interrupt path incurs two full save/restore cycles and two `mret` transitions per context-switch tick.

---

### Stack Configuration

| Define | Default | Description |
|--------|---------|-------------|
| `STK_STACK_SIZE_MIN` | See below | Minimum stack size in `Word` elements. Enforced at stack allocation. |
| `STK_STACK_MEMORY_ALIGN` | `16` (RISC-V) | Required stack alignment in bytes. RISC-V ABI requires 16-byte alignment. |
| `STK_STACK_MEMORY_FILLER` | `0xDEADBEEF` | Sentinel value written to the entire stack at init for overflow detection and watermark measurement. |
| `STK_SLEEP_TRAP_STACK_SIZE` | `STK_STACK_SIZE_MIN` | Stack size for the idle/sleep trap task. Increase if `IEventOverrider::OnSleep()` uses significant stack depth. |

Default `STK_STACK_SIZE_MIN` values by ISA:

| ISA | No FPU | With FPU |
|-----|--------|----------|
| RV32E (reduced 16-register) | 32 | `32 + (__riscv_flen * 2)` |
| RV32I / RV64I (standard 32-register) | 256 | `512 + (__riscv_flen * 2)` |

> The higher minimum for RV32I/RV64I is necessary to prevent memory corruption on platforms like RP2350 due to the larger register file saved on each context switch.

---

### Kernel Tuning

These defines apply across all STK architectures but are frequently relevant for RISC-V embedded targets:

| Define | Default | Description |
|--------|---------|-------------|
| `STK_CRITICAL_SECTION_NESTINGS_MAX` | `16` | Maximum allowable nesting depth for `hw::CriticalSection`. Exceeding this triggers `KERNEL_PANIC_CS_NESTING_OVERFLOW`. |
| `STK_SEGGER_SYSVIEW` | `0` | Enable SEGGER SystemView trace integration. Automatically enables `STK_NEED_TASK_ID` and `STK_SYNC_DEBUG_NAMES`. |
| `STK_SYNC_DEBUG_NAMES` | `0` | Attach string names to synchronization primitives for trace tools. |

---

## Platform Examples

### RP2350 (Raspberry Pi Pico 2)

Ready-to-use project examples (marked `_riscv`) are available in:
```
build/example/project/eclipse/rpi
```

The RP2350 Hazard3 RISC-V core has the following characteristics relevant to STK:

- `mtime` and `mtimecmp` are exposed **per-core via the SIO peripheral** (`SIO_BASE`), not through a classic CLINT at a fixed base address. Because `SIO_BASE` is already core-private, no hart-index offset is needed — set `STK_RISCV_CLINT_MTIMECMP_PER_HART 0`.
- `mtime` runs at **1 MHz** from the watchdog tick generator, independent of the system PLL.
- `mcycle` is **inhibited by default** (`mcountinhibit = 0x5`) — STK clears this automatically during `Initialize()` when `STK_SUBMICORSECOND_PRECISION_TIMER 1` is set.
- All STK tasks run in **M-mode**.
- ISR handlers **must not reside in XIP flash** — use `__not_in_flash("stk")` for `STK_RISCV_ISR_SECTION`.
- ISR handler names are defined in `crt0_riscv.S` of pico-sdk.
- Core 1 is started separately via `multicore_launch_core1()`. Each core calls `PlatformRiscV::Initialize()` and `Start()` independently.

**`stk_config.h` for RP2350 (single-core, core 0 only):**
```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <stdint.h>
#include <risc-v/encoding.h>
#include <pico.h>

// Select RISC-V architecture port
#define _STK_ARCH_RISC_V

// RP2350: mtime and mtimecmp are accessed via SIO (per-core registers)
#define STK_RISCV_CLINT_MTIME_ADDR    ((volatile uint64_t *)(SIO_BASE + SIO_MTIME_OFFSET))
#define STK_RISCV_CLINT_MTIMECMP_ADDR ((volatile uint64_t *)(SIO_BASE + SIO_MTIMECMP_OFFSET))

// SIO_BASE is already per-core — no hart index offset needed
#define STK_RISCV_CLINT_MTIMECMP_PER_HART  0

// ISR handler names from pico-sdk crt0_riscv.S
#define STK_SYSTICK_HANDLER  isr_riscv_machine_timer
#define STK_SVC_HANDLER      isr_riscv_machine_exception

// ISR handlers must not be in XIP flash on RP2350
#define STK_RISCV_ISR_SECTION  __not_in_flash("stk")

#endif /* STK_CONFIG_H_ */
```

**`stk_config.h` for RP2350 (dual-core, both cores running STK):**
```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <stdint.h>
#include <risc-v/encoding.h>
#include <pico.h>

// Select RISC-V architecture port
#define _STK_ARCH_RISC_V

// Enable both cores
#define STK_ARCH_CPU_COUNT    2
#define STK_ARCH_GET_CPU_ID() (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET)) // equivalent to get_core_num()

// RP2350: mtime and mtimecmp via SIO (per-core, no hart offset)
#define STK_RISCV_CLINT_MTIME_ADDR    ((volatile uint64_t *)(SIO_BASE + SIO_MTIME_OFFSET))
#define STK_RISCV_CLINT_MTIMECMP_ADDR ((volatile uint64_t *)(SIO_BASE + SIO_MTIMECMP_OFFSET))
#define STK_RISCV_CLINT_MTIMECMP_PER_HART  0

// ISR handler names from pico-sdk crt0_riscv.S
#define STK_SYSTICK_HANDLER  isr_riscv_machine_timer
#define STK_SVC_HANDLER      isr_riscv_machine_exception

// ISR handlers must not be in XIP flash on RP2350
#define STK_RISCV_ISR_SECTION  __not_in_flash("stk")

#endif /* STK_CONFIG_H_ */
```

For sub-microsecond timing with `mcycle` (optional, requires accurate `SystemCoreClock`):
```cpp
#define STK_SUBMICORSECOND_PRECISION_TIMER  1
#define STK_SYSTEM_CORE_CLOCK_FREQUENCY     150000000U  // must match actual PLL output
```

---

### ESP32-H2 / ESP32-C6

- [ESP32-H2 Technical Reference Manual](https://documentation.espressif.com/esp32-h2_technical_reference_manual_en.pdf)
- [ESP32-C6 Technical Reference Manual](https://documentation.espressif.com/esp32-c6_technical_reference_manual_en.pdf)

**`stk_config.h` for ESP32-H2 / ESP32-C6:**
```cpp
#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include <risc-v/encoding.h>
#include <esp_attr.h>

// Select RISC-V architecture port
#define _STK_ARCH_RISC_V

// ESP32-H2/C6 CLINT addresses
#define STK_RISCV_CLINT_BASE_ADDR      0x2000000U
#define STK_RISCV_CLINT_MTIME_ADDR     (STK_RISCV_CLINT_BASE_ADDR + 0x1808U)  // MTIME + MTIMEH
#define STK_RISCV_CLINT_MTIMECMP_ADDR  (STK_RISCV_CLINT_BASE_ADDR + 0x1810U)  // MTIMECMP + MTIMECMPH

// ISR handler names (match ESP-IDF vector table)
#define STK_SYSTICK_HANDLER  m_timer_interrupt_handler
#define STK_SVC_HANDLER      exception_handler

// Place ISR handlers in IRAM (required by ESP-IDF for interrupt handlers)
#define STK_RISCV_ISR_SECTION  IRAM_ATTR

#endif /* STK_CONFIG_H_ */
```

---

## Frequently Asked Questions

**`HiResClock::GetCycles()` always returns 0**

The `mcycle` counter is inhibited. This is the hardware default on some MCUs (e.g. RP2350 sets `mcountinhibit = 0x5`). STK clears the inhibit bit automatically in `PlatformRiscV::Initialize()` when `STK_SUBMICORSECOND_PRECISION_TIMER 1` is set. Ensure `Initialize()` has been called before the first `GetCycles()` call.

**`GetTimeUs()` returns wrong values**

Check that `STK_TIMER_CLOCK_FREQUENCY` (for the `mtime` backend) or `STK_SYSTEM_CORE_CLOCK_FREQUENCY` / `SystemCoreClock` (for the `mcycle` backend) match your actual hardware clock. The two clocks are independent on most MCUs — the CLINT timer is typically 1 MHz regardless of the CPU PLL.

**Task sleep durations are proportionally wrong**

`STK_TIMER_CLOCK_FREQUENCY` is incorrectly set to the CPU frequency instead of the `mtime` reference frequency. The scheduler uses `STK_TIMER_CLOCK_FREQUENCY` to program `mtimecmp` increments. On RP2350 and ESP32 this must be `1000000U` (1 MHz).

**Scheduler never starts / no timer interrupts**

Verify `STK_RISCV_CLINT_MTIME_ADDR` and `STK_RISCV_CLINT_MTIMECMP_ADDR` against your MCU's datasheet. Also confirm that the vector table symbol names match `STK_SYSTICK_HANDLER` and `STK_SVC_HANDLER`.

**On RP2350: timer fires on one core but not the other**

The RP2350 exposes `mtime`/`mtimecmp` through `SIO_BASE` which is a core-private window — each core sees its own registers at the same address. Make sure `STK_RISCV_CLINT_MTIMECMP_PER_HART` is set to `0`. If it is `1`, STK will apply a hart-index offset to `MTIMECMP_ADDR`, corrupting the address on both cores.

**On RP2350: hard fault or crash immediately on `Start()`**

ISR handlers are executing from XIP flash. Define `STK_RISCV_ISR_SECTION __not_in_flash("stk")` to place them in RAM. The RP2350 XIP flash controller cannot be accessed from an interrupt handler during certain flash operations, which causes a hard fault.

**Linker error: undefined reference to `riscv_mtvec_mti` (or similar)**

Your vector table uses different symbol names. Override the handler name defines to match:
```cpp
#define STK_SYSTICK_HANDLER  your_mti_handler_name
#define STK_SVC_HANDLER      your_exception_handler_name
```
On RP2350 with pico-sdk the correct names are `isr_riscv_machine_timer` and `isr_riscv_machine_exception` (defined in `crt0_riscv.S`).

**Stack overflow or memory corruption on first context switch**

Check `STK_STACK_SIZE_MIN`. On RV32I/RV64I the minimum is 256 words due to the 32-register save frame. Smaller values will silently corrupt adjacent memory on the first context switch.
