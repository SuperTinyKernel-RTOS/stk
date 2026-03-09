/*
   Simple C++ startup routine to setup CRT
   SPDX-License-Identifier: Unlicense

   (https://five-embeddev.com/ | http://www.shincbm.com/) 

*/

#include <stdint.h>
#include <string.h>

#include <risc-v/encoding.h>
#include "vector_table.h"

#define RISCV_MTVEC_MODE_VECTORED 1

// Required for C++ destructors.
void *__dso_handle __attribute__ ((weak));

// Generic C function pointer.
typedef void(*function_t)(void) ;

// These symbols are defined by the linker script.
// See metal.ld
extern uint8_t metal_segment_bss_target_start;
extern uint8_t metal_segment_bss_target_end;
extern const uint8_t metal_segment_data_source_start;
extern uint8_t metal_segment_data_target_start;
extern uint8_t metal_segment_data_target_end;
extern const uint8_t metal_segment_itim_source_start;
extern uint8_t metal_segment_itim_target_start;
extern uint8_t metal_segment_itim_target_end;

extern function_t __preinit_array_start[];
extern function_t __preinit_array_end[];
extern function_t __init_array_start[];
extern function_t __init_array_end[];
extern function_t __fini_array_start[];
extern function_t __fini_array_end[];

// This function will be placed by the linker script according to the section
// Raw function 'called' by the CPU with no runtime.
extern void _enter(void)  __attribute__ ((naked, section(".text.metal.init.enter")));

// Entry and exit points as C functions.
extern void _start(void) __attribute__ ((noreturn, used));
void _Exit(int exit_code) __attribute__ ((noreturn, noinline));
extern void _init_args(int *argc, char ***argv) __attribute__ ((weak));
extern int main(int argc, char *argv[]);
extern void __libc_init_array(void);

// Initialize args.
void _init_args(int *argc, char ***argv)
{
    static char _name[] = "";
    static char *_argv[2] = { _name, NULL };

    (*argv) = &_argv[0];
    (*argc) = 1;
}

inline void __attribute__((always_inline)) __run_init_array (void) {
  int count;
  int i;

  count = __preinit_array_end - __preinit_array_start;
  for (i = 0; i < count; i++)
    __preinit_array_start[i] ();

  // If you need to run the code in the .init section, please use
  // the startup files, since this requires the code in crti.o and crtn.o
  // to add the function prologue/epilogue.
  //_init(); // DO NOT ENABE THIS!

  count = __init_array_end - __init_array_start;
  for (i = 0; i < count; i++)
    __init_array_start[i] ();
}

// Run all the cleanup routines (mainly static destructors).
inline void __attribute__((always_inline)) __run_fini_array (void) {
  int count;
  int i;

  count = __fini_array_end - __fini_array_start;
  for (i = count; i > 0; i--)
    __fini_array_start[i - 1] ();

  // If you need to run the code in the .fini section, please use
  // the startup files, since this requires the code in crti.o and crtn.o
  // to add the function prologue/epilogue.
  //_fini(); // DO NOT ENABE THIS!
}

// The linker script will place this in the reset entry point.
// It will be 'called' with no stack or C runtime configuration.
// NOTE - this only supports a single hart.
// tp will not be initialized
void _enter(void) {
    // Setup SP and GP
    // The locations are defined in the linker script
    __asm__ volatile  (
        ".option push;"
        // The 'norelax' option is critical here.
        // Without 'norelax' the global pointer will
        // be loaded relative to the global pointer!
         ".option norelax;"
        "la    gp, __global_pointer$;"
        ".option pop;"
        "la    sp, _sp;"
        "jal   zero, _start;"
        :  /* output: none %0 */
        : /* input: none */
        : /* clobbers: none */); 
    // This point will not be executed, _start() will be called with no return.
}

// At this point we have a stack and global poiner, but no access to global variables.
void _start(void) {

    // Init memory regions
    // Clear the .bss section (global variables with no initial values)
    memset((void*) &metal_segment_bss_target_start,
           0, 
           (&metal_segment_bss_target_end - &metal_segment_bss_target_start));

    // Initialize the .data section (global variables with initial values)
    memcpy((void*)&metal_segment_data_target_start,
           (const void*)&metal_segment_data_source_start,
           (&metal_segment_data_target_end - &metal_segment_data_target_start));

    // Initialize the .itim section (code moved from flash to SRAM to improve performance)
    memcpy((void*)&metal_segment_itim_target_start,
           (const void*)&metal_segment_itim_source_start,
           (&metal_segment_itim_target_end - &metal_segment_itim_target_start));

    // Call C++ constructors of static instances
    __run_init_array();
    __libc_init_array();

    // Set table with ISR handlers
    write_csr(mtvec, (size_t)riscv_mtvec_table | RISCV_MTVEC_MODE_VECTORED);

    // Enable Floating Point (FP)
#if (__riscv_flen != 0)
    __asm volatile(
    "li t0, %0        \n"
    "csrs mstatus, t0 \n"
    ::
    "i" (MSTATUS_FS | MSTATUS_XS));
#endif

    // Executable
    {
        int rc, argc;
        char **argv;

        // Init args
        _init_args(&argc, &argv);

        // Execute
        rc = main(argc, argv);

        // Call C++ destructors
        __run_fini_array();

        _Exit(rc);
    }
}

// This should never be called. Busy loop with the CPU in idle state.
void _Exit(int exit_code) {
    (void)exit_code;
    // Halt
    while (1) {
        __asm__ volatile ("wfi");
    }
}
