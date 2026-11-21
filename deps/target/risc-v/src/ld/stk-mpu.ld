/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  stk-mpu.ld
    \brief Linker script of the STK RISC-V target with MPU (PMP) support, for builds with STK_MPU enabled.
    \note  This is stk.ld extended with the \c .stk_mpu_xxx regions below. Use stk.ld if MPU is not used.
    \note  The MPU of RISC-V is the Physical Memory Protection (PMP) unit. Regions are laid out so that
           each one can be described by a single NAPOT PMP entry: its size is a power of two and its base
           address is naturally aligned to that size. As on ARM PMSAv7, a section is therefore padded to
           a fixed, tunable window instead of being sized to its actual content: the unused tail is
           inert padding still covered by the region.

    \par MPU regions
    \verbatim
    section                   memory  access (priv/user)  window (default)  tune with
    user ROM block            rom     RX / RX             256K              STK_MPU_ROM_WINDOW
      (.init .. .text, .stk_mpu_shared_code (8K, STK_MPU_SHARED_CODE_WINDOW) lies inside it)
    .stk_mpu_kernel_code      rom     RX / --             2K                STK_MPU_KERNEL_CODE_WINDOW
                                      (placed after the user ROM block, covered by no user PMP entry)
    .stk_mpu_shared_data/bss  ram     RW / RW (no exec)   4K (data + bss)   STK_MPU_SHARED_RAM_WINDOW
    .stk_mpu_kernel_data/bss  ram     RW / -- (no exec)   8K (data + bss)   STK_MPU_KERNEL_RAM_WINDOW
    stack (MSP/exception)     ram     RW / --             = __stack_size    __stack_size
    \endverbatim
    \note  Access is what OnConfigureMpu() is expected to program, see the \c STK_MPU_xxx_SECTION macros
           in stk_arch.h.
    \note  Windows are tuned with \c --defsym like the other options (see below). Each window must be
           a power of two and must be equal to the size given to the PMP entry in OnConfigureMpu(),
           not to the size of the content. Both conditions, as well as an overflow of the content of
           a window, are checked at link time. Re-tune to the smallest power of two that fits using
           the map file.
    \note  The stack size must also be a power of two, the stack is aligned to its size.

    \par Symbols of the MPU regions (consumed by OnConfigureMpu() and init/startup.c)
     - \c __stk_mpu_shared_code_start, \c __stk_mpu_shared_code_end
     - \c __stk_mpu_kernel_code_start, \c __stk_mpu_kernel_code_end
     - \c __stk_mpu_shared_data_start, \c __stk_mpu_shared_data_end, \c __stk_mpu_shared_data_source__ (image in ROM)
     - \c __stk_mpu_shared_bss_start,  \c __stk_mpu_shared_bss_end (end of the window)
     - \c __stk_mpu_kernel_data_start, \c __stk_mpu_kernel_data_end, \c __stk_mpu_kernel_data_source__ (image in ROM)
     - \c __stk_mpu_kernel_bss_start,  \c __stk_mpu_kernel_bss_end (end of the window)
     - \c __stk_mpu_msp_stack_start,   \c __stk_mpu_msp_stack_end
    \note  The data and bss halves of a window share one PMP entry, which covers
           [\c __stk_mpu_xxx_data_start, \c __stk_mpu_xxx_bss_end).

    \par Memory regions
     - \c rom  - flash/ROM: code, read-only data and load images of initialized data.
     - \c ram  - main RAM: .data, .bss, MPU windows, stack and heap.
     - \c itim - instruction tightly integrated memory: code placed in \c .itim* sections is
                 loaded to ROM and copied here by the startup code.

    \par Build-time options
    \note  Default origins/sizes of the regions match the SiFive FE310 (HiFive1 / QEMU "sifive_e")
           memory map. All values are in bytes and are overridden with \c --defsym:
           \code
           -Wl,--defsym=STK_RAM_SIZE=0x10000               // size of ram
           -Wl,--defsym=STK_ROM_SIZE=0x100000              // size of rom
           -Wl,--defsym=__stack_size=0x2000                // stack size (power of two)
           -Wl,--defsym=__heap_size=0x4000                 // heap size
           -Wl,--defsym=__heap_max=1                       // heap uses all RAM left after the stack
           -Wl,--defsym=STK_MPU_SHARED_RAM_WINDOW=0x1000   // MPU windows, see the table above
           -Wl,--defsym=STK_MPU_ROM_WINDOW=0x40000         // user ROM block, must hold all code/rodata
           \endcode
    \warning GNU ld processes options in order, so \c --defsym must appear \b before the \c -T stk-mpu.ld
             option on the link command line, otherwise it is silently ignored. Pass them with the
             compiler flags (CFLAGS/CXXFLAGS, \c -Xlinker \c --defsym=...) rather than with
             CMAKE_EXE_LINKER_FLAGS, which CMake places after \c -T.

    \par Symbols consumed by the startup code (see init/startup.c)
     - \c __stk_bss_start, \c __stk_bss_end - .bss range to zero.
     - \c __stk_data_load, \c __stk_data_start, \c __stk_data_end - .data/.tdata image and its RAM range.
     - \c __stk_itim_load, \c __stk_itim_start, \c __stk_itim_end - .itim image and its ITIM range.
     - \c __global_pointer$, \c _sp - initial \c gp and \c sp.
     - \c __heap_start, \c __heap_end - heap range (see init/system.c).

    \see stk.ld, STK_MPU_SHARED_DATA_SECTION, STK_MPU_KERNEL_DATA_SECTION
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

/*! \brief MPU windows (power of two), see \c STK_MPU_xxx_WINDOW in the description above. */
__stk_mpu_shared_code_window = DEFINED(STK_MPU_SHARED_CODE_WINDOW) ? STK_MPU_SHARED_CODE_WINDOW : 8K;
__stk_mpu_kernel_code_window = DEFINED(STK_MPU_KERNEL_CODE_WINDOW) ? STK_MPU_KERNEL_CODE_WINDOW : 2K;
__stk_mpu_shared_ram_window  = DEFINED(STK_MPU_SHARED_RAM_WINDOW)  ? STK_MPU_SHARED_RAM_WINDOW  : 4K;
__stk_mpu_kernel_ram_window  = DEFINED(STK_MPU_KERNEL_RAM_WINDOW)  ? STK_MPU_KERNEL_RAM_WINDOW  : 8K;

MEMORY
{
    itim (airwx) : ORIGIN = 0x08000000, LENGTH = 0x2000
    ram  (arw!xi) : ORIGIN = 0x80000000, LENGTH = __stk_ram_size
    rom  (irx!wa) : ORIGIN = 0x20010000, LENGTH = __stk_rom_size
}

/*! \brief User-visible ROM block (power of two), covers .init ... shared code window.
    \note  Tune with STK_MPU_ROM_WINDOW. The kernel code window is placed right after this block,
           outside of every user PMP entry, so that user code can not access it. The block base is
           ORIGIN(rom) rounded down to the window size (NAPOT alignment).
*/
__stk_mpu_rom_window     = DEFINED(STK_MPU_ROM_WINDOW) ? STK_MPU_ROM_WINDOW : 0x40000;
__stk_mpu_user_rom_start = ORIGIN(rom) & ~(__stk_mpu_rom_window - 1);
__stk_mpu_user_rom_end   = __stk_mpu_user_rom_start + __stk_mpu_rom_window;

PHDRS
{
    rom       PT_LOAD;
    itim_init PT_LOAD;
    text      PT_LOAD;
    ram_init  PT_LOAD;
    tls       PT_TLS;
    ram       PT_LOAD;
    mpu_shared PT_LOAD;
    mpu_kernel PT_LOAD;
    ram_tail  PT_LOAD;
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

    /*! \brief MPU: shared code window (ROM, read+execute for privileged and user code).
        \note  Kernel entry points which user tasks are allowed to call, see \c STK_MPU_SHARED_CODE_SECTION.
    */
    .stk_mpu_shared_code : ALIGN(__stk_mpu_shared_code_window)
    {
        __stk_mpu_shared_code_start = .;
        *(.stk_mpu_shared_code .stk_mpu_shared_code.*)
        ASSERT(. <= __stk_mpu_shared_code_start + __stk_mpu_shared_code_window,
               "STK: .stk_mpu_shared_code does not fit into STK_MPU_SHARED_CODE_WINDOW");
        . = MAX(., __stk_mpu_shared_code_start + __stk_mpu_shared_code_window);
        __stk_mpu_shared_code_end = .;
    } >rom :text

    /*! \brief MPU: kernel-only code window (ROM, read+execute for privileged code only).
        \note  Reachable only by the kernel itself, never by a task, see \c STK_MPU_KERNEL_CODE_SECTION.
    */
    /*! \note  The section has an explicit address (end of the user ROM block): a plain padding of \c .
               between sections is ignored for sections assigned to a memory region (\c >rom), and inside
               a section an absolute value assigned to \c . is taken as an offset from the section start.
    */
    .stk_mpu_kernel_code __stk_mpu_user_rom_end : ALIGN(__stk_mpu_kernel_code_window)
    {
        __stk_mpu_kernel_code_start = .;
        *(.stk_mpu_kernel_code .stk_mpu_kernel_code.*)
        ASSERT(. <= __stk_mpu_kernel_code_start + __stk_mpu_kernel_code_window,
               "STK: .stk_mpu_kernel_code does not fit into STK_MPU_KERNEL_CODE_WINDOW");
        . = MAX(., __stk_mpu_kernel_code_start + __stk_mpu_kernel_code_window);
        __stk_mpu_kernel_code_end = .;
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

    /*! \brief MPU: shared data/bss window (RAM, read+write, no execute, for privileged and user code).
        \note  The initialized half (\c .stk_mpu_shared_data, see \c STK_MPU_SHARED_DATA_SECTION) is loaded
               from ROM by the startup code. The zero-initialized half (\c .stk_mpu_shared_bss, see
               \c STK_MPU_SHARED_BSS_SECTION) has no image and is zeroed by the startup code up to the end
               of the window.
    */
    .stk_mpu_shared_data : AT(ALIGN(LOADADDR(.tdata) + SIZEOF(.tdata), 4)) ALIGN(__stk_mpu_shared_ram_window)
    {
        __stk_mpu_shared_data_start = .;
        *(.stk_mpu_shared_data .stk_mpu_shared_data.*)
        . = ALIGN(4);
        __stk_mpu_shared_data_end = .;
    } >ram :mpu_shared

    __stk_mpu_shared_data_source__ = LOADADDR(.stk_mpu_shared_data);

    .stk_mpu_shared_bss (NOLOAD) : AT(LOADADDR(.stk_mpu_shared_data) + SIZEOF(.stk_mpu_shared_data)) ALIGN(4)
    {
        __stk_mpu_shared_bss_start = .;
        *(.stk_mpu_shared_bss .stk_mpu_shared_bss.*)
        ASSERT(. <= __stk_mpu_shared_data_start + __stk_mpu_shared_ram_window,
               "STK: .stk_mpu_shared_data/bss do not fit into STK_MPU_SHARED_RAM_WINDOW");
        . = MAX(., __stk_mpu_shared_data_start + __stk_mpu_shared_ram_window);
        __stk_mpu_shared_bss_end = .;
    } >ram :mpu_shared

    /*! \brief MPU: kernel-only data/bss window (RAM, read+write for privileged code only, no execute).
        \note  Same layout and initialization as the shared window, see \c STK_MPU_KERNEL_DATA_SECTION
               and \c STK_MPU_KERNEL_BSS_SECTION.
    */
    .stk_mpu_kernel_data : AT(ALIGN(LOADADDR(.stk_mpu_shared_data) + SIZEOF(.stk_mpu_shared_data), 4)) ALIGN(__stk_mpu_kernel_ram_window)
    {
        __stk_mpu_kernel_data_start = .;
        *(.stk_mpu_kernel_data .stk_mpu_kernel_data.*)
        . = ALIGN(4);
        __stk_mpu_kernel_data_end = .;
    } >ram :mpu_kernel

    __stk_mpu_kernel_data_source__ = LOADADDR(.stk_mpu_kernel_data);

    .stk_mpu_kernel_bss (NOLOAD) : AT(LOADADDR(.stk_mpu_kernel_data) + SIZEOF(.stk_mpu_kernel_data)) ALIGN(4)
    {
        __stk_mpu_kernel_bss_start = .;
        *(.stk_mpu_kernel_bss .stk_mpu_kernel_bss.*)
        ASSERT(. <= __stk_mpu_kernel_data_start + __stk_mpu_kernel_ram_window,
               "STK: .stk_mpu_kernel_data/bss do not fit into STK_MPU_KERNEL_RAM_WINDOW");
        . = MAX(., __stk_mpu_kernel_data_start + __stk_mpu_kernel_ram_window);
        __stk_mpu_kernel_bss_end = .;
    } >ram :mpu_kernel

    /*! \brief Stack of the (single) hart (MSP/exception stack).
        \note  Grows down from \c _sp.
        \note  With MPU the stack is a region of its own, so it is aligned to its size.
    */
    .stack (NOLOAD) : ALIGN(__stk_stack_size)
    {
        __stk_stack_start = .;
        . += __stk_stack_size;
        PROVIDE(_sp = .);
        __stk_stack_end = .;
    } >ram AT>ram :ram_tail

    PROVIDE(__stk_mpu_msp_stack_start = __stk_stack_start);
    PROVIDE(__stk_mpu_msp_stack_end   = __stk_stack_end);

    .heap (NOLOAD) : ALIGN(8)
    {
        PROVIDE(__end = .);
        PROVIDE(end = .);   /*!< Expected by newlib's sbrk (libnosys). */
        PROVIDE(_end = .);
        PROVIDE(__heap_start = .);
        . = DEFINED(__heap_max) ? ORIGIN(ram) + LENGTH(ram) : . + __stk_heap_size;
        PROVIDE(__heap_end = .);
        PROVIDE(_heap_end = .);
    } >ram AT>ram :ram_tail

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

ASSERT((__stk_stack_size & (__stk_stack_size - 1)) == 0, "STK: __stack_size must be a power of two with MPU")
ASSERT((__stk_mpu_shared_code_window & (__stk_mpu_shared_code_window - 1)) == 0, "STK: STK_MPU_SHARED_CODE_WINDOW must be a power of two")
ASSERT((__stk_mpu_kernel_code_window & (__stk_mpu_kernel_code_window - 1)) == 0, "STK: STK_MPU_KERNEL_CODE_WINDOW must be a power of two")
ASSERT((__stk_mpu_rom_window & (__stk_mpu_rom_window - 1)) == 0, "STK: STK_MPU_ROM_WINDOW must be a power of two")
ASSERT(__stk_mpu_shared_code_end <= __stk_mpu_user_rom_end, "STK: code/rodata do not fit into STK_MPU_ROM_WINDOW")
ASSERT((__stk_mpu_shared_ram_window  & (__stk_mpu_shared_ram_window  - 1)) == 0, "STK: STK_MPU_SHARED_RAM_WINDOW must be a power of two")
ASSERT((__stk_mpu_kernel_ram_window  & (__stk_mpu_kernel_ram_window  - 1)) == 0, "STK: STK_MPU_KERNEL_RAM_WINDOW must be a power of two")
ASSERT((__stk_stack_size & 0xF) == 0, "STK: __stack_size must be a multiple of 16")
ASSERT(__heap_end <= ORIGIN(ram) + LENGTH(ram), "STK: stack + heap do not fit into RAM")
