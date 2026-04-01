/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_H_
#define STK_SYNC_H_

/*! \file  stk_sync.h
    \brief Implementation of synchronization primitives.
*/

/*! \namespace stk::sync
    \brief     Synchronization primitives for task coordination and resource protection.

    ISR SAFETY GUIDELINES:
    ------------------------------------------------------------------------------------
    Special care must be taken when calling synchronization  methods from within an Interrupt Service Routine (ISR).

    As a general rule, methods that can cause the caller to block or sleep are **STRICTLY FORBIDDEN** in ISRs.

    | Primitive             | ISR Safe Methods                                                                                                                                   |
    | :-------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------- |
    | **Event**             | \c Set(), \c Pulse(), \c Reset(), \c TryWait()                                                                                                     |
    | **EventFlags**        | \c Set(), \c Clear(), \c Get(), \c TryWait(), \c Wait(NO_WAIT)                                                                                     |                                                                                                        |
    | **Semaphore**         | \c Signal(), \c TryWait()                                                                                                                          |
    | **SpinLock**          | None                                                                                                                                               |
    | **Mutex**             | None                                                                                                                                               |
    | **RWMutex**           | None                                                                                                                                               |
    | **ConditionVariable** | \c NotifyOne(), \c NotifyAll(), \c Wait(NO_WAIT)                                                                                                   |
    | **Pipe**              | \c Write(NO_WAIT), \c WriteBulk(NO_WAIT), \c TryWrite(), \c TryWriteBulk(), \c Read(NO_WAIT), \c ReadBulk(NO_WAIT), \c TryRead(), \c TryReadBulk() |

    NOTE:
    * **SpinLock**, **Mutex**, **RWMutex**: Ownership is tied to a Task ID (\a TId).
      Since ISRs lack a valid Task ID context, and these primitives use internal Mutex 
      logic for state protection, their operations are never safe in ISRs.
    * **ConditionVariable::Notify** methods are safe as they only trigger internal wake-ups without
      blocking the caller.

    WARNING:
    Calling a blocking method from an ISR will lead to undefined behavior, memory corruption, or a deadlock.
    In debug build STK_ASSERT will break code execution if ineligible for ISR method is called.
*/
namespace stk {
namespace sync {
} // namespace sync
} // namespace stk

#include "stk_sync_cs.h"
#include "stk_sync_cv.h"
#include "stk_sync_event.h"
#include "stk_sync_eventflags.h"
#include "stk_sync_spinlock.h"
#include "stk_sync_mutex.h"
#include "stk_sync_semaphore.h"
#include "stk_sync_pipe.h"
#include "stk_sync_rwmutex.h"

#endif /* STK_SYNC_H_ */
