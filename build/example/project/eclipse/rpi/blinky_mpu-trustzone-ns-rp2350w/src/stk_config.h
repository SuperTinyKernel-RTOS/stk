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

// Use ARM TrustZone feature: Non-Secure side (DO NOT define _STK_CORTEX_M_TRUSTZONE)
#define _STK_CORTEX_M_TRUSTZONE_NON_SECURE

// Enable MPU support
#define STK_MPU               (1)

// Provide STD API via NSC interface.
#define STK_TZ_NS_STD_API     (1)

// Let STK process MemManage and HardFault ISRs
#define STK_USE_MEMMANAGE_HANDLER (1)
#define STK_USE_HARDFAULT_HANDLER (1)

// Define _STK_CPU_COUNT as 2 to use STK on both CPU cores or on CPU1, if 1 then STK can be hosted on CPU0 only
#define STK_ARCH_CPU_COUNT    (2)
#define STK_ARCH_GET_CPU_ID() (*(uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET)) // see get_core_num() in pico/platform.h

// RP2350 ISR handlers, see crt0.S of pico-sdk
#define STK_MEMMANAGE_HANDLER isr_memmanage  // optional, see STK_USE_MEMMANAGE_HANDLER

#endif /* STK_CONFIG_H_ */
