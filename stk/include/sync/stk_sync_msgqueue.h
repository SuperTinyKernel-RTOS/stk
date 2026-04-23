/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_MSGQUEUE_H_
#define STK_SYNC_MSGQUEUE_H_

#include <cstring> // for memcpy

#include "stk_sync_cv.h"

/*! \file  stk_sync_msgqueue.h
    \brief Implementation of synchronization primitives:
           stk::sync::MessageQueue         - runtime-sized queue over an external buffer.
           stk::sync::MessageQueueT<N,MSG> - compile-time-sized queue with internal storage.
*/

namespace stk {
namespace sync {

/*! \class MessageQueue
    \brief Fixed-capacity, fixed-message-size FIFO queue for inter-task communication.

    MessageQueue provides a synchronized ring-buffer that transports opaque,
    fixed-size byte messages between tasks. It follows the following blocking semantics:
     - \c Put() blocks if the queue is full until space becomes available or the
       timeout expires.
     - \c Get() blocks if the queue is empty until a message is produced or the
       timeout expires.

    Unlike stk::sync::Pipe, which is parameterised on an element \e type,
    MessageQueue is parameterised on a \e byte count (\c MSG). This makes it
    suitable for passing heterogeneous or C-ABI structs without requiring the
    message type to be copyable via the C++ assignment operator. The message
    payload is always copied with \c memcpy.

    \code
    // Caller owns and provides the buffer (e.g. from a static pool):
    struct SensorMsg { uint32_t timestamp_ms; int16_t value; };
    static uint8_t s_buf[8 * sizeof(SensorMsg)];
    stk::sync::MessageQueue g_SensorQ(s_buf, 8, sizeof(SensorMsg));
    \endcode

    \note The caller is responsible for ensuring that \a buf remains valid for
          the entire lifetime of the queue object.
    \note Maximum number of messages (\a capacity) must not exceed CAPACITY_MAX.
    \note Message size (\a msg_size) must be at least 1.

    \see  MessageQueueT, Pipe, ConditionVariable, Semaphore
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
class MessageQueue : public ITraceable
{
public:
    /*! \brief     Max capacity supported (number of messages).
    */
    static const size_t CAPACITY_MAX = 0xFFFEU;

    /*! \brief     Constructor.
        \param[in] buf       Pointer to the externally-allocated storage.
                             Must be at least \a capacity * \a msg_size bytes.
        \param[in] capacity  Maximum number of messages [1, CAPACITY_MAX].
        \param[in] msg_size  Size of each message in bytes (>= 1).
    */
    explicit MessageQueue(uint8_t *buf, size_t capacity, size_t msg_size);

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error
                   (dangling waiters). An assertion is triggered in debug builds via the
                   ConditionVariable destructors.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~MessageQueue() = default;

    /*! \brief     Put a message into the back of the queue (FIFO order).
        \details   Copies \a msg_size bytes from \a msg_ptr into the next available
                   slot in the ring buffer. If the queue is full the calling task
                   is suspended until space becomes available or the timeout
                   expires.
        \param[in] msg_ptr: Pointer to the message payload (must be at least \a msg_size bytes).
        \param[in] timeout: Maximum time to wait for a free slot (ticks).
                   Use \c WAIT_INFINITE to block indefinitely, \c NO_WAIT for a
                   non-blocking attempt.
        \warning   ISR-safe only with \a timeout = \c NO_WAIT, ISR-unsafe otherwise.
        \return    \c true if the message was successfully enqueued,
                   \c false if the timeout expired before space became available.
    */
    bool Put(const void *msg_ptr, Timeout timeout = WAIT_INFINITE);

    /*! \brief     Attempt to put a message into the back of the queue without blocking.
        \details   Enqueues the message only if a free slot is immediately
                   available. Returns \c false instantly if the queue is full.
        \param[in] msg_ptr: Pointer to the message payload.
        \warning   ISR-safe.
        \return    \c true if the message was enqueued, \c false if the queue
                   was full.
    */
    bool TryPut(const void *msg_ptr) { return Put(msg_ptr, NO_WAIT); }

    /*! \brief     Put a message into the front of the queue (LIFO / priority-insert order).
        \details   Copies \a msg_size bytes from \a msg_ptr into the slot immediately
                   before the current read pointer, making it the next message that
                   \c Get() will return. If the queue is full the calling task is
                   suspended until space becomes available or the timeout expires.
        \param[in] msg_ptr: Pointer to the message payload (must be at least \a msg_size bytes).
        \param[in] timeout: Maximum time to wait for a free slot (ticks).
                   Use \c WAIT_INFINITE to block indefinitely, \c NO_WAIT for a
                   non-blocking attempt.
        \warning   ISR-safe only with \a timeout = \c NO_WAIT, ISR-unsafe otherwise.
        \return    \c true if the message was successfully enqueued at the front,
                   \c false if the timeout expired before space became available.
        \see       Put, TryPutFront
    */
    bool PutFront(const void *msg_ptr, Timeout timeout = WAIT_INFINITE);

    /*! \brief     Attempt to put a message into the front of the queue without blocking.
        \details   Enqueues the message at the front only if a free slot is immediately
                   available. Returns \c false instantly if the queue is full.
        \param[in] msg_ptr: Pointer to the message payload.
        \warning   ISR-safe.
        \return    \c true if the message was enqueued at the front, \c false if the
                   queue was full.
        \see       TryPut, PutFront
    */
    bool TryPutFront(const void *msg_ptr) { return PutFront(msg_ptr, NO_WAIT); }

    /*! \brief     Get a message from the queue.
        \details   Copies \a msg_size bytes from the oldest slot in the ring buffer
                   into the buffer pointed to by \a msg_ptr. If the queue is
                   empty the calling task is suspended until a message is
                   produced or the timeout expires.
        \param[out] msg_ptr: Destination buffer for the retrieved message
                    (must be at least \a msg_size bytes).
        \param[in]  timeout: Maximum time to wait for a message (ticks).
                    Use \c WAIT_INFINITE to block indefinitely, \c NO_WAIT for a
                    non-blocking attempt.
        \warning    ISR-safe only with \a timeout = \c NO_WAIT, ISR-unsafe otherwise.
        \return     \c true if a message was successfully retrieved,
                    \c false if the timeout expired before a message was available.
    */
    bool Get(void *msg_ptr, Timeout timeout = WAIT_INFINITE);

    /*! \brief     Attempt to get a message from the queue without blocking.
        \details   Dequeues a message only if one is immediately available.
                   Returns \c false instantly if the queue is empty.
        \param[out] msg_ptr: Destination buffer for the retrieved message.
        \warning   ISR-safe.
        \return    \c true if a message was retrieved, \c false if the queue
                   was empty.
    */
    bool TryGet(void *msg_ptr) { return Get(msg_ptr, NO_WAIT); }

    /*! \brief     Peek at the next message to be delivered (back of the FIFO) without removing it.
        \details   Copies \a msg_size bytes from the oldest slot in the ring buffer
                   into the buffer pointed to by \a msg_ptr, leaving the message in
                   place so that a subsequent \c Get() will return the same message.
                   If the queue is empty the calling task is suspended until a message
                   is produced or the timeout expires.
        \param[out] msg_ptr: Destination buffer for the peeked message
                    (must be at least \a msg_size bytes).
        \param[in]  timeout: Maximum time to wait for a message (ticks).
                    Use \c WAIT_INFINITE to block indefinitely, \c NO_WAIT for a
                    non-blocking attempt.
        \warning    ISR-safe only with \a timeout = \c NO_WAIT, ISR-unsafe otherwise.
        \return     \c true if a message was successfully peeked,
                    \c false if the timeout expired before a message was available.
        \see        Get, TryPeek, PeekFront
    */
    bool Peek(void *msg_ptr, Timeout timeout = WAIT_INFINITE);

    /*! \brief     Attempt to peek at the next message without blocking.
        \details   Copies the oldest message into \a msg_ptr only if one is
                   immediately available. The message is not removed from the queue.
                   Returns \c false instantly if the queue is empty.
        \param[out] msg_ptr: Destination buffer for the peeked message.
        \warning   ISR-safe.
        \return    \c true if a message was peeked, \c false if the queue was empty.
        \see       Peek, TryGet
    */
    bool TryPeek(void *msg_ptr) { return Peek(msg_ptr, NO_WAIT); }

    /*! \brief     Peek at the most recently front-inserted message (front of the FIFO) without removing it.
        \details   Copies \a msg_size bytes from the slot immediately before the
                   current write pointer (i.e. the message that \c PutFront() most
                   recently placed) into the buffer pointed to by \a msg_ptr, leaving
                   the message in place. If the queue is empty the calling task is
                   suspended until a message is produced or the timeout expires.
        \param[out] msg_ptr: Destination buffer for the peeked message
                    (must be at least \a msg_size bytes).
        \param[in]  timeout: Maximum time to wait for a message (ticks).
                    Use \c WAIT_INFINITE to block indefinitely, \c NO_WAIT for a
                    non-blocking attempt.
        \warning    ISR-safe only with \a timeout = \c NO_WAIT, ISR-unsafe otherwise.
        \return     \c true if a message was successfully peeked,
                    \c false if the timeout expired before a message was available.
        \see        PutFront, TryPeekFront, Peek
    */
    bool PeekFront(void *msg_ptr, Timeout timeout = WAIT_INFINITE);

    /*! \brief     Attempt to peek at the front message without blocking.
        \details   Copies the most recently front-inserted message into \a msg_ptr
                   only if one is immediately available. The message is not removed
                   from the queue. Returns \c false instantly if the queue is empty.
        \param[out] msg_ptr: Destination buffer for the peeked message.
        \warning   ISR-safe.
        \return    \c true if a message was peeked, \c false if the queue was empty.
        \see       PeekFront, TryPutFront
    */
    bool TryPeekFront(void *msg_ptr) { return PeekFront(msg_ptr, NO_WAIT); }

    /*! \brief     Discard all messages and reset the queue to the empty state.
        \details   Resets the head, tail and count to zero. Any tasks blocked in
                   \c Put() are woken so they can re-evaluate and enqueue their
                   messages into the now-empty queue.
        \warning   Messages that were in the queue are silently discarded.
                   Ensure no consumers depend on them before calling Reset().
        \warning   ISR-safe.
    */
    void Reset();

    /*! \brief     Get the maximum number of messages the queue can hold.
        \return    Construction-time capacity.
        \note      ISR-safe.
    */
    size_t GetCapacity() const { return m_capacity; }

    /*! \brief     Get the size of each message in bytes.
        \return    Construction-time message size.
        \note      ISR-safe.
    */
    size_t GetMsgSize() const { return m_msg_size; }

    /*! \brief     Get the current number of messages in the queue.
        \return    Point-in-time snapshot of the message count. May be stale
                   by the time the caller acts on it in a multi-task environment.
        \note      ISR-safe on targets where a \c size_t-aligned read is atomic.
    */
    size_t GetCount() const { return m_count; }

    /*! \brief     Get the number of free slots currently available.
        \return    Point-in-time snapshot of the free-slot count.
        \note      ISR-safe.
    */
    size_t GetSpace() const { return (m_capacity - m_count); }

    /*! \brief     Get pointer to the message buffer.
        \return    Pointer to the beginning of the message buffer.
        \note      ISR-safe.
    */
    uint8_t *GetBuffer() { return m_buffer; }

    /*! \brief     Check whether the queue is currently empty.
        \return    \c true if the queue contains no messages.
        \note      ISR-safe.
    */
    bool IsEmpty() const { return (m_count == 0U); }

    /*! \brief     Check whether the queue is currently full.
        \return    \c true if the queue contains \a capacity messages.
        \note      ISR-safe.
    */
    bool IsFull() const { return (m_count == m_capacity); }

    /*! \brief     Verify that the backing storage is valid and the pool is ready for use.
        \details   Always \c true for pools constructed with external storage.
                   For heap-constructed queue, \c false if \c operator new failed.
                   Must be checked after the heap constructor when operating without
                   exceptions (the typical embedded configuration).
        \return    \c true if the queue is ready for use.
        \note      ISR-safe.
    */
    bool IsStorageValid() const { return (m_buffer != nullptr); }

private:
    STK_NONCOPYABLE_CLASS(MessageQueue);

    // Get pointer to the raw storage for slot index \a idx.
    uint8_t *Slot(size_t idx) const { return m_buffer + (idx * m_msg_size); }

    // Advance a ring-buffer index forward by one with wrap-around.
    size_t Next(size_t idx) const { return (idx + 1U) % m_capacity; }

    // Retreat a ring-buffer index backward by one with wrap-around.
    // Uses a branch instead of modular arithmetic to avoid unsigned underflow.
    size_t Prev(size_t idx) const { return (idx == 0U) ? (m_capacity - 1U) : (idx - 1U); }

    uint8_t          *m_buffer;       //!< flat byte ring-buffer: capacity slots of msg_size bytes each
    const size_t      m_capacity;     //!< maximum number of messages stored in the queue
    const size_t      m_msg_size;     //!< size of each message in bytes
    size_t            m_count;        //!< current number of messages stored in the queue
    size_t            m_head;         //!< write index (next slot to be written by Put())
    size_t            m_tail;         //!< read  index (next slot to be read by Get())
    ConditionVariable m_cv_not_empty; //!< signaled by Put() when the queue transitions from empty
    ConditionVariable m_cv_not_full;  //!< signaled by Get()/Reset() when the queue is no longer full
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

inline MessageQueue::MessageQueue(uint8_t *buf, size_t capacity, size_t msg_size)
: m_buffer(buf),
  m_capacity(capacity),
  m_msg_size(msg_size),
  m_count(0U),
  m_head(0U),
  m_tail(0U)
{
    STK_ASSERT(buf      != nullptr);
    STK_ASSERT(capacity >= 1U);
    STK_ASSERT(capacity <= CAPACITY_MAX);
    STK_ASSERT(msg_size >= 1U);
}

// ---------------------------------------------------------------------------
// Put
// ---------------------------------------------------------------------------

inline bool MessageQueue::Put(const void *msg_ptr, Timeout timeout)
{
    STK_ASSERT(msg_ptr != nullptr);             // API contract: msg_ptr must not be null
    STK_ASSERT(m_count <= (CAPACITY_MAX - 1U)); // API contract: must not exceed capacity

    ScopedCriticalSection cs_;

    while (m_count == m_capacity)
    {
        if (!m_cv_not_full.Wait(cs_, timeout))
            return false;
    }

    memcpy(Slot(m_head), msg_ptr, m_msg_size);
    m_head = Next(m_head);
    m_count++;

    m_cv_not_empty.NotifyOne_CS();

    return true;
}

// ---------------------------------------------------------------------------
// PutFront
// ---------------------------------------------------------------------------

inline bool MessageQueue::PutFront(const void *msg_ptr, Timeout timeout)
{
    STK_ASSERT(msg_ptr != nullptr);             // API contract: msg_ptr must not be null
    STK_ASSERT(m_count <= (CAPACITY_MAX - 1U)); // API contract: must not exceed capacity

    ScopedCriticalSection cs_;

    while (m_count == m_capacity)
    {
        if (!m_cv_not_full.Wait(cs_, timeout))
            return false;
    }

    // Retreat the tail pointer to claim the slot that Get() would read next,
    // then write the message there.  This makes the new message the head of
    // the logical sequence without touching m_head at all.
    m_tail = Prev(m_tail);
    memcpy(Slot(m_tail), msg_ptr, m_msg_size);
    m_count++;

    m_cv_not_empty.NotifyOne_CS();

    return true;
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------

inline bool MessageQueue::Get(void *msg_ptr, Timeout timeout)
{
    STK_ASSERT(msg_ptr != nullptr); // API contract: msg_ptr must not be null

    ScopedCriticalSection cs_;

    while (m_count == 0U)
    {
        if (!m_cv_not_empty.Wait(cs_, timeout))
            return false;
    }

    memcpy(msg_ptr, Slot(m_tail), m_msg_size);
    m_tail = Next(m_tail);
    m_count--;

    m_cv_not_full.NotifyOne_CS();

    return true;
}

// ---------------------------------------------------------------------------
// Peek
// ---------------------------------------------------------------------------

inline bool MessageQueue::Peek(void *msg_ptr, Timeout timeout)
{
    STK_ASSERT(msg_ptr != nullptr); // API contract: msg_ptr must not be null

    ScopedCriticalSection cs_;

    while (m_count == 0U)
    {
        if (!m_cv_not_empty.Wait(cs_, timeout))
            return false;
    }

    // Copy from the tail slot without advancing the index or decrementing
    // the count, so the message remains available for the next Get().
    memcpy(msg_ptr, Slot(m_tail), m_msg_size);

    return true;
}

// ---------------------------------------------------------------------------
// PeekFront
// ---------------------------------------------------------------------------

inline bool MessageQueue::PeekFront(void *msg_ptr, Timeout timeout)
{
    STK_ASSERT(msg_ptr != nullptr); // API contract: msg_ptr must not be null

    ScopedCriticalSection cs_;

    while (m_count == 0U)
    {
        if (!m_cv_not_empty.Wait(cs_, timeout))
            return false;
    }

    // The front-inserted message is at m_tail (PutFront retreats m_tail then
    // writes, so the newly placed message is always at the current m_tail).
    // For a pure-Put queue this is equally correct: m_tail is the oldest slot.
    memcpy(msg_ptr, Slot(m_tail), m_msg_size);

    return true;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

inline void MessageQueue::Reset()
{
    ScopedCriticalSection cs_;

    m_count = 0U;
    m_head  = 0U;
    m_tail  = 0U;

    // Wake all blocked producers: the queue is now entirely empty.
    m_cv_not_full.NotifyAll_CS();
}

// ---------------------------------------------------------------------------

/*! \class MessageQueueT
    \brief Fixed-capacity, fixed-message-size FIFO queue with \b internal storage.
    \tparam N   Capacity of the queue (maximum number of messages).
    \tparam MSG Message size in bytes.

    Thin template wrapper over stk::sync::MessageQueue that owns its own
    ring-buffer. Use this when the capacity and message size are known at
    compile time and no external buffer needs to be supplied.

    \code
    // Example: Sending sensor readings from an ISR to a processing task
    struct SensorMsg {
        uint32_t timestamp_ms;
        int16_t  value;
    };

    stk::sync::MessageQueueT<8, sizeof(SensorMsg)> g_SensorQ;

    void ISR_Sensor() {
        SensorMsg msg = { GetTicks(), ReadSensor() };
        // non-blocking put from ISR (timeout = NO_WAIT)
        g_SensorQ.TryPut(&msg);
    }

    void Task_Processing() {
        SensorMsg msg;
        // block until a message arrives, with a 500 tick timeout
        if (g_SensorQ.Get(&msg, 500)) {
            Process(msg);
        }
    }
    \endcode

    \see  MessageQueue, Pipe, ConditionVariable, Semaphore
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
template <size_t N, size_t MSG>
class MessageQueueT : public MessageQueue
{
public:
    STK_STATIC_ASSERT_DESC(N   >= 1U,           "MessageQueueT: capacity N must be at least 1");
    STK_STATIC_ASSERT_DESC(N   <= CAPACITY_MAX, "MessageQueueT: capacity N must not exceed CAPACITY_MAX");
    STK_STATIC_ASSERT_DESC(MSG >= 1U,           "MessageQueueT: message size MSG must be at least 1");

    /*! \brief     Constructor.
    */
    MessageQueueT() : MessageQueue(m_storage, N, MSG)
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error
                   (dangling waiters). An assertion is triggered in debug builds via the
                   ConditionVariable destructors.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~MessageQueueT() = default;

private:
    STK_NONCOPYABLE_CLASS(MessageQueueT);

    alignas(alignof(max_align_t)) uint8_t m_storage[N * MSG]; //!< flat byte ring-buffer: N slots of MSG bytes each
};

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_MSGQUEUE_H_ */
