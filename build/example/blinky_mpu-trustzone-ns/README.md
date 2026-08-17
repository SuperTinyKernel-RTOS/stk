# STK TrustZone Example — RP2350 (Cortex-M33)

**SuperTinyKernel RTOS** — Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.

This example demonstrates STK's ARM TrustZone support on the **RP2350** (Raspberry Pi Pico 2, Cortex-M33 / ARMv8-M), running a Secure-side and Non-Secure-side task set as two separate binaries that communicate across the Secure/Non-Secure boundary.

---

## Commercial Component Notice

STK's **TrustZone architecture port** (Secure/Non-Secure dual-binary kernel support) is a **commercial component** and its source is not included in the public [`stk`](https://github.com/SuperTinyKernel-RTOS/stk) repository.

To let you build and run these examples anyway, **precompiled Secure and Non-Secure TrustZone libraries are provided** for download — see [Downloading the Precompiled TrustZone Libraries](#downloading-the-precompiled-trustzone-libraries) below. No license is needed to build and run these examples for demonstration/evaluation purposes. **For use in any production project, free or commercial, a license must be obtained.** Contact the developer at [stk@neutroncode.com](mailto:stk@neutroncode.com) for licensing.

---

## Example Folders

This README applies to all four RP2350 TrustZone examples in [`build/example`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example):

| Folder | Eclipse Project Folder | Security state | MPU hardening |
| --- | --- | --- | --- |
| [`blinky-trustzone-sc`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky-trustzone-sc) | [`blinky-trustzone-sc-rp2350w`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/rpi/blinky-trustzone-sc-rp2350w) | Secure | No |
| [`blinky-trustzone-ns`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky-trustzone-ns) | [`blinky-trustzone-ns-rp2350w`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/rpi/blinky-trustzone-ns-rp2350w) | Non-Secure | No |
| [`blinky_mpu-trustzone-sc`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky_mpu-trustzone-sc) | [`blinky_mpu-trustzone-sc-rp2350w`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/rpi/blinky_mpu-trustzone-sc-rp2350w) | Secure | Yes — independent per-zone MPU |
| [`blinky_mpu-trustzone-ns`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky_mpu-trustzone-ns) | [`blinky_mpu-trustzone-ns-rp2350w`](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/rpi/blinky_mpu-trustzone-ns-rp2350w) | Non-Secure | Yes — independent per-zone MPU |

The `-sc` and `-ns` folders are counterparts of the same demo, split into their respective Secure and Non-Secure Eclipse projects — build and flash **both** the `-sc` and `-ns` project from the pair you're interested in; the Secure image is what actually boots the core and hands off to the Non-Secure image.

The `blinky_mpu-*` pair additionally enables `STK_MPU_STACK_GUARD` on both sides, so each zone is independently protected by its own hardware MPU instance, on top of the SAU/IDAU TrustZone isolation. See the [Cortex-M driver README](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/stk/include/arch/arm/cortex-m#readme) for details on TrustZone and MPU configuration.

---

## Downloading the Precompiled TrustZone Libraries

1. Download the archive:
   [`https://neutroncode.com/download/public/stk-arm-tz.zip`](https://neutroncode.com/download/stk/stk-arm-tz.zip)
2. Extract it **as a sibling of the `stk` repository folder** — not inside it. The archive contains an `stk-arm-tz` folder; after extraction your directory layout should look like:

   ```
   project/
   ├── stk/            <- this repository (contains build/example/...)
   └── stk-arm-tz/     <- extracted contents of stk-arm-tz.zip
   ```

3. Both folders must sit at the same directory level. The Eclipse projects reference the precompiled libraries via a relative path to `../../stk-arm-tz`, so if `stk-arm-tz` is missing or misplaced, the build will fail to find the TrustZone libraries.

---

## Building & Running (Eclipse)

1. Make sure `stk-arm-tz` is downloaded and extracted next to `stk` as described above.
2. In Eclipse, go to **File → Import → General → Existing Projects into Workspace** and select this example's folder.
3. Connect your RP2350 board (Raspberry Pi Pico 2) via its debug probe (e.g. Picoprobe / CMSIS-DAP or SWD).
4. From the toolbar/menu build & launch configuration selector, choose the **`Debug (Lib)`** configuration.
5. Build and start the debug session. This compiles your project's Secure/Non-Secure application code, links it against the precompiled TrustZone library, flashes the board, and starts execution.

Repeat for the matching `-sc`/`-ns` project in the pair — both must be flashed for the demo to run correctly, since the Secure image is what initializes the core and jumps to the Non-Secure image.

---

## What This Demonstrates

- STK task scheduling running independently in the Secure and Non-Secure worlds on the same core, with the kernel present in the Secure binary.
- A basic blinky task toggling a GPIO/LED from each security state, showing that both sides keep their own tasks, stacks, and (in the `blinky_mpu-*` variant) their own MPU-enforced memory isolation, without either side being able to access the other's memory.

---

## See Also

- [Main STK README](https://github.com/SuperTinyKernel-RTOS/stk#readme) — project overview, features, and general Quick Start.
- [ARM Cortex-M Port README](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/stk/include/arch/arm/cortex-m#readme) — full TrustZone and MPU configuration reference.
