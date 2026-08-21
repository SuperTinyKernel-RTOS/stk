# STK MPU Example — EVKB-IMXRT1050 (Cortex-M7)

**SuperTinyKernel RTOS** — Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.

This example demonstrates STK's Memory Protection Unit (MPU) support on the **EVKB-IMXRT1050** (NXP i.MX RT1050, Cortex-M7 / ARMv7-M PMSAv7, 16 regions), running a single Privileged task and four Non-Privileged tasks under one binary, each hardware-isolated to only the memory it needs.

Unlike STK's TrustZone port, MPU support is part of the core, freely available [`stk`](https://github.com/SuperTinyKernel-RTOS/stk) repository and requires no separate commercial library to build and run.

---

## Example Project

This example's project is **not** hosted in the core `stk` repository. It lives in the dedicated NXP examples repository:

[`https://github.com/SuperTinyKernel-RTOS/stk-examples-nxp`](https://github.com/SuperTinyKernel-RTOS/stk-examples-nxp)

That repository collects STK example projects targeting NXP microcontrollers (i.MX RT, LPC, and others), each set up as an importable MCUXpresso IDE project alongside the core `stk` sources. Clone or download `stk-examples-nxp` and look for the EVKB-IMXRT1050 MPU project folder to find this example, its board support package, and its linker script.

---

## What This Demonstrates

- A **16-region PMSAv7 MPU** configured with `STK_MPU=1`, `STK_MPU_STACK_GUARD=1`, and `STK_MPU_TASK_REGIONS=4`, giving each task an automatic hardware stack guard plus one application-defined region.
- **Static/global regions** covering FLASH, shared task data, kernel-only data/code, the exception (MSP) stack, external SDRAM, a non-cacheable DMA pool, and system control space — configured once in `PlatformEventHandler::OnConfigureMpu()`.
- **Per-task regions**: four Non-Privileged LED tasks, each granted access only to its own instance memory via `ITask::GetMpuRegions()`.
- A **Privileged task** (`SecureHwCommandQueueTask`) with its own dedicated peripheral and instance-memory regions, processing hardware commands on behalf of the Non-Privileged tasks via a lock-free pipe — showing how to delegate restricted hardware access safely instead of granting it globally.
- **Fault detection**: an intentionally-disabled line of code shows what happens if a Non-Privileged task reaches outside its granted regions (e.g. touching Privileged-only data) — a MemManage fault fires and a custom `OnException()` handler dumps CPU and MPU state for diagnostics.
- Drift-free task scheduling: four tasks hand off to each other via `EventFlags` on a precise 250 ms cadence using `SleepUntil()`, cycling an LED pattern.

---

## Building & Running (MCUXpresso IDE)

1. Make sure both `stk` and `stk-examples-nxp` are cloned/downloaded and sit side by side, as described above.
2. In MCUXpresso IDE, go to **File → Import → General → Existing Projects into Workspace** and select this example's project folder from `stk-examples-nxp`.
3. Connect your EVKB-IMXRT1050 board via its on-board debug probe (CMSIS-DAP) or an external SWD probe.
4. Select the **Debug** build configuration from the toolbar/menu.
5. Build and start the debug session. This compiles the example, links it against the core STK sources, flashes the board, and starts execution.

Watch the board's LEDs cycle in sequence, driven by the Non-Privileged tasks and serviced through the Privileged hardware command task.

---

## See Also

- [Main STK README](https://github.com/SuperTinyKernel-RTOS/stk#readme) — project overview, features, and general Quick Start.
- [ARM Cortex-M Port README](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/stk/include/arch/arm/cortex-m#readme) — full MPU and TrustZone configuration reference.
- [`stk-examples-nxp`](https://github.com/SuperTinyKernel-RTOS/stk-examples-nxp) — this and other NXP-targeted STK example projects.
