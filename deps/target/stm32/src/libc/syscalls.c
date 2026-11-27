/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>

#include "diag/trace.h"

/*! \file  syscalls.c
    \brief Minimal system calls of the C library (newlib) for the STK STM32Fx target.

    Standard output and error (file descriptors 1 and 2) go to the trace channel, every other call is not
    implemented and fails with \c ENOSYS. All functions are weak: the application can provide a real implementation
    (for example a UART back-end of \c _write).
    \see   trace.h, system.c
*/

/*! \brief Environment of the program: empty. */
char *__env[1] = { 0 };
char **environ = __env;

#define STK_NOT_IMPLEMENTED(result) do { errno = ENOSYS; return (result); } while (0)

#define STK_WEAK __attribute__ ((weak, used)) // used: must survive LTO, libc references the stubs

STK_WEAK int _close(int fd)                                  { (void)fd; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _execve(const char *name, char *const *argv, char *const *env)
                                                              { (void)name; (void)argv; (void)env; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _fork(void)                                     { STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _fstat(int fd, struct stat *st)
{
#if defined(TRACE)
    if ((fd >= 0) && (fd <= 2)) { st->st_mode = S_IFCHR; return 0; } // stdin/stdout/stderr are character devices
#endif
    (void)fd; (void)st;
    STK_NOT_IMPLEMENTED(-1);
}
STK_WEAK int _getpid(void)                                   { STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _gettimeofday(struct timeval *tv, void *tz)     { (void)tv; (void)tz; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _isatty(int fd)
{
#if defined(TRACE)
    // stdout/stderr are a terminal: newlib then line-buffers them, otherwise printf() output stays in the buffer
    // until it is full or fflush() is called and does not show up in the debugger console
    if ((fd >= 0) && (fd <= 2)) return 1;
#endif
    (void)fd;
    STK_NOT_IMPLEMENTED(0);
}
STK_WEAK int _kill(int pid, int sig)                         { (void)pid; (void)sig; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _link(const char *oldname, const char *newname) { (void)oldname; (void)newname; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _lseek(int fd, int offset, int whence)          { (void)fd; (void)offset; (void)whence; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _open(const char *name, int flags, int mode)    { (void)name; (void)flags; (void)mode; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _read(int fd, char *buf, int len)               { (void)fd; (void)buf; (void)len; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _stat(const char *name, struct stat *st)        { (void)name; (void)st; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK clock_t _times(struct tms *buf)                     { (void)buf; STK_NOT_IMPLEMENTED((clock_t)-1); }
STK_WEAK int _unlink(const char *name)                       { (void)name; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int _wait(int *status)                              { (void)status; STK_NOT_IMPLEMENTED(-1); }

STK_WEAK int raise(int sig)                                  { (void)sig; STK_NOT_IMPLEMENTED(-1); }
STK_WEAK int kill(pid_t pid, int sig)                        { (void)pid; (void)sig; STK_NOT_IMPLEMENTED(-1); }

STK_WEAK int _write(int fd, char *buf, int len)
{
#if defined(TRACE)
    if ((fd == 1) || (fd == 2))
        return (int)trace_write(buf, (size_t)len);
#endif

    (void)fd;
    (void)buf;
    (void)len;
    STK_NOT_IMPLEMENTED(-1);
}
