/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_ARM_CORTEX_M_ASM_H_
#define STK_ARCH_ARM_CORTEX_M_ASM_H_

#include "stk_config.h"

// Inline ASM helpers:
#ifdef __ICCARM__
    #define STK_ASM_SYNTAX_UNIFIED /* IAR: not needed, unified is default */
    #define STK_ASM_POOL           /* IAR: not needed, handled automatically */
    #define STK_ASM_ALIGN_2        /* IAR: not needed */
#else
    #define STK_ASM_SYNTAX_UNIFIED ".syntax unified  \n"
    #define STK_ASM_POOL           ".pool            \n"
    #define STK_ASM_ALIGN_2        ".align 2         \n"
#endif

/*! \def       STK_ASM_GLOBAL_SYMBOL
    \brief     Generates a toolchain-agnostic assembly directive to declare a global symbol.
    \param[in] name: Symbol identifier to export globally.
*/
#if defined(__ICCARM__)
    // IAR Assembler uses PUBLIC instead of .global
    #define STK_ASM_GLOBAL_SYMBOL(name) "PUBLIC " #name  "\n"
#else
    // GCC, Clang, and Keil armclang (ARMCC v6+)
    #define STK_ASM_GLOBAL_SYMBOL(name) ".global " #name "\n"
#endif

#ifdef _STK_CORTEX_M_TRUSTZONE
/* ARMv8-M TrustZone (Cortex-M33 / Mainline)
   EXC_RETURN bit layout (ARMv8-M ARM B1.5.8):
     bit 6 (0x40): 1 = Secure stack, 0 = Non-Secure stack.  <-- S-bit
     bit 2 (0x04): 1 = PSP,          0 = MSP.
   We test bit 6 first to select the correct world's stack pointer,
   then bit 2 to choose MSP vs PSP within that world. */
#if (__CORTEX_M >= 33U)
/* -----------------------------------------------------------------
   Cortex-M33 and later (ARMv8-M Mainline) -- IT/ITE available.
   ----------------------------------------------------------------- */
#define STK_ASM_EXTRACT_STACK_POINTER_TO_R0 \
    "TST    LR, #4              \n" /* bit 2: 0 = MSP, 1 = PSP */ \
    "BNE    1f                  \n" \
    /* --- MSP branch --- */ \
    "TST    LR, #64             \n" /* bit 6 (S-bit): 1 = Secure, 0 = Non-Secure */ \
    "ITE    NE                  \n" \
    "MRSNE  r0, MSP             \n" /* r0 = Secure MSP */ \
    "MRSEQ  r0, MSP_NS          \n" /* r0 = Non-Secure MSP */ \
    "B      2f                  \n" \
    "1:                         \n" \
    "TST    LR, #64             \n" /* bit 6 (S-bit): 1 = Secure, 0 = Non-Secure */ \
    "ITE    NE                  \n" \
    "MRSNE  r0, PSP             \n" /* r0 = Secure PSP */ \
    "MRSEQ  r0, PSP_NS          \n" /* r0 = Non-Secure PSP */ \
    "2:                         \n"
#else
/* -----------------------------------------------------------------
   Cortex-M23 (ARMv8-M Baseline) -- no IT instructions, limited ISA.
   Shift bits into the sign position and use BMI (Branch if Minus).
     bit 2 -> sign: LSLS r1, #29  (32 - 3 = 29)
     bit 6 -> sign: LSLS r1, #25  (32 - 7 = 25)
   ----------------------------------------------------------------- */
#define STK_ASM_EXTRACT_STACK_POINTER_TO_R0 \
    "MOV    r1, LR              \n" \
    "LSLS   r1, r1, #29         \n" /* shift bit 2 into sign */ \
    "BMI    3f                  \n" /* bit 2 set  -> PSP branch */ \
    /* --- MSP branch --- */ \
    "MOV    r1, LR              \n" \
    "LSLS   r1, r1, #25         \n" /* shift bit 6 (S-bit) into sign */ \
    "BMI    4f                  \n" /* S=1 -> Secure MSP */ \
    "MRS    r0, MSP_NS          \n" /* S=0 -> Non-Secure MSP */ \
    "B      2f                  \n" \
    "4:                         \n" \
    "MRS    r0, MSP             \n" /* r0 = Secure MSP */ \
    "B      2f                  \n" \
    "3:                         \n" \
    "MOV    r1, LR              \n" \
    "LSLS   r1, r1, #25         \n" /* shift bit 6 (S-bit) into sign */ \
    "BMI    5f                  \n" /* S=1 -> Secure PSP */ \
    "MRS    r0, PSP_NS          \n" /* S=0 -> Non-Secure PSP */ \
    "B      2f                  \n" \
    "5:                         \n" \
    "MRS    r0, PSP             \n" /* r0 = Secure PSP */ \
    "2:                         \n"
#endif
#elif (__CORTEX_M >= 3U)
/* Cortex-M3/M4/M7 */
#define STK_ASM_EXTRACT_STACK_POINTER_TO_R0 \
    "TST    LR, #4              \n" /* check EXC_RETURN bit 2 */ \
    "ITE    EQ                  \n" \
    "MRSEQ  r0, MSP             \n" /* r0 = MSP */ \
    "MRSNE  r0, PSP             \n" /* else r0 = PSP */
#else
/* Cortex-M0/M0+ (limited ISA) */
#define STK_ASM_EXTRACT_STACK_POINTER_TO_R0 \
    "MOV    r0, LR              \n" /* r0 = LR */ \
    "LSLS   r0, r0, #29         \n" /* if (r0 & 4) */ \
    "BMI    6f                  \n" /* else */ \
    "MRS    r0, MSP             \n" /* r0 = MSP */ \
    "B      7f                  \n" \
    "6:                         \n" \
    "MRS    r0, PSP             \n" /* else r0 = PSP */ \
    "7:                         \n"
#endif

#endif /* STK_ARCH_ARM_CORTEX_M_H_ */
