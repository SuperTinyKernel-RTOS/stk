/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */
 
 /*! \file  stk_memory_allocator_default.cpp
     \brief Default ready-made implementation of stk::memory::MemoryAllocator backed by a static BytePool.

     \par When and why to include this file in your build:
         - **Why**: 
           This file provides the strong definitions for \c stk::memory::MemoryAllocator::Allocate(),
           \c Free(), and \c GetStats(), satisfying the weak declarations in \c stk_memory_allocator.h.
           It sets up a deterministic, thread-safe static heap using a \c BytePool of size
           \c STK_MEMORY_DEFAULT_HEAP_SIZE (default 8 KB) placed in the BSS segment (.bss).
           It requires no C library \c malloc, no linker heap region configuration, and no platform
           \c __malloc_lock hooks.
         - **When to include**: 
           Add this source file to your build system (e.g. CMake sources or Makefile target) when your
           application or integrated third-party components require dynamic memory allocation via
           \c MemoryAllocator, C++ container helpers (<tt>AllocateOneT</tt>, <tt>AllocateArrayT</tt>,
           <tt>PoolPtr</tt>), or global \c new / \c delete operators routing (<tt>STK_MEMORY_OVERRIDE_NEW_DELETE=1</tt>),
           and you do not want to write a custom wrapper around `malloc`/`free`.
         - **When NOT to include**:
           1. **Custom Allocator**: If your application provides its own custom implementation of
              \c MemoryAllocator::Allocate() and \c Free() (e.g. wrapping an RTOS heap or hardware memory controller).
           2. **Zero-Heap Enforcement**: If your project enforces a strict zero-heap configuration
              (<tt>STK_MEMORY_NO_HEAP=1</tt>), where all memory must be statically allocated via fixed blocks
              or stack frames.
           3. **Build Target Rule**: Compile this file in **exactly one** translation unit within your build
              target to avoid duplicate symbol definitions at link time.
*/

#include "stk_arch.h"
#include "memory/stk_memory_allocator_default.h"
