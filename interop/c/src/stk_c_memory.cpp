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

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

using namespace stk;
using namespace stk::memory;

// Returns a size of memory in stk::Word elements required for object allocation.
template <typename T> static constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1) / sizeof(stk::Word));
}

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
