/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "cmsis_device.h"
#include "diag/trace.h"

/*! \file  system.c
    \brief System level C library back-end of the STK STM32Fx target (newlib): heap, exit, abort and assertions.
    \see   sections.ld (\c __heap_start, \c __heap_end)
*/

/*! \brief   Linker symbols of the heap: it spans the RAM between .bss/.noinit and the main stack. */
extern uint8_t __heap_start;
extern uint8_t __heap_end;

/*! \brief     Allocate memory from the heap (back-end of malloc).
    \param[in] incr: Number of bytes to add to the heap, rounded up to a multiple of 8.
    \return    Previous end of the heap, or <tt>(void *)-1</tt> with \c errno set to \c ENOMEM if the heap is
               exhausted (this is not fatal, malloc then returns NULL).
*/
void *_sbrk(int incr) __attribute__ ((used));

void *_sbrk(int incr)
{
    static uint8_t *brk = NULL;
    uint8_t *prev;

    if (brk == NULL)
        brk = &__heap_start;

    prev = brk;

    if (incr >= 0)
    {
        // the break stays 8-byte aligned (word access on Cortex-M0 must be aligned, malloc wants 8)
        const size_t size = ((size_t)incr + 7U) & ~(size_t)7U;

        if (size > (size_t)(&__heap_end - brk))
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        brk += size;
    }
    else
    {
        const size_t size = ((size_t)(-incr) + 7U) & ~(size_t)7U;

        if (size > (size_t)(brk - &__heap_start))
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        brk -= size;
    }

    return prev;
}

/*! \brief Reset the device, the default of __reset_hardware(). */
void __reset_hardware(void) __attribute__ ((weak, noreturn));

void __reset_hardware(void)
{
    NVIC_SystemReset();
}

/*! \brief     End the program: a release build resets the device, a DEBUG build parks the CPU where the debugger
               can see it.
    \param[in] code: Exit code (ignored).
    \note      Weak, can be overridden by the application.
    \note      Used, must survive LTO optimization.
*/
void _exit(int code) __attribute__ ((weak, used, noreturn));

void _exit(int code)
{
    (void)code;

#if !defined(DEBUG)
    __reset_hardware();
#endif

    for (;;)
    {
        // nothing to do
    }
}

void abort(void) __attribute__ ((weak, used, noreturn));

void abort(void)
{
    trace_puts("abort(), exiting...");
    _exit(1);
}

/*! \brief     Failed assert() of the C library: prints the reason and aborts.
    \param[in] file:       Source file name.
    \param[in] line:       Source line.
    \param[in] func:       Function name, may be NULL.
    \param[in] failedexpr: Failed expression.
*/
void __assert_func(const char *file, int line, const char *func, const char *failedexpr) __attribute__ ((noreturn));

void __assert_func(const char *file, int line, const char *func, const char *failedexpr)
{
    (void)file;
    (void)line;
    (void)func;
    (void)failedexpr;

    trace_printf("assertion \"%s\" failed: file \"%s\", line %d%s%s\n", failedexpr, file, line,
        (func != NULL) ? ", function: " : "", (func != NULL) ? func : "");

#if defined(DEBUG)
    __asm volatile ("bkpt 0");
#endif

    abort();
}

#if defined(USE_FULL_ASSERT)

#include "stm32_assert.h"

void assert_failed(uint8_t *file, uint32_t line) __attribute__ ((weak, noreturn));

void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;

    trace_printf("assert_param() failed: file \"%s\", line %u\n", (const char *)file, (unsigned)line);

#if defined(DEBUG)
    __asm volatile ("bkpt 0");
#endif

    abort();
}

#endif /* USE_FULL_ASSERT */
