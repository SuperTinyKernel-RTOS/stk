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

// Use tickless mode for battery-powered devices or when max power saving is required
#define STK_TICKLESS_IDLE 1

// ARM Cortex-M4 platform
#define _STK_ARCH_ARM_CORTEX_M

#endif /* STK_CONFIG_H_ */
