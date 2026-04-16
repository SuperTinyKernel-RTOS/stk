/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <assert.h>
#include "stm32f4xx.h"
#include "stm32f4xx_hal_gpio.h"
#include "../led.h"

#define LED_PORT GPIOD

static void Led_InitGpio(int32_t pin)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {};
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pin = pin;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = 0;
    HAL_GPIO_Init(LED_PORT, &gpio);
}

static int32_t Led_GetPin(Led::Id led)
{
    switch (led)
    {
    case Led::RED: return GPIO_PIN_14;
    case Led::GREEN: return GPIO_PIN_12;
    case Led::BLUE: return GPIO_PIN_15;
    default:
        return GPIO_PIN_MASK;
    }
}

void Led::Init(Id led, bool init_state)
{
    uint16_t pin = Led_GetPin(led);
    if (GPIO_PIN_MASK != pin)
    {
        Led_InitGpio(Led_GetPin(led));
        Set(led, init_state);
    }
}

void Led::Set(Id led, bool state)
{
    uint16_t pin = Led_GetPin(led);
    if (GPIO_PIN_MASK != pin)
        HAL_GPIO_WritePin(LED_PORT, pin, (state ? GPIO_PIN_SET : GPIO_PIN_RESET));
}
