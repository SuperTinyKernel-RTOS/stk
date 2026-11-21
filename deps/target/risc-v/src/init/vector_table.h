/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_RISCV_VECTOR_TABLE_H_
#define STK_RISCV_VECTOR_TABLE_H_

/*! \file  vector_table.h
    \brief Trap vector tables and interrupt handlers of the STK RISC-V target.

    Every handler declared here has a weak default implementation (an alias to an empty "nop"
    handler, see vector_table.c). To handle an event define a function with the same name in the
    application: the strong definition replaces the weak one. The function must carry the matching
    interrupt attribute, for example:
    \code
    extern "C" void riscv_mtvec_mti(void) __attribute__ ((interrupt ("machine")));
    \endcode

    Tables are indexed by the interrupt cause (\c mcause / \c scause / \c ucause, see "Machine/Supervisor
    Trap Vector Base-Address Register" of the RISC-V Privileged Architecture), each entry is a single
    4-byte jump to the handler of that cause.

    \note  In vectored mode interrupt cause 0 shares the entry with synchronous exceptions. This is not
           ambiguous in practice: user software interrupts are either disabled or delegated to user mode.
    \note  Define \c VECTOR_TABLE_MTVEC_PLATFORM_INTS (in both vector_table.c and the application) to exclude
           the platform-specific interrupts 0-15 (\c mcause 16-31) from the machine table and to provide
           your own handlers or table for them.
*/

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Vector tables
// ----------------------------------------------------------------------------

/*! \brief   Machine mode vector table, 256-byte aligned.
    \note    Its address (with the vectored mode bit) is programmed to \c mtvec.
    \warning Do not call, the symbol is a table of jumps, not a function.
*/
void riscv_mtvec_table(void) __attribute__ ((naked));

/*! \brief   Supervisor mode vector table, 256-byte aligned.
    \note    Its address (with the vectored mode bit) is programmed to \c stvec.
    \warning Do not call, the symbol is a table of jumps, not a function.
*/
void riscv_stvec_table(void) __attribute__ ((naked));

/*! \brief   User mode vector table, 256-byte aligned.
    \note    Its address (with the vectored mode bit) is programmed to \c utvec.
    \warning Do not call, the symbol is a table of jumps, not a function.
*/
void riscv_utvec_table(void) __attribute__ ((naked));

// ----------------------------------------------------------------------------
// Machine mode table (mtvec)
// ----------------------------------------------------------------------------

/*! \brief Synchronous exception, also interrupt cause 0 in vectored mode. */
void riscv_mtvec_exception(void) __attribute__ ((interrupt ("machine")));

/*! \brief Machine software interrupt (cause 3). */
void riscv_mtvec_msi(void) __attribute__ ((interrupt ("machine")));

/*! \brief Machine timer interrupt (cause 7). */
void riscv_mtvec_mti(void) __attribute__ ((interrupt ("machine")));

/*! \brief Machine external interrupt (cause 11). */
void riscv_mtvec_mei(void) __attribute__ ((interrupt ("machine")));

/*! \brief Supervisor software interrupt (cause 1), taken in machine mode. */
void riscv_mtvec_ssi(void) __attribute__ ((interrupt ("machine")));

/*! \brief Supervisor timer interrupt (cause 5), taken in machine mode. */
void riscv_mtvec_sti(void) __attribute__ ((interrupt ("machine")));

/*! \brief Supervisor external interrupt (cause 9), taken in machine mode. */
void riscv_mtvec_sei(void) __attribute__ ((interrupt ("machine")));

// ----------------------------------------------------------------------------
// Supervisor mode table (stvec)
// ----------------------------------------------------------------------------

/*! \brief Synchronous exception, also interrupt cause 0 in vectored mode. */
void riscv_stvec_exception(void) __attribute__ ((interrupt ("supervisor")));

/*! \brief Supervisor software interrupt (cause 1). */
void riscv_stvec_ssi(void) __attribute__ ((interrupt ("supervisor")));

/*! \brief Supervisor timer interrupt (cause 5). */
void riscv_stvec_sti(void) __attribute__ ((interrupt ("supervisor")));

/*! \brief Supervisor external interrupt (cause 9). */
void riscv_stvec_sei(void) __attribute__ ((interrupt ("supervisor")));

// ----------------------------------------------------------------------------
// User mode table (utvec)
// ----------------------------------------------------------------------------

/*! \brief User software interrupt (cause 0). */
void riscv_utvec_usi(void) __attribute__ ((interrupt ("user")));

/*! \brief User timer interrupt (cause 4). */
void riscv_utvec_uti(void) __attribute__ ((interrupt ("user")));

/*! \brief User external interrupt (cause 8). */
void riscv_utvec_uei(void) __attribute__ ((interrupt ("user")));

#ifndef VECTOR_TABLE_MTVEC_PLATFORM_INTS

// ----------------------------------------------------------------------------
// Platform interrupts of the machine mode table (cause 16 + N, bit 16 + N of mip/mie)
// ----------------------------------------------------------------------------

/*! \brief Platform interrupt 0 (cause 16). */
void riscv_mtvec_platform_irq0(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 1 (cause 17). */
void riscv_mtvec_platform_irq1(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 2 (cause 18). */
void riscv_mtvec_platform_irq2(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 3 (cause 19). */
void riscv_mtvec_platform_irq3(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 4 (cause 20). */
void riscv_mtvec_platform_irq4(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 5 (cause 21). */
void riscv_mtvec_platform_irq5(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 6 (cause 22). */
void riscv_mtvec_platform_irq6(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 7 (cause 23). */
void riscv_mtvec_platform_irq7(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 8 (cause 24). */
void riscv_mtvec_platform_irq8(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 9 (cause 25). */
void riscv_mtvec_platform_irq9(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 10 (cause 26). */
void riscv_mtvec_platform_irq10(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 11 (cause 27). */
void riscv_mtvec_platform_irq11(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 12 (cause 28). */
void riscv_mtvec_platform_irq12(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 13 (cause 29). */
void riscv_mtvec_platform_irq13(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 14 (cause 30). */
void riscv_mtvec_platform_irq14(void) __attribute__ ((interrupt ("machine")));

/*! \brief Platform interrupt 15 (cause 31). */
void riscv_mtvec_platform_irq15(void) __attribute__ ((interrupt ("machine")));

#endif // VECTOR_TABLE_MTVEC_PLATFORM_INTS

#ifdef __cplusplus
}
#endif

#endif /* STK_RISCV_VECTOR_TABLE_H_ */
