/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

#include "cmsis_device.h"

// Undefine if MCU is Arm Cortex-M4
#define _STK_ARCH_ARM_CORTEX_M

// Low-power scenario, use (0) for a high-performance processing when consumed power does not matter
#define STK_TICKLESS_IDLE       (1U)

// For C interface:
#define STK_C_CPU_COUNT         (1U)
#define STK_C_KERNEL_MAX_TASKS  (4U)
#define STK_C_KERNEL_TYPE_CPU_0 Kernel<KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),\
                                    STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>

#ifdef _STK_ARCH_ARM_CORTEX_M
    // Redefine if SysTick handler name is different from SysTick_Handler
    //#define STK_SYSTICK_HANDLER SysTick_Handler

    // Redefine if PendSv handler name is different from PendSV_Handler
    //#define STK_PENDSV_HANDLER PendSV_Handler

    // Redefine if SVC handler name is different from SVC_Handler
    //#define STK_SVC_HANDLER SVC_Handler
#endif

#endif /* STK_CONFIG_H_ */
