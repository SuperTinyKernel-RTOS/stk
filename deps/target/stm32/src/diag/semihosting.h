/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STM32FX_SEMIHOSTING_H_
#define STK_STM32FX_SEMIHOSTING_H_

/*! \file  semihosting.h
    \brief ARM semihosting call (Cortex-M): requests to the debugger through the \c BKPT instruction.
    \see   https://github.com/ARM-software/abi-aa/blob/main/semihosting/semihosting.rst
*/

#define STK_SEMIHOSTING_SYS_OPEN   (0x01) //!< Open a file, R1 -> {name, mode, name length}.
#define STK_SEMIHOSTING_SYS_WRITEC (0x03) //!< Write a character, R1 -> character.
#define STK_SEMIHOSTING_SYS_WRITE0 (0x04) //!< Write a null-terminated string, R1 -> string.
#define STK_SEMIHOSTING_SYS_WRITE  (0x05) //!< Write to a file, R1 -> {handle, data, length}, returns number of bytes NOT written.

/*! \def   STK_SEMIHOSTING_BKPT_NUMBER
    \brief Immediate of the \c BKPT instruction which stands for a semihosting call.
*/
#define STK_SEMIHOSTING_BKPT_NUMBER (0xAB)

/*! \brief     Call the host (debugger).
    \param[in] reason: Operation, see STK_SEMIHOSTING_SYS_xxx.
    \param[in] arg:    Operation specific parameter.
    \return    Result of the operation (R0).
    \note      Without a debugger the BKPT ends up in the HardFault handler (see exceptions.c).
*/
static inline __attribute__((always_inline)) int stk_semihosting_call(int reason, const void *arg)
{
    register int r0 __asm("r0") = reason;
    register const void *r1 __asm("r1") = arg;

    __asm volatile ("bkpt %[imm]"
                    : "+r" (r0)
                    : "r" (r1), [imm] "i" (STK_SEMIHOSTING_BKPT_NUMBER)
                    : "r2", "r3", "ip", "lr", "memory", "cc");
    return r0;
}

#endif /* STK_STM32FX_SEMIHOSTING_H_ */
