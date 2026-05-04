# STK Low-Power Demo — User Manual

**SuperTinyKernel RTOS · STK C API · STM32F407G-DISC1**

Source: [`build/example/low_power/example.c`](https://github.com/SuperTinyKernel-RTOS/stk/blob/main/build/example/low_power_c/example.c)

Eclipse project: [`build/example/project/eclipse/stm/low_power_c-stm32f407g-disc1`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/stm/low_power_c-stm32f407g-disc1)

> A complete walkthrough of `example.c`: how the demo is structured, how the three-tier low-power strategy works, and how to tune or port it to your own project.

---

## Table of Contents

1. [Overview](#overview)
2. [Hardware](#hardware)
3. [Prerequisites](#prerequisites)
4. [Project Structure](#project-structure)
5. [Configuration (`stk_config.h`)](#configuration-stk_configh)
6. [How Ultra-Low Power Works](#how-ultra-low-power-works)
   - [The Idle Hook: `IEventOverrider::OnSleep()`](#the-idle-hook-ieventoverrideronsleep)
   - [Tier 1 — Scheduler Idle (≤ 8 ticks)](#tier-1--scheduler-idle--8-ticks)
   - [Tier 2 — CPU SLEEP Mode (9–39 ticks)](#tier-2--cpu-sleep-mode-939-ticks)
   - [Tier 3 — CPU STOP Mode (≥ 40 ticks)](#tier-3--cpu-stop-mode--40-ticks)
   - [Kernel-Suspended Deep Sleep](#kernel-suspended-deep-sleep)
7. [Tickless Idle (`KERNEL_TICKLESS`)](#tickless-idle-kernel_tickless)
8. [RTC Wakeup Timer](#rtc-wakeup-timer)
9. [ADC Power Management](#adc-power-management)
10. [PLL / Clock Restore after STOP](#pll--clock-restore-after-stop)
11. [Task Architecture](#task-architecture)
    - [LED Tasks](#led-tasks)
    - [ADC Timer + Log Task](#adc-timer--log-task)
12. [USER Button: Suspend / Resume](#user-button-suspend--resume)
13. [Kernel Setup in `RunExample()`](#kernel-setup-in-runexample)
14. [Sleep Trap Stack](#sleep-trap-stack)
15. [LED Indicators at a Glance](#led-indicators-at-a-glance)
16. [Tuning for Your Application](#tuning-for-your-application)
17. [Porting to Other Cortex-M Devices](#porting-to-other-cortex-m-devices)
18. [Troubleshooting](#troubleshooting)

---

## Overview

This demo shows how to achieve **ultra-low standby current** on an STM32F407 while running a fully preemptive RTOS. The key insight is that the CPU is idle most of the time: three LED tasks each sleep for 1 second between blinks, and a temperature-sensor timer fires only every 2 seconds. By hooking into STK's idle callback (`OnSleep`), the demo places the CPU into the deepest power state the current idle window allows, then wakes it automatically at the exact tick the next task needs to run.

The result is a CPU that is actively executing for only a few microseconds per second, spending the rest of its time in either SLEEP or STOP mode.

---

## Hardware

| Signal | MCU pin | Notes |
|---|---|---|
| USER button | PA0 | Active-HIGH; board has external pull-down |
| LED GREEN | PD12 | |
| LED ORANGE | PD13 | |
| LED RED | PD14 | |
| LED BLUE | PD15 | Used as "kernel suspended" indicator |
| Temperature sensor | ADC1 ch 16 | Internal; TSVREFE must be enabled |

All peripherals used in the demo are on-chip or on-board — no external components are required beyond the STM32F407G-DISC1 board itself.

---

## Prerequisites

- **STK RTOS** source checked out or linked into your project (see [github.com/SuperTinyKernel-RTOS](https://github.com/SuperTinyKernel-RTOS)).
- ARM Cortex-M4 toolchain (e.g. `arm-none-eabi-g++`).
- CMSIS device headers for STM32F4 (`cmsis_device.h` and the STM32F4xx register map).

---

## Project Structure

```
example.c          – Main demo: tasks, low-power overrider, entry point
example.h          – Board-specific declarations (Led::Id, etc.)
stk_config.h       – Kernel compile-time configuration (you edit this)
```

---

## Configuration (`stk_config.h`)

```cpp
// Tell STK which CPU architecture is in use.
#define _STK_ARCH_ARM_CORTEX_M

// Set to (1) to enable tickless idle; (0) for always-on SysTick.
#define STK_TICKLESS_IDLE (1)

// OnSleep() does non-trivial work; give the sleep trap enough stack.
#define STK_SLEEP_TRAP_STACK_SIZE (256)
```

These three defines are the only changes needed in `stk_config.h` to enable the full low-power feature set shown in the demo. Everything else (interrupt vector names, etc.) can remain at its default.

---

## How Ultra-Low Power Works

### The Idle Hook: `IEventOverrider::OnSleep()`

STK calls `OnSleep(sleep_ticks)` every time **all tasks are sleeping** and the CPU would otherwise spin-wait until the next tick. The argument `sleep_ticks` is the number of kernel ticks until the earliest scheduled task wake-up.

By providing a `stk_event_overrider_t` struct with an `on_sleep` callback and registering it with `stk_kernel_set_event_overrider(kernel, &overrider)`, the application completely replaces the default idle path. Returning `true` signals to the platform driver that the application handled the idle period; returning `false` tells the driver to re-enter its own idle path after the call.

The `OnSleep` function in `example.c` implements a three-tier strategy based on the value of `sleep_ticks`:

```
sleep_ticks ≤ 8     →  Tier 1: stay in scheduler idle (no sleep instruction)
sleep_ticks 9–39    →  Tier 2: CPU SLEEP (__WFI, SysTick wakes)
sleep_ticks ≥ 40    →  Tier 3: CPU STOP (RTC wakeup timer, PLL restore)
```

### Tier 1 — Scheduler Idle (≤ 8 ticks)

For very short idle windows, entering and exiting a hardware sleep mode costs more time than it saves. The overrider issues a plain `CpuEnterSleepMode()` (which executes `__WFI`) and returns `true` so STK loops back into `OnSleep` if the CPU is still idle. The SysTick interrupt fires at the next tick boundary and wakes the CPU normally.

### Tier 2 — CPU SLEEP Mode (9–39 ticks)

When the idle window is long enough to benefit from a sleep instruction but short enough that stopping the PLL is not worthwhile:

1. **ADC is suspended** (`ADC1->CR2 &= ~ADC_CR2_ADON`) to cut its ~1 mA quiescent draw.
2. `CpuEnterSleepMode()` clears `SLEEPDEEP` in `SCB->SCR` and executes `__WFI`.
3. The next SysTick interrupt (or any other unmasked interrupt) wakes the CPU.
4. **ADC is resumed** before returning so the ADC timer finds it ready.

The PLL remains running throughout; no clock restore is needed. Wake-up latency is typically a few CPU cycles.

### Tier 3 — CPU STOP Mode (≥ 40 ticks)

For idle windows of 40 ms or longer, the demo enters STM32 STOP mode — the deepest sleep state that still preserves SRAM and register contents:

1. **RTC wakeup timer is armed** via `Board::RtcWakeupArm(sleep_ticks)`. The timer runs on LSI (~32 kHz) through the RTC/16 prescaler, giving a resolution of 0.5 ms per count. It is set to fire one tick early (the guard) so SysTick can close the residual timing gap.
2. **The kernel is suspended** (`kernel->Suspend()`), which stops the SysTick counter and prevents any accidental tick accounting while the CPU is powered down.
3. **ADC is suspended** to eliminate its quiescent current.
4. **`CpuEnterDeepSleepMode()`** is called: it sets `SLEEPDEEP` in `SCB->SCR`, issues `__WFI`, and when the RTC (or any other unmasked interrupt) fires, immediately calls `RestorePllClock()` to bring HSE + PLL back to 168 MHz before returning.
5. **RTC wakeup timer is disarmed** (safe to call even if it already fired).
6. **ADC is resumed**.
7. **The kernel is resumed** with `kernel->Resume(sleep_ticks)`, which advances the tick counter by exactly the number of ticks that were slept, preventing time skew.
8. `OnSleep` returns `false` so the platform driver waits in a regular idle until the next context switch.

### Kernel-Suspended Deep Sleep

When the USER button suspends the kernel (`g_KernelSuspended = true`), `OnSleep` skips arming the RTC timer and skips suspending/resuming the kernel — because the kernel is already suspended. The CPU enters STOP mode and will only wake when the USER button EXTI0 interrupt fires again. This gives the maximum possible power saving during a user-initiated pause.

---

## Tickless Idle (`KERNEL_TICKLESS`)

Setting `STK_TICKLESS_IDLE (1)` in `stk_config.h` enables `KERNEL_TICKLESS` in the kernel mode flags via `STK_C_KERNEL_MODE`:

```c
#define STK_C_KERNEL_MODE \
    (KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0))
```

In tickless mode, STK **suppresses SysTick firings** while all tasks are sleeping. Without this, SysTick would wake the CPU every millisecond, defeating the purpose of STOP mode. With it, the CPU only wakes when the RTC timer (or an external interrupt) fires, multiplying the power benefit of `OnSleep()` considerably.

---

## RTC Wakeup Timer

The RTC wakeup timer is the backbone of Tier 3 sleep. Its key properties:

- **Survives STOP mode** — unlike SysTick, the RTC wakeup counter keeps running when the main PLL and most peripherals are powered down.
- **Clock source**: LSI (~32 kHz), divided by 16 via `WUCKSEL = 000`. Effective rate: 2000 counts/s → 0.5 ms per count.
- **Conversion**: `WUTR = sleep_ticks × 2 − guard − 1` where the guard is 2 counts (1 tick) and the `−1` accounts for the STM32 hardware convention that the timer fires after `WUTR + 1` cycles.
- **Wake-up path**: The RTC wakeup line is routed to EXTI line 22. `RTC_WKUP_IRQHandler` clears both the RTC flag and the EXTI pending bit; no application work is done there.

### Initialization sequence (`RtcWakeupInit`)

1. Start LSI and wait for `LSIRDY`.
2. Unlock backup domain (`PWR->CR |= PWR_CR_DBP`), select LSI as RTC clock source (`RTCSEL = 10`).
3. Enable the RTC peripheral clock.
4. Disable write protection (magic keys `0xCA`, `0x53`), enter init mode, set prescalers, exit init mode, re-enable write protection.
5. Configure EXTI line 22 for rising-edge interrupt mode and enable `RTC_WKUP_IRQn` in NVIC at priority 12.

---

## ADC Power Management

The on-chip temperature sensor is read via ADC1 channel 16. The ADC has a non-trivial quiescent current when powered (`ADON = 1`), so the demo aggressively powers it down during every sleep period — even Tier 2 SLEEP — and restores it on wake-up.

| Function | What it does |
|---|---|
| `AdcInit()` | Enables ADC1 clock, sets TSVREFE, configures 12-bit single-shot, 480-cycle sample time on ch 16, asserts ADON and waits for stabilisation |
| `AdcSuspend()` | Clears `ADC_CR2_ADON` — ADC powers down immediately |
| `AdcResume()` | Sets `ADC_CR2_ADON` and spins ~3 µs at 168 MHz for stabilisation before returning |
| `AdcRead()` | Calls `AdcResume()`, triggers a software conversion, spins on `EOC`, returns the 12-bit result |

`TEMP_ValueToDeg()` converts the raw ADC count to degrees Celsius using a linear approximation anchored at 1060 counts = 26 °C with a 2.5 mV/°C slope.

---

## PLL / Clock Restore after STOP

Entering STOP mode on STM32F4 silently switches the system clock to HSI (16 MHz) and powers down HSE and the main PLL. `RestorePllClock()` reverses this:

```
1. Set RCC_CR_HSEON and poll HSERDY.
2. Set RCC_CR_PLLON and poll PLLRDY.
3. Write RCC_CFGR_SW_PLL and poll SWS until the hardware confirms the switch.
```

All PLL multiplier/divider fields and FLASH wait-states are preserved by hardware across STOP — `RestorePllClock` deliberately does not touch them. After it returns, HCLK is back at 168 MHz. This function is called from inside `CpuEnterDeepSleepMode()` immediately after `__WFI` returns, with interrupts still disabled, so neither the scheduler nor any ISR can observe the transitional HSI frequency.

---

## Task Architecture

### LED Tasks

Three LED task instances share a single `stk_ef_t` event flags object (`g_TaskFlags`). Each task:

1. Waits for its own flag bit with `stk_ef_wait(g_TaskFlags, my_flag, STK_EF_OPT_WAIT_ANY, STK_WAIT_INFINITE)`.
2. Atomically switches the active LED inside a `stk_critical_section_enter` / `stk_critical_section_exit` pair.
3. Sleeps for exactly 1 second using `stk_sleep_until(g_Timeline += stk_ticks_from_ms(1000))` — drift-free because the deadline is absolute, not relative.
4. Sets the next task's flag bit with `stk_ef_set()` and loops.

This round-robin handoff means only one LED is on at any given time, and each task sleeps for ~1 second, creating large idle windows that trigger Tier 3 STOP mode.

### ADC Timer + Log Task

The ADC is sampled by a `stk::time::TimerHost::Timer` subclass (`AdcTimer`) that fires every 2 seconds. This replaces a dedicated ADC task, saving one kernel task slot. The timer callback:

- Calls `Board::AdcRead()` (blocking spin on `EOC`, completes in < 5 µs).
- Pushes an `AdcSample{timestamp, raw}` into `g_AdcPipe` using `TryWrite()` (non-blocking — never stalls the timer handler task).

`LogTask<ACCESS_PRIVILEGED>` blocks indefinitely on `g_AdcPipe.Read(sample, GetTicksFromMs(10000))` with a 10-second timeout. On each received sample it converts the raw ADC value to degrees Celsius and prints it via `printf`. If no sample arrives within 10 seconds it prints a warning heartbeat so the console stays active.

**Why a pipe and not a shared variable?** The pipe decouples producer and consumer timing, is thread-safe without explicit locking, and allows the log task to process samples at its own pace without blocking the timer callback.

---

## USER Button: Suspend / Resume

`EXTI0_IRQHandler` is triggered on a rising edge of PA0 (button press). It toggles between two states:

**Running → Suspended**
```c
stk_kernel_suspend(g_Kernel);   // stops SysTick; returns ticks to next wake-up
g_KernelSuspended = true;
LedSwitchOnExclusive(LED_BLUE); // BLUE LED = system paused
```

**Suspended → Running**
```c
stk_kernel_resume(g_Kernel, 0); // restart scheduling; 0 = "woke from indefinite sleep"
g_KernelSuspended = false;
LedInit();                       // all LEDs off; tasks will control them again
```

While suspended:
- All LED tasks and the ADC timer are frozen.
- `OnSleep` detects `g_KernelSuspended` and enters STOP mode without arming the RTC timer — the CPU will only wake on the next button press (EXTI0) or another external interrupt.

> **Note**: The demo omits button debouncing for clarity. Add a software debounce (e.g. a short delay or a state-machine filter) before using this pattern in production.

---

## Kernel Setup in `RunExample()`

```c
/* Kernel type is configured in stk_config.h via STK_C_KERNEL_TYPE_CPU_0:
 *   KERNEL_STATIC | KERNEL_SYNC | KERNEL_TICKLESS
 *   4 user tasks + TimerHost::TASK_COUNT
 *   SwitchStrategyRR, PlatformDefault                */
g_Kernel = stk_kernel_create(0);
stk_kernel_init(g_Kernel, STK_PERIODICITY_DEFAULT);
```

**Initialization order matters:**

| Step | Call | Reason |
|---|---|---|
| 1 | `LedInit()` / `AdcInit()` / `ButtonInit()` / `RtcWakeupInit()` | Peripherals ready before any task accesses them |
| 2 | `stk_kernel_init(g_Kernel, ...)` | Must precede task/timer registration |
| 3 | `stk_timerhost_init(timer_host, g_Kernel, true)` | Must follow `stk_kernel_init()`, precede `stk_kernel_start()` |
| 4 | `stk_timer_start(timer_host, adc_timer, period, period)` | Timer registered before scheduler starts |
| 5 | `stk_kernel_set_event_overrider(g_Kernel, &lp_overrider)` | Must precede `stk_kernel_start()` |
| 6 | `stk_kernel_add_task(g_Kernel, task_*)` | All tasks added before start |
| 7 | `stk_kernel_start(g_Kernel)` | Never returns in `KERNEL_STATIC` mode |

---

## Sleep Trap Stack

`OnSleep` executes in the context of the kernel's internal sleep trap task. Because this demo's `OnSleep` does non-trivial work (function calls, local variables, PLL restore), the default sleep trap stack is too small. The configuration sets:

```cpp
#define STK_SLEEP_TRAP_STACK_SIZE (256)
```

A runtime assert inside `OnSleep` enforces this:

```c
assert(STK_SLEEP_TRAP_STACK_SIZE >= 256);
```

To check actual stack utilisation, inspect `Kernel::m_sleep_trap->memory` at runtime — bytes not touched by the stack are filled with `STK_STACK_MEMORY_FILLER`. If more than ~80% of the sleep trap stack is consumed, increase `STK_SLEEP_TRAP_STACK_SIZE`.

---

## LED Indicators at a Glance

| LED | State | Meaning |
|---|---|---|
| RED | ON (1 s) | LED task 0 is active |
| GREEN | ON (1 s) | LED task 1 is active |
| ORANGE | ON (1 s) | LED task 2 is active |
| BLUE | ON (steady) | Kernel suspended (USER button was pressed) |
| All OFF | — | CPU is in SLEEP or STOP mode between LED transitions |

---

## Tuning for Your Application

### Adjusting the sleep tier thresholds

In `OnSleep()`:

```cpp
enum {
    TICKS_IDLE_SLEEP = 8,   // below this: no sleep instruction
    TICKS_DEEP_SLEEP = 40   // at or above this: STOP mode
};
```

- **Lower `TICKS_IDLE_SLEEP`** if your tasks have sub-millisecond timing requirements and you want to avoid even a plain `__WFI`.
- **Lower `TICKS_DEEP_SLEEP`** if your application has shorter task periods but you still want STOP mode savings. Be aware that STOP entry/exit has a fixed latency (HSE startup + PLL lock time, typically 2–4 ms on the DISC1) — don't set this threshold lower than the STOP mode overhead.
- **Raise `TICKS_DEEP_SLEEP`** if your latency budget is tight and you cannot afford PLL restore time on wake.

### Changing the ADC sample rate

```c
uint32_t adc_period = (uint32_t)stk_ticks_from_ms(2000); // currently 2 s
stk_timer_start(timer_host, adc_timer, adc_period, adc_period);
```

Increase the period to sample less frequently; decrease it to sample more often. Note that a shorter period reduces the idle window and may push more wake-ups out of Tier 3 into Tier 2.

### Adding more peripherals

Follow the ADC pattern: add `Suspend()` and `Resume()` calls in `OnSleep()` around the sleep instruction so peripherals are powered down for the duration of each sleep period.

### Disabling tickless idle

Set `STK_TICKLESS_IDLE (0)` in `stk_config.h`. SysTick will fire every millisecond and the CPU will wake every millisecond even inside STOP mode, effectively disabling Tier 3 savings.

---

## Porting to Other Cortex-M Devices

The demo is written against STM32F4 register names, but the architecture is device-agnostic. To port:

1. **RTC wakeup timer**: Replace `RtcWakeupInit`, `RtcWakeupArm`, `RtcWakeupDisarm`, and `RTC_WKUP_IRQHandler` with your device's equivalent timed wake-up source that survives STOP/Deep Sleep. On devices without an RTC, a LPTIM or WDT may serve this role.
2. **PLL restore**: Replace `RestorePllClock` with the correct sequence for your device's clock tree.
3. **ADC suspend/resume**: Adapt the `ADC1->CR2` register writes to your ADC's power-control interface.
4. **GPIO and EXTI**: Adapt `ButtonInit` to your button GPIO and EXTI configuration.
5. **`stk_config.h`**: Keep `_STK_ARCH_ARM_CORTEX_M` defined; adjust handler name macros if your startup file uses non-standard names.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| CPU never enters STOP mode | `sleep_ticks` never reaches 40 | Increase task sleep periods or lower `TICKS_DEEP_SLEEP` |
| `HardFault` on wake from STOP | PLL not restored before first peripheral access | Ensure `RestorePllClock()` completes inside `CpuEnterDeepSleepMode()` before any interrupt handler runs |
| Time drifts after STOP wake | `kernel->Resume()` called with wrong tick count | Always pass the original `sleep_ticks` value from `OnSleep` to `kernel->Resume()` |
| `STK_STATIC_ASSERT` failure at compile time | Sleep trap stack too small | Increase `STK_SLEEP_TRAP_STACK_SIZE` in `stk_config.h` |
| BLUE LED stays on after button press | Debounce issue causing double-toggle | Add software debounce logic in `EXTI0_IRQHandler` |
| ADC reads garbage after STOP wake | ADC stabilisation delay too short | Increase the spin count in `AdcResume()` |
| `printf` output stops when kernel is suspended | `LogTask` is frozen | Expected: the 10 s timeout will print a warning once the kernel resumes |

---

*SuperTinyKernel RTOS — © 2022-2026 Neutron Code Limited. MIT License.*
