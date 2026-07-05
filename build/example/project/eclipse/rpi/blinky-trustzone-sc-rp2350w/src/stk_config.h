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

#include <RP2350.h>
#include <pico.h>

// Use ARM Cortex-M33 cores of RP2350
#define _STK_ARCH_ARM_CORTEX_M

// Use ARM TrustZone feature: Secure-side
#define _STK_CORTEX_M_TRUSTZONE

#define STK_SECURE_STACK_SIZE (1024)

#define STK_MAX_NS_TASKS      (4U)

// Define _STK_CPU_COUNT as 2 to use STK on both CPU cores or on CPU1, if 1 then STK can be hosted on CPU0 only
#define STK_ARCH_CPU_COUNT    (2U)
#define STK_ARCH_GET_CPU_ID() (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET)) // see get_core_num() in pico/platform.h

// RP2350 ISR handlers, see crt0.S of pico-sdk
#define STK_SYSTICK_HANDLER   isr_systick
#define STK_PENDSV_HANDLER    isr_pendsv
#define STK_SVC_HANDLER       isr_svcall

#endif /* STK_CONFIG_H_ */
