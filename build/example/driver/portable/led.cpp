/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include "../led.h"

static const char *Led_GetPin(Led::Id led)
{
    switch (led)
    {
    case LED_RED: return "RED";
    case LED_GREEN: return "GREEN";
    case LED_BLUE: return "BLUE";
    default:
        assert(false);
        return NULL;
    }
}

void Log(const char *label, Led::Id led, bool state)
{
    static const time_t g_SecNow = time(NULL);
    time_t now = time(NULL);

    printf("%ds [%s]: %s - %s\n", (int)(now - g_SecNow), label, Led_GetPin(led), (state ? "ON" : "OFF"));
}

void Led::Init(Id led, bool init_state)
{
    Log("LED_INIT", led, init_state);

    // required to show log in Eclipse IDE Console for 64-bit binary
#if defined(_WIN32) && !defined(_MSC_VER)
    setbuf(stdout, NULL);
#endif
}

void Led::Set(Id led, bool state)
{
    Log("LED_SET_STATE", led, state);
}
