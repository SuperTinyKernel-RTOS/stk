/***********************************************************************************************//**
 * \file cyabs_rtos_types.h
 *
 * \brief
 * RTOS specific types of the SuperTinyKernel (STK) port of the Infineon RTOS abstraction layer.
 * Plain C compatible: the STK objects live in opaque, suitably sized and aligned storage that is
 * embedded in the cy_*_t types (the same approach as in the STK ThreadX shim).
 *
 ***************************************************************************************************
 * \copyright
 * Licensed under the Apache License, Version 2.0 (same license as the abstraction-rtos library).
 * SPDX-License-Identifier: Apache-2.0
 **************************************************************************************************/

#ifndef CYABS_RTOS_TYPES_H
#define CYABS_RTOS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cyabs_rtos_stk_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

/******************************************************
*                 Type Definitions
******************************************************/

// RTOS thread priority. STK numbers priorities upwards (a bigger value is more urgent), which is
// the natural order of this enumeration: MIN <= LOW <= ... <= REALTIME <= MAX.
typedef enum
{
    CY_RTOS_PRIORITY_MIN         = CY_RTOS_STK_PRIO_MIN,          /**< Minimum allowable Thread
                                                                     priority */
    CY_RTOS_PRIORITY_LOW         = CY_RTOS_STK_PRIO_LOW,          /**< A low priority Thread */
    CY_RTOS_PRIORITY_BELOWNORMAL = CY_RTOS_STK_PRIO_BELOWNORMAL,  /**< A slightly below normal
                                                                     Thread priority */
    CY_RTOS_PRIORITY_NORMAL      = CY_RTOS_STK_PRIO_NORMAL,       /**< The normal Thread priority */
    CY_RTOS_PRIORITY_ABOVENORMAL = CY_RTOS_STK_PRIO_ABOVENORMAL,  /**< A slightly elevated Thread
                                                                     priority */
    CY_RTOS_PRIORITY_HIGH        = CY_RTOS_STK_PRIO_HIGH,         /**< A high priority Thread */
    CY_RTOS_PRIORITY_REALTIME    = CY_RTOS_STK_PRIO_REALTIME,     /**< Realtime Thread priority */
    CY_RTOS_PRIORITY_MAX         = CY_RTOS_STK_PRIO_MAX           /**< Maximum allowable Thread
                                                                     priority */
} cy_thread_priority_t;

/** Thread handle: points to the thread control block, which is allocated by
 *  cy_rtos_thread_create() and released by cy_rtos_thread_join(). */
struct cy_stk_thread;
typedef struct cy_stk_thread* cy_thread_t;

/** Mutex (always recursive, with priority inheritance). */
typedef struct
{
    uint32_t id;                                    /**< validity marker (internal) */
    uint64_t storage[CY_RTOS_STK_MUTEX_WORDS];      /**< stk::sync::Mutex (internal) */
} cy_mutex_t;

/** Counting semaphore. */
typedef struct
{
    uint32_t id;                                    /**< validity marker (internal) */
    uint64_t storage[CY_RTOS_STK_SEMAPHORE_WORDS];  /**< stk::sync::SemaphoreT<uint32_t> (internal) */
} cy_semaphore_t;

/** Event flags (all 32 bits are usable). */
typedef struct
{
    uint32_t id;                                    /**< validity marker (internal) */
    uint64_t storage[CY_RTOS_STK_EVENT_WORDS];      /**< stk::sync::EventFlagsT<uint64_t> (internal) */
} cy_event_t;

/** Message queue (ring buffer of fixed size items). */
typedef struct
{
    uint32_t id;                                    /**< validity marker (internal) */
    uint8_t* buffer;                                /**< ring buffer, heap allocated (internal) */
    uint64_t storage[CY_RTOS_STK_QUEUE_WORDS];      /**< stk::sync::MessageQueue (internal) */
} cy_queue_t;

/** Software timer. */
typedef struct
{
    uint32_t id;                                    /**< validity marker (internal) */
    uint64_t storage[CY_RTOS_STK_TIMER_WORDS];      /**< stk::time::Timer based object (internal) */
} cy_timer_t;

// The argument types are wide enough to carry a pointer as well as an integer, like the
// ThreadX (integer) and the FreeRTOS/RTX (pointer) ports are used by the Infineon libraries.
typedef uintptr_t               cy_thread_arg_t;
typedef uintptr_t               cy_timer_callback_arg_t;
typedef uint32_t                cy_time_t;
typedef uint32_t                cy_rtos_error_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* CYABS_RTOS_TYPES_H */
