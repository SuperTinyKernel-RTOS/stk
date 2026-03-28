/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef DRIVER_SMP_H_
#define DRIVER_SMP_H_

#include <stdint.h>

#ifdef __cplusplus

struct Cpu
{
    static void Start(uint8_t cpu_id, void (*entry_func)(void));
};

#else

    void Cpu_Start(uint8_t cpu_id, void (*entry_func)(void));

#endif

#endif /* DRIVER_SMP_H_ */
