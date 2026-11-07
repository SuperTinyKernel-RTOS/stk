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
    \brief    C language bindings for stk::memory::BlockMemoryPool, stk::memory::BytePool and
              stk::memory::MemoryAllocator.

    \c stk_blockpool_t instances are allocated from a static pool of
    \c STK_C_BLOCKPOOL_MAX slots.  Obtain a handle with \c stk_blockpool_create()
    or \c stk_blockpool_create_static() and release it with \c stk_blockpool_destroy().

    Two storage modes are exposed, mirroring the C++ class:

    | Mode             | Function                       | Who owns storage?       |
    |------------------|--------------------------------|-------------------------|
    | Heap storage     | stk_blockpool_create()         | Pool (freed on destroy) |
    | External storage | stk_blockpool_create_static()  | Caller                  |

    The \c _ex variants additionally take an explicit block alignment (cache-line / DMA / SIMD buffers).

    For zero-heap deployments, declare a storage buffer with the helper macro
    \c STK_BLOCKPOOL_STORAGE_DECL() and pass it to \c stk_blockpool_create_static():

    \code
    #define PKT_COUNT  8U
    #define PKT_SIZE   sizeof(Packet)

    STK_BLOCKPOOL_STORAGE_DECL(g_PktStorage, PKT_COUNT, PKT_SIZE);

    stk_blockpool_t *pool = stk_blockpool_create_static(
        PKT_COUNT, PKT_SIZE,
        (uint8_t *)g_PktStorage, sizeof(g_PktStorage), "pkt_pool");

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

    \c stk_bytepool_t (variable-size, first-fit allocator with coalescing) follows the
    same pattern with \c STK_C_BYTEPOOL_MAX slots, \c stk_bytepool_create(),
    \c stk_bytepool_create_static() and \c stk_bytepool_destroy():

    \code
    STK_BYTEPOOL_STORAGE_DECL(g_Heap, STK_BYTEPOOL_REQUIRED_SIZE(128) * 4 + STK_BYTEPOOL_REQUIRED_SIZE(512));

    stk_bytepool_t *bp = stk_bytepool_create_static((uint8_t *)g_Heap, sizeof(g_Heap), "msg_pool");

    void Task_Producer(void) {
        char *msg = (char *)stk_bytepool_timed_alloc(bp, 64, stk_ticks_from_ms(100));
        if (msg) { Format(msg, 64); Queue_Write(msg); }
    }

    void Task_Consumer(void) {
        void *msg = NULL;
        if (Queue_Read(&msg)) { Send((const char *)msg); stk_bytepool_free(bp, msg); }
    }
    \endcode

    The general-purpose allocator (\c stk::memory::MemoryAllocator) is exposed through the
    \c stk_mem_*() functions.

    \note The \c STK_MEMORY_* configuration macros (block/byte pool alignment, bitmap, guard word, ...) change the
          layout and sizing rules of the C++ pools. They are mirrored below with identical defaults and are
          verified against the C++ classes at compile time in stk_c_memory.cpp. Set them in the build system so
          that every translation unit (C and C++) sees the same values.

    \defgroup c_api_memory STK C Memory API
    \brief    Pure C interface for stk::memory::BlockMemoryPool, stk::memory::BytePool and stk::memory::MemoryAllocator.
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

/*! \def     STK_C_BYTEPOOL_MAX
    \brief   Maximum number of concurrent \c stk_bytepool_t instances (default: 8).
    \note    Increase if your application needs more simultaneous pools.
*/
#ifndef STK_C_BYTEPOOL_MAX
    #define STK_C_BYTEPOOL_MAX (8U)
#endif

/*! \def     STK_C_MEMORY_DEFAULT_ALLOCATOR
    \brief   Selects the implementation of \c stk::memory::MemoryAllocator provided by the C binding
             (default: 0).
             - \c 0: wraps the C library \c malloc() / \c free() (the lock hooks of the C library must be
                     implemented, e.g. \c __malloc_lock for newlib). Statistics are not available (all zero).
             - \c 1: uses the deterministic, ISR-safe allocator on top of a static BytePool
                     (stk_memory_allocator_default.h), heap size is \c STK_MEMORY_DEFAULT_HEAP_SIZE.
    \note    Has no effect if \c STK_MEMORY_NO_HEAP is 1 (no allocator is provided then).
*/
#ifndef STK_C_MEMORY_DEFAULT_ALLOCATOR
    #define STK_C_MEMORY_DEFAULT_ALLOCATOR (0)
#endif

// -----------------------------------------------------------------------------
// Mirrors of the C++ configuration macros (same names, same defaults).
// -----------------------------------------------------------------------------

/*! \def     STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT
    \brief   Default block alignment of a block pool (see stk_memory_blockpool.h).
*/
#ifndef STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT
    #ifdef _STK_ARCH_RISC_V
        #define STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT (16U)
    #else
        #define STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT (8U)
    #endif
#endif

/*! \def     STK_MEMORY_BLOCKPOOL_BITMAP
    \brief   When 1, a block pool keeps an "allocated" bit per block (O(1) double-free detection in release builds)
             and its storage requirement grows by \c capacity/8 bytes (see stk_memory_blockpool.h).
*/
#ifndef STK_MEMORY_BLOCKPOOL_BITMAP
    #define STK_MEMORY_BLOCKPOOL_BITMAP (0)
#endif

/*! \def     STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT
    \brief   Payload alignment of byte pool allocations (see stk_memory_bytepool.h).
*/
#ifndef STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT
    #ifdef _STK_ARCH_RISC_V
        #define STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT (16U)
    #else
        #define STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT (8U)
    #endif
#endif

/*! \def     STK_MEMORY_BYTEPOOL_GUARD
    \brief   When 1, every byte pool allocation carries a 4-byte overrun guard word (see stk_memory_bytepool.h).
*/
#ifndef STK_MEMORY_BYTEPOOL_GUARD
    #define STK_MEMORY_BYTEPOOL_GUARD (0)
#endif

// =============================================================================
// BlockPool storage helper macros
// =============================================================================

/*! \def     STK_BLOCKPOOL_ALIGN
    \brief   Default block alignment in bytes (equals \c STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT,
             i.e. \c BlockMemoryPool::ALIGN_DEFAULT).
*/
#define STK_BLOCKPOOL_ALIGN (STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT)

/*! \def     STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX(raw_size, align)
    \brief   Compute the internally aligned block size for a given raw byte size and alignment.
    \details Rounds \a raw_size up to the nearest multiple of \a align, with a minimum of \a align.
             Matches \c BlockMemoryPool::AlignBlockSize().
*/
#define STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX(raw_size, align) \
    (((raw_size) < (align)) \
        ? (align) \
        : (((raw_size) + ((align) - 1U)) & ~((align) - 1U)))

/*! \def     STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(raw_size)
    \brief   Same as \c STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX() with the default alignment.
*/
#define STK_BLOCKPOOL_ALIGN_BLOCK_SIZE(raw_size) \
    STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX(raw_size, STK_BLOCKPOOL_ALIGN)

/*! \def     STK_BLOCKPOOL_BITMAP_BYTES(capacity)
    \brief   Size in bytes of the "allocated" bitmap of a pool of \a capacity blocks
             (0 unless \c STK_MEMORY_BLOCKPOOL_BITMAP is 1). Matches \c BlockMemoryPool::BitmapBytes().
*/
#define STK_BLOCKPOOL_BITMAP_BYTES(capacity) \
    ((STK_MEMORY_BLOCKPOOL_BITMAP != 0) ? (((capacity) + 7U) / 8U) : 0U)

/*! \def     STK_BLOCKPOOL_STORAGE_SIZE_EX(capacity, raw_block_size, align)
    \brief   Compute the minimum external storage buffer size in bytes (blocks plus the optional bitmap).
             Matches \c BlockMemoryPool::RequiredStorageSize().
    \param   capacity: Number of blocks the pool will hold.
    \param   raw_block_size: Raw per-block size in bytes.
    \param   align: Block alignment in bytes (power of two, not less than sizeof(void *)).
    \note    If the buffer start can not be guaranteed to be aligned add <tt>align - 1</tt>.
*/
#define STK_BLOCKPOOL_STORAGE_SIZE_EX(capacity, raw_block_size, align) \
    (((capacity) * STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX(raw_block_size, align)) + STK_BLOCKPOOL_BITMAP_BYTES(capacity))

/*! \def     STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size)
    \brief   Same as \c STK_BLOCKPOOL_STORAGE_SIZE_EX() with the default alignment.
*/
#define STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size) \
    STK_BLOCKPOOL_STORAGE_SIZE_EX(capacity, raw_block_size, STK_BLOCKPOOL_ALIGN)

/*! \def     STK_BLOCKPOOL_STORAGE_DECL_EX(name, capacity, raw_block_size, align)
    \brief   Declare a correctly sized storage array aligned to \a align bytes.
    \details Expands to a \c static \c stk_word_t array with the required size (rounded up to whole words)
             and the requested alignment attribute. Intended for file-scope or function-scope use.
    \param   name: C identifier for the array variable.
    \param   capacity: Number of blocks the pool will hold.
    \param   raw_block_size: Raw per-block size in bytes.
    \param   align: Block alignment in bytes.
*/
#define STK_BLOCKPOOL_STORAGE_DECL_EX(name, capacity, raw_block_size, align) \
    static stk_word_t name[STK_C_CEIL_DIV(STK_BLOCKPOOL_STORAGE_SIZE_EX(capacity, raw_block_size, align), sizeof(stk_word_t))] \
        __stk_c_align(align)

/*! \def     STK_BLOCKPOOL_STORAGE_DECL(name, capacity, raw_block_size)
    \brief   Declare a correctly sized and aligned external storage array (default alignment).
    \param   name: C identifier for the array variable.
    \param   capacity: Number of blocks the pool will hold.
    \param   raw_block_size: Raw per-block size in bytes.

    \code
    STK_BLOCKPOOL_STORAGE_DECL(g_BufStorage, 16, sizeof(MyBuf));
    stk_blockpool_t *pool = stk_blockpool_create_static(
        16, sizeof(MyBuf), (uint8_t *)g_BufStorage, sizeof(g_BufStorage), NULL);
    \endcode
*/
#define STK_BLOCKPOOL_STORAGE_DECL(name, capacity, raw_block_size) \
    STK_BLOCKPOOL_STORAGE_DECL_EX(name, capacity, raw_block_size, STK_BLOCKPOOL_ALIGN)

// =============================================================================
// BytePool storage helper macros
// =============================================================================

/*! \def     STK_BYTEPOOL_ALIGN
    \brief   Payload alignment and block size granularity of a byte pool, in bytes
             (equals \c STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT: 8, 16 on RISC-V by default).
*/
#define STK_BYTEPOOL_ALIGN (STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT)

/*! \def     STK_BYTEPOOL_HEADER_SIZE
    \brief   Per-block header size in bytes: <tt>max(8, STK_BYTEPOOL_ALIGN)</tt>. Every allocation consumes
             this many bytes of the pool in addition to its aligned payload.
*/
#define STK_BYTEPOOL_HEADER_SIZE ((STK_BYTEPOOL_ALIGN > 8U) ? STK_BYTEPOOL_ALIGN : 8U)

/*! \def     STK_BYTEPOOL_GUARD_SIZE
    \brief   Size of the overrun guard word of a used block (4 with \c STK_MEMORY_BYTEPOOL_GUARD, otherwise 0).
*/
#define STK_BYTEPOOL_GUARD_SIZE ((STK_MEMORY_BYTEPOOL_GUARD != 0) ? 4U : 0U)

/*! \def     STK_BYTEPOOL_ALIGN_SIZE(raw_size)
    \brief   Round \a raw_size up to the nearest multiple of \c STK_BYTEPOOL_ALIGN.
*/
#define STK_BYTEPOOL_ALIGN_SIZE(raw_size) \
    (((raw_size) + (STK_BYTEPOOL_ALIGN - 1U)) & ~(STK_BYTEPOOL_ALIGN - 1U))

/*! \def     STK_BYTEPOOL_REQUIRED_SIZE(payload_size)
    \brief   Pool space in bytes consumed by one allocation of \a payload_size bytes
             (aligned payload, plus the guard word if enabled, plus the block header).
             Sum the value for every block that must be alive simultaneously to size external storage.
             Matches \c BytePool::RequiredSize().
*/
#define STK_BYTEPOOL_REQUIRED_SIZE(payload_size) \
    (STK_BYTEPOOL_ALIGN_SIZE((payload_size) + STK_BYTEPOOL_GUARD_SIZE) + STK_BYTEPOOL_HEADER_SIZE)

/*! \def     STK_BYTEPOOL_STORAGE_DECL(name, size_bytes)
    \brief   Declare an external storage array for a byte pool, aligned to \c STK_BYTEPOOL_ALIGN.
    \details Expands to a \c static \c uint64_t array (so that the start address is at least 8-byte
             aligned on every target) with the alignment attribute \c STK_BYTEPOOL_ALIGN. Use
             \c sizeof(name) as the \a storage_size argument.
    \param   name: C identifier for the array variable.
    \param   size_bytes: Required pool size in bytes (headers included), e.g. a sum of
             \c STK_BYTEPOOL_REQUIRED_SIZE() values.
*/
#define STK_BYTEPOOL_STORAGE_DECL(name, size_bytes) \
    static uint64_t name[((size_bytes) + (sizeof(uint64_t) - 1U)) / sizeof(uint64_t)] \
        __stk_c_align(STK_BYTEPOOL_ALIGN)

// =============================================================================
// Types
// =============================================================================

/*! \brief Opaque handle to a \c stk::memory::BlockMemoryPool instance.
*/
typedef struct stk_blockpool_t stk_blockpool_t;

/*! \brief Opaque handle to a \c stk::memory::BytePool instance.
*/
typedef struct stk_bytepool_t stk_bytepool_t;

/*! \brief     Snapshot of allocator statistics.
    \note      Layout-compatible in meaning with \c stk::memory::MemoryAllocator::Stats.
               Byte counters of pools are in bytes (BlockMemoryPool: blocks * block size;
               BytePool: used blocks including their headers).
    \see       stk_blockpool_get_stats(), stk_bytepool_get_stats(), stk_mem_get_stats()
*/
typedef struct stk_mem_stats_t
{
    size_t capacity;         //!< Total capacity in bytes.
    size_t allocated;        //!< Bytes currently allocated.
    size_t peak_allocated;   //!< High watermark of allocated bytes.
    size_t allocate_count;   //!< Number of successful allocations.
    size_t free_count;       //!< Number of successful frees.
    size_t alloc_fail_count; //!< Number of allocation requests that could not be satisfied.
    size_t min_ever_free;    //!< Minimum free bytes recorded since the start (watermark).
} stk_mem_stats_t;

// =============================================================================
// BlockPool - lifecycle
// =============================================================================

/*! \brief     Create a block pool backed by \b heap-allocated storage (default alignment).
    \details   Allocates a flat byte buffer of
               \c STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size) bytes (plus alignment slack)
               via the memory allocator.
               Call \c stk_blockpool_is_storage_valid() immediately after creation
               when operating without exceptions (typical embedded configuration).
    \param[in] capacity: Total number of blocks.
    \param[in] raw_block_size: Requested per-block size in bytes.
    \param[in] name: Optional human-readable name (may be \c NULL).
               Forwarded to \c ITraceable::SetTraceName().
    \return    Pool handle, or \c NULL if the static slot pool is exhausted
               (\c STK_C_BLOCKPOOL_MAX reached) or heap support is compiled out (\c STK_MEMORY_NO_HEAP).
               A non-NULL handle does \b not guarantee the backing storage was
               successfully heap-allocated; always call \c stk_blockpool_is_storage_valid()
               immediately after creation when operating without exceptions.
    \note      Not ISR-safe.
    \see       stk_blockpool_create_ex(), stk_blockpool_is_storage_valid(), stk_blockpool_destroy()
*/
stk_blockpool_t *stk_blockpool_create(size_t      capacity,
                                      size_t      raw_block_size,
                                      const char *name);

/*! \brief     Create a block pool backed by \b heap-allocated storage with an explicit block alignment.
    \param[in] capacity: Total number of blocks.
    \param[in] raw_block_size: Requested per-block size in bytes.
    \param[in] name: Optional human-readable name (may be \c NULL).
    \param[in] alignment: Block alignment in bytes, a power of two not less than \c sizeof(void *).
    \return    See \c stk_blockpool_create(). An invalid alignment results in an invalid pool
               (check \c stk_blockpool_is_storage_valid()).
    \note      Not ISR-safe.
*/
stk_blockpool_t *stk_blockpool_create_ex(size_t      capacity,
                                         size_t      raw_block_size,
                                         const char *name,
                                         size_t      alignment);

/*! \brief     Create a block pool backed by \b caller-supplied (external) storage (default alignment).
    \details   The pool references \a storage directly without taking ownership.
               The caller must keep the buffer alive for the entire lifetime of
               the pool. The storage is \b not freed on destruction. A misaligned start address is
               corrected internally (the unaligned head bytes are not used) and the capacity is trimmed
               to what fits (asserted in debug builds).
    \param[in] capacity: Total number of blocks the pool can hold.
    \param[in] raw_block_size: Requested per-block size in bytes.
    \param[in] storage: Pointer to a caller-owned byte buffer. Should be aligned to
               \c STK_BLOCKPOOL_ALIGN and large enough to hold
               at least \c STK_BLOCKPOOL_STORAGE_SIZE(capacity, raw_block_size)
               bytes. Asserted at construction time.
    \param[in] storage_size: Size of \a storage in bytes.
    \param[in] name: Optional human-readable name (may be \c NULL).
    \return    Pool handle, or \c NULL if the static slot pool is exhausted
               (\c STK_C_BLOCKPOOL_MAX reached).
    \note      Not ISR-safe.
    \see       stk_blockpool_create_static_ex(), stk_blockpool_destroy()
*/
stk_blockpool_t *stk_blockpool_create_static(size_t      capacity,
                                             size_t      raw_block_size,
                                             uint8_t    *storage,
                                             size_t      storage_size,
                                             const char *name);

/*! \brief     Create a block pool backed by \b caller-supplied storage with an explicit block alignment.
    \param[in] capacity: Total number of blocks the pool can hold.
    \param[in] raw_block_size: Requested per-block size in bytes.
    \param[in] storage: Caller-owned byte buffer, at least
               \c STK_BLOCKPOOL_STORAGE_SIZE_EX(capacity, raw_block_size, alignment) bytes.
    \param[in] storage_size: Size of \a storage in bytes.
    \param[in] name: Optional human-readable name (may be \c NULL).
    \param[in] alignment: Block alignment in bytes, a power of two not less than \c sizeof(void *).
    \return    Pool handle, or \c NULL if the static slot pool is exhausted.
    \note      Not ISR-safe.
*/
stk_blockpool_t *stk_blockpool_create_static_ex(size_t      capacity,
                                                size_t      raw_block_size,
                                                uint8_t    *storage,
                                                size_t      storage_size,
                                                const char *name,
                                                size_t      alignment);

/*! \brief     Destroy a pool and return its slot to the static pool.
    \details   If the pool owns heap storage, it is freed. External storage is
               never touched.
    \param[in] pool: Pool handle obtained via one of the \c stk_blockpool_create*() functions.
    \warning   Destroying a pool while tasks are blocked in \c stk_blockpool_alloc()
               or \c stk_blockpool_timed_alloc() is a logic error and triggers an
               assertion in debug builds. All blocks must be returned before destruction.
    \warning   Set the pool pointer to \c NULL after this call to prevent accidental
               use-after-destroy (the handle slot may be reused by a subsequent
               \c stk_blockpool_create() call).
    \note      Not ISR-safe.
*/
void stk_blockpool_destroy(stk_blockpool_t *pool);

// =============================================================================
// BlockPool - allocation
// =============================================================================

/*! \brief     Allocate one block, blocking indefinitely until one is available.
    \param[in] pool: Pool handle.
    \return    Pointer to an uninitialized block of at least \a raw_block_size bytes.
               Returns \c NULL only if the wait was cancelled (via \c IKernel::CancelTaskWait())
               or the pool is invalid (see \c stk_blockpool_is_storage_valid()); never on timeout.
               Use \c stk_blockpool_timed_alloc_ex() to tell these apart.
    \warning   Not ISR-safe.
*/
void *stk_blockpool_alloc(stk_blockpool_t *pool);

/*! \brief     Allocate one block, blocking until one becomes available or the timeout expires.
    \param[in] pool: Pool handle.
    \param[in] timeout: Maximum time to wait in OS ticks.
               Pass \c STK_WAIT_INFINITE to block indefinitely (same as \c stk_blockpool_alloc()),
               or \c STK_NO_WAIT for a non-blocking attempt identical to \c stk_blockpool_try_alloc().
    \return    Pointer to an uninitialized block, or \c NULL if the timeout expired
               before a block became available, the wait was cancelled or the pool is invalid.
    \note      Collapses timeout and cancellation. Use \c stk_blockpool_timed_alloc_ex() to tell them apart.
    \note      If OS tick rate is 1 kHz then 1 tick = 1 ms. Use \c stk_ticks_from_ms()
               to convert a millisecond value to ticks for best portability across
               different tick rates.
    \warning   ISR-safe \b only when \a timeout = \c STK_NO_WAIT; not ISR-safe otherwise.
*/
void *stk_blockpool_timed_alloc(stk_blockpool_t *pool, stk_timeout_t timeout);

/*! \brief     Allocate one block, preserving the full wait outcome.
    \param[in]  pool: Pool handle.
    \param[out] block: Receives the allocated block on \c STK_WAIT_RESULT_SIGNAL, \c NULL on any other
                result. Must not be \c NULL.
    \param[in]  timeout: Maximum time to wait in OS ticks (see \c stk_blockpool_timed_alloc()).
    \return     \c STK_WAIT_RESULT_SIGNAL   - a block was allocated,
                \c STK_WAIT_RESULT_TIMEOUT  - no block became available before the timeout
                                              (or \c STK_NO_WAIT on an empty pool),
                \c STK_WAIT_RESULT_CANCELED - the wait was cancelled, nothing was allocated,
                \c STK_WAIT_RESULT_FAIL     - invalid arguments, invalid pool or the kernel rejected the wait.
    \warning    ISR-safe \b only when \a timeout = \c STK_NO_WAIT; not ISR-safe otherwise.
*/
stk_wait_result_t stk_blockpool_timed_alloc_ex(stk_blockpool_t *pool, void **block, stk_timeout_t timeout);

/*! \brief     Non-blocking allocation attempt.
    \details   Returns a block immediately if one is available, or \c NULL if the
               pool is empty. Never suspends the calling task.
    \param[in] pool: Pool handle.
    \return    Pointer to an uninitialized block, or \c NULL if the pool is empty.
    \note      ISR-safe.
*/
void *stk_blockpool_try_alloc(stk_blockpool_t *pool);

// =============================================================================
// BlockPool - deallocation
// =============================================================================

/*! \brief     Return a previously allocated block to the pool.
    \details   Pushes the block back onto the free-list head in O(1) and wakes
               exactly one task blocked inside \c stk_blockpool_alloc() or
               \c stk_blockpool_timed_alloc(), if any.
    \param[in] pool: Pool handle.
    \param[in] ptr: Pointer previously returned by an allocation function of this pool.
               Bounds and alignment are validated; failures trigger an assertion in debug builds.
    \return    \c true  on success.
               \c false if \a ptr is \c NULL, out of range, misaligned or already free
               (double-free; release-build detection requires \c STK_MEMORY_BLOCKPOOL_BITMAP) -
               each case indicates a caller logic error.
    \note      ISR-safe.
    \warning   Null the pointer after \c stk_blockpool_free() to prevent double-free.
*/
bool stk_blockpool_free(stk_blockpool_t *pool, void *ptr);

// =============================================================================
// BlockPool - query
// =============================================================================

/*! \brief     Verify that the backing storage is valid and the pool is ready for use.
    \details   Always \c true for pools created with a sufficiently large external buffer.
               For heap-constructed pools, \c false if the heap allocation failed. \c false as well
               if the arguments were invalid or the buffer is too small for a single block.
    \param[in] pool: Pool handle.
    \return    \c true if the pool is ready for use.
    \note      ISR-safe.
*/
bool stk_blockpool_is_storage_valid(const stk_blockpool_t *pool);

/*! \brief     Get the total block capacity of the pool.
    \param[in] pool: Pool handle.
    \return    Maximum number of blocks that can be simultaneously allocated. Can be lower than requested
               if the supplied buffer was too small, 0 for an invalid pool.
    \note      ISR-safe.
*/
size_t stk_blockpool_get_capacity(const stk_blockpool_t *pool);

/*! \brief     Get the aligned block size used internally by the allocator.
    \details   Equal to \c STK_BLOCKPOOL_ALIGN_BLOCK_SIZE_EX(raw_block_size, alignment) as passed
               at construction. Always >= the pool alignment.
    \param[in] pool: Pool handle.
    \return    Aligned block size in bytes.
    \note      ISR-safe.
*/
size_t stk_blockpool_get_block_size(const stk_blockpool_t *pool);

/*! \brief     Get the alignment of every block of the pool.
    \param[in] pool: Pool handle.
    \return    Alignment in bytes.
    \note      ISR-safe.
*/
size_t stk_blockpool_get_block_alignment(const stk_blockpool_t *pool);

/*! \brief     Get the number of currently allocated (outstanding) blocks.
    \param[in] pool: Pool handle.
    \return    Point-in-time snapshot. May be stale immediately after return in a
               multi-task environment.
    \note      ISR-safe on targets where an aligned size_t read is a single instruction.
*/
size_t stk_blockpool_get_used_count(const stk_blockpool_t *pool);

/*! \brief     Get the number of free (available) blocks.
    \param[in] pool: Pool handle.
    \return    Point-in-time snapshot of capacity - used_count.
    \note      ISR-safe.
*/
size_t stk_blockpool_get_free_count(const stk_blockpool_t *pool);

/*! \brief     Get the high watermark: the maximum number of blocks allocated at the same time.
    \details   Run your worst-case workload and use this value to size the pool.
    \param[in] pool: Pool handle.
    \note      ISR-safe on targets where an aligned size_t read is a single instruction.
*/
size_t stk_blockpool_get_peak_used_count(const stk_blockpool_t *pool);

/*! \brief     Get the number of allocation requests that failed because the pool was exhausted
               (timeouts and \c STK_NO_WAIT misses).
    \param[in] pool: Pool handle.
    \note      ISR-safe on targets where an aligned size_t read is a single instruction.
*/
size_t stk_blockpool_get_alloc_fail_count(const stk_blockpool_t *pool);

/*! \brief     Get a consistent statistics snapshot (bytes: blocks * block size).
    \param[in]  pool: Pool handle.
    \param[out] stats: Receives the snapshot. Must not be \c NULL.
    \note      ISR-safe.
*/
void stk_blockpool_get_stats(const stk_blockpool_t *pool, stk_mem_stats_t *stats);

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

// =============================================================================
// BytePool - lifecycle
// =============================================================================

/*! \brief     Create a byte pool backed by \b heap-allocated storage.
    \details   Allocates \a storage_size bytes (plus alignment slack) via the memory allocator. The size
               includes block headers (see \c STK_BYTEPOOL_REQUIRED_SIZE()). Call
               \c stk_bytepool_is_storage_valid() immediately after creation when operating without exceptions.
    \param[in] storage_size: Size of the pool area in bytes (headers included).
    \param[in] name: Optional human-readable name (may be \c NULL).
    \return    Pool handle, or \c NULL if the static slot pool is exhausted
               (\c STK_C_BYTEPOOL_MAX reached) or heap support is compiled out (\c STK_MEMORY_NO_HEAP).
               A non-NULL handle does \b not guarantee that the heap allocation succeeded;
               check \c stk_bytepool_is_storage_valid().
    \note      Not ISR-safe.
    \see       stk_bytepool_is_storage_valid(), stk_bytepool_destroy()
*/
stk_bytepool_t *stk_bytepool_create(size_t storage_size, const char *name);

/*! \brief     Create a byte pool backed by \b caller-supplied (external) storage.
    \details   The pool references \a storage directly without taking ownership. The caller
               must keep the buffer alive for the entire lifetime of the pool. A misaligned
               start address is corrected internally (unaligned head/tail bytes are unused).
    \param[in] storage: Pointer to a caller-owned byte buffer, preferably aligned to
               \c STK_BYTEPOOL_ALIGN (see \c STK_BYTEPOOL_STORAGE_DECL()).
    \param[in] storage_size: Size of \a storage in bytes. Must be at least
               <tt>STK_BYTEPOOL_HEADER_SIZE + STK_BYTEPOOL_ALIGN</tt> (asserted).
    \param[in] name: Optional human-readable name (may be \c NULL).
    \return    Pool handle, or \c NULL if the static slot pool is exhausted
               (\c STK_C_BYTEPOOL_MAX reached).
    \note      Not ISR-safe.
    \see       stk_bytepool_destroy()
*/
stk_bytepool_t *stk_bytepool_create_static(uint8_t    *storage,
                                           size_t      storage_size,
                                           const char *name);

/*! \brief     Destroy a byte pool and return its slot to the static pool.
    \details   Heap storage is freed if owned. External storage is never touched.
    \param[in] pool: Handle obtained via \c stk_bytepool_create() or
               \c stk_bytepool_create_static().
    \warning   Destroying a pool while tasks are blocked in \c stk_bytepool_alloc() or
               \c stk_bytepool_timed_alloc() is a logic error. All blocks must be returned before destruction.
    \warning   Set the pool pointer to \c NULL after this call; the slot may be reused.
    \note      Not ISR-safe.
*/
void stk_bytepool_destroy(stk_bytepool_t *pool);

// =============================================================================
// BytePool - allocation
// =============================================================================

/*! \brief     Allocate a block, blocking indefinitely until enough contiguous memory is available.
    \param[in] pool: Pool handle.
    \param[in] size: Requested payload size in bytes (> 0 and <= \c stk_bytepool_get_max_request()).
    \return    Pointer to an uninitialized block aligned to \c STK_BYTEPOOL_ALIGN, or \c NULL
               if the request is invalid (zero, larger than the pool) or the wait was cancelled.
               Never returns \c NULL because of a timeout.
    \warning   Not ISR-safe.
*/
void *stk_bytepool_alloc(stk_bytepool_t *pool, size_t size);

/*! \brief     Allocate a block, blocking until memory becomes available or the timeout expires.
    \details   The timeout is a total budget, not per wake-up.
    \param[in] pool: Pool handle.
    \param[in] size: Requested payload size in bytes.
    \param[in] timeout: Maximum time to wait in OS ticks. \c STK_WAIT_INFINITE blocks
               indefinitely; \c STK_NO_WAIT is identical to \c stk_bytepool_try_alloc().
    \return    Pointer to an uninitialized block, or \c NULL on timeout, cancellation or
               invalid arguments. Use \c stk_bytepool_timed_alloc_ex() to tell them apart.
    \warning   ISR-safe \b only when \a timeout = \c STK_NO_WAIT.
*/
void *stk_bytepool_timed_alloc(stk_bytepool_t *pool, size_t size, stk_timeout_t timeout);

/*! \brief     Allocate a block, preserving the full wait outcome.
    \param[in]  pool: Pool handle.
    \param[out] block: Receives the allocated block on \c STK_WAIT_RESULT_SIGNAL, \c NULL on any other
                result. Must not be \c NULL.
    \param[in]  size: Requested payload size in bytes.
    \param[in]  timeout: Maximum time to wait in OS ticks (see \c stk_bytepool_timed_alloc()).
    \return     \c STK_WAIT_RESULT_SIGNAL   - a block was allocated (immediately or after waiting),
                \c STK_WAIT_RESULT_TIMEOUT  - no suitable block became available before the timeout
                                              (or \c STK_NO_WAIT was passed),
                \c STK_WAIT_RESULT_CANCELED - the wait was cancelled,
                \c STK_WAIT_RESULT_FAIL     - invalid arguments (\a block is \c NULL, \a size is 0 or exceeds
                                              \c stk_bytepool_get_max_request(), no valid storage) or the
                                              kernel rejected the wait.
    \warning    ISR-safe \b only when \a timeout = \c STK_NO_WAIT.
*/
stk_wait_result_t stk_bytepool_timed_alloc_ex(stk_bytepool_t *pool, void **block, size_t size, stk_timeout_t timeout);

/*! \brief     Non-blocking allocation attempt.
    \param[in] pool: Pool handle.
    \param[in] size: Requested payload size in bytes.
    \return    Pointer to an uninitialized block, or \c NULL if no suitable free block exists.
    \note      ISR-safe, but execution time is proportional to the number of blocks scanned.
*/
void *stk_bytepool_try_alloc(stk_bytepool_t *pool, size_t size);

// =============================================================================
// BytePool - deallocation
// =============================================================================

/*! \brief     Return a previously allocated block to the pool.
    \details   Marks the block free, merges it with following free blocks and wakes all tasks
               blocked in \c stk_bytepool_alloc() / \c stk_bytepool_timed_alloc().
    \param[in] pool: Pool handle.
    \param[in] ptr: Pointer previously returned by an allocation function of this pool.
    \return    \c true on success; \c false if \a ptr is \c NULL, outside the pool, misaligned,
               not in use (double-free) or - with \c STK_MEMORY_BYTEPOOL_GUARD - its guard word was
               overwritten (the block is kept, i.e. leaked, to protect the pool metadata).
    \note      ISR-safe, but execution time is proportional to the number of blocks scanned.
    \warning   Null the pointer after \c stk_bytepool_free() to prevent double-free.
*/
bool stk_bytepool_free(stk_bytepool_t *pool, void *ptr);

// =============================================================================
// BytePool - query
// =============================================================================

/*! \brief     Collect pool statistics (mutually consistent snapshot).
    \param[in]  pool: Pool handle.
    \param[out] available: Total allocatable payload bytes over all free fragments
                (may be \c NULL).
    \param[out] largest: Payload size of the largest free fragment, i.e. the biggest request
                that would succeed right now (may be \c NULL).
    \param[out] fragments: Number of free fragments; 1 means no external fragmentation
                (may be \c NULL).
    \note      ISR-safe, but O(n) in the number of blocks. See \c stk_bytepool_get_stats() for an O(1) alternative.
*/
void stk_bytepool_get_info(const stk_bytepool_t *pool,
                           size_t               *available,
                           size_t               *largest,
                           size_t               *fragments);

/*! \brief     Get an O(1) statistics snapshot.
    \details   \c capacity is the managed area size and \c allocated the bytes held by used blocks, both
               including headers (the real pool consumption).
    \param[in]  pool: Pool handle.
    \param[out] stats: Receives the snapshot. Must not be \c NULL.
    \note      ISR-safe.
*/
void stk_bytepool_get_stats(const stk_bytepool_t *pool, stk_mem_stats_t *stats);

/*! \brief     Get the length of the longest block-list walk (in blocks) of any allocation so far.
    \details   The walk is executed inside a critical section, so this measures the worst-case time
               interrupts/scheduling were blocked by the allocator.
    \param[in] pool: Pool handle.
    \note      ISR-safe on targets where an aligned size_t read is a single instruction.
*/
size_t stk_bytepool_get_max_scan_length(const stk_bytepool_t *pool);

/*! \brief     Get the number of allocation requests that failed because no suitable block was available
               (timeouts, \c STK_NO_WAIT misses and requests larger than \c stk_bytepool_get_max_request()).
    \param[in] pool: Pool handle.
    \note      ISR-safe on targets where an aligned size_t read is a single instruction.
*/
size_t stk_bytepool_get_alloc_fail_count(const stk_bytepool_t *pool);

/*! \brief     Get the high watermark of pool consumption in bytes (headers included).
    \param[in] pool: Pool handle.
    \note      ISR-safe on targets where an aligned size_t read is a single instruction.
*/
size_t stk_bytepool_get_peak_used_size(const stk_bytepool_t *pool);

/*! \brief     Verify that the backing storage is valid and the pool is ready for use.
    \param[in] pool: Pool handle.
    \return    \c true if the pool is ready; \c false if heap allocation failed or the supplied
               area is too small for even one block.
    \note      ISR-safe.
*/
bool stk_bytepool_is_storage_valid(const stk_bytepool_t *pool);

/*! \brief     Get the total managed (aligned) area size in bytes, headers included.
    \param[in] pool: Pool handle.
    \return    Managed area size, or 0 if the storage is invalid.
    \note      ISR-safe.
*/
size_t stk_bytepool_get_size(const stk_bytepool_t *pool);

/*! \brief     Get the largest request that can ever succeed on this pool.
    \param[in] pool: Pool handle.
    \return    Payload size of a single block spanning the whole pool (less the guard word, if enabled),
               or 0 if the storage is invalid. Larger requests fail immediately instead of blocking forever.
    \note      ISR-safe.
*/
size_t stk_bytepool_get_max_request(const stk_bytepool_t *pool);

// =============================================================================
// General-purpose allocator (stk::memory::MemoryAllocator)
// =============================================================================

/*! \def     STK_MEM_MIN_ALIGNMENT
    \brief   Minimum alignment in bytes guaranteed for pointers returned by \c stk_mem_allocate()
             (equals \c MemoryAllocator::MIN_ALIGNMENT).
*/
#ifdef _STK_ARCH_RISC_V
    #define STK_MEM_MIN_ALIGNMENT (16U)
#else
    #define STK_MEM_MIN_ALIGNMENT (8U)
#endif

/*! \brief     Check that the memory allocator implementation is linked in.
    \return    \c true if allocation functions are available (always \c true unless \c STK_MEMORY_NO_HEAP is 1).
    \note      ISR-safe.
*/
bool stk_mem_is_available(void);

/*! \brief     Allocate a memory chunk from the general-purpose allocator.
    \param[in] size: Size in bytes.
    \return    Pointer aligned to at least \c STK_MEM_MIN_ALIGNMENT, or \c NULL on failure (or if \a size is 0).
    \note      Thread-safe. ISR-safe only with \c STK_C_MEMORY_DEFAULT_ALLOCATOR = 1.
*/
void *stk_mem_allocate(size_t size);

/*! \brief     Free a chunk obtained with \c stk_mem_allocate().
    \param[in] ptr: Pointer to the chunk. \c NULL is allowed and results in noop.
    \note      Thread-safe. ISR-safe only with \c STK_C_MEMORY_DEFAULT_ALLOCATOR = 1.
*/
void stk_mem_free(void *ptr);

/*! \brief     Allocate a chunk with an alignment stricter than \c STK_MEM_MIN_ALIGNMENT.
    \details   The result \b must be released with \c stk_mem_free_aligned(), never with \c stk_mem_free().
    \param[in] size: Size in bytes.
    \param[in] alignment: Required alignment, a power of two.
    \return    Aligned pointer, or \c NULL on failure, if \a alignment is not a power of two or heap support
               is compiled out.
*/
void *stk_mem_allocate_aligned(size_t size, size_t alignment);

/*! \brief     Free a chunk obtained with \c stk_mem_allocate_aligned().
    \param[in] ptr: Pointer returned by \c stk_mem_allocate_aligned(). \c NULL is allowed and results in noop.
*/
void stk_mem_free_aligned(void *ptr);

/*! \brief     Get statistics of the general-purpose allocator.
    \param[out] stats: Receives the snapshot. Must not be \c NULL. All zero if the implementation
                does not provide statistics (\c STK_C_MEMORY_DEFAULT_ALLOCATOR = 0).
*/
void stk_mem_get_stats(stk_mem_stats_t *stats);

#ifdef __cplusplus
namespace stk { namespace memory { class BlockMemoryPool; class BytePool; } }

/*! \brief     Get the underlying C++ \c stk::memory::BlockMemoryPool object wrapped by a \c stk_blockpool_t handle.
    \param[in] pool: Pool handle obtained via \c stk_blockpool_create*().
    \return    Pointer to the wrapped \c stk::memory::BlockMemoryPool instance. Never \c NULL for a valid handle.
    \note      C++ callers only (guarded by \c __cplusplus); not part of the C ABI.
*/
extern "C" stk::memory::BlockMemoryPool *stk_blockpool_get_instance(stk_blockpool_t *pool);

/*! \brief     Get the underlying C++ \c stk::memory::BytePool object wrapped by a \c stk_bytepool_t handle.
    \param[in] pool: Pool handle obtained via \c stk_bytepool_create() or
               \c stk_bytepool_create_static().
    \return    Pointer to the wrapped \c stk::memory::BytePool instance. Never \c NULL for a valid handle.
    \note      C++ callers only (guarded by \c __cplusplus); not part of the C ABI.
*/
extern "C" stk::memory::BytePool *stk_bytepool_get_instance(stk_bytepool_t *pool);
#endif // __cplusplus

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* STK_C_MEMORY_H_ */
