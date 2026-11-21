# RISC-V target for SuperTinyKernel (STK)

Bare-metal, single-hart start-up code, linker scripts, trap vector tables and minimal C library
back-end for building STK applications on RISC-V (RV32/RV64, GCC toolchain). It is the `deps/target/risc-v`
dependency of the STK repository and is built as a static library named `riscv`.

The default memory map is a generic one; adjust the `MEMORY` origins to your device and override sizes at
link time (see [Linker scripts](#linker-scripts)).

The kernel side of this port is selected with the `_STK_ARCH_RISC_V` macro (RV32I/RV32E/RV64, optional FPU),
defined by the build system; `stk_arch.h` then includes `arch/risc-v/stk_arch_risc-v.h`.

## Layout

```
risc-v/
├── CMakeLists.txt             builds the `riscv` static library from src/init/*.c
├── ProjectInclude.cmake       include path, link directory, device header (_STK_DEVICE_INC)
├── ProjectIncludeLib.cmake    linker script selection, adds `riscv` to TARGET_DEPS / TARGET_LIBS
├── include/risc-v/
│   └── encoding.h             CSR numbers, mstatus/mie/mip bit masks, instruction encodings
└── src/
    ├── ld/
    │   ├── stk.ld             default linker script
    │   └── stk-mpu.ld         stk.ld + PMP (MPU) regions, used when STK_MPU is enabled
    └── init/
        ├── startup.c          reset entry (_enter), C runtime start (_start), _Exit
        ├── vector_table.c/.h  mtvec / stvec / utvec vector tables and weak default handlers
        ├── system.c           _sbrk, abort, __assert_func
        └── semihosting.c      console I/O through the debugger (RISC-V semihosting)
```

## Build integration

The CMake files are consumed by the STK build system, which defines `ROOT_DIR` (the repository root):

| File | What it does |
|---|---|
| `ProjectInclude.cmake` | Adds `deps/target/risc-v/include` to the include path, `deps/target/risc-v/src/ld` to the link path, and sets `_STK_DEVICE_INC` to `risc-v/encoding.h` |
| `ProjectIncludeLib.cmake` | Adds `-T stk.ld` (or `-T stk-mpu.ld` if `STK_MPU` is set) to the C/C++ link flags, appends `riscv` to `TARGET_DEPS` and links it with `--whole-archive` so the weak/strong symbol overrides and `_enter` are never dropped |
| `CMakeLists.txt` | Compiles `src/init/*.c` into the `riscv` library; defines `STK_MPU=1` for it when `STK_MPU` is set |

Enable PMP-based memory protection by configuring with `-DSTK_MPU=ON`. This switches both the linker script and
the start-up code.

## Start-up sequence

`_enter` (section `.text.stk.init.enter`, first in ROM, the reset address) is a naked function that loads
`gp` from `__global_pointer$` (with relaxation off) and `sp` from `_sp`, then jumps to `_start`, which:

1. zeroes `.bss`;
2. copies `.data`/`.tdata` from ROM to RAM;
3. copies `.itcm` from ROM to ITCM (instruction tightly coupled memory);
4. with `STK_MPU`: copies the shared/kernel MPU data windows from ROM and zeroes their bss tails;
5. runs `.preinit_array`/`.init_array` and `__libc_init_array()` (static C++ constructors);
6. programs `mtvec` with `riscv_mtvec_table` in **vectored** mode;
7. if the core has an FPU (`__riscv_flen != 0`), enables it in `mstatus` (FS/XS);
8. calls `main(argc, argv)` (weak `_init_args()` supplies a single empty argument), then the static destructors,
   then `_Exit()`, which parks the hart in a `wfi` loop.

Notes:
- **Single hart only.** The start-up code does not initialize `tp`; when `STK_TLS` is enabled the kernel uses `tp`
  as the per-task TLS pointer (`hw::GetTls()`/`SetTls()` are single inline instructions on RISC-V).
- The `.init`/`.fini` sections are *not* executed (they need `crti.o`/`crtn.o`); use the init/fini arrays.
- `mstatus` interrupt enabling is not done here: that is left to the kernel/application.

## Linker scripts

Both scripts expose the same default regions:

| Region | Origin | Length (default) | Holds |
|---|---|---|---|
| `rom` | `0x20010000` | `0x200000` (2 MiB) | code, rodata, load images of `.data`/`.itcm` |
| `ram` | `0x80000000` | `0x40000` (256 KiB) | `.data`, `.bss`, stack, heap |
| `itcm` | `0x08000000` | `0x2000` (8 KiB) | code in `.itcm*` sections (copied from ROM at start-up) |

See [ITCM](#itcm-instruction-tightly-coupled-memory) for placing code in the `itcm` region.

### Build-time options

Pass with `--defsym`:

| Symbol | Default | Meaning |
|---|---|---|
| `STK_RAM_SIZE` | `0x40000` | Size of `ram` |
| `STK_ROM_SIZE` | `0x200000` | Size of `rom` |
| `__stack_size` | `0x8000` | Stack size; multiple of 16 (power of two with `stk-mpu.ld`) |
| `__heap_size` | `0x800` | Heap size |
| `__heap_max` | not set | If defined, the heap takes all RAM left after the stack |

```
-Xlinker --defsym=STK_RAM_SIZE=0x10000 -Xlinker --defsym=__stack_size=0x2000
```

> **Order matters.** GNU ld applies options in command-line order, so `--defsym` must come **before**
> `-T stk.ld`, or it is silently ignored. Put them in `CFLAGS`/`CXXFLAGS` (as `-Xlinker --defsym=...`), not in
> `CMAKE_EXE_LINKER_FLAGS`, which CMake places after `-T`.

The link fails (via `ASSERT`) if the stack size is misaligned or stack + heap do not fit in RAM.

### Symbols used by the start-up code and heap

`__stk_bss_start/end`, `__stk_data_load/start/end`, `__stk_itcm_load/start/end`, `__global_pointer$`, `_sp`,
`__heap_start`, `__heap_end`, plus the init/fini array bounds.

### ITCM (instruction tightly coupled memory)

ITCM is a small on-chip memory that sits next to the core and is accessed with fixed, low latency, without
flash wait states or an instruction cache. Code executed from it has predictable timing, which makes it the
place for performance- or jitter-critical routines (tick/timer ISR, context-switch helpers, tight loops).
Other vendors call it ILM, ITIM or simply TCM. It is optional: if nothing is placed in `.itcm`, the section is
empty and nothing is copied.

How it works:
- The `itcm` region (default origin `0x08000000`, 8 KiB) is declared in the `MEMORY` block of both linker
  scripts. **Adjust the origin and length to your device**; to change them edit the `MEMORY` block (they are not
  `--defsym` options).
- Everything in `.itcm` / `.itcm.*` sections is linked at an ITCM address but stored in ROM (`AT>rom`).
- At boot `_start` copies the image from `__stk_itcm_load` (ROM) to `__stk_itcm_start`..`__stk_itcm_end` (ITCM),
  after `.bss` and `.data` and before the static constructors and `main()`. Code placed in ITCM is therefore
  usable from the first constructor on.

Usage, tag a function with the section:

```cpp
#define STK_ITCM_CODE __attribute__((section(".itcm")))

STK_ITCM_CODE void FastLoop(void)
{
    // executed from ITCM
}
```

Notes:
- The code must fit into the `itcm` region, otherwise the link fails with `STK: .itcm does not fit into ITCM`
  (the linker's own memory region overflow error may also be reported).
- Interrupt handlers: the vector tables (`riscv_*tvec_table`) are placed with the normal code in ROM and every
  entry is a single `jal`, which reaches only +/-1 MiB. If the ITCM address is farther away from the table than
  that (as with the default origins), a handler placed in ITCM cannot be linked from the table directly, so
  keep handlers in ROM and call ITCM routines from them.
- With `stk-mpu.ld` the PMP windows cover ROM and RAM only. If unprivileged tasks must execute code from ITCM,
  add a PMP entry for it in `OnConfigureMpu()`.

### MPU (PMP) variant: `stk-mpu.ld`

RISC-V's MPU is the Physical Memory Protection unit. `stk-mpu.ld` lays out sections so that each region is
describable by one **NAPOT** PMP entry: power-of-two size, base naturally aligned to that size. Like ARM PMSAv7,
each section is padded to a fixed *window*, not to its actual content size.

| Region | Memory | Access (priv / user) | Default window | Tune with |
|---|---|---|---|---|
| User ROM block (`.init` … `.text`) | rom | RX / RX | 256 KiB | `STK_MPU_ROM_WINDOW` |
| `.stk_mpu_shared_code` (inside the user ROM block) | rom | RX / RX | 8 KiB | `STK_MPU_SHARED_CODE_WINDOW` |
| `.stk_mpu_kernel_code` (after the user ROM block) | rom | RX / – | 2 KiB | `STK_MPU_KERNEL_CODE_WINDOW` |
| `.stk_mpu_shared_data` + `_bss` | ram | RW / RW, no exec | 4 KiB | `STK_MPU_SHARED_RAM_WINDOW` |
| `.stk_mpu_kernel_data` + `_bss` | ram | RW / –, no exec | 8 KiB | `STK_MPU_KERNEL_RAM_WINDOW` |
| Stack (MSP/exception) | ram | RW / – | `__stack_size` | `__stack_size` |

Rules:
- Each window must be a power of two **and equal to the size you program into the PMP entry** in
  `OnConfigureMpu()`, not to the size of the content. Powers of two and content overflow are checked at link time;
  re-tune to the smallest power of two that fits using the map file.
- Data and bss of a window share one PMP entry covering `[__stk_mpu_xxx_data_start, __stk_mpu_xxx_bss_end)`.
- Symbols for `OnConfigureMpu()`: `__stk_mpu_{shared,kernel}_{code,data,bss}_{start,end}`,
  `__stk_mpu_{shared,kernel}_data_source__`, `__stk_mpu_msp_stack_{start,end}`.

#### Placing objects in the MPU regions

`stk_arch.h` defines attribute macros that put functions/variables into these sections. They are active only when
`STK_MPU` is enabled; otherwise they expand to nothing, so the same source builds either way.

| Macro | Section | Intended MPU access |
|---|---|---|
| `STK_MPU_SHARED_DATA_SECTION` | `.stk_mpu_shared_data` | read/write for privileged and user tasks, no execute |
| `STK_MPU_SHARED_BSS_SECTION` | `.stk_mpu_shared_bss` | same as shared data |
| `STK_MPU_SHARED_CODE_SECTION` | `.stk_mpu_shared_code` | execute + read-only for privileged and user tasks (secure system entry points) |
| `STK_MPU_KERNEL_DATA_SECTION` | `.stk_mpu_kernel_data` | read/write for privileged code only, no execute |
| `STK_MPU_KERNEL_BSS_SECTION` | `.stk_mpu_kernel_bss` | same as kernel data |
| `STK_MPU_KERNEL_CODE_SECTION` | `.stk_mpu_kernel_code` | execute + read for privileged code only |

```cpp
static uint8_t g_scratch[512] STK_MPU_SHARED_BSS_SECTION;   // zero-filled at boot, no flash image
static int     g_counter      STK_MPU_SHARED_DATA_SECTION = 5; // initial value copied from ROM at boot
```

- **`_BSS_` vs `_DATA_`:** variables in a `_BSS_` section must not have a non-zero initializer. No image is stored in
  flash; the start-up code zero-fills them (up to the end of the window). Prefer `_BSS_` for large or scratch buffers.
- **Kernel sections are optional** when the MPU is configured with `hw::mpu::MPU_CFG_PRIVILEGED_BG_MEM`
  (privileged code uses the background memory map); in that case do not declare them in the linker script.
- With `stk-mpu.ld`, the data and bss halves of one window are initialized by `_start` (see
  [Start-up sequence](#start-up-sequence)).

## Interrupts and traps

`vector_table.c/.h` provide three 256-byte-aligned tables, indexed by cause, each entry a single 4-byte
`jal zero, <handler>` (compressed instructions and relaxation are disabled for the tables so entries cannot shrink):

| Table | CSR | Handlers |
|---|---|---|
| `riscv_mtvec_table` | `mtvec` (programmed by `_start`) | `riscv_mtvec_exception`, `_ssi`, `_msi`, `_sti`, `_mti`, `_sei`, `_mei`, `_platform_irq0`…`irq15` (causes 16–31) |
| `riscv_stvec_table` | `stvec` | `riscv_stvec_exception`, `_ssi`, `_sti`, `_sei` |
| `riscv_utvec_table` | `utvec` | `riscv_utvec_usi`, `_uti`, `_uei` |

Only `mtvec` is set up by the start-up code; program `stvec`/`utvec` yourself if you use those modes.

Every handler is **weak**, aliased to an empty default (`riscv_nop_machine/supervisor/user`). To handle an event,
define a function with the same name and the matching interrupt attribute:

```c
extern "C" void riscv_mtvec_mti(void) __attribute__ ((interrupt ("machine")));
void riscv_mtvec_mti(void)
{
    // machine timer interrupt
}
```

In vectored mode, interrupt cause 0 shares its slot with synchronous exceptions (`riscv_*_exception`).

To supply your own platform interrupt table, define `VECTOR_TABLE_MTVEC_PLATFORM_INTS` both in `vector_table.c`
and in the application; this removes platform interrupts 0–15 from the machine table.

## C library back-end

- **`system.c`**: `_sbrk()` grows the heap between `__heap_start` and `__heap_end` (returns `-1`/`ENOMEM` when
  exhausted); `abort()` parks the hart in a `wfi` loop; `__assert_func()` prints the failure and aborts.
- **`semihosting.c`**: console I/O through the debugger using the
  [RISC-V semihosting](https://github.com/riscv-non-isa/riscv-semihosting)
  protocol (`slli`/`ebreak`/`srai` marker sequence).

  | Implemented | Behavior |
  |---|---|
  | `printf()` | Formats into a 128-byte buffer (`SEMIHOSTING_PRINTF_BUFFER`, longer output truncated) and sends it with `SYS_WRITE0` |
  | `putchar()` / `getchar()` | `SYS_WRITEC` / `SYS_READC` |
  | `time()` | Host time in seconds via `SYS_TIME` |

  Other stubs (`_open`, `_close`, `_read`, `_lseek`, `_isatty`, `_getpid`, `_kill`, `_gettimeofday`, ...) are not
  implemented: most halt the CPU in a `wfi` loop, `_write` and `_fstat` return `-1` with `ENOSYS`. Semihosting
  requires a debugger/emulator that handles `ebreak` as a semihosting call (e.g. QEMU with
  `-semihosting`); without one, `printf()` will trap.

## Device header

`include/risc-v/encoding.h` holds the standard RISC-V CSR numbers, `mstatus`/`mie`/`mip` masks and instruction
encodings, and provides the `read_csr()`/`write_csr()`/`swap_csr()` helpers (used by the start-up code to program `mtvec`). The build exports it to the
rest of STK as `_STK_DEVICE_INC`.

## Porting to another RISC-V device

1. Copy `stk.ld` (or `stk-mpu.ld`) and adjust the `MEMORY` origins, or just override sizes with `--defsym`.
2. Provide the platform's timer/interrupt handlers by overriding the weak `riscv_mtvec_*` functions.
3. Replace `semihosting.c` with a UART back-end (`_write`, `printf`, ...) for hardware without a debugger.
