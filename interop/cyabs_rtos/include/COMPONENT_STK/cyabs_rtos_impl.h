/***********************************************************************************************//**
 * \file cyabs_rtos_impl.h
 *
 * \brief
 * RTOS specific constants of the SuperTinyKernel (STK) port of the Infineon RTOS abstraction
 * layer.
 *
 ***************************************************************************************************
 * \copyright
 * Licensed under the Apache License, Version 2.0 (same license as the abstraction-rtos library).
 * SPDX-License-Identifier: Apache-2.0
 **************************************************************************************************/

#ifndef CYABS_RTOS_IMPL_H
#define CYABS_RTOS_IMPL_H

#include <stdint.h>
#include <stdbool.h>
#include "cyabs_rtos_stk_config.h"
#include "cyabs_rtos_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/******************************************************
*                 Constants
******************************************************/
#define CY_RTOS_MIN_STACK_SIZE  CY_RTOS_STK_MIN_STACK_BYTES /**< Minimum stack size in bytes */
#define CY_RTOS_ALIGNMENT       0x00000008UL                /**< Minimum alignment for RTOS
                                                               objects */
#define CY_RTOS_ALIGNMENT_MASK  0x00000007UL                /**< Checks for 8-byte alignment */

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* CYABS_RTOS_IMPL_H */
