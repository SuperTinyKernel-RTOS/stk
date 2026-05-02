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

#include "../led.h"

using namespace bsp;

#define NO_LED (~0)

static void Led_InitGpio(uint16_t pin)
{
    if (pin == NO_LED)
        return;

#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    static bool s_init = false;
    if (!s_init)
    {
        cyw43_arch_init();
        s_init = true;
    }
#endif
}

static uint16_t Led_GetPin(Led::Id led)
{
    switch (led)
    {
    case Led::GREEN:
    #if defined(PICO_DEFAULT_LED_PIN)
        return PICO_DEFAULT_LED_PIN;
    #elif defined(CYW43_WL_GPIO_LED_PIN)
        return CYW43_WL_GPIO_LED_PIN;
    #endif
    default:
        return NO_LED;
    }
}

void Led::Init(Id led, bool init_state)
{
    uint16_t pin = Led_GetPin(led);
    if (NO_LED == pin)
        return;

    Led_InitGpio(Led_GetPin(led));
    Set(led, init_state);
}

void Led::Set(Id led, bool state)
{
    uint16_t pin = Led_GetPin(led);
    if (NO_LED == pin)
        return;

#if defined(PICO_DEFAULT_LED_PIN)
    gpio_put(pin, state);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(pin, state);
#endif
}
