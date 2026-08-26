/*
 * Copyright (c) 2017 Simon Goldschmidt
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com> (STK port)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Original author: Simon Goldschmidt <goldsimon@gmx.de>
 * STK port: Neutron Code Limited <stk@neutroncode.com>
 *
 * ---------------------------------------------------------------------------
 * This is the lwIP sys_arch port for SuperTinyKernel(TM) RTOS (STK).
 *
 * STK does not run a hidden/implicit "the one and only" kernel instance: the
 * application owns and instantiates its own stk::IKernel object (typically
 * stk::Kernel<...>) and is responsible for calling Initialize()/AddTask()/
 * Start() on it. Because sys_thread_new() has no way to receive a kernel
 * reference from lwIP itself, the application MUST register its kernel
 * instance with this port before calling sys_init() / tcpip_init() /
 * lwip_init():
 *
 *     stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC, MY_MAX_TASKS,
 *                 stk::SwitchStrategySmoothWeightedRoundRobin,
 *                 stk::PlatformDefault> my_kernel;
 *
 *     my_kernel.Initialize();
 *     sys_arch_set_kernel(&my_kernel);   // <-- STK-specific, do this first
 *     tcpip_init(NULL, NULL);            // spawns tcpip_thread via sys_thread_new()
 *     ... AddTask() any of your own tasks ...
 *     my_kernel.Start();                 // never returns
 *
 * The kernel MUST be instantiated with stk::KERNEL_SYNC (this port relies on
 * stk::sync::Mutex/Semaphore/MessageQueue) and SHOULD be instantiated with
 * stk::KERNEL_DYNAMIC if you want threads spawned by sys_thread_new() to be
 * removable (lwIP threads are normally fire-and-forget and never return).
 * ---------------------------------------------------------------------------
 */
#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "lwip/opt.h"
#include "lwip/arch.h"

/* -----------------------------------------------------------------------------
 * Port configuration
 * -------------------------------------------------------------------------- */

/** Version of STK interface. */
#define LWIP_STK_VERSION (0x20260801)

/** Set this to 1 if the stacksize passed to sys_thread_new() should be
 * interpreted as a number of stack Words (stk::Word == uintptr_t, STK-native).
 * Set to 0 to interpret it as a byte count (lwIP-like) instead. Default is 1.
 */
#ifndef LWIP_STK_THREAD_STACKSIZE_IS_STACKWORDS
#define LWIP_STK_THREAD_STACKSIZE_IS_STACKWORDS  1
#endif

/** Hardware access mode used for threads spawned by sys_thread_new()
 * (tcpip_thread and any application thread created through lwIP). */
#ifndef LWIP_STK_THREAD_ACCESS_MODE
#define LWIP_STK_THREAD_ACCESS_MODE  stk::ACCESS_PRIVILEGED
#endif

/** Set this to 1 to include a sanity check that sys_arch_protect() and
 * sys_arch_unprotect() are called in a matching, correctly-nested fashion. */
#ifndef LWIP_STK_SYS_ARCH_PROTECT_SANITY_CHECK
#define LWIP_STK_SYS_ARCH_PROTECT_SANITY_CHECK   0
#endif

/** Set this to 1 to let sys_mbox_free() check that the queue is empty when freed. */
#ifndef LWIP_STK_CHECK_QUEUE_EMPTY_ON_FREE
#define LWIP_STK_CHECK_QUEUE_EMPTY_ON_FREE       0
#endif

/** Set this to 1 to enable core locking check functions in this port.
 * For this to work, define LWIP_ASSERT_CORE_LOCKED() and
 * LWIP_MARK_TCPIP_THREAD() correctly in your lwipopts.h! */
#ifndef LWIP_STK_CHECK_CORE_LOCKING
#define LWIP_STK_CHECK_CORE_LOCKING              1
#endif

/** Set this to 0 to implement sys_now() yourself, e.g. using a HW timer.
 * Default is 1, where stk::GetTimeNowMs() (kernel tick counter) is used. */
#ifndef LWIP_STK_SYS_NOW_FROM_STK
#define LWIP_STK_SYS_NOW_FROM_STK                1
#endif

/** Max number of distinct calling threads that can concurrently hold a
 * per-thread netconn semaphore (LWIP_NETCONN_SEM_PER_THREAD). STK's ITask
 * has no generic user-data/TLS slot of its own, so this port keeps a small,
 * bounded side-table keyed by TId instead. Raise this if you get an
 * assertion from sys_arch_netconn_sem_alloc(). */
#ifndef LWIP_STK_NETCONN_SEM_MAX_THREADS
#define LWIP_STK_NETCONN_SEM_MAX_THREADS         16U
#endif

/** Max number of stk::sync::Mutex instances this port can hand out concurrently
 * (sys_mutex_new()). lwIP itself only allocates a handful (e.g. the tcpip core
 * lock, when LWIP_TCPIP_CORE_LOCKING is on and LWIP_STK_CUSTOM_CORE_LOCKING is
 * off) - raise this if you get an ERR_MEM from sys_mutex_new() at runtime. */
#ifndef LWIP_STK_MUTEX_POOL_SIZE
#define LWIP_STK_MUTEX_POOL_SIZE                 4U
#endif

/** Max number of stk::sync::Semaphore instances this port can hand out
 * concurrently (sys_sem_new(), including LWIP_NETCONN_SEM_PER_THREAD slots).
 * Semaphores are created and freed throughout the connection lifecycle (e.g.
 * one per synchronous netconn/socket call in flight), so this typically needs
 * to be considerably larger than LWIP_STK_MUTEX_POOL_SIZE - raise it if you
 * get an ERR_MEM from sys_sem_new(). */
#ifndef LWIP_STK_SEM_POOL_SIZE
#define LWIP_STK_SEM_POOL_SIZE                   16U
#endif

/** Max number of sys_mbox_t instances (control blocks) this port can hand out
 * concurrently (sys_mbox_new()) - roughly one per active netconn/socket
 * (recvmbox, plus acceptmbox for listening sockets) plus the tcpip_thread
 * mbox itself. Raise if you get an ERR_MEM from sys_mbox_new(). */
#ifndef LWIP_STK_MBOX_POOL_SIZE
#define LWIP_STK_MBOX_POOL_SIZE                  8U
#endif

/** Per-mbox ring-buffer block size in bytes, shared by every sys_mbox_t via a
 * single fixed-block-size pool. Must be >= the largest (capacity * sizeof(void*))
 * that any sys_mbox_new() call in your build will request - i.e. cover the
 * largest of TCPIP_MBOX_SIZE, DEFAULT_ACCEPTMBOX_SIZE, DEFAULT_RAW_RECVMBOX_SIZE,
 * DEFAULT_UDP_RECVMBOX_SIZE, DEFAULT_TCP_RECVMBOX_SIZE, ... per your lwipopts.h.
 * A single shared block size (rather than one pool per distinct mbox size) trades
 * a little internal fragmentation for simplicity and determinism; raise it if
 * sys_mbox_new() starts returning ERR_MEM for an otherwise-reasonable size. */
#ifndef LWIP_STK_MBOX_BUF_BLOCK_BYTES
#define LWIP_STK_MBOX_BUF_BLOCK_BYTES             (32U * sizeof(void *))
#endif

/** Max number of sys_thread_t instances (control blocks + stacks) this port
 * can hand out concurrently (sys_thread_new()) - tcpip_thread plus any
 * application threads spawned through lwIP's own thread API. lwIP threads are
 * fire-and-forget and normally never exit, so in practice this just needs to
 * cover however many are created once at startup. */
#ifndef LWIP_STK_THREAD_POOL_SIZE
#define LWIP_STK_THREAD_POOL_SIZE                4U
#endif

/** Per-thread stack block size in stk::Word units, shared by every sys_thread_t
 * via a single fixed-block-size pool. Must be >= the largest stack any
 * sys_thread_new() call in your build will request (in words) - i.e. cover
 * tcpip_thread's TCPIP_THREAD_STACKSIZE and any application thread's requested
 * stacksize. sys_thread_new() asserts if a request exceeds this. */
#ifndef LWIP_STK_THREAD_STACK_BLOCK_WORDS
#define LWIP_STK_THREAD_STACK_BLOCK_WORDS        1024U
#endif

/** This is returned by _fromisr() sys functions to tell the outermost function
 * that a higher priority task was woken and the scheduler needs to be invoked.
 * \note STK's SmoothWeightedRoundRobin / fixed-priority switch strategies
 *       re-evaluate scheduling internally as part of Signal()/Set()/Put(),
 *       so this port never actually returns ERR_NEED_SCHED. It is kept only
 *       for API/source compatibility with code written against other ports.
 */
#define ERR_NEED_SCHED 123

/* This port includes STK headers in sys_arch.cpp only. STK uses C++ classes
 * as object types. We use wrapper structs holding an opaque void* instead of
 * the C++ type directly so that this header remains includable from lwIP's
 * plain-C translation units.
 *
 * sys_arch.cpp is compiled as C++ (it needs STK's C++ API), so every
 * function prototype below is wrapped in extern "C" when seen from a C++
 * translation unit. Without this, a C++ compiler would give these
 * declarations C++ (mangled) linkage, which would not match the plain-C
 * linkage lwIP's own *.c sources expect when they call these functions -
 * and sys_arch.cpp's C++ definitions must match whichever linkage the
 * declaration in this header established. */
#ifdef __cplusplus
extern "C" {
#endif

void sys_arch_msleep(u32_t delay_ms);
#define sys_msleep(ms) sys_arch_msleep(ms)

#if SYS_LIGHTWEIGHT_PROT
typedef u32_t sys_prot_t;
#endif /* SYS_LIGHTWEIGHT_PROT */

#if !LWIP_COMPAT_MUTEX
struct _sys_mut {
  void *mut; /* stk::sync::Mutex*, heap-allocated */
};
typedef struct _sys_mut sys_mutex_t;
#define sys_mutex_valid_val(mutex)   ((mutex).mut != NULL)
#define sys_mutex_valid(mutex)       (((mutex) != NULL) && sys_mutex_valid_val(*(mutex)))
#define sys_mutex_set_invalid(mutex) ((mutex)->mut = NULL)
#endif /* !LWIP_COMPAT_MUTEX */

#define SYS_SEM_MAX_COUNT (1U)
struct _sys_sem {
  void *sem; /* stk::sync::Semaphore*, heap-allocated */
};
typedef struct _sys_sem sys_sem_t;
#define sys_sem_valid_val(sema)   ((sema).sem != NULL)
#define sys_sem_valid(sema)       (((sema) != NULL) && sys_sem_valid_val(*(sema)))
#define sys_sem_set_invalid(sema) ((sema)->sem = NULL)

struct _sys_mbox {
  void *mbx; /* internal LwipStkMbox*, heap-allocated (MessageQueue + its buffer) */
};
typedef struct _sys_mbox sys_mbox_t;
#define sys_mbox_valid_val(mbox)   ((mbox).mbx != NULL)
#define sys_mbox_valid(mbox)       (((mbox) != NULL) && sys_mbox_valid_val(*(mbox)))
#define sys_mbox_set_invalid(mbox) ((mbox)->mbx = NULL)

struct _sys_thread {
  void *thread_handle; /* internal LwipStkThread* (implements stk::ITask), heap-allocated */
};
typedef struct _sys_thread sys_thread_t;

#if LWIP_NETCONN_SEM_PER_THREAD
sys_sem_t* sys_arch_netconn_sem_get(void);
void sys_arch_netconn_sem_alloc(void);
void sys_arch_netconn_sem_free(void);
#define LWIP_NETCONN_THREAD_SEM_GET()   sys_arch_netconn_sem_get()
#define LWIP_NETCONN_THREAD_SEM_ALLOC() sys_arch_netconn_sem_alloc()
#define LWIP_NETCONN_THREAD_SEM_FREE()  sys_arch_netconn_sem_free()
#endif /* LWIP_NETCONN_SEM_PER_THREAD */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ---------------------------------------------------------------------------
 * STK-specific: kernel registration.
 *
 * Deliberately OUTSIDE the extern "C" block above: it takes a real C++ type
 * (stk::IKernel*), so it can only ever be declared/called from C++ anyway.
 * Only callable from C++ application code (not from lwIP's own C sources,
 * which never need to call it). Must be called exactly once, before
 * sys_init()/tcpip_init()/lwip_init().
 * ------------------------------------------------------------------------ */
#ifdef __cplusplus
namespace stk { class IKernel; }

extern "C" void sys_arch_set_kernel(stk::IKernel *kernel);
#endif /* __cplusplus */

#endif /* LWIP_ARCH_SYS_ARCH_H */
