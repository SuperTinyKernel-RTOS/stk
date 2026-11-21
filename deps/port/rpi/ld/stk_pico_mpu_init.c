/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  stk_mpu_init.cpp
    \brief Initializes MPU memory regions.
*/

#include <string.h> // for memcpy, memset

#include <pico/runtime.h>

// Init MPU data windows: the Pico SDK's default linker script/runtime does not know
// about STK's custom .stk_mpu_*_data/.stk_mpu_*_bss sections, so their FLASH->RAM copy
// and zero-fill has to be done manually, and it must happen before any C++ static
// constructor runs (globals living in those sections - like g_TaskFlags and g_Timeline
// below - would otherwise be constructed on top of uninitialized/stale memory).
// On RISC-V the kernel window (.stk_mpu_kernel_data/bss, used by the driver itself, e.g.
// g_StkLastPanicId, s_StkPlatformContext) must be initialized the same way.
//
// Two ways to guarantee that ordering are shown here; this example uses the second:
//   1. __attribute__((constructor(101))) - 101 is the highest priority available to
//      user code, forcing the function to the very start of the __init_array loop
//      (kept below, commented out, for reference).
//   2. PICO_RUNTIME_INIT_FUNC(..., "00100") - registers the function in the Pico
//      SDK's preinit_array section, which the SDK's runtime guarantees to run
//      before init_array (and therefore before any static constructor).

static void stk_init_mpu_window(char *data_start, const char *data_end, const char *data_source,
                                char *bss_start, const char *bss_end)
{
    // copy initialized variables from FLASH to RAM
    const size_t data_size = data_end - data_start;
    if (data_size > 0)
    {
        memcpy(data_start, data_source, data_size);
    }

    // zero-fill the BSS region
    const size_t bss_size = bss_end - bss_start;
    if (bss_size > 0)
    {
        memset(bss_start, 0, bss_size);
    }
}

//__attribute__((constructor(101)))
static void stk_init_mpu_memory()
{
    extern char __stk_mpu_shared_data_start[];
    extern char __stk_mpu_shared_data_end[];
    extern char __stk_mpu_shared_data_source__[];
    extern char __stk_mpu_shared_bss_start[];
    extern char __stk_mpu_shared_bss_end[];

    stk_init_mpu_window(__stk_mpu_shared_data_start, __stk_mpu_shared_data_end, __stk_mpu_shared_data_source__,
                        __stk_mpu_shared_bss_start, __stk_mpu_shared_bss_end);

#if defined(__riscv)
    extern char __stk_mpu_kernel_data_start[];
    extern char __stk_mpu_kernel_data_end[];
    extern char __stk_mpu_kernel_data_source__[];
    extern char __stk_mpu_kernel_bss_start[];
    extern char __stk_mpu_kernel_bss_end[];

    stk_init_mpu_window(__stk_mpu_kernel_data_start, __stk_mpu_kernel_data_end, __stk_mpu_kernel_data_source__,
                        __stk_mpu_kernel_bss_start, __stk_mpu_kernel_bss_end);
#endif
}
PICO_RUNTIME_INIT_FUNC(stk_init_mpu_memory, "00100");
