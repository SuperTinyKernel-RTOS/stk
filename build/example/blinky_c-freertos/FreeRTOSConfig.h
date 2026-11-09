/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

// Minimal FreeRTOS configuration for the FreeRTOS wrapper (see interop/freertos/README.md).

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configMAX_PRIORITIES                    8U    // must be <= 32
#define configMINIMAL_STACK_SIZE                256U  // words; used by application code, not enforced by wrapper
#define configTOTAL_HEAP_SIZE                   (32 * 1024U)
#define portMAX_DELAY                           0xFFFFFFFFUL

#endif /* FREERTOS_CONFIG_H */
