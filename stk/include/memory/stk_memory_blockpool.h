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

        [ block_0 | block_1 | ... | block_{n-1} ]
        ^                                       ^
        m_storage                         (m_storage + n * m_block_size)

        At construction all blocks are chained: block_i->next = block_{i+1},
        last->next = nullptr. m_free_list points to block_0.

    **Two storage modes** mirror the CMSIS \c osMemoryPoolAttr_t \c mp_mem / \c mp_size
    fields and allow fully static (zero-heap) deployments:

    | Mode             | Constructor                                          | Frees storage? |
    |------------------|------------------------------------------------------|----------------|
    | External storage | \c BlockMemoryPool(cap, blksz, buf, bufsz [, name])  | No - caller    |
    | Heap storage     | \c BlockMemoryPool(cap, blksz [, name])              | Yes - dtor     |

    **Blocking semantics**: \c TimedAlloc() suspends the calling task via a
    \c ConditionVariable until \c Free() returns a block, or the timeout expires.
    This replaces the spin-yield polling loop that was used in the old CMSIS wrapper,
    giving up the CPU completely while blocked instead of burning cycles.

    **ISR safety**: \c TryAlloc() and \c Free() are ISR-safe (guarded by a critical
    section). \c Alloc() and \c TimedAlloc() with a non-zero timeout must only be
    called from task context.

    \code
    // -----------------------------------------------------------------------
    // Example 1 - native STK usage with zero-heap external storage
    // -----------------------------------------------------------------------
    static const uint32_t PKT_COUNT = 8U;
    static const uint32_t PKT_SIZE  = sizeof(Packet);

    alignas(sizeof(void *)) static uint8_t
        g_PktStorage[PKT_COUNT * stk::memory::BlockMemoryPool::AlignBlockSize(PKT_SIZE)];

    stk::memory::BlockMemoryPool g_PktPool(PKT_COUNT, PKT_SIZE,
                                           g_PktStorage, sizeof(g_PktStorage));

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

    \endcode

    \see  sync::ConditionVariable, sync::Semaphore, sync::Pipe
    \note Blocking alloc paths require the kernel to be compiled with \a KERNEL_SYNC.
          \c TryAlloc() and \c Free() are always available regardless of kernel mode.
*/
class BlockMemoryPool : public ITraceable
{
public:
    /*! \brief     Max capacity supported (number of blocks).
    */
    static const size_t CAPACITY_MAX = 0xFFFEU;

    /*! \brief     Construct a pool backed by \b caller-supplied (external) storage.
        \details   The pool references \a storage directly without taking ownership.
                   The caller must keep the buffer alive for the entire lifetime of
                   this object. The storage is \b not freed on destruction.
        \param[in] capacity:       Total number of blocks the pool can hold.
        \param[in] raw_block_size: Requested per-block size in bytes. Rounded up
                                   internally to \c AlignBlockSize(raw_block_size).
        \param[in] storage:        Pointer to a caller-owned byte buffer. Must be
                                   aligned to at least \c sizeof(void*) and sized to
                                   hold at least \c capacity * AlignBlockSize(raw_block_size)
                                   bytes. Asserted at construction time.
        \param[in] storage_size:   Size of \a storage in bytes (used for the size assertion).
        \param[in] name:           Optional name forwarded to \c ITraceable::SetTraceName().
        \note      Mirrors the \c mp_mem / \c mp_size path in \c osMemoryPoolNew().
        \note      ISR-unsafe (must be called from task or init context).
    */
    explicit BlockMemoryPool(size_t capacity, size_t raw_block_size, uint8_t *storage,
                             size_t storage_size, const char *name = nullptr);

    /*! \brief     Construct a pool with \b heap-allocated storage.
        \details   Allocates a flat byte buffer of
                   \c capacity * AlignBlockSize(raw_block_size) bytes via
                   \c operator new (std::nothrow). When exceptions are disabled
                   (embedded default), check \c IsStorageValid() immediately after
                   construction to detect allocation failure before first use.
        \param[in] capacity:       Total number of blocks.
        \param[in] raw_block_size: Requested per-block size in bytes.
        \param[in] name:           Optional name forwarded to \c ITraceable::SetTraceName().
        \note      Mirrors the heap-fallback path in \c osMemoryPoolNew().
        \note      ISR-unsafe.
    */
#if STK_MEMORY_PLACEMENT_NEW
    explicit BlockMemoryPool(size_t capacity, size_t raw_block_size, const char *name = nullptr);
#endif

    /*! \brief     Destructor.
        \details   Frees heap-allocated storage if \c m_storage_owned is \c true.
                   External storage is never touched.
        \note      Destroying the pool while tasks are blocked in \c Alloc() or
                   \c TimedAlloc() is a logical error (dangling waiters). An assertion
                   is triggered in debug builds inside the \c ConditionVariable destructor.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~BlockMemoryPool();

    /*! \brief     Round a raw block size up to the nearest multiple of \c BLOCK_ALIGN.
        \details   Enforces a minimum of \c BLOCK_ALIGN so the free-list link always
                   fits inside every free block without extra metadata.
        \param[in] raw_size: Requested block size in bytes.
        \return    Aligned block size in bytes (>= \c BLOCK_ALIGN).
        \note      Use this to compute the required external storage buffer size:
                   \code
                   uint8_t buf[N * BlockMemoryPool::AlignBlockSize(sizeof(MyType))];
                   \endcode
    */
    static constexpr size_t AlignBlockSize(size_t raw_size)
    {
        return Max(BLOCK_ALIGN, (raw_size + (BLOCK_ALIGN - 1U)) & ~(BLOCK_ALIGN - 1U));
    }

    /*! \brief     Allocate one block, blocking until one becomes available or the timeout expires.
        \details   If the pool is empty the calling task is suspended via the internal
                   \c ConditionVariable and woken by the next \c Free() call. On return the
                   caller owns the block and must eventually return it via \c Free().
        \param[in] timeout: Maximum time to wait (ticks).
                   \c WAIT_INFINITE - block indefinitely (default).
                   \c NO_WAIT       - return \c nullptr immediately if pool is empty
                                      (identical to \c TryAlloc(); ISR-safe).
        \return    Pointer to an uninitialized block of at least \c GetRawBlockSize() bytes,
                   or \c nullptr if the timeout expired before a block became available.
        \warning   ISR-safe \b only when \a timeout = \c NO_WAIT; ISR-unsafe otherwise.
    */
    void *TimedAlloc(Timeout timeout = WAIT_INFINITE);

    /*! \brief     Allocate one typed block, blocking until one becomes available or the timeout expires.
        \details   Thin typed wrapper around \c TimedAlloc(). Asserts that \c sizeof(T) fits
                   within the aligned block size chosen at construction.
        \param[in] timeout: Maximum time to wait (ticks). Same semantics as \c TimedAlloc().
        \return    Typed pointer, or \c nullptr if the timeout expired.
        \warning   ISR-safe \b only when \a timeout = \c NO_WAIT; ISR-unsafe otherwise.
    */
    template <typename T> T *TimedAllocT(Timeout timeout = WAIT_INFINITE);

    /*! \brief     Allocate one block, blocking indefinitely until one is available.
        \return    Pointer to an uninitialized block. Never returns \c nullptr.
        \warning   ISR-unsafe.
    */
    void *Alloc();

    /*! \brief     Allocate one typed block, blocking indefinitely until one is available.
        \return    Typed pointer. Never returns \c nullptr.
        \warning   ISR-unsafe.
    */
    template <typename T> T *AllocT();

    /*! \brief     Non-blocking allocation attempt.
        \details   Returns a block immediately if the pool is not empty, or \c nullptr
                   if all blocks are currently allocated. Never suspends the calling task.
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

    /*! \brief     Return a previously allocated block to the pool.
        \details   Pushes the block back onto the free-list head in O(1) and wakes
                   exactly one task blocked inside \c Alloc() or \c TimedAlloc(),
                   if any. The woken task is guaranteed to find a free block available.
        \param[in] ptr: Pointer previously returned by \c Alloc(), \c TimedAlloc(), or
                   \c TryAlloc(). Must not be \c nullptr and must belong to this pool
                   instance. Bounds and alignment are validated; failures trigger an
                   assertion in debug builds and return \c false in release builds.
        \return    \c true  - block successfully returned.
                   \c false - \a ptr is \c nullptr, out of range, or misaligned;
                               each case indicates a caller logic error.
        \note      ISR-safe.
        \warning   Double-free is not detected and will silently corrupt the free-list.
                   Null the pointer after \c Free() to prevent this.
    */
    bool Free(void *ptr);

    /*! \brief     Verify that the backing storage is valid and the pool is ready for use.
        \details   Always \c true for pools constructed with external storage.
                   For heap-constructed pools, \c false if \c operator new failed.
                   Must be checked after the heap constructor when operating without
                   exceptions (the typical embedded configuration).
        \return    \c true if the pool is ready for use.
        \note      ISR-safe.
    */
    bool IsStorageValid() const { return (m_storage != nullptr); }

    /*! \brief     Get the total block capacity of the pool.
        \return    Maximum number of blocks that can be simultaneously allocated.
                   Matches \c osMemoryPoolGetCapacity().
        \note      ISR-safe.
    */
    size_t GetCapacity() const { return m_capacity; }

    /*! \brief     Get the aligned block size used internally by the allocator.
        \details   Equal to \c AlignBlockSize(raw_block_size) passed at construction.
                   Always >= \c BLOCK_ALIGN. Matches \c osMemoryPoolGetBlockSize().
        \return    Aligned block size in bytes.
        \note      ISR-safe.
    */
    size_t GetBlockSize() const { return m_block_size; }

    /*! \brief     Get the number of currently allocated (outstanding) blocks.
        \return    Point-in-time snapshot. Matches \c osMemoryPoolGetCount().
        \note      May be stale immediately after return in a multi-task environment.
        \note      ISR-safe on targets where a 16-bit aligned read is a single instruction.
    */
    size_t GetUsedCount() const { return m_used_count; }

    /*! \brief     Get the number of free (available) blocks.
        \return    Point-in-time snapshot of \c GetCapacity() - \c GetUsedCount().
                   Matches \c osMemoryPoolGetSpace().
        \note      ISR-safe.
    */
    size_t GetFreeCount() const { return (m_capacity - m_used_count); }

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

    /*! \brief     Minimum block alignment in bytes: \c sizeof(MemoryBlock).
        \details   Ensures the intrusive free-list pointer stored in the first word of
                   every free block is naturally aligned on all supported STK targets.
    */
    static const uint32_t BLOCK_ALIGN = static_cast<uint32_t>(sizeof(MemoryBlock));

    /*! \brief     Initialise the intrusive free-list across \c m_storage.
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

    uint8_t                 *m_storage;        //!< flat byte array holding all N blocks (owned or external)
    MemoryBlock             *m_free_list;       //!< head of the intrusive free-list (nullptr when pool is empty)
    sync::ConditionVariable  m_cv;             //!< signalled by Free() to wake one task blocked in TimedAlloc()
    size_t                   m_block_size;     //!< aligned block size in bytes (>= BLOCK_ALIGN)
    size_t                   m_capacity;       //!< total number of blocks
    uint16_t                 m_used_count;     //!< number of blocks currently allocated (outstanding)
    bool                     m_storage_owned;  //!< true -> storage is heap-allocated; free in destructor
};

// ---------------------------------------------------------------------------
// Constructors / Destructor
// ---------------------------------------------------------------------------

inline BlockMemoryPool::BlockMemoryPool(size_t capacity, size_t raw_block_size, uint8_t *storage,
                                        size_t storage_size, const char *name)
: m_storage(storage),
  m_free_list(nullptr),
  m_block_size(AlignBlockSize(raw_block_size)),
  m_capacity(capacity),
  m_used_count(0U),
  m_storage_owned(false)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(capacity <= CAPACITY_MAX);
    STK_ASSERT(raw_block_size > 0U);
    STK_ASSERT(storage != nullptr);

    // API contract: caller-supplied buffer must be large enough
    STK_ASSERT(storage_size >= (capacity * m_block_size));

    // in Release builds we ensure capacity which fits storage size, in Debug build the assertion above will be hit
    if ((capacity * m_block_size) > storage_size)
        m_capacity = storage_size / m_block_size;

#if STK_SYNC_DEBUG_NAMES
    SetTraceName(name);
#else
    STK_UNUSED(name);
#endif

    BuildFreeList();
}

#if STK_MEMORY_PLACEMENT_NEW
inline BlockMemoryPool::BlockMemoryPool(size_t capacity, size_t raw_block_size, const char *name)
: m_storage(MemoryAllocator::AllocateArrayT<uint8_t>(capacity * AlignBlockSize(raw_block_size))),
  m_free_list(nullptr),
  m_block_size(AlignBlockSize(raw_block_size)),
  m_capacity(capacity),
  m_used_count(0U),
  m_storage_owned(true)
{
    STK_ASSERT(capacity > 0U);
    STK_ASSERT(capacity <= CAPACITY_MAX);
    STK_ASSERT(raw_block_size > 0U);

#if STK_SYNC_DEBUG_NAMES
    SetTraceName(name);
#else
    STK_UNUSED(name);
#endif

    if (m_storage != nullptr)
        BuildFreeList();
    // else: m_free_list remains nullptr; caller must check IsStorageValid()
}
#endif

inline BlockMemoryPool::~BlockMemoryPool()
{
    // ConditionVariable destructor asserts the wait list is empty
#if STK_MEMORY_PLACEMENT_NEW
    if (m_storage_owned)
    {
        MemoryAllocator::FreeArrayT(m_storage, (m_capacity * m_block_size));
        m_storage = nullptr;
    }
#endif
}

// ---------------------------------------------------------------------------
// Alloc / TimedAlloc / TryAlloc
// ---------------------------------------------------------------------------

inline void *BlockMemoryPool::TimedAlloc(Timeout timeout)
{
    if (hw::IsInsideISR() && (timeout != NO_WAIT))
    {
        STK_ASSERT(false); // API contract: ISR callers must pass NO_WAIT / use TryAlloc()
        return nullptr;
    }

    sync::ScopedCriticalSection cs_;

    while (m_free_list == nullptr)
    {
        // Atomically release the critical section, suspend the task, and
        // re-acquire before returning - no CPU cycles wasted while waiting.
        if (!m_cv.Wait(cs_, timeout))
            return nullptr; // timeout expired
    }

    return PopFreeList();
}

template <typename T>
inline T *BlockMemoryPool::TimedAllocT(Timeout timeout)
{
    STK_ASSERT(sizeof(T) <= m_block_size); // API contract: block size should larger or equal to object's size

    return static_cast<T *>(TimedAlloc(timeout));
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
// Free
// ---------------------------------------------------------------------------

inline bool BlockMemoryPool::Free(void *ptr)
{
    if (ptr == nullptr)
        return false;

    // bounds check: ptr must be in range [m_storage, m_storage + capacity * block_size)
    const uint8_t *p8 = static_cast<uint8_t *>(ptr);
    const uint8_t *lo = m_storage;
    const uint8_t *hi = m_storage + (m_capacity * m_block_size);

    if ((p8 < lo) || (p8 >= hi))
    {
        STK_ASSERT(false); // API contract: ptr does not belong to this pool
        return false;
    }

    // alignment check: ptr must be at the start of a block boundary
    if ((static_cast<size_t>(p8 - lo) % m_block_size) != 0U)
    {
        STK_ASSERT(false); // API contract: ptr is misaligned (not a block start)
        return false;
    }

    sync::ScopedCriticalSection cs_;

    if (m_used_count == 0U)
    {
        STK_ASSERT(false); // pool is already fully free - definite double-free
        return false;
    }

#if defined(_DEBUG) || defined(DEBUG)
    // O(n) double-free detection: walk the free-list and check if ptr is already on it,
    // only active in debug builds - compiles away completely in release
    for (const MemoryBlock *node = m_free_list; (node != nullptr); node = node->next)
    {
        if (node == reinterpret_cast<const MemoryBlock *>(ptr))
        {
            STK_ASSERT(false); // double-free: ptr is already on the free-list
            return false;
        }
    }
#endif

    // push block onto free-list head (O(1))
    auto *blk   = reinterpret_cast<MemoryBlock *>(ptr);
    blk->next   = m_free_list;
    m_free_list = blk;
    m_used_count = static_cast<uint16_t>(m_used_count - 1U);

    // wake one blocked allocator - true scheduler wait, no spin-yield
    m_cv.NotifyOne_CS();

    return true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

inline void BlockMemoryPool::BuildFreeList()
{
    STK_ASSERT((hw::PtrToWord(m_storage) % BLOCK_ALIGN) == 0U);

    m_free_list  = nullptr;
    m_used_count = 0U;

    // Link blocks in reverse order so m_free_list ends up pointing at block_0
    // (lowest address), giving ascending allocation order.
    for (size_t i = m_capacity; i-- > 0U; )
    {
        MemoryBlock *blk = reinterpret_cast<MemoryBlock *>(m_storage + (i * m_block_size));

        blk->next   = m_free_list;
        m_free_list = blk;
    }
}

inline void *BlockMemoryPool::PopFreeList()
{
    STK_ASSERT(m_used_count < m_capacity);

    MemoryBlock *blk = m_free_list;

    m_free_list  = blk->next;
    m_used_count = static_cast<uint16_t>(m_used_count + 1U);

    return blk;
}

} // namespace memory
} // namespace stk

#endif /* STK_MEMORY_BLOCKPOOL_H_ */
