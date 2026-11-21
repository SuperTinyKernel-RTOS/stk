/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "vector_table.h"

/*! \file  vector_table.c
    \brief Trap vector tables and default interrupt handlers of the STK RISC-V target, see vector_table.h.
    \note  Relies on GCC function attributes (interrupt, naked, weak, alias):
           https://gcc.gnu.org/onlinedocs/gcc/RISC-V-Function-Attributes.html
*/

/*! \def   STK_VECTOR_TABLE_ATTR
    \brief Attributes of a vector table function (function suffix).
    \param[in] sect: Name of the section the table is placed to, the linker script puts it along with other code.
    \note  Tables are 256-byte aligned: the low bits of \c mtvec / \c stvec / \c utvec hold the mode.
*/
#define STK_VECTOR_TABLE_ATTR(sect) __attribute__ ((naked, section(sect), aligned(256)))

/*! \def   STK_ISR_ALIAS
    \brief Attributes of a weak interrupt handler aliased to the default one (function suffix).
    \param[in] mode: Interrupt mode ("machine", "supervisor" or "user").
    \param[in] nop:  Name of the default handler to alias.
    \note  A strong definition of the handler in the application overrides the alias.
*/
#define STK_ISR_ALIAS(mode, nop) __attribute__ ((interrupt(mode), weak, used, alias(nop)))

/*! \def   STK_VEC
    \brief Emits one entry of a vector table: a 4-byte jump to \a handler (inline assembly string).
    \param[in] handler: Name of the handler function.
*/
#define STK_VEC(handler) "jal zero, " #handler "\n\t"

// ----------------------------------------------------------------------------
// Default handlers
// ----------------------------------------------------------------------------

/*! \brief Default machine mode handler: does nothing, returns from the trap. */
void riscv_nop_machine(void) __attribute__ ((interrupt ("machine"), used, aligned(4)));

/*! \brief Default supervisor mode handler: does nothing, returns from the trap. */
void riscv_nop_supervisor(void) __attribute__ ((interrupt ("supervisor"), used, aligned(4)));

/*! \brief Default user mode handler: does nothing, returns from the trap. */
void riscv_nop_user(void) __attribute__ ((interrupt ("user"), used, aligned(4)));

void riscv_nop_machine(void)    { /* nothing to do */ }
void riscv_nop_supervisor(void) { /* nothing to do */ }
void riscv_nop_user(void)       { /* nothing to do */ }

// ----------------------------------------------------------------------------
// Weak handlers (aliased to the default ones until overridden by the application)
// ----------------------------------------------------------------------------

void riscv_mtvec_exception(void) STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_msi(void)       STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_mti(void)       STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_mei(void)       STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_ssi(void)       STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_sti(void)       STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_sei(void)       STK_ISR_ALIAS("machine", "riscv_nop_machine");

void riscv_stvec_exception(void) STK_ISR_ALIAS("supervisor", "riscv_nop_supervisor");
void riscv_stvec_ssi(void)       STK_ISR_ALIAS("supervisor", "riscv_nop_supervisor");
void riscv_stvec_sti(void)       STK_ISR_ALIAS("supervisor", "riscv_nop_supervisor");
void riscv_stvec_sei(void)       STK_ISR_ALIAS("supervisor", "riscv_nop_supervisor");

void riscv_utvec_usi(void)       STK_ISR_ALIAS("user", "riscv_nop_user");
void riscv_utvec_uti(void)       STK_ISR_ALIAS("user", "riscv_nop_user");
void riscv_utvec_uei(void)       STK_ISR_ALIAS("user", "riscv_nop_user");

#ifndef VECTOR_TABLE_MTVEC_PLATFORM_INTS

void riscv_mtvec_platform_irq0(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq1(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq2(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq3(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq4(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq5(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq6(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq7(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq8(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq9(void)  STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq10(void) STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq11(void) STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq12(void) STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq13(void) STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq14(void) STK_ISR_ALIAS("machine", "riscv_nop_machine");
void riscv_mtvec_platform_irq15(void) STK_ISR_ALIAS("machine", "riscv_nop_machine");

#endif // VECTOR_TABLE_MTVEC_PLATFORM_INTS

// ----------------------------------------------------------------------------
// Vector tables
// ----------------------------------------------------------------------------

/*! \brief   Machine mode vector table.
    \note    \c mcause: 0 exception, 1/5/9 supervisor software/timer/external, 3/7/11 machine
             software/timer/external, 16+ platform interrupts. Reserved causes jump to the default handler.
    \note    Every entry is exactly one 4-byte "jal zero, <handler>", so the address of an entry is
             \c table + 4 * cause. Compressed instructions and linker relaxation are disabled for the table,
             otherwise the jumps could shrink to 2 bytes and shift the following entries.
    \warning Do not call.
*/
void riscv_mtvec_table(void) STK_VECTOR_TABLE_ATTR(".text.mtvec_table");
void riscv_mtvec_table(void)
{
    __asm__ volatile (
        ".option push\n\t"
        ".option norvc\n\t"
        ".option norelax\n\t"
        STK_VEC(riscv_mtvec_exception)  // 0
        STK_VEC(riscv_mtvec_ssi)        // 1
        STK_VEC(riscv_nop_machine)      // 2 reserved
        STK_VEC(riscv_mtvec_msi)        // 3
        STK_VEC(riscv_nop_machine)      // 4 reserved
        STK_VEC(riscv_mtvec_sti)        // 5
        STK_VEC(riscv_nop_machine)      // 6 reserved
        STK_VEC(riscv_mtvec_mti)        // 7
        STK_VEC(riscv_nop_machine)      // 8 reserved
        STK_VEC(riscv_mtvec_sei)        // 9
        STK_VEC(riscv_nop_machine)      // 10 reserved
        STK_VEC(riscv_mtvec_mei)        // 11
        STK_VEC(riscv_nop_machine)      // 12 reserved
        STK_VEC(riscv_nop_machine)      // 13 reserved
        STK_VEC(riscv_nop_machine)      // 14 reserved
        STK_VEC(riscv_nop_machine)      // 15 reserved
#ifndef VECTOR_TABLE_MTVEC_PLATFORM_INTS
        STK_VEC(riscv_mtvec_platform_irq0)   // 16
        STK_VEC(riscv_mtvec_platform_irq1)   // 17
        STK_VEC(riscv_mtvec_platform_irq2)   // 18
        STK_VEC(riscv_mtvec_platform_irq3)   // 19
        STK_VEC(riscv_mtvec_platform_irq4)   // 20
        STK_VEC(riscv_mtvec_platform_irq5)   // 21
        STK_VEC(riscv_mtvec_platform_irq6)   // 22
        STK_VEC(riscv_mtvec_platform_irq7)   // 23
        STK_VEC(riscv_mtvec_platform_irq8)   // 24
        STK_VEC(riscv_mtvec_platform_irq9)   // 25
        STK_VEC(riscv_mtvec_platform_irq10)  // 26
        STK_VEC(riscv_mtvec_platform_irq11)  // 27
        STK_VEC(riscv_mtvec_platform_irq12)  // 28
        STK_VEC(riscv_mtvec_platform_irq13)  // 29
        STK_VEC(riscv_mtvec_platform_irq14)  // 30
        STK_VEC(riscv_mtvec_platform_irq15)  // 31
#endif
        ".option pop\n\t"
    );
}

/*! \brief   Supervisor mode vector table.
    \note    \c scause: 0 exception, 1 software, 5 timer, 9 external. Reserved causes jump to the default handler.
    \note    Entries are 4-byte jumps, see riscv_mtvec_table.
    \warning Do not call.
*/
void riscv_stvec_table(void) STK_VECTOR_TABLE_ATTR(".text.stvec_table");
void riscv_stvec_table(void)
{
    __asm__ volatile (
        ".option push\n\t"
        ".option norvc\n\t"
        ".option norelax\n\t"
        STK_VEC(riscv_stvec_exception)  // 0
        STK_VEC(riscv_stvec_ssi)        // 1
        STK_VEC(riscv_nop_supervisor)   // 2 reserved
        STK_VEC(riscv_nop_supervisor)   // 3 reserved
        STK_VEC(riscv_nop_supervisor)   // 4 reserved
        STK_VEC(riscv_stvec_sti)        // 5
        STK_VEC(riscv_nop_supervisor)   // 6 reserved
        STK_VEC(riscv_nop_supervisor)   // 7 reserved
        STK_VEC(riscv_nop_supervisor)   // 8 reserved
        STK_VEC(riscv_stvec_sei)        // 9
        ".option pop\n\t"
    );
}

/*! \brief   User mode vector table.
    \note    \c ucause: 0 software, 4 timer, 8 external. Reserved causes jump to the default handler.
    \note    Entries are 4-byte jumps, see riscv_mtvec_table.
    \warning Do not call.
*/
void riscv_utvec_table(void) STK_VECTOR_TABLE_ATTR(".text.utvec_table");
void riscv_utvec_table(void)
{
    __asm__ volatile (
        ".option push\n\t"
        ".option norvc\n\t"
        ".option norelax\n\t"
        STK_VEC(riscv_utvec_usi)        // 0
        STK_VEC(riscv_nop_user)         // 1 reserved
        STK_VEC(riscv_nop_user)         // 2 reserved
        STK_VEC(riscv_nop_user)         // 3 reserved
        STK_VEC(riscv_utvec_uti)        // 4
        STK_VEC(riscv_nop_user)         // 5 reserved
        STK_VEC(riscv_nop_user)         // 6 reserved
        STK_VEC(riscv_nop_user)         // 7 reserved
        STK_VEC(riscv_utvec_uei)        // 8
        ".option pop\n\t"
    );
}
