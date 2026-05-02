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

using namespace bsp;

static bool g_LedState[LED_MAX] = {};

static const char *Led_GetPin(Led::Id led)
{
    switch (led)
    {
    case LED_RED: return "RED";
    case LED_ORANGE: return "ORANGE";
    case LED_GREEN: return "GREEN";
    case LED_BLUE: return "BLUE";
    default:
        return "UNK";
    }
}

static void PrintMsg(const char *label, Led::Id led, bool state)
{
    static const time_t g_SecNow = time(NULL);
    time_t now = time(NULL);

    printf("%ds [%s]: %s - %s\n", (int)(now - g_SecNow), label, Led_GetPin(led), (state ? "ON" : "OFF"));
}

void Led::Init(Id led, bool init_state)
{
    // required to show log in Eclipse IDE Console for 64-bit binary
#if defined(_WIN32) && !defined(_MSC_VER)
    static bool s_init = false;
    if (!s_init)
    {
        setbuf(stdout, NULL);
        s_init = true;
    }
#endif

    if (static_cast<uint32_t>(led) < LED_MAX)
        g_LedState[led] = init_state;

    PrintMsg("LED_INIT", led, init_state);
}

void Led::Set(Id led, bool state)
{
    if (static_cast<uint32_t>(led) < LED_MAX)
    {
        if (g_LedState[led] == state)
            return;

        g_LedState[led] = state;
    }

    PrintMsg("LED_SET_STATE", led, state);
}
