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
#define STK_TICKLESS_IDLE (1)

// Increase the stack size of the sleep handler as we do an extended processing there.
#define STK_SLEEP_TRAP_STACK_SIZE (256)

// For C interface:
#define STK_C_CPU_COUNT         (1)
#define STK_C_KERNEL_MAX_TASKS  (4)
#define STK_C_KERNEL_MODE       (KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0))
#define STK_C_KERNEL_TYPE_CPU_0 Kernel<STK_C_KERNEL_MODE, STK_C_KERNEL_MAX_TASKS + stk::time::TimerHost::TASK_COUNT, \
                                    SwitchStrategyRR, PlatformDefault>

#endif /* STK_CONFIG_H_ */
