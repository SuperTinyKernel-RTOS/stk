/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <cstddef> // for std::size_t

#include <stk_config.h>
#include <stk.h>
#include <memory/stk_memory.h>

#include "stk_c.h"
#include "stk_c_memory.h"

using namespace stk;
using namespace stk::memory;

// Guard against divergence between the C macro STK_BLOCKPOOL_ALIGN_BLOCK_SIZE and
// the C++ BlockMemoryPool::AlignBlockSize() function.  Both must produce identical
// results; if this assertion fires, update one of the two definitions.
static_assert(
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(1)  == BlockMemoryPool::AlignBlockSize(1)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(3)  == BlockMemoryPool::AlignBlockSize(3)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(4)  == BlockMemoryPool::AlignBlockSize(4)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(7)  == BlockMemoryPool::AlignBlockSize(7)  &&
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(16) == BlockMemoryPool::AlignBlockSize(16),
    "STK_BLOCKPOOL_ALIGN_BLOCK_SIZE and BlockMemoryPool::AlignBlockSize() have diverged. "
    "Keep both definitions in sync.");

// Returns a size of memory in stk::Word elements required for object allocation.
template <typename T> static constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1) / sizeof(stk::Word));
}

// Private memory allocators (we define malloc, free here to overcome absence of declaration in
// case of -ffreestanding compiler flag).
extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);
void *stk::memory::MemoryAllocator::Allocate(size_t size) { return malloc(size); }
void stk::memory::MemoryAllocator::Free(void *ptr) { free(ptr); }

// ---------------------------------------------------------------------------
// stk_blockpool_t — wraps a BlockMemoryPool instance
//
// The struct is opaque to C callers. Instances live in s_BlockPools[].
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Static pool of stk_blockpool_t slots
// ---------------------------------------------------------------------------

static struct BlockPoolSlot
{
    BlockPoolSlot() : busy(false)
    {}

    stk_blockpool_t       *pool()       { return reinterpret_cast<stk_blockpool_t *>(storage); }
    const stk_blockpool_t *pool() const { return reinterpret_cast<const stk_blockpool_t *>(storage); }

    // Raw storage for placement-new, keeps the slot trivially constructible
    // while letting BlockMemoryPool's own ctor/dtor run normally.
    Word storage[StkGetWordCountForType<stk_blockpool_t>()];
    bool busy;
}
s_BlockPools[STK_C_BLOCKPOOL_MAX];

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Find the slot that owns 'pool'; returns nullptr if not found.
static BlockPoolSlot *FindSlot(const stk_blockpool_t *pool)
{
    for (uint32_t i = 0; i < STK_C_BLOCKPOOL_MAX; ++i)
    {
        if (s_BlockPools[i].busy && (s_BlockPools[i].pool() == pool))
            return &s_BlockPools[i];
    }
    return nullptr;
}

// Acquire a free slot; returns nullptr when the pool is exhausted.
static BlockPoolSlot *AcquireSlot()
{
    for (uint32_t i = 0; i < STK_C_BLOCKPOOL_MAX; ++i)
    {
        if (!s_BlockPools[i].busy)
        {
            s_BlockPools[i].busy = true;
            return &s_BlockPools[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// C-interface
// ---------------------------------------------------------------------------
extern "C" {

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle — heap storage
// ─────────────────────────────────────────────────────────────────────────────

stk_blockpool_t *stk_blockpool_create(size_t capacity, size_t raw_block_size, const char *name)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(raw_block_size > 0U);

    sync::ScopedCriticalSection __cs;

    BlockPoolSlot *slot = AcquireSlot();
    if (slot == nullptr)
    {
        // pool exhausted — increase STK_C_BLOCKPOOL_MAX
        STK_ASSERT(false);
        return nullptr;
    }

    return new (slot->storage) stk_blockpool_t(capacity, raw_block_size, name);
}

stk_blockpool_t *stk_blockpool_create_static(size_t      capacity,
                                             size_t      raw_block_size,
                                             uint8_t    *storage,
                                             size_t      storage_size,
                                             const char *name)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(raw_block_size > 0U);
    STK_ASSERT(storage != nullptr);
    STK_ASSERT(storage_size >= (capacity * BlockMemoryPool::AlignBlockSize(raw_block_size)));

    sync::ScopedCriticalSection __cs;

    BlockPoolSlot *slot = AcquireSlot();
    if (slot == nullptr)
    {
        // pool exhausted — increase STK_C_BLOCKPOOL_MAX
        STK_ASSERT(false);
        return nullptr;
    }

    return new (slot->storage) stk_blockpool_t(capacity, raw_block_size,
                                               storage, storage_size, name);
}

void stk_blockpool_destroy(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    sync::ScopedCriticalSection __cs;

    BlockPoolSlot *slot = FindSlot(pool);

    // pool not found: double-destroy or corruption
    STK_ASSERT(slot != nullptr);
    if (slot == nullptr)
        return;

    // Explicitly run the destructor (frees heap storage if owned).
    pool->~stk_blockpool_t();
    slot->busy = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Allocation
// ─────────────────────────────────────────────────────────────────────────────

void *stk_blockpool_alloc(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);
    // stk_blockpool_alloc() blocks indefinitely and must never be called from an ISR.
    // Use stk_blockpool_try_alloc() or stk_blockpool_timed_alloc(..., STK_NO_WAIT) instead.
    STK_ASSERT(!hw::IsInsideISR());

    return pool->handle.Alloc();
}

void *stk_blockpool_timed_alloc(stk_blockpool_t *pool, uint32_t timeout)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.TimedAlloc(static_cast<Timeout>(timeout));
}

void *stk_blockpool_try_alloc(stk_blockpool_t *pool)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.TryAlloc();
}

// ─────────────────────────────────────────────────────────────────────────────
// Deallocation
// ─────────────────────────────────────────────────────────────────────────────

bool stk_blockpool_free(stk_blockpool_t *pool, void *ptr)
{
    STK_ASSERT(pool != nullptr);

    return pool->handle.Free(ptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query
// ─────────────────────────────────────────────────────────────────────────────

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

} // extern "C"
