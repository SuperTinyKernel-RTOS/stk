/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STM32FX_CMSIS_DEVICE_H_
#define STK_STM32FX_CMSIS_DEVICE_H_

/*! \file  cmsis_device.h
    \brief Device header of the STK STM32Fx target, exported to the rest of STK as \c _STK_DEVICE_INC.
    \note  The family macro (STM32F0, STM32F1 or STM32F4) and the device macro (e.g. STM32F407xx) are defined by
           ProjectInclude.cmake. The headers included here are the CMSIS-Core and CMSIS device headers which are
           fetched from STMicroelectronics by deps/scripts/fetch_deps.py.
*/

#if defined(STM32F0)
    #include "stm32f0xx.h"
#elif defined(STM32F1)
    #include "stm32f1xx.h"
#elif defined(STM32F4)
    #include "stm32f4xx.h"
#elif defined(STM32F7)
    #include "stm32f7xx.h"
#else
    #error "Unsupported STM32 family: define STM32F0, STM32F1, STM32F4 or STM32F7 (see ProjectInclude.cmake)."
#endif

#endif /* STK_STM32FX_CMSIS_DEVICE_H_ */
