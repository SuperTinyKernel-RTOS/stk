/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef TX_API_H
#define TX_API_H

/*! \file  tx_api.h
    \brief Eclipse ThreadX (Azure RTOS ThreadX) compatible API on top of SuperTinyKernel RTOS.

    \defgroup threadx STK ThreadX API
    \brief    ThreadX interface for C++ API of SuperTinyKernel RTOS.

    This header is a drop-in replacement for ThreadX's tx_api.h: application code written against
    the standard tx_* C API compiles unchanged, while the implementation (threadx_stk.cpp) maps it
    onto STK (sched::SwitchStrategyPreemptionThreshold<TX_MAX_PRIORITIES, true> = ThreadX-style priority + preemption-threshold + time-slicing).

    The header is plain C (also valid C++). Control blocks are caller-allocated as in ThreadX; the STK
    objects live in the opaque storage member of each control block (placement-new, no heap).

    See README.md in this folder for the full list of differences from native ThreadX.

    @{
*/

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Configuration (override with -D or in a tx_user.h included before this file) */
/* ------------------------------------------------------------------------- */

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif

/* Tick rate. STK is initialized with a tick resolution of 1000000 / TX_TIMER_TICKS_PER_SECOND us. */
#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND       (1000UL)
#endif

/* Number of priority levels, [1, 1024] (limit of sched::SwitchStrategyPreemptionThreshold). 0 is the highest priority.
   Up to 32 levels use a single-word ready bitmap, 33-64 a 64-bit one, more a multi-word bitmap (larger RAM/ROM footprint). */
#ifndef TX_MAX_PRIORITIES
#define TX_MAX_PRIORITIES               (32U)
#endif

/* Maximum number of concurrently registered threads (the tick and handler tasks of the timer host are
   accounted separately). */
#ifndef TX_STK_MAX_THREADS
#define TX_STK_MAX_THREADS              (16U)
#endif

/* Timer (expiration function) tasks: priority 0 = highest, as in ThreadX. Both the tick task and the handler
   task of the timer host run at this priority. The stack size of the handler task (which runs the expiration
   functions) is set with STK_TIMER_HANDLER_STACK_SIZE of stk_time_timer.h (bytes are not used here). */
#ifndef TX_TIMER_THREAD_PRIORITY
#define TX_TIMER_THREAD_PRIORITY        (0U)
#endif
#ifndef TX_TIMER_THREAD_STACK_SIZE
#define TX_TIMER_THREAD_STACK_SIZE      (1024U) /* kept for source compatibility, see above */
#endif

/* Size (in pointer-sized words) of the opaque storage inside every control block. The implementation
   verifies at compile time that the STK objects fit, increase the value if a static assertion fires. */
#ifndef TX_STK_THREAD_STORAGE_WORDS
#define TX_STK_THREAD_STORAGE_WORDS     (24U)
#endif
#ifndef TX_STK_MUTEX_STORAGE_WORDS
#define TX_STK_MUTEX_STORAGE_WORDS      (16U)
#endif
#ifndef TX_STK_SEMAPHORE_STORAGE_WORDS
#define TX_STK_SEMAPHORE_STORAGE_WORDS  (16U)
#endif

#ifndef TX_STK_QUEUE_STORAGE_WORDS
#define TX_STK_QUEUE_STORAGE_WORDS      (48U)
#endif
#ifndef TX_STK_BLOCK_POOL_STORAGE_WORDS
#define TX_STK_BLOCK_POOL_STORAGE_WORDS (32U)
#endif
#ifndef TX_STK_BYTE_POOL_STORAGE_WORDS
#define TX_STK_BYTE_POOL_STORAGE_WORDS  (26U)
#endif
/* The event flags and timer storage is counted in 64-bit words (not pointer-sized): the event flags object holds a
   64-bit flags word (all 32 ThreadX flags plus the error bit reserved by STK) and the timer object holds 64-bit tick
   values, so both need 8-byte alignment also on 32-bit targets, where ALIGN_TYPE is only 4 bytes. */
#ifndef TX_STK_EVENT_FLAGS_STORAGE_DWORDS
#define TX_STK_EVENT_FLAGS_STORAGE_DWORDS (16U)
#endif
#ifndef TX_STK_TIMER_STORAGE_DWORDS
#define TX_STK_TIMER_STORAGE_DWORDS     (8U)
#endif

/* ------------------------------------------------------------------------- */
/* Version                                                                   */
/* ------------------------------------------------------------------------- */

#define AZURE_RTOS_THREADX
#define THREADX_MAJOR_VERSION           6
#define THREADX_MINOR_VERSION           5
#define THREADX_PATCH_VERSION           2
#define THREADX_BUILD_VERSION           202603
#define TX_STK_WRAPPER_NAME             "ThreadX API on SuperTinyKernel"

/* ------------------------------------------------------------------------- */
/* Basic types                                                               */
/* ------------------------------------------------------------------------- */

#define VOID                            void
typedef char                            CHAR;
typedef unsigned char                   UCHAR;
typedef int                             INT;
typedef unsigned int                    UINT;
#if (ULONG_MAX == 0xFFFFFFFFUL)
typedef long                            LONG;
typedef unsigned long                   ULONG;
#else
typedef int                             LONG;
typedef unsigned int                    ULONG;
#endif
typedef unsigned long long              ULONG64;
typedef short                           SHORT;
typedef unsigned short                  USHORT;
typedef uintptr_t                       ALIGN_TYPE;

#define TX_NAME_CONST                   const

/* ------------------------------------------------------------------------- */
/* API input parameters and general constants                                */
/* ------------------------------------------------------------------------- */

#define TX_NO_WAIT                      ((ULONG)  0)
#define TX_WAIT_FOREVER                 ((ULONG)  0xFFFFFFFFUL)
#define TX_AND                          ((UINT)   2)
#define TX_AND_CLEAR                    ((UINT)   3)
#define TX_OR                           ((UINT)   0)
#define TX_OR_CLEAR                     ((UINT)   1)
#define TX_1_ULONG                      ((UINT)   1)
#define TX_2_ULONG                      ((UINT)   2)
#define TX_4_ULONG                      ((UINT)   4)
#define TX_8_ULONG                      ((UINT)   8)
#define TX_16_ULONG                     ((UINT)   16)
#define TX_NO_TIME_SLICE                ((ULONG)  0)
#define TX_AUTO_START                   ((UINT)   1)
#define TX_DONT_START                   ((UINT)   0)
#define TX_AUTO_ACTIVATE                ((UINT)   1)
#define TX_NO_ACTIVATE                  ((UINT)   0)
#define TX_TRUE                         ((UINT)   1)
#define TX_FALSE                        ((UINT)   0)
#define TX_NULL                         ((void *) 0)
#define TX_INHERIT                      ((UINT)   1)
#define TX_NO_INHERIT                   ((UINT)   0)
#define TX_THREAD_ENTRY                 ((UINT)   0)
#define TX_THREAD_EXIT                  ((UINT)   1)
#define TX_NO_SUSPENSIONS               ((UINT)   0)
#define TX_NO_MESSAGES                  ((UINT)   0)
#define TX_EMPTY                        ((ULONG)  0)
#define TX_CLEAR_ID                     ((ULONG)  0)

/* Interrupt posture (Cortex-M style PRIMASK values). */
#define TX_INT_DISABLE                  ((UINT)   1)
#define TX_INT_ENABLE                   ((UINT)   0)

/* Thread execution states. */
#define TX_READY                        ((UINT) 0)
#define TX_COMPLETED                    ((UINT) 1)
#define TX_TERMINATED                   ((UINT) 2)
#define TX_SUSPENDED                    ((UINT) 3)
#define TX_SLEEP                        ((UINT) 4)
#define TX_QUEUE_SUSP                   ((UINT) 5)
#define TX_SEMAPHORE_SUSP               ((UINT) 6)
#define TX_EVENT_FLAG                   ((UINT) 7)
#define TX_BLOCK_MEMORY                 ((UINT) 8)
#define TX_BYTE_MEMORY                  ((UINT) 9)
#define TX_IO_DRIVER                    ((UINT) 10)
#define TX_FILE                         ((UINT) 11)
#define TX_TCP_IP                       ((UINT) 12)
#define TX_MUTEX_SUSP                   ((UINT) 13)
#define TX_PRIORITY_CHANGE              ((UINT) 14)

/* Return codes. */
#define TX_SUCCESS                      ((UINT) 0x00)
#define TX_DELETED                      ((UINT) 0x01)
#define TX_POOL_ERROR                   ((UINT) 0x02)
#define TX_PTR_ERROR                    ((UINT) 0x03)
#define TX_WAIT_ERROR                   ((UINT) 0x04)
#define TX_SIZE_ERROR                   ((UINT) 0x05)
#define TX_GROUP_ERROR                  ((UINT) 0x06)
#define TX_NO_EVENTS                    ((UINT) 0x07)
#define TX_OPTION_ERROR                 ((UINT) 0x08)
#define TX_QUEUE_ERROR                  ((UINT) 0x09)
#define TX_QUEUE_EMPTY                  ((UINT) 0x0A)
#define TX_QUEUE_FULL                   ((UINT) 0x0B)
#define TX_SEMAPHORE_ERROR              ((UINT) 0x0C)
#define TX_NO_INSTANCE                  ((UINT) 0x0D)
#define TX_THREAD_ERROR                 ((UINT) 0x0E)
#define TX_PRIORITY_ERROR               ((UINT) 0x0F)
#define TX_NO_MEMORY                    ((UINT) 0x10)
#define TX_START_ERROR                  ((UINT) 0x10)
#define TX_DELETE_ERROR                 ((UINT) 0x11)
#define TX_RESUME_ERROR                 ((UINT) 0x12)
#define TX_CALLER_ERROR                 ((UINT) 0x13)
#define TX_SUSPEND_ERROR                ((UINT) 0x14)
#define TX_TIMER_ERROR                  ((UINT) 0x15)
#define TX_TICK_ERROR                   ((UINT) 0x16)
#define TX_ACTIVATE_ERROR               ((UINT) 0x17)
#define TX_THRESH_ERROR                 ((UINT) 0x18)
#define TX_SUSPEND_LIFTED               ((UINT) 0x19)
#define TX_WAIT_ABORTED                 ((UINT) 0x1A)
#define TX_WAIT_ABORT_ERROR             ((UINT) 0x1B)
#define TX_MUTEX_ERROR                  ((UINT) 0x1C)
#define TX_NOT_AVAILABLE                ((UINT) 0x1D)
#define TX_NOT_OWNED                    ((UINT) 0x1E)
#define TX_INHERIT_ERROR                ((UINT) 0x1F)
#define TX_NOT_DONE                     ((UINT) 0x20)
#define TX_CEILING_EXCEEDED             ((UINT) 0x21)
#define TX_INVALID_PRIORITY             ((UINT) 0x22)
#define TX_FEATURE_NOT_ENABLED          ((UINT) 0xFF)

/* Control block identifiers (same values as ThreadX). */
#define TX_THREAD_ID                    ((ULONG) 0x54485244UL)
#define TX_MUTEX_ID                     ((ULONG) 0x4D555445UL)
#define TX_QUEUE_ID                     ((ULONG) 0x51554555UL)
#define TX_SEMAPHORE_ID                 ((ULONG) 0x53454D41UL)
#define TX_EVENT_FLAGS_ID               ((ULONG) 0x4556454EUL)
#define TX_BLOCK_POOL_ID                ((ULONG) 0x424C4F43UL)
#define TX_BYTE_POOL_ID                 ((ULONG) 0x42595445UL)
#define TX_TIMER_ID                     ((ULONG) 0x54494D52UL)

/* ------------------------------------------------------------------------- */
/* Control blocks                                                            */
/* ------------------------------------------------------------------------- */

struct TX_THREAD_STRUCT;
struct TX_MUTEX_STRUCT;
struct TX_SEMAPHORE_STRUCT;
struct TX_QUEUE_STRUCT;
struct TX_EVENT_FLAGS_GROUP_STRUCT;
struct TX_BLOCK_POOL_STRUCT;
struct TX_BYTE_POOL_STRUCT;
struct TX_TIMER_STRUCT;

typedef struct TX_THREAD_STRUCT
{
    ULONG               tx_thread_id;
    ULONG               tx_thread_run_count;
    VOID               *tx_thread_stack_start;
    ULONG               tx_thread_stack_size;
    ULONG               tx_thread_time_slice;
    TX_NAME_CONST CHAR *tx_thread_name;
    UINT                tx_thread_priority;           /* user priority (0 = highest)               */
    UINT                tx_thread_state;
    UINT                tx_thread_preempt_threshold;  /* user preemption-threshold                 */
    VOID              (*tx_thread_entry)(ULONG entry_input);
    ULONG               tx_thread_entry_parameter;
    VOID              (*tx_thread_entry_exit_notify)(struct TX_THREAD_STRUCT *thread_ptr, UINT condition);
    VOID               *tx_thread_extension_ptr;      /* free for application use (TLS-like)       */
    UINT                tx_thread_stk_start_priority;   /* values given to tx_thread_create(), restored */
    UINT                tx_thread_stk_start_threshold;  /* by tx_thread_reset() (internal)              */
    ULONG               tx_thread_stk_start_time_slice;
    ALIGN_TYPE          tx_thread_stk_storage[TX_STK_THREAD_STORAGE_WORDS];
} TX_THREAD;

typedef struct TX_MUTEX_STRUCT
{
    ULONG               tx_mutex_id;
    TX_NAME_CONST CHAR *tx_mutex_name;
    ALIGN_TYPE          tx_mutex_stk_storage[TX_STK_MUTEX_STORAGE_WORDS];
} TX_MUTEX;

typedef struct TX_SEMAPHORE_STRUCT
{
    ULONG               tx_semaphore_id;
    TX_NAME_CONST CHAR *tx_semaphore_name;
    VOID              (*tx_semaphore_put_notify)(struct TX_SEMAPHORE_STRUCT *notify_semaphore_ptr);
    ALIGN_TYPE          tx_semaphore_stk_storage[TX_STK_SEMAPHORE_STORAGE_WORDS];
} TX_SEMAPHORE;

typedef struct TX_EVENT_FLAGS_GROUP_STRUCT
{
    ULONG               tx_event_flags_group_id;
    TX_NAME_CONST CHAR *tx_event_flags_group_name;
    VOID              (*tx_event_flags_group_set_notify)(struct TX_EVENT_FLAGS_GROUP_STRUCT *notify_group_ptr);
    ULONG64             tx_event_flags_group_stk_storage[TX_STK_EVENT_FLAGS_STORAGE_DWORDS];
} TX_EVENT_FLAGS_GROUP;

typedef struct TX_QUEUE_STRUCT
{
    ULONG               tx_queue_id;
    TX_NAME_CONST CHAR *tx_queue_name;
    UINT                tx_queue_message_size;       /* in ULONGs */
    VOID              (*tx_queue_send_notify)(struct TX_QUEUE_STRUCT *notify_queue_ptr);
    ALIGN_TYPE          tx_queue_stk_storage[TX_STK_QUEUE_STORAGE_WORDS];
} TX_QUEUE;

typedef struct TX_BLOCK_POOL_STRUCT
{
    ULONG               tx_block_pool_id;
    TX_NAME_CONST CHAR *tx_block_pool_name;
    ALIGN_TYPE          tx_block_pool_stk_storage[TX_STK_BLOCK_POOL_STORAGE_WORDS];
} TX_BLOCK_POOL;

typedef struct TX_BYTE_POOL_STRUCT
{
    ULONG               tx_byte_pool_id;
    TX_NAME_CONST CHAR *tx_byte_pool_name;
    ALIGN_TYPE          tx_byte_pool_stk_storage[TX_STK_BYTE_POOL_STORAGE_WORDS];
} TX_BYTE_POOL;

/* The timer object (stk::time::TimerHost::Timer) lives in the opaque storage, the timers are serviced by the
   timer host (tick task + handler task). The storage is 64-bit aligned, see TX_STK_TIMER_STORAGE_DWORDS. */
typedef struct TX_TIMER_STRUCT
{
    ULONG               tx_timer_id;
    TX_NAME_CONST CHAR *tx_timer_name;
    VOID              (*tx_timer_expiration_function)(ULONG input);
    ULONG               tx_timer_expiration_input;
    ULONG               tx_timer_initial_ticks;
    ULONG               tx_timer_reschedule_ticks;
    ULONG64             tx_timer_stk_storage[TX_STK_TIMER_STORAGE_DWORDS];
} TX_TIMER;

/* ------------------------------------------------------------------------- */
/* Application entry point (provided by the application)                     */
/* ------------------------------------------------------------------------- */

VOID    tx_application_define(VOID *first_unused_memory);

/* ------------------------------------------------------------------------- */
/* Kernel / system                                                           */
/* ------------------------------------------------------------------------- */

VOID    tx_kernel_enter(VOID);
UINT    tx_interrupt_control(UINT new_posture);
ULONG   tx_time_get(VOID);
VOID    tx_time_set(ULONG new_time);

/* ------------------------------------------------------------------------- */
/* Thread                                                                    */
/* ------------------------------------------------------------------------- */

UINT        tx_thread_create(TX_THREAD *thread_ptr, TX_NAME_CONST CHAR *name_ptr,
                             VOID (*entry_function)(ULONG entry_input), ULONG entry_input,
                             VOID *stack_start, ULONG stack_size,
                             UINT priority, UINT preempt_threshold,
                             ULONG time_slice, UINT auto_start);
UINT        tx_thread_delete(TX_THREAD *thread_ptr);
UINT        tx_thread_entry_exit_notify(TX_THREAD *thread_ptr,
                             VOID (*thread_entry_exit_notify)(TX_THREAD *notify_thread_ptr, UINT type));
TX_THREAD  *tx_thread_identify(VOID);
UINT        tx_thread_info_get(TX_THREAD *thread_ptr, TX_NAME_CONST CHAR **name, UINT *state,
                             ULONG *run_count, UINT *priority, UINT *preemption_threshold,
                             ULONG *time_slice, TX_THREAD **next_thread, TX_THREAD **next_suspended_thread);
UINT        tx_thread_preemption_change(TX_THREAD *thread_ptr, UINT new_threshold, UINT *old_threshold);
UINT        tx_thread_priority_change(TX_THREAD *thread_ptr, UINT new_priority, UINT *old_priority);
VOID        tx_thread_relinquish(VOID);
UINT        tx_thread_reset(TX_THREAD *thread_ptr);
UINT        tx_thread_resume(TX_THREAD *thread_ptr);
UINT        tx_thread_sleep(ULONG timer_ticks);
UINT        tx_thread_stack_error_notify(VOID (*stack_error_handler)(TX_THREAD *thread_ptr));
UINT        tx_thread_suspend(TX_THREAD *thread_ptr);
UINT        tx_thread_terminate(TX_THREAD *thread_ptr);
UINT        tx_thread_time_slice_change(TX_THREAD *thread_ptr, ULONG new_time_slice, ULONG *old_time_slice);
UINT        tx_thread_wait_abort(TX_THREAD *thread_ptr);

/* ------------------------------------------------------------------------- */
/* Mutex                                                                     */
/* ------------------------------------------------------------------------- */

UINT    tx_mutex_create(TX_MUTEX *mutex_ptr, TX_NAME_CONST CHAR *name_ptr, UINT inherit);
UINT    tx_mutex_delete(TX_MUTEX *mutex_ptr);
UINT    tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option);
UINT    tx_mutex_put(TX_MUTEX *mutex_ptr);
UINT    tx_mutex_info_get(TX_MUTEX *mutex_ptr, TX_NAME_CONST CHAR **name, ULONG *count, TX_THREAD **owner,
                          TX_THREAD **first_suspended, ULONG *suspended_count, TX_MUTEX **next_mutex);
UINT    tx_mutex_prioritize(TX_MUTEX *mutex_ptr);

/* ------------------------------------------------------------------------- */
/* Semaphore                                                                 */
/* ------------------------------------------------------------------------- */

UINT    tx_semaphore_create(TX_SEMAPHORE *semaphore_ptr, TX_NAME_CONST CHAR *name_ptr, ULONG initial_count);
UINT    tx_semaphore_delete(TX_SEMAPHORE *semaphore_ptr);
UINT    tx_semaphore_get(TX_SEMAPHORE *semaphore_ptr, ULONG wait_option);
UINT    tx_semaphore_put(TX_SEMAPHORE *semaphore_ptr);
UINT    tx_semaphore_ceiling_put(TX_SEMAPHORE *semaphore_ptr, ULONG ceiling);
UINT    tx_semaphore_info_get(TX_SEMAPHORE *semaphore_ptr, TX_NAME_CONST CHAR **name, ULONG *current_value,
                              TX_THREAD **first_suspended, ULONG *suspended_count, TX_SEMAPHORE **next_semaphore);
UINT    tx_semaphore_prioritize(TX_SEMAPHORE *semaphore_ptr);
UINT    tx_semaphore_put_notify(TX_SEMAPHORE *semaphore_ptr,
                              VOID (*semaphore_put_notify)(TX_SEMAPHORE *notify_semaphore_ptr));

/* ------------------------------------------------------------------------- */
/* Event flags                                                               */
/* ------------------------------------------------------------------------- */

UINT    tx_event_flags_create(TX_EVENT_FLAGS_GROUP *group_ptr, TX_NAME_CONST CHAR *name_ptr);
UINT    tx_event_flags_delete(TX_EVENT_FLAGS_GROUP *group_ptr);
UINT    tx_event_flags_get(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG requested_flags, UINT get_option,
                           ULONG *actual_flags_ptr, ULONG wait_option);
UINT    tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set, UINT set_option);
UINT    tx_event_flags_info_get(TX_EVENT_FLAGS_GROUP *group_ptr, TX_NAME_CONST CHAR **name, ULONG *current_flags,
                                TX_THREAD **first_suspended, ULONG *suspended_count,
                                TX_EVENT_FLAGS_GROUP **next_group);
UINT    tx_event_flags_set_notify(TX_EVENT_FLAGS_GROUP *group_ptr,
                                VOID (*events_set_notify)(TX_EVENT_FLAGS_GROUP *notify_group_ptr));

/* ------------------------------------------------------------------------- */
/* Message queue                                                             */
/* ------------------------------------------------------------------------- */

UINT    tx_queue_create(TX_QUEUE *queue_ptr, TX_NAME_CONST CHAR *name_ptr, UINT message_size,
                        VOID *queue_start, ULONG queue_size);
UINT    tx_queue_delete(TX_QUEUE *queue_ptr);
UINT    tx_queue_flush(TX_QUEUE *queue_ptr);
UINT    tx_queue_info_get(TX_QUEUE *queue_ptr, TX_NAME_CONST CHAR **name, ULONG *enqueued, ULONG *available_storage,
                          TX_THREAD **first_suspended, ULONG *suspended_count, TX_QUEUE **next_queue);
UINT    tx_queue_prioritize(TX_QUEUE *queue_ptr);
UINT    tx_queue_receive(TX_QUEUE *queue_ptr, VOID *destination_ptr, ULONG wait_option);
UINT    tx_queue_send(TX_QUEUE *queue_ptr, VOID *source_ptr, ULONG wait_option);
UINT    tx_queue_front_send(TX_QUEUE *queue_ptr, VOID *source_ptr, ULONG wait_option);
UINT    tx_queue_send_notify(TX_QUEUE *queue_ptr, VOID (*queue_send_notify)(TX_QUEUE *notify_queue_ptr));

/* ------------------------------------------------------------------------- */
/* Block memory pool                                                         */
/* ------------------------------------------------------------------------- */

UINT    tx_block_pool_create(TX_BLOCK_POOL *pool_ptr, TX_NAME_CONST CHAR *name_ptr, ULONG block_size,
                             VOID *pool_start, ULONG pool_size);
UINT    tx_block_pool_delete(TX_BLOCK_POOL *pool_ptr);
UINT    tx_block_allocate(TX_BLOCK_POOL *pool_ptr, VOID **block_ptr, ULONG wait_option);
UINT    tx_block_release(VOID *block_ptr);
UINT    tx_block_pool_info_get(TX_BLOCK_POOL *pool_ptr, TX_NAME_CONST CHAR **name, ULONG *available_blocks,
                               ULONG *total_blocks, TX_THREAD **first_suspended, ULONG *suspended_count,
                               TX_BLOCK_POOL **next_pool);
UINT    tx_block_pool_prioritize(TX_BLOCK_POOL *pool_ptr);

/* ------------------------------------------------------------------------- */
/* Byte memory pool                                                          */
/* ------------------------------------------------------------------------- */

UINT    tx_byte_pool_create(TX_BYTE_POOL *pool_ptr, TX_NAME_CONST CHAR *name_ptr, VOID *pool_start, ULONG pool_size);
UINT    tx_byte_pool_delete(TX_BYTE_POOL *pool_ptr);
UINT    tx_byte_allocate(TX_BYTE_POOL *pool_ptr, VOID **memory_ptr, ULONG memory_size, ULONG wait_option);
UINT    tx_byte_release(VOID *memory_ptr);
UINT    tx_byte_pool_info_get(TX_BYTE_POOL *pool_ptr, TX_NAME_CONST CHAR **name, ULONG *available_bytes,
                              ULONG *fragments, TX_THREAD **first_suspended, ULONG *suspended_count,
                              TX_BYTE_POOL **next_pool);
UINT    tx_byte_pool_prioritize(TX_BYTE_POOL *pool_ptr);

/* ------------------------------------------------------------------------- */
/* Application timer                                                         */
/* ------------------------------------------------------------------------- */

UINT    tx_timer_create(TX_TIMER *timer_ptr, TX_NAME_CONST CHAR *name_ptr,
                        VOID (*expiration_function)(ULONG input), ULONG expiration_input,
                        ULONG initial_ticks, ULONG reschedule_ticks, UINT auto_activate);
UINT    tx_timer_delete(TX_TIMER *timer_ptr);
UINT    tx_timer_activate(TX_TIMER *timer_ptr);
UINT    tx_timer_deactivate(TX_TIMER *timer_ptr);
UINT    tx_timer_change(TX_TIMER *timer_ptr, ULONG initial_ticks, ULONG reschedule_ticks);
UINT    tx_timer_info_get(TX_TIMER *timer_ptr, TX_NAME_CONST CHAR **name, UINT *active, ULONG *remaining_ticks,
                          ULONG *reschedule_ticks, TX_TIMER **next_timer);

/* ------------------------------------------------------------------------- */
/* ThreadX macros commonly used by applications                              */
/* ------------------------------------------------------------------------- */

#define TX_DISABLE { unsigned int tx_stk_old_posture_ = tx_interrupt_control(TX_INT_DISABLE);
#define TX_RESTORE   (void) tx_interrupt_control(tx_stk_old_posture_); }

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* TX_API_H */
