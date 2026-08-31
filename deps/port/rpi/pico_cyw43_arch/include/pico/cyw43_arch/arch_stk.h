/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_CYW43_ARCH_ARCH_STK_H
#define _PICO_CYW43_ARCH_ARCH_STK_H

// Version of STK interface
#define CYW43_STK_VERSION (0x20260801)

// PICO_CONFIG: CYW43_NO_DEFAULT_TASK_STACK, Disables the default static allocation of the CYW43 STK task stack, type=bool, default=0, group=pico_cyw43_arch
#ifndef CYW43_NO_DEFAULT_TASK_STACK
#define CYW43_NO_DEFAULT_TASK_STACK (0U)
#endif

// PICO_CONFIG: CYW43_TASK_STACK_SIZE, Stack size for the CYW43 STK task in 4-byte words, type=int, default=1024, group=pico_cyw43_arch
#ifndef CYW43_TASK_STACK_SIZE
    #ifdef MBEDTLS_CONFIG_FILE
        #define CYW43_TASK_STACK_SIZE (2048U) // mbedTLS needs more stack memory
    #else
        #define CYW43_TASK_STACK_SIZE (1024U)
    #endif
#endif

// PICO_CONFIG: CYW43_TASK_PRIORITY, Priority for the CYW43 STK task, type=int, default=IDLE_PRIORITY (0) + 4, group=pico_cyw43_arch
#ifndef CYW43_TASK_PRIORITY
#define CYW43_TASK_PRIORITY (stk::DEFAULT_WEIGHT + 4U)
#endif

#endif
