/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_MEMORY_BLOCKPOOL_H_
#define STK_MEMORY_BLOCKPOOL_H_

#include "stk_memory_allocator.h"
#include "sync/stk_sync_cv.h"

/*! \def   STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT
    \brief Default block alignment (bytes, power of two, not less than sizeof(void *)) used when a pool is
           constructed without an explicit alignment. Defaults to 8 so that \c double / \c uint64_t members are
           properly aligned on 32-bit targets (where sizeof(void *) is only 4). Define it as
           \c sizeof(void *) to restore the pre-existing behavior: the smallest possible blocks
           (4 bytes on 32-bit targets) at the cost of 4-byte-only alignment there.
*/
#ifndef STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT
    #ifdef _STK_ARCH_RISC_V
        #define STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT (16U)
    #else
        #define STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT (8U)
    #endif
#endif

/*! \def   STK_MEMORY_BLOCKPOOL_BITMAP
    \brief When defined as 1, BlockMemoryPool keeps one "allocated" bit per block (capacity/8 bytes,
           carved from the end of the pool storage) which gives O(1) double-free detection in
           \b release builds too. Costs one integer division per Alloc()/Free(); off by default.
    \note  Changes the storage requirement: size external buffers with
           BlockMemoryPool::RequiredStorageSize() (StaticBlockPool does it for you).
*/
#ifndef STK_MEMORY_BLOCKPOOL_BITMAP
    #define STK_MEMORY_BLOCKPOOL_BITMAP (0)
#endif

/*! \file  stk_memory_blockpool.h
    \brief Implementation of fixed-size block memory pool: stk::memory::BlockMemoryPool.
*/

namespace stk {
namespace memory {

/*! \class BlockMemoryPool
    \brief Fixed-size block allocator with O(1) alloc/free and proper task-blocking semantics.

    BlockMemoryPool provides a deterministic, fragmentation-free allocator for scenarios
    where the same block size is repeatedly allocated and released - such as packet
    buffers, sensor sample records, or task-local state objects.

    The pool uses an intrusive singly-linked free-list inside the storage array.
    When a block is free, its first \c sizeof(void*) bytes hold a pointer to the next
    free block; no separate metadata array is needed. Alloc and Free are therefore O(1)
    with a minimal critical section.

    Memory layout (pool storage, contiguous):

        [ block_0 | block_1 | ... | block_{n-1} ][ bitmap (optional) ]
        ^                                       ^
        m_blocks                         (m_blocks + n * m_block_size)

        At construction all blocks are chained: block_i->next = block_{i+1},
        last->next = nullptr. m_free_list points to block_0.

    **Alignment**: every block starts on a multiple of the pool alignment (default
    \c ALIGN_DEFAULT = 8 bytes unless \c STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT is overridden, so \c double / \c uint64_t members are safe on 32-bit targets;
    pass a larger value for cache-line / DMA / SIMD buffers). A misaligned external buffer is
    corrected internally (like BytePool does): the unaligned head is skipped and the capacity is trimmed
    to what fits. \c TimedAllocT() refuses (returns nullptr) types that do not fit the block size or alignment.

    **Three storage modes**:

    | Mode             | Constructor / type                                   | Frees storage? |
    |------------------|------------------------------------------------------|----------------|
    | Embedded storage | \c StaticBlockPool<size, count> (zero-heap, easiest) | n/a            |
    | External storage | \c BlockMemoryPool(cap, blksz, buf, bufsz [, name])  | No - caller    |
    | Heap storage     | \c BlockMemoryPool(cap, blksz [, name])              | Yes - dtor     |

    **Blocking semantics**: \c TimedAlloc() suspends the calling task via a
    \c ConditionVariable until \c Free() returns a block, or the timeout expires.
    This replaces the spin-yield polling loop that was used in the old CMSIS wrapper,
    giving up the CPU completely while blocked instead of burning cycles.

    **ISR safety**: \c TryAlloc() and \c Free() are ISR-safe (guarded by a critical
    section) and never touch the condition variable on the allocation path. \c Alloc() and
    \c TimedAlloc() with a non-zero timeout must only be called from task context.

    **Diagnostics**: \c GetPeakUsedCount() (high watermark) and \c GetAllocFailCount() tell you how
    big the pool really needs to be; \c GetStats() returns the same MemoryAllocator::Stats structure
    as the general-purpose allocator (in bytes).

    **Object helpers**: \c NewT() / \c TryNewT() / \c TimedNewT() allocate and construct, \c DeleteT()
    destroys and frees, \c TryMakeT() / \c MakeT() return a PoolPtr that frees automatically.

    \code
    // -----------------------------------------------------------------------
    // Example 1 - native STK usage with zero-heap embedded storage
    // -----------------------------------------------------------------------
    static const uint32_t PKT_COUNT = 8U;

    stk::memory::StaticBlockPool<sizeof(Packet), PKT_COUNT> g_PktPool;

    void ISR_Receiver() {
        void *blk = g_PktPool.TryAlloc();       // ISR-safe, non-blocking
        if (blk) {
            FillPacket(static_cast<Packet *>(blk));
            g_ParseQueue.Write(blk, NO_WAIT);
        }
    }

    void Task_Parser() {
        void *blk = nullptr;
        if (g_ParseQueue.Read(blk)) {
            Parse(static_cast<Packet *>(blk));
            g_PktPool.Free(blk);                // O(1), wakes one blocked allocator
        }
    }

    // -----------------------------------------------------------------------
    // Example 2 - hand-sized external storage
    // -----------------------------------------------------------------------
    alignas(8) static uint8_t g_Buf[stk::memory::BlockMemoryPool::RequiredStorageSize(PKT_COUNT, sizeof(Packet))];

    stk::memory::BlockMemoryPool g_Pool2(PKT_COUNT, sizeof(Packet), g_Buf, sizeof(g_Buf));
    \endcode

    \see  sync::ConditionVariable, sync::Semaphore, sync::Pipe, StaticBlockPool
    \note Blocking alloc paths require the kernel to be compiled with \a KERNEL_SYNC.
          \c TryAlloc() and \c Free() are always available regardless of kernel mode.
    \note Do not create global pool objects with the heap constructor: they run before the heap
          is usable (static initialization order). Use external or embedded storage for globals.
*/
class BlockMemoryPool : public ITraceable
{
public:
    /*! \brief     Max capacity supported (number of blocks).
    */
    static const size_t CAPACITY_MAX = 0xFFFEU;

    /*! \brief     Default block alignment in bytes (see \c STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT).
    */
    static const size_t ALIGN_DEFAULT = STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT;

    /*! \brief     Construct a pool backed by \b caller-supplied (external) storage.
        \details   The pool references \a storage directly without taking ownership.
                   The caller must keep the buffer alive for the entire lifetime of
                   this object. The storage is \b not freed on destruction. A misaligned
                   start address is corrected internally (the unaligned head bytes are not used)
                   and the capacity is trimmed to what fits (asserted in debug builds).
        \param[in] capacity:       Total number of blocks the pool can hold.
        \param[in] raw_block_size: Requested per-block size in bytes. Rounded up
                                   internally to \c AlignBlockSize(raw_block_size, alignment).
        \param[in] storage:        Pointer to a caller-owned byte buffer. Should be aligned to
                                   \a alignment and sized to hold at least
                                   \c RequiredStorageSize(capacity, raw_block_size, alignment) bytes.
                                   Asserted at construction time.
        \param[in] storage_size:   Size of \a storage in bytes.
        \param[in] name:           Optional name forwarded to \c ITraceable::SetTraceName().
        \param[in] alignment:      Block alignment, a power of two not less than sizeof(void*).
        \note      Mirrors the \c mp_mem / \c mp_size path in \c osMemoryPoolNew().
        \note      ISR-unsafe (must be called from task or init context).
        \note      Check \c IsStorageValid() if the arguments come from run-time data.
    */
    explicit BlockMemoryPool(size_t capacity, size_t raw_block_size, uint8_t *storage,
                             size_t storage_size, const char *name = nullptr,
                             size_t alignment = ALIGN_DEFAULT);

    /*! \brief     Construct a pool with \b heap-allocated storage.
        \details   Allocates a flat byte buffer of
                   \c RequiredStorageSize(capacity, raw_block_size, alignment) bytes via
                   \c MemoryAllocator::AllocateArrayT (non-throwing, overflow-checked). When exceptions are disabled
                   (embedded default), check \c IsStorageValid() immediately after
                   construction to detect allocation failure before first use.
        \param[in] capacity:       Total number of blocks.
        \param[in] raw_block_size: Requested per-block size in bytes.
        \param[in] name:           Optional name forwarded to \c ITraceable::SetTraceName().
        \param[in] alignment:      Block alignment, a power of two not less than sizeof(void*).
        \note      Mirrors the heap-fallback path in \c osMemoryPoolNew().
        \note      ISR-unsafe. Not available if \c STK_MEMORY_NO_HEAP is 1.
    */
#if STK_MEMORY_HAS_HEAP
    explicit BlockMemoryPool(size_t capacity, size_t raw_block_size, const char *name = nullptr,
                             size_t alignment = ALIGN_DEFAULT);
#endif

    /*! \brief     Destructor.
        \details   Frees heap-allocated storage if \c m_storage_owned is \c true.
                   External storage is never touched. Asserts that all blocks were returned.
        \note      Destroying the pool while tasks are blocked in \c Alloc() or
                   \c TimedAlloc() is a logical error (dangling waiters). An assertion
                   is triggered in debug builds inside the \c ConditionVariable destructor.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~BlockMemoryPool();

    /*! \brief     Round a raw block size up to the nearest multiple of \a alignment.
        \details   Enforces a minimum of \a alignment so the free-list link always
                   fits inside every free block without extra metadata.
        \param[in] raw_size:  Requested block size in bytes.
        \param[in] alignment: Block alignment (power of two), \c ALIGN_DEFAULT by default.
        \return    Aligned block size in bytes (>= \a alignment).
    */
    static constexpr size_t AlignBlockSize(size_t raw_size, size_t alignment = ALIGN_DEFAULT)
    {
        return Max(alignment, ((raw_size + (alignment - 1U)) & ~(alignment - 1U)));
    }

    /*! \brief     Number of bytes of the "allocated" bitmap for \a capacity blocks.
        \return    0 unless \c STK_MEMORY_BLOCKPOOL_BITMAP is 1.
    */
    static constexpr size_t BitmapBytes(size_t capacity)
    {
        return ((STK_MEMORY_BLOCKPOOL_BITMAP != 0) ? ((capacity + 7U) / 8U) : 0U);
    }

    /*! \brief     Compute the size of the storage buffer required for an external-storage pool.
        \details   Accounts for the aligned block size and the optional bitmap. If you cannot
                   guarantee that the buffer is aligned to \a alignment add <tt>alignment - 1</tt>.
        \code
        alignas(8) uint8_t buf[BlockMemoryPool::RequiredStorageSize(N, sizeof(MyType))];
        \endcode
        \param[in] capacity:       Number of blocks.
        \param[in] raw_block_size: Requested per-block size in bytes.
        \param[in] alignment:      Block alignment (power of two), \c ALIGN_DEFAULT by default.
        \return    Required buffer size in bytes.
    */
    static constexpr size_t RequiredStorageSize(size_t capacity, size_t raw_block_size, size_t alignment = ALIGN_DEFAULT)
    {
        return ((capacity * AlignBlockSize(raw_block_size, alignment)) + BitmapBytes(capacity));
    }

    /*! \brief     Allocate one block, blocking until one becomes available or the timeout expires.
        \details   If the pool is empty the calling task is suspended via the internal
                   \c ConditionVariable and woken by the next \c Free() call. On return the
                   caller owns the block and must eventually return it via \c Free().
        \param[in] timeout_ticks: Maximum time to wait (ticks).
                   \c WAIT_INFINITE - block indefinitely (default).
                   \c NO_WAIT       - return \c nullptr immediately if pool is empty
                                      (identical to \c TryAlloc(); ISR-safe).
        \return    Pointer to an uninitialized block of at least \c GetBlockSize() bytes,
                   or \c nullptr if the timeout expired, the pool is invalid (see \c IsStorageValid()),
                   or the wait was cancelled via \c IKernel::CancelTaskWait() before a block became available.
        \note      Collapses timeout and cancellation. Use \c TimedAllocEx() to tell them apart.
        \warning   ISR-safe \b only when \a timeout_ticks = \c NO_WAIT; ISR-unsafe otherwise.
    */
    void *TimedAlloc(Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Allocate one block, preserving the full wait outcome.
        \details   Identical to \c TimedAlloc(), except the caller gets the raw \c EWaitResult
                   so a cancellation (via \c IKernel::CancelTaskWait()) can be distinguished
                   from a timeout. Mirrors \c sync::Event::WaitEx().
        \param[out] block: Receives the allocated block on \c WAIT_RESULT_SIGNAL, \c nullptr on
                   any other result. Must not be \c nullptr.
        \param[in] timeout_ticks: Same as \c TimedAlloc().
        \return    \c WAIT_RESULT_SIGNAL   - a block was allocated (immediately or after waiting),
                   \c WAIT_RESULT_TIMEOUT  - no block became available before the timeout (or \c NO_WAIT
                                             was passed on an empty pool); a block freed at the very
                                             moment the timeout expired is still returned as SIGNAL,
                   \c WAIT_RESULT_CANCELED - the wait was cancelled, nothing was allocated,
                   \c WAIT_RESULT_FAIL     - invalid arguments, invalid pool (no usable storage) or the
                                             kernel rejected the wait.
        \warning   ISR-safe \b only when \a timeout_ticks = \c NO_WAIT; ISR-unsafe otherwise.
    */
    EWaitResult TimedAllocEx(void **block, Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Allocate one typed block, blocking until one becomes available or the timeout expires.
        \details   Thin typed wrapper around \c TimedAlloc(). The memory is uninitialized; no constructor
                   is run (see \c TimedNewT()). Returns \c nullptr (and asserts in debug builds) if
                   \c sizeof(T) exceeds the block size or \c alignof(T) exceeds the pool alignment.
        \param[in] timeout_ticks: Maximum time to wait (ticks). Same semantics as \c TimedAlloc().
        \return    Typed pointer, or \c nullptr if the timeout expired or T does not fit.
        \warning   ISR-safe \b only when \a timeout_ticks = \c NO_WAIT; ISR-unsafe otherwise.
    */
    template <typename T> T *TimedAllocT(Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Allocate one block, blocking indefinitely until one is available.
        \return    Pointer to an uninitialized block. Returns \c nullptr only if the wait was
                   cancelled via \c IKernel::CancelTaskWait() or the pool is invalid; never on timeout (there is none).
                   Use \c TimedAllocEx() if cancellation must be told apart from other failures.
        \warning   ISR-unsafe.
    */
    void *Alloc();

    /*! \brief     Allocate one typed block, blocking indefinitely until one is available.
        \return    Typed pointer. Returns \c nullptr only if the wait was cancelled, the pool is invalid
                   or T does not fit (see \c Alloc()).
        \warning   ISR-unsafe.
    */
    template <typename T> T *AllocT();

    /*! \brief     Non-blocking allocation attempt.
        \details   Returns a block immediately if the pool is not empty, or \c nullptr
                   if all blocks are currently allocated. Never suspends the calling task
                   and never touches the condition variable.
        \return    Pointer to an uninitialized block, or \c nullptr if the pool is empty.
        \note      ISR-safe.
    */
    void *TryAlloc();

    /*! \brief     Non-blocking typed allocation attempt.
        \details   Returns a typed pointer immediately if the pool is not empty, or \c nullptr
                   if all blocks are currently allocated. Never suspends the calling task.
        \return    Typed pointer, or \c nullptr if the pool is empty.
        \note      ISR-safe.
    */
    template <typename T> T *TryAllocT();

#if STK_MEMORY_PLACEMENT_NEW

    /*! \brief     Allocate one block and construct a \c T in it (placement new), waiting up to \a timeout_ticks.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \param[in] args: Constructor arguments forwarded to \c T.
        \return    Pointer to the constructed object, or \c nullptr (see \c TimedAllocT()).
        \note      Pair with \c DeleteT(). ISR-safe only with \c NO_WAIT (and only if the constructor is).
    */
    template <typename T, typename... TArgs> T *TimedNewT(Timeout timeout_ticks, TArgs &&...args);

    /*! \brief     Allocate one block and construct a \c T in it, blocking indefinitely.
        \warning   ISR-unsafe.
    */
    template <typename T, typename... TArgs> T *NewT(TArgs &&...args);

    /*! \brief     Non-blocking \c TimedNewT(). ISR-safe if the constructor of \c T is.
    */
    template <typename T, typename... TArgs> T *TryNewT(TArgs &&...args);

    /*! \brief     Destroy an object created with the NewT() family and return its block to the pool.
        \param[in] ptr: Object pointer, nullptr is allowed and results in noop.
        \note      ISR-safe if the destructor of \c T is.
    */
    template <typename T> void DeleteT(T *ptr);

    /*! \brief     Non-blocking allocate-and-construct returning an owning PoolPtr.
    */
    template <typename T, typename... TArgs> PoolPtr<T, BlockMemoryPool> TryMakeT(TArgs &&...args);

    /*! \brief     Blocking allocate-and-construct returning an owning PoolPtr.
        \warning   ISR-unsafe.
    */
    template <typename T, typename... TArgs> PoolPtr<T, BlockMemoryPool> MakeT(TArgs &&...args);

#endif // STK_MEMORY_PLACEMENT_NEW

    /*! \brief     Return a previously allocated block to the pool.
        \details   Pushes the block back onto the free-list head in O(1) and wakes
                   exactly one task blocked inside \c Alloc() or \c TimedAlloc(), if any,
                   so it can re-check for a free block. Under contention from \c TryAlloc()
                   (including from another task or an ISR racing in before the woken task
                   is scheduled), that task may find the block already taken and continue
                   waiting rather than being guaranteed one on this wake.
        \param[in] ptr: Pointer previously returned by \c Alloc(), \c TimedAlloc(), or
                   \c TryAlloc(). Must not be \c nullptr and must belong to this pool
                   instance. Bounds and alignment are validated; failures trigger an
                   assertion in debug builds and return \c false in release builds.
        \return    \c true  - block successfully returned.
                   \c false - \a ptr is \c nullptr, out of range, misaligned or already free
                               (double-free, release-build detection requires
                               \c STK_MEMORY_BLOCKPOOL_BITMAP); each case indicates a caller logic error.
        \note      ISR-safe.
        \warning   Without \c STK_MEMORY_BLOCKPOOL_BITMAP a double-free is detected only if the pool is
                   already fully free, or in builds with \c STK_MEMORY_POOL_CHECKS (O(n) free-list walk);
                   otherwise it silently corrupts the free-list. Null the pointer after \c Free().
    */
    bool Free(void *ptr);

    /*! \brief     Verify that the backing storage is valid and the pool is ready for use.
        \details   \c false if the constructor arguments were invalid, a heap allocation failed, or
                   the supplied buffer is too small to hold a single block. Must be checked after the
                   heap constructor when operating without exceptions (the typical embedded configuration).
                   An invalid pool fails every allocation immediately instead of blocking forever.
        \return    \c true if the pool is ready for use.
        \note      ISR-safe.
    */
    bool IsStorageValid() const { return ((m_blocks != nullptr) && (m_capacity != 0U)); }

    /*! \brief     Get the total block capacity of the pool.
        \return    Maximum number of blocks that can be simultaneously allocated.
                   Matches \c osMemoryPoolGetCapacity(). Can be lower than requested if the supplied
                   buffer was too small (debug builds assert), 0 for an invalid pool.
        \note      ISR-safe.
    */
    size_t GetCapacity() const { return m_capacity; }

    /*! \brief     Get the aligned block size used internally by the allocator.
        \details   Equal to \c AlignBlockSize(raw_block_size, alignment) passed at construction.
                   Matches \c osMemoryPoolGetBlockSize().
        \return    Aligned block size in bytes.
        \note      ISR-safe.
    */
    size_t GetBlockSize() const { return m_block_size; }

    /*! \brief     Get the alignment (bytes) of every block.
        \note      ISR-safe.
    */
    size_t GetBlockAlignment() const { return m_block_align; }

    /*! \brief     Get the number of currently allocated (outstanding) blocks.
        \return    Point-in-time snapshot. Matches \c osMemoryPoolGetCount().
        \note      May be stale immediately after return in a multi-task environment.
        \note      ISR-safe on targets where an aligned \c size_t read is a single instruction.
    */
    size_t GetUsedCount() const { return m_used_count; }

    /*! \brief     Get the number of free (available) blocks.
        \return    Point-in-time snapshot of \c GetCapacity() - \c GetUsedCount().
                   Matches \c osMemoryPoolGetSpace().
        \note      ISR-safe.
    */
    size_t GetFreeCount() const { return (m_capacity - m_used_count); }

    /*! \brief     Get the high watermark: the maximum number of blocks that were allocated at the same time.
        \details   Run your worst-case workload and use this value to size the pool.
        \note      ISR-safe on targets where an aligned \c size_t read is a single instruction.
    */
    size_t GetPeakUsedCount() const { return ((m_block_size != 0U) ? (m_stats.peak_allocated / m_block_size) : 0U); }

    /*! \brief     Get the number of allocation requests that failed because the pool was exhausted
                   (timeouts and \c NO_WAIT misses).
        \note      ISR-safe on targets where an aligned \c size_t read is a single instruction.
    */
    size_t GetAllocFailCount() const { return m_stats.alloc_fail_count; }

    /*! \brief     Get a consistent statistics snapshot in the same format as MemoryAllocator::GetStats().
        \details   All values are in bytes (blocks * block size); counters are in operations.
        \return    Stats structure.
        \note      ISR-safe.
    */
    MemoryAllocator::Stats GetStats() const;

    /*! \brief     Check whether all blocks are currently allocated (pool exhausted).
        \return    \c true if no blocks are available for allocation.
        \note      ISR-safe.
    */
    bool IsFull() const { return (GetFreeCount() == 0U); }

    /*! \brief     Check whether all blocks are free (no outstanding allocations).
        \return    \c true if no blocks are currently allocated.
        \note      ISR-safe.
    */
    bool IsEmpty() const { return (m_used_count == 0U); }

private:
    STK_NONCOPYABLE_CLASS(BlockMemoryPool);

    /*! \struct    MemoryBlock
        \brief     Intrusive free-list node overlaid on the first word of every free block.
        \details   When a block is free its first \c sizeof(MemoryBlock) bytes hold a \c next
                   pointer to the next free block. No separate metadata array is required.
    */
    struct MemoryBlock
    {
        MemoryBlock *next; //!< next free block in the list, or nullptr if this is the last one
    };

    static const uint8_t FILL_ALLOC = 0xCDU; //!< payload fill on allocation (STK_MEMORY_POOL_CHECKS)
    static const uint8_t FILL_FREE  = 0xDDU; //!< payload fill on free (STK_MEMORY_POOL_CHECKS)

    /*! \brief     Check an alignment value: a power of two, large enough for the free-list link.
    */
    static bool IsValidAlignment(size_t alignment)
    {
        return ((alignment >= sizeof(MemoryBlock)) && ((alignment & (alignment - 1U)) == 0U));
    }

    /*! \brief     Check constructor arguments (block size overflow included).
    */
    static bool ArgsValid(size_t capacity, size_t raw_block_size, size_t alignment, size_t block_size)
    {
        return ((capacity > 0U) && (capacity <= CAPACITY_MAX) && (raw_block_size > 0U) &&
                IsValidAlignment(alignment) && (block_size >= raw_block_size));
    }

    /*! \brief     Align storage, trim the capacity to what fits and build the free-list.
        \note      Leaves the pool invalid (m_blocks == nullptr, m_capacity == 0) if anything is wrong.
    */
    void Setup(size_t raw_block_size, size_t alignment);

    /*! \brief     Initialise the intrusive free-list across \c m_blocks.
        \details   Links blocks in reverse order so after the loop \c m_free_list
                   points to \c block_0 (lowest address), giving ascending allocation
                   order. Each free block stores a pointer to the next free block in
                   its own first \c sizeof(MemoryBlock) bytes - zero extra metadata.
    */
    void BuildFreeList();

    /*! \brief    Pop the head block from the free-list, increment \c m_used_count, and return it.
        \note     Caller must hold a critical section and ensure \c m_free_list != nullptr.
    */
    void *PopFreeList();

    /*! \brief    Fill \a size bytes at \a ptr with \a value (used by diagnostics only).
    */
    static __stk_forceinline void Fill(void *ptr, size_t size, uint8_t value)
    {
        STK_MEMSET(ptr, value, size);
    }

#if STK_MEMORY_BLOCKPOOL_BITMAP
    bool BitmapTest(size_t index) const { return ((m_bitmap[index >> 3U] & static_cast<uint8_t>(1U << (index & 7U))) != 0U); }
    void BitmapSet(size_t index)        { m_bitmap[index >> 3U] = static_cast<uint8_t>(m_bitmap[index >> 3U] | static_cast<uint8_t>(1U << (index & 7U))); }
    void BitmapClear(size_t index)      { m_bitmap[index >> 3U] = static_cast<uint8_t>(m_bitmap[index >> 3U] & static_cast<uint8_t>(~(1U << (index & 7U)))); }
#endif

    uint8_t                 *m_storage;        //!< raw storage as supplied or allocated (before alignment)
    uint8_t                 *m_blocks;         //!< aligned start of block 0, nullptr if the pool is invalid
    uint8_t                 *m_bitmap;         //!< "allocated" bitmap (STK_MEMORY_BLOCKPOOL_BITMAP), otherwise unused
    MemoryBlock             *m_free_list;      //!< head of the intrusive free-list (nullptr when pool is empty)
    sync::ConditionVariable  m_cv;             //!< signalled by Free() to wake one task blocked in TimedAlloc()
    size_t                   m_storage_size;   //!< size of the raw storage in bytes
    size_t                   m_block_size;     //!< aligned block size in bytes (>= m_block_align)
    size_t                   m_block_align;    //!< alignment of every block in bytes
    size_t                   m_capacity;       //!< total number of blocks (0 if invalid)
    size_t                   m_used_count;     //!< number of blocks currently allocated (outstanding)
    MemoryAllocator::Stats   m_stats;          //!< live statistics in bytes (blocks * block size): usage, watermark, counters
    bool                     m_storage_owned;  //!< true -> storage is heap-allocated; free in destructor
};

// ---------------------------------------------------------------------------
// Constructors / Destructor
// ---------------------------------------------------------------------------

inline BlockMemoryPool::BlockMemoryPool(size_t capacity, size_t raw_block_size, uint8_t *storage,
                                        size_t storage_size, const char *name, size_t alignment)
: m_storage(storage),
  m_blocks(nullptr),
  m_bitmap(nullptr),
  m_free_list(nullptr),
  m_cv(),
  m_storage_size(storage_size),
  m_block_size(AlignBlockSize(raw_block_size, alignment)),
  m_block_align(alignment),
  m_capacity(capacity),
  m_used_count(0U),
  m_stats(capacity * m_block_size),
  m_storage_owned(false)
{
    STK_ASSERT(storage != nullptr);

#if STK_SYNC_DEBUG_NAMES
    SetTraceName(name);
#else
    STK_UNUSED(name);
#endif

    Setup(raw_block_size, alignment);
}

#if STK_MEMORY_HAS_HEAP
inline BlockMemoryPool::BlockMemoryPool(size_t capacity, size_t raw_block_size, const char *name,
                                        size_t alignment)
: m_storage(nullptr),
  m_blocks(nullptr),
  m_bitmap(nullptr),
  m_free_list(nullptr),
  m_cv(),
  m_storage_size(0U),
  m_block_size(AlignBlockSize(raw_block_size, alignment)),
  m_block_align(alignment),
  m_capacity(capacity),
  m_used_count(0U),
  m_stats(capacity * m_block_size),
  m_storage_owned(true)
{
#if STK_SYNC_DEBUG_NAMES
    SetTraceName(name);
#else
    STK_UNUSED(name);
#endif

    if (ArgsValid(capacity, raw_block_size, alignment, m_block_size))
    {
        // total = blocks + bitmap + slack for aligning the start (Allocate() itself guarantees ALIGNMENT);
        // every step is checked so that a huge capacity * block size can not wrap around
        const size_t max_size = static_cast<size_t>(~static_cast<size_t>(0U));
        const size_t slack    = ((alignment > MemoryAllocator::MIN_ALIGNMENT) ? (alignment - 1U) : 0U);
        const size_t bitmap   = BitmapBytes(capacity);

        if ((capacity <= (max_size / m_block_size)) &&
            ((capacity * m_block_size) <= (max_size - bitmap - slack)))
        {
            const size_t total = ((capacity * m_block_size) + bitmap + slack);

            m_storage = MemoryAllocator::AllocateArrayT<uint8_t>(total);
            if (m_storage != nullptr)
            {
                m_storage_size = total;
            }
        }
    }

    // else: m_blocks remains nullptr; caller must check IsStorageValid()
    Setup(raw_block_size, alignment);
}
#endif

inline BlockMemoryPool::~BlockMemoryPool()
{
    // ConditionVariable destructor asserts the wait list is empty
    STK_ASSERT(m_used_count == 0U); // all blocks must be returned before the pool goes away

#if STK_MEMORY_HAS_HEAP
    if (m_storage_owned && (m_storage != nullptr))
    {
        MemoryAllocator::FreeArrayT(m_storage, m_storage_size);
        m_storage = nullptr;
    }
#endif

    m_blocks = nullptr;
}

// ---------------------------------------------------------------------------
// Alloc / TimedAlloc / TryAlloc
// ---------------------------------------------------------------------------

inline EWaitResult BlockMemoryPool::TimedAllocEx(void **block, Timeout timeout_ticks)
{
    EWaitResult result = WAIT_RESULT_FAIL;

    STK_ASSERT(block != nullptr); // API contract: output pointer must be valid

    if (block == nullptr)
    {
        // result is WAIT_RESULT_FAIL already, so noop
    }
    else
    {
        *block = nullptr;

        if ((timeout_ticks != NO_WAIT) && hw::IsInsideISR())
        {
            STK_ASSERT(false); // API contract: ISR callers must pass NO_WAIT / use TryAlloc()
        }
        else if (!IsStorageValid())
        {
            // result is WAIT_RESULT_FAIL: an invalid pool can never produce a block, so do not block on it
        }
        else
        {
            sync::ScopedCriticalSection cs_;

            const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

            // capture an absolute deadline once, before entering the wait loop; a different
            // allocator (TryAlloc() from another task or an ISR) can steal the just-freed
            // block between NotifyOne_CS() and this task re-acquiring the critical section
            // (Mesa-style wakeup, not a direct handoff - see Free()'s note), so re-passing
            // the original timeout_ticks on each retry would silently restart it;
            // kept as 64-bit Ticks (not truncated to Timeout) so it can't wrap on long uptimes
            const Ticks deadline = (timed_wait ? (GetTicks() + static_cast<Ticks>(timeout_ticks)) : 0);

            result = WAIT_RESULT_SIGNAL; // fast path: a block is already available

            while (m_free_list == nullptr)
            {
                Timeout remaining = timeout_ticks;

                if (timeout_ticks == NO_WAIT)
                {
                    // no waiting requested (ISR-safe path): never touch the condition variable
                    result = WAIT_RESULT_TIMEOUT;
                    break;
                }

                if (timed_wait)
                {
                    const Ticks left = (deadline - GetTicks());
                    if (left <= 0)
                    {
                        result = WAIT_RESULT_TIMEOUT; // budget is spent and the free-list was just checked
                        break;
                    }
                    remaining = static_cast<Timeout>(left);
                }

                // Atomically release the critical section, suspend the task, and
                // re-acquire before returning - no CPU cycles wasted while waiting.
                result = m_cv.WaitEx(cs_, remaining);
                if (result != WAIT_RESULT_SIGNAL)
                {
                    break; // timeout, cancellation or kernel failure: report as is
                }
            }

            // timeout raced with a Free(): the lock was re-acquired and a block is available,
            // so honor it instead of reporting a failure (cancellation is never overridden)
            if ((result == WAIT_RESULT_TIMEOUT) && (m_free_list != nullptr))
            {
                result = WAIT_RESULT_SIGNAL;
            }

            // only allocate a block if the wait ended in a signal (or none was needed)
            if (result == WAIT_RESULT_SIGNAL)
            {
                *block = PopFreeList();
            }
            else if (result == WAIT_RESULT_TIMEOUT)
            {
                m_stats.RecordAllocFail();
            }
            else
            {
                // cancellation or kernel failure is not pool exhaustion: not counted
            }
        }
    }

    return result;
}

inline void *BlockMemoryPool::TimedAlloc(Timeout timeout_ticks)
{
    void *block = nullptr;

    if (TimedAllocEx(&block, timeout_ticks) != WAIT_RESULT_SIGNAL)
    {
        block = nullptr;
    }

    return block;
}

template <typename T>
inline T *BlockMemoryPool::TimedAllocT(Timeout timeout_ticks)
{
    T *result = nullptr;

    STK_ASSERT(sizeof(T) <= m_block_size);  // API contract: block size should be larger or equal to object's size
    STK_ASSERT(alignof(T) <= m_block_align); // API contract: blocks are aligned to the pool alignment only

    // refuse (rather than hand out an undersized or misaligned block) in release builds too
    if ((sizeof(T) <= m_block_size) && (alignof(T) <= m_block_align))
    {
        result = static_cast<T *>(TimedAlloc(timeout_ticks));
    }

    return result;
}

inline void *BlockMemoryPool::Alloc()
{
    return TimedAlloc(WAIT_INFINITE);
}

template <typename T>
inline T *BlockMemoryPool::AllocT()
{
    return TimedAllocT<T>(WAIT_INFINITE);
}

inline void *BlockMemoryPool::TryAlloc()
{
    return TimedAlloc(NO_WAIT);
}

template <typename T>
inline T *BlockMemoryPool::TryAllocT()
{
    return TimedAllocT<T>(NO_WAIT);
}

// ---------------------------------------------------------------------------
// Typed object helpers
// ---------------------------------------------------------------------------

#if STK_MEMORY_PLACEMENT_NEW

template <typename T, typename... TArgs>
inline T *BlockMemoryPool::TimedNewT(Timeout timeout_ticks, TArgs &&...args)
{
    T *const ptr = TimedAllocT<T>(timeout_ticks);

    if (ptr != nullptr)
    {
        auto const obj = new (static_cast<void *>(ptr)) T(static_cast<TArgs &&>(args)...);
        STK_UNUSED(obj);
    }

    return ptr;
}

template <typename T, typename... TArgs>
inline T *BlockMemoryPool::NewT(TArgs &&...args)
{
    return TimedNewT<T>(WAIT_INFINITE, static_cast<TArgs &&>(args)...);
}

template <typename T, typename... TArgs>
inline T *BlockMemoryPool::TryNewT(TArgs &&...args)
{
    return TimedNewT<T>(NO_WAIT, static_cast<TArgs &&>(args)...);
}

template <typename T>
inline void BlockMemoryPool::DeleteT(T *ptr)
{
    if (ptr != nullptr)
    {
        ptr->~T();

        const bool freed = Free(ptr);
        STK_UNUSED(freed);
    }
}

template <typename T, typename... TArgs>
inline PoolPtr<T, BlockMemoryPool> BlockMemoryPool::TryMakeT(TArgs &&...args)
{
    return PoolPtr<T, BlockMemoryPool>(TryNewT<T>(static_cast<TArgs &&>(args)...), this);
}

template <typename T, typename... TArgs>
inline PoolPtr<T, BlockMemoryPool> BlockMemoryPool::MakeT(TArgs &&...args)
{
    return PoolPtr<T, BlockMemoryPool>(NewT<T>(static_cast<TArgs &&>(args)...), this);
}

#endif // STK_MEMORY_PLACEMENT_NEW

// ---------------------------------------------------------------------------
// Free
// ---------------------------------------------------------------------------

inline bool BlockMemoryPool::Free(void *ptr)
{
    bool success = false;

    if ((ptr != nullptr) && (m_blocks != nullptr))
    {
        // bounds check: ptr must be in range [m_blocks, m_blocks + capacity * block_size)
        const Word pt = hw::PtrToWord(ptr);
        const Word lo = hw::PtrToWord(m_blocks);
        const Word hi = lo + (m_capacity * m_block_size);

        if ((pt < lo) || (pt >= hi))
        {
            STK_ASSERT(false); // API contract: ptr does not belong to this pool
        }
        // alignment check: ptr must be at the start of a block boundary
        else if ((static_cast<size_t>(pt - lo) % m_block_size) != 0U)
        {
            STK_ASSERT(false); // API contract: ptr is misaligned (not a block start)
        }
        else
        {
            const hw::CriticalSection::ScopedLock cs_;

            if (m_used_count == 0U)
            {
                STK_ASSERT(false); // pool is already fully free - definite double-free
            }
            else
            {
                bool valid = true;

            #if STK_MEMORY_BLOCKPOOL_BITMAP
                // O(1) double-free detection that stays active in release builds
                const size_t index = (static_cast<size_t>(pt - lo) / m_block_size);

                if (!BitmapTest(index))
                {
                    STK_ASSERT(false); // double-free: block is not marked as allocated
                    valid = false;
                }
                else
                {
                    BitmapClear(index);
                }
            #elif STK_MEMORY_POOL_CHECKS
                // O(n) double-free detection: walk the free-list and check if ptr is already on it,
                // only active if STK_MEMORY_POOL_CHECKS is enabled - compiles away completely otherwise
                for (const MemoryBlock *node = m_free_list; (node != nullptr); node = node->next)
                {
                    if (node == reinterpret_cast<const MemoryBlock *>(ptr))
                    {
                        STK_ASSERT(false); // double-free: ptr is already on the free-list
                        valid = false;
                        break;
                    }
                }
            #endif

                if (valid)
                {
                #if STK_MEMORY_POOL_CHECKS
                    // poison everything except the link word that is written below
                    Fill(static_cast<uint8_t *>(ptr) + sizeof(MemoryBlock), (m_block_size - sizeof(MemoryBlock)), FILL_FREE);
                #endif

                    // push block onto free-list head (O(1))
                    auto *const blk = reinterpret_cast<MemoryBlock *>(ptr);
                    blk->next       = m_free_list;
                    m_free_list     = blk;
                    m_used_count    = (m_used_count - 1U);
                    m_stats.RecordFree(m_block_size);

                    // wake one blocked allocator - true scheduler wait, no spin-yield
                    m_cv.NotifyOne_CS();

                    success = true;
                }
            }
        }
    }

    return success;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

inline MemoryAllocator::Stats BlockMemoryPool::GetStats() const
{
    const hw::CriticalSection::ScopedLock cs_;

    return m_stats;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

inline void BlockMemoryPool::Setup(size_t raw_block_size, size_t alignment)
{
    const size_t requested = m_capacity;

    m_blocks     = nullptr;
    m_bitmap     = nullptr;
    m_free_list  = nullptr;
    m_capacity   = 0U;
    m_used_count = 0U;

    STK_ASSERT(requested > 0U);
    STK_ASSERT(requested <= CAPACITY_MAX);
    STK_ASSERT(raw_block_size > 0U);
    STK_ASSERT(IsValidAlignment(alignment));

    if (ArgsValid(requested, raw_block_size, alignment, m_block_size) && (m_storage != nullptr))
    {
        // align the start of the first block, the unaligned head bytes are not used
        const size_t addr   = static_cast<size_t>(hw::PtrToWord(m_storage));
        const size_t shift  = (((addr + (alignment - 1U)) & ~(alignment - 1U)) - addr);
        const size_t usable = ((m_storage_size > shift) ? (m_storage_size - shift) : 0U);

        // trim the capacity to what fits (blocks + optional bitmap); never exceeds usable, so no overflow
        size_t n = (usable / m_block_size);
        if (n > requested)
        {
            n = requested;
        }

        while ((n > 0U) && (((n * m_block_size) + BitmapBytes(n)) > usable))
        {
            --n;
        }

        // API contract: caller-supplied buffer must be large enough (after alignment);
        // in Release builds the capacity is trimmed to fit instead
        STK_ASSERT(n == requested);

        if (n > 0U)
        {
            m_blocks   = (m_storage + shift);
            m_capacity = n;
            m_bitmap   = (m_blocks + (n * m_block_size));

            BuildFreeList();
        }
    }

    // the real capacity is known only now (trimmed to the storage, 0 if the pool is invalid)
    m_stats.Reset(m_capacity * m_block_size);
}

inline void BlockMemoryPool::BuildFreeList()
{
    STK_ASSERT((hw::PtrToWord(m_blocks) % m_block_align) == 0U);

    m_free_list  = nullptr;
    m_used_count = 0U;

#if STK_MEMORY_BLOCKPOOL_BITMAP
    for (size_t i = 0U; i < BitmapBytes(m_capacity); ++i)
    {
        m_bitmap[i] = 0U;
    }
#endif

    // Link blocks in reverse order so m_free_list ends up pointing at block_0
    // (lowest address), giving ascending allocation order.
    for (size_t i = m_capacity; i-- > 0U; )
    {
        MemoryBlock *const blk = hw::WordToPtr<MemoryBlock>(hw::PtrToWord(m_blocks) + (i * m_block_size));

        blk->next   = m_free_list;
        m_free_list = blk;
    }
}

inline void *BlockMemoryPool::PopFreeList()
{
    STK_ASSERT(m_used_count < m_capacity);

    MemoryBlock *const blk = m_free_list;

    m_free_list  = blk->next;
    m_used_count = (m_used_count + 1U);

    m_stats.RecordAllocate(m_block_size);

#if STK_MEMORY_BLOCKPOOL_BITMAP
    BitmapSet(static_cast<size_t>(hw::PtrToWord(blk) - hw::PtrToWord(m_blocks)) / m_block_size);
#endif

#if STK_MEMORY_POOL_CHECKS
    Fill(blk, m_block_size, FILL_ALLOC);
#endif

    return blk;
}

// ---------------------------------------------------------------------------
// StaticBlockPool - zero-heap pool that embeds its own, correctly sized and aligned storage
// ---------------------------------------------------------------------------

namespace detail {

/*! \brief Storage holder. Kept as a separate (first) base class so that the buffer exists
           before BlockMemoryPool's constructor links the free-list into it.
*/
template <size_t TBlockSize, size_t TCapacity, size_t TAlign>
struct BlockPoolStorage
{
    alignas(TAlign) uint8_t m_buf[BlockMemoryPool::RequiredStorageSize(TCapacity, TBlockSize, TAlign)];
};

} // namespace detail

/*! \class StaticBlockPool
    \brief Fixed-size block pool that owns its storage: no heap, no separately declared (and possibly
           mis-sized or misaligned) buffer, everything is computed at compile time.
    \tparam TBlockSize: Requested block size in bytes (e.g. sizeof(Packet)).
    \tparam TCapacity:  Number of blocks.
    \tparam TAlign:     Block alignment in bytes (power of two), default BlockMemoryPool::ALIGN_DEFAULT.
    \code
    stk::memory::StaticBlockPool<sizeof(Packet), 8> g_PktPool("pkt");
    \endcode
    \note Place the object into a specific memory region (CCM, SDRAM, DMA RAM) with your toolchain's
          section attribute on the variable declaration.
*/
template <size_t TBlockSize, size_t TCapacity, size_t TAlign = BlockMemoryPool::ALIGN_DEFAULT>
class StaticBlockPool : private detail::BlockPoolStorage<TBlockSize, TCapacity, TAlign>,
                        public BlockMemoryPool
{
    STK_STATIC_ASSERT_DESC(TBlockSize > 0U, "block size must be greater than zero");
    STK_STATIC_ASSERT_DESC((TCapacity > 0U) && (TCapacity <= BlockMemoryPool::CAPACITY_MAX), "invalid capacity");
    STK_STATIC_ASSERT_DESC((TAlign >= sizeof(void *)) && ((TAlign & (TAlign - 1U)) == 0U),
        "alignment must be a power of two not less than sizeof(void *)");

public:
    explicit StaticBlockPool(const char *name = nullptr)
        : BlockMemoryPool(TCapacity, TBlockSize, this->m_buf, sizeof(this->m_buf), name, TAlign)
    {}
};

#if STK_MEMORY_PLACEMENT_NEW
/*! \brief Pool of \a N blocks sized and aligned for objects of type \a T.
*/
template <typename T, size_t N>
using StaticBlockPoolFor = StaticBlockPool<sizeof(T), N,
    ((alignof(T) > BlockMemoryPool::ALIGN_DEFAULT) ? alignof(T) : BlockMemoryPool::ALIGN_DEFAULT)>;
#endif

} // namespace memory
} // namespace stk

#endif /* STK_MEMORY_BLOCKPOOL_H_ */
