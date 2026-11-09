/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_H_
#define STK_SYNC_H_

/*! \file  stk_sync.h
    \brief Collection of synchronization primitives (\c stk::sync namespace).
*/

/*! \namespace stk::sync
    \brief     Synchronization primitives for task coordination and resource protection.

    All primitives except ScopedCriticalSection require the kernel to be built with \c KERNEL_SYNC.

    ISR SAFETY GUIDELINES:
    ------------------------------------------------------------------------------------
    Methods that can block the caller are **STRICTLY FORBIDDEN** in an ISR. A method which takes a
    timeout is ISR-safe only with \c NO_WAIT. The table lists the methods that may be called from an ISR.

    | Primitive             | ISR Safe Methods                                                                                                                                                          |
    | :-------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
    | **Event**             | \c Set(), \c Pulse(), \c Reset(), \c TryWait(), \c Wait(NO_WAIT), \c WaitEx(NO_WAIT)                                                                                      |
    | **EventFlagsT**       | \c Set(), \c Clear(), \c And(), \c Get(), \c TryWait(), \c Wait(NO_WAIT), \c TryWaitEx(), \c WaitEx(NO_WAIT), \c Sync(NO_WAIT)                                            |
    | **Semaphore**         | \c Signal(), \c TrySignal(), \c TrySignal(ceiling), \c TryWait(), \c Wait(NO_WAIT), \c WaitEx(NO_WAIT), \c GetCount()                                                     |
    | **SpinLock**          | \c None                                                                                                                                                                   |
    | **Mutex**             | \c TryLock(), \c TimedLock(NO_WAIT), \c TimedLockEx(NO_WAIT), \c Unlock(), \c GetOwner(), \c IsLocked(), \c GetRecursionCount()                                           |
    | **FastMutex**         | \c TryLock(), \c TimedLock(NO_WAIT), \c TimedLockEx(NO_WAIT), \c Unlock(), \c GetOwner(), \c IsLocked()                                                                   |
    | **RWMutex**           | \c TryReadLock(), \c TryLock(), \c TimedReadLock(NO_WAIT), \c TimedReadLockEx(NO_WAIT), \c TimedLock(NO_WAIT), \c TimedLockEx(NO_WAIT), \c ReadUnlock(), \c Unlock()   |
    | **ConditionVariable** | \c NotifyOne(), \c NotifyAll(), \c NotifyOne_CS(), \c NotifyAll_CS(), \c Wait(NO_WAIT), \c WaitEx(NO_WAIT)                                                                |
    | **Barrier**           | \c None                                                                                                                                                                   |
    | **Pipe / PipeT**      | \c Write(NO_WAIT), \c WriteBulk(NO_WAIT), \c TryWrite(), \c TryWriteBulk(), \c Read(NO_WAIT), \c ReadBulk(NO_WAIT), \c TryRead(), \c TryReadBulk(), \c Reset()           |
    | **Pipe** (only)       | \c ReadBulkTriggered(NO_WAIT), \c TryReadBulkTriggered()                                                                                                                  |
    | **MessageQueue**      | \c Put(NO_WAIT), \c TryPut(), \c PutFront(NO_WAIT), \c TryPutFront(), \c Get(NO_WAIT), \c TryGet(), \c Peek(NO_WAIT), \c TryPeek(), \c PeekFront(NO_WAIT), \c TryPeekFront(), \c Reset() |

    NOTE:
    - The \c *Ex() variants (\c WriteEx(), \c ReadBulkEx(), \c PutEx(), ...) follow the same rules as their
      plain counterparts.
    - **EventFlagsT**: Applies to every flags type, including the default \c EventFlags typedef
      (\c EventFlagsT<uint32_t>).
    - **Mutex**, **FastMutex**, **RWMutex**: Ownership is tied to a Task ID (\a TId). In an ISR the id is
      \c TID_ISR_N | exception number (see IsIsrTid()), so a non-blocking acquisition made by an ISR must be
      released by the same ISR. Never block on them in an ISR. For data shared between a task and an ISR
      prefer hw::CriticalSection (or ScopedCriticalSection).
    - **SpinLock**, **Barrier**: Never ISR-safe. SpinLock busy-waits for a lock owned by a task, Barrier blocks.
    - **Scheduler lock** (IKernelService::LockScheduler()): while it is held only non-blocking use of these
      primitives is valid (NO_WAIT / Try*() calls and acquisitions which succeed immediately).

    WARNING:
    - Calling a blocking method from an ISR leads to undefined behavior, memory corruption or a deadlock.
      In a debug build STK_ASSERT stops execution when a method which is not allowed in an ISR is called
      from one.
*/
namespace stk {
namespace sync {
} // namespace sync
} // namespace stk

#include "stk_sync_cs.h"
#include "stk_sync_cv.h"
#include "stk_sync_spinlock.h"
#include "stk_sync_mutex.h"
#include "stk_sync_fastmutex.h"
#include "stk_sync_rwmutex.h"
#include "stk_sync_semaphore.h"
#include "stk_sync_event.h"
#include "stk_sync_eventflags.h"
#include "stk_sync_pipe.h"
#include "stk_sync_msgqueue.h"
#include "stk_sync_barrier.h"

#endif /* STK_SYNC_H_ */
