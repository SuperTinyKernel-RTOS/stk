/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdint.h>
#include <string.h>

#include <risc-v/encoding.h>
#include "vector_table.h"

/*! \file  startup.c
    \brief C/C++ runtime startup (CRT) of the STK RISC-V target, single hart.

    Entry point \c _enter is placed by the linker script (stk.ld or stk-mpu.ld) at the reset address. It sets
    up \c sp and \c gp and jumps to \c _start, which initializes the memory regions (.bss, .data, .itim and,
    with STK_MPU, the MPU regions), runs the static constructors, programs the trap vector table and calls
    \c main().
    \note  \c tp (thread pointer) is not initialized.
    \see   stk.ld, stk-mpu.ld, vector_table.h
*/

/*! \def   RISCV_MTVEC_MODE_VECTORED
    \brief Value of the mode bits of \c mtvec which selects vectored interrupts.
*/
#define RISCV_MTVEC_MODE_VECTORED 1

/*! \brief Object handle for the registration of static destructors (required by C++).
*/
void *__dso_handle __attribute__ ((weak));

/*! \typedef function_t
    \brief   Generic C function pointer (constructor, destructor).
*/
typedef void(*function_t)(void);

/*! \name  Symbols defined by the linker script.
    \see   stk.ld, stk-mpu.ld
*/
extern uint8_t __stk_bss_start;       //!< Start of .bss.
extern uint8_t __stk_bss_end;         //!< End of .bss.
extern const uint8_t __stk_data_load; //!< Load address (ROM) of the .data image.
extern uint8_t __stk_data_start;      //!< Start of .data (RAM).
extern uint8_t __stk_data_end;        //!< End of .data.
extern const uint8_t __stk_itim_load; //!< Load address (ROM) of the .itim image.
extern uint8_t __stk_itim_start;      //!< Start of .itim (ITIM).
extern uint8_t __stk_itim_end;        //!< End of .itim.

#if STK_MPU
/*! \name  MPU (PMP) regions, defined by stk-mpu.ld.
    \note  Each region is a window made of data (initialized from ROM) followed by bss (zeroed),
           the bss ends at the end of the window.
    \see   STK_MPU_SHARED_DATA_SECTION, STK_MPU_KERNEL_DATA_SECTION
*/
extern const uint8_t __stk_mpu_shared_data_source__; //!< Load address (ROM) of the shared data image.
extern uint8_t __stk_mpu_shared_data_start;          //!< Start of the shared window (RAM).
extern uint8_t __stk_mpu_shared_data_end;            //!< End of the shared data.
extern uint8_t __stk_mpu_shared_bss_start;           //!< Start of the shared bss.
extern uint8_t __stk_mpu_shared_bss_end;             //!< End of the shared window.
extern const uint8_t __stk_mpu_kernel_data_source__; //!< Load address (ROM) of the kernel data image.
extern uint8_t __stk_mpu_kernel_data_start;          //!< Start of the kernel window (RAM).
extern uint8_t __stk_mpu_kernel_data_end;            //!< End of the kernel data.
extern uint8_t __stk_mpu_kernel_bss_start;           //!< Start of the kernel bss.
extern uint8_t __stk_mpu_kernel_bss_end;             //!< End of the kernel window.
#endif

/*! \name  Constructor/destructor tables, defined by the linker script.
*/
extern function_t __preinit_array_start[]; //!< Start of the pre-init array.
extern function_t __preinit_array_end[];   //!< End of the pre-init array.
extern function_t __init_array_start[];    //!< Start of the init array (static constructors).
extern function_t __init_array_end[];      //!< End of the init array.
extern function_t __fini_array_start[];    //!< Start of the fini array (static destructors).
extern function_t __fini_array_end[];      //!< End of the fini array.

/*! \brief   Reset entry point.
    \note    Placed by the linker script at the reset address, see section \c .text.stk.init.enter.
    \note    Raw function "called" by the CPU with no stack and no C runtime configured.
    \warning Single hart only.
*/
extern void _enter(void) __attribute__ ((naked, section(".text.stk.init.enter")));

/*! \brief Entry point as a C function. Never returns. */
extern void _start(void) __attribute__ ((noreturn, used));

/*! \brief     Terminate the program: busy loop with the CPU in idle state.
    \param[in] exit_code: Exit code (ignored).
    \note      Should never be called in a normal flow.
*/
void _Exit(int exit_code) __attribute__ ((noreturn, noinline));

/*! \brief     Initialize arguments of main().
    \param[out] argc: Number of arguments.
    \param[out] argv: Arguments.
    \note      Weak, can be overridden by the application.
*/
extern void _init_args(int *argc, char ***argv) __attribute__ ((weak));

/*! \brief     Application entry point.
    \param[in] argc: Number of arguments.
    \param[in] argv: Arguments.
    \return    Exit code.
*/
extern int main(int argc, char *argv[]);

/*! \brief Initialize C library (constructors of the libc). */
extern void __libc_init_array(void);

void _init_args(int *argc, char ***argv)
{
    static char _name[] = "";
    static char *_argv[2] = { _name, NULL };

    (*argv) = &_argv[0];
    (*argc) = 1;
}

/*! \brief Run the pre-init and init arrays (static constructors).
    \note  The \c .init section is not executed: it requires the prologue/epilogue from crti.o and crtn.o,
           use the startup files of the toolchain if the code of the \c .init section is needed.
           Do not enable the call of _init() here.
*/
inline void __attribute__((always_inline)) __run_init_array(void)
{
    int count;
    int i;

    count = __preinit_array_end - __preinit_array_start;
    for (i = 0; i < count; i++)
        __preinit_array_start[i]();

    count = __init_array_end - __init_array_start;
    for (i = 0; i < count; i++)
        __init_array_start[i]();
}

/*! \brief Run all cleanup routines (mainly static destructors).
    \note  The \c .fini section is not executed, see __run_init_array. Do not enable the call of _fini() here.
*/
inline void __attribute__((always_inline)) __run_fini_array(void)
{
    int count;
    int i;

    count = __fini_array_end - __fini_array_start;
    for (i = count; i > 0; i--)
        __fini_array_start[i - 1]();
}

void _enter(void)
{
    // set up sp and gp, locations are defined in the linker script
    __asm__ volatile (
        ".option push;"
        // 'norelax' is critical here: without it the global pointer would be loaded relative to the global pointer
        ".option norelax;"
        "la    gp, __global_pointer$;"
        ".option pop;"
        "la    sp, _sp;"
        "jal   zero, _start;"
        :  /* output: none %0 */
        :  /* input: none */
        :  /* clobbers: none */);

    // this point is not reached, _start() is jumped to with no return
}

/*! \note At this point there is a stack and a global pointer, but no access to global variables yet. */
void _start(void)
{
    // clear .bss (global variables with no initial values)
    memset((void*) &__stk_bss_start,
           0,
           (&__stk_bss_end - &__stk_bss_start));

    // initialize .data (global variables with initial values)
    memcpy((void*)&__stk_data_start,
           (const void*)&__stk_data_load,
           (&__stk_data_end - &__stk_data_start));

    // initialize .itim (code moved from flash to SRAM to improve performance)
    memcpy((void*)&__stk_itim_start,
           (const void*)&__stk_itim_load,
           (&__stk_itim_end - &__stk_itim_start));

#if STK_MPU
    // initialize MPU regions: the .data part is loaded from ROM, the .bss part is cleared
    // (including the unused tail of the window), see STK_MPU_xxx_SECTION macros
    memcpy((void*)&__stk_mpu_shared_data_start,
           (const void*)&__stk_mpu_shared_data_source__,
           (&__stk_mpu_shared_data_end - &__stk_mpu_shared_data_start));
    memset((void*)&__stk_mpu_shared_bss_start,
           0,
           (&__stk_mpu_shared_bss_end - &__stk_mpu_shared_bss_start));
    memcpy((void*)&__stk_mpu_kernel_data_start,
           (const void*)&__stk_mpu_kernel_data_source__,
           (&__stk_mpu_kernel_data_end - &__stk_mpu_kernel_data_start));
    memset((void*)&__stk_mpu_kernel_bss_start,
           0,
           (&__stk_mpu_kernel_bss_end - &__stk_mpu_kernel_bss_start));
#endif

    // call C++ constructors of static instances
    __run_init_array();
    __libc_init_array();

    // set the table with ISR handlers
    write_csr(mtvec, (size_t)riscv_mtvec_table | RISCV_MTVEC_MODE_VECTORED);

    // enable Floating Point (FP)
#if (__riscv_flen != 0)
    __asm volatile(
    "li t0, %0        \n"
    "csrs mstatus, t0 \n"
    ::
    "i" (MSTATUS_FS | MSTATUS_XS));
#endif

    // execute
    {
        int rc, argc;
        char **argv;

        _init_args(&argc, &argv);

        rc = main(argc, argv);

        // call C++ destructors
        __run_fini_array();

        _Exit(rc);
    }
}

void _Exit(int exit_code)
{
    (void)exit_code;

    // halt
    while (1)
    {
        __asm__ volatile ("wfi");
    }
}
