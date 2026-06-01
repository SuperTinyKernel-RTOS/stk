/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <assert.h>
#include "pico/stdlib.h"

// Use GPIO of WiFi chip
#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif

#include "pico/multicore.h"

#include "../cpu.h"

using namespace bsp;

void Cpu::Start(uint8_t cpu_id, void (*entry_func)(void))
{
    assert(cpu_id < 2);

    if (cpu_id == 0)
        entry_func();
    else
    if (cpu_id == 1)
        multicore_launch_core1(entry_func);
}

// C interface
extern "C" {

void Cpu_Start(uint8_t cpu_id, void (*entry_func)(void))
{
    Cpu::Start(cpu_id, entry_func);
}

} // extern "C"
