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

// MCU is Arm Cortex-M.
#define _STK_ARCH_ARM_CORTEX_M

// Low-power scenario, use (0) for a high-performance processing when consumed power does not matter.
#define STK_TICKLESS_IDLE (1)

// Use TLS.
#define STK_TLS (1)

// Use fast inline TLS based on CPU register (-ffixed-r9 compiler flag is used to preserve R9 for TLS).
#define STK_TLS_PREFER_REGISTER (1)

// Override ISR handlers if your BSP are using other names.
#ifdef _STK_ARCH_ARM_CORTEX_M
    // Redefine if SysTick handler name is different from SysTick_Handler
    //#define STK_SYSTICK_HANDLER SysTick_Handler

    // Redefine if PendSv handler name is different from PendSV_Handler
    //#define STK_PENDSV_HANDLER PendSV_Handler

    // Redefine if SVC handler name is different from SVC_Handler
    //#define STK_SVC_HANDLER SVC_Handler
#endif

#endif /* STK_CONFIG_H_ */
