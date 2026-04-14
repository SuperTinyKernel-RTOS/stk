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

#define _STK_ARCH_X86_WIN32

// Tickless mode is the most power-efficient as CPU is put into a low-power when tasks have no work.
#define STK_TICKLESS_IDLE 1

#endif /* STK_CONFIG_H_ */
