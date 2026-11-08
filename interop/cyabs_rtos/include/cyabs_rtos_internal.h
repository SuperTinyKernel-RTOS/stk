/***********************************************************************************************//**
 * \file cyabs_rtos_internal.h
 *
 * \brief
 * Internal interface used for RTOS abstraction utilities.
 ***************************************************************************************************
 * \copyright
 * (c) 2019-2025, Infineon Technologies AG, or an affiliate of Infineon
 * Technologies AG.  SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **************************************************************************************************/

#ifndef CYABS_RTOS_INTERNAL_H
#define CYABS_RTOS_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "cyabs_rtos_device_caps.h"
#if !defined (CY_RTOS_IS_H1_DEVICE)
#include <cmsis_compiler.h>
#endif

/** Checks to see if code is currently executing within an interrupt context.
 *
 * @return Boolean indicating whether this was executed from an interrupt context.
 */
static inline bool is_in_isr(void)
{
    #if defined(COMPONENT_CR4) // Can work for any Cortex-A & Cortex-R
    uint32_t mode = __get_mode();
    return (mode == 0x11U /*FIQ*/) || (mode == 0x12U /*IRQ*/) || (mode == 0x13U /*SVC*/) ||
           (mode == 0x17U /*ABT*/) || (mode == 0x1BU /*UND*/);
    #elif defined (CY_RTOS_IS_H1_DEVICE)
    // This device does not allow calling from interrupt context.
    return false;
    #else // Cortex-M
    return (__get_IPSR() != 0U);
    #endif
}


#if defined(__cplusplus)
}
#endif

#endif /* CYABS_RTOS_INTERNAL_H */
