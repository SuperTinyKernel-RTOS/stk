/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  mem.ld
    \brief Memory map of the STK STM32F4 target (default device STM32F407xx), used together with sections.ld.
    \note  The sizes are overridden with \c --defsym, which must appear \b before the \c -T mem.ld option on the
           link command line (GNU ld processes options in order). Pass them with the compiler flags
           (\c -Xlinker \c --defsym=...), not with CMAKE_EXE_LINKER_FLAGS, which CMake places after \c -T:
           \code
           -Xlinker --defsym=STK_FLASH_SIZE=0x20000
           -Xlinker --defsym=STK_RAM_SIZE=0x4000
           \endcode
           Region origins are fixed by the device, edit the MEMORY block for external memory or a bootloader offset.
*/

/* the internal symbols are used because "X = DEFINED(X) ? X : default" does not pick up --defsym reliably */
__stk_flash_size = DEFINED(STK_FLASH_SIZE) ? STK_FLASH_SIZE : 1024K;
__stk_ram_size   = DEFINED(STK_RAM_SIZE)   ? STK_RAM_SIZE   : 128K;
__stk_ccm_size   = 64K;

MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = __stk_flash_size
    RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = __stk_ram_size
    CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = __stk_ccm_size
}
