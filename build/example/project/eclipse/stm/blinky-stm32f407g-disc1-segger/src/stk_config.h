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
#include "core_cm4.h"

// ARM Cortex-M4 platform
#define _STK_ARCH_ARM_CORTEX_M

#define STK_SEGGER_SYSVIEW (1)
#define STK_TICKLESS_IDLE  (1)

#if STK_SEGGER_SYSVIEW
#include <SEGGER_SYSVIEW.h>
#endif

#endif /* STK_CONFIG_H_ */
