/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_MEMORY_ALLOCATOR_H_
#define STK_MEMORY_ALLOCATOR_H_

#include "stk_defs.h"

/*! \def   STK_MEMORY_PLACEMENT_NEW
    \brief When defined as 1, placement new C++ operator is enabled for memory::MemoryAllocator
           and for the NewT/DeleteT helpers of the memory pools.
    \see   memory::MemoryAllocator::AllocateOneT, memory::MemoryAllocator::AllocateArrayT
*/
#ifndef STK_MEMORY_PLACEMENT_NEW
    #define STK_MEMORY_PLACEMENT_NEW (1)
#endif

/*! \def   STK_MEMORY_NO_HEAP
    \brief When defined as 1, nothing in the memory module references memory::MemoryAllocator::Allocate()
           or memory::MemoryAllocator::Free(): the heap-storage constructors of the pools and the
           MemoryAllocator typed/aligned helpers are removed from the build. Use it to prove
           (and enforce) a zero-heap configuration, e.g. for safety audits. Pools remain usable
           with external storage (including StaticBlockPool and StaticBytePool).
    \note  Independent from STK_MEMORY_PLACEMENT_NEW, which only controls availability of placement new.
*/
#ifndef STK_MEMORY_NO_HEAP
    #define STK_MEMORY_NO_HEAP (0)
#endif

/*! \def   STK_MEMORY_HAS_HEAP
    \brief Derived (do not define): 1 if heap-backed features are compiled in.
*/
#if STK_MEMORY_PLACEMENT_NEW && !STK_MEMORY_NO_HEAP
    #define STK_MEMORY_HAS_HEAP (1)
#else
    #define STK_MEMORY_HAS_HEAP (0)
#endif

/*! \def   STK_MEMORY_POOL_CHECKS
    \brief When 1, memory pools perform expensive diagnostics: O(n) double-free detection,
           payload fill patterns (0xCD on allocation, 0xDD on free) and block-start validation.
           Defaults to 1 if _DEBUG or DEBUG is defined, otherwise 0. Because those macros are not
           reliably defined by embedded toolchains (e.g. -Og), define this one explicitly in your
           build system to control the behavior.
*/
#ifndef STK_MEMORY_POOL_CHECKS
    #if defined(_DEBUG) || defined(DEBUG)
        #define STK_MEMORY_POOL_CHECKS (1)
    #else
        #define STK_MEMORY_POOL_CHECKS (0)
    #endif
#endif

/*! \def   STK_MEMORY_OVERRIDE_NEW_DELETE
    \brief When defined as 1, the global C++ \c operator new / \c new[] / \c delete / \c delete[] (all standard
           variants: throwing, \c std::nothrow, sized, and the C++17 over-aligned ones) are replaced with
           versions that use MemoryAllocator::Allocate() / Free() (/ AllocateAligned() / FreeAligned()),
           so plain \c new, \c delete, \c std::string, \c std::vector, \c std::make_unique etc. draw from
           STK's allocator. Disabled by default.
    \details Behavior follows the C++ standard where the platform allows it:
           - \c new(0) returns a unique non-null pointer;
           - on failure the installed \c std::new_handler is called and the allocation retried until it
             succeeds or there is no handler;
           - then throwing \c new throws \c std::bad_alloc, or - when exceptions are disabled (typical for
             embedded) - invokes the no-return hook \c STK_MEMORY_NEW_OOM_ACTION() (default \c std::terminate(),
             same as libstdc++);
             the \c std::nothrow forms return \c nullptr.
    \note   The replacement functions are emitted \b weak in every translation unit that includes this header
           with the macro enabled (the standard forbids \c inline replacements), so it is safe to enable the
           macro in the build system. A strong definition of any of these operators elsewhere in the program
           takes precedence.
    \note   Subject to the MemoryAllocator contract (thread-safety, no ISR use unless your implementation
           allows it). The backend MemoryAllocator::Weak::Allocate() must not itself use \c new (infinite recursion).
           Requires heap support (\c STK_MEMORY_NO_HEAP must be 0).
*/
#ifndef STK_MEMORY_OVERRIDE_NEW_DELETE
    #define STK_MEMORY_OVERRIDE_NEW_DELETE (0)
#endif

#if STK_MEMORY_OVERRIDE_NEW_DELETE && !STK_MEMORY_HAS_HEAP
    #error "STK_MEMORY_OVERRIDE_NEW_DELETE requires heap support: STK_MEMORY_PLACEMENT_NEW=1 and STK_MEMORY_NO_HEAP=0"
#endif

/*! \def   STK_MEMORY_NEW_OOM_ACTION
    \brief Action taken by the replaced throwing \c operator new when memory is exhausted and exceptions are
           disabled. Define as a function-like macro, e.g. <tt>#define STK_MEMORY_NEW_OOM_ACTION() MyPanic()</tt>.
           It \b must not return (panic, reset, log-and-halt...): a throwing \c new never yields \c nullptr and
           compilers rely on that, so if the action does return, \c std::terminate() is called right after it.
           Default: \c std::terminate().
    \note   Not used if exceptions are enabled (\c std::bad_alloc is thrown instead).
*/
#if STK_MEMORY_OVERRIDE_NEW_DELETE
    #include <new>
    #include <exception>
    #ifndef STK_MEMORY_NEW_OOM_ACTION
        #define STK_MEMORY_NEW_OOM_ACTION() std::terminate()
    #endif
    #if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        #define STK_MEMORY_NEW_HAS_EXCEPTIONS (1)
    #else
        #define STK_MEMORY_NEW_HAS_EXCEPTIONS (0)
    #endif
#endif

#if STK_MEMORY_PLACEMENT_NEW
#include <type_traits>
#include <new>
#endif

/*! \file  stk_memory_allocator.h
    \brief Memory allocator front-end with checked API and its weak backend hooks: stk::memory::MemoryAllocator.
*/

namespace stk {
namespace memory {

/*! \struct MemoryAllocator
    \brief Memory allocator for allocating dynamic memory.
    \note  STK does not use dynamic memory allocation but some auxiliary classes may provide such
           functionality for convenience. In this case you must provide your own implementation
           of the backend hooks MemoryAllocator::Weak::Allocate() and MemoryAllocator::Weak::Free()
           (and optionally MemoryAllocator::Weak::GetStats()), or include the optional
           stk_memory_allocator_default.h in exactly one translation unit to get a ready-made,
           deterministic implementation backed by a static BytePool.

    \par Two layers
         - \b Backend: MemoryAllocator::Weak (weak, implemented by the application or by one of the
           stk_memory_allocator_*.h headers). It is NOT meant to be called directly.
         - \b Front-end: MemoryAllocator::Allocate(), Free(), GetStats() and all typed/aligned helpers.
           They verify that a backend is linked in (see IsAvailable()) and fail gracefully if it is not.
           Application code must use only this layer.

    \warning The Weak hooks are declared weak and STK does not define them.
             If you provide none of them, on most toolchains the symbols resolve to address 0
             and calling them is a jump to null (a fault at run time), \b not a linker error.
             The front-end asserts (debug) and fails gracefully in that case (Allocate() returns nullptr,
             Free() is a noop, GetStats() returns empty Stats); use IsAvailable() to check explicitly.

    \par Contract for backend implementations (MemoryAllocator::Weak)
         - Weak::Allocate() must return memory aligned to at least \c MIN_ALIGNMENT bytes.
         - Weak::Allocate() and Weak::Free() must be thread-safe. If you wrap a C library \c malloc (e.g. newlib)
           make sure the lock hooks (\c __malloc_lock) are implemented.
         - Unless your implementation says otherwise, Allocate() and Free() must NOT be called from an ISR.
         - Weak::Allocate(0) may return nullptr.
         - Weak::Free(nullptr) must be a noop.

    Example of your trivial implementation:
    \code
    void *MemoryAllocator::Weak::Allocate(size_t size)
    {
        return malloc(size);
    }
    void MemoryAllocator::Weak::Free(void *ptr)
    {
        free(ptr);
    }
    \endcode

    \note With \c STK_MEMORY_OVERRIDE_NEW_DELETE=1 the global \c new / \c delete operators are routed to this
          allocator as well (see the macro for the exact semantics).

    \note Need memory from a specific region (CCM/TCM, SDRAM, DMA-capable RAM)? The single global
          allocator cannot express that. Create a pool over a buffer placed in that region with the
          external-storage constructors (or StaticBlockPool/StaticBytePool with a linker section attribute).
*/
struct MemoryAllocator
{
    /*! \def     STK_MIN_ALIGNMENT
        \brief   Minimum alignment (bytes) guaranteed for pointers returned by Allocate().
        \details Defaults to the larger of the toolchain's fundamental alignment
                 (alignof(std::max_align_t)) and the platform stack alignment
                 (STK_STACK_MEMORY_ALIGN, 16 on RISC-V). Can be overridden in stk_config.h
                 when the underlying allocator guarantees more. Overriding to a smaller
                 value is rejected at compile time.
    */
#ifndef STK_MIN_ALIGNMENT
    #define STK_MIN_ALIGNMENT (stk::Max<size_t>(alignof(std::max_align_t), STK_STACK_MEMORY_ALIGN))
#endif

    /*! \brief   Minimum alignment (bytes) guaranteed for pointers returned by Allocate().
        \details Types with a stricter alignment must be allocated with AllocateAligned().
        \note    The allocator must deliver this: the heap base, the block header size and
                 the rounding of block sizes must all be multiples of this value.
    */
    static constexpr size_t MIN_ALIGNMENT = STK_MIN_ALIGNMENT;

    STK_STATIC_ASSERT_DESC(IsPow2(MIN_ALIGNMENT), "MIN_ALIGNMENT must be a power of two");
    STK_STATIC_ASSERT_DESC(MIN_ALIGNMENT >= alignof(std::max_align_t),
        "MIN_ALIGNMENT is weaker than the toolchain's fundamental alignment");
    STK_STATIC_ASSERT_DESC(MIN_ALIGNMENT >= sizeof(void *),
         "MIN_ALIGNMENT must be at least pointer-aligned");

    /*! \struct Stats
        \brief Snapshot of the memory allocator's statistics.
        \note  The Record*() methods are not atomic: they must be called with the allocator lock held
               (e.g. inside the critical section that protects the allocator state).
        \note  BlockMemoryPool and BytePool keep a live instance of this structure and return a copy of it
               from their GetStats(), so all allocators report identical semantics.
    */
    struct Stats
    {
        Stats(size_t capacity_ = 0U)
            : capacity(capacity_), allocated(0U), peak_allocated(0U), allocate_count(0U), free_count(0U),
              alloc_fail_count(0U), min_ever_free(capacity_)
        {}

        size_t          capacity;         //!< Total capacity of the memory pool in bytes.
        volatile size_t allocated;        //!< Number of bytes currently allocated (dynamic value).
        volatile size_t peak_allocated;   //!< Maximum number of bytes that were allocated at the same time (high watermark).
        volatile size_t allocate_count;   //!< Number of successful allocations with Allocate().
        volatile size_t free_count;       //!< Number of successful frees with Free().
        volatile size_t alloc_fail_count; //!< Number of allocation requests that could not be satisfied.
        volatile size_t min_ever_free;    //!< Minimum free bytes recorded since system start (watermark).

        /*! \brief     Restart the statistics for a pool of the given capacity.
            \details   Clears all counters and watermarks. Used by pools that learn their real capacity only
                       after aligning/trimming their storage.
            \param[in] capacity_: Total capacity in bytes.
        */
        void Reset(size_t capacity_)
        {
            capacity         = capacity_;
            allocated        = 0U;
            peak_allocated   = 0U;
            allocate_count   = 0U;
            free_count       = 0U;
            alloc_fail_count = 0U;
            min_ever_free    = capacity_;
        }

        /*! \brief     Get total capacity of the memory pool.
            \return    Capacity in bytes.
        */
        size_t GetCapacity() const { return capacity; }

        /*! \brief     Get number of bytes currently available for allocation.
            \return    Available bytes.
        */
        size_t GetAvailable() const
        {
            const size_t used = allocated;
            return ((used < capacity) ? (capacity - used) : 0U);
        }

        /*! \brief     Record successful allocation.
            \param[in] size: Size of allocated memory chunk.
        */
        void RecordAllocate(size_t size)
        {
            const size_t used = (allocated + size);

            allocated      = used;
            allocate_count = (allocate_count + 1U);

            if (used > peak_allocated)
            {
                peak_allocated = used;
            }

            const size_t avail = GetAvailable();
            if (avail < min_ever_free)
            {
                min_ever_free = avail;
            }
        }

        /*! \brief     Record successful deallocation.
            \param[in] size: Size of freed memory chunk.
        */
        void RecordFree(size_t size)
        {
            const size_t used = allocated;

            STK_ASSERT(used >= size);

            allocated  = ((used >= size) ? (used - size) : 0U);
            free_count = (free_count + 1U);
        }

        /*! \brief     Record failed allocation request.
        */
        void RecordAllocFail()
        {
            alloc_fail_count = (alloc_fail_count + 1U);
        }
    };

    /*! \struct  Weak
        \brief   Weak backend hooks of the allocator: the part that the application (or one of the
                 stk_memory_allocator_*.h headers) implements.
        \details STK never defines these functions. If no definition is linked in, on most toolchains the
                 symbols resolve to address 0 and a call is a jump to null (a run-time fault), not a
                 linker error.
        \warning Do NOT call these functions directly from application code: they are unguarded.
                 Use the checked front-end instead: MemoryAllocator::Allocate(), Free() and GetStats().
        \note    Allocate() and Free() are required; GetStats() is optional (the front-end returns empty
                 Stats if it is not provided).
        \see     IsAvailable(), Allocate(), Free(), GetStats()
    */
    struct Weak
    {
        /*! \brief     Allocate the memory chunk (backend hook).
            \param[in] size: Size of the memory chunk.
            \return    Pointer to the allocated memory chunk (aligned to at least MIN_ALIGNMENT),
                       nullptr if allocator failed to allocate it.
        */
        static void *Allocate(size_t size) __stk_weak;

        /*! \brief     Free the memory chunk (backend hook).
            \param[in] ptr: Pointer to the memory chunk. nullptr is allowed and results in noop.
        */
        static void Free(void *ptr) __stk_weak;

        /*! \brief     Get stats of memory allocation (backend hook, optional).
            \return    Stats structure.
        */
        static Stats GetStats() __stk_weak;
    };

    /*! \brief     Check that Weak::Allocate() and Weak::Free() were provided by the application.
        \return    true if both hooks have an implementation linked in.
    */
    static bool IsAvailable()
    {
        return (hw::PtrToWord(&Weak::Allocate) != 0U) &&
               (hw::PtrToWord(&Weak::Free) != 0U);
    }

    /*! \brief     Allocate the memory chunk (checked).
        \details   Forwards to Weak::Allocate() if a backend is linked in. Asserts (debug) and returns
                   nullptr otherwise, so it is safe to call even if the application provided no backend.
        \param[in] size: Size of the memory chunk.
        \return    Pointer to the allocated memory chunk (aligned to at least MIN_ALIGNMENT),
                   nullptr if the allocator failed to allocate it or no backend is available.
    */
    static inline void *Allocate(size_t size)
    {
        void *ptr = nullptr;

        // guard against unlinked weak symbols
        const bool available = IsAvailable();
        STK_ASSERT(available);

        if (available)
        {
            ptr = Weak::Allocate(size);
        }

        return ptr;
    }

    /*! \brief     Free the memory chunk (checked).
        \details   Forwards to Weak::Free() if a backend is linked in. Asserts (debug) and does nothing
                   otherwise.
        \param[in] ptr: Pointer to the memory chunk. nullptr is allowed and results in noop.
    */
    static inline void Free(void *ptr)
    {
        // guard against unlinked weak symbols
        const bool available = IsAvailable();
        STK_ASSERT(available);

        if (available)
        {
            Weak::Free(ptr);
        }
    }

    /*! \brief     Get stats of memory allocation (checked).
        \details   Forwards to Weak::GetStats() if the application provided it. Unlike Allocate()/Free(),
                   statistics are optional: no assertion is raised if the hook is missing.
        \return    Stats structure, default-constructed (all zeros) if the backend provides no statistics.
    */
    static inline Stats GetStats()
    {
        return ((hw::PtrToWord(&Weak::GetStats) != 0U) ? Weak::GetStats() : Stats());
    }

#if STK_MEMORY_HAS_HEAP

    /*! \brief     Allocate memory with an alignment stricter than MIN_ALIGNMENT and an optional offset.
        \details   Over-allocates and stores the original pointer right below the aligned block
                   (rounded down to a pointer boundary, see layout below).
                   The result \b must be released with FreeAligned(), never with Free().
        \param[in] size: Size of the memory chunk in bytes.
        \param[in] alignment: Required alignment, a power of two (valid values below sizeof(void *) are raised to it).
        \param[in] offset: Offset from the alignment boundary, i.e. (result + offset) is aligned.
        \return    Aligned pointer, nullptr on failure or if arguments are invalid.

        Layout:
            raw                                  aligned
            |<-- pad -->|<-- raw ptr -->|<-space->|<-- size bytes -->
                        ^ block_start
            where space = (-offset) mod sizeof(void *), which keeps block_start pointer-aligned
            and lets FreeAligned() recover it from the low bits of 'aligned'.
    */
    static inline void *AllocateAligned(size_t size, size_t alignment, size_t offset = 0U)
    {
        void *result = nullptr;

        constexpr size_t ptr_size = sizeof(void *);
        constexpr size_t max_size = static_cast<size_t>(~static_cast<size_t>(0U));

        const bool pow2         = IsPow2(alignment);
        const bool offset_valid = ((offset == 0U) || (offset < size));

        STK_ASSERT(pow2);

        // note: Allocate() below is the checked front-end, it returns nullptr if no backend is linked in
        if (pow2 && offset_valid)
        {
            const size_t adjusted_alignment = Max(alignment, ptr_size);

            // bytes between the metadata slot and the aligned block, keeps the slot pointer-aligned
            const size_t space         = (0U - offset) & (ptr_size - 1U);
            const size_t private_space = ptr_size + space + (adjusted_alignment - 1U);

            if (size <= (max_size - private_space))
            {
                uint8_t *const raw = static_cast<uint8_t *>(Allocate(private_space + size));

                if (raw != nullptr)
                {
                    // lowest address that leaves room for the metadata slot, with (aligned + offset) on the boundary
                    const Word lowest  = hw::PtrToWord(raw) + static_cast<Word>(ptr_size + space + offset);
                    const Word aligned = AlignPow2(lowest, static_cast<Word>(adjusted_alignment)) - static_cast<Word>(offset);

                    // cannot wrap: 'aligned' stays within [raw + ptr_size + space, raw + private_space]
                    void **const block_start = hw::WordToPtr<void *>(aligned - static_cast<Word>(space + ptr_size));

                    *block_start = raw;
                    result       = hw::WordToPtr<uint8_t>(aligned);
                }
            }
        }

        return result;
    }

    /*! \brief     Free memory obtained with AllocateAligned().
        \param[in] ptr: Pointer returned by AllocateAligned(). nullptr is allowed and results in noop.
    */
    static inline void FreeAligned(void *ptr)
    {
        constexpr size_t ptr_size = sizeof(void *);

        if (ptr != nullptr)
        {
            const Word ptr_word = hw::PtrToWord(ptr);

            // 'space' survives in the low bits because the effective alignment is >= sizeof(void *)
            const Word space = ptr_word & static_cast<Word>(ptr_size - 1U);

            // retrieve the original raw pointer stored below the block
            void **const block_start = hw::WordToPtr<void *>(ptr_word - space - static_cast<Word>(ptr_size));

            Free(*block_start);
        }
    }

    /*! \brief     Allocate a single element and construct it in-place.
        \details   If \a args is empty and TElement is trivially default constructible, no construction
                   is performed and the memory is left uninitialized (same as malloc). In every other
                   case (including trivially copyable types constructed from arguments) the element
                   is constructed with placement new.
        \param[in] args: Constructor arguments forwarded to TElement.
        \return    Pointer to the constructed element, nullptr if allocation failed.
        \note      Pair with FreeOneT<TElement>() to properly invoke the destructor.
        \note      TElement must not require alignment above MIN_ALIGNMENT (compile-time checked).
    */
    template <typename TElement, typename... TArgs>
    static inline TElement *AllocateOneT(TArgs &&...args)
    {
        STK_STATIC_ASSERT_DESC(alignof(TElement) <= MIN_ALIGNMENT,
            "TElement alignment exceeds MemoryAllocator::MIN_ALIGNMENT");

        // note: Allocate() is the checked front-end, it returns nullptr if no backend is linked in
        TElement *ptr = static_cast<TElement *>(Allocate(sizeof(TElement)));

        if (ptr != nullptr)
        {
            if __stk_constexpr_cpp17 ((sizeof...(TArgs) != 0U) || !std::is_trivially_default_constructible<TElement>::value)
            {
                auto const elm = new (static_cast<void *>(ptr)) TElement(static_cast<TArgs &&>(args)...);
                STK_UNUSED(elm);
            }
        }

        return ptr;
    }

    /*! \brief     Allocate an array of elements and default/copy-construct each one.
        \details   Same construction rules as AllocateOneT(). The arguments are passed to every
                   element as lvalues (copied), never moved from.
        \param[in] count: Number of elements to allocate.
        \param[in] args: Constructor arguments forwarded to every element.
        \return    Pointer to the first element, nullptr if allocation failed, count is 0
                   or count * sizeof(TElement) overflows.
        \note      Pair with FreeArrayT<TElement>() passing the same count to properly invoke destructors.
    */
    template <typename TElement, typename... TArgs>
    static inline TElement *AllocateArrayT(size_t count, TArgs &&...args)
    {
        STK_STATIC_ASSERT_DESC(alignof(TElement) <= MIN_ALIGNMENT,
            "TElement alignment exceeds MemoryAllocator::MIN_ALIGNMENT");

        TElement *ptr = nullptr;

        const size_t max_size = static_cast<size_t>(~static_cast<size_t>(0U));

        // note: Allocate() is the checked front-end, it returns nullptr if no backend is linked in
        if ((count != 0U) && (count <= (max_size / sizeof(TElement))))
        {
            ptr = static_cast<TElement *>(Allocate(count * sizeof(TElement)));
            if (ptr != nullptr)
            {
                if __stk_constexpr_cpp17 ((sizeof...(TArgs) != 0U) || !std::is_trivially_default_constructible<TElement>::value)
                {
                    for (size_t i = 0U; i < count; ++i)
                    {
                        auto const elm = new (static_cast<void *>(&ptr[i])) TElement(args...);
                        STK_UNUSED(elm);
                    }
                }
            }
        }

        return ptr;
    }

    /*! \brief     Allocate an array of trivial elements without constructor overhead.
        \details   Enforces at compile time that TElement is a trivial type via static assertion.
                   Allocates raw memory via Allocate() and leaves it uninitialized (similar to malloc),
                   avoiding any placement-new constructor loops.
        \param[in] count: Number of elements to allocate.
        \return    Pointer to the first element, nullptr if allocation failed, count is 0,
                   or count * sizeof(TElement) overflows.
        \note      Pair with Free() (or FreeArrayT) to release the memory.
    */
    template <typename TElement>
    static inline TElement *AllocateTrivialArrayT(size_t count)
    {
        // ensure the type is strictly trivial at compile time
        STK_STATIC_ASSERT_DESC(std::is_trivial<TElement>::value,
            "TElement must be a trivial type");

        // ensure alignment does not exceed allocator guarantees
        STK_STATIC_ASSERT_DESC(alignof(TElement) <= MIN_ALIGNMENT,
            "TElement alignment exceeds MemoryAllocator::MIN_ALIGNMENT");

        return AllocateArrayT<TElement>(count);
    }

    /*! \brief     Destroy and free a single element allocated via AllocateOneT().
        \param[in] ptr: Pointer to the element. nullptr is allowed and results in noop.
    */
    template <typename TElement>
    static inline void FreeOneT(TElement *ptr)
    {
        if (ptr != nullptr)
        {
            if __stk_constexpr_cpp17 (!std::is_trivially_destructible<TElement>())
            {
                ptr->~TElement();
            }

            Free(ptr);
        }
    }

    /*! \brief     Destroy and free an array allocated via AllocateArrayT().
        \param[in] ptr: Pointer to the first element. nullptr is allowed and results in noop.
        \param[in] count: Number of elements (must match the count passed to AllocateArrayT()).
    */
    template <typename TElement>
    static inline void FreeArrayT(TElement ptr[], size_t count)
    {
        if (ptr != nullptr)
        {
            if __stk_constexpr_cpp17 (!std::is_trivially_destructible<TElement>())
            {
                // destroy in reverse order (mirrors stack unwinding)
                for (size_t i = count; i > 0U; --i)
                {
                    ptr[i - 1].~TElement();
                }
            }
            else
            {
                STK_UNUSED(count);
            }

            Free(ptr);
        }
    }

    /*! \brief     Free an array of trivial elements allocated via AllocateTrivialArrayT().
        \details   Enforces at compile time that TElement is a trivial type via static assertion.
                   Bypasses destructor loops entirely and directly releases the memory via Free().
        \param[in] ptr: Pointer to the first element. nullptr is allowed and results in noop.
    */
    template <typename TElement>
    static inline void FreeTrivialArrayT(TElement ptr[])
    {
        // ensure the type is strictly trivial at compile time
        STK_STATIC_ASSERT_DESC(std::is_trivial<TElement>::value,
            "TElement must be a trivial type");

        // note: count (0) is ignored
        FreeArrayT(ptr, 0);
    }

#endif // STK_MEMORY_HAS_HEAP
};

#if STK_MEMORY_PLACEMENT_NEW

/*! \class   PoolPtr
    \brief   Minimal move-only owning pointer for an object that lives in a memory pool.
    \details Returns the object to its pool (destructor + Free) when it goes out of scope, which
             prevents leaks and double-frees in application code. Works with any pool type that
             provides <tt>void DeleteT(T *)</tt> (BlockMemoryPool, BytePool).
    \code
    stk::memory::PoolPtr<Packet, BlockMemoryPool> pkt = g_Pool.TryMakeT<Packet>(1, 2);
    if (pkt) { pkt->Fill(); }
    // block is returned to g_Pool here
    \endcode
*/
template <typename T, class TPool>
class PoolPtr
{
public:
    PoolPtr() : m_ptr(nullptr), m_pool(nullptr) {}

    /*! \brief     Take ownership of \a ptr which was allocated from \a pool with NewT()/TryNewT()/TimedNewT().
    */
    PoolPtr(T *ptr, TPool *pool) : m_ptr(ptr), m_pool(pool) {}

    PoolPtr(PoolPtr &&other) noexcept : m_ptr(other.m_ptr), m_pool(other.m_pool)
    {
        other.m_ptr  = nullptr;
        other.m_pool = nullptr;
    }

    PoolPtr &operator=(PoolPtr &&other) noexcept
    {
        if (this != &other)
        {
            Reset();

            m_ptr  = other.m_ptr;
            m_pool = other.m_pool;

            other.m_ptr  = nullptr;
            other.m_pool = nullptr;
        }
        return *this;
    }

    PoolPtr(const PoolPtr &) = delete;
    PoolPtr &operator=(const PoolPtr &) = delete;

    ~PoolPtr() { Reset(); }

    /*! \brief     Destroy the object and return its memory to the pool (noop if empty).
    */
    void Reset()
    {
        if ((m_ptr != nullptr) && (m_pool != nullptr))
        {
            m_pool->DeleteT(m_ptr);
        }

        m_ptr  = nullptr;
        m_pool = nullptr;
    }

    /*! \brief     Release ownership without freeing.
        \return    The raw pointer; the caller becomes responsible for pool->DeleteT().
    */
    T *Release()
    {
        T *const ptr = m_ptr;

        m_ptr  = nullptr;
        m_pool = nullptr;

        return ptr;
    }

    T *Get() const { return m_ptr; }
    T *operator->() const { return m_ptr; }
    T &operator*() const { return *m_ptr; }
    explicit operator bool() const { return (m_ptr != nullptr); }

private:
    T     *m_ptr;
    TPool *m_pool;
};

#endif // STK_MEMORY_PLACEMENT_NEW

} // namespace memory
} // namespace stk

#if STK_MEMORY_OVERRIDE_NEW_DELETE

// ---------------------------------------------------------------------------
// Global operator new / delete routed to MemoryAllocator
// ---------------------------------------------------------------------------

namespace stk {
namespace memory {
namespace detail {

/*! \brief     Standard-conforming allocation loop shared by all operator new variants.
    \param[in] size: Requested size (0 is served as 1 so that the result is unique and non-null).
    \param[in] alignment: 0 for the default alignment, otherwise an over-alignment (AllocateAligned()).
    \param[in] throwing: true for the throwing forms: failure raises std::bad_alloc / OOM action.
*/
inline void *NewImpl(std::size_t size, std::size_t alignment, bool throwing)
{
    void *ptr = nullptr;

    const std::size_t n = ((size != 0U) ? size : 1U);

    for (;;)
    {
        // note: both paths go through the checked front-end (nullptr if no backend is linked in)
        ptr = ((alignment != 0U) ? MemoryAllocator::AllocateAligned(n, alignment) : MemoryAllocator::Allocate(n));

        if (ptr != nullptr)
        {
            break;
        }

        // standard behavior: give the installed new_handler a chance to release memory, then retry
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr)
        {
            break;
        }

        handler();
    }

    if ((ptr == nullptr) && throwing)
    {
    #if STK_MEMORY_NEW_HAS_EXCEPTIONS
        throw std::bad_alloc();
    #else
        STK_MEMORY_NEW_OOM_ACTION(); // must not return
        std::terminate();            // ... but never hand a null pointer to a throwing new
    #endif
    }

    return ptr;
}

inline void DeleteImpl(void *ptr, bool aligned)
{
    // note: both paths go through the checked front-end
    if (aligned)
    {
        MemoryAllocator::FreeAligned(ptr);
    }
    else
    {
        MemoryAllocator::Free(ptr);
    }
}

} // namespace detail
} // namespace memory
} // namespace stk

// throwing forms
__stk_weak void *operator new(std::size_t size)   { return stk::memory::detail::NewImpl(size, 0U, true); }
__stk_weak void *operator new[](std::size_t size) { return stk::memory::detail::NewImpl(size, 0U, true); }

// non-throwing forms
__stk_weak void *operator new(std::size_t size, const std::nothrow_t &) noexcept   { return stk::memory::detail::NewImpl(size, 0U, false); }
__stk_weak void *operator new[](std::size_t size, const std::nothrow_t &) noexcept { return stk::memory::detail::NewImpl(size, 0U, false); }

// deallocation
__stk_weak void operator delete(void *ptr) noexcept   { stk::memory::detail::DeleteImpl(ptr, false); }
__stk_weak void operator delete[](void *ptr) noexcept { stk::memory::detail::DeleteImpl(ptr, false); }
__stk_weak void operator delete(void *ptr, const std::nothrow_t &) noexcept   { stk::memory::detail::DeleteImpl(ptr, false); }
__stk_weak void operator delete[](void *ptr, const std::nothrow_t &) noexcept { stk::memory::detail::DeleteImpl(ptr, false); }

#if __cplusplus >= 201402L
// sized deallocation (the size is not needed by MemoryAllocator::Free())
__stk_weak void operator delete(void *ptr, std::size_t) noexcept   { stk::memory::detail::DeleteImpl(ptr, false); }
__stk_weak void operator delete[](void *ptr, std::size_t) noexcept { stk::memory::detail::DeleteImpl(ptr, false); }
#endif

#if defined(__cpp_aligned_new)
// over-aligned forms (C++17): must pair with each other, hence AllocateAligned()/FreeAligned()
__stk_weak void *operator new(std::size_t size, std::align_val_t al)   { return stk::memory::detail::NewImpl(size, static_cast<std::size_t>(al), true); }
__stk_weak void *operator new[](std::size_t size, std::align_val_t al) { return stk::memory::detail::NewImpl(size, static_cast<std::size_t>(al), true); }
__stk_weak void *operator new(std::size_t size, std::align_val_t al, const std::nothrow_t &) noexcept   { return stk::memory::detail::NewImpl(size, static_cast<std::size_t>(al), false); }
__stk_weak void *operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t &) noexcept { return stk::memory::detail::NewImpl(size, static_cast<std::size_t>(al), false); }

__stk_weak void operator delete(void *ptr, std::align_val_t) noexcept   { stk::memory::detail::DeleteImpl(ptr, true); }
__stk_weak void operator delete[](void *ptr, std::align_val_t) noexcept { stk::memory::detail::DeleteImpl(ptr, true); }
__stk_weak void operator delete(void *ptr, std::align_val_t, const std::nothrow_t &) noexcept   { stk::memory::detail::DeleteImpl(ptr, true); }
__stk_weak void operator delete[](void *ptr, std::align_val_t, const std::nothrow_t &) noexcept { stk::memory::detail::DeleteImpl(ptr, true); }
__stk_weak void operator delete(void *ptr, std::size_t, std::align_val_t) noexcept   { stk::memory::detail::DeleteImpl(ptr, true); }
__stk_weak void operator delete[](void *ptr, std::size_t, std::align_val_t) noexcept { stk::memory::detail::DeleteImpl(ptr, true); }
#endif // __cpp_aligned_new

#endif // STK_MEMORY_OVERRIDE_NEW_DELETE

#endif /* STK_MEMORY_ALLOCATOR_H_ */
