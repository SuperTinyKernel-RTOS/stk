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

// RISC-V arch.
#define _STK_ARCH_RISC_V

// Use tickless mode for battery-powered devices or when max power saving is required
#define STK_TICKLESS_IDLE (0)

// Enable MPU support
#ifndef STK_MPU
    #define STK_MPU (1)
#endif
#define STK_MPU_STACK_GUARD  (1)
#define STK_MPU_TASK_REGIONS (2) // set to 8 for more protection of MPU against modifications (reprogrammed fully on every context switch)

#endif /* STK_CONFIG_H_ */
