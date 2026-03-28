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

#define STK_SYSTICK_HANDLER _STK_SYSTICK_HANDLER_DISABLE
#define _STK_ARCH_ARM_CORTEX_M
#define STK_TICKLESS_IDLE 0

#endif /* STK_CONFIG_H_ */
