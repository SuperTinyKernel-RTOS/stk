/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_MEMORY_H_
#define STK_C_MEMORY_H_

#include "stk_c.h"

/*! \file     stk_c_memory.h
    \brief    C language binding for stk::memory::BlockMemoryPool.

    \c stk_blockpool_t instances are allocated from a static pool of
    \c STK_C_BLOCKPOOL_MAX slots.  Obtain a handle with \c stk_blockpool_create()
    or \c stk_blockpool_create_static() and release it with \c stk_blockpool_destroy().

    Two storage modes are exposed, mirroring the C++ class:

    | Mode             | Function                       | Who owns storage?       |
    |------------------|--------------------------------|-------------------------|
    | Heap storage     | stk_blockpool_create()         | Pool (freed on destroy) |
    | External storage | stk_blockpool_create_static()  | Caller                  |

    For zero-heap deployments, declare a storage buffer with the helper macro
    \c STK_BLOCKPOOL_STORAGE_SIZE() and pass it to \c stk_blockpool_create_static():

    \code
    #define PKT_COUNT  8U
    #define PKT_SIZE   sizeof(Packet)

    STK_BLOCKPOOL_STORAGE_DECL(g_PktStorage, PKT_COUNT, PKT_SIZE);

    stk_blockpool_t *pool = stk_blockpool_create_static(
        PKT_COUNT, PKT_SIZE,
        g_PktStorage, sizeof(g_PktStorage), "pkt_pool");

    void ISR_Receiver(void) {
        void *blk = stk_blockpool_try_alloc(pool);
        if (blk) { FillPacket((Packet *)blk); Queue_Write(blk); }
    }

    void Task_Parser(void) {
        void *blk = NULL;
        if (Queue_Read(&blk)) {
            Parse((Packet *)blk);
            stk_blockpool_free(pool, blk);
        }
    }
    \endcode

    \defgroup c_api_memory STK C Memory API
    \brief    Pure C interface for stk::memory::BlockMemoryPool.
    @{
*/

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Configuration macros
// =============================================================================

/*! \def     STK_C_BLOCKPOOL_MAX
    \brief   Maximum number of concurrent \c stk_blockpool_t instances (default: 8).
    \note    Increase if your application needs more simultaneous pools.
*/
#ifndef STK_C_BLOCKPOOL_MAX
    #define STK_C_BLOCKPOOL_MAX (8U)
#endif

// =============================================================================
// Storage helper macros
// =============================================================================

/*! \def     STK_BLOCKPOOL_ALIGN
    \brief   Required storage alignment in bytes (equals sizeof(void*)).
*/
#define STK_BLOCKPOOL_ALIGN (sizeof(stk_word_t))

/*! \def     STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(raw_size)
    \brief   Compute the internally aligned block size for a given raw byte size.
    \details Rounds \a raw_size up to the nearest multiple of \c STK_BLOCKPOOL_ALIGN,
             with a minimum of \c STK_BLOCKPOOL_ALIGN.  Use this at compile time to
             size external storage buffers correctly.
*/
#define STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(raw_size) \
    (((raw_size) < STK_BLOCKPOOL_ALIGN) \
        ? STK_BLOCKPOOL_ALIGN \
        : (((raw_size) + (STK_BLOCKPOOL_ALIGN - 1U)) & ~(STK_BLOCKPOOL_ALIGN - 1U)))

/*! \def     STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size)
    \brief   Compute the minimum external storage buffer size in bytes.
    \param   capacity: Number of blocks the pool will hold.
    \param   raw_block_size: Raw per-block size in bytes.
*/
#define STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size) \
    ((capacity) * STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(raw_block_size))

/*! \def     STK_BLOCKPOOL_STORAGE_DECL(name, capacity, raw_block_size)
    \brief   Declare a correctly sized and aligned external storage array.
    \details Expands to a \c static \c stk_word_t array with the required size and
             pointer-sized alignment. Intended for file-scope or function-scope use.
             \c STK_BLOCKPOOL_ALIGN_BLOCK_SIZE() guarantees the total byte count is
             always an exact multiple of \c sizeof(stk_word_t), so the integer
             division in the array dimension is lossless by construction.
    \param   name: C identifier for the array variable.
    \param   capacity: Number of blocks the pool will hold.
    \param   raw_block_size: Raw per-block size in bytes.

    \code
    STK_BLOCKPOOL_STORAGE_DECL(g_BufStorage, 16, sizeof(MyBuf));
    stk_blockpool_t *pool = stk_blockpool_create_static(
        16, sizeof(MyBuf), g_BufStorage, sizeof(g_BufStorage), NULL);
    \endcode
*/
#define STK_BLOCKPOOL_STORAGE_DECL(name, capacity, raw_block_size) \
    static stk_word_t name[STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size) / sizeof(stk_word_t)]

// =============================================================================
// Types
// =============================================================================

/*! \brief Opaque handle to a \c stk::memory::BlockMemoryPool instance.
*/
typedef struct stk_blockpool_t stk_blockpool_t;

// =============================================================================
// Lifecycle - heap storage
// =============================================================================

/*! \brief     Create a block pool backed by \b heap-allocated storage.
    \details   Allocates a flat byte buffer of
               \c capacity * AlignBlockSize(raw_block_size) bytes from the heap.
               Call \c stk_blockpool_is_storage_valid() immediately after creation
               when operating without exceptions (typical embedded configuration).
    \param[in] capacity: Total number of blocks.
    \param[in] raw_block_size: Requested per-block size in bytes.
    \param[in] name: Optional human-readable name (may be \c NULL).
               Forwarded to \c ITraceable::SetTraceName().
    \return    Pool handle, or \c NULL if the static slot pool is exhausted
               (\c STK_C_BLOCKPOOL_MAX reached).
               A non-NULL handle does \b not guarantee the backing storage was
               successfully heap-allocated; always call \c stk_blockpool_is_storage_valid()
               immediately after creation when operating without exceptions.
    \note      Not ISR-safe.
    \see       stk_blockpool_is_storage_valid(), stk_blockpool_destroy()
*/
stk_blockpool_t *stk_blockpool_create(size_t      capacity,
                                      size_t      raw_block_size,
                                      const char *name);

/*! \brief     Create a block pool backed by \b caller-supplied (external) storage.
    \details   The pool references \a storage directly without taking ownership.
               The caller must keep the buffer alive for the entire lifetime of
               the pool. The storage is \b not freed on destruction.
    \param[in] capacity: Total number of blocks the pool can hold.
    \param[in] raw_block_size: Requested per-block size in bytes.
    \param[in] storage: Pointer to a caller-owned byte buffer. Must be aligned to
               at least \c sizeof(void*) and large enough to hold
               at least \c STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size)
               bytes. Asserted at construction time.
    \param[in] storage_size: Size of \a storage in bytes (used for the size assertion).
    \param[in] name: Optional human-readable name (may be \c NULL).
    \return    Pool handle, or \c NULL if the static slot pool is exhausted
               (\c STK_C_BLOCKPOOL_MAX reached).
    \note      Not ISR-safe.
    \see       stk_blockpool_destroy()
*/
stk_blockpool_t *stk_blockpool_create_static(size_t      capacity,
                                             size_t      raw_block_size,
                                             uint8_t    *storage,
                                             size_t      storage_size,
                                             const char *name);

/*! \brief     Destroy a pool and return its slot to the static pool.
    \details   If the pool owns heap storage, it is freed. External storage is
               never touched.
    \param[in] pool: Pool handle obtained via \c stk_blockpool_create() or
               \c stk_blockpool_create_static().
    \warning   Destroying a pool while tasks are blocked in \c stk_blockpool_alloc()
               or \c stk_blockpool_timed_alloc() is a logic error and triggers an
               assertion in debug builds.
    \warning   Set the pool pointer to \c NULL after this call to prevent accidental
               use-after-destroy (the handle slot may be reused by a subsequent
               \c stk_blockpool_create() call).
    \note      Not ISR-safe.
*/
void stk_blockpool_destroy(stk_blockpool_t *pool);

// =============================================================================
// Allocation
// =============================================================================

/*! \brief     Allocate one block, blocking indefinitely until one is available.
    \param[in] pool: Pool handle.
    \return    Pointer to an uninitialized block of at least \a raw_block_size bytes.
               Never returns \c NULL.
    \warning   Not ISR-safe.
*/
void *stk_blockpool_alloc(stk_blockpool_t *pool);

/*! \brief     Allocate one block, blocking until one becomes available or the timeout expires.
    \param[in] pool: Pool handle.
    \param[in] timeout: Maximum time to wait in OS ticks.
               Pass \c STK_WAIT_INFINITE to block indefinitely (same as \c stk_blockpool_alloc()),
               or \c STK_NO_WAIT for a non-blocking attempt identical to \c stk_blockpool_try_alloc().
    \return    Pointer to an uninitialized block, or \c NULL if the timeout expired
               before a block became available.
    \note      If OS tick rate is 1 kHz then 1 tick = 1 ms. Use \c stk_ticks_from_ms()
               to convert a millisecond value to ticks for best portability across
               different tick rates.
    \warning   ISR-safe \b only when \a timeout = \c STK_NO_WAIT; not ISR-safe otherwise.
*/
void *stk_blockpool_timed_alloc(stk_blockpool_t *pool, stk_timeout_t timeout);

/*! \brief     Non-blocking allocation attempt.
    \details   Returns a block immediately if one is available, or \c NULL if the
               pool is empty. Never suspends the calling task.
    \param[in] pool: Pool handle.
    \return    Pointer to an uninitialized block, or \c NULL if the pool is empty.
    \note      ISR-safe.
*/
void *stk_blockpool_try_alloc(stk_blockpool_t *pool);

// =============================================================================
// Deallocation
// =============================================================================

/*! \brief     Return a previously allocated block to the pool.
    \details   Pushes the block back onto the free-list head in O(1) and wakes
               exactly one task blocked inside \c stk_blockpool_alloc() or
               \c stk_blockpool_timed_alloc(), if any.
    \param[in] pool: Pool handle.
    \param[in] ptr: Pointer previously returned by \c stk_blockpool_alloc(),
               \c stk_blockpool_timed_alloc(), or \c stk_blockpool_try_alloc().
               Must belong to \a pool. Bounds and alignment are validated;
               failures trigger an assertion in debug builds.
    \return    \c true  on success.
               \c false if \a ptr is \c NULL, out of range, or misaligned -
               each case indicates a caller logic error.
    \note      ISR-safe.
    \warning   Null the pointer after \c stk_blockpool_free() to prevent double-free.
*/
bool stk_blockpool_free(stk_blockpool_t *pool, void *ptr);

// =============================================================================
// Query
// =============================================================================

/*! \brief     Verify that the backing storage is valid and the pool is ready for use.
    \details   Always \c true for pools created with \c stk_blockpool_create_static().
               For heap-constructed pools (\c stk_blockpool_create()), \c false if
               \c operator new failed. Must be checked after heap construction when
               operating without exceptions.
    \param[in] pool: Pool handle.
    \return    \c true if the pool is ready for use.
    \note      ISR-safe.
*/
bool stk_blockpool_is_storage_valid(const stk_blockpool_t *pool);

/*! \brief     Get the total block capacity of the pool.
    \param[in] pool: Pool handle.
    \return    Maximum number of blocks that can be simultaneously allocated.
    \note      ISR-safe.
*/
size_t stk_blockpool_get_capacity(const stk_blockpool_t *pool);

/*! \brief     Get the aligned block size used internally by the allocator.
    \details   Equal to \c STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(raw_block_size) as passed
               at construction. Always >= \c STK_BLOCKPOOL_ALIGN.
    \param[in] pool: Pool handle.
    \return    Aligned block size in bytes.
    \note      ISR-safe.
*/
size_t stk_blockpool_get_block_size(const stk_blockpool_t *pool);

/*! \brief     Get the number of currently allocated (outstanding) blocks.
    \param[in] pool: Pool handle.
    \return    Point-in-time snapshot. May be stale immediately after return in a
               multi-task environment.
    \note      ISR-safety depends on target ABI: the counter is a 16-bit value, so a
               single-instruction atomic read is guaranteed on 32-bit Cortex-M (aligned
               halfword load) but not on 8-bit targets where two bus cycles may be needed.
               Treat the result as advisory in all multi-core or 8-bit contexts.
*/
size_t stk_blockpool_get_used_count(const stk_blockpool_t *pool);

/*! \brief     Get the number of free (available) blocks.
    \param[in] pool: Pool handle.
    \return    Point-in-time snapshot of capacity - used_count.
    \note      ISR-safe.
*/
size_t stk_blockpool_get_free_count(const stk_blockpool_t *pool);

/*! \brief     Check whether all blocks are currently allocated (pool exhausted).
    \param[in] pool: Pool handle.
    \return    \c true if no blocks are available for allocation.
    \note      ISR-safe.
*/
bool stk_blockpool_is_full(const stk_blockpool_t *pool);

/*! \brief     Check whether all blocks are free (no outstanding allocations).
    \param[in] pool: Pool handle.
    \return    \c true if no blocks are currently allocated.
    \note      ISR-safe.
*/
bool stk_blockpool_is_empty(const stk_blockpool_t *pool);

#ifdef __cplusplus
namespace stk { namespace memory { class BlockMemoryPool; } }

/*! \brief     Get the underlying C++ \c stk::memory::BlockMemoryPool object wrapped by a \c stk_blockpool_t handle.
    \param[in] pool: Pool handle obtained via \c stk_blockpool_create() or
               \c stk_blockpool_create_static().
    \return    Pointer to the wrapped \c stk::memory::BlockMemoryPool instance. Never \c NULL for a valid handle.
    \note      C++ callers only (guarded by \c __cplusplus); not part of the C ABI.
*/
extern "C" stk::memory::BlockMemoryPool *stk_blockpool_get_instance(stk_blockpool_t *pool);
#endif // __cplusplus

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* STK_C_MEMORY_H_ */
