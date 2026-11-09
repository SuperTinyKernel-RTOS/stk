/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_MEMORY_ALLOCATOR_MALLOC_H_
#define STK_MEMORY_ALLOCATOR_MALLOC_H_

#include "stk_memory_allocator.h"

 /*! \file  stk_memory_allocator_malloc.h
     \brief Ready-made implementation of stk::memory::MemoryAllocator backed by C malloc.

     \par When and why to include this file in your build:
         - **Why**:
           This file provides the strong definitions for \c stk::memory::MemoryAllocator::Weak::Allocate(),
           \c Weak::Free(), and \c Weak::GetStats(), satisfying the weak declarations in
           \c stk_memory_allocator.h. Application code reaches them through the checked
           \c MemoryAllocator::Allocate(), \c Free() and \c GetStats().
           It forwards to the C library \c malloc / \c free, so the heap is whatever your C library and
           linker script provide, and thread-safety depends on the C library lock hooks
           (e.g. \c __malloc_lock in newlib). For a deterministic static heap that needs none of that,
           use \c stk_memory_allocator_default.h instead. No statistics are available from \c malloc,
           GetStats() returns an empty Stats.
         - **When to include**:
           Include this header in one source file of your build (e.g. a dedicated .cpp) when your
           application or integrated third-party components require dynamic memory allocation via
           \c MemoryAllocator, C++ container helpers (<tt>AllocateOneT</tt>, <tt>AllocateArrayT</tt>,
           <tt>AllocateAligned</tt>), heap-storage pool constructors, or global \c new / \c delete operators routing (<tt>STK_MEMORY_OVERRIDE_NEW_DELETE=1</tt>),
           and you do not want to write a custom wrapper around `malloc`/`free`.
         - **When NOT to include**:
           1. **Custom Allocator**: If your application provides its own custom implementation of
              \c MemoryAllocator::Weak::Allocate() and \c Weak::Free() (e.g. wrapping an RTOS heap or hardware memory controller).
           2. **Zero-Heap Enforcement**: If your project enforces a strict zero-heap configuration
              (<tt>STK_MEMORY_NO_HEAP=1</tt>), where all memory must be statically allocated via fixed blocks
              or stack frames.
           3. **Build Target Rule**: Include this header in **exactly one** translation unit within your build
              target to avoid duplicate symbol definitions at link time.
*/

// Private memory allocators (we define malloc, free here to overcome absence of declaration in
// case of -ffreestanding compiler flag).
extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);

void *stk::memory::MemoryAllocator::Weak::Allocate(size_t size)
{
    return malloc(size);
}

void stk::memory::MemoryAllocator::Weak::Free(void *ptr)
{
    free(ptr);
}

stk::memory::MemoryAllocator::Stats stk::memory::MemoryAllocator::Weak::GetStats()
{
    // no statistics from malloc
    return Stats();
}

#endif /* STK_MEMORY_ALLOCATOR_MALLOC_H_ */
