/* Non-Secure world linker script for RP2350 TrustZone.
   Based on memmap_tz_sc.ld (Secure counterpart).

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
   SuperTinyKernel (STK) RTOS
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

   TrustZone note: this is the Non-Secure-world linker script, so its
   stk_mpu_* sections (the __stk_mpu_ram_text_start/_end range inside .data,
   plus .stk_mpu_shared_data / .stk_mpu_shared_bss) live entirely in
   FLASH_NS/RAM_NS and are only consumed by the MPU table of Non-Secure
   tasks -- a separate mechanism from TrustZone itself, which sandboxes
   Non-Secure tasks from each other, not from the Secure world (that
   boundary is enforced by the SAU and configured on the Secure side, see
   memmap_tz_sc.ld). Note that PART 1, .stk_mpu_shared_code, is absent from
   this script: in this project's split, the "shared trusted entry point"
   code (STK_MPU_SHARED_CODE_SECTION) is only ever placed in the Secure
   image; Non-Secure tasks that need shared executable code reach it
   through the NSC gateway veneers (.gnu.sgstubs) exported by the Secure
   script instead. This script's .stk_mpu_shared_data/.stk_mpu_shared_bss
   are a completely separate, Non-Secure-only RW+XN block, unrelated to the
   Secure image's identically-named sections in RAM_S.
   ============================================================================ */

MEMORY
{
    /* Non-Secure Flash region (after Secure + NSC regions) */
    INCLUDE "pico_flash_region_tz_ns.ld"

    /* Non-Secure RAM: 248k (shrunk by 8k to leave room for NS scratch above).
       The Secure world reserves 0x20000000..0x2003FFFF for itself.
       NS scratch regions sit at 0x2007E000..0x20080000 (top of original 256k). */
    RAM_NS(rwx)    : ORIGIN = 0x20040000, LENGTH = 248k

    /* Scratch RAMs for core stacks – placed above NS RAM */
    SCRATCH_X(rwx) : ORIGIN = 0x2007E000, LENGTH = 4k /* (NS core 1 stack) */
    SCRATCH_Y(rwx) : ORIGIN = 0x2007F000, LENGTH = 4k /* (NS core 0 stack, top = 0x20080000) */
}

/* Non-Secure entry point (reset handler in NS vector table) */
ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        . = ALIGN(32);
        __flash_binary_start = .;
    } > FLASH_NS

    /* The bootrom / Secure Manager will branch to this image via the NS
       vector table reset handler.  The debugger uses the ELF entry point
       (_entry_point) to restart through the Secure Manager. */

    .text : {
        __logical_binary_start = .;
        . = ALIGN(128); /* TrustZone vector table alignment */
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        *(.init)
        /* Exclude time-critical / float routines from flash; they land in
           .data (RAM_NS) below */
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        /* Constructors */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        /* Destructors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH_NS

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH_NS

    .ARM.extab : {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH_NS

    __exidx_start = .;
    .ARM.exidx : {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH_NS
    __exidx_end = .;
    
    /* ======================================================================== */
    /* SuperTinyKernel (STK) RTOS                                               */
    /* -------------------------------------------------------------------------*/
    /* PART 1: INTEGRATED SHARED MPU CODE SECTION (RX Only)                     */
    /* Aligned to 32-bytes (Cortex-M33 requirement).                            */
    /* ======================================================================== */
    .stk_mpu_shared_code : ALIGN(32)
    {
        __stk_mpu_shared_code_start = .;
        *(.stk_mpu_shared_code .stk_mpu_shared_code.*)
        . = ALIGN(32);
        
        __stk_mpu_shared_code_end = .;
    } > FLASH_NS
    /* ======================================================================== */

    /* Machine-inspectable binary information */
    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info : {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH_NS
    __binary_info_end = .;
    . = ALIGN(4);

    /* ------------------------------------------------------------------ */
    /* RAM_NS sections                                                    */
    /* ------------------------------------------------------------------ */

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM_NS

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM_NS

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        /* Remaining .text / .rodata that must run from RAM_NS */

        /* ========================================================= */
        /* SuperTinyKernel (STK) RTOS                                */
        /* ----------------------------------------------------------*/
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

        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        *(.jcr)
        . = ALIGN(4);
    } > RAM_NS AT> FLASH_NS

    .tdata : {
        . = ALIGN(4);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        __tdata_end = .;
    } > RAM_NS AT> FLASH_NS
    PROVIDE(__data_end__ = .);

    /* __etext: backwards-compat name for the .data initialisation source */
    __etext = LOADADDR(.data);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        __tls_end = .;
    } > RAM_NS

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM_NS
    
    /* ======================================================================== */
    /* SuperTinyKernel (STK) RTOS                                               */
    /* -------------------------------------------------------------------------*/
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
    } > RAM_NS AT> FLASH_NS
    
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
    } > RAM_NS
    /* ======================================================================== */

    .heap (NOLOAD): {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM_NS

    __HeapLimit = ORIGIN(RAM_NS) + LENGTH(RAM_NS);

    /* ------------------------------------------------------------------ */
    /* Scratch / stack sections                                           */
    /* ------------------------------------------------------------------ */

    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH_NS
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH_NS
    __scratch_y_source__ = LOADADDR(.scratch_y);

    /* Core 1 stack (optional; empty if platform_launch_core1 not used) */
    .stack1_dummy (NOLOAD): {
        *(.stack1*)
    } > SCRATCH_X

    /* Core 0 stack – at the end of SCRATCH_Y so all of SCRATCH_X is
       available to core 1 when present */
    .stack_dummy (NOLOAD): {
        KEEP(*(.stack*))
    } > SCRATCH_Y

    .flash_end : {
        KEEP(*(.embedded_end_block*))
        . = ALIGN(32);
        PROVIDE(__flash_binary_end = .);
    } > FLASH_NS =0xaa

    /* ------------------------------------------------------------------ */
    /* Absolute symbols                                                     */
    /* ------------------------------------------------------------------ */

    __StackLimit    = ORIGIN(RAM_NS) + LENGTH(RAM_NS);
    __StackOneTop   = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop      = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom   = __StackTop    - SIZEOF(.stack_dummy);
    PROVIDE(__stack = __StackTop);

    /* ============================================================================
       SuperTinyKernel (STK) RTOS
       TRUSTZONE SECTIONS -- NON-SECURE COUNTERPART
       ============================================================================
       Unlike memmap_tz_sc.ld, this Non-Secure script does not itself program
       or export SAU boundary symbols (__nsc_start, __ns_ram_start, etc.) --
       the SAU is Secure-only hardware, configured once by Configure_SAU() in
       the Secure image before this image is ever entered. This script only
       needs to (a) place this image's own vector table where
       InvokeNonSecureState() expects to find it (word 0 = initial MSP_NS,
       word 1 = the NS reset handler address, read out of
       __ns_flash_start == ORIGIN(FLASH_NS) on the Secure side) and (b) keep
       its own SCRATCH_X/SCRATCH_Y core-stack boundaries in sync with what the
       Secure image already carved out of the address map for it.

       __ns_scratch_start / __ns_scratch_end below are exactly that: computed
       here from this script's own SCRATCH_X/SCRATCH_Y MEMORY regions, they
       must resolve to the same addresses as the hardcoded
       __ns_scratch_start/_end in memmap_tz_sc.ld (both currently
       0x2007E000..0x20080000). Any Non-Secure-callable (NSC) entry points
       this image calls into Secure code through -- e.g. NSC_OnExitNs(),
       NSC_GetKey(), NSC_bsp_Led_SwitchOnExclusive() in example.cpp -- are
       reached via auto-generated veneers that live in the Secure image's
       FLASH_NSC region (.gnu.sgstubs), not in anything defined by this
       script; from this side they are simply ordinary function calls to
       addresses inside FLASH_NSC; the hardware transitions to Secure state
       transparently.
       ============================================================================ */

    /* Non-Secure Scratch RAM boundaries — mirrors __ns_scratch_start/end
       exported from the Secure image's memmap_tz_sc.ld */
    __ns_scratch_start = ORIGIN(SCRATCH_X);
    __ns_scratch_end   = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);

    /* picolibc / LLVM */
    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end   = __HeapLimit);
    PROVIDE( __tls_align  = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    /* llvm-libc */
    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    /* ------------------------------------------------------------------ */
    /* Sanity checks                                                        */
    /* ------------------------------------------------------------------ */

    ASSERT(__StackBottom >= __HeapLimit,
        "region RAM_NS overflowed: heap and stack collision")

    ASSERT(__binary_info_header_end - __logical_binary_start <= 1024,
        "Binary info must be in first 1024 bytes of the binary")

    ASSERT(__embedded_block_end - __logical_binary_start <= 4096,
        "Embedded block must be in first 4096 bytes of the binary")
}
