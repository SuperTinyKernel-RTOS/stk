/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STM32FX_F7XX_HAL_CONF_H_
#define STK_STM32FX_F7XX_HAL_CONF_H_

/*! \file  stm32f7xx_hal_conf.h
    \brief HAL configuration of the STK STM32F7 target.
    \note  The configuration is the template which STMicroelectronics publishes with the HAL driver
           (stm32f7xx_hal_driver/Inc/stm32f7xx_hal_conf_template.h, fetched by deps/scripts/fetch_deps.py): all HAL
           modules are enabled. Oscillator values are taken from the compiler definitions (HSE_VALUE, see
           ProjectInclude.cmake). Only the HAL core, RCC and Cortex modules are built into the \c stm32fx library, add
           the sources of other modules to the application if it uses them.
           To customize the configuration, define the macros here before including the template.
*/

#include "stm32f7xx_hal_conf_template.h"

#endif /* STK_STM32FX_F7XX_HAL_CONF_H_ */
