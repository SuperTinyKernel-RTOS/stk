/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_PRIORITY_BITMAP_H_
#define STK_PRIORITY_BITMAP_H_

/*! \file  stk_priority_bitmap.h
    \brief Priority bitmap operations (stk::sched::PriorityBitmapTraits), a multi-word bitmap for more than
           64 priorities (stk::sched::PriorityBitmap) and default bitmap type selection (stk::sched::ReadyBitmapSelect)
           shared by the bitmap-based priority strategies, e.g. stk::sched::SwitchStrategyFixedPriority.
*/

#include "stk_common.h"

namespace stk {
namespace sched {

/*! \class PriorityBitmapTraits
    \brief Operations on the "ready priorities" bitmap used by SwitchStrategyFixedPriority and
           SwitchStrategyPreemptionThreshold.

    The primary template supports any unsigned integer type (uint8_t, uint16_t, uint32_t, uint64_t).
    To support more priority levels than a native integer can hold use stk::sched::PriorityBitmap (96, 128, ...,
    1024 priorities, its traits are provided below), or define your own bitmap class and provide a
    \b full specialization of this template for it. A specialization
    must provide:
    \code
    template <> struct PriorityBitmapTraits<MyBitmap>
    {
        enum { CAPACITY = 128 };                                    // number of priority levels the bitmap can hold
        static void     Set(MyBitmap &b, uint32_t bit);             // set bit
        static void     Clear(MyBitmap &b, uint32_t bit);           // clear bit
        static bool     IsSet(const MyBitmap &b, uint32_t bit);     // true if bit is set
        static bool     IsEmpty(const MyBitmap &b);                 // true if no bit is set
        static uint32_t HighestSet(const MyBitmap &b);              // index of the most significant set bit (b is not empty)
    };
    \endcode
    The bitmap class itself must be default-constructible into the \e empty state (all bits clear),
    because the strategy value-initializes it.

    \note All operations run inside the scheduler (critical section), keep them O(1) or as close
          to it as possible.
*/
template <class BitmapT>
struct PriorityBitmapTraits;

//! \cond INTERNAL
namespace priority_bitmap_detail {

/*! \brief Highest-set-bit scan helper selected by the integer size in bytes (<= 4 bytes). */
template <size_t SIZE>
struct Scan
{
    static __stk_forceinline uint32_t Highest(uint32_t value)
    {
        return 31U - CountLeadingZeros(value);
    }
};

/*! \brief Highest-set-bit scan helper for 8-byte integers: reuses the 32-bit scan on 32-bit halves,
           so no 64-bit compiler builtin is required. */
template <>
struct Scan<8U>
{
    static __stk_forceinline uint32_t Highest(uint64_t value)
    {
        const uint32_t hi = static_cast<uint32_t>(value >> 32U);

        return (hi != 0U) ? (63U - CountLeadingZeros(hi))
                          : (31U - CountLeadingZeros(static_cast<uint32_t>(value)));
    }
};

} // namespace priority_bitmap_detail
//! \endcond

template <class BitmapT>
struct PriorityBitmapTraits
{
    enum { CAPACITY = (sizeof(BitmapT) * 8U) }; //!< Number of priority levels the bitmap can hold.

    //! Set bit \a bit. \note The shift is performed in BitmapT width, so it is valid for any bit < CAPACITY.
    static __stk_forceinline void Set(BitmapT &bitmap, uint32_t bit)
    {
        bitmap = static_cast<BitmapT>(bitmap | static_cast<BitmapT>(static_cast<BitmapT>(1U) << bit));
    }

    //! Clear bit \a bit.
    static __stk_forceinline void Clear(BitmapT &bitmap, uint32_t bit)
    {
        bitmap = static_cast<BitmapT>(bitmap & static_cast<BitmapT>(~static_cast<BitmapT>(static_cast<BitmapT>(1U) << bit)));
    }

    //! Check that bit \a bit is set.
    static __stk_forceinline bool IsSet(const BitmapT &bitmap, uint32_t bit)
    {
        return ((bitmap & static_cast<BitmapT>(static_cast<BitmapT>(1U) << bit)) != static_cast<BitmapT>(0U));
    }

    //! Check that no bit is set.
    static __stk_forceinline bool IsEmpty(const BitmapT &bitmap)
    {
        return (bitmap == static_cast<BitmapT>(0U));
    }

    //! Index of the most significant set bit. \warning Bitmap must not be empty.
    static __stk_forceinline uint32_t HighestSet(const BitmapT &bitmap)
    {
        return priority_bitmap_detail::Scan<sizeof(BitmapT)>::Highest(bitmap);
    }
};

/*! \class PriorityBitmap
    \brief Two-level bitmap for more priority levels than a native integer can hold (BITS is a multiple
           of 32 above 64, e.g. 96, 128, ..., 1024).

    Level 0 is an array of BITS / 32 words (bit \c i lives in word <tt>i / 32</tt>, position <tt>i % 32</tt>).
    Level 1 is a single 32-bit \e summary word where bit \c w is set if and only if word \c w is non-zero.
    The summary keeps every operation O(1) regardless of BITS:
    - Set(): two bit-sets,
    - Clear(): one bit-clear plus one conditional summary clear,
    - IsEmpty(): one compare (summary == 0),
    - HighestSet(): two CountLeadingZeros() calls (summary, then the word it selects), no loops.

    \tparam BITS  Number of bits. Must be a multiple of 32 and at most 1024 (32 words, the summary width).
                  Any such value works (32 and 64 too), but plain \c uint32_t / \c uint64_t are cheaper
                  for up to 64 priorities.

    \note Memory: BITS / 8 + 4 bytes, e.g. 36 bytes for 256 priorities, 132 bytes for 1024.
    \note The default constructor creates an empty bitmap (value-initialization is therefore valid).
    \see  PriorityBitmapTraits
*/
template <uint32_t BITS>
class PriorityBitmap
{
public:
    STK_STATIC_ASSERT_DESC_N(Multiple32, (BITS % 32U) == 0U, "PriorityBitmap: BITS must be a multiple of 32");
    STK_STATIC_ASSERT_DESC_N(Min32,       BITS >= 32U,       "PriorityBitmap: BITS must be at least 32");
    STK_STATIC_ASSERT_DESC_N(Max1024,     BITS <= 1024U,     "PriorityBitmap: BITS must not exceed 1024 (32-bit summary)");

    enum
    {
        CAPACITY = BITS,       //!< Number of bits.
        WORDS    = (BITS / 32U) //!< Number of 32-bit words.
    };

    //! Construct an empty bitmap.
    PriorityBitmap() : m_summary(0U), m_words()
    {}

    //! Set bit \a bit, [0, BITS).
    __stk_forceinline void Set(uint32_t bit)
    {
        STK_ASSERT(bit < BITS);

        const uint32_t word = (bit >> 5U);

        m_words[word] |= (1U << (bit & 31U));
        m_summary     |= (1U << word);
    }

    //! Clear bit \a bit, [0, BITS).
    __stk_forceinline void Clear(uint32_t bit)
    {
        STK_ASSERT(bit < BITS);

        const uint32_t word = (bit >> 5U);

        m_words[word] &= ~(1U << (bit & 31U));

        if (m_words[word] == 0U)
        {
            m_summary &= ~(1U << word);
        }
    }

    //! Check that bit \a bit is set, [0, BITS).
    __stk_forceinline bool IsSet(uint32_t bit) const
    {
        STK_ASSERT(bit < BITS);

        return ((m_words[bit >> 5U] & (1U << (bit & 31U))) != 0U);
    }

    //! Check that no bit is set.
    __stk_forceinline bool IsEmpty() const
    {
        return (m_summary == 0U);
    }

    //! Index of the most significant set bit. \warning Bitmap must not be empty.
    __stk_forceinline uint32_t HighestSet() const
    {
        STK_ASSERT(m_summary != 0U);

        const uint32_t word = (31U - CountLeadingZeros(m_summary));

        STK_ASSERT(m_words[word] != 0U);

        return ((word << 5U) + (31U - CountLeadingZeros(m_words[word])));
    }

private:
    uint32_t m_summary;      //!< Bit w set when m_words[w] != 0.
    uint32_t m_words[WORDS]; //!< Level 0: the bits.
};

//! \cond INTERNAL
template <uint32_t BITS>
struct PriorityBitmapTraits< PriorityBitmap<BITS> >
{
    enum { CAPACITY = BITS };

    static __stk_forceinline void Set(PriorityBitmap<BITS> &bitmap, uint32_t bit)
    {
        bitmap.Set(bit);
    }

    static __stk_forceinline void Clear(PriorityBitmap<BITS> &bitmap, uint32_t bit)
    {
        bitmap.Clear(bit);
    }

    static __stk_forceinline bool IsSet(const PriorityBitmap<BITS> &bitmap, uint32_t bit)
    {
        return bitmap.IsSet(bit);
    }

    static __stk_forceinline bool IsEmpty(const PriorityBitmap<BITS> &bitmap)
    {
        return bitmap.IsEmpty();
    }

    static __stk_forceinline uint32_t HighestSet(const PriorityBitmap<BITS> &bitmap)
    {
        return bitmap.HighestSet();
    }
};
//! \endcond

/*! \class ReadyBitmapSelect
    \brief Selects the default bitmap type for a number of priority levels:
           \c uint32_t for up to 32, \c uint64_t for up to 64, PriorityBitmap<> (rounded up to a multiple
           of 32 bits) above that.
    \tparam MAX_PRIORITIES: Number of priority levels.
*/
template <uint16_t MAX_PRIORITIES, uint32_t KIND = ((MAX_PRIORITIES <= 32U) ? 0U : ((MAX_PRIORITIES <= 64U) ? 1U : 2U))>
struct ReadyBitmapSelect
{
    typedef PriorityBitmap<(((MAX_PRIORITIES + 31U) / 32U) * 32U)> Type;
};

//! \cond INTERNAL
template <uint16_t MAX_PRIORITIES>
struct ReadyBitmapSelect<MAX_PRIORITIES, 0U>
{
    typedef uint32_t Type;
};

template <uint16_t MAX_PRIORITIES>
struct ReadyBitmapSelect<MAX_PRIORITIES, 1U>
{
    typedef uint64_t Type;
};
//! \endcond

} // namespace sched
} // namespace stk

#endif /* STK_PRIORITY_BITMAP_H_ */
