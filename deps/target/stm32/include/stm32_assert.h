/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STM32FX_STM32_ASSERT_H_
#define STK_STM32FX_STM32_ASSERT_H_

#include <stdint.h>

/*! \file  stm32_assert.h
    \brief Parameter check of the STM32 HAL (\c assert_param), included by the HAL configuration when
           USE_FULL_ASSERT is defined.
*/

#ifdef __cplusplus
extern "C" {
#endif

#if defined(USE_FULL_ASSERT)

/*! \brief     Report a failed parameter check: prints the location to the trace channel and halts.
    \param[in] file: Source file name.
    \param[in] line: Source line.
    \note      Weak, can be overridden by the application. Never returns.
*/
void assert_failed(uint8_t *file, uint32_t line);

/* the HAL configuration of some families (STM32F7) defines assert_param itself */
#ifndef assert_param
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
#endif

#else

#ifndef assert_param
#define assert_param(expr) ((void)0U)
#endif

#endif /* USE_FULL_ASSERT */

#ifdef __cplusplus
}
#endif

#endif /* STK_STM32FX_STM32_ASSERT_H_ */
