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

// ARM Cortex-M4 platform
#define _STK_ARCH_ARM_CORTEX_M

// Low-power scenario, use (0) for a high-performance processing when consumed power does not matter
#define STK_TICKLESS_IDLE       (1U)

// Max SVCall priority.
#define STK_CORTEX_M_SVCALL_ISR_PRIORITY (0U)

#endif /* STK_CONFIG_H_ */
