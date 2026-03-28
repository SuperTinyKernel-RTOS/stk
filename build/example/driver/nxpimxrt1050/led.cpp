/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <assert.h>
#include "board.h"
#include "../led.h"

void Led::Init(Id led, bool init_state)
{
    uint8_t logic_state = (init_state ? LOGIC_LED_ON : LOGIC_LED_OFF);

    switch (led)
    {
    case Led::RED: break; // unavailabe on board
    case Led::GREEN: USER_LED_INIT(logic_state); break;
    case Led::BLUE: break; // unavailabe on board
    default:
        assert(false);
        break;
    }
}

void Led::Set(Id led, bool state)
{
    switch (led)
    {
    case Led::RED: break; // unavailabe on board
    case Led::GREEN: (state ? USER_LED_ON() : USER_LED_OFF()); break;
    case Led::BLUE: break; // unavailabe on board
    default:
        assert(false);
        break;
    }
}
