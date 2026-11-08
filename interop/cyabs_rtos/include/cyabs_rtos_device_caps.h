/***********************************************************************************************//**
 * \file cyabs_rtos_device_caps.h
 *
 * \brief
 * Device capability macros used to avoid broad COMPONENT checks.
 ***************************************************************************************************
 * \copyright
 * (c) 2019-2026, Infineon Technologies AG, or an affiliate of Infineon
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

#ifndef CYABS_RTOS_DEVICE_CAPS_H
#define CYABS_RTOS_DEVICE_CAPS_H

#ifdef __cplusplus
extern "C"
{
#endif

#if (defined(CY_IP_MXS40SSRSS) || defined(CY_IP_MXS22SRSS)) && !defined(CY_DEVICE_PSE84)
#define CY_RTOS_HAS_DEEPSLEEP_RAM
#endif

/* Current H1 devices with known ThreadX ROM-specific behavior. */
#if defined(CYW55500A0) || defined(CYW55500A1) || defined(CYW55900A0)
#define CY_RTOS_IS_H1_DEVICE
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* CYABS_RTOS_DEVICE_CAPS_H */
