/* Copyright (c) 2020 SiFive Inc. */
/* SPDX-License-Identifier: Apache-2.0 */

OUTPUT_ARCH("riscv")

/* Default Linker Script
 *
 * This is the default linker script for all Freedom Metal applications.
 */

ENTRY(_enter)

RAM_SIZE = DEFINED(__METAL_RAM_SIZE__) ? __METAL_RAM_SIZE__ : 0x40000;
ROM_SIZE = DEFINED(__METAL_ROM_SIZE__) ? __METAL_ROM_SIZE__ : 0x200000;

MEMORY
{
    itim (airwx) : ORIGIN = 0x8000000, LENGTH = 0x2000
    ram (arw!xi) : ORIGIN = 0x80000000, LENGTH = RAM_SIZE
    rom (irx!wa) : ORIGIN = 0x20010000, LENGTH = ROM_SIZE
}

PHDRS
{
    rom PT_LOAD;
    ram_init PT_LOAD;
    tls PT_TLS;
    ram PT_LOAD;
    itim_init PT_LOAD;
    text PT_LOAD;
    lim_init PT_LOAD;
}

SECTIONS
{
    /* Each hart is allocated its own stack of size __stack_size. This value
     * can be overriden at build-time by adding the following to CFLAGS:
     *
     *     -Xlinker --defsym=__stack_size=0xf00
     *
     * where 0xf00 can be replaced with a multiple of 16 of your choice.
     *
     * __stack_size is PROVIDE-ed as a symbol so that initialization code
     * initializes the stack pointers for each hart at the right offset from
     * the _sp symbol.
     */
    __stack_size = DEFINED(__stack_size) ? __stack_size : 0x8000;
    PROVIDE(__stack_size = __stack_size);

    /* The size of the heap can be overriden at build-time by adding the
     * following to CFLAGS:
     *
     *     -Xlinker --defsym=__heap_size=0xf00
     *
     * where 0xf00 can be replaced with the value of your choice.
     *
     * Altertatively, the heap can be grown to fill the entire remaining region
     * of RAM by adding the following to CFLAGS:
     *
     *     -Xlinker --defsym=__heap_max=1
     *
     * Note that depending on the memory layout, the bitness (32/64bit) of the
     * target, and the code model in use, this might cause a relocation error.
     */
    __heap_size = DEFINED(__heap_size) ? __heap_size : 0x800;

    /* The boot hart sets which hart runs the pre-main initialization routines,
     * including copying .data into RAM, zeroing the BSS region, running
     * constructors, etc. After initialization, the boot hart is also the only
     * hart which runs application code unless the application overrides the
     * secondary_main() function to start execution on secondary harts.
     */
    PROVIDE(__metal_boot_hart = 0);

    /* The chicken bit is used by pre-main initialization to enable/disable
     * certain core features */
    PROVIDE(__metal_chicken_bit = 1);

    /* The memory_ecc_scrub bit is used by _entry code to enable/disable
     * memories scrubbing to zero  */
    PROVIDE(__metal_eccscrub_bit = 0);

    /* The RAM memories map for ECC scrubbing */
    PROVIDE( metal_dtim_0_memory_start = 0x80000000 );
    PROVIDE( metal_dtim_0_memory_end = 0x80000000 + 0x4000 );
    PROVIDE( metal_itim_0_memory_start = 0x8000000 );
    PROVIDE( metal_itim_0_memory_end = 0x8000000 + 0x2000 );

    /* ROM SECTION
     *
     * The following sections contain data which lives in read-only memory, if
     * such memory is present in the design, for the entire duration of program
     * execution.
     */

    .init : {
        /* The _enter symbol is placed in the .text.metal.init.enter section
         * and must be placed at the beginning of the program */
        KEEP (*(.text.metal.init.enter))
        KEEP (*(.text.metal.init.*))
        KEEP (*(SORT_NONE(.init)))
        KEEP (*(.text.libgloss.start))
    } >rom :rom

    .fini : {
        KEEP (*(SORT_NONE(.fini)))
    } >rom :rom

    .preinit_array : ALIGN(8) {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } >rom :rom

    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
        PROVIDE_HIDDEN ( metal_constructors_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.metal.init_array.*)));
        KEEP (*(.metal.init_array));
        PROVIDE_HIDDEN ( metal_constructors_end = .);
    } >rom :rom

    .fini_array : ALIGN(8) {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
        PROVIDE_HIDDEN ( metal_destructors_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.metal.fini_array.*)));
        KEEP (*(.metal.fini_array));
        PROVIDE_HIDDEN ( metal_destructors_end = .);
    } >rom :rom

    .ctors : {
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
        KEEP (*(.metal.ctors .metal.ctors.*))
    } >rom :rom

    .dtors : {
        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
        KEEP (*(.metal.dtors .metal.dtors.*))
    } >rom : rom

    .rodata : {
        *(.rdata)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(8);
        *(.srodata.cst16)
        *(.srodata.cst8)
        *(.srodata.cst4)
        *(.srodata.cst2)
        *(.srodata .srodata.*)
    } >rom :rom
        
    /* ITIM SECTION
     *
     * The following sections contain data which is copied from read-only
     * memory into an instruction tightly-integrated memory (ITIM), if one
     * is present in the design, during pre-main program initialization.
     *
     * Generally, the data copied into the ITIM should be performance-critical
     * functions which benefit from low instruction-fetch latency.
     */

    .itim : ALIGN(8) {
        *(.itim .itim.*)
    } >itim AT>rom :itim_init

    PROVIDE( metal_segment_itim_source_start = LOADADDR(.itim) );
    PROVIDE( metal_segment_itim_target_start = ADDR(.itim) );
    PROVIDE( metal_segment_itim_target_end = ADDR(.itim) + SIZEOF(.itim) );

    /* LIM SECTION
     *
     * The following sections contain data which is copied from read-only
     * memory into a loosely integrated memory (LIM), which is shared with L2
     * cache, during pre-main program initialization.
     *
     * Generally, the data copied into the LIM should be performance-critical
     * functions which benefit from low instruction-fetch latency.
     */

    .lim : ALIGN(8) {
        *(.lim .lim.*)
    } >ram AT>rom :lim_init

    PROVIDE( metal_segment_lim_source_start = LOADADDR(.lim) );
    PROVIDE( metal_segment_lim_target_start = ADDR(.lim) );
    PROVIDE( metal_segment_lim_target_end = ADDR(.lim) + SIZEOF(.lim) );

    /* TEXT SECTION
     *
     * The following section contains the code of the program, excluding
     * everything that's been allocated into the ITIM/LIM already
     */

    .text : {
        *(.text.unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text .text.*)
        *(.gnu.linkonce.t.*)   
    } >rom :text

    /* RAM SECTION
     *
     * The following sections contain data which is copied from read-only
     * memory into a read-write-capable memory such as data tightly-integrated
     * memory (DTIM) or another main memory, as well as the BSS, stack, and
     * heap.
     *
     * You might notice that .data, .tdata, .tbss, .tbss_space, and .bss all
     * have an apparently unnecessary ALIGN at their top. This is because
     * the implementation of _start in Freedom Metal libgloss depends on the
     * ADDR and LOADADDR being 8-byte aligned.
     */

    .data : ALIGN(8) {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE( __global_pointer$ = . + 0x800 );
        *(.sdata .sdata.* .sdata2.*)
        *(.gnu.linkonce.s.*)
    } >ram AT>rom :ram_init
    
    .tdata : ALIGN(8) {
        PROVIDE( __tls_base = . );
    *(.tdata .tdata.* .gnu.linkonce.td.*)
    } >ram AT>rom :tls :ram_init
        
    PROVIDE( __tdata_source = LOADADDR(.tdata) );
    PROVIDE( __tdata_size = SIZEOF(.tdata) );

    PROVIDE( metal_segment_data_source_start = LOADADDR(.data) );
    PROVIDE( metal_segment_data_target_start = ADDR(.data) );
    PROVIDE( metal_segment_data_target_end = ADDR(.tdata) + SIZEOF(.tdata) );

    .tbss : ALIGN(8) {
    *(.tbss .tbss.* .gnu.linkonce.tb.*)
    *(.tcommon .tcommon.*)
    PROVIDE( __tls_end = . );
    } >ram AT>ram :tls :ram
    PROVIDE( __tbss_size = SIZEOF(.tbss) );
    PROVIDE( __tls_size = __tls_end - __tls_base );

    .tbss_space : ALIGN(8) {
    . = . + __tbss_size;
    } >ram :ram

    .bss (NOLOAD): ALIGN(8) {
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
    } >ram :ram

    PROVIDE( metal_segment_bss_source_start = LOADADDR(.tbss) );
    PROVIDE( metal_segment_bss_target_start = ADDR(.tbss) );
    PROVIDE( metal_segment_bss_target_end = ADDR(.bss) + SIZEOF(.bss) );

 

    .stack (NOLOAD) : ALIGN(16) {
        PROVIDE(metal_segment_stack_begin = .);
        . += __stack_size; /* Hart 0 */
        PROVIDE( _sp = . );
        PROVIDE(metal_segment_stack_end = .);
    } >ram :ram

    .heap (NOLOAD) : ALIGN(8) {
        PROVIDE( __end = . );
        PROVIDE( end = . ); /* Required by libnosys.a in function _sbrk when compiled with --specs=nosys.specs */
        PROVIDE( __heap_start = . );
        PROVIDE( metal_segment_heap_target_start = . );
        /* If __heap_max is defined, grow the heap to use the rest of RAM,
         * otherwise set the heap size to __heap_size */
        . = DEFINED(__heap_max) ? MIN( LENGTH(ram) - ( . - ORIGIN(ram)) , 0x10000000) : __heap_size;
        PROVIDE( metal_segment_heap_target_end = . );
        PROVIDE( _heap_end = . );
        PROVIDE( __heap_end = . );
    } >ram :ram
    
    /* C++ Exception handling */
    .eh_frame_hdr   	: { *(.eh_frame_hdr) *(.eh_frame_entry .eh_frame_entry.*) } >ram :ram
    .eh_frame       	: ONLY_IF_RO { KEEP (*(.eh_frame)) *(.eh_frame.*) } >ram :ram
    .gcc_except_table   : ONLY_IF_RO { *(.gcc_except_table .gcc_except_table.*) } >ram :ram
    .gnu_extab   		: ONLY_IF_RO { *(.gnu_extab*) } >ram :ram
    /* These sections are generated by the Sun/Oracle C++ compiler.  */
    .exception_ranges   : ONLY_IF_RO { *(.exception_ranges*) } >ram :ram
    /* Adjust the address for the data segment.  We want to adjust up to
    the same address within the page on the next page up.  */
    . = DATA_SEGMENT_ALIGN (CONSTANT (MAXPAGESIZE), CONSTANT (COMMONPAGESIZE));
    /* Exception handling  */
    .eh_frame       	: ONLY_IF_RW { KEEP (*(.eh_frame)) *(.eh_frame.*) } >ram :ram
    .gnu_extab      	: ONLY_IF_RW { *(.gnu_extab) } >ram :ram
    .gcc_except_table   : ONLY_IF_RW { *(.gcc_except_table .gcc_except_table.*) } >ram :ram
    .exception_ranges   : ONLY_IF_RW { *(.exception_ranges*) } >ram :ram

    /* DWARF debug sections.
    Symbols in the DWARF debugging sections are relative to the beginning
    of the section so we begin them at 0.  */
    /* DWARF 1.  */
    .debug          0 : { *(.debug) }
    .line           0 : { *(.line) }
    /* GNU DWARF 1 extensions.  */
    .debug_srcinfo  0 : { *(.debug_srcinfo) }
    .debug_sfnames  0 : { *(.debug_sfnames) }
    /* DWARF 1.1 and DWARF 2.  */
    .debug_aranges  0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    /* DWARF 2.  */
    .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line .debug_line.* .debug_line_end) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_str      0 : { *(.debug_str) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_macinfo  0 : { *(.debug_macinfo) }
    /* SGI/MIPS DWARF 2 extensions.  */
    .debug_weaknames 0 : { *(.debug_weaknames) }
    .debug_funcnames 0 : { *(.debug_funcnames) }
    .debug_typenames 0 : { *(.debug_typenames) }
    .debug_varnames  0 : { *(.debug_varnames) }
    /* DWARF 3.  */
    .debug_pubtypes 0 : { *(.debug_pubtypes) }
    .debug_ranges   0 : { *(.debug_ranges) }
    /* DWARF 5.  */
    .debug_addr     0 : { *(.debug_addr) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_macro    0 : { *(.debug_macro) }
    .debug_names    0 : { *(.debug_names) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .debug_sup      0 : { *(.debug_sup) }
    .gnu.attributes 0 : { KEEP (*(.gnu.attributes)) }
    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
