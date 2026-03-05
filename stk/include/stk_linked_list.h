/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_LINKED_LIST_H_
#define STK_LINKED_LIST_H_

/*! \file  stk_linked_list.h
    \brief Intrusive doubly-linked list implementation used internally by the kernel.

    Provides two class templates:
     - DListEntry<_Ty, _ClosedLoop>: the node embedded inside a host object (_Ty).
     - DListHead<_Ty, _ClosedLoop>:  the list container that owns and traverses the nodes.

    The list is \e intrusive: the link pointers live inside the host object itself,
    so no separate heap allocation is required for list membership.
    A single host object may belong to at most one list at a time.

    The \c _ClosedLoop template parameter selects between two structural variants:
     - \c false: open (linear) list — first->prev and last->next are \c NULL.
     - \c true:  closed (circular) list — first->prev points to last and
                 last->next points to first, enabling O(1) wrap-around traversal.
*/

namespace stk {
namespace util {

// Forward declaration required so DListEntry can reference DListHead as a friend
// and store a back-pointer to the owning list head.
template <class _Ty, bool _ClosedLoop> class DListHead;

/*! \class DListEntry
    \brief Intrusive doubly-linked list node. Embed this as a base class in any object (_Ty)
           that needs to participate in a DListHead list.

    \tparam _Ty:         The host class that derives from DListEntry. Used by the implicit
                         conversion operators to safely downcast the node pointer back to the
                         host object pointer without a dynamic_cast.
    \tparam _ClosedLoop: When \c true the list is kept circular (last->next == first and
                         first->prev == last). When \c false the list is linear (boundary
                         pointers are \c NULL). Must match the \c _ClosedLoop of the
                         DListHead this entry will be inserted into.

    \note  An entry that is not currently in any list has \c m_head == \c NULL.
           Use IsLinked() to test membership before calling any DListHead operation.
    \note  A single DListEntry instance may belong to at most one DListHead at a time.
           Inserting a linked entry into a second list without first removing it from the
           first will trigger an assertion.
    \note  Not thread-safe. The caller is responsible for protecting list operations
           with a hw::CriticalSection or equivalent.
*/
template <class _Ty, bool _ClosedLoop> class DListEntry
{
    friend class DListHead<_Ty, _ClosedLoop>;

public:
    /*! \brief Construct an unlinked entry. All pointers initialised to NULL.
    */
    explicit DListEntry() : m_head(nullptr), m_next(nullptr), m_prev(nullptr)
    {}

    /*! \typedef DLEntryType
        \brief   Convenience alias for this entry type. Used to avoid repeating the full template spelling.
    */
    typedef DListEntry<_Ty, _ClosedLoop> DLEntryType;

    /*! \typedef DLHeadType
        \brief   Convenience alias for the corresponding list head type.
    */
    typedef DListHead<_Ty, _ClosedLoop>  DLHeadType;

    /*! \brief  Get the list head this entry currently belongs to.
        \return Pointer to the owning DListHead, or \c NULL if the entry is not linked.
    */
    DLHeadType *GetHead() const  { return m_head; }

    /*! \brief  Get the next entry in the list.
        \return Pointer to the next DListEntry, or \c NULL if this is the last entry
                (open list) or the first entry (closed loop, where next wraps to first).
        \note   In a closed loop (\c _ClosedLoop == true) this pointer is never \c NULL
                when the entry is linked.
    */
    DLEntryType *GetNext() const { return m_next; }

    /*! \brief  Get the previous entry in the list.
        \return Pointer to the previous DListEntry, or \c NULL if this is the first entry
                (open list) or the last entry (closed loop, where prev wraps to last).
        \note   In a closed loop (\c _ClosedLoop == true) this pointer is never \c NULL
                when the entry is linked.
    */
    DLEntryType *GetPrev() const { return m_prev; }

    /*! \brief  Check whether this entry is currently a member of any list.
        \return \c true if linked (m_head != NULL); \c false otherwise.
    */
    bool IsLinked() const        { return (GetHead() != nullptr); }

    /*! \brief Implicit conversion to a mutable pointer to the host object (_Ty).
        \note  Safe because _Ty must derive from DListEntry<_Ty, _ClosedLoop>.
               Eliminates the need for explicit static_cast at call sites.
    */
    operator _Ty *()             { return static_cast<_Ty *>(this); }

    /*! \brief Implicit conversion to a const pointer to the host object (_Ty).
    */
    operator const _Ty *() const { return static_cast<const _Ty *>(this); }

protected:
    /*! \brief     Protected non-virtual destructor.
        \note      Non-virtual by design: adding a vtable pointer would increase the size of every
                   kernel object and pull in the C++ runtime. The consequence is that deleting a
                   DListEntry pointer directly (rather than through the host _Ty pointer) will not
                   invoke the host destructor — do not do this.
        \note      An entry should be removed from its list (via DListHead::Unlink) before the
                   host object is destroyed, to keep the list's neighbour pointers consistent.
    */
    ~DListEntry()
    {}

private:
    /*! \brief     Wire this entry into a list between \a prev and \a next.
        \param[in] head: The owning DListHead. Stored as a back-pointer for IsLinked() and ownership checks.
        \param[in] next: The entry that will follow this one, or \c NULL if this becomes the last entry.
        \param[in] prev: The entry that will precede this one, or \c NULL if this becomes the first entry.
        \note      Called exclusively by DListHead::Link(). Assumes the entry is not currently linked.
                   Updates the neighbours' forward/back pointers to splice this entry in.
    */
    void Link(DLHeadType *head, DLEntryType *next, DLEntryType *prev)
    {
        m_head = head;
        m_next = next;
        m_prev = prev;

        if (m_prev != nullptr)
            m_prev->m_next = this;

        if (m_next != nullptr)
            m_next->m_prev = this;
    }

    /*! \brief  Remove this entry from its current list.
        \note   Called exclusively by DListHead::Unlink(). Patches the neighbours' pointers to
                bridge over this entry, then clears m_head, m_next, and m_prev to NULL so the
                entry is in a clean unlinked state.
        \note   Does not update DListHead::m_count or m_first / m_last — those are the
                responsibility of the calling DListHead::Unlink().
    */
    void Unlink()
    {
        if (m_prev != nullptr)
            m_prev->m_next = m_next;

        if (m_next != nullptr)
            m_next->m_prev = m_prev;

        m_head = nullptr;
        m_next = nullptr;
        m_prev = nullptr;
    }

    DLHeadType  *m_head; //!< Owning list head, or \c NULL when the entry is not linked.
    DLEntryType *m_next; //!< Next entry in the list, or \c NULL (open list boundary) / first entry (closed loop).
    DLEntryType *m_prev; //!< Previous entry in the list, or \c NULL (open list boundary) / last entry (closed loop).
};

/*! \class DListHead
    \brief Intrusive doubly-linked list container. Manages a collection of DListEntry nodes
           embedded in host objects of type _Ty.

    \tparam _Ty:         The host class whose instances are stored in this list. Each _Ty must
                         derive from DListEntry<_Ty, _ClosedLoop>.
    \tparam _ClosedLoop: When \c true the list is maintained as a circular ring: after every
                         insertion or removal, last->next is set to first and first->prev is
                         set to last. This allows scheduler algorithms to wrap around the end
                         of the list in O(1) without a branch. When \c false the boundary
                         pointers remain \c NULL (standard open list).

    \note  Complexity: all operations are O(1) except Clear() and RelinkTo() which are O(n).
    \note  An entry may only be in one list at a time. Attempting to insert an already-linked
           entry will trigger an assertion.
    \note  PopBack() and PopFront() call Unlink() internally. Calling them on an empty list
           will pass \c NULL to Unlink() and trigger an assertion. Guard with IsEmpty().
    \note  Not thread-safe. The caller is responsible for protecting concurrent access with
           a hw::CriticalSection or equivalent.
*/
template <class _Ty, bool _ClosedLoop> class DListHead
{
    friend class DListEntry<_Ty, _ClosedLoop>;

public:
    /*! \typedef DLEntryType
        \brief   Convenience alias for the node type stored in this list.
    */
    typedef DListEntry<_Ty, _ClosedLoop> DLEntryType;

    /*! \brief Construct an empty list head (count = 0, first = last = NULL).
    */
    explicit DListHead(): m_count(0), m_first(nullptr), m_last(nullptr)
    {}

    /*! \brief  Get the number of entries currently in the list.
        \return Element count.
    */
    size_t GetSize() const             { return m_count; }

    /*! \brief  Check whether the list contains no entries.
        \return \c true if empty; \c false otherwise.
    */
    bool IsEmpty() const               { return (m_count == 0); }

    /*! \brief  Get the first (front) entry without removing it.
        \return Pointer to the first DListEntry, or \c NULL if the list is empty.
    */
    DLEntryType *GetFirst() const      { return m_first; }

    /*! \brief  Get the last (back) entry without removing it.
        \return Pointer to the last DListEntry, or \c NULL if the list is empty.
    */
    DLEntryType *GetLast() const       { return m_last; }

    /*! \brief  Remove and unlink all entries. After this call the list is empty.
        \note   Runs in O(n). Each entry is individually unlinked so its pointers are
                reset to NULL and it is left in a clean state suitable for re-insertion.
    */
    void Clear()                       { while (!IsEmpty()) Unlink(m_first); }

    /*! \brief     Append \a entry to the back of the list (pointer overload).
        \param[in] entry: Entry to insert. Must not already be linked to any list.
    */
    void LinkBack(DLEntryType *entry)  { Link(entry, nullptr, m_last); }

    /*! \brief     Append \a entry to the back of the list (reference overload).
        \param[in] entry: Entry to insert. Must not already be linked to any list.
    */
    void LinkBack(DLEntryType &entry)  { Link(&entry, nullptr, m_last); }

    /*! \brief     Prepend \a entry to the front of the list (pointer overload).
        \param[in] entry: Entry to insert. Must not already be linked to any list.
        \note      In an open list, \a entry becomes the new first element.
                   In a closed loop, the circular pointers are updated by UpdateEnds().
    */
    void LinkFront(DLEntryType *entry) { Link(entry, m_last, nullptr); }

    /*! \brief     Prepend \a entry to the front of the list (reference overload).
        \param[in] entry: Entry to insert. Must not already be linked to any list.
    */
    void LinkFront(DLEntryType &entry) { Link(&entry, m_last, nullptr); }

    /*! \brief  Remove and return the last entry.
        \return Pointer to the removed entry.
        \warning Triggers an assertion if the list is empty (Unlink receives \c NULL).
                 Always check IsEmpty() before calling.
    */
    DLEntryType *PopBack()
    {
        DLEntryType *ret = m_last;
        Unlink(ret);
        return ret;
    }

    /*! \brief  Remove and return the first entry.
        \return Pointer to the removed entry.
        \warning Triggers an assertion if the list is empty (Unlink receives \c NULL).
                 Always check IsEmpty() before calling.
    */
    DLEntryType *PopFront()
    {
        DLEntryType *ret = m_first;
        Unlink(ret);
        return ret;
    }

    /*! \brief     Remove \a entry from this list.
        \param[in] entry: Entry to remove. Must be non-NULL, currently linked, and owned by this list.
        \note      Asserts that \a entry is non-NULL, is linked, and belongs to this head.
                   Decrements m_count and calls UpdateEnds() to repair boundary and closed-loop pointers.
    */
    void Unlink(DLEntryType *entry)
    {
        STK_ASSERT(entry != nullptr);
        STK_ASSERT(entry->IsLinked());
        STK_ASSERT(entry->GetHead() == this);

        if (m_first == entry)
            m_first = entry->GetNext();

        if (m_last == entry)
            m_last = entry->GetPrev();

        entry->Unlink();
        --m_count;

        UpdateEnds();
    }

    /*! \brief     Move all entries from this list to the back of \a to, preserving order.
        \param[in] to: Destination list. Must not be the same object as \c *this.
        \note      After this call \c *this is empty and every entry that was here is now
                   at the back of \a to, in the same relative order.
        \note      Asserts that \a to is not the same list as \c *this.
    */
    void RelinkTo(DListHead &to)
    {
        STK_ASSERT(&to != this);

        while (!this->IsEmpty())
        {
            to.LinkBack(this->PopFront());
        }
    }

    /*! \brief     Insert \a entry into the list between \a prev and \a next.
        \param[in] entry: Entry to insert. Must be non-NULL and not currently linked.
        \param[in] next:  Entry that will follow \a entry after insertion, or \c NULL to append.
        \param[in] prev:  Entry that will precede \a entry after insertion, or \c NULL to prepend.
        \note      When \a prev is \c NULL the method treats this as a front-insertion: \a next is
                   overridden to \c m_first so \a entry becomes the new first element.
        \note      Updates m_first and m_last as needed. For closed-loop lists, calls UpdateEnds()
                   to maintain the circular back-link from last to first.
    */
    void Link(DLEntryType *entry, DLEntryType *next = nullptr, DLEntryType *prev = nullptr)
    {
        STK_ASSERT(entry != nullptr);
        STK_ASSERT(!entry->IsLinked());

        if (prev == nullptr)
            next = m_first;

        ++m_count;
        entry->Link(this, next, prev);

        if ((m_first == nullptr) || (m_first == entry->GetNext()))
            m_first = entry;

        if ((m_last == nullptr) || (m_last == entry->GetPrev()))
            m_last = entry;

        if (_ClosedLoop)
            UpdateEnds();
    }

private:
    /*! \brief  Repair the boundary and circular pointers after every structural change.
        \note   Called after every insertion and removal. Two responsibilities:
                1. If the list is now empty, resets m_first and m_last to \c NULL so
                   the head is in a canonical empty state.
                2. If \c _ClosedLoop is \c true and the list is non-empty, sets
                   \c m_last->m_next = m_first and \c m_first->m_prev = m_last to
                   maintain the circular invariant.
        \note   For open lists (\c _ClosedLoop == false) only the empty-list reset runs;
                the branch is eliminated at compile time by the constant template parameter.
    */
    void UpdateEnds()
    {
        if (IsEmpty())
        {
            m_first = nullptr;
            m_last  = nullptr;
        }
        else
        if (_ClosedLoop)
        {
            m_first->m_prev = m_last;
            m_last->m_next = m_first;
        }
    }

    size_t       m_count; //!< Number of entries currently in the list.
    DLEntryType *m_first; //!< Pointer to the first (front) entry, or \c NULL when empty.
    DLEntryType *m_last;  //!< Pointer to the last (back) entry, or \c NULL when empty.
};

} // namespace util
} // namespace stk


#endif /* STK_LINKED_LIST_H_ */
