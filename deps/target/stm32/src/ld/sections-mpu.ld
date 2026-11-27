/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  sections-mpu.ld
    \brief Sections of the STK STM32Fx target with the MPU isolated memory layout (STK_MPU), Cortex-M3/M4 (ARMv7-M).
    \note  Use sections.ld instead if STK_MPU is not enabled. It is the same script plus the \c .stk_mpu_xxx sections
           described below; used together with the memory map of the family: <tt>-T mem.ld -T sections-mpu.ld</tt>.
           Enable it with <tt>-DSTK_MPU=ON</tt>, which also makes startup.c initialize the MPU regions.

    \par Build-time options
    \note  Besides the options of sections.ld (see mem.ld about the order of \c --defsym), the windows of the MPU
           regions are overridden in bytes, each must be a power of two and equal to the size which is programmed into
           the MPU region by the application:
           \code
           -Xlinker --defsym=STK_MPU_SHARED_CODE_WINDOW=0x2000   // default 8 KiB
           -Xlinker --defsym=STK_MPU_KERNEL_CODE_WINDOW=0x1000   // default 4 KiB
           -Xlinker --defsym=STK_MPU_SHARED_RAM_WINDOW=0x4000    // default 16 KiB
           -Xlinker --defsym=STK_MPU_KERNEL_RAM_WINDOW=0x1000    // default 4 KiB
           \endcode
           The link fails if the content of a window does not fit.

    \par Symbols consumed by startup.c (STK_MPU)
     - \c __stk_mpu_shared_data_load, \c __stk_mpu_shared_data_start, \c __stk_mpu_shared_data_raw_end - initialized
       data image (flash) and its range (RAM); same for \c kernel.
     - \c __stk_mpu_shared_bss_start, \c __stk_mpu_shared_bss_end - range to zero; same for \c kernel.

    \par MPU description
    ============================================================================
    __stk_mpu_* REGIONS - SuperTinyKernel (STK) RTOS MPU-ISOLATED MEMORY LAYOUT
    ============================================================================

    These output sections gather SuperTinyKernel (STK) RTOS code/data that
    is shared across every MPU-sandboxed task into contiguous blocks, each
    covered by a single hardware MPU region. This is an ARMv7-M (Cortex-M3/M4,
    PMSAv7) target: unlike ARMv8-M's byte-granular MPU, the classic Cortex-M
    MPU requires each region's size to be a power of two *and* its base
    address to be naturally aligned to that same size. That is why
    .stk_mpu_shared_code below is opened with `ALIGN(8192)` (a strict 8KB
    region) and .stk_mpu_shared_data with `ALIGN(16384)` (a strict 16KB
    region) - these are hard MPU size limits, not just alignment padding:
    anything placed in either section must fit within its region's fixed
    power-of-two budget, and both the start and the padded end are re-aligned
    to that same boundary so the region covers exactly, and only, its
    intended block.

    Application code never places anything into these sections directly.
    Instead, STK (see stk_arch_arm-cortex-m.h) provides attribute macros that
    route ordinary C/C++ declarations here:

        STK_MPU_SHARED_CODE_SECTION  -> .stk_mpu_shared_code   (functions)
        STK_MPU_SHARED_DATA_SECTION  -> .stk_mpu_shared_data   (initialized globals/statics)
        STK_MPU_SHARED_BSS_SECTION   -> .stk_mpu_shared_bss    (zero-initialized globals/statics)

    The exported boundary symbols (__stk_mpu_*_start/_end, with a leading-
    underscore alias for consumers that prefer it) are picked up at runtime -
    typically via `extern char sym[];` plus `hw::PtrToWord(sym)` - to build
    the .addr/.size fields of a stk::MpuRegionConfig entry, either in a
    task's GetMpuRegions() override (per-task regions) or in the platform's
    static/global MPU table shared by every task (see
    PlatformEventHandler::OnConfigureMpu() in example.cpp, REGION 4/5).

    Region breakdown:

    1. .stk_mpu_shared_code (PART 1, in FLASH, executed in place, 8KB region)
       Exported symbols: __stk_mpu_shared_code_start / _end.
       Populated by STK_MPU_SHARED_CODE_SECTION and executed directly from
       flash (XIP). Mapped by STK to ACCESS_PRIV_RO_USER_RO + EXEC_ALLOWED:
       read-only, executable, reachable by both privileged and unprivileged
       (sandboxed) tasks alike - shared "trusted entry point" code common
       to every task, described once instead of being duplicated per task.

    2. .stk_mpu_kernel_code (PART 2, in FLASH, executed in place, 4KB region)
       Exported symbols: __stk_mpu_kernel_code_start / _end.
       Populated by STK_MPU_KERNEL_CODE_SECTION. Mapped by STK strictly to
       PRIVILEGED READ-ONLY (ACCESS_PRIV_RO_USER_NO + EXEC_ALLOWED). Isolated
       from user space and shared application memory.

    3. .stk_mpu_shared_data (PART 3, RAM AT> FLASH, 16KB region)
       Exported symbols: __stk_mpu_shared_data_start / _end. Populated by
       STK_MPU_SHARED_DATA_SECTION: initialized read/write globals/statics
       shared by every task (e.g. event flags, queues). Lives in RAM at
       runtime, but its initial contents are stored in FLASH and copied to
       RAM during startup (startup.c copies it from
       __stk_mpu_shared_data_load, like .data).

       Nested inside this same 16KB block - rather than a standalone
       output section - is the shared bss, populated by
       STK_MPU_SHARED_BSS_SECTION for zero-initialized shared
       globals/statics (__stk_mpu_shared_bss_start / _end). It is placed
       immediately after the initialized data and is zero-filled by startup
       instead of being copied from flash. Nesting it inside
       .stk_mpu_shared_data (instead of giving it its own 8/16KB-
       aligned region, as the RP2040/Cortex-M33 variant of this script
       does) avoids spending a second scarce power-of-two MPU region on
       what is, from the MPU's point of view, the same RW+XN block.

    4. .stk_mpu_kernel_data (PART 4, RAM AT> FLASH, 4KB region)
       Exported symbols: __stk_mpu_kernel_data_start / _end.
       Populated by STK_MPU_KERNEL_DATA_SECTION (initialized globals) and
       STK_MPU_KERNEL_BSS_SECTION (zero-initialized globals). Mapped strictly
       to PRIVILEGED READ-WRITE (ACCESS_PRIV_RW_USER_NO + EXEC_NEVER).

    Together, the initialized and zero-initialized halves of
    .stk_mpu_shared_data form one contiguous RW+XN (ACCESS_FULL, EXEC_NEVER)
    MPU region: full read/write access for privileged and unprivileged
    tasks alike, with code execution from it explicitly forbidden to
    prevent code injection.

    In short: these markers let STK's static MPU table and each task's
    GetMpuRegions() precisely locate and size the shared "toolkit" code and
    data blocks (in both FLASH and RAM) so they can be enclosed in their
    own dedicated, power-of-two-sized MPU regions, isolated from
    task-private memory and from each other by access permission
    (RX vs RW+XN).
    ============================================================================

    \par Memory layout
     - \c FLASH  - vector table, code, read-only data, load images of the initialized data, MPU code windows.
     - \c RAM    - .data, .bss, MPU data windows, .noinit, then the heap which grows up; the main stack at the top.
     - \c CCMRAM - as in sections.ld (STM32F4).
    \see sections.ld, mem.ld, startup.c
*/

ENTRY(Reset_Handler)

/* Require the linker to resolve __isr_vectors at link time so vectors_stm32xxx.o
 * is pulled in from the static library when the vector table lives in an archive. */
EXTERN(__isr_vectors)

/* build-time option (internal symbol, see mem.ld) */
__stk_stack_size = DEFINED(__stack_size) ? __stack_size : 0x400;

/* MPU windows: power of two sizes, see the description above */
__stk_mpu_shared_code_window = DEFINED(STK_MPU_SHARED_CODE_WINDOW) ? STK_MPU_SHARED_CODE_WINDOW : 0x2000;
__stk_mpu_kernel_code_window = DEFINED(STK_MPU_KERNEL_CODE_WINDOW) ? STK_MPU_KERNEL_CODE_WINDOW : 0x1000;
__stk_mpu_shared_ram_window  = DEFINED(STK_MPU_SHARED_RAM_WINDOW)  ? STK_MPU_SHARED_RAM_WINDOW  : 0x4000;
__stk_mpu_kernel_ram_window  = DEFINED(STK_MPU_KERNEL_RAM_WINDOW)  ? STK_MPU_KERNEL_RAM_WINDOW  : 0x1000;

/* the main stack takes the top of RAM */
_estack = ORIGIN(RAM) + LENGTH(RAM);
__stack = _estack;
__stack_limit = _estack - __stk_stack_size;

SECTIONS
{
    /*! \brief Vector table: must be the first thing in flash. */
    .isr_vector : ALIGN(4)
    {
        __vectors_start = .;
        KEEP(*(.isr_vector))
    } >FLASH

    /*! \brief Code. */
    .text : ALIGN(4)
    {
        *(.text .text.*)
        *(.glue_7)          /* ARM -> Thumb veneers, always generated by the linker */
        *(.glue_7t)         /* Thumb -> ARM veneers */
        *(vtable)           /* C++ virtual tables */
        KEEP(*(.eh_frame*))
    } >FLASH

    /*! \brief Read-only data. */
    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.* .constdata .constdata.*)
        . = ALIGN(4);
    } >FLASH

    /*! \brief Arrays of static constructors / destructors, see startup.c. */
    .preinit_array : ALIGN(4)
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array .preinit_array.*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } >FLASH

    .init_array : ALIGN(4)
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } >FLASH

    .fini_array : ALIGN(4)
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } >FLASH

    /*! \brief C++ exception unwinding tables. */
    .ARM.extab : ALIGN(4)
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } >FLASH

    .ARM.exidx : ALIGN(4)
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } >FLASH

    . = ALIGN(4);
    _etext = .;
    __etext = .;

    /*! \brief MPU, PART 1: shared code (RX for privileged and user), one window aligned to its size. */
    .stk_mpu_shared_code : ALIGN(__stk_mpu_shared_code_window)
    {
        __stk_mpu_shared_code_start = .;
        _stk_mpu_shared_code_start = .;

        *(.stk_mpu_shared_code .stk_mpu_shared_code.*)

        . = ALIGN(4);
        __stk_mpu_shared_code_actual_end = .;

        /* pad so that the section occupies the full MPU region */
        . = ALIGN(__stk_mpu_shared_code_window);
        __stk_mpu_shared_code_end = .;
        _stk_mpu_shared_code_end = .;
    } >FLASH

    /*! \brief MPU, PART 2: kernel code (privileged RX only). */
    .stk_mpu_kernel_code : ALIGN(__stk_mpu_kernel_code_window)
    {
        __stk_mpu_kernel_code_start = .;

        *(.stk_mpu_kernel_code .stk_mpu_kernel_code.*)

        . = ALIGN(4);
        __stk_mpu_kernel_code_actual_end = .;

        . = ALIGN(__stk_mpu_kernel_code_window);
        __stk_mpu_kernel_code_end = .;
    } >FLASH

    /*! \brief Initialized data of CCM RAM, the image is in flash. */
    _sidata_ccm = LOADADDR(.data_ccm);

    .data_ccm : ALIGN(4)
    {
        _sdata_ccm = .;
        *(.data.CCMRAM .data.CCMRAM.*)
        . = ALIGN(4);
        _edata_ccm = .;
    } >CCMRAM AT>FLASH

    /*! \brief Initialized data, the image is in flash. */
    _sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT>FLASH

    /*! \brief Zero-initialized data. NOLOAD: the sections hold no image. */
    .bss_ccm (NOLOAD) : ALIGN(4)
    {
        _sbss_ccm = .;
        *(.bss.CCMRAM .bss.CCMRAM.*)
        . = ALIGN(4);
        _ebss_ccm = .;
    } >CCMRAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM

    /*! \brief MPU, PART 3: shared data and bss (RW + XN for privileged and user), one window aligned to its size. */
    .stk_mpu_shared_data : ALIGN(__stk_mpu_shared_ram_window)
    {
        __stk_mpu_shared_data_start = .;
        _stk_mpu_shared_data_start = .;

        /* initialized variables, the image is in flash */
        *(.stk_mpu_shared_data .stk_mpu_shared_data.*)
        . = ALIGN(4);
        __stk_mpu_shared_data_raw_end = .;

        /* zero-initialized variables */
        __stk_mpu_shared_bss_start = .;
        *(.stk_mpu_shared_bss .stk_mpu_shared_bss.*)
        . = ALIGN(4);
        __stk_mpu_shared_bss_end = .;

        . = ALIGN(__stk_mpu_shared_ram_window);
        __stk_mpu_shared_data_end = .;
        _stk_mpu_shared_data_end = .;
    } >RAM AT>FLASH

    __stk_mpu_shared_data_load = LOADADDR(.stk_mpu_shared_data);

    /*! \brief MPU, PART 4: kernel data and bss (privileged RW + XN only). */
    .stk_mpu_kernel_data : ALIGN(__stk_mpu_kernel_ram_window)
    {
        __stk_mpu_kernel_data_start = .;

        /* initialized kernel variables, the image is in flash */
        *(.stk_mpu_kernel_data .stk_mpu_kernel_data.*)
        . = ALIGN(4);
        __stk_mpu_kernel_data_raw_end = .;

        /* zero-initialized kernel variables */
        __stk_mpu_kernel_bss_start = .;
        *(.stk_mpu_kernel_bss .stk_mpu_kernel_bss.*)
        . = ALIGN(4);
        __stk_mpu_kernel_bss_end = .;

        . = ALIGN(__stk_mpu_kernel_ram_window);
        __stk_mpu_kernel_data_end = .;
    } >RAM AT>FLASH

    __stk_mpu_kernel_data_load = LOADADDR(.stk_mpu_kernel_data);

    /*! \brief Data which survives the start-up: neither initialized nor zeroed. */
    .noinit_ccm (NOLOAD) : ALIGN(4)
    {
        *(.noinit.CCMRAM .noinit.CCMRAM.*)
    } >CCMRAM

    .noinit (NOLOAD) : ALIGN(8)
    {
        *(.noinit .noinit.*)
        . = ALIGN(8);
    } >RAM

    /*! \brief Heap: everything between the static data and the main stack, the symbols are used by _sbrk(). */
    __heap_start = ALIGN(8);
    PROVIDE(end = __heap_start);
    PROVIDE(_end = __heap_start);
    PROVIDE(__end__ = __heap_start);
    __heap_end = __stack_limit;

    /*! \brief Link-time check only, no memory is allocated here. */
    ASSERT(__stk_stack_size % 8 == 0, "STK: __stack_size must be a multiple of 8")
    ASSERT(__stk_mpu_shared_code_actual_end - __stk_mpu_shared_code_start <= __stk_mpu_shared_code_window, "STK: .stk_mpu_shared_code does not fit into its MPU window")
    ASSERT(__stk_mpu_kernel_code_actual_end - __stk_mpu_kernel_code_start <= __stk_mpu_kernel_code_window, "STK: .stk_mpu_kernel_code does not fit into its MPU window")
    ASSERT(__stk_mpu_shared_bss_end - __stk_mpu_shared_data_start <= __stk_mpu_shared_ram_window, "STK: .stk_mpu_shared_data does not fit into its MPU window")
    ASSERT(__stk_mpu_kernel_bss_end - __stk_mpu_kernel_data_start <= __stk_mpu_kernel_ram_window, "STK: .stk_mpu_kernel_data does not fit into its MPU window")
    ASSERT(__heap_start <= __heap_end, "STK: .data/.bss and the main stack do not fit into RAM")

    /DISCARD/ :
    {
        *(.note.GNU-stack)
    }
}
