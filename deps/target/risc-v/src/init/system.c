/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>

/*! \file  system.c
    \brief Minimal system calls of the STK RISC-V target (newlib/libnosys stubs): abort, assert and sbrk.
    \see   stk.ld (\c __heap_start, \c __heap_end)
*/

/*! \brief Abort the program: busy loop with the CPU in idle state. */
void abort(void) __attribute__((noreturn));

/*! \brief     Assertion failure handler of the C library: prints the reason and aborts.
    \param[in] file: Source file name.
    \param[in] line: Source line.
    \param[in] func: Function name.
    \param[in] expr: Failed expression.
*/
extern void __assert_func(const char *file, int line, const char *func, const char *expr) __attribute__((noreturn));

/*! \brief     Allocate memory from the heap (C library backend of malloc).
    \param[in] incr: Number of bytes to add to the heap, rounded up to the size of \c size_t.
    \return    Previous end of the heap on success, \c (caddr_t)-1 with \c errno set to \c ENOMEM if the
                heap, which is limited by \c __heap_end of the linker script, is exhausted.
*/
extern caddr_t _sbrk(int incr) __attribute__ ((used));

void abort(void)
{
    for (;;)
    {
        __asm__ volatile(
        "wfi                \n"
        "nop                \n");
    }
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    printf("assert failed:\n - file: %s\n - line: %d\n - function: %s\n - what: %s\n", file, line, func, expr);
    abort();
}

caddr_t _sbrk(int incr)
{
    extern char __heap_start;
    extern char __heap_end;

    static char *heap_pos = &__heap_start;

    char *ret = heap_pos;
    const char *end = &__heap_end;

    // align the increment
    incr = (incr + (sizeof(size_t) - 1)) & (~(sizeof(size_t) - 1));

    if ((heap_pos + incr) > end)
    {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    heap_pos += incr;
    return (caddr_t)ret;
}
