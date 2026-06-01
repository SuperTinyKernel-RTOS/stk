/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <assert.h>
#include "stm32f0xx.h"
#include "stm32f0xx_hal_gpio.h"
#include "../led.h"

using namespace bsp;

#define LED_PORT GPIOC

static void Led_InitGpio(uint16_t pin)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {};
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pin = pin;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = 0;
    HAL_GPIO_Init(LED_PORT, &gpio);
}

static uint16_t Led_GetPin(Led::Id led)
{
    switch (led)
    {
    case Led::RED: return GPIO_PIN_MASK; // unavailable on STM32F0DISCOVERY board
    case Led::GREEN: return GPIO_PIN_9;
    case Led::BLUE: return GPIO_PIN_8;
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
