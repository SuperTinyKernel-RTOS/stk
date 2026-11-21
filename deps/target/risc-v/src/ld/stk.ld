/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  stk.ld
    \brief Default linker script of the STK RISC-V target (single hart, bare-metal).
    \note  Use stk-mpu.ld instead if STK_MPU is enabled.

    \par Memory regions
     - \c rom  - flash/ROM: code, read-only data and load images of initialized data.
     - \c ram  - main RAM: .data, .bss, stack and heap.
     - \c itim - instruction tightly integrated memory: code placed in \c .itim* sections is
                 loaded to ROM and copied here by the startup code.

    \par Build-time options
    \note  Default origins/sizes of the regions match the SiFive FE310 (HiFive1 / QEMU "sifive_e")
           memory map. All values are in bytes and are overridden with \c --defsym:
           \code
           -Wl,--defsym=STK_RAM_SIZE=0x10000   // size of ram
           -Wl,--defsym=STK_ROM_SIZE=0x100000  // size of rom
           -Wl,--defsym=__stack_size=0x2000    // stack size (multiple of 16)
           -Wl,--defsym=__heap_size=0x4000     // heap size
           -Wl,--defsym=__heap_max=1           // heap uses all RAM left after the stack
           \endcode
    \warning GNU ld processes options in order, so \c --defsym must appear \b before the \c -T stk.ld
             option on the link command line, otherwise it is silently ignored. Pass them with the
             compiler flags (CFLAGS/CXXFLAGS, \c -Xlinker \c --defsym=...) rather than with
             CMAKE_EXE_LINKER_FLAGS, which CMake places after \c -T.

    \par Symbols consumed by the startup code (see init/startup.c)
     - \c __stk_bss_start, \c __stk_bss_end - .bss range to zero.
     - \c __stk_data_load, \c __stk_data_start, \c __stk_data_end - .data/.tdata image and its RAM range.
     - \c __stk_itim_load, \c __stk_itim_start, \c __stk_itim_end - .itim image and its ITIM range.
     - \c __global_pointer$, \c _sp - initial \c gp and \c sp.
     - \c __heap_start, \c __heap_end - heap range (see init/system.c).

    \see stk-mpu.ld
*/

OUTPUT_ARCH("riscv")

ENTRY(_enter)

/*! \brief Build-time overrides (--defsym), read into separate symbols.
    \note  Assigning a symbol from itself (\c X = DEFINED(X) ? X : default) does not pick up the --defsym
           value reliably across linker versions, therefore the internal \c __stk_xxx symbols are used.
*/
__stk_ram_size   = DEFINED(STK_RAM_SIZE) ? STK_RAM_SIZE : 0x40000;
__stk_rom_size   = DEFINED(STK_ROM_SIZE) ? STK_ROM_SIZE : 0x200000;
__stk_stack_size = DEFINED(__stack_size) ? __stack_size : 0x8000;
__stk_heap_size  = DEFINED(__heap_size)  ? __heap_size  : 0x800;

MEMORY
{
    itim (airwx) : ORIGIN = 0x08000000, LENGTH = 0x2000
    ram  (arw!xi) : ORIGIN = 0x80000000, LENGTH = __stk_ram_size
    rom  (irx!wa) : ORIGIN = 0x20010000, LENGTH = __stk_rom_size
}

PHDRS
{
    rom       PT_LOAD;
    itim_init PT_LOAD;
    text      PT_LOAD;
    ram_init  PT_LOAD;
    tls       PT_TLS;
    ram       PT_LOAD;
}

SECTIONS
{
    /*! \brief ROM: everything that stays in read-only memory. */

    /*! \brief Reset entry point (\c _enter).
        \note  Must be the first thing in ROM.
    */
    .init :
    {
        KEEP (*(.text.stk.init.enter))
        KEEP (*(.text.stk.init.*))
        KEEP (*(SORT_NONE(.init)))
    } >rom :rom

    .fini :
    {
        KEEP (*(SORT_NONE(.fini)))
    } >rom :rom

    .preinit_array : ALIGN(4)
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } >rom :rom

    .init_array : ALIGN(4)
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
    } >rom :rom

    .fini_array : ALIGN(4)
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } >rom :rom

    .ctors :
    {
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
    } >rom :rom

    .dtors :
    {
        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
    } >rom :rom

    .rodata :
    {
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

    /*! \brief Linker-generated build ID note (if enabled).
        \note  Kept with the read-only data, otherwise the linker places this orphan section arbitrarily.
    */
    .note.gnu.build-id :
    {
        *(.note.gnu.build-id)
    } >rom :rom

    /*! \brief C++ exception handling tables.
        \note  Empty if built with -fno-exceptions.
    */
    .eh_frame_hdr :
    {
        *(.eh_frame_hdr)
        *(.eh_frame_entry .eh_frame_entry.*)
    } >rom :rom

    .eh_frame : ONLY_IF_RO
    {
        KEEP (*(.eh_frame))
        *(.eh_frame.*)
    } >rom :rom

    .gcc_except_table : ONLY_IF_RO
    {
        *(.gcc_except_table .gcc_except_table.*)
    } >rom :rom

    .gnu_extab : ONLY_IF_RO
    {
        *(.gnu_extab*)
    } >rom :rom

    /*! \brief ITIM: performance-critical code, executed from instruction TCM.
        \note  Linked at the ITIM address, stored in ROM and copied by the startup code.
    */
    .itim : ALIGN(8)
    {
        __stk_itim_start = .;
        *(.itim .itim.*)
        . = ALIGN(8);
        __stk_itim_end = .;
    } >itim AT>rom :itim_init

    __stk_itim_load = LOADADDR(.itim);

    /*! \brief Code. */
    .text :
    {
        *(.text.unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    } >rom :text

    /*! \brief RAM: initialized data (loaded from ROM), thread-local storage template,
               zero-initialized data, stack and heap.
    */

    /*! \brief Initialized data and the TLS template.
        \note  .data and .tdata are copied by the startup code as one contiguous block.
    */
    .data : ALIGN(8)
    {
        __stk_data_start = .;
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        /*! \note Small data is addressed relative to \c gp, which points 2 KiB into it. */
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.* .sdata2.*)
        *(.gnu.linkonce.s.*)
    } >ram AT>rom :ram_init

    .tdata : ALIGN(8)
    {
        PROVIDE(__tls_base = .);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        . = ALIGN(8);
        __stk_data_end = .;
    } >ram AT>rom :tls :ram_init

    __stk_data_load = LOADADDR(.data);
    PROVIDE(__tdata_source = LOADADDR(.tdata));
    PROVIDE(__tdata_size   = SIZEOF(.tdata));

    .tbss : ALIGN(8)
    {
        __stk_bss_start = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon .tcommon.*)
        PROVIDE(__tls_end = .);
    } >ram AT>rom :tls

    PROVIDE(__tbss_size = SIZEOF(.tbss));
    PROVIDE(__tls_size  = __tls_end - __tls_base);

    /*! \brief Space of .tbss.
        \note  .tbss only describes the TLS image and takes no space itself, so it is reserved here.
    */
    .tbss_space (NOLOAD) : ALIGN(8)
    {
        . = . + __tbss_size;
    } >ram AT>ram :ram

    .bss (NOLOAD) : ALIGN(8)
    {
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        __stk_bss_end = .;
    } >ram AT>ram :ram

    /*! \brief Stack of the (single) hart.
        \note  Grows down from \c _sp.
    */
    .stack (NOLOAD) : ALIGN(16)
    {
        __stk_stack_start = .;
        . += __stk_stack_size;
        PROVIDE(_sp = .);
        __stk_stack_end = .;
    } >ram AT>ram :ram

    .heap (NOLOAD) : ALIGN(8)
    {
        PROVIDE(__end = .);
        PROVIDE(end = .);   /*!< Expected by newlib's sbrk (libnosys). */
        PROVIDE(_end = .);
        PROVIDE(__heap_start = .);
        . = DEFINED(__heap_max) ? ORIGIN(ram) + LENGTH(ram) : . + __stk_heap_size;
        PROVIDE(__heap_end = .);
        PROVIDE(_heap_end = .);
    } >ram AT>ram :ram

    /*! \brief Writable variants of the exception handling sections.
        \note  Rare, normally these sections are placed in ROM (see above).
    */
    .eh_frame : ONLY_IF_RW
    {
        KEEP (*(.eh_frame))
        *(.eh_frame.*)
    } >ram :ram

    .gcc_except_table : ONLY_IF_RW
    {
        *(.gcc_except_table .gcc_except_table.*)
    } >ram :ram

    .gnu_extab : ONLY_IF_RW
    {
        *(.gnu_extab)
    } >ram :ram

    /*! \brief Debug information.
        \note  Not loaded, addresses start at 0.
    */
    .debug              0 : { *(.debug) }
    .line               0 : { *(.line) }
    .debug_srcinfo      0 : { *(.debug_srcinfo) }
    .debug_sfnames      0 : { *(.debug_sfnames) }
    .debug_aranges      0 : { *(.debug_aranges) }
    .debug_pubnames     0 : { *(.debug_pubnames) }
    .debug_info         0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev       0 : { *(.debug_abbrev) }
    .debug_line         0 : { *(.debug_line .debug_line.* .debug_line_end) }
    .debug_frame        0 : { *(.debug_frame) }
    .debug_str          0 : { *(.debug_str) }
    .debug_loc          0 : { *(.debug_loc) }
    .debug_macinfo      0 : { *(.debug_macinfo) }
    .debug_weaknames    0 : { *(.debug_weaknames) }
    .debug_funcnames    0 : { *(.debug_funcnames) }
    .debug_typenames    0 : { *(.debug_typenames) }
    .debug_varnames     0 : { *(.debug_varnames) }
    .debug_pubtypes     0 : { *(.debug_pubtypes) }
    .debug_ranges       0 : { *(.debug_ranges) }
    .debug_addr         0 : { *(.debug_addr) }
    .debug_line_str     0 : { *(.debug_line_str) }
    .debug_loclists     0 : { *(.debug_loclists) }
    .debug_macro        0 : { *(.debug_macro) }
    .debug_names        0 : { *(.debug_names) }
    .debug_rnglists     0 : { *(.debug_rnglists) }
    .debug_str_offsets  0 : { *(.debug_str_offsets) }
    .debug_sup          0 : { *(.debug_sup) }
    .gnu.attributes     0 : { KEEP (*(.gnu.attributes)) }

    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.gnu_debuglink)
        *(.gnu.lto_*)
    }
}

ASSERT((__stk_stack_size & 0xF) == 0, "STK: __stack_size must be a multiple of 16")
ASSERT(__heap_end <= ORIGIN(ram) + LENGTH(ram), "STK: stack + heap do not fit into RAM")
