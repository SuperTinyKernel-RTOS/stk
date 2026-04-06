/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "led.h"

// C interface
extern "C" {

void Led_Init(LedId led, bool init_state)
{
    Led::Init(led, init_state);
}

void Led_Set(LedId led, bool state)
{
    Led::Set(led, state);
}

} // extern "C"
