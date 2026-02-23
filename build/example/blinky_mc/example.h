/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef EXAMPLE_H_
#define EXAMPLE_H_

#include <assert.h>
#include "../driver/led.h"
#include "../driver/cpu.h"

#ifdef __cplusplus
    #define STK_EXTERN extern "C"
#else
    #define STK_EXTERN extern
#endif

STK_EXTERN void RunExample();

#endif /* EXAMPLE_H_ */
