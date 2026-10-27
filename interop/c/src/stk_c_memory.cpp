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

// Guard against divergence between the C macro STK_BLOCKPOOL_ALIGN_BLOCK_SIZE and
// the C++ BlockMemoryPool::AlignBlockSize() function.  Both must produce identical
// results; if this assertion fires, update one of the two definitions.
static_assert(
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(1U)  == BlockMemoryPool::AlignBlockSize(1U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(3U)  == BlockMemoryPool::AlignBlockSize(3U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(4U)  == BlockMemoryPool::AlignBlockSize(4U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(7U)  == BlockMemoryPool::AlignBlockSize(7U)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(16U) == BlockMemoryPool::AlignBlockSize(16U),
    "STK_BLOCKPOOL_ALIGN_BLOCK_SIZE and BlockMemoryPool::AlignBlockSize() have diverged. "
    "Keep both definitions in sync.");

// Same guard for the BytePool C macros: they must match BytePool::AlignSize(),
// BytePool::RequiredSize() and BytePool::HEADER_SIZE.
static_assert(
    STK_BYTEPOOL_HEADER_SIZE == BytePool::HEADER_SIZE &&
    STK_BYTEPOOL_ALIGN_SIZE(1U)  == BytePool::AlignSize(1U)  &&
    STK_BYTEPOOL_ALIGN_SIZE(8U)  == BytePool::AlignSize(8U)  &&
    STK_BYTEPOOL_ALIGN_SIZE(9U)  == BytePool::AlignSize(9U)  &&
    STK_BYTEPOOL_ALIGN_SIZE(64U) == BytePool::AlignSize(64U) &&
    STK_BYTEPOOL_REQUIRED_SIZE(1U)   == BytePool::RequiredSize(1U)   &&
    STK_BYTEPOOL_REQUIRED_SIZE(128U) == BytePool::RequiredSize(128U),
    "STK_BYTEPOOL_* macros and BytePool have diverged. Keep both definitions in sync.");

// Returns a size of memory in stk::Word elements required for object allocation.
template <typename T> static constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1U) / sizeof(stk::Word));
}

// Private memory allocators (we define malloc, free here to overcome absence of declaration in
// case of -ffreestanding compiler flag).
extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);
void *stk::memory::MemoryAllocator::Allocate(size_t size) { return malloc(size); }
void stk::memory::MemoryAllocator::Free(void *ptr) { free(ptr); }

// -----------------------------------------------------------------------------
// stk_blockpool_t — wraps a BlockMemoryPool instance
//
// The struct is opaque to C callers. Instances live in s_BlockPools[].
// -----------------------------------------------------------------------------

struct stk_blockpool_t
{
    // Constructor forwarded to the external-storage BlockMemoryPool ctor.
    stk_blockpool_t(size_t capacity, size_t raw_block_size,
                    uint8_t *storage, size_t storage_size, const char *name)
        : handle(capacity, raw_block_size, storage, storage_size, name)
    {}

    // Constructor forwarded to the heap-storage BlockMemoryPool ctor.
    stk_blockpool_t(size_t capacity, size_t raw_block_size, const char *name)
        : handle(capacity, raw_block_size, name)
    {}

    BlockMemoryPool handle;
};

// -----------------------------------------------------------------------------
// Static pool of stk_blockpool_t slots
// -----------------------------------------------------------------------------

// Raw storage for one stk_blockpool_t, sized and aligned for the type.
struct BlockPoolMem
{
    alignas(stk_blockpool_t) Word data[StkGetWordCountForType<stk_blockpool_t>()];
};

static struct BlockPoolSlot
{
    BlockPoolSlot() : busy(false)
    {}

    stk_blockpool_t       *pool()       { return reinterpret_cast<stk_blockpool_t *>(reinterpret_cast<void *>(mem.data)); }
    const stk_blockpool_t *pool() const { return reinterpret_cast<const stk_blockpool_t *>(reinterpret_cast<const void *>(mem.data)); }

    BlockPoolMem mem;
    bool         busy;
}
s_BlockPools[STK_C_BLOCKPOOL_MAX];

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

    // Constructor forwarded to the heap-storage BytePool ctor.
    stk_bytepool_t(size_t storage_size, const char *name)
        : handle(storage_size, name)
    {}

    BytePool handle;
};

// Raw storage for one stk_bytepool_t, sized and aligned for the type.
struct BytePoolMem
{
    alignas(stk_bytepool_t) Word data[StkGetWordCountForType<stk_bytepool_t>()];
};

static struct BytePoolSlot
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
static BlockPoolSlot *FindSlot(const stk_blockpool_t *pool)
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
static BlockPoolSlot *AcquireSlot()
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
static BytePoolSlot *FindBytePoolSlot(const stk_bytepool_t *pool)
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
static BytePoolSlot *AcquireBytePoolSlot()
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

// =============================================================================
// C-interface
// =============================================================================
extern "C" {

// -----------------------------------------------------------------------------
// Lifecycle — heap storage
// -----------------------------------------------------------------------------

stk_blockpool_t *stk_blockpool_create(size_t capacity, size_t raw_block_size, const char *name)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(raw_block_size > 0U);

    const sync::ScopedCriticalSection cs_;

    BlockPoolSlot *const slot = AcquireSlot();

    // pool exhausted — increase STK_C_BLOCKPOOL_MAX
    STK_ASSERT(slot != nullptr);

    stk_blockpool_t *result = nullptr;
    if (slot != nullptr)
    {
        result = ConstructIn<stk_blockpool_t>(slot->mem, capacity, raw_block_size, name);
    }

    return result;
}

stk_blockpool_t *stk_blockpool_create_static(size_t      capacity,
                                             size_t      raw_block_size,
                                             uint8_t    *storage_ptr,
                                             size_t      storage_size,
                                             const char *name)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(raw_block_size > 0U);
    STK_ASSERT(storage_ptr != nullptr);
    STK_ASSERT(storage_size >= (capacity * BlockMemoryPool::AlignBlockSize(raw_block_size)));

    const sync::ScopedCriticalSection cs_;

    BlockPoolSlot *const slot = AcquireSlot();

    // pool exhausted — increase STK_C_BLOCKPOOL_MAX
    STK_ASSERT(slot != nullptr);

    stk_blockpool_t *result = nullptr;
    if (slot != nullptr)
    {
        result = ConstructIn<stk_blockpool_t>(slot->mem, capacity, raw_block_size,
                                              storage_ptr, storage_size, name);
    }

    return result;
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
// Allocation
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

void *stk_blockpool_try_alloc(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.TryAlloc();
}

// -----------------------------------------------------------------------------
// Deallocation
// -----------------------------------------------------------------------------

bool stk_blockpool_free(stk_blockpool_t *pool, void *ptr)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.Free(ptr);
}

// -----------------------------------------------------------------------------
// Query
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
    STK_ASSERT(storage_size >= (2U * BytePool::HEADER_SIZE));

    const sync::ScopedCriticalSection cs_;

    BytePoolSlot *const slot = AcquireBytePoolSlot();

    // pool exhausted — increase STK_C_BYTEPOOL_MAX
    STK_ASSERT(slot != nullptr);

    stk_bytepool_t *result = nullptr;
    if (slot != nullptr)
    {
        result = ConstructIn<stk_bytepool_t>(slot->mem, storage_size, name);
    }

    return result;
}

stk_bytepool_t *stk_bytepool_create_static(uint8_t    *storage,
                                           size_t      storage_size,
                                           const char *name)
{
    STK_ASSERT(storage != nullptr);
    STK_ASSERT(storage_size >= (2U * BytePool::HEADER_SIZE));

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
} // extern "C"
// =============================================================================
