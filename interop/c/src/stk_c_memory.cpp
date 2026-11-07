/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk.h>
#include "stk_c.h" // must be included AFTER stk.h
#include "stk_c_memory.h"
#include <memory/stk_memory.h>

using namespace stk;
using namespace stk::memory;

// -----------------------------------------------------------------------------
// Compile-time consistency checks between the C macros and the C++ classes.
// If any of them fires, a C++ configuration (STK_MEMORY_*) is seen differently by the C header:
// set the macros in the build system for every translation unit.
// -----------------------------------------------------------------------------

// Configuration mirrors.
STK_STATIC_ASSERT_DESC(STK_BLOCKPOOL_ALIGN == BlockMemoryPool::ALIGN_DEFAULT,
    "STK_BLOCKPOOL_ALIGN and BlockMemoryPool::ALIGN_DEFAULT have diverged "
    "(STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT). Keep both definitions in sync.");
STK_STATIC_ASSERT_DESC(STK_BYTEPOOL_ALIGN == BytePool::BLOCK_ALIGN,
    "STK_BYTEPOOL_ALIGN and BytePool::BLOCK_ALIGN have diverged "
    "(STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT). Keep both definitions in sync.");
STK_STATIC_ASSERT_DESC(STK_MEM_MIN_ALIGNMENT == MemoryAllocator::MIN_ALIGNMENT,
    "STK_MEM_MIN_ALIGNMENT and MemoryAllocator::MIN_ALIGNMENT have diverged. Keep both definitions in sync.");

// Guard against divergence between the C macro STK_BLOCKPOOL_ALIGN_BLOCK_SIZE and
// the C++ BlockMemoryPool::AlignBlockSize() function, including the optional bitmap.
STK_STATIC_ASSERT_DESC(
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(1U)          == BlockMemoryPool::AlignBlockSize(1U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(3U)          == BlockMemoryPool::AlignBlockSize(3U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(4U)          == BlockMemoryPool::AlignBlockSize(4U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(7U)          == BlockMemoryPool::AlignBlockSize(7U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(16U)         == BlockMemoryPool::AlignBlockSize(16U) &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(17U)         == BlockMemoryPool::AlignBlockSize(17U) &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX(5U, 32U)  == BlockMemoryPool::AlignBlockSize(5U, 32U) &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX(33U, 32U) == BlockMemoryPool::AlignBlockSize(33U, 32U) &&
    STK_BLOCKPOOL_BITMAP_BYTES(1U)              == BlockMemoryPool::BitmapBytes(1U)  &&
    STK_BLOCKPOOL_BITMAP_BYTES(8U)              == BlockMemoryPool::BitmapBytes(8U)  &&
    STK_BLOCKPOOL_BITMAP_BYTES(9U)              == BlockMemoryPool::BitmapBytes(9U)  &&
    STK_BLOCKPOOL_STORAGE_SIZE(8U, 13U)         == BlockMemoryPool::RequiredStorageSize(8U, 13U) &&
    STK_BLOCKPOOL_STORAGE_SIZE_EX(5U, 20U, 32U) == BlockMemoryPool::RequiredStorageSize(5U, 20U, 32U),
    "STK_BLOCKPOOL_* macros and BlockMemoryPool have diverged. Keep both definitions in sync.");

// Same guard for the BytePool C macros: they must match BytePool::AlignSize(),
// BytePool::RequiredSize() and BytePool::HEADER_SIZE (guard word included).
STK_STATIC_ASSERT_DESC(
    STK_BYTEPOOL_HEADER_SIZE         == BytePool::HEADER_SIZE &&
    STK_BYTEPOOL_ALIGN_SIZE(1U)      == BytePool::AlignSize(1U)  &&
    STK_BYTEPOOL_ALIGN_SIZE(8U)      == BytePool::AlignSize(8U)  &&
    STK_BYTEPOOL_ALIGN_SIZE(9U)      == BytePool::AlignSize(9U)  &&
    STK_BYTEPOOL_ALIGN_SIZE(64U)     == BytePool::AlignSize(64U) &&
    STK_BYTEPOOL_REQUIRED_SIZE(1U)   == BytePool::RequiredSize(1U)   &&
    STK_BYTEPOOL_REQUIRED_SIZE(8U)   == BytePool::RequiredSize(8U)   &&
    STK_BYTEPOOL_REQUIRED_SIZE(128U) == BytePool::RequiredSize(128U),
    "STK_BYTEPOOL_* macros and BytePool have diverged. Keep both definitions in sync.");

// C wait result enum must stay in sync with its C++ counterpart (values are cast directly).
STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_WAIT_RESULT_FAIL)     == static_cast<int32_t>(stk::WAIT_RESULT_FAIL)     &&
                       static_cast<int32_t>(STK_WAIT_RESULT_SIGNAL)   == static_cast<int32_t>(stk::WAIT_RESULT_SIGNAL)   &&
                       static_cast<int32_t>(STK_WAIT_RESULT_TIMEOUT)  == static_cast<int32_t>(stk::WAIT_RESULT_TIMEOUT)  &&
                       static_cast<int32_t>(STK_WAIT_RESULT_CANCELED) == static_cast<int32_t>(stk::WAIT_RESULT_CANCELED),
                       "stk_wait_result_t mismatch");

// -----------------------------------------------------------------------------
// stk_blockpool_t — wraps a BlockMemoryPool instance
//
// The struct is opaque to C callers. Instances live in s_BlockPools[].
// -----------------------------------------------------------------------------

struct stk_blockpool_t
{
    // Constructor forwarded to the external-storage BlockMemoryPool ctor.
    stk_blockpool_t(size_t capacity, size_t raw_block_size,
                    uint8_t *storage, size_t storage_size, const char *name, size_t alignment)
        : handle(capacity, raw_block_size, storage, storage_size, name, alignment)
    {}

#if STK_MEMORY_HAS_HEAP
    // Constructor forwarded to the heap-storage BlockMemoryPool ctor.
    stk_blockpool_t(size_t capacity, size_t raw_block_size, const char *name, size_t alignment)
        : handle(capacity, raw_block_size, name, alignment)
    {}
#endif

    BlockMemoryPool handle;
};

// -----------------------------------------------------------------------------
// stk_bytepool_t — wraps a BytePool instance
//
// The struct is opaque to C callers. Instances live in s_BytePools[].
// -----------------------------------------------------------------------------

struct stk_bytepool_t
{
    // Constructor forwarded to the external-storage BytePool ctor.
    stk_bytepool_t(uint8_t *storage, size_t storage_size, const char *name)
        : handle(storage, storage_size, name)
    {}

#if STK_MEMORY_HAS_HEAP
    // Constructor forwarded to the heap-storage BytePool ctor.
    stk_bytepool_t(size_t storage_size, const char *name)
        : handle(storage_size, name)
    {}
#endif

    BytePool handle;
};

// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// Anonymous namespace to make local functions file-local.
namespace {
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Returns a size of memory in stk::Word elements required for object allocation.
template <typename T> constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1U) / sizeof(stk::Word));
}

// Copy stats of the C++ class into the C structure.
void CopyStats(const MemoryAllocator::Stats &src, stk_mem_stats_t *dst)
{
    STK_ASSERT(dst != nullptr);

    if (dst != nullptr)
    {
        dst->capacity         = src.capacity;
        dst->allocated        = src.allocated;
        dst->peak_allocated   = src.peak_allocated;
        dst->allocate_count   = src.allocate_count;
        dst->free_count       = src.free_count;
        dst->alloc_fail_count = src.alloc_fail_count;
        dst->min_ever_free    = src.min_ever_free;
    }
}

// -----------------------------------------------------------------------------
// Static pool of stk_blockpool_t slots
// -----------------------------------------------------------------------------

// Raw storage for one stk_blockpool_t, sized and aligned for the type.
struct BlockPoolMem
{
    alignas(stk_blockpool_t) Word data[StkGetWordCountForType<stk_blockpool_t>()];
};

struct BlockPoolSlot
{
    BlockPoolSlot() : busy(false)
    {}

    stk_blockpool_t       *pool()       { return reinterpret_cast<stk_blockpool_t *>(reinterpret_cast<void *>(mem.data)); }
    const stk_blockpool_t *pool() const { return reinterpret_cast<const stk_blockpool_t *>(reinterpret_cast<const void *>(mem.data)); }

    BlockPoolMem mem;
    bool         busy;
}
s_BlockPools[STK_C_BLOCKPOOL_MAX];

// Raw storage for one stk_bytepool_t, sized and aligned for the type.
struct BytePoolMem
{
    alignas(stk_bytepool_t) Word data[StkGetWordCountForType<stk_bytepool_t>()];
};

struct BytePoolSlot
{
    BytePoolSlot() : busy(false)
    {}

    stk_bytepool_t       *pool()       { return reinterpret_cast<stk_bytepool_t *>(reinterpret_cast<void *>(mem.data)); }
    const stk_bytepool_t *pool() const { return reinterpret_cast<const stk_bytepool_t *>(reinterpret_cast<const void *>(mem.data)); }

    BytePoolMem mem;
    bool        busy;
}
s_BytePools[STK_C_BYTEPOOL_MAX];

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

// Find the slot that owns 'pool'; returns nullptr if not found.
BlockPoolSlot *FindSlot(const stk_blockpool_t *pool)
{
    BlockPoolSlot *result = nullptr;

    for (size_t i = 0U; i < STK_C_BLOCKPOOL_MAX; ++i)
    {
        if (s_BlockPools[i].busy)
        {
            if (s_BlockPools[i].pool() == pool)
            {
                result = &s_BlockPools[i];
                break;
            }
        }
    }

    return result;
}

// Acquire a free slot; returns nullptr when the pool is exhausted.
BlockPoolSlot *AcquireSlot()
{
    BlockPoolSlot *result = nullptr;

    for (size_t i = 0U; i < STK_C_BLOCKPOOL_MAX; ++i)
    {
        if (!s_BlockPools[i].busy)
        {
            s_BlockPools[i].busy = true;
            result = &s_BlockPools[i];
            break;
        }
    }

    return result;
}

// Find the byte pool slot that owns 'pool'; returns nullptr if not found.
BytePoolSlot *FindBytePoolSlot(const stk_bytepool_t *pool)
{
    BytePoolSlot *result = nullptr;

    for (size_t i = 0U; i < STK_C_BYTEPOOL_MAX; ++i)
    {
        if (s_BytePools[i].busy)
        {
            if (s_BytePools[i].pool() == pool)
            {
                result = &s_BytePools[i];
                break;
            }
        }
    }

    return result;
}

// Acquire a free byte pool slot; returns nullptr when the slot pool is exhausted.
BytePoolSlot *AcquireBytePoolSlot()
{
    BytePoolSlot *result = nullptr;

    for (size_t i = 0U; i < STK_C_BYTEPOOL_MAX; ++i)
    {
        if (!s_BytePools[i].busy)
        {
            s_BytePools[i].busy = true;
            result = &s_BytePools[i];
            break;
        }
    }

    return result;
}

// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
} // namespace
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// =============================================================================
// C-interface
// =============================================================================
extern "C" {

// -----------------------------------------------------------------------------
// BlockPool - lifecycle
// -----------------------------------------------------------------------------

stk_blockpool_t *stk_blockpool_create_ex(size_t      capacity,
                                         size_t      raw_block_size,
                                         const char *name,
                                         size_t      alignment)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(raw_block_size > 0U);

    stk_blockpool_t *result = nullptr;

#if STK_MEMORY_HAS_HEAP
    const sync::ScopedCriticalSection cs_;

    BlockPoolSlot *const slot = AcquireSlot();

    // pool exhausted — increase STK_C_BLOCKPOOL_MAX
    STK_ASSERT(slot != nullptr);

    if (slot != nullptr)
    {
        result = ConstructIn<stk_blockpool_t>(slot->mem, capacity, raw_block_size, name, alignment);
    }
#else
    // heap support is compiled out (STK_MEMORY_NO_HEAP): use stk_blockpool_create_static*()
    STK_UNUSED(name);
    STK_UNUSED(alignment);
#endif

    return result;
}

stk_blockpool_t *stk_blockpool_create(size_t capacity, size_t raw_block_size, const char *name)
{
    return stk_blockpool_create_ex(capacity, raw_block_size, name, BlockMemoryPool::ALIGN_DEFAULT);
}

stk_blockpool_t *stk_blockpool_create_static_ex(size_t      capacity,
                                                size_t      raw_block_size,
                                                uint8_t    *storage_ptr,
                                                size_t      storage_size,
                                                const char *name,
                                                size_t      alignment)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(raw_block_size > 0U);
    STK_ASSERT(storage_ptr != nullptr);
    STK_ASSERT(storage_size >= BlockMemoryPool::RequiredStorageSize(capacity, raw_block_size, alignment));

    const sync::ScopedCriticalSection cs_;

    BlockPoolSlot *const slot = AcquireSlot();

    // pool exhausted — increase STK_C_BLOCKPOOL_MAX
    STK_ASSERT(slot != nullptr);

    stk_blockpool_t *result = nullptr;
    if (slot != nullptr)
    {
        result = ConstructIn<stk_blockpool_t>(slot->mem, capacity, raw_block_size,
                                              storage_ptr, storage_size, name, alignment);
    }

    return result;
}

stk_blockpool_t *stk_blockpool_create_static(size_t      capacity,
                                             size_t      raw_block_size,
                                             uint8_t    *storage_ptr,
                                             size_t      storage_size,
                                             const char *name)
{
    return stk_blockpool_create_static_ex(capacity, raw_block_size, storage_ptr, storage_size, name,
                                          BlockMemoryPool::ALIGN_DEFAULT);
}

void stk_blockpool_destroy(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    const sync::ScopedCriticalSection cs_;

    BlockPoolSlot *const slot = FindSlot(pool);

    // pool not found: double-destroy or corruption
    STK_ASSERT(slot != nullptr);
    if (slot != nullptr)
    {
        // Explicitly run the destructor (frees heap storage if owned).
        pool->~stk_blockpool_t();
        slot->busy = false;
    }
}

// -----------------------------------------------------------------------------
// BlockPool - allocation
// -----------------------------------------------------------------------------

void *stk_blockpool_alloc(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);
    // stk_blockpool_alloc() blocks indefinitely and must never be called from an ISR.
    // Use stk_blockpool_try_alloc() or stk_blockpool_timed_alloc(..., STK_NO_WAIT) instead.
    STK_ASSERT(!hw::IsInsideISR());

    return pool->handle.Alloc();
}

void *stk_blockpool_timed_alloc(stk_blockpool_t *pool, stk_timeout_t timeout)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.TimedAlloc(timeout);
}

stk_wait_result_t stk_blockpool_timed_alloc_ex(stk_blockpool_t *pool, void **block, stk_timeout_t timeout)
{
    STK_ASSERT(pool != nullptr);
    STK_ASSERT(block != nullptr);

    return static_cast<stk_wait_result_t>(pool->handle.TimedAllocEx(block, timeout));
}

void *stk_blockpool_try_alloc(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.TryAlloc();
}

// -----------------------------------------------------------------------------
// BlockPool - deallocation
// -----------------------------------------------------------------------------

bool stk_blockpool_free(stk_blockpool_t *pool, void *ptr)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.Free(ptr);
}

// -----------------------------------------------------------------------------
// BlockPool - query
// -----------------------------------------------------------------------------

bool stk_blockpool_is_storage_valid(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.IsStorageValid();
}

size_t stk_blockpool_get_capacity(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetCapacity();
}

size_t stk_blockpool_get_block_size(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetBlockSize();
}

size_t stk_blockpool_get_block_alignment(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetBlockAlignment();
}

size_t stk_blockpool_get_used_count(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetUsedCount();
}

size_t stk_blockpool_get_free_count(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetFreeCount();
}

size_t stk_blockpool_get_peak_used_count(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetPeakUsedCount();
}

size_t stk_blockpool_get_alloc_fail_count(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetAllocFailCount();
}

void stk_blockpool_get_stats(const stk_blockpool_t *pool, stk_mem_stats_t *stats)
{
    STK_ASSERT(pool != nullptr);

    CopyStats(pool->handle.GetStats(), stats);
}

bool stk_blockpool_is_full(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.IsFull();
}

bool stk_blockpool_is_empty(const stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.IsEmpty();
}

BlockMemoryPool *stk_blockpool_get_instance(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return &pool->handle;
}

// =============================================================================
// BytePool
// =============================================================================

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

stk_bytepool_t *stk_bytepool_create(size_t storage_size, const char *name)
{
    STK_ASSERT(storage_size >= (BytePool::HEADER_SIZE + BytePool::BLOCK_ALIGN));

    stk_bytepool_t *result = nullptr;

#if STK_MEMORY_HAS_HEAP
    const sync::ScopedCriticalSection cs_;

    BytePoolSlot *const slot = AcquireBytePoolSlot();

    // pool exhausted — increase STK_C_BYTEPOOL_MAX
    STK_ASSERT(slot != nullptr);

    if (slot != nullptr)
    {
        result = ConstructIn<stk_bytepool_t>(slot->mem, storage_size, name);
    }
#else
    // heap support is compiled out (STK_MEMORY_NO_HEAP): use stk_bytepool_create_static()
    STK_UNUSED(name);
#endif

    return result;
}

stk_bytepool_t *stk_bytepool_create_static(uint8_t    *storage,
                                           size_t      storage_size,
                                           const char *name)
{
    STK_ASSERT(storage != nullptr);
    STK_ASSERT(storage_size >= (BytePool::HEADER_SIZE + BytePool::BLOCK_ALIGN));

    const sync::ScopedCriticalSection cs_;

    BytePoolSlot *const slot = AcquireBytePoolSlot();

    // pool exhausted — increase STK_C_BYTEPOOL_MAX
    STK_ASSERT(slot != nullptr);

    stk_bytepool_t *result = nullptr;
    if (slot != nullptr)
    {
        result = ConstructIn<stk_bytepool_t>(slot->mem, storage, storage_size, name);
    }

    return result;
}

void stk_bytepool_destroy(stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    const sync::ScopedCriticalSection cs_;

    BytePoolSlot *const slot = FindBytePoolSlot(pool);

    // pool not found: double-destroy or corruption
    STK_ASSERT(slot != nullptr);
    if (slot != nullptr)
    {
        // Explicitly run the destructor (frees heap storage if owned).
        pool->~stk_bytepool_t();
        slot->busy = false;
    }
}

// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------

void *stk_bytepool_alloc(stk_bytepool_t *pool, size_t size)
{
    STK_ASSERT(pool != nullptr);
    // stk_bytepool_alloc() blocks indefinitely and must never be called from an ISR.
    // Use stk_bytepool_try_alloc() or stk_bytepool_timed_alloc(..., STK_NO_WAIT) instead.
    STK_ASSERT(!hw::IsInsideISR());

    return pool->handle.Alloc(size);
}

void *stk_bytepool_timed_alloc(stk_bytepool_t *pool, size_t size, stk_timeout_t timeout)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.TimedAlloc(size, timeout);
}

stk_wait_result_t stk_bytepool_timed_alloc_ex(stk_bytepool_t *pool, void **block, size_t size, stk_timeout_t timeout)
{
    STK_ASSERT(pool != nullptr);
    STK_ASSERT(block != nullptr);

    return static_cast<stk_wait_result_t>(pool->handle.TimedAllocEx(block, size, timeout));
}

void *stk_bytepool_try_alloc(stk_bytepool_t *pool, size_t size)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.TryAlloc(size);
}

// -----------------------------------------------------------------------------
// Deallocation
// -----------------------------------------------------------------------------

bool stk_bytepool_free(stk_bytepool_t *pool, void *ptr)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.Free(ptr);
}

// -----------------------------------------------------------------------------
// Query
// -----------------------------------------------------------------------------

void stk_bytepool_get_info(const stk_bytepool_t *pool,
                           size_t               *available,
                           size_t               *largest,
                           size_t               *fragments)
{
    STK_ASSERT(pool != nullptr);

    size_t avail = 0U;
    size_t large = 0U;
    size_t frags = 0U;

    pool->handle.GetInfo(avail, large, frags);

    if (available != nullptr) { *available = avail; }
    if (largest   != nullptr) { *largest   = large; }
    if (fragments != nullptr) { *fragments = frags; }
}

void stk_bytepool_get_stats(const stk_bytepool_t *pool, stk_mem_stats_t *stats)
{
    STK_ASSERT(pool != nullptr);

    CopyStats(pool->handle.GetStats(), stats);
}

size_t stk_bytepool_get_max_scan_length(const stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetMaxScanLength();
}

size_t stk_bytepool_get_alloc_fail_count(const stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetAllocFailCount();
}

size_t stk_bytepool_get_peak_used_size(const stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetPeakUsedSize();
}

bool stk_bytepool_is_storage_valid(const stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.IsStorageValid();
}

size_t stk_bytepool_get_size(const stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetSize();
}

size_t stk_bytepool_get_max_request(const stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.GetMaxRequest();
}

BytePool *stk_bytepool_get_instance(stk_bytepool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return &pool->handle;
}

// =============================================================================
// General-purpose allocator
// =============================================================================

bool stk_mem_is_available(void)
{
#if STK_MEMORY_NO_HEAP
    return false;
#else
    return MemoryAllocator::IsAvailable();
#endif
}

void *stk_mem_allocate(size_t size)
{
    void *result = nullptr;

    if (stk_mem_is_available())
    {
        result = MemoryAllocator::Allocate(size);
    }

    return result;
}

void stk_mem_free(void *ptr)
{
    if ((ptr != nullptr) && stk_mem_is_available())
    {
        MemoryAllocator::Free(ptr);
    }
}

void *stk_mem_allocate_aligned(size_t size, size_t alignment)
{
    void *result = nullptr;

#if STK_MEMORY_HAS_HEAP
    result = MemoryAllocator::AllocateAligned(size, alignment);
#else
    STK_UNUSED(size);
    STK_UNUSED(alignment);
#endif

    return result;
}

void stk_mem_free_aligned(void *ptr)
{
#if STK_MEMORY_HAS_HEAP
    MemoryAllocator::FreeAligned(ptr);
#else
    STK_UNUSED(ptr);
#endif
}

void stk_mem_get_stats(stk_mem_stats_t *stats)
{
    STK_ASSERT(stats != nullptr);

    if (stk_mem_is_available())
    {
        CopyStats(MemoryAllocator::GetStats(), stats);
    }
    else
    {
        CopyStats(MemoryAllocator::Stats(), stats);
    }
}

// =============================================================================
} // extern "C"
// =============================================================================
