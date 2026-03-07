/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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

void abort(void) __attribute__((noreturn));
extern void __assert_func(const char *file, int line, const char *func, const char *expr) __attribute__((noreturn));
extern caddr_t _sbrk(int incr) __attribute__ ((used));

void abort(void)
{
    //printf("abort\n");

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

    // align
    incr = (incr + (sizeof(size_t) - 1)) & (~(sizeof(size_t) - 1));

    if ((heap_pos + incr) > end)
    {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    heap_pos += incr;
    return (caddr_t)ret;
}
