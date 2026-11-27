/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdint.h>

#include "exceptions.h"

/*! \file  startup.c
    \brief C/C++ runtime startup (CRT) of the STK STM32Fx target (Cortex-M0/M3/M4, single core).

    The reset vector points to \c Reset_Handler, which sets up the main stack and jumps to \c _start. The latter:
     1. disables interrupts (the kernel or the application enables them, see below);
     2. calls \c __initialize_hardware_early() (system initialization of the device, no RAM is usable yet);
     3. initializes \c .data (copy from flash), \c .bss (zero) and the same sections of CCM RAM (F4) and, with
        STK_MPU, of the MPU regions (sections-mpu.ld);
     4. calls \c __initialize_hardware() (HAL, clocks, now RAM is usable);
     5. calls \c __initialize_args(), then runs the \c .preinit_array / \c .init_array (static C++ constructors);
     6. enables interrupts and calls \c main();
     7. runs the \c .fini_array (static destructors) and calls \c _exit().
    The linker script (sections.ld) defines the symbols used here.
    \note  The \c .init / \c .fini sections are not executed: they need the crti.o / crtn.o of the toolchain.
    \see   hardware.c, sections.ld, exceptions.h
*/

/*! \brief Object handle for the registration of static destructors (required by C++). */
void *__dso_handle __attribute__ ((weak));

/*! \typedef function_t
    \brief   Generic C function pointer (constructor, destructor).
*/
typedef void (*function_t)(void);

/*! \name  Symbols defined by the linker script.
    \see   sections.ld
*/
///@{
extern uint32_t _sidata;     //!< Load address (flash) of the .data image.
extern uint32_t _sdata;      //!< Start of .data (RAM).
extern uint32_t _edata;      //!< End of .data.
extern uint32_t _sbss;       //!< Start of .bss.
extern uint32_t _ebss;       //!< End of .bss.
extern uint32_t _sidata_ccm; //!< Load address (flash) of the .data image of CCM RAM.
extern uint32_t _sdata_ccm;  //!< Start of .data of CCM RAM (empty if the device has no CCM RAM).
extern uint32_t _edata_ccm;  //!< End of .data of CCM RAM.
extern uint32_t _sbss_ccm;   //!< Start of .bss of CCM RAM.
extern uint32_t _ebss_ccm;   //!< End of .bss of CCM RAM.

#if STK_MPU
/*! \name  MPU regions, defined by sections-mpu.ld: initialized data (copied from flash) followed by bss (zeroed).
    \see   STK_MPU_SHARED_DATA_SECTION, STK_MPU_KERNEL_DATA_SECTION
*/
extern uint32_t __stk_mpu_shared_data_load;    //!< Load address (flash) of the shared data image.
extern uint32_t __stk_mpu_shared_data_start;   //!< Start of the shared window (RAM).
extern uint32_t __stk_mpu_shared_data_raw_end; //!< End of the shared data.
extern uint32_t __stk_mpu_shared_bss_start;    //!< Start of the shared bss.
extern uint32_t __stk_mpu_shared_bss_end;      //!< End of the shared bss.
extern uint32_t __stk_mpu_kernel_data_load;    //!< Load address (flash) of the kernel data image.
extern uint32_t __stk_mpu_kernel_data_start;   //!< Start of the kernel window (RAM).
extern uint32_t __stk_mpu_kernel_data_raw_end; //!< End of the kernel data.
extern uint32_t __stk_mpu_kernel_bss_start;    //!< Start of the kernel bss.
extern uint32_t __stk_mpu_kernel_bss_end;      //!< End of the kernel bss.
#endif

extern function_t __preinit_array_start[]; //!< Start of the pre-init array.
extern function_t __preinit_array_end[];   //!< End of the pre-init array.
extern function_t __init_array_start[];    //!< Start of the init array (static constructors).
extern function_t __init_array_end[];      //!< End of the init array.
extern function_t __fini_array_start[];    //!< Start of the fini array (static destructors).
extern function_t __fini_array_end[];      //!< End of the fini array.
///@}

/*! \brief Entry point as a C function, never returns.
    \note  Weak, an application can provide its own start-up sequence.
    \note  Used, must survive LTO optimization.
*/
void _start(void) __attribute__ ((noreturn, used, weak));

/*! \brief Newlib syscall stubs (src/libc/syscalls.c), forced into the link from this always-linked object.
    \note  The stubs are weak and referenced only by libc (_close_r, _fstat_r, ...), which is searched after
           this library on the link line. When the library is linked as a plain static archive (no
           --whole-archive), syscalls.o would never be extracted and libc's references stay undefined.
           A strong reference from here (the object holding the entry point is always extracted) pulls
           syscalls.o in; being weak, the application can still override any of the stubs.
*/
extern int _close(int);
extern int _fstat(int, void *);
extern int _isatty(int);
extern int _lseek(int, int, int);
extern int _open(const char *, int, int);
extern int _read(int, char *, int);
extern int _write(int, char *, int);
extern int _getpid(void);
extern int _kill(int, int);
static const void *const s_syscalls_keep[] __attribute__ ((used, section(".rodata.stk_keep"))) =
{
    (const void *)_close, (const void *)_fstat, (const void *)_isatty, (const void *)_lseek,
    (const void *)_open,  (const void *)_read,  (const void *)_write,  (const void *)_getpid,
    (const void *)_kill
};

/*! \brief     Early hardware initialization, called first, before RAM is initialized.
    \note      Must not use global variables.
*/
extern void __initialize_hardware_early(void);

/*! \brief Hardware initialization, called after RAM is initialized and before the static constructors. */
extern void __initialize_hardware(void);

/*! \brief      Initialize the arguments of main().
    \param[out] argc: Number of arguments.
    \param[out] argv: Arguments.
    \note       Weak, the default is a single empty argument.
*/
extern void __initialize_args(int *argc, char ***argv);

/*! \brief     Application entry point.
    \param[in] argc: Number of arguments.
    \param[in] argv: Arguments.
    \return    Exit code.
*/
extern int main(int argc, char *argv[]);

/*! \brief     Terminate the program, see system.c.
    \param[in] code: Exit code.
*/
extern void _exit(int code) __attribute__ ((weak, used, noreturn));

void __initialize_args(int *argc, char ***argv) __attribute__ ((weak));

void __initialize_args(int *argc, char ***argv)
{
    // strings of argv are modifiable and argv ends with a null pointer
    static char name[] = "";
    static char *args[2] = { name, NULL };

    *argv = &args[0];
    *argc = 1;
}

/*! \brief     Copy an initialized data region from flash to RAM.
    \param[in] from:  Load address (flash).
    \param[in] begin: Start of the region (RAM).
    \param[in] end:   End of the region.
    \note      Regions are word aligned by the linker script.
*/
static inline __attribute__((always_inline)) void copy_region(const uint32_t *from, uint32_t *begin, const uint32_t *end)
{
    while (begin < end)
        *begin++ = *from++;
}

/*! \brief     Zero a region of RAM.
    \param[in] begin: Start of the region.
    \param[in] end:   End of the region.
*/
static inline __attribute__((always_inline)) void zero_region(uint32_t *begin, const uint32_t *end)
{
    while (begin < end)
        *begin++ = 0U;
}

/*! \brief Run the pre-init and init arrays (static constructors). */
static void run_init_array(void)
{
    for (function_t *f = __preinit_array_start; f < __preinit_array_end; ++f)
        (*f)();

    for (function_t *f = __init_array_start; f < __init_array_end; ++f)
        (*f)();
}

/*! \brief Run the fini array (static destructors) in the reverse order. */
static void run_fini_array(void)
{
    for (function_t *f = __fini_array_end; f > __fini_array_start; --f)
        (*(f - 1))();
}

/*! \brief Needed by the C++ runtime if exceptions are used, see \c .fini note above. */
void _fini(void) __attribute__ ((weak));

void _fini(void)
{
    // nothing to do
}

void Reset_Handler(void) __attribute__ ((naked, weak));

void Reset_Handler(void)
{
    // the core loads MSP from the vector table, set it again for the case when this entry is reached otherwise
    // (e.g. a jump from a bootloader); no C code runs before the stack is valid
    __asm volatile (
        " ldr  r0, =_estack \n"
        " msr  msp, r0      \n"
        " ldr  r0, =_start  \n"
        " bx   r0           \n");
}

void _start(void)
{
    int argc;
    char **argv;

    __asm volatile ("cpsid i" ::: "memory");

    __initialize_hardware_early();

    copy_region(&_sidata, &_sdata, &_edata);
    copy_region(&_sidata_ccm, &_sdata_ccm, &_edata_ccm);
    zero_region(&_sbss, &_ebss);
    zero_region(&_sbss_ccm, &_ebss_ccm);

#if STK_MPU
    copy_region(&__stk_mpu_shared_data_load, &__stk_mpu_shared_data_start, &__stk_mpu_shared_data_raw_end);
    zero_region(&__stk_mpu_shared_bss_start, &__stk_mpu_shared_bss_end);
    copy_region(&__stk_mpu_kernel_data_load, &__stk_mpu_kernel_data_start, &__stk_mpu_kernel_data_raw_end);
    zero_region(&__stk_mpu_kernel_bss_start, &__stk_mpu_kernel_bss_end);
#endif

    __initialize_hardware();

    __initialize_args(&argc, &argv);

    run_init_array();

    __asm volatile ("cpsie i" ::: "memory");

    {
        const int rc = main(argc, argv);

        run_fini_array();

        _exit(rc);
    }
}
