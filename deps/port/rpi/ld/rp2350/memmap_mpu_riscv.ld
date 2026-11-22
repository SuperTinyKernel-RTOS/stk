/* RP2350 (Hazard3 RISC-V) linker script with STK MPU (PMP) support.
   Based on the pico-sdk memmap_default.ld / ARM variant memmap_mpu.ld.

   ============================================================================
   WHY A SEPARATE SCRIPT FROM memmap_mpu.ld (ARM)
   ============================================================================
   RISC-V PMP regions used by STK are NAPOT only: size is a power of two and
   the base is aligned to that size. ARMv8-M MPU (memmap_mpu.ld) only needs a
   32-byte aligned base/limit, so a section there can be sized to its content.
   Here every MPU window is padded to a fixed, tunable power-of-two size and
   aligned to it, so one window == one PMP entry. The unused tail of a window
   is inert padding still covered by the region.

   The symbol names and window semantics match stk-mpu.ld (QEMU RISC-V), so
   the RISC-V branch of the MPU example works unchanged. RP2350 only adds
   the RAM-text window (see below).

   section                    mem    access (priv/user)  default window  tune with
   user ROM block             FLASH  RX / RX             256K            STK_MPU_ROM_WINDOW
     (.text .rodata .stk_mpu_shared_code (8K, STK_MPU_SHARED_CODE_WINDOW) inside)
   .stk_mpu_kernel_code       FLASH  RX / --             2K              STK_MPU_KERNEL_CODE_WINDOW
     (pinned right after the user ROM block, covered by no user PMP entry)
   RAM text (in .data)        RAM    RX / RX             8K              STK_MPU_RAM_TEXT_WINDOW
     (libgcc, libc mem-functions and libm code AND their .rodata tables the SDK script keeps in RAM)
   .stk_mpu_shared_data/bss   RAM    RW / RW (no exec)   4K (data+bss)   STK_MPU_SHARED_RAM_WINDOW
   .stk_mpu_kernel_data/bss   RAM    RW / --  (no exec)  8K (data+bss)   STK_MPU_KERNEL_RAM_WINDOW

   Each window must be a power of two and EXACTLY equal the size given to the
   PMP entry in OnConfigureMpu() (use end - start, never the content size).
   Power-of-two, alignment and overflow are checked at link time. Re-tune to
   the smallest power of two that fits using the map file.

   Exported symbols (consumed by OnConfigureMpu() and the data init code):
     __stk_mpu_user_rom_start / _end
     __stk_mpu_shared_code_start / _end
     __stk_mpu_kernel_code_start / _end
     __stk_mpu_ram_text_start / _end
     __stk_mpu_shared_data_start / _end, __stk_mpu_shared_data_source__
     __stk_mpu_shared_bss_start  / _end (end of the whole window)
     __stk_mpu_kernel_data_start / _end, __stk_mpu_kernel_data_source__
     __stk_mpu_kernel_bss_start  / _end (end of the whole window)
   The data and bss halves of a window share ONE PMP entry:
     [__stk_mpu_xxx_data_start, __stk_mpu_xxx_bss_end)

   The pico-sdk crt0 does not know the .stk_mpu_*_data/bss sections: their
   FLASH->RAM copy and zero-fill must be done by the application before any
   static constructor runs (see InitMpuData() in example.cpp). This applies to
   the KERNEL window as well, the driver keeps its state there.

   Build-time options (--defsym), e.g.
     -Wl,--defsym=STK_MPU_ROM_WINDOW=0x20000
   NOTE: GNU ld processes options in order, --defsym must come BEFORE -T/the
   linker script on the command line, otherwise it is silently ignored.

   Memory layout of FLASH: the kernel code window is pinned right after the
   user ROM block, so the binary is padded up to ROM window size. Keep
   STK_MPU_ROM_WINDOW as small as the firmware allows to save flash size.

   Defines the following symbols for use by code:
    __exidx_start __exidx_end __etext __data_start__ __preinit_array_start
    __preinit_array_end __init_array_start __init_array_end __fini_array_start
    __fini_array_end __data_end__ __bss_start__ __bss_end__ __end__ end
    __HeapLimit __StackLimit __StackTop __stack (== StackTop)
*/

MEMORY
{
    INCLUDE "pico_flash_region.ld"
    RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k
    SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

/* MPU windows (power of two), overridable with --defsym, see description above.
   Internal __stk_xxx symbols are used because "X = DEFINED(X) ? X : d" is not
   reliable across ld versions. */
__stk_mpu_rom_window         = DEFINED(STK_MPU_ROM_WINDOW)         ? STK_MPU_ROM_WINDOW         : 512K;
__stk_mpu_shared_code_window = DEFINED(STK_MPU_SHARED_CODE_WINDOW) ? STK_MPU_SHARED_CODE_WINDOW : 8K;
__stk_mpu_kernel_code_window = DEFINED(STK_MPU_KERNEL_CODE_WINDOW) ? STK_MPU_KERNEL_CODE_WINDOW : 2K;
__stk_mpu_ram_text_window    = DEFINED(STK_MPU_RAM_TEXT_WINDOW)    ? STK_MPU_RAM_TEXT_WINDOW    : 8K;
__stk_mpu_shared_ram_window  = DEFINED(STK_MPU_SHARED_RAM_WINDOW)  ? STK_MPU_SHARED_RAM_WINDOW  : 4K;
__stk_mpu_kernel_ram_window  = DEFINED(STK_MPU_KERNEL_RAM_WINDOW)  ? STK_MPU_KERNEL_RAM_WINDOW  : 8K;

/* User-visible ROM block (NAPOT): ORIGIN(FLASH) rounded down to the window size. */
__stk_mpu_user_rom_start = ORIGIN(FLASH) & ~(__stk_mpu_rom_window - 1);
__stk_mpu_user_rom_end   = __stk_mpu_user_rom_start + __stk_mpu_rom_window;

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
    /* PART 1: SHARED MPU CODE WINDOW (FLASH, RX for user and privileged)       */
    /* Lies inside the user ROM block, NAPOT: size == shared code window.       */
    /* ======================================================================== */
    .stk_mpu_shared_code : ALIGN(__stk_mpu_shared_code_window)
    {
        __stk_mpu_shared_code_start = .;
        *(.stk_mpu_shared_code .stk_mpu_shared_code.*)
        ASSERT(. <= __stk_mpu_shared_code_start + __stk_mpu_shared_code_window,
               "STK: .stk_mpu_shared_code does not fit into STK_MPU_SHARED_CODE_WINDOW");
        . = MAX(., __stk_mpu_shared_code_start + __stk_mpu_shared_code_window);
        __stk_mpu_shared_code_end = .;
    } > FLASH

    /* Kernel-only code window (FLASH, no user PMP entry). Explicit address: the end of the
       user ROM block, a plain padding of "." between sections is ignored for >FLASH sections. */
    .stk_mpu_kernel_code __stk_mpu_user_rom_end : ALIGN(__stk_mpu_kernel_code_window)
    {
        __stk_mpu_kernel_code_start = .;
        *(.stk_mpu_kernel_code .stk_mpu_kernel_code.*)
        ASSERT(. <= __stk_mpu_kernel_code_start + __stk_mpu_kernel_code_window,
               "STK: .stk_mpu_kernel_code does not fit into STK_MPU_KERNEL_CODE_WINDOW");
        . = MAX(., __stk_mpu_kernel_code_start + __stk_mpu_kernel_code_window);
        __stk_mpu_kernel_code_end = .;
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
        /* PART 2: RAM CODE WINDOW FOR MPU (RX for user + privileged) */
        /* NAPOT: aligned to the window, padded to the window size.  */
        /* ========================================================= */
        . = ALIGN(__stk_mpu_ram_text_window);
        __stk_mpu_ram_text_start = .;

        *(.text*)                        /* Excluded text/libgcc code */

        /* RAM-resident rodata (e.g. libgcc __clz_tab used by __divdi3, libc/libm tables):
           U-mode code reads it, so it must live inside the window. This also pulls in
           every other .rodata* input section that is not placed in FLASH above. */
        . = ALIGN(4);
        *(.rodata*)

        ASSERT(. <= __stk_mpu_ram_text_start + __stk_mpu_ram_text_window,
               "STK: RAM text+rodata does not fit into STK_MPU_RAM_TEXT_WINDOW");
        . = MAX(., __stk_mpu_ram_text_start + __stk_mpu_ram_text_window);
        __stk_mpu_ram_text_end = .;
        /* ========================================================= */

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
    /* PART 3: MPU DATA WINDOWS (RAM, RW, no execute). NAPOT: aligned to the    */
    /* window, data+bss padded to the window size. Loaded/zeroed by the app     */
    /* (see InitMpuData() in example.cpp), not by crt0.                         */
    /* ======================================================================== */

    /* 3.1 Shared window: accessible by every task (user PMP entry) */
    .stk_mpu_shared_data : ALIGN(__stk_mpu_shared_ram_window)
    {
        __stk_mpu_shared_data_start = .;
        *(.stk_mpu_shared_data .stk_mpu_shared_data.*)
        . = ALIGN(4);
        __stk_mpu_shared_data_end = .;
    } > RAM AT> FLASH

    __stk_mpu_shared_data_source__ = LOADADDR(.stk_mpu_shared_data);

    .stk_mpu_shared_bss (NOLOAD) : ALIGN(4)
    {
        __stk_mpu_shared_bss_start = .;
        *(.stk_mpu_shared_bss .stk_mpu_shared_bss.*)
        ASSERT(. <= __stk_mpu_shared_data_start + __stk_mpu_shared_ram_window,
               "STK: .stk_mpu_shared_data/bss do not fit into STK_MPU_SHARED_RAM_WINDOW");
        . = MAX(., __stk_mpu_shared_data_start + __stk_mpu_shared_ram_window);
        __stk_mpu_shared_bss_end = .;
    } > RAM

    /* 3.2 Kernel window: privileged code only (no user PMP entry), the driver keeps its state here */
    .stk_mpu_kernel_data : ALIGN(__stk_mpu_kernel_ram_window)
    {
        __stk_mpu_kernel_data_start = .;
        *(.stk_mpu_kernel_data .stk_mpu_kernel_data.*)
        . = ALIGN(4);
        __stk_mpu_kernel_data_end = .;
    } > RAM AT> FLASH

    __stk_mpu_kernel_data_source__ = LOADADDR(.stk_mpu_kernel_data);

    .stk_mpu_kernel_bss (NOLOAD) : ALIGN(4)
    {
        __stk_mpu_kernel_bss_start = .;
        *(.stk_mpu_kernel_bss .stk_mpu_kernel_bss.*)
        ASSERT(. <= __stk_mpu_kernel_data_start + __stk_mpu_kernel_ram_window,
               "STK: .stk_mpu_kernel_data/bss do not fit into STK_MPU_KERNEL_RAM_WINDOW");
        . = MAX(., __stk_mpu_kernel_data_start + __stk_mpu_kernel_ram_window);
        __stk_mpu_kernel_bss_end = .;
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

    /* MPU: every window must be a power of two (NAPOT) and the content must fit */
    ASSERT((__stk_mpu_rom_window         & (__stk_mpu_rom_window         - 1)) == 0, "STK: STK_MPU_ROM_WINDOW must be a power of two")
    ASSERT((__stk_mpu_shared_code_window & (__stk_mpu_shared_code_window - 1)) == 0, "STK: STK_MPU_SHARED_CODE_WINDOW must be a power of two")
    ASSERT((__stk_mpu_kernel_code_window & (__stk_mpu_kernel_code_window - 1)) == 0, "STK: STK_MPU_KERNEL_CODE_WINDOW must be a power of two")
    ASSERT((__stk_mpu_ram_text_window    & (__stk_mpu_ram_text_window    - 1)) == 0, "STK: STK_MPU_RAM_TEXT_WINDOW must be a power of two")
    ASSERT((__stk_mpu_shared_ram_window  & (__stk_mpu_shared_ram_window  - 1)) == 0, "STK: STK_MPU_SHARED_RAM_WINDOW must be a power of two")
    ASSERT((__stk_mpu_kernel_ram_window  & (__stk_mpu_kernel_ram_window  - 1)) == 0, "STK: STK_MPU_KERNEL_RAM_WINDOW must be a power of two")
    ASSERT(__stk_mpu_shared_code_end <= __stk_mpu_user_rom_end, "STK: code/rodata do not fit into STK_MPU_ROM_WINDOW")
    ASSERT((__stk_mpu_ram_text_start  & (__stk_mpu_ram_text_window   - 1)) == 0, "STK: RAM text window is not naturally aligned")
    ASSERT((__stk_mpu_shared_data_start & (__stk_mpu_shared_ram_window - 1)) == 0, "STK: shared RAM window is not naturally aligned")

    /* todo assert on extra code */
}

