/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STM32FX_TRACE_H_
#define STK_STM32FX_TRACE_H_

#include <stddef.h>
#include <sys/types.h>

/*! \file  trace.h
    \brief Trace channel: debug output which is independent of the C library (printf-like calls).

    The trace is enabled by defining \c TRACE (ProjectInclude.cmake does it) and goes to the channel selected by
    one of the macros: \c STK_TRACE_SEMIHOSTING (ARM semihosting, needs a debugger) or \c STK_TRACE_ITM
    (Instrumentation Trace Macrocell over SWO, Cortex-M3/M4 only). Without \c TRACE all calls are empty inline
    functions, so the calls need no conditional compilation.
    \note  The calls are not re-entrant: they format into a shared buffer (STK_TRACE_BUFFER_SIZE bytes, longer
           lines are truncated).
*/

#if defined(TRACE)

#ifdef __cplusplus
extern "C" {
#endif

/*! \brief Initialize the trace channel (nothing to do for ITM and semihosting). */
void trace_initialize(void);

/*! \brief     Write raw bytes to the trace channel.
    \param[in] buf:   Data.
    \param[in] nbyte: Number of bytes.
    \return    Number of bytes written, or -1 if the channel is not available.
*/
ssize_t trace_write(const char *buf, size_t nbyte);

/*! \brief     Formatted output (printf format).
    \return    Number of characters the formatted text has, or negative on a format error.
*/
int trace_printf(const char *format, ...) __attribute__ ((format(printf, 1, 2)));

/*! \brief     Write a string and a new line.
    \param[in] s: Null-terminated string.
    \return    Number of bytes written.
*/
int trace_puts(const char *s);

/*! \brief     Write a character.
    \param[in] c: Character.
    \return    The character.
*/
int trace_putchar(int c);

/*! \brief     Print the arguments of main().
    \param[in] argc: Number of arguments.
    \param[in] argv: Arguments.
*/
void trace_dump_args(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#else /* !TRACE */

static inline void trace_initialize(void) {}
static inline ssize_t trace_write(const char *buf, size_t nbyte) { (void)buf; (void)nbyte; return 0; }
static inline int trace_printf(const char *format, ...) { (void)format; return 0; }
static inline int trace_puts(const char *s) { (void)s; return 0; }
static inline int trace_putchar(int c) { return c; }
static inline void trace_dump_args(int argc, char *argv[]) { (void)argc; (void)argv; }

#endif /* TRACE */

#endif /* STK_STM32FX_TRACE_H_ */
