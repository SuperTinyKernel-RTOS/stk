/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_MEMORY_ALLOCATOR_DEFAULT_H_
#define STK_MEMORY_ALLOCATOR_DEFAULT_H_

#include "stk_memory_allocator.h"
#include "stk_memory_bytepool.h"

#include <new>

/*! \file  stk_memory_allocator_default.h
    \brief Optional ready-made implementation of stk::memory::MemoryAllocator on top of a static BytePool.

    \warning Include this header in \b exactly one translation unit of your application (it defines
             MemoryAllocator::Allocate(), Free() and GetStats()). Do not include it if you provide
             your own implementation.

    The heap lives in a static array (.bss) of \c STK_MEMORY_DEFAULT_HEAP_SIZE bytes, so the result is
    deterministic, needs no C library \c malloc, no linker heap region and no \c __malloc_lock hooks:
    - Allocate() is non-blocking (BytePool::TryAlloc()): it returns nullptr when memory is exhausted.
    - Allocate() and Free() are thread-safe and may be used from an ISR (see BytePool for the
      execution-time caveat).
    - Returned pointers are aligned to BytePool::BLOCK_ALIGN (at least MemoryAllocator::ALIGNMENT, enforced at
      compile time; see STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT).
    - Allocate(0) returns nullptr.
    - The pool is created on first use inside a critical section, so there is no dependency on
      static-initialization order and no compiler-generated guard variables.
*/

/*! \def   STK_MEMORY_DEFAULT_HEAP_SIZE
    \brief Size of the default heap in bytes (block headers included). Default: 8192.
*/
#ifndef STK_MEMORY_DEFAULT_HEAP_SIZE
    #define STK_MEMORY_DEFAULT_HEAP_SIZE (8192U)
#endif

namespace stk {
namespace memory {
namespace detail {

STK_STATIC_ASSERT_DESC(BytePool::BLOCK_ALIGN >= MemoryAllocator::MIN_ALIGNMENT,
    "STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT is below MemoryAllocator::ALIGNMENT: the default allocator would violate its contract");

alignas(BytePool::BLOCK_ALIGN) static uint8_t g_DefaultHeapArea[STK_MEMORY_DEFAULT_HEAP_SIZE];
alignas(BytePool) static uint8_t              g_DefaultHeapPoolMem[sizeof(BytePool)];
static BytePool * volatile                    g_DefaultHeapPool = nullptr;

/*! \brief Get the default heap pool, creating it on the first call.
*/
static inline BytePool &DefaultHeapPool()
{
    if (g_DefaultHeapPool == nullptr)
    {
        const hw::CriticalSection::ScopedLock cs_;

        if (g_DefaultHeapPool == nullptr)
        {
            g_DefaultHeapPool = new (static_cast<void *>(g_DefaultHeapPoolMem))
                BytePool(g_DefaultHeapArea, sizeof(g_DefaultHeapArea), "stk.heap");
        }
    }

    return *g_DefaultHeapPool;
}

} // namespace detail

void *MemoryAllocator::Allocate(size_t size)
{
    return detail::DefaultHeapPool().TryAlloc(size);
}

void MemoryAllocator::Free(void *ptr)
{
    if (ptr != nullptr)
    {
        const bool freed = detail::DefaultHeapPool().Free(ptr);
        STK_UNUSED(freed);
    }
}

MemoryAllocator::Stats MemoryAllocator::GetStats()
{
    return detail::DefaultHeapPool().GetStats();
}

} // namespace memory
} // namespace stk

#endif /* STK_MEMORY_ALLOCATOR_DEFAULT_H_ */
