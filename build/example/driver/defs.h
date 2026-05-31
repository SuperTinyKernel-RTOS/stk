/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef DRIVER_DEFS_H_
#define DRIVER_DEFS_H_

#ifdef __cplusplus
    #include <cstdbool>
    #include <cstdint>
#else
    #include <stdbool.h>
    #include <stdint.h>
#endif

#ifdef __cplusplus
    #define STK_EXTERN extern "C"
#else
    #define STK_EXTERN extern
#endif

#endif /* DRIVER_DEFS_H_ */
