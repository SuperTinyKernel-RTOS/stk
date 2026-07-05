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
    /* RAM_NS sections                                                      */
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
        *(.text*)
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

    .heap (NOLOAD): {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM_NS

    __HeapLimit = ORIGIN(RAM_NS) + LENGTH(RAM_NS);

    /* ------------------------------------------------------------------ */
    /* Scratch / stack sections                                             */
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
