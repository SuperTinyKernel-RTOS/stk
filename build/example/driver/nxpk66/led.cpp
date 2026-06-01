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

using namespace bsp;

void Led::Init(Id led, bool init_state)
{
    uint8_t logic_state = (init_state ? LOGIC_LED_ON : LOGIC_LED_OFF);

    switch (led)
    {
    case Led::RED: LED_RED_INIT(logic_state); break;
    case Led::GREEN: LED_GREEN_INIT(logic_state); break;
    case Led::BLUE: LED_BLUE_INIT(logic_state); break;
    case Led::ORANGE: LED_BLUE_INIT(logic_state); LED_RED_INIT(logic_state); break;
    default:
        break;
    }
}

void Led::Set(Id led, bool state)
{
    switch (led)
    {
    case Led::RED: (state ? LED_RED_ON() : LED_RED_OFF()); break;
    case Led::GREEN: (state ? LED_GREEN_ON() : LED_GREEN_OFF()); break;
    case Led::BLUE: (state ? LED_BLUE_ON() : LED_BLUE_OFF()); break;
    case Led::ORANGE:
        if (state)
        {
            LED_BLUE_ON();
            LED_RED_ON();
        }
        else
        {
            LED_BLUE_OFF();
            LED_RED_OFF();
        }
        break;
    default:
        break;
    }
}
