/* Based on GCC ARM embedded samples.
   Defines the following symbols for use by code:
    __exidx_start
    __exidx_end
    __etext
    __data_start__
    __preinit_array_start
    __preinit_array_end
    __init_array_start
    __init_array_end
    __fini_array_start
    __fini_array_end
    __data_end__
    __bss_start__
    __bss_end__
    __end__
    end
    __HeapLimit
    __StackLimit
    __StackTop
    __stack (== StackTop)
*/

/* ============================================================================
   __stk_mpu_* REGIONS -- SuperTinyKernel (STK) RTOS MPU-ISOLATED MEMORY LAYOUT
   ============================================================================

   This script carves out several extra output sections, all prefixed
   "stk_mpu_", so that SuperTinyKernel (STK) RTOS code/data shared across
   every MPU-sandboxed task can be gathered into contiguous, predictably
   aligned blocks and then covered by a *single* hardware MPU region each.
   ARMv8-M (Cortex-M33) requires MPU region base addresses to be aligned;
   this script uses 32-byte alignment for that reason, so every stk_mpu_*
   section is opened with `ALIGN(32)` and its end is re-aligned to 32 bytes
   as well, guaranteeing no unrelated symbol is accidentally pulled into
   (or excluded from) the region.

   Application code never places anything into these sections directly.
   Instead, STK (see stk_arch_arm-cortex-m.h) provides three attribute
   macros that route ordinary C/C++ declarations into them:

       STK_MPU_SHARED_CODE_SECTION  -> .stk_mpu_shared_code   (functions)
       STK_MPU_SHARED_DATA_SECTION  -> .stk_mpu_shared_data   (initialized globals/statics)
       STK_MPU_SHARED_BSS_SECTION   -> .stk_mpu_shared_bss    (zero-initialized globals/statics)

   The linker-exported boundary symbols below (__stk_mpu_*_start/_end) are
   then picked up at runtime -- typically via `extern char sym[];` plus
   `hw::PtrToWord(sym)` -- to compute the .addr/.size fields of a
   stk::MpuRegionConfig entry, either in a task's GetMpuRegions() override
   (per-task regions) or in the platform's static/global MPU table (regions
   shared by every task, e.g. PlatformEventHandler::OnConfigureMpu() in
   example.cpp). This is why the sections must be contiguous and
   size-computable purely from their start/end symbols.

   Region breakdown:

   1. .stk_mpu_shared_code (PART 1, in FLASH, executed in place)
      Exported symbols: __stk_mpu_shared_code_start / _end
      Populated by STK_MPU_SHARED_CODE_SECTION. Executes directly from
      flash (XIP). At runtime this maps to an MPU region with
      ACCESS_PRIV_RO_USER_RO + EXEC_ALLOWED: read-only, executable, and
      reachable by both privileged and unprivileged (sandboxed) tasks --
      i.e. common "trusted entry point" code shared by every task, so it
      only needs to be described once instead of being duplicated in each
      task's own per-task region list.

   2. __stk_mpu_ram_text_start / _end (PART 2, inside .data, in RAM)
      A sub-region carved out of the normal .data output section. It
      captures the "leftover" *(.text*) input sections that were
      deliberately excluded from the main FLASH .text section earlier in
      this script (see the EXCLUDE_FILE filters around libgcc/libc/libm)
      and therefore get copied into RAM at startup instead of executing
      from flash. The 32-byte-aligned start/end markers let this
      relocated, executable RAM code be described as its own MPU region,
      distinct from the surrounding (non-executable) RAM data around it.

   3. .stk_mpu_shared_data (PART 3.1, RAM AT> FLASH)
      Exported symbols: __stk_mpu_shared_data_start / _end, plus
      __stk_mpu_shared_data_source__ (its LOADADDR in FLASH).
      Populated by STK_MPU_SHARED_DATA_SECTION: initialized read/write
      globals/statics shared by every task (e.g. event flags, queues).
      Lives in RAM at runtime, but its initial contents are stored in
      FLASH and copied to RAM during startup (like .data), hence the
      exported source/load address.

   4. .stk_mpu_shared_bss (PART 3.2, RAM only, NOLOAD)
      Exported symbols: __stk_mpu_shared_bss_start / _end.
      Populated by STK_MPU_SHARED_BSS_SECTION: zero-initialized
      globals/statics that share the same MPU region/permissions as
      .stk_mpu_shared_data, but -- having no "AT> FLASH" load region --
      consume zero space in the flash image. Startup code zero-fills this
      range (like .bss) instead of copying it, so it should be preferred
      over .stk_mpu_shared_data for large or scratch shared buffers.

   .stk_mpu_shared_data and .stk_mpu_shared_bss are placed back-to-back
   (data first, then bss) and 32-byte aligned only at the very end of the
   combined block, so together they form one contiguous RW+XN
   (ACCESS_FULL, EXEC_NEVER) MPU region in RAM: full read/write access for
   privileged and unprivileged tasks alike, with code execution from it
   explicitly forbidden to prevent code injection.

   In short: these markers let STK's static MPU table and each task's
   GetMpuRegions() precisely locate and size the shared "toolkit" code and
   data blocks (in both FLASH and RAM) so they can be enclosed in their
   own dedicated MPU regions, isolated from task-private memory and from
   each other by access permission (RX vs RW+XN).
   ============================================================================ */

MEMORY
{
    INCLUDE "pico_flash_region.ld"
    RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k
    SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    /* The bootrom will enter the image at the point indicated in your
       IMAGE_DEF, which is usually the reset handler of your vector table.

       The debugger will use the ELF entry point, which is the _entry_point
       symbol, and in our case is *different from the bootrom's entry point.*
       This is used to go back through the bootrom on debugger launches only,
       to perform the same initial flash setup that would be performed on a
       cold boot.
    */

    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        /* TODO revisit this now memset/memcpy/float in ROM */
        /* bit of a hack right now to exclude all floating point and time critical (e.g. memset, memcpy) code from
         * FLASH ... we will include any thing excluded here in .data below by default */
        *(.init)
        *libgcc.a:cmse_nonsecure_call.o
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        /* Pull all c'tors into .text */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        /* Followed by destructors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        /* finit data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    /* Note the boot2 section is optional, and should be discarded if there is
       no reference to it *inside* the binary, as it is not called by the
       bootrom. (The bootrom performs a simple best-effort XIP setup and
       leaves it to the binary to do anything more sophisticated.) However
       there is still a size limit of 256 bytes, to ensure the boot2 can be
       stored in boot RAM.

       Really this is a "XIP setup function" -- the name boot2 is historic and
       refers to its dual-purpose on RP2040, where it also handled vectoring
       from the bootrom into the user image.
    */

    .boot2 : {
        __boot2_start__ = .;
        *(.boot2)
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ <= 256,
        "ERROR: Pico second stage bootloader must be no more than 256 bytes in size")

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;
    
    /* ======================================================================== */
    /* PART 1: INTEGRATED SHARED MPU CODE SECTION (RX Only)                     */
    /* Aligned to 32-bytes (Cortex-M33 requirement).                            */
    /* ======================================================================== */
    .stk_mpu_shared_code : ALIGN(32)
    {
        __stk_mpu_shared_code_start = .;
        *(.stk_mpu_shared_code .stk_mpu_shared_code.*)
        . = ALIGN(32);
        
        __stk_mpu_shared_code_end = .;
    } > FLASH
    /* ======================================================================== */

    /* Machine inspectable binary information */
    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        /* remaining .text and .rodata; i.e. stuff we exclude above because we want it in RAM */
                
        /* ========================================================= */
        /* PART 2: ISOLATED RAM CODE BLOCK FOR MPU                   */
        /* ========================================================= */
        . = ALIGN(32);                  /* MPU alignment requirement */
        __stk_mpu_ram_text_start = .;   /* Marker 1: START           */

        *(.text*)                        /* Excluded text/libgcc code */

        . = ALIGN(32);                  /* MPU alignment requirement */
        __stk_mpu_ram_text_end = .;     /* Marker 2: END             */
        /* ========================================================= */
        
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);

        *(.data*)
        *(.sdata*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        *(.jcr)
        . = ALIGN(4);
    } > RAM AT> FLASH

    .tdata : {
        . = ALIGN(4);
		*(.tdata .tdata.* .gnu.linkonce.td.*)
        /* All data end */
        __tdata_end = .;
    } > RAM AT> FLASH
    PROVIDE(__data_end__ = .);

    /* __etext is (for backwards compatibility) the name of the .data init source pointer (...) */
    __etext = LOADADDR(.data);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)

        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;

        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM
    
    /* ======================================================================== */
    /* PART 3: INTEGRATED SHARED MPU DATA SECTION (RW + XN)                     */
    /* This section is defined in RAM but loaded from FLASH.                    */
    /* Aligned to 32-bytes (Cortex-M33 requirement).                            */
    /* ======================================================================== */
    /* 1. Initialized Data Section - Consumes space in FLASH */
    .stk_mpu_shared_data : ALIGN(32)
    {
        __stk_mpu_shared_data_start = .;
        *(.stk_mpu_shared_data .stk_mpu_shared_data.*)
        . = ALIGN(4); 
        __stk_mpu_shared_data_end = .;
    } > RAM AT> FLASH
    
    __stk_mpu_shared_data_source__ = LOADADDR(.stk_mpu_shared_data);

    /* 2. Zero-Initialized BSS Section - Allocates 0 bytes in FLASH */
    /* By omitting "AT> FLASH", this section consumes ZERO flash footprint */
    .stk_mpu_shared_bss : ALIGN(4)
    {
        __stk_mpu_shared_bss_start = .;
        *(.stk_mpu_shared_bss .stk_mpu_shared_bss.*)
        
        /* Align the very end of the total combined block to 32 bytes */
        . = ALIGN(32); 
        __stk_mpu_shared_bss_end = .;
    } > RAM
    /* ======================================================================== */

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM
    /* historically on GCC sbrk was growing past __HeapLimit to __StackLimit, however
       to be more compatible, we now set __HeapLimit explicitly to where the end of the heap is */
    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM);

    /* Start and end symbols must be word-aligned */
    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    /* .stack*_dummy section doesn't contains any symbols. It is only
     * used for linker to calculate size of stack sections, and assign
     * values to stack symbols later
     *
     * stack1 section may be empty/missing if platform_launch_core1 is not used */

    /* by default we put core 0 stack at the end of scratch Y, so that if core 1
     * stack is not used then all of SCRATCH_X is free.
     */
    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > SCRATCH_Y

    .flash_end : {
        KEEP(*(.embedded_end_block*))
        PROVIDE(__flash_binary_end = .);
    } > FLASH =0xaa

    /* stack limit is poorly named, but historically is maximum heap ptr */
    __StackLimit = ORIGIN(RAM) + LENGTH(RAM);
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    PROVIDE(__stack = __StackTop);

    /* picolibc and LLVM */
    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    /* llvm-libc */
    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    /* Check if data + heap + stack exceeds RAM limit */
    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")

    /* todo assert on extra code */
}

