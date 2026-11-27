/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <inttypes.h>
#include <stdint.h>

#include "cmsis_device.h"
#include "diag/trace.h"
#include "exceptions.h"

#if (defined(STK_TRACE_SEMIHOSTING) || defined(STK_TRACE_SEMIHOSTING_DEBUG)) && (__CORTEX_M >= 3)
#include "../diag/semihosting.h"
#endif

/*! \file  exceptions.c
    \brief Default (weak) handlers of the core exceptions of the STK STM32Fx target.

    The kernel overrides SVC, PendSV and SysTick; an application overrides any other handler by defining a function
    of the same name. The fault handlers (HardFault, and MemManage/BusFault/UsageFault on Cortex-M3/M4) find the stack
    frame of the faulting code and pass it to a C function (\c xxx_Handler_C), which reports it on the trace channel
    and stops the CPU. In a DEBUG build the debugger gets a breakpoint before the CPU stops.
    \see   exceptions.h
*/

/*! \brief Stop the CPU: breakpoint for the debugger (DEBUG build), then a busy loop. */
static void __attribute__((noreturn, noinline)) stop_cpu(void)
{
#if defined(DEBUG)
    __asm volatile ("bkpt 0");
#endif
    for (;;)
    {
        // nothing to do
    }
}

/*! \def   STK_FAULT_ENTRY
    \brief Defines a fault handler which finds the stack the fault happened on and calls \a handler_C(frame, lr).
    \param[in] handler: Name of the exception handler.
    \note  Bit 2 of EXC_RETURN (LR) tells which stack was in use: 0 - MSP, 1 - PSP. The handler is naked, so no
           register is touched before the frame pointer is found.
*/
#if (__CORTEX_M >= 3)
#define STK_FAULT_ENTRY(handler)                    \
    void handler(void) __attribute__ ((weak, naked)); \
    void handler(void)                              \
    {                                               \
        __asm volatile (                            \
            " tst   lr, #4                 \n"      \
            " ite   eq                     \n"      \
            " mrseq r0, msp                \n"      \
            " mrsne r0, psp                \n"      \
            " mov   r1, lr                 \n"      \
            " ldr   r2, =" #handler "_C    \n"      \
            " bx    r2                     \n");    \
    }
#else
// Cortex-M0: no conditional execution of MRS and no TST with an immediate
#define STK_FAULT_ENTRY(handler)                    \
    void handler(void) __attribute__ ((weak, naked)); \
    void handler(void)                              \
    {                                               \
        __asm volatile (                            \
            " movs  r0, #4                 \n"      \
            " mov   r1, lr                 \n"      \
            " tst   r0, r1                 \n"      \
            " beq   1f                     \n"      \
            " mrs   r0, psp                \n"      \
            " b     2f                     \n"      \
            "1:                            \n"      \
            " mrs   r0, msp                \n"      \
            "2:                            \n"      \
            " mov   r1, lr                 \n"      \
            " ldr   r2, =" #handler "_C    \n"      \
            " bx    r2                     \n");    \
    }
#endif

// ----------------------------------------------------------------------------
// Fault reporting
// ----------------------------------------------------------------------------

/*! \brief     Report a fault on the trace channel and stop the CPU.
    \param[in] what:  Name of the fault.
    \param[in] frame: Exception stack frame.
    \param[in] lr:    EXC_RETURN value.
    \note      Cortex-M3/M4 only: the fault address registers are read by the caller before anything else can fault
               again, and are printed if the status bits say they hold a valid address.
*/
static void __attribute__((noreturn, noinline)) fault_report(const char *what, const ExceptionFrame_t *frame,
    uint32_t lr
#if (__CORTEX_M >= 3)
    , uint32_t cfsr, uint32_t mmfar, uint32_t bfar
#endif
    )
{
    (void)what;
    (void)frame;
    (void)lr;

#if defined(TRACE)
    trace_printf("[%s]\n", what);
    trace_printf("Stack frame:\n");
    trace_printf(" R0  = %08" PRIX32 "\n", frame->r0);
    trace_printf(" R1  = %08" PRIX32 "\n", frame->r1);
    trace_printf(" R2  = %08" PRIX32 "\n", frame->r2);
    trace_printf(" R3  = %08" PRIX32 "\n", frame->r3);
    trace_printf(" R12 = %08" PRIX32 "\n", frame->r12);
    trace_printf(" LR  = %08" PRIX32 "\n", frame->lr);
    trace_printf(" PC  = %08" PRIX32 "\n", frame->pc);
    trace_printf(" PSR = %08" PRIX32 "\n", frame->psr);
#if (__CORTEX_M >= 3)
    trace_printf("Fault status:\n");
    trace_printf(" CFSR = %08" PRIX32 "\n", cfsr);
    trace_printf(" HFSR = %08" PRIX32 "\n", SCB->HFSR);
    trace_printf(" DFSR = %08" PRIX32 "\n", SCB->DFSR);
    trace_printf(" AFSR = %08" PRIX32 "\n", SCB->AFSR);
    if ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0U)
        trace_printf(" MMFAR = %08" PRIX32 "\n", mmfar);
    if ((cfsr & SCB_CFSR_BFARVALID_Msk) != 0U)
        trace_printf(" BFAR  = %08" PRIX32 "\n", bfar);
#endif
    trace_printf("EXC_RETURN = %08" PRIX32 "\n", lr);
#endif // TRACE

    stop_cpu();
}

#if (__CORTEX_M >= 3)

/*! \brief   Read the fault status and the fault addresses, report the fault, never returns.
    \details The addresses are read first: a new fault while the handler runs could overwrite them.
*/
#define FAULT_REPORT(what, frame, lr)                 \
    do {                                              \
        const uint32_t mmfar = SCB->MMFAR;            \
        const uint32_t bfar  = SCB->BFAR;             \
        const uint32_t cfsr  = SCB->CFSR;             \
        fault_report(what, frame, lr, cfsr, mmfar, bfar); \
    } while (0)

#else

#define FAULT_REPORT(what, frame, lr) fault_report(what, frame, lr)

#endif

#if (defined(STK_TRACE_SEMIHOSTING) || defined(STK_TRACE_SEMIHOSTING_DEBUG)) && (__CORTEX_M >= 3)

/*! \brief     Let a program with semihosting trace run without a debugger.
    \details   BKPT executed with no debugger attached raises a HardFault (DEBUGEVT, BKPT set). If the instruction is a
               semihosting call to write text, the text is dropped and the execution continues after the BKPT.
    \param[in,out] frame: Exception stack frame, PC is moved over the BKPT.
    \return    true if the fault was a semihosting write and it was consumed.
*/
static int semihosting_without_debugger(ExceptionFrame_t *frame)
{
    if (((SCB->DFSR & SCB_DFSR_BKPT_Msk) == 0U) || ((SCB->HFSR & SCB_HFSR_DEBUGEVT_Msk) == 0U))
        return 0;

    if (*(const uint16_t *)frame->pc != (uint16_t)(0xBE00U | STK_SEMIHOSTING_BKPT_NUMBER)) // BKPT #imm8
        return 0;

    switch (frame->r0)
    {
    case STK_SEMIHOSTING_SYS_OPEN:   frame->r0 = 1U; break; // fake handle, writes below are dropped
    case STK_SEMIHOSTING_SYS_WRITE:  frame->r0 = 0U; break; // 0 bytes left unwritten: all consumed
    case STK_SEMIHOSTING_SYS_WRITE0:
    case STK_SEMIHOSTING_SYS_WRITEC: break;
    default: return 0;
    }

    SCB->DFSR = SCB_DFSR_BKPT_Msk;      // write one to clear
    SCB->HFSR = SCB_HFSR_DEBUGEVT_Msk;  // write one to clear
    frame->pc += 2U;                    // continue after the 16-bit BKPT instruction
    return 1;
}

#endif

// ----------------------------------------------------------------------------
// Fault handlers
// ----------------------------------------------------------------------------

STK_FAULT_ENTRY(HardFault_Handler)

void HardFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr) __attribute__ ((weak, used));

void HardFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr)
{
#if (defined(STK_TRACE_SEMIHOSTING) || defined(STK_TRACE_SEMIHOSTING_DEBUG)) && (__CORTEX_M >= 3)
    if (semihosting_without_debugger(frame))
        return;
#endif

    FAULT_REPORT("HardFault", frame, lr);
}

#if (__CORTEX_M >= 3)

STK_FAULT_ENTRY(MemManage_Handler)
STK_FAULT_ENTRY(BusFault_Handler)
STK_FAULT_ENTRY(UsageFault_Handler)

void MemManage_Handler_C(ExceptionFrame_t *frame, uint32_t lr) __attribute__ ((weak, used));
void BusFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr) __attribute__ ((weak, used));
void UsageFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr) __attribute__ ((weak, used));

void MemManage_Handler_C(ExceptionFrame_t *frame, uint32_t lr)
{
    FAULT_REPORT("MemManage", frame, lr);
}

void BusFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr)
{
    FAULT_REPORT("BusFault", frame, lr);
}

void UsageFault_Handler_C(ExceptionFrame_t *frame, uint32_t lr)
{
    FAULT_REPORT("UsageFault", frame, lr);
}

#endif

// ----------------------------------------------------------------------------
// Other core exceptions
// ----------------------------------------------------------------------------

void NMI_Handler(void) __attribute__ ((weak));
void SVC_Handler(void) __attribute__ ((weak));
void PendSV_Handler(void) __attribute__ ((weak));
void SysTick_Handler(void) __attribute__ ((weak));

void NMI_Handler(void)
{
    stop_cpu();
}

void SVC_Handler(void)
{
    stop_cpu();
}

void PendSV_Handler(void)
{
    stop_cpu();
}

void SysTick_Handler(void)
{
    // the HAL initializes SysTick (HAL_Init); advance its tick until the kernel installs its own handler
    HAL_IncTick();
}

#if (__CORTEX_M >= 3)

void DebugMon_Handler(void) __attribute__ ((weak));

void DebugMon_Handler(void)
{
    stop_cpu();
}

#endif
