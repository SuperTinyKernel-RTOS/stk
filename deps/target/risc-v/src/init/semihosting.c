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
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

/*! \def   __SEMIHOSTING_PRINTF_BUFFER
    \brief printf() buffer size (bytes).
*/
#define SEMIHOSTING_PRINTF_BUFFER (128)

extern int _open(const char *path, int oflag, ...);
extern int _close(int fildes);
extern ssize_t _read(int fildes, void* buf, size_t nbyte);
extern ssize_t _write(int fildes, const void* buf, size_t nbyte);
extern off_t _lseek(int fildes, off_t offset, int whence);
extern off_t _lseek(int fildes, off_t offset, int whence);
extern int _fstat(int fildes, struct stat *buf);
extern int _isatty(int fildes);
extern pid_t _getpid(void);
extern int _kill(pid_t pid, int sig);
extern int _gettimeofday(struct timeval *tv, void *tz);
extern int printf(const char *fmt, ...);
extern int putchar(int c);
extern int getchar(void);
extern time_t time(time_t *_timer);

// Details: https://wiki.segger.com/Semihosting
#define SEMIHOSTING_SYS_OPEN            (0x01)
#define SEMIHOSTING_SYS_CLOSE           (0x02)
#define SEMIHOSTING_SYS_WRITEC          (0x03)
#define SEMIHOSTING_SYS_WRITE0          (0x04)
#define SEMIHOSTING_SYS_WRITE           (0x05)
#define SEMIHOSTING_SYS_READ            (0x06)
#define SEMIHOSTING_SYS_READC           (0x07)
#define SEMIHOSTING_SYS_ISERROR         (0x08)
#define SEMIHOSTING_SYS_ISTTY           (0x09)
#define SEMIHOSTING_SYS_SEEK            (0x0A)
#define SEMIHOSTING_SYS_FLEN            (0x0C)
#define SEMIHOSTING_SYS_TMPNAM          (0x0D)
#define SEMIHOSTING_SYS_REMOVE          (0x0E)
#define SEMIHOSTING_SYS_RENAME          (0x0F)
#define SEMIHOSTING_SYS_CLOCK           (0x10)
#define SEMIHOSTING_SYS_TIME            (0x11)
#define SEMIHOSTING_SYS_SYSTEM          (0x12)
#define SEMIHOSTING_SYS_ERRNO           (0x13)
#define SEMIHOSTING_SYS_GET_CMDLINE     (0x15)
#define SEMIHOSTING_SYS_HEAPINFO        (0x16)
#define SEMIHOSTING_EnterSVC            (0x17)
#define SEMIHOSTING_ReportException     (0x18)
#define SEMIHOSTING_SYS_ELAPSED         (0x30)
#define SEMIHOSTING_SYS_TICKFREQ        (0x31)

#define SEMIHOSTING_UNIMPLEMENTED() do { __asm volatile("wfi"); } while(1)

/*! \brief     Call host.
    \note      https://github.com/riscv-software-src/riscv-semihosting/blob/main/riscv-semihosting-spec.adoc
    \return    Return value of the call.
*/
static inline __attribute__((always_inline)) int32_t smh_call_host(int32_t reason, const void *arg)
{
    register int32_t value __asm("a0") = reason;
    register const void *parg __asm("a1") = arg;

    __asm volatile(
    " .option push          \n"
    " .option norvc         \n" // slli, ebreak, srai "instructions must be 32-bit-wide instructions, they may not be compressed 16-bit instructions"
    " .balign 16            \n"
    " slli zero, zero, 0x1f \n" // entry NOP
    " ebreak                \n" // break to debugger
    " srai zero, zero, 0x7  \n" // NOP encoding the semihosting call number 7
    " .option pop           \n"

    : "=r" (value)            /* output */
    : "0" (value), "r" (parg) /* input */
    : "memory"                /* clobber */
    );

    return value;
}

static inline void smh_write0(const char *buf)
{
    smh_call_host(SEMIHOSTING_SYS_WRITE0, buf);
}

static inline void smh_writec(char c)
{
    smh_call_host(SEMIHOSTING_SYS_WRITEC, &c);
}

static inline int32_t smh_readc(void)
{
    return smh_call_host(SEMIHOSTING_SYS_READC, NULL);
}

int _open(const char *path, int oflag, ...)
{
    (void)path;
    (void)oflag;

    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

int _close(int fildes)
{
    (void)fildes;

    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

ssize_t _read(int fildes, void* buf, size_t nbyte)
{
    (void)fildes;
    (void)buf;
    (void)nbyte;

    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

ssize_t _write(int fildes, const void* buf, size_t nbyte)
{
    (void)fildes;
    (void)buf;
    (void)nbyte;

    // TO-DO
    errno = ENOSYS;
    return -1;
}

off_t _lseek(int fildes, off_t offset, int whence)
{
    (void)fildes;
    (void)offset;
    (void)whence;

    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

int _fstat(int fildes, struct stat *buf)
{
    (void)fildes;
    (void)buf;

    // TO-DO
    errno = ENOSYS;
    return -1;
}

int _isatty(int fildes)
{
    (void)fildes;

    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

pid_t _getpid(void)
{
    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

int _kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;

    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tv;
    (void)tz;

    // TO-DO
    SEMIHOSTING_UNIMPLEMENTED();
    return -1;
}

int printf(const char *fmt, ...)
{
    char buffer[SEMIHOSTING_PRINTF_BUFFER] = {};

    va_list va;
    va_start(va, fmt);
    int32_t ret = vsnprintf(buffer, sizeof(buffer) - 1, fmt, va);
    va_end(va);
    
    smh_write0(buffer);
    return ret;
}

int putchar(int c)
{
    smh_writec((char)c);
    return c;
}

int getchar(void)
{
    return smh_readc();
}

time_t time(time_t *_timer)
{
    return smh_call_host(SEMIHOSTING_SYS_TIME, _timer);
}
