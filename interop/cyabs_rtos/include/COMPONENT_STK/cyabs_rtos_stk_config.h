/***********************************************************************************************//**
 * \file cyabs_rtos_stk_config.h
 *
 * \brief
 * Build-time configuration of the SuperTinyKernel (STK) port of the Infineon RTOS abstraction
 * layer. Every macro can be overridden from the build system (-D...) or by defining it before
 * this header is included (e.g. in a project-wide header). Plain C compatible.
 *
 ***************************************************************************************************
 * \copyright
 * Licensed under the Apache License, Version 2.0 (same license as the abstraction-rtos library).
 * SPDX-License-Identifier: Apache-2.0
 **************************************************************************************************/

#ifndef CYABS_RTOS_STK_CONFIG_H
#define CYABS_RTOS_STK_CONFIG_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

/******************************************************
*                 Scheduling
******************************************************/

/** Number of priority levels of the fixed-priority scheduler (0 = lowest, LEVELS-1 = highest).
 *  Range [8, 1024]. The cy_thread_priority_t constants are spread over this range. */
#ifndef CY_RTOS_STK_PRIORITY_LEVELS
#define CY_RTOS_STK_PRIORITY_LEVELS     (32U)
#endif

#if (CY_RTOS_STK_PRIORITY_LEVELS < 8U) || (CY_RTOS_STK_PRIORITY_LEVELS > 1024U)
#error "CY_RTOS_STK_PRIORITY_LEVELS must be in the range [8, 1024]"
#endif

/** Maximum number of threads that can exist at the same time (kernel task slots for
 *  cy_rtos_thread_create()). The slots of the software timer tasks are added on top of it. */
#ifndef CY_RTOS_STK_MAX_THREADS
#define CY_RTOS_STK_MAX_THREADS         (16U)
#endif

/** System tick period in microseconds, the resolution of all timeouts and delays. */
#ifndef CY_RTOS_STK_TICK_PERIOD_US
#define CY_RTOS_STK_TICK_PERIOD_US      (1000U)
#endif

/** Priority levels behind the cy_thread_priority_t constants (same relative layout as the
 *  ThreadX port, but with the STK direction: a bigger number is a more urgent thread). */
#define CY_RTOS_STK_PRIO_MIN            (0U)
#define CY_RTOS_STK_PRIO_LOW            ((CY_RTOS_STK_PRIORITY_LEVELS * 1U) / 7U)
#define CY_RTOS_STK_PRIO_BELOWNORMAL    ((CY_RTOS_STK_PRIORITY_LEVELS * 2U) / 7U)
#define CY_RTOS_STK_PRIO_NORMAL         ((CY_RTOS_STK_PRIORITY_LEVELS * 3U) / 7U)
#define CY_RTOS_STK_PRIO_ABOVENORMAL    ((CY_RTOS_STK_PRIORITY_LEVELS * 4U) / 7U)
#define CY_RTOS_STK_PRIO_HIGH           ((CY_RTOS_STK_PRIORITY_LEVELS * 5U) / 7U)
#define CY_RTOS_STK_PRIO_REALTIME       ((CY_RTOS_STK_PRIORITY_LEVELS * 6U) / 7U)
#define CY_RTOS_STK_PRIO_MAX            (CY_RTOS_STK_PRIORITY_LEVELS - 1U)

/******************************************************
*                 Software timers
******************************************************/

/** 1: cy_rtos_timer_*() are available. The kernel then reserves stk::time::TimerHost::TASK_COUNT
 *  extra task slots (a tick task and a handler task) and their stacks.
 *  0: no timer tasks, cy_rtos_timer_init() returns CY_RTOS_UNSUPPORTED. */
#ifndef CY_RTOS_STK_USE_TIMERS
#define CY_RTOS_STK_USE_TIMERS          (1U)
#endif

/** Priority levels of the timer tasks. The tick task (it evaluates the deadlines) must be at least
 *  as urgent as the handler task, in which the timer callbacks run (same choice as the STK
 *  CMSIS-RTOS2 wrapper). */
#ifndef CY_RTOS_STK_TIMER_TICK_PRIORITY
#define CY_RTOS_STK_TIMER_TICK_PRIORITY     (CY_RTOS_STK_PRIO_HIGH)
#endif
#ifndef CY_RTOS_STK_TIMER_HANDLER_PRIORITY
#define CY_RTOS_STK_TIMER_HANDLER_PRIORITY  (CY_RTOS_STK_PRIO_ABOVENORMAL)
#endif

/******************************************************
*                 Memory
******************************************************/

/** Heap used for thread control blocks / stacks (stack == NULL) and queue storage: by default
 *  stk::memory::MemoryAllocator (the application supplies its backend, as for the CMSIS-RTOS2
 *  wrapper), or malloc/free if STK has no heap support. To use another heap define, for the C++
 *  build of the port, CY_RTOS_STK_MALLOC(size) and CY_RTOS_STK_FREE(ptr). */

/** Minimum stack size accepted by cy_rtos_thread_create(), in bytes. It must be at least
 *  STK_STACK_SIZE_MIN * sizeof(stk::Word) (checked at compile time by the port). */
#ifndef CY_RTOS_STK_MIN_STACK_BYTES
#if defined(__riscv)
#define CY_RTOS_STK_MIN_STACK_BYTES     (2560U)
#else
#define CY_RTOS_STK_MIN_STACK_BYTES     (512U)
#endif
#endif

/** Size (in uint64_t words) of the in-place storage of the STK objects inside the cy_*_t types.
 *  The defaults are the measured sizes of the STK objects with a small reserve, for 32-bit and
 *  64-bit targets. The port verifies them with static_assert: if STK is configured with extra
 *  members (e.g. STK_SYNC_DEBUG_NAMES) the build stops and tells which value to increase. */
#if (UINTPTR_MAX > 0xFFFFFFFFU)
#define CY_RTOS_STK_MUTEX_WORDS_DEFAULT     (11U)
#define CY_RTOS_STK_SEMAPHORE_WORDS_DEFAULT (9U)
#define CY_RTOS_STK_EVENT_WORDS_DEFAULT     (9U)
#define CY_RTOS_STK_QUEUE_WORDS_DEFAULT     (21U)
#define CY_RTOS_STK_TIMER_WORDS_DEFAULT     (22U)
#else
#define CY_RTOS_STK_MUTEX_WORDS_DEFAULT     (6U)
#define CY_RTOS_STK_SEMAPHORE_WORDS_DEFAULT (6U)
#define CY_RTOS_STK_EVENT_WORDS_DEFAULT     (6U)
#define CY_RTOS_STK_QUEUE_WORDS_DEFAULT     (11U)
#define CY_RTOS_STK_TIMER_WORDS_DEFAULT     (17U)
#endif

#ifndef CY_RTOS_STK_MUTEX_WORDS
#define CY_RTOS_STK_MUTEX_WORDS             CY_RTOS_STK_MUTEX_WORDS_DEFAULT
#endif
#ifndef CY_RTOS_STK_SEMAPHORE_WORDS
#define CY_RTOS_STK_SEMAPHORE_WORDS         CY_RTOS_STK_SEMAPHORE_WORDS_DEFAULT
#endif
#ifndef CY_RTOS_STK_EVENT_WORDS
#define CY_RTOS_STK_EVENT_WORDS             CY_RTOS_STK_EVENT_WORDS_DEFAULT
#endif
#ifndef CY_RTOS_STK_QUEUE_WORDS
#define CY_RTOS_STK_QUEUE_WORDS             CY_RTOS_STK_QUEUE_WORDS_DEFAULT
#endif
#ifndef CY_RTOS_STK_TIMER_WORDS
#define CY_RTOS_STK_TIMER_WORDS             CY_RTOS_STK_TIMER_WORDS_DEFAULT
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* CYABS_RTOS_STK_CONFIG_H */
