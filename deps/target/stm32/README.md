# STM32F0/F1/F4 target for SuperTinyKernel (STK)

Bare-metal start-up code, linker scripts, exception handlers and a minimal C library back-end for building STK
applications on STM32F0 (Cortex-M0), STM32F1 (Cortex-M3), STM32F4 (Cortex-M4F) and STM32F7 (Cortex-M7) with the GNU Arm toolchain. It is the
`deps/target/stm32fx` dependency of the STK repository and is built as a static library named `stm32fx`.

All code in this directory is written for STK. CMSIS-Core, the CMSIS device package (device headers, system file) and
the HAL driver are **not** stored here: they are fetched from the repositories of STMicroelectronics:

```
python3 deps/scripts/fetch_deps.py stm32-f4     # F0: stm32-f0, F1: stm32-f1, F7: stm32-f7, everything: stm32
```

They are placed to `deps/stm32/{cmsis-core,f0,f1,f4,f7}` at the versions pinned in `deps/scripts/deps.py`.

## Layout

```
stm32fx/
├── CMakeLists.txt             builds the `stm32fx` static library
├── ProjectInclude.cmake       family/device selection, include paths, link directories, definitions, _STK_DEVICE_INC
├── ProjectIncludeLib.cmake    linker scripts, adds `stm32fx` to TARGET_DEPS / TARGET_LIBS
├── include/
│   ├── cmsis_device.h         device header (_STK_DEVICE_INC): includes stm32f0xx.h / stm32f1xx.h / stm32f4xx.h
│   ├── stm32_assert.h         assert_param() of the HAL
│   ├── stm32f{0,1,4,7}xx_hal_conf.h   HAL configuration: the template of the fetched HAL driver
│   └── diag/trace.h           trace channel (trace_printf, trace_puts, ...)
├── src/
│   ├── init/
│   │   ├── startup.c          Reset_Handler, _start (C runtime start-up)
│   │   ├── hardware.c         hardware init hooks, SystemClock_Config
│   │   ├── exceptions.c/.h    core exception handlers, fault reporting
│   │   └── vectors_<device>.c vector tables (generated, see below)
│   ├── libc/
│   │   ├── system.c           _sbrk, _exit, abort, __assert_func, assert_failed
│   │   └── syscalls.c         newlib system call stubs, _write -> trace
│   ├── diag/
│   │   ├── trace.c            trace channel: semihosting or ITM
│   │   └── semihosting.h      ARM semihosting call
│   └── ld/
│       ├── sections.ld        sections, common for all families
│       ├── sections-mpu.ld    sections.ld + MPU (STK_MPU) regions, Cortex-M3/M4
│       └── stm32f{0,1,4,7}/mem.ld   memory map of the family
└── tools/gen_vectors.py       vector table generator
```

Built into the library from the fetched sources: `system_stm32fXxx.c` (CMSIS device) and the HAL modules `hal`,
`hal_cortex`, `hal_rcc`, `hal_rcc_ex`. Add the sources of other HAL modules to the application if it uses them.

## Build integration

| File | What it does |
|---|---|
| `ProjectInclude.cmake` | Selects the family from `TARGET_CORTEX_M0/M3/M4/M7` and the device from `TARGET_CPU_FAMILY`; adds the include paths of the target and of the fetched ST packages; adds `-DSTM32F0/F1/F4/F7 -D<device> -DUSE_HAL_DRIVER -DUSE_FULL_ASSERT -DHSE_VALUE=8000000` and the trace definitions; sets `_STK_DEVICE_INC` to `cmsis_device.h` |
| `ProjectIncludeLib.cmake` | Adds `-T mem.ld -T sections.ld` (`sections-mpu.ld` if `STK_MPU` is set) to the link flags, links `stm32fx` with `--whole-archive` so the strong/weak overrides are never dropped |
| `CMakeLists.txt` | Compiles the target and the fetched ST sources into `stm32fx`; defines `STK_MPU=1` for it when `STK_MPU` is set |

Default devices (`TARGET_CPU_FAMILY`): `STM32F051x8` (M0), `STM32F103xB` (M3), `STM32F407xx` (M4), `STM32F746xx` (M7). The link must use
`-nostartfiles` and `--gc-sections` (the HAL objects reference modules which are not built).

`STK_STM32FX_TRACE` selects the trace channel: `semihosting` (default), `itm` (Cortex-M3/M4, SWO) or `none`.

### Other devices

A vector table is needed for every device. Tables for the default devices are included; generate another one from the
start-up file of the CMSIS device package:

```
python3 tools/gen_vectors.py ../../stm32/f4/cmsis-device/Source/Templates/gcc/startup_stm32f411xe.s \
        -o src/init/vectors_stm32f411xe.c
```

then configure with `-DTARGET_CPU_FAMILY=STM32F411xE`. Adjust the memory sizes with `--defsym` (below) or `mem.ld`.
The clock settings of `SystemClock_Config()` (hardware.c) are for an 8 MHz crystal: 168 MHz for F405/407/415/417/427/
429/437/439/446 and all F7 (no over-drive needed), 84 MHz for the other F4 devices. F0 and F1 stay on the default HSI clock (8 MHz); override the weak
`SystemClock_Config()` to change it.

## Start-up sequence

`Reset_Handler` sets MSP and jumps to `_start`, which:

1. disables interrupts;
2. calls `__initialize_hardware_early()`: `SystemInit()`, VTOR (M3/M4), FPU access (no RAM is usable yet);
3. copies `.data` from flash, zeroes `.bss` (and the same for CCM/DTCM RAM on F4/F7);
4. calls `__initialize_hardware()`: `HAL_Init()`, `SystemClock_Config()`, `SystemCoreClockUpdate()`;
5. calls `__initialize_args()` (weak, one empty argument), then runs `.preinit_array` / `.init_array`;
6. enables interrupts and calls `main(argc, argv)`;
7. runs `.fini_array` and calls `_exit()` (release: device reset; DEBUG: busy loop).

`__initialize_hardware_early`, `__initialize_hardware`, `SystemClock_Config`, `__initialize_args`, `_start`, `_exit`,
`abort`, `assert_failed` and the system calls are weak and can be replaced by the application. The `.init`/`.fini`
sections are not executed (they need `crti.o`/`crtn.o`).

## Linker scripts

`mem.ld` (family) defines `FLASH`, `RAM` and `CCMRAM` (zero length if the device has none); `sections.ld` lays out the
vector table, code, read-only data, init arrays, `.data`/`.bss`/`.noinit`, the heap and the main stack.

| Family | Default flash | Default RAM | CCM RAM |
|---|---|---|---|
| STM32F0 | 64 KiB | 8 KiB | – |
| STM32F1 | 128 KiB | 20 KiB | – |
| STM32F4 | 1 MiB | 128 KiB | 64 KiB (CCM) |
| STM32F7 | 1 MiB | 256 KiB (SRAM1+SRAM2 at `0x20010000`) | 64 KiB (DTCM at `0x20000000`) |

Options (`--defsym`):

| Symbol | Default | Meaning |
|---|---|---|
| `STK_FLASH_SIZE` | per family | size of `FLASH` |
| `STK_RAM_SIZE` | per family | size of `RAM` |
| `__stack_size` | `0x400` | main stack (exceptions, start-up, main until the kernel switches stacks), multiple of 8 |

```
-Xlinker --defsym=STK_RAM_SIZE=0x4000 -Xlinker --defsym=__stack_size=0x800
```

> **Order matters.** GNU ld applies options in command-line order, so `--defsym` must come **before** `-T mem.ld`.
> Put them in `CFLAGS`/`CXXFLAGS` (`-Xlinker --defsym=...`), not in `CMAKE_EXE_LINKER_FLAGS`.

The heap (`__heap_start`..`__heap_end`) is all RAM between the static data and the main stack; the link fails if they
do not fit. Variables are placed in CCM RAM with the sections `.data.CCMRAM` (on F7 the region named CCMRAM is the DTCM), `.bss.CCMRAM`, `.noinit.CCMRAM`.

### MPU variant: `sections-mpu.ld`

Configure with `-DSTK_MPU=ON` to link with `sections-mpu.ld` and make the start-up code initialize the MPU regions
(copy of the initialized data from flash, zeroing of the bss). The script lays out the sections `.stk_mpu_shared_code`,
`.stk_mpu_kernel_code`, `.stk_mpu_shared_data` (+ `_bss`) and `.stk_mpu_kernel_data` (+ `_bss`) so that each is one
PMSAv7 region: power-of-two size, base aligned to the size. Each is padded to a fixed *window*:

| Region | Memory | Default window | Tune with |
|---|---|---|---|
| shared code | flash | 8 KiB | `STK_MPU_SHARED_CODE_WINDOW` |
| kernel code | flash | 4 KiB | `STK_MPU_KERNEL_CODE_WINDOW` |
| shared data + bss | RAM | 16 KiB | `STK_MPU_SHARED_RAM_WINDOW` |
| kernel data + bss | RAM | 4 KiB | `STK_MPU_KERNEL_RAM_WINDOW` |

The windows must equal the sizes programmed into the MPU regions by the application; the link fails if a content does
not fit. The default windows need more RAM than STM32F0/F1 devices have: on the default F103 (20 KiB) reduce them,
e.g. `-Xlinker --defsym=STK_MPU_SHARED_RAM_WINDOW=0x1000` (before `-T`, see above). The symbols
`__stk_mpu_{shared,kernel}_{code,data}_{start,end}` describe the regions; the full description of the layout is at the
top of the script.

## Interrupts and faults

Vector tables are in `vectors_<device>.c`. Device handlers are weak aliases of `Default_Handler`: define a function
with the CMSIS name (e.g. `TIM2_IRQHandler`) to handle an interrupt. The kernel overrides `SVC_Handler`,
`PendSV_Handler` and `SysTick_Handler`; the default `SysTick_Handler` advances the HAL tick.

`HardFault_Handler` (and `MemManage`/`BusFault`/`UsageFault` on M3/M4) find the faulting stack and call a weak C
function (`HardFault_Handler_C`, ...), which prints registers and fault status on the trace channel and stops the CPU.
With semihosting trace on M3/M4, a `BKPT` that raises a HardFault because no debugger is attached is skipped, so a
program that uses `trace_printf` still runs standalone (the text is dropped). Cortex-M0 cannot do this.

## Trace and C library

`trace_printf/puts/putchar/write` (`diag/trace.h`) go to semihosting (`SYS_WRITE0`) or ITM port 0. Output is formatted
in a 128-byte shared buffer (not re-entrant, longer lines truncated). With trace enabled, `stdout`/`stderr` of the
C library (`_write` on descriptors 1 and 2) also go there. `_sbrk` serves the heap, other system calls fail with
`ENOSYS`.

## Changes from the previous version

- The former third-party start-up, exception, trace and syscall code was replaced by the code above; HAL, CMSIS-Core and
  CMSIS device files are fetched, no longer stored.
- Linker scripts were rewritten: `mem.ld` + `sections.ld` (`libs.ld` is gone, as are the unused flash-bank,
  external-memory and memory-array regions); heap symbols are `__heap_start`/`__heap_end`.
- Trace macros are `STK_TRACE_SEMIHOSTING` / `STK_TRACE_ITM` (CMake option `STK_STM32FX_TRACE`); the semihosting-stdout
  variant was dropped. `cxx.cpp` (never built) was dropped.
- The default `SysTick_Handler` advances the HAL tick; vector tables follow the CMSIS device package of the pinned
  versions (e.g. `FSMC_IRQHandler` on F407, `FMC_IRQHandler` is an alias of it).
- Not carried over: DEBUG-only start-up guard checks, `arm_math.h`/DSP headers (not part of CMSIS-Core).
