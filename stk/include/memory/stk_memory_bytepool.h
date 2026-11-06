/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_MEMORY_BYTEPOOL_H_
#define STK_MEMORY_BYTEPOOL_H_

#include "stk_memory_allocator.h"
#include "sync/stk_sync_cv.h"

/*! \def   STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT
    \brief Payload alignment of BytePool allocations in bytes (a power of two, not less than 4) and the
           granularity of block sizes. Defaults to 8, which keeps \c double / \c uint64_t payloads aligned on
           32-bit targets and satisfies MemoryAllocator::ALIGNMENT.
    \details Unlike BlockMemoryPool this is a compile-time setting, not a constructor argument: it also
           determines the layout of the block header (the payload follows it directly), so
           \c BytePool::HEADER_SIZE becomes \c max(8, alignment).
           - \c 4 (or \c sizeof(void *) on 32-bit targets): less padding per allocation and a 12-byte
             minimum block, but payloads are only 4-byte aligned: typed allocation of 8-byte-aligned types fails
             to compile and the default allocator (stk_memory_allocator_default.h) refuses to build.
           - \c 16, \c 32, \c 64: cache-line / SIMD / DMA aligned payloads. The header grows to the same size, so
             every allocation costs that many bytes of overhead.
    \note  Must have the same value in every translation unit (it changes the class layout), so set it in
           the build system, not in a single source file.
*/
#ifndef STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT
    #ifdef _STK_ARCH_RISC_V
        #define STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT (16U)
    #else
        #define STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT (8U)
    #endif
#endif

/*! \def   STK_MEMORY_BYTEPOOL_GUARD
    \brief When defined as 1, every allocation is followed by a 4-byte guard word that is verified on
           Free(): a payload overrun is reported (assert + Free() returns false, the block is kept
           so the pool metadata is not touched) instead of silently corrupting the next header.
    \note  Costs 4 bytes (rounded up to BLOCK_ALIGN) per allocation. BytePool::RequiredSize() and
           GetMaxRequest() account for it. Off by default.
*/
#ifndef STK_MEMORY_BYTEPOOL_GUARD
    #define STK_MEMORY_BYTEPOOL_GUARD (0)
#endif

/*! \def   STK_MEMORY_BYTEPOOL_NEXT_FIT
    \brief When defined as 1, searches start from where the previous allocation ended (rover) instead of
           from the beginning of the pool. This usually shortens the list walk (and therefore the
           critical section) when many used blocks accumulate at the start of the pool, at the price
           of somewhat higher fragmentation. First-fit (0) is the default.
    \see   BytePool::GetMaxScanLength()
*/
#ifndef STK_MEMORY_BYTEPOOL_NEXT_FIT
    #define STK_MEMORY_BYTEPOOL_NEXT_FIT (0)
#endif

/*! \file  stk_memory_bytepool.h
    \brief Implementation of variable-size memory pool: stk::memory::BytePool.
*/

namespace stk {
namespace memory {

/*! \class BytePool
    \brief Variable-size (first-fit) allocator with block coalescing and proper task-blocking semantics.

    BytePool manages a single contiguous memory area and hands out blocks of arbitrary
    size from it. It is the counterpart of BlockMemoryPool for workloads where allocation
    sizes vary (message payloads, strings, protocol frames), at the price of a non-constant
    allocation time and the possibility of fragmentation.

    The pool keeps an implicit block list inside the storage area. Every block (free or
    used) is preceded by a header (\c HEADER_SIZE, 8 bytes by default) holding the total block size and a state marker.
    Blocks are laid out back to back, so the next block is always found at
    <tt>header + size</tt> and no separate metadata is required.

    Memory layout (pool storage, contiguous):

        [ hdr | payload ][ hdr | payload (free) ][ hdr | payload ] ...
        ^                                                          ^
        m_first                                                    m_end

        Payload pointers returned to the caller are aligned to \c BLOCK_ALIGN (8 bytes by default,
        see \c STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT).

    **Allocation policy**: first-fit (or next-fit with \c STK_MEMORY_BYTEPOOL_NEXT_FIT). While scanning,
    adjacent free blocks are merged, so a failed scan always leaves the pool fully coalesced. A found
    block is split when the remainder is large enough to hold a header and a minimum payload.

    **Freeing**: the block is marked free and merged with the free blocks that follow it.
    Merging with a free block that \b precedes it is done lazily by the next scan, so every
    free run is always reported and allocated as a single block.

    **Three storage modes** allow fully static (zero-heap) deployments:

    | Mode             | Constructor / type                       | Frees storage? |
    |------------------|------------------------------------------|----------------|
    | Embedded storage | \c StaticBytePool<size>                  | n/a            |
    | External storage | \c BytePool(buf, bufsz [, name])         | No - caller    |
    | Heap storage     | \c BytePool(bufsz [, name])              | Yes - dtor     |

    **Blocking semantics**: \c TimedAlloc() suspends the calling task via a
    \c ConditionVariable until enough contiguous memory is released by \c Free(), or the
    timeout expires. All waiters are woken on every \c Free() (their requests differ in size,
    so waking only one could leave a satisfiable request sleeping); each re-checks its own
    request and goes back to sleep if it still does not fit. The original timeout is
    converted to an absolute deadline so repeated wake-ups never extend it.
    With many waiters this wake-up is a "thundering herd": every waiter runs one scan per \c Free().

    **ISR safety**: \c TryAlloc() and \c Free() may be called from an ISR (guarded by a
    critical section) but, unlike BlockMemoryPool, their execution time is \b not constant:
    it is proportional to the number of blocks scanned. Do not use them from time-critical
    ISRs on pools with many blocks. \c Alloc() and \c TimedAlloc() with a non-zero timeout
    must only be called from task context. \c GetMaxScanLength() reports the longest list walk
    observed so far, so the worst-case critical section can be measured on the real workload.

    **Diagnostics**: O(1) counters (used bytes, peak, allocation/free/failure counts, longest scan)
    are available through \c GetStats() and friends; \c GetInfo() adds the O(n) fragmentation picture.
    In builds with \c STK_MEMORY_POOL_CHECKS payloads are filled (0xCD on allocation, 0xDD on free);
    with \c STK_MEMORY_BYTEPOOL_GUARD overruns are caught on \c Free().

    **Object helpers**: \c NewT() / \c TryNewT() / \c TimedNewT() allocate and construct, \c DeleteT()
    destroys and frees, \c TryMakeT() / \c MakeT() return a PoolPtr that frees automatically.

    \code
    // -----------------------------------------------------------------------
    // Example - zero-heap embedded storage
    // -----------------------------------------------------------------------
    stk::memory::StaticBytePool<4096> g_Pool;

    void Task_Producer() {
        char *msg = static_cast<char *>(g_Pool.TimedAlloc(64, 100)); // wait up to 100 ticks
        if (msg) {
            Format(msg, 64);
            g_Queue.Write(msg, NO_WAIT);
        }
    }

    void Task_Consumer() {
        void *msg = nullptr;
        if (g_Queue.Read(msg)) {
            Send(static_cast<const char *>(msg));
            g_Pool.Free(msg);                   // wakes blocked allocators
        }
    }
    \endcode

    \see  BlockMemoryPool, sync::ConditionVariable, StaticBytePool
    \note Blocking alloc paths require the kernel to be compiled with \a KERNEL_SYNC.
          \c TryAlloc() and \c Free() are always available regardless of kernel mode.
    \note A request larger than \c GetMaxRequest() can never succeed and fails immediately
          instead of blocking forever.
    \note Large requests may be starved by a stream of small ones: freed memory is not
          reserved for the longest-waiting task.
    \note Do not create global pool objects with the heap constructor: they run before the heap
          is usable (static initialization order). Use external or embedded storage for globals.
*/
class BytePool : public ITraceable
{
public:
    /*! \brief     Payload alignment and block size granularity in bytes.
        \details   Set with \c STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT (8 by default).
    */
    static const size_t BLOCK_ALIGN = STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT;

    STK_STATIC_ASSERT_DESC((BLOCK_ALIGN >= 4U) && ((BLOCK_ALIGN & (BLOCK_ALIGN - 1U)) == 0U),
        "STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT must be a power of two not less than 4");

    /*! \brief     Size of the per-block header in bytes: \c max(8, BLOCK_ALIGN).
        \details   Every allocation consumes \c HEADER_SIZE bytes of the pool in addition
                   to its (aligned) payload. It is a multiple of \c BLOCK_ALIGN so the payload that
                   directly follows the header is aligned as well.
    */
    static const size_t HEADER_SIZE = ((BLOCK_ALIGN > 8U) ? BLOCK_ALIGN : 8U);

    /*! \brief     Construct a pool backed by \b caller-supplied (external) storage.
        \details   The pool references \a storage directly without taking ownership.
                   The caller must keep the buffer alive for the entire lifetime of
                   this object. The storage is \b not freed on destruction. A misaligned
                   start address is corrected internally (the unaligned head and tail
                   bytes are not used).
        \param[in] storage:      Pointer to a caller-owned byte buffer. Should be aligned
                                 to \c BLOCK_ALIGN to avoid wasting bytes.
        \param[in] storage_size: Size of \a storage in bytes. Must be large enough to hold at
                                 least one header and a minimum payload; otherwise
                                 \c IsStorageValid() returns \c false.
        \param[in] name:         Optional name forwarded to \c ITraceable::SetTraceName().
        \note      ISR-unsafe (must be called from task or init context).
    */
    explicit BytePool(uint8_t *storage, size_t storage_size, const char *name = nullptr);

    /*! \brief     Construct a pool with \b heap-allocated storage.
        \details   Allocates a flat byte buffer of \a storage_size bytes via
                   \c MemoryAllocator::AllocateArrayT (non-throwing). When exceptions are disabled
                   (embedded default), check \c IsStorageValid() immediately after
                   construction to detect allocation failure before first use.
        \param[in] storage_size: Size of the pool area in bytes (headers included).
        \param[in] name:         Optional name forwarded to \c ITraceable::SetTraceName().
        \note      ISR-unsafe. Not available if \c STK_MEMORY_NO_HEAP is 1.
    */
#if STK_MEMORY_HAS_HEAP
    explicit BytePool(size_t storage_size, const char *name = nullptr);
#endif

    /*! \brief     Destructor.
        \details   Frees heap-allocated storage if \c m_storage_owned is \c true.
                   External storage is never touched. Asserts that all blocks were returned.
        \note      Destroying the pool while tasks are blocked in \c Alloc() or
                   \c TimedAlloc() is a logical error (dangling waiters). An assertion
                   is triggered in debug builds inside the \c ConditionVariable destructor.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~BytePool();

    /*! \brief     Round a size up to the nearest multiple of the block alignment (\c BLOCK_ALIGN).
        \param[in] raw_size: Size in bytes.
        \return    Aligned size in bytes.
    */
    static constexpr size_t AlignSize(size_t raw_size)
    {
        return ((raw_size + (BLOCK_ALIGN - 1U)) & ~(BLOCK_ALIGN - 1U));
    }

    /*! \brief     Compute the pool space consumed by a single allocation of \a payload_size bytes.
        \details   Equals the aligned payload (plus the guard word if \c STK_MEMORY_BYTEPOOL_GUARD is 1)
                   plus the block header. Use it to size external storage, adding the value for every
                   block that must be alive at the same time:
                   \code
                   alignas(BytePool::BLOCK_ALIGN) uint8_t buf[BytePool::RequiredSize(128) * 4 + BytePool::RequiredSize(512)];
                   \endcode
        \param[in] payload_size: Requested payload size in bytes.
        \return    Space in bytes taken from the pool.
    */
    static constexpr size_t RequiredSize(size_t payload_size)
    {
        return (AlignSize(payload_size + GUARD_SIZE) + HEADER_SIZE);
    }

    /*! \brief     Allocate a block, preserving the full wait outcome.
        \details   Searches for a free block of at least \a size bytes. If none exists the calling
                   task is suspended via the internal \c ConditionVariable and re-tries after
                   every \c Free() until the request is satisfied, the timeout expires or the
                   wait is cancelled. The timeout is a total budget, not per wake-up.
        \param[out] block: Receives the allocated block on \c WAIT_RESULT_SIGNAL, \c nullptr on
                   any other result. Must not be \c nullptr.
        \param[in] size:   Requested payload size in bytes (must be > 0 and <= \c GetMaxRequest()).
        \param[in] timeout_ticks: Maximum time to wait (ticks).
                   \c WAIT_INFINITE - block indefinitely (default).
                   \c NO_WAIT       - fail immediately if no suitable block exists (ISR-safe).
        \return    \c WAIT_RESULT_SIGNAL   - a block was allocated (immediately or after waiting),
                   \c WAIT_RESULT_TIMEOUT  - no suitable block became available before the timeout
                                             (or \c NO_WAIT was passed); a block freed at the very
                                             moment the timeout expired is still returned as SIGNAL,
                   \c WAIT_RESULT_CANCELED - the wait was cancelled via \c IKernel::CancelTaskWait(),
                   \c WAIT_RESULT_FAIL     - invalid arguments (\a block is \c nullptr, \a size is 0
                                             or exceeds \c GetMaxRequest(), no valid storage) or the
                                             kernel rejected the wait.
        \warning   ISR-safe \b only when \a timeout_ticks = \c NO_WAIT; ISR-unsafe otherwise.
    */
    EWaitResult TimedAllocEx(void **block, size_t size, Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Allocate a block, blocking until memory becomes available or the timeout expires.
        \param[in] size:          Requested payload size in bytes.
        \param[in] timeout_ticks: Maximum time to wait (ticks). Same semantics as \c TimedAllocEx().
        \return    Pointer to an uninitialized block of at least \a size bytes aligned to \c BLOCK_ALIGN,
                   or \c nullptr on timeout, cancellation or invalid arguments.
        \note      Collapses timeout and cancellation. Use \c TimedAllocEx() to tell them apart.
        \warning   ISR-safe \b only when \a timeout_ticks = \c NO_WAIT; ISR-unsafe otherwise.
    */
    void *TimedAlloc(size_t size, Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Allocate one typed block, blocking until memory becomes available or the timeout expires.
        \details   Thin typed wrapper around \c TimedAlloc() for \c sizeof(T) bytes. The memory is
                   uninitialized; no constructor is run (see \c TimedNewT()). \c T must not require
                   alignment above \c BLOCK_ALIGN (compile-time checked).
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \return    Typed pointer, or \c nullptr if the timeout expired.
        \warning   ISR-safe \b only when \a timeout_ticks = \c NO_WAIT; ISR-unsafe otherwise.
    */
    template <typename T> T *TimedAllocT(Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Allocate a block, blocking indefinitely until memory is available.
        \param[in] size: Requested payload size in bytes.
        \return    Pointer to an uninitialized block. Returns \c nullptr only if the wait was
                   cancelled via \c IKernel::CancelTaskWait() or the request is invalid
                   (never on timeout). Use \c TimedAllocEx() to tell these apart.
        \warning   ISR-unsafe.
    */
    void *Alloc(size_t size);

    /*! \brief     Allocate one typed block, blocking indefinitely until memory is available.
        \return    Typed pointer, or \c nullptr (see \c Alloc()).
        \warning   ISR-unsafe.
    */
    template <typename T> T *AllocT();

    /*! \brief     Non-blocking allocation attempt.
        \param[in] size: Requested payload size in bytes.
        \return    Pointer to an uninitialized block, or \c nullptr if no suitable free block exists.
        \note      ISR-safe (execution time is proportional to the number of blocks scanned).
    */
    void *TryAlloc(size_t size);

    /*! \brief     Non-blocking typed allocation attempt.
        \return    Typed pointer, or \c nullptr if no suitable free block exists.
        \note      ISR-safe (see \c TryAlloc()).
    */
    template <typename T> T *TryAllocT();

#if STK_MEMORY_PLACEMENT_NEW

    /*! \brief     Allocate a block for a \c T and construct it (placement new), waiting up to \a timeout_ticks.
        \param[in] timeout_ticks: Maximum time to wait (ticks).
        \param[in] args: Constructor arguments forwarded to \c T.
        \return    Pointer to the constructed object, or \c nullptr.
        \note      Pair with \c DeleteT(). ISR-safe only with \c NO_WAIT (and only if the constructor is).
    */
    template <typename T, typename... TArgs> T *TimedNewT(Timeout timeout_ticks, TArgs &&...args);

    /*! \brief     Allocate a block for a \c T and construct it, blocking indefinitely.
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
    template <typename T, typename... TArgs> PoolPtr<T, BytePool> TryMakeT(TArgs &&...args);

    /*! \brief     Blocking allocate-and-construct returning an owning PoolPtr.
        \warning   ISR-unsafe.
    */
    template <typename T, typename... TArgs> PoolPtr<T, BytePool> MakeT(TArgs &&...args);

#endif // STK_MEMORY_PLACEMENT_NEW

    /*! \brief     Return a previously allocated block to the pool.
        \details   Marks the block free, merges it with the free blocks that follow it and wakes
                   all tasks blocked inside \c Alloc() or \c TimedAlloc() so each can re-check its
                   own request. A woken task may find that the memory was taken by a racing
                   \c TryAlloc() and continue waiting.
        \param[in] ptr: Pointer previously returned by an allocation function of this pool
                   instance. Must not be \c nullptr. Range, alignment and the block state marker
                   are validated (plus the guard word with \c STK_MEMORY_BYTEPOOL_GUARD); failures
                   trigger an assertion in debug builds and return \c false in release builds.
                   With \c STK_MEMORY_POOL_CHECKS the pointer is additionally verified to be the
                   start of a real block.
        \return    \c true  - block successfully returned.
                   \c false - \a ptr is \c nullptr, does not belong to this pool, is misaligned,
                              the block is not in use (double-free) or its guard word was overwritten;
                              each case indicates a caller logic error.
        \note      ISR-safe (see \c TryAlloc()).
        \warning   Double-free detection relies on the block header and is reliable only until
                   the freed memory is handed out again. Null the pointer after \c Free().
    */
    bool Free(void *ptr);

    /*! \brief     Collect pool statistics.
        \details   Walks the block list under a critical section, so the three values are
                   mutually consistent. Adjacent free blocks are counted as one fragment.
        \param[out] available: Total payload bytes that can still be allocated, summed over all
                    free fragments (a single request can use at most \a largest of them).
        \param[out] largest:   Payload size of the largest free fragment, i.e. the biggest request
                    that would succeed right now (less the guard word with \c STK_MEMORY_BYTEPOOL_GUARD).
        \param[out] fragments: Number of free fragments (1 means no external fragmentation).
        \note      ISR-safe, but O(n) in the number of blocks. See \c GetStats() for an O(1) alternative.
    */
    void GetInfo(size_t &available, size_t &largest, size_t &fragments) const;

    /*! \brief     Get a consistent O(1) statistics snapshot in the same format as MemoryAllocator::GetStats().
        \details   \c capacity is the managed area size and \c allocated the bytes held by used blocks,
                   both including headers (i.e. the real pool consumption). \c min_ever_free is derived
                   from the peak, so it is exact in terms of consumed bytes, not of allocatable payload.
        \return    Stats structure.
        \note      ISR-safe.
    */
    MemoryAllocator::Stats GetStats() const;

    /*! \brief     Get the length of the longest block-list walk (in blocks) of any allocation so far.
        \details   The walk is executed inside the critical section, so this is a direct measure of the
                   worst-case time interrupts/scheduling were blocked by the allocator (multiply by the cost
                   of one iteration of your target). Use it to decide whether a time-critical ISR may call
                   \c TryAlloc().
        \note      ISR-safe on targets where an aligned \c size_t read is a single instruction.
    */
    size_t GetMaxScanLength() const { return m_max_scan; }

    /*! \brief     Get the number of allocation requests that failed because no suitable block was
                   available (timeouts, \c NO_WAIT misses and requests larger than \c GetMaxRequest()).
        \note      ISR-safe on targets where an aligned \c size_t read is a single instruction.
    */
    size_t GetAllocFailCount() const { return m_stats.alloc_fail_count; }

    /*! \brief     Get the high watermark of pool consumption in bytes (headers included).
        \note      ISR-safe on targets where an aligned \c size_t read is a single instruction.
    */
    size_t GetPeakUsedSize() const { return m_stats.peak_allocated; }

    /*! \brief     Verify that the backing storage is valid and the pool is ready for use.
        \details   Always \c true for pools constructed with sufficiently large external storage.
                   For heap-constructed pools, \c false if the allocation failed. \c false as well
                   if the supplied area is too small to hold even a single block.
        \return    \c true if the pool is ready for use.
        \note      ISR-safe.
    */
    bool IsStorageValid() const { return (m_first != nullptr); }

    /*! \brief     Get the total size of the managed (aligned) area in bytes, headers included.
        \return    Managed area size in bytes, or 0 if the storage is invalid.
        \note      ISR-safe.
    */
    size_t GetSize() const
    {
        return ((m_first != nullptr) ? static_cast<size_t>(
            reinterpret_cast<const uint8_t *>(m_end) - reinterpret_cast<const uint8_t *>(m_first)) : 0U);
    }

    /*! \brief     Get the largest request that can ever succeed on this pool.
        \details   Equals the payload of a single block spanning the entire pool
                   (less the guard word with \c STK_MEMORY_BYTEPOOL_GUARD).
        \return    Maximum payload size in bytes, or 0 if the storage is invalid.
        \note      ISR-safe.
    */
    size_t GetMaxRequest() const
    {
        const size_t size = GetSize();
        return ((size >= MIN_BLOCK_SIZE) ? (size - HEADER_SIZE - GUARD_SIZE) : 0U);
    }

private:
    STK_NONCOPYABLE_CLASS(BytePool);

    /*! \struct    BlockHeader
        \brief     Header preceding every block (free or used) inside the pool area.
    */
    struct alignas(BLOCK_ALIGN) BlockHeader
    {
        uint32_t size;  //!< total block size including this header (multiple of BLOCK_ALIGN)
        uint32_t state; //!< STATE_FREE / STATE_USED, anything else is treated as corruption
    };

    /*! \brief     Smallest block: a header plus the minimum payload (\c BLOCK_ALIGN).
        \details   Also the smallest remainder worth splitting off.
    */
    static const size_t MIN_BLOCK_SIZE = (HEADER_SIZE + BLOCK_ALIGN);

    /*! \brief     Largest managed area: block sizes are stored as 32-bit values (kept a multiple of BLOCK_ALIGN).
    */
    static const size_t POOL_SIZE_MAX = (static_cast<size_t>(0xFFFFFFFFUL) & ~(BLOCK_ALIGN - 1U));

    /*! \brief     Size of the overrun guard word at the end of every used block (0 = disabled).
    */
    static const size_t GUARD_SIZE = ((STK_MEMORY_BYTEPOOL_GUARD != 0) ? 4U : 0U);

    static const uint32_t GUARD_MAGIC = 0xA5C3E17BUL; //!< value of the guard word of a used block

    static const uint32_t STATE_FREE = 0x46524545UL; //!< "FREE"
    static const uint32_t STATE_USED = 0x55534544UL; //!< "USED"
    static const uint32_t STATE_NONE = 0x00000000UL; //!< stale header absorbed by a merge

    static const uint8_t FILL_ALLOC = 0xCDU; //!< payload fill on allocation (STK_MEMORY_POOL_CHECKS)
    static const uint8_t FILL_FREE  = 0xDDU; //!< payload fill on free (STK_MEMORY_POOL_CHECKS)

#if STK_MEMORY_HAS_HEAP
    /*! \brief     Size of the heap buffer for \a size usable bytes: adds slack for aligning the area start when
                   BLOCK_ALIGN is stricter than the guarantee of MemoryAllocator::Allocate(). 0 on overflow.
    */
    static size_t HeapSize(size_t size)
    {
        const size_t slack = ((BLOCK_ALIGN > MemoryAllocator::MIN_ALIGNMENT) ? (BLOCK_ALIGN - 1U) : 0U);
        return ((size <= (static_cast<size_t>(~static_cast<size_t>(0U)) - slack)) ? (size + slack) : 0U);
    }
#endif

    /*! \brief     Align the area and create the initial single free block.
        \note      Leaves \c m_first and \c m_end as \c nullptr if the area is unusable.
    */
    void Init();

    /*! \brief     Get the header of the block that follows \a h.
        \note      The result may equal \c m_end, which must not be dereferenced.
    */
    static BlockHeader *Next(const BlockHeader *h)
    {
        return reinterpret_cast<BlockHeader *>(const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(h)) + h->size);
    }

    /*! \brief     Get the guard word location of the used block \a h (its last 4 bytes).
    */
    static uint32_t *GuardOf(const BlockHeader *h)
    {
        return reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(h)) + h->size - sizeof(uint32_t));
    }

    /*! \brief     Fill \a size bytes at \a ptr with \a value (used by diagnostics only).
    */
    static __stk_forceinline void Fill(void *ptr, size_t size, uint8_t value)
    {
        STK_MEMSET(ptr, value, size);
    }

    /*! \brief     Check that the header \a h (which must be inside the pool) looks intact.
        \details   Verifies the state marker, the size granularity and that the block does not
                   extend beyond the pool end. Protects the list walkers from endless loops
                   and out-of-bounds reads if a caller overruns a payload.
    */
    bool IsSane(const BlockHeader *h) const;

    /*! \brief     Merge \a h (a free block) with the free blocks that directly follow it.
        \note      Critical section must be held.
    */
    void Coalesce(BlockHeader *h);

    /*! \brief     Find (and claim) the first free block that can hold \a need bytes.
        \param[in] need: Total block size including header (already aligned).
        \return    Payload pointer, or \c nullptr if no free block is large enough.
        \note      Critical section must be held. Coalesces every free run it passes.
    */
    void *FindFree(size_t need);

    /*! \brief     Scan the blocks in [\a from, \a stop) for a free block that can hold \a need bytes.
        \param[in,out] scanned: Incremented for every inspected block.
        \note      Critical section must be held.
    */
    void *ScanRange(BlockHeader *from, const BlockHeader *stop, size_t need, size_t &scanned);

    /*! \brief     Check that \a h is the header of an existing block (walks the list from the start).
        \note      Critical section must be held. O(n), used by STK_MEMORY_POOL_CHECKS builds only.
    */
    bool IsBlockStart(const BlockHeader *h) const;

    /*! \brief     Add one free run of \a run bytes (headers included) to the statistics.
    */
    static void AccountRun(size_t run, size_t &available, size_t &largest, size_t &fragments);

    uint8_t                 *m_storage;        //!< raw storage as supplied or allocated (before alignment)
    BlockHeader             *m_first;          //!< first block header (aligned), nullptr if the pool is invalid
    BlockHeader             *m_end;            //!< one past the last block (aligned), nullptr if the pool is invalid
    BlockHeader             *m_rover;          //!< where the next search starts (STK_MEMORY_BYTEPOOL_NEXT_FIT)
    sync::ConditionVariable  m_cv;             //!< signalled by Free() to wake tasks blocked in TimedAlloc()
    size_t                   m_storage_size;   //!< size of the raw storage in bytes
    MemoryAllocator::Stats   m_stats;          //!< live statistics: bytes of used blocks (headers included), watermark, counters
    size_t                   m_max_scan;       //!< longest block-list walk of a single allocation
    bool                     m_storage_owned;  //!< true -> storage is heap-allocated; free in destructor
};

// ---------------------------------------------------------------------------
// Constructors / Destructor
// ---------------------------------------------------------------------------

inline BytePool::BytePool(uint8_t *storage, size_t storage_size, const char *name)
: m_storage(storage),
  m_first(nullptr),
  m_end(nullptr),
  m_rover(nullptr),
  m_cv(),
  m_storage_size(storage_size),
  m_stats(storage_size),
  m_max_scan(0U),
  m_storage_owned(false)
{
    STK_ASSERT(storage != nullptr);
    STK_ASSERT(storage_size >= MIN_BLOCK_SIZE);

#if STK_SYNC_DEBUG_NAMES
    SetTraceName(name);
#else
    STK_UNUSED(name);
#endif

    Init();
}

#if STK_MEMORY_HAS_HEAP
inline BytePool::BytePool(size_t storage_size, const char *name)
: m_storage(MemoryAllocator::AllocateArrayT<uint8_t>(HeapSize(storage_size))),
  m_first(nullptr),
  m_end(nullptr),
  m_rover(nullptr),
  m_cv(),
  m_storage_size(HeapSize(storage_size)),
  m_stats(storage_size),
  m_max_scan(0U),
  m_storage_owned(true)
{
    STK_ASSERT(storage_size >= MIN_BLOCK_SIZE);

#if STK_SYNC_DEBUG_NAMES
    SetTraceName(name);
#else
    STK_UNUSED(name);
#endif

    // if the allocation failed m_storage is nullptr, Init() leaves m_first as nullptr:
    // the caller must check IsStorageValid()
    Init();
}
#endif

inline BytePool::~BytePool()
{
    // ConditionVariable destructor asserts the wait list is empty
    STK_ASSERT(m_stats.allocated == 0U); // all blocks must be returned before the pool goes away

#if STK_MEMORY_HAS_HEAP
    if (m_storage_owned && (m_storage != nullptr))
    {
        MemoryAllocator::FreeArrayT(m_storage, m_storage_size);
        m_storage = nullptr;
    }
#endif

    m_first = nullptr;
    m_end   = nullptr;
}

// ---------------------------------------------------------------------------
// Alloc / TimedAlloc / TryAlloc
// ---------------------------------------------------------------------------

inline EWaitResult BytePool::TimedAllocEx(void **block, size_t size, Timeout timeout_ticks)
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

        if (hw::IsInsideISR() && (timeout_ticks != NO_WAIT))
        {
            STK_ASSERT(false); // API contract: ISR callers must pass NO_WAIT / use TryAlloc()
        }
        else if ((size == 0U) || (size > GetMaxRequest()))
        {
            // result is WAIT_RESULT_FAIL: such a request can never succeed, so do not block on it;
            // an oversized request on a valid pool still counts as a failed allocation (pool sizing aid)
            if ((size != 0U) && IsStorageValid())
            {
                const hw::CriticalSection::ScopedLock cs_;

                m_stats.RecordAllocFail();
            }
        }
        else
        {
            sync::ScopedCriticalSection cs_;

            // safe from overflow: size <= GetMaxRequest() <= POOL_SIZE_MAX - HEADER_SIZE - GUARD_SIZE
            const size_t need = RequiredSize(size);

            const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

            // capture an absolute deadline once, before entering the wait loop; every Free()
            // wakes all waiters (Mesa-style, not a direct handoff) and a woken task may have
            // to wait again, so re-passing the original timeout_ticks on each retry would
            // silently restart it; kept as 64-bit Ticks (not truncated to Timeout) so it
            // can't wrap on long uptimes
            const Ticks deadline = (timed_wait ? (GetTicks() + static_cast<Ticks>(timeout_ticks)) : 0);

            void *ptr = FindFree(need); // fast path: a suitable block is already available
            result = WAIT_RESULT_SIGNAL;

            while (ptr == nullptr)
            {
                Timeout remaining = timeout_ticks;

                if (timeout_ticks == NO_WAIT)
                {
                    result = WAIT_RESULT_TIMEOUT; // no waiting requested (ISR-safe path)
                    break;
                }

                if (timed_wait)
                {
                    const Ticks left = (deadline - GetTicks());
                    if (left <= 0)
                    {
                        result = WAIT_RESULT_TIMEOUT; // budget is spent and a scan was just made
                        break;
                    }
                    remaining = static_cast<Timeout>(left);
                }

                // Atomically release the critical section, suspend the task, and
                // re-acquire before returning - no CPU cycles wasted while waiting.
                result = m_cv.WaitEx(cs_, remaining);
                if ((result != WAIT_RESULT_SIGNAL) && (result != WAIT_RESULT_TIMEOUT))
                {
                    break; // cancellation or kernel failure: report as is
                }

                // the lock was re-acquired: re-scan regardless of the wake reason, because the memory
                // may have been released at the very moment the timeout expired (honor it instead of
                // failing), or may have been taken by someone else after the wake-up (wait again)
                ptr = FindFree(need);
                if (ptr != nullptr)
                {
                    result = WAIT_RESULT_SIGNAL;
                }
                else if (result == WAIT_RESULT_TIMEOUT)
                {
                    break;
                }
                else
                {
                    // spurious or stolen wake-up: continue waiting within the remaining budget
                }
            }

            if ((ptr == nullptr) && (result == WAIT_RESULT_TIMEOUT))
            {
                m_stats.RecordAllocFail(); // memory exhausted/fragmented (cancellation is not counted)
            }

            *block = ptr;
        }
    }

    return result;
}

inline void *BytePool::TimedAlloc(size_t size, Timeout timeout_ticks)
{
    void *block = nullptr;

    if (TimedAllocEx(&block, size, timeout_ticks) != WAIT_RESULT_SIGNAL)
    {
        block = nullptr;
    }

    return block;
}

template <typename T>
inline T *BytePool::TimedAllocT(Timeout timeout_ticks)
{
    STK_STATIC_ASSERT_DESC(alignof(T) <= BLOCK_ALIGN, "payload is aligned to BLOCK_ALIGN only (STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT)");

    return static_cast<T *>(TimedAlloc(sizeof(T), timeout_ticks));
}

inline void *BytePool::Alloc(size_t size)
{
    return TimedAlloc(size, WAIT_INFINITE);
}

template <typename T>
inline T *BytePool::AllocT()
{
    return TimedAllocT<T>(WAIT_INFINITE);
}

inline void *BytePool::TryAlloc(size_t size)
{
    return TimedAlloc(size, NO_WAIT);
}

template <typename T>
inline T *BytePool::TryAllocT()
{
    return TimedAllocT<T>(NO_WAIT);
}

// ---------------------------------------------------------------------------
// Typed object helpers
// ---------------------------------------------------------------------------

#if STK_MEMORY_PLACEMENT_NEW

template <typename T, typename... TArgs>
inline T *BytePool::TimedNewT(Timeout timeout_ticks, TArgs &&...args)
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
inline T *BytePool::NewT(TArgs &&...args)
{
    return TimedNewT<T>(WAIT_INFINITE, static_cast<TArgs &&>(args)...);
}

template <typename T, typename... TArgs>
inline T *BytePool::TryNewT(TArgs &&...args)
{
    return TimedNewT<T>(NO_WAIT, static_cast<TArgs &&>(args)...);
}

template <typename T>
inline void BytePool::DeleteT(T *ptr)
{
    if (ptr != nullptr)
    {
        ptr->~T();

        const bool freed = Free(ptr);
        STK_UNUSED(freed);
    }
}

template <typename T, typename... TArgs>
inline PoolPtr<T, BytePool> BytePool::TryMakeT(TArgs &&...args)
{
    return PoolPtr<T, BytePool>(TryNewT<T>(static_cast<TArgs &&>(args)...), this);
}

template <typename T, typename... TArgs>
inline PoolPtr<T, BytePool> BytePool::MakeT(TArgs &&...args)
{
    return PoolPtr<T, BytePool>(NewT<T>(static_cast<TArgs &&>(args)...), this);
}

#endif // STK_MEMORY_PLACEMENT_NEW

// ---------------------------------------------------------------------------
// Free
// ---------------------------------------------------------------------------

inline bool BytePool::Free(void *ptr)
{
    bool success = false;

    if ((ptr != nullptr) && (m_first != nullptr))
    {
        // bounds check: payload must be in range [m_first + HEADER_SIZE, m_end)
        const Word pt = hw::PtrToWord(ptr);
        const Word lo = hw::PtrToWord(m_first);
        const Word hi = hw::PtrToWord(m_end);

        if ((pt < (lo + HEADER_SIZE)) || (pt >= hi))
        {
            STK_ASSERT(false); // API contract: ptr does not belong to this pool
        }
        // alignment check: payloads always start on a BLOCK_ALIGN boundary relative to the area start
        else if ((static_cast<size_t>(pt - lo) % BLOCK_ALIGN) != 0U)
        {
            STK_ASSERT(false); // API contract: ptr is misaligned (not a payload start)
        }
        else
        {
            const hw::CriticalSection::ScopedLock cs_;

            BlockHeader *const h = (reinterpret_cast<BlockHeader *>(ptr) - 1);

            if (h->state != STATE_USED)
            {
                STK_ASSERT(false); // block is not in use - double-free or foreign pointer
            }
            else
            {
                bool is_block = true;

            #if STK_MEMORY_POOL_CHECKS
                // O(n) validation that ptr really is the start of a block (and not an address
                // inside some payload which happens to look like a header), only active if
                // STK_MEMORY_POOL_CHECKS is enabled - compiles away completely otherwise
                is_block = IsBlockStart(h);
                STK_ASSERT(is_block);
            #endif

                if (is_block && IsSane(h))
                {
                    bool guard_ok = true;

                #if STK_MEMORY_BYTEPOOL_GUARD
                    // the guard word sits at the very end of the block; if it was overwritten the
                    // caller overran its payload, the following header may be damaged as well: keep
                    // the block (leak it) instead of merging it into a possibly corrupted list
                    guard_ok = (*GuardOf(h) == GUARD_MAGIC);
                    STK_ASSERT(guard_ok);
                #endif

                    if (guard_ok)
                    {
                        const size_t block_size = h->size;

                        m_stats.RecordFree(block_size);

                        h->state = STATE_FREE;

                    #if STK_MEMORY_POOL_CHECKS
                        Fill((h + 1), (block_size - HEADER_SIZE), FILL_FREE);
                    #endif

                        // merge with the free blocks that follow; the ones that precede are merged by the next scan
                        Coalesce(h);

                        // memory became available: waiters have different request sizes, so all of
                        // them must re-evaluate (waking only one could leave a satisfiable request asleep)
                        m_cv.NotifyAll_CS();

                        success = true;
                    }
                }
            }
        }
    }

    return success;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

inline void BytePool::GetInfo(size_t &available, size_t &largest, size_t &fragments) const
{
    available = 0U;
    largest   = 0U;
    fragments = 0U;

    if (m_first != nullptr)
    {
        const hw::CriticalSection::ScopedLock cs_;

        size_t run = 0U; // length of the current run of adjacent free blocks

        for (const BlockHeader *h = m_first; (h < m_end); h = Next(h))
        {
            if (!IsSane(h))
            {
                STK_ASSERT(false); // pool metadata is corrupted (payload overrun?)
                break;
            }

            if (h->state == STATE_FREE)
            {
                run += h->size;
            }
            else
            {
                AccountRun(run, available, largest, fragments);
                run = 0U;
            }
        }

        AccountRun(run, available, largest, fragments);
    }
}

inline MemoryAllocator::Stats BytePool::GetStats() const
{
    const hw::CriticalSection::ScopedLock cs_;

    return m_stats;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

inline void BytePool::Init()
{
    STK_STATIC_ASSERT_DESC(sizeof(BlockHeader) == HEADER_SIZE, "BlockHeader must be exactly HEADER_SIZE bytes");

    if (m_storage != nullptr)
    {
        // align the area start to BLOCK_ALIGN and trim its size to a multiple of BLOCK_ALIGN
        const size_t addr   = static_cast<size_t>(hw::PtrToWord(m_storage));
        const size_t shift  = (AlignSize(addr) - addr);
        size_t       usable = ((m_storage_size > shift) ? ((m_storage_size - shift) & ~(BLOCK_ALIGN - 1U)) : 0U);

        // block sizes are 32-bit: a larger area would be wasted beyond this limit
        if (usable > POOL_SIZE_MAX)
        {
            usable = POOL_SIZE_MAX;
        }

        if (usable >= MIN_BLOCK_SIZE)
        {
            m_first = hw::WordToPtr<BlockHeader>(hw::PtrToWord(m_storage) + shift);
            m_end   = hw::WordToPtr<BlockHeader>(hw::PtrToWord(m_first) + usable);

            m_first->size  = static_cast<uint32_t>(usable);
            m_first->state = STATE_FREE;

            m_rover = m_first;

            m_stats.Reset(usable);
        }
    }
}

inline bool BytePool::IsSane(const BlockHeader *h) const
{
    const bool state_ok = ((h->state == STATE_FREE) || (h->state == STATE_USED));

    return (state_ok &&
            (h->size >= MIN_BLOCK_SIZE) &&
            ((h->size % BLOCK_ALIGN) == 0U) &&
            (static_cast<size_t>(reinterpret_cast<const uint8_t *>(m_end) - reinterpret_cast<const uint8_t *>(h)) >= h->size));
}

inline void BytePool::Coalesce(BlockHeader *h)
{
    for (BlockHeader *n = Next(h); (n < m_end) && (n->state == STATE_FREE) && IsSane(n); n = Next(h))
    {
    #if STK_MEMORY_BYTEPOOL_NEXT_FIT
        // never leave the rover pointing at a header that is about to disappear
        if (m_rover == n)
        {
            m_rover = h;
        }
    #endif

        h->size += n->size;

        // wipe the absorbed header so it can never be mistaken for a live block
        n->state = STATE_NONE;
        n->size  = 0U;
    }
}

inline void *BytePool::ScanRange(BlockHeader *from, const BlockHeader *stop, size_t need, size_t &scanned)
{
    void *result = nullptr;

    for (BlockHeader *h = from; (h != nullptr) && (h < stop) && (result == nullptr); h = Next(h))
    {
        ++scanned;

        if (!IsSane(h))
        {
            STK_ASSERT(false); // pool metadata is corrupted (payload overrun?)
            break;
        }

        if (h->state == STATE_FREE)
        {
            Coalesce(h);

            if (h->size >= need)
            {
                const size_t rest = (h->size - need);

                // split off the tail if it can form a valid block of its own
                if (rest >= MIN_BLOCK_SIZE)
                {
                    BlockHeader *const tail = reinterpret_cast<BlockHeader *>(reinterpret_cast<uint8_t *>(h) + need);
                    tail->size  = static_cast<uint32_t>(rest);
                    tail->state = STATE_FREE;

                    h->size = static_cast<uint32_t>(need);
                }

                h->state = STATE_USED;
                result   = (h + 1);

                m_stats.RecordAllocate(h->size);

            #if STK_MEMORY_POOL_CHECKS
                Fill(result, (h->size - HEADER_SIZE - GUARD_SIZE), FILL_ALLOC);
            #endif

            #if STK_MEMORY_BYTEPOOL_GUARD
                *GuardOf(h) = GUARD_MAGIC;
            #endif

            #if STK_MEMORY_BYTEPOOL_NEXT_FIT
                {
                    BlockHeader *const nxt = Next(h);
                    m_rover = ((nxt < m_end) ? nxt : m_first);
                }
            #endif
            }
        }
    }

    return result;
}

inline void *BytePool::FindFree(size_t need)
{
    size_t scanned = 0U;

#if STK_MEMORY_BYTEPOOL_NEXT_FIT
    BlockHeader *const start = ((m_rover != nullptr) ? m_rover : m_first);

    void *result = ScanRange(start, m_end, need, scanned);

    if ((result == nullptr) && (start != m_first))
    {
        // wrap around: the part before the rover (this also coalesces the free run that precedes it)
        result = ScanRange(m_first, start, need, scanned);
    }
#else
    void *const result = ScanRange(m_first, m_end, need, scanned);
#endif

    if (scanned > m_max_scan)
    {
        m_max_scan = scanned;
    }

    return result;
}

inline bool BytePool::IsBlockStart(const BlockHeader *h) const
{
    bool found = false;

    for (const BlockHeader *b = m_first; (b != nullptr) && (b <= h) && (b < m_end) && IsSane(b); b = Next(b))
    {
        if (b == h)
        {
            found = true;
            break;
        }
    }

    return found;
}

inline void BytePool::AccountRun(size_t run, size_t &available, size_t &largest, size_t &fragments)
{
    if (run != 0U)
    {
        const size_t payload = (run - HEADER_SIZE);

        available += payload;
        ++fragments;

        if (payload > largest)
        {
            largest = payload;
        }
    }
}

// ---------------------------------------------------------------------------
// StaticBytePool - zero-heap pool that embeds its own, correctly aligned storage
// ---------------------------------------------------------------------------

namespace detail {

/*! \brief Storage holder. Kept as a separate (first) base class so that the buffer exists
           before BytePool's constructor writes the first block header into it.
*/
template <size_t TSize>
struct BytePoolStorage
{
    alignas(BytePool::BLOCK_ALIGN) uint8_t m_buf[TSize];
};

} // namespace detail

/*! \class StaticBytePool
    \brief Variable-size pool that owns its storage: no heap and no separately declared, possibly
           misaligned buffer.
    \tparam TSize: Size of the managed area in bytes (headers included). Use BytePool::RequiredSize()
                   to compute it from the blocks that must live at the same time.
    \code
    stk::memory::StaticBytePool<4096> g_Pool("msgs");
    \endcode
    \note Place the object into a specific memory region (CCM, SDRAM, DMA RAM) with your toolchain's
          section attribute on the variable declaration.
*/
template <size_t TSize>
class StaticBytePool : private detail::BytePoolStorage<TSize>,
                       public BytePool
{
    STK_STATIC_ASSERT_DESC(TSize >= (BytePool::HEADER_SIZE + BytePool::BLOCK_ALIGN), "pool size must fit at least one block");
    STK_STATIC_ASSERT_DESC(TSize <= (0xFFFFFFFFUL & ~(BytePool::BLOCK_ALIGN - 1U)), "pool size exceeds the 32-bit block size limit");

public:
    explicit StaticBytePool(const char *name = nullptr)
        : BytePool(this->m_buf, sizeof(this->m_buf), name)
    {}
};

} // namespace memory
} // namespace stk

#endif /* STK_MEMORY_BYTEPOOL_H_ */
