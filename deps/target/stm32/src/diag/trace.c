/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cmsis_device.h"
#include "diag/trace.h"

#if defined(TRACE)

/*! \file  trace.c
    \brief Trace channel of the STK STM32Fx target, see trace.h.
*/

/*! \def   STK_TRACE_BUFFER_SIZE
    \brief Size of the buffer of trace_printf() (bytes), longer lines are truncated.
*/
#ifndef STK_TRACE_BUFFER_SIZE
#define STK_TRACE_BUFFER_SIZE (192)
#endif

/*! \def   STK_TRACE_ITM_PORT
    \brief ITM stimulus port the trace goes to (STK_TRACE_ITM).
*/
#ifndef STK_TRACE_ITM_PORT
#define STK_TRACE_ITM_PORT (0)
#endif

#if defined(STK_TRACE_ITM) && (__CORTEX_M < 3)
#error "STK_TRACE_ITM requires a Cortex-M3/M4 device (ITM is not available on Cortex-M0)."
#endif

#if defined(STK_TRACE_SEMIHOSTING) || defined(STK_TRACE_SEMIHOSTING_DEBUG)
#include "semihosting.h"
#endif

void trace_initialize(void)
{
    // ITM and semihosting need no initialization: the debugger configures them
}

ssize_t trace_write(const char *buf, size_t nbyte)
{
#if defined(STK_TRACE_ITM)

    for (size_t i = 0; i < nbyte; ++i)
    {
        // report how much was sent if the debugger did not enable ITM or the stimulus port
        if (((ITM->TCR & ITM_TCR_ITMENA_Msk) == 0U) || ((ITM->TER & (1UL << STK_TRACE_ITM_PORT)) == 0U))
            return (ssize_t)i;

        while (ITM->PORT[STK_TRACE_ITM_PORT].u32 == 0U)
        {
            // wait until the stimulus port is ready
        }
        ITM->PORT[STK_TRACE_ITM_PORT].u8 = (uint8_t)buf[i];
    }
    return (ssize_t)nbyte;

#elif defined(STK_TRACE_SEMIHOSTING_DEBUG)

    // SYS_WRITE0 takes a null-terminated string: send the data in chunks with a terminator appended.
    // Not all debuggers show it (SEGGER J-Link GDB server does not), use the default STDOUT channel then.
    char chunk[32];
    size_t left = nbyte;

    while (left != 0U)
    {
        const size_t n = (left < (sizeof(chunk) - 1U)) ? left : (sizeof(chunk) - 1U);

        memcpy(chunk, buf, n);
        chunk[n] = '\0';
        stk_semihosting_call(STK_SEMIHOSTING_SYS_WRITE0, chunk);

        buf  += n;
        left -= n;
    }
    return (ssize_t)nbyte;

#elif defined(STK_TRACE_SEMIHOSTING)

    // Semihosting STDOUT: the special file ":tt" is opened once and written with SYS_WRITE. The host forwards it
    // to the console of the debugger (Eclipse console with SEGGER J-Link and OpenOCD), the output is shown per line.
    static int handle = 0;
    const void *block[3];
    int ret;

    if (handle == 0)
    {
        block[0] = ":tt";
        block[1] = (const void *)4U;                          // mode "w"
        block[2] = (const void *)(sizeof(":tt") - 1U);        // length without the terminator

        ret = stk_semihosting_call(STK_SEMIHOSTING_SYS_OPEN, block);
        if (ret == -1)
            return -1;
        handle = ret;
    }

    block[0] = (const void *)(intptr_t)handle;
    block[1] = buf;
    block[2] = (const void *)nbyte;

    ret = stk_semihosting_call(STK_SEMIHOSTING_SYS_WRITE, block); // returns the number of bytes NOT written
    if ((ret == -1) || (ret == (int)nbyte)) // SEGGER may return -1, a full miss is an error as well
        return -1;
    return (ssize_t)nbyte - (ssize_t)ret;

#else

    (void)buf;
    (void)nbyte;
    return -1;

#endif
}

int trace_printf(const char *format, ...)
{
    static char buf[STK_TRACE_BUFFER_SIZE];
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    if (n > 0)
    {
        const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : (sizeof(buf) - 1U); // vsnprintf reports the untruncated length
        trace_write(buf, len);
    }
    return n;
}

int trace_puts(const char *s)
{
    int n = (int)trace_write(s, strlen(s));
    trace_write("\n", 1);
    return n + 1;
}

int trace_putchar(int c)
{
    const char ch = (char)c;
    trace_write(&ch, 1);
    return c;
}

void trace_dump_args(int argc, char *argv[])
{
    trace_printf("main(argc=%d, argv=[", argc);
    for (int i = 0; i < argc; ++i)
        trace_printf("%s\"%s\"", (i != 0) ? ", " : "", argv[i]);
    trace_printf("]);\n");
}

#endif /* TRACE */
