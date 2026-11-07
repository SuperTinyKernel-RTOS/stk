/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_MEMORY_H_
#define STK_MEMORY_H_

/*! \file  stk_memory.h
    \brief Collection of memory-related primitives (\c stk::memory namespace).
*/

/*! \namespace stk::memory
    \brief     Memory-related primitives: fixed-size block pool, variable-size byte pool, zero-heap pool
               variants, pool-aware object helpers (\c PoolPtr) and the general-purpose allocator interface.

    ISR SAFETY GUIDELINES:
    ------------------------------------------------------------------------------------
    Special care must be taken when calling memory methods from within an Interrupt Service Routine (ISR).

    As a general rule, methods that can cause the caller to block or sleep are **STRICTLY FORBIDDEN** in ISRs.

    | Primitive                                 | ISR Safe Methods                                                                                                                                                                                                  |
    | :---------------------------------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
    | **BlockMemoryPool**                       | \c TryAlloc(), \c TryAllocT(), \c TimedAlloc(NO_WAIT), \c TimedAllocT(NO_WAIT), \c TimedAllocEx(NO_WAIT), \c Free(), \c TryNewT(), \c TimedNewT(NO_WAIT), \c TryMakeT(), \c DeleteT(), \c GetStats(), query methods |
    | **BytePool**                              | \c TryAlloc(), \c TryAllocT(), \c TimedAlloc(NO_WAIT), \c TimedAllocT(NO_WAIT), \c TimedAllocEx(NO_WAIT), \c Free(), \c TryNewT(), \c TimedNewT(NO_WAIT), \c TryMakeT(), \c DeleteT(), \c GetInfo(), \c GetStats(), query methods |
    | **StaticBlockPool**, **StaticBytePool**   | Same as the pool they derive from (\c BlockMemoryPool, \c BytePool)                                                                                                                                               |
    | **PoolPtr**                               | \c Get(), \c Release(), \c Reset(), \c operator bool, destructor                                                                                                                                                  |
    | **MemoryAllocator**                       | \c IsAvailable(); \c Allocate(), \c Free(), \c GetStats() and everything built on them only if the implementation is ISR safe (the default one is)                                                                |

    NOTE:
    - **Query methods**: \c IsStorageValid(), \c GetCapacity(), \c GetBlockSize(), \c GetBlockAlignment(), \c GetUsedCount(),
      \c GetFreeCount(), \c GetPeakUsedCount(), \c GetAllocFailCount(), \c IsFull(), \c IsEmpty() (\c BlockMemoryPool) and
      \c IsStorageValid(), \c GetSize(), \c GetMaxRequest(), \c GetPeakUsedSize(), \c GetAllocFailCount(), \c GetMaxScanLength()
      (\c BytePool) are all ISR safe.
    - **TimedAlloc, TimedAllocT, TimedAllocEx, TimedNewT**: ISR safe only with \c NO_WAIT, which is equivalent to the
      corresponding \c Try method. Any other timeout, \c WAIT_INFINITE included, suspends the caller on a
      \c ConditionVariable and is never ISR safe. \c Alloc(), \c AllocT(), \c NewT() and \c MakeT() always block.
    - **TryNewT(), TimedNewT(NO_WAIT), TryMakeT(), DeleteT(), PoolPtr**: ISR safe only if the constructor and
      destructor of the object are.
    - **BlockMemoryPool**: Allocation and release take constant time (O(1)) inside a minimal critical section.
    - **BytePool**: \c TryAlloc() and \c Free() are ISR safe but their execution time is proportional to the number of
      blocks scanned (not constant), and \c GetInfo() walks the whole pool (O(n)). Avoid them in time-critical ISRs on
      pools with many blocks; \c GetMaxScanLength() reports the longest walk (the worst-case time the critical section
      was held) observed on the real workload. \c GetStats() is O(1).
    - **Constructors and destructors** of the pools are never ISR safe (init or task context only). The heap-storage
      constructors additionally call \c MemoryAllocator::Allocate().
    - **MemoryAllocator**: STK does not implement \c Allocate(), \c Free() and \c GetStats(); whether they (and the typed and
      aligned helpers or the global \c new and \c delete override that rely on them) may be called from an ISR is defined by
      your implementation. \c stk_memory_allocator_default.h, which is \b not included by this header (it defines functions:
      include it in exactly one translation unit), is ISR safe.
    - Blocking methods rely on the kernel being compiled with \c KERNEL_SYNC; \c TryAlloc() and \c Free() are always available.

    WARNING:
    - Calling a blocking method from an ISR is a usage error. In debug build STK_ASSERT will break code execution;
      in release build the call fails immediately (\c nullptr or \c WAIT_RESULT_FAIL is returned, the caller is never
      suspended), so the allocation is lost.
*/

namespace stk {
namespace memory {
} // namespace memory
} // namespace stk

#include "stk_memory_allocator.h"
#include "stk_memory_blockpool.h"
#include "stk_memory_bytepool.h"

#endif /* STK_MEMORY_H_ */
