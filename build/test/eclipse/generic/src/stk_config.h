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

// note: Generic tests do not use platform-dependent implementation

#define _STK_ASSERT

// if Kernel is not configured as tickless it should work as non-tickless with a little
// overhead of couple of instructions, by default we enable tickless code path to be able
// to test with KERNEL_TICKLESS and without it in the same test suite
#define STK_TICKLESS_IDLE (1)

// Use TLS.
#define STK_TLS (1)

// Use MPU.
#define STK_MPU (1)

// Simulate Secure side build.
#define STK_TZ_SECURE (1)
#define STK_TZ_NON_SECURE (0)

#endif /* STK_CONFIG_H_ */
