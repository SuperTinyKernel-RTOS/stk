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

#include "risc-v/encoding.h"

// Risc-V
//#define _STK_ARCH_RISC_V

// Minimal stack size depending on the configured architecture (STK default is 32).
#if (__riscv_32e != 1)
    #if (__riscv_flen == 0)
        #define STK_STACK_SIZE_MIN 64
    #else
        #define STK_STACK_SIZE_MIN 128
    #endif
#endif

// Use TLS.
#define STK_TLS (1)
#define STK_TLS_PREFER_REGISTER (0)

// Use MPU.
#define STK_MPU (1)

// Simulate Secure side build.
#define STK_TZ_SECURE (1)
#define STK_TZ_NON_SECURE (0)

#endif /* STK_CONFIG_H_ */
