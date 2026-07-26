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

// TrustZone interface config
#define STK_TZ_SECURE_STACK_SIZE    (1024U)
#define STK_TZ_NON_SECURE_TASKS_MAX (4U)

// MPU
#define STK_MPU               (0)
#define STK_MPU_STACK_GUARD   (0)

// Let STK process MemManage and HardFault ISRs
#define STK_USE_MEMMANAGE_HANDLER (1)
#define STK_USE_HARDFAULT_HANDLER (1)

// Define _STK_CPU_COUNT as 2 to use STK on both CPU cores or on CPU1, if 1 then STK can be hosted on CPU0 only
#define STK_ARCH_CPU_COUNT    (2U)
#define STK_ARCH_GET_CPU_ID() (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET)) // see get_core_num() in pico/platform.h

// RP2350 ISR handlers, see crt0.S of pico-sdk
#define STK_SYSTICK_HANDLER   isr_systick
#define STK_PENDSV_HANDLER    isr_pendsv
#define STK_SVC_HANDLER       isr_svcall
#define STK_MEMMANAGE_HANDLER isr_memmanage  // optional, see STK_USE_MEMMANAGE_HANDLER
#define STK_HARDFAULT_HANDLER isr_hardfault  // optional, see STK_USE_HARDFAULT_HANDLER

#endif /* STK_CONFIG_H_ */
