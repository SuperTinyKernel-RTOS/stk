/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STM32FX_EXCEPTIONS_H_
#define STK_STM32FX_EXCEPTIONS_H_

#include <stdint.h>

#include "cmsis_device.h"

/*! \file  exceptions.h
    \brief Entry points of the vector table and the exception handlers of the STK STM32Fx target.
    \see   exceptions.c, startup.c, vectors_xxx.c
*/

#ifdef __cplusplus
extern "C" {
#endif

/*! \union stk_vector_t
    \brief Entry of the vector table: initial stack pointer (entry 0) or handler.
*/
typedef union
{
    void (*isr)(void); //!< Handler.
    const void *stack; //!< Initial value of the main stack pointer.
} IsrVector_t;

/*! \brief Top of the main stack, defined by the linker script. */
extern const uint8_t _estack;

/*! \brief Exception stack frame pushed by the core on exception entry (without the optional FPU registers). */
typedef struct
{
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t psr;
} ExceptionFrame_t;

/*! \name  Core exception handlers.
    \note  Weak (exceptions.c), the kernel and the application override them by defining a function of the same
           name. NMI, SVC, DebugMon and PendSV stop the CPU, SysTick advances the HAL tick.
*/
///@{
void Reset_Handler(void);
void NMI_Handler(void);
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
#if (__CORTEX_M >= 3)
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);
#endif
///@}

/*! \name  C part of the fault handlers.
    \param[in] frame: Exception stack frame (of the MSP or PSP stack, whichever was in use).
    \param[in] lr:    EXC_RETURN value.
    \note  Weak, an application can override them to implement its own fault reporting. By default the fault is
           reported to the trace channel and the CPU stops.
*/
///@{
void HardFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr);
#if (__CORTEX_M >= 3)
void MemManage_Handler_C(ExceptionFrame_t *frame, uint32_t lr);
void BusFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr);
void UsageFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr);
#endif
///@}

#ifdef __cplusplus
}
#endif

#endif /* STK_STM32FX_EXCEPTIONS_H_ */
