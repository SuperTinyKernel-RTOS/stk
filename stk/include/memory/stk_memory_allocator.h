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

/*! \def   STK_MEMORY_PLACEMENTNEW
    \brief When defined as 1, placement new C++ operator is enabled for memory::MemoryAllocator.
    \see   memory::MemoryAllocator::AllocateOneT, memory::MemoryAllocator::AllocateArrayT
*/
#ifndef STK_MEMORY_PLACEMENT_NEW
    #define STK_MEMORY_PLACEMENT_NEW 1
#endif

#if STK_MEMORY_PLACEMENT_NEW
#include <type_traits>
#include <new>
#endif

/*! \file  stk_memory_allocator.h
    \brief Weak declaration of memory allocator: stk::memory::MemoryAllocator.
*/

namespace stk {
namespace memory {

/*! \class MemoryAllocator
    \brief Memory allocator for allocating dynamic memory.
    \note  STK does not use dynamic memory allocation but some auxiliary classes may provide such
           functionality for convenience. In this case you must provide your own implementation
           of MemoryAllocator::Allocate() and MemoryAllocator::Free(). By default STK does not
           provide any implementation, therefore you will get a linker error if these two functions
           are left unimplemented.

    Example of your trivial implementation:
    \code
    void *MemoryAllocator::Allocate(size_t size)
    {
        return malloc(size);
    }
    void MemoryAllocator::Free(void *ptr)
    {
        free(ptr);
    }
    \endcode
*/
struct MemoryAllocator
{
    /*! \brief     Allocate the memory chunk.
        \param[in] size: Size of the memory chunk.
        \return    Pointer to the allocated memory chunk, nullptr if allocator failed to allocate it.
    */
    static void *Allocate(size_t size) __stk_weak;

    /*! \brief     Free the memory chunk.
        \param[in] ptr: Pointer to the memory chunk. nullptr is allowed and results in noop.
    */
    static void Free(void *ptr) __stk_weak;

#if STK_MEMORY_PLACEMENT_NEW

    /*! \brief     Allocate a single element and construct it in-place.
        \param[in] args: Constructor arguments forwarded to TElement.
        \return    Pointer to the constructed element, nullptr if allocation failed.
        \note      Pair with FreeOneT<TElement>() to properly invoke the destructor.
    */
    template <typename TElement, typename... TArgs>
    static inline TElement *AllocateOneT(TArgs &&...args)
    {
        STK_ASSERT(Allocate != nullptr);

        TElement *ptr = reinterpret_cast<TElement *>(Allocate(sizeof(TElement)));
        if (ptr != nullptr)
        {
            if (!std::is_trivially_constructible<TElement, TArgs...>())
                new (ptr) TElement(static_cast<TArgs &&>(args)...);
        }

        return ptr;
    }

    /*! \brief     Allocate an array of elements and default/copy-construct each one.
        \param[in] count: Number of elements to allocate.
        \param[in] args: Constructor arguments forwarded to every element.
        \return    Pointer to the first element, nullptr if allocation failed or count is 0.
        \note      Pair with FreeArrayT<TElement>() passing the same count to properly invoke destructors.
    */
    template <typename TElement, typename... TArgs>
    static inline TElement *AllocateArrayT(size_t count, TArgs &&...args)
    {
        if (count == 0)
            return nullptr;

        STK_ASSERT(Allocate != nullptr);

        TElement *ptr = reinterpret_cast<TElement *>(Allocate(count * sizeof(TElement)));
        if (ptr != nullptr)
        {
            if (!std::is_trivially_constructible<TElement, TArgs...>())
            {
                for (size_t i = 0; i < count; ++i)
                    new (&ptr[i]) TElement(static_cast<TArgs &&>(args)...);
            }
        }

        return ptr;
    }

    /*! \brief     Destroy and free a single element allocated via AllocateOne().
        \param[in] ptr: Pointer to the element. nullptr is allowed and results in noop.
    */
    template <typename TElement>
    static inline void FreeOneT(TElement *ptr)
    {
        STK_ASSERT(Free != nullptr);

        if (ptr != nullptr)
        {
            if (!std::is_trivially_destructible<TElement>())
                ptr->~TElement();

            Free(ptr);
        }
    }

    /*! \brief     Destroy and free an array allocated via AllocateT().
        \param[in] ptr: Pointer to the first element. nullptr is allowed and results in noop.
        \param[in] count: Number of elements (must match the count passed to AllocateT()).
    */
    template <typename TElement>
    static inline void FreeArrayT(TElement *ptr, size_t count)
    {
        STK_ASSERT(Free != nullptr);

        if (ptr != nullptr)
        {
            if (!std::is_trivially_destructible<TElement>())
            {
                // destroy in reverse order (mirrors stack unwinding)
                for (size_t i = count; i > 0; --i)
                    ptr[i - 1].~TElement();
            }

            Free(ptr);
        }
    }

#endif // STK_MEMORY_PLACEMENT_NEW
};

} // namespace memory
} // namespace stk

#endif /* STK_MEMORY_ALLOCATOR_H_ */
