/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_PIPE_H_
#define STK_SYNC_PIPE_H_

#include <type_traits> // for std::is_scalar

#include "stk_sync_cv.h"

/*! \file  stk_sync_pipe.h
    \brief Implementation of synchronization primitives:
           stk::sync::Pipe        - runtime-sized byte-stream pipe over an external buffer.
           stk::sync::PipeT<T,N> - compile-time-sized, type-safe pipe with internal storage.
*/

namespace stk {
namespace sync {

/*! \class  Pipe
    \brief  Thread-safe FIFO communication pipe for inter-task data passing.

    Pipe provides a synchronized ring-buffer mechanism that allows tasks to
    exchange data safely. It is parameterised on an \e element byte count
    (\c element_size) rather than a C++ type, making it suitable for passing
    heterogeneous or C-ABI structs without requiring the element type to be
    copyable via the C++ assignment operator. All element payloads are copied
    with \c memcpy.

    It follows the following blocking semantics:
     - \c Write() blocks if the pipe is full until space becomes available or
       the timeout expires.
     - \c Read() blocks if the pipe is empty until data is produced or the
       timeout expires.

    The caller is responsible for providing an appropriately sized external
    buffer and ensuring it remains valid for the entire lifetime of the Pipe
    object. For a self-contained, type-safe alternative with compile-time
    capacity and direct typed assignment, see stk::sync::PipeT.

    \note  The boolean / count-returning calls (\c Write(), \c Read(), \c WriteBulk(), \c ReadBulk(),
           \c ReadBulkTriggered()) collapse a timeout and a cancellation (IKernel::CancelTaskWait())
           into the same failure result. Use the \c *Ex() variants (\c WriteEx(), \c ReadEx(),
           \c WriteBulkEx(), \c ReadBulkEx(), \c ReadBulkTriggeredEx()) if the caller needs to tell
           them apart.

    \code
    // Example: Producer-Consumer pattern with external buffer
    struct Sample { uint32_t timestamp_ms; int16_t value; };
    static uint8_t s_buf[8 * sizeof(Sample)];
    stk::sync::Pipe g_DataPipe(s_buf, 8, sizeof(Sample));

    void Task_Producer() {
        Sample s = { GetTicks(), ReadSensor() };
        // blocks if pipe is full
        g_DataPipe.Write(&s);
    }

    void Task_Consumer() {
        Sample received;
        // blocks until data is available, with a 1 s timeout
        if (g_DataPipe.Read(&received, 1000)) {
            // ... process received value ...
        }
    }
    \endcode

    \see  PipeT, MessageQueue, ConditionVariable
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
class Pipe : public ITraceable
{
public:
    /*! \brief     Max capacity supported (number of elements).
    */
    static const size_t CAPACITY_MAX = 0xFFFEU;

    /*! \brief     Constructor.
        \param[in] buf: Pointer to externally-allocated storage. Must be at least \a capacity * \a element_size bytes.
        \param[in] capacity: Maximum number of elements [1, CAPACITY_MAX].
        \param[in] element_size: Size of each element in bytes (>= 1).
    */
    explicit Pipe(uint8_t *buf, size_t capacity, size_t element_size);

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error
                   (dangling waiters). An assertion is triggered in debug builds via the
                   ConditionVariable destructors.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~Pipe() = default;

    /*! \brief     Write a single element to the pipe, preserving the full wait outcome.
        \details   Identical to \c Write(), except the caller gets the raw \c EWaitResult, so a
                   cancellation (via IKernel::CancelTaskWait()) can be distinguished from a timeout.
        \param[in] data: Pointer to the element payload (must be at least \a element_size bytes).
        \param[in] timeout_ticks: Maximum time to wait for space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if data was written, \c WAIT_RESULT_TIMEOUT if no space became
                   available before the timeout expired (or \a NO_WAIT was passed on a full pipe),
                   \c WAIT_RESULT_CANCELED if the wait was cancelled via IKernel::CancelTaskWait(),
                   \c WAIT_RESULT_FAIL if the kernel rejected the wait. Nothing is written on a
                   non-signal result.
    */
    EWaitResult WriteEx(const void *data, Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Write a single element to the pipe.
        \details   Copies \a element_size bytes from \a data into the next available
                   slot in the ring buffer. If the pipe is full, the calling task is
                   suspended until space becomes available or the timeout expires.
        \param[in] data: Pointer to the element payload (must be at least \a element_size bytes).
        \param[in] timeout_ticks: Maximum time to wait for space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c true if data was successfully written, \c false otherwise (timeout, cancellation
                   or NO_WAIT on a full pipe). Use \c WriteEx() to tell these apart.
    */
    bool Write(const void *data, Timeout timeout_ticks = WAIT_INFINITE)
    {
        return (WriteEx(data, timeout_ticks) == WAIT_RESULT_SIGNAL);
    }

    /*! \brief     Attempt to write a single element to the pipe without blocking.
        \details   Enqueues the element only if a free slot is immediately available.
                   Returns \c false instantly if the pipe is full.
        \param[in] data: Pointer to the element payload.
        \warning   ISR-safe.
        \return    \c true if data was successfully written, \c false if no space is available.
    */
    bool TryWrite(const void *data) { return Write(data, NO_WAIT); }

    /*! \brief     Write multiple elements to the pipe, preserving the full wait outcome.
        \details   Identical to \c WriteBulk(), except the caller gets the raw \c EWaitResult and the
                   number of elements written through \a written, so a cancellation (via
                   IKernel::CancelTaskWait()) can be distinguished from a timeout. Elements already
                   copied into the pipe before a timeout or cancellation stay in the pipe.
        \param[in] src: Pointer to the source array (must hold at least \a count elements
                   of \a element_size bytes each).
        \param[in] count: Number of elements to write.
        \param[out] written: Number of elements actually written (always set, 0 on \c WAIT_RESULT_FAIL).
        \param[in] timeout_ticks: Maximum time to wait for sufficient space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if all \a count elements were written (also for \a count = 0),
                   \c WAIT_RESULT_TIMEOUT if the timeout expired (or \a NO_WAIT was passed) before all
                   elements fitted, \c WAIT_RESULT_CANCELED if the wait was cancelled via
                   IKernel::CancelTaskWait(), \c WAIT_RESULT_FAIL if \a src is \c nullptr or the kernel
                   rejected the wait. On any non-signal result \a written holds the partial count.
    */
    EWaitResult WriteBulkEx(const void *src, size_t count, size_t &written,
                            Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Write multiple elements to the pipe.
        \details   Copies a block of \a count elements into the FIFO. If the pipe does not have
                   enough space for the entire block, it will block until the full amount can be
                   written or the timeout expires.
        \param[in] src: Pointer to the source array (must hold at least \a count elements
                   of \a element_size bytes each).
        \param[in] count: Number of elements to write.
        \param[in] timeout_ticks: Maximum time to wait for sufficient space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    Number of elements actually written. Equal to \c count unless a timeout or a
                   cancellation occurred. Use \c WriteBulkEx() to tell these apart.
        \code
        // Example:
        Sample frame[64];
        FillFrame(frame);

        size_t result = g_Pipe.WriteBulk(frame, 64, 500);
        if (result < 64) {
            // handle partial write / timeout
        }
        \endcode
    */
    size_t WriteBulk(const void *src, size_t count, Timeout timeout_ticks = WAIT_INFINITE)
    {
        size_t written = 0U;
        const EWaitResult result = WriteBulkEx(src, count, written, timeout_ticks);
        STK_UNUSED(result);
        return written;
    }

    /*! \brief     Attempt to write multiple elements to the pipe without blocking.
        \details   Copies as many elements as possible without blocking. Elements that do not
                   fit are discarded.
        \param[in] src: Pointer to the source array.
        \param[in] count: Number of elements to write.
        \warning   ISR-safe.
        \return    Number of elements actually written.
    */
    size_t TryWriteBulk(const void *src, size_t count) { return WriteBulk(src, count, NO_WAIT); }

    /*! \brief     Read a single element from the pipe, preserving the full wait outcome.
        \details   Identical to \c Read(), except the caller gets the raw \c EWaitResult, so a
                   cancellation (via IKernel::CancelTaskWait()) can be distinguished from a timeout.
        \param[out] data: Destination buffer for the retrieved element (must be at least \a element_size bytes).
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if an element was read, \c WAIT_RESULT_TIMEOUT if no data became
                   available before the timeout expired (or \a NO_WAIT was passed on an empty pipe),
                   \c WAIT_RESULT_CANCELED if the wait was cancelled via IKernel::CancelTaskWait(),
                   \c WAIT_RESULT_FAIL if the kernel rejected the wait. Nothing is read on a
                   non-signal result.
    */
    EWaitResult ReadEx(void *data, Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Read a single element from the pipe.
        \details   Copies \a element_size bytes from the oldest slot in the ring buffer into
                   the buffer pointed to by \a data. If the pipe is empty, the calling task is
                   suspended until data is produced or the timeout expires.
        \param[out] data: Destination buffer for the retrieved element (must be at least \a element_size bytes).
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c true if data was successfully read, \c false otherwise (timeout, cancellation or
                   NO_WAIT on an empty pipe). Use \c ReadEx() to tell these apart.
    */
    bool Read(void *data, Timeout timeout_ticks = WAIT_INFINITE)
    {
        return (ReadEx(data, timeout_ticks) == WAIT_RESULT_SIGNAL);
    }

    /*! \brief     Attempt to read a single element from the pipe without blocking.
        \details   Dequeues an element only if one is immediately available.
                   Returns \c false instantly if the pipe is empty.
        \param[out] data: Destination buffer for the retrieved element.
        \warning   ISR-safe.
        \return    \c true if data was successfully read, \c false if the pipe was empty.
    */
    bool TryRead(void *data) { return Read(data, NO_WAIT); }

    /*! \brief     Read multiple elements from the pipe, preserving the full wait outcome.
        \details   Identical to \c ReadBulk(), except the caller gets the raw \c EWaitResult and the
                   number of elements read through \a read_count, so a cancellation (via
                   IKernel::CancelTaskWait()) can be distinguished from a timeout. Elements already
                   copied into \a dst before a timeout or cancellation stay valid and are counted.
        \param[out] dst: Pointer to the destination array (must hold at least \a count elements
                   of \a element_size bytes each).
        \param[in] count:   Number of elements to read.
        \param[out] read_count: Number of elements actually read (always set, 0 on \c WAIT_RESULT_FAIL).
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if all \a count elements were read (also for \a count = 0),
                   \c WAIT_RESULT_TIMEOUT if the timeout expired (or \a NO_WAIT was passed) before all
                   elements were available, \c WAIT_RESULT_CANCELED if the wait was cancelled via
                   IKernel::CancelTaskWait(), \c WAIT_RESULT_FAIL if \a dst is \c nullptr or the kernel
                   rejected the wait. On any non-signal result \a read_count holds the partial count.
    */
    EWaitResult ReadBulkEx(void *dst, size_t count, size_t &read_count,
                           Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Read multiple elements from the pipe.
        \details   Attempts to retrieve \a count elements from the FIFO. If the pipe does not
                   contain enough elements to satisfy the request, it will block until the full
                   amount is read or the timeout expires.
        \param[out] dst: Pointer to the destination array (must hold at least \a count elements
                   of \a element_size bytes each).
        \param[in] count:   Number of elements to read.
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    Number of elements actually read. Equal to \c count unless a timeout or a
                   cancellation occurred. Use \c ReadBulkEx() to tell these apart.
        \code
        // Example:
        Sample frame[64];

        size_t result = g_Pipe.ReadBulk(frame, 64, 500);
        if (result == 64) {
            // process full frame
        } else {
            // handle partial read / timeout
        }
        \endcode
    */
    size_t ReadBulk(void *dst, size_t count, Timeout timeout_ticks = WAIT_INFINITE)
    {
        size_t read_count = 0U;
        const EWaitResult result = ReadBulkEx(dst, count, read_count, timeout_ticks);
        STK_UNUSED(result);
        return read_count;
    }

    /*! \brief     Attempt to read multiple elements from the pipe without blocking.
        \details   Reads as many elements as are currently available without blocking.
        \param[out] dst: Pointer to the destination array.
        \param[in] count: Number of elements to read.
        \warning   ISR-safe.
        \return    Number of elements actually read.
    */
    size_t TryReadBulk(void *dst, size_t count) { return ReadBulk(dst, count, NO_WAIT); }

    /*! \brief     Read at least \a trigger elements, then drain up to \a max_count without blocking,
                   preserving the full wait outcome.
        \details   Identical to \c ReadBulkTriggered(), except the caller gets the raw \c EWaitResult
                   and the number of elements read through \a read_count, so a cancellation (via
                   IKernel::CancelTaskWait()) can be distinguished from a timeout. Like the plain
                   variant, whatever is available is drained even when the wait did not succeed.
        \param[out] dst: Destination buffer; must hold at least \a max_count elements
                   of \a element_size bytes.
        \param[in] trigger: Minimum number of elements that must be available before the wait is
                   considered successful. Clamped to [1, max_count] internally.
        \param[in] max_count: Maximum number of elements to return in total.
        \param[out] read_count: Number of elements actually read (always set, 0 on \c WAIT_RESULT_FAIL).
        \param[in] timeout_ticks: Maximum time to wait for \a trigger elements. Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if the trigger threshold was reached (also for \a max_count = 0),
                   \c WAIT_RESULT_TIMEOUT if the timeout expired before the threshold was reached,
                   \c WAIT_RESULT_CANCELED if the wait was cancelled via IKernel::CancelTaskWait(),
                   \c WAIT_RESULT_FAIL if \a dst is \c nullptr or the kernel rejected the wait.
                   On a timeout or cancellation \a read_count holds the (possibly zero) number of
                   elements that were available and drained.
    */
    EWaitResult ReadBulkTriggeredEx(void *dst, size_t trigger, size_t max_count, size_t &read_count,
                                    Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Read at least \a trigger elements, then drain up to \a max_count without blocking.
        \details   Blocks until \b at least \a trigger elements are simultaneously available in the
                   pipe, or the timeout expires. Once the threshold is reached the call dequeues
                   \c min(max_count, available) elements in a single critical-section pass — no
                   busy-spin, no second call.

        \param[out] dst: Destination buffer; must hold at least \a max_count elements
                   of \a element_size bytes.
        \param[in] trigger: Minimum number of elements that must be available before any
                   data is dequeued. Clamped to [1, max_count] internally.
        \param[in] max_count: Maximum number of elements to return in total.
        \param[in] timeout_ticks: Maximum time to wait for \a trigger elements. Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    Number of elements actually read.
                   - Equal to zero if timeout fired before a single element arrived.
                   - Greater than zero but less than \a trigger if a partial trigger's worth of
                     data was written before timeout (this can only happen when \a trigger > 1
                     and the timeout expires after some — but not enough — elements arrive).
                   - Greater than or equal to \a trigger if the threshold was satisfied (the
                     caller may then assume the trigger condition is met).
        \note      A cancellation (IKernel::CancelTaskWait()) is reported the same way as a timeout.
                   Use \c ReadBulkTriggeredEx() to tell these apart.
    */
    size_t ReadBulkTriggered(void *dst, size_t trigger, size_t max_count,
                             Timeout timeout_ticks = WAIT_INFINITE)
    {
        size_t read_count = 0U;
        const EWaitResult result = ReadBulkTriggeredEx(dst, trigger, max_count, read_count, timeout_ticks);
        STK_UNUSED(result);
        return read_count;
    }

    /*! \brief     Non-blocking variant of ReadBulkTriggered.
        \details   Returns immediately with however many elements are available, up to
                   \a max_count.  The trigger threshold is not enforced (equivalent to
                   trigger = 1 with NO_WAIT).
        \param[out] dst: Destination buffer.
        \param[in] max_count: Maximum number of elements to read.
        \warning   ISR-safe.
        \return    Number of elements actually read.
    */
    size_t TryReadBulkTriggered(void *dst, size_t max_count)
    {
        return ReadBulkTriggered(dst, 1U, max_count, NO_WAIT);
    }

    /*! \brief     Discard all elements and reset the pipe to the empty state.
        \details   Resets head, tail and count to zero. Any tasks blocked in \c Write()
                   are woken so they can re-evaluate and enqueue into the now-empty pipe.
        \warning   Elements that were in the pipe are silently discarded.
                   Ensure no consumers depend on them before calling Reset().
        \warning   ISR-safe.
    */
    void Reset();

    /*! \brief     Get the maximum number of elements the pipe can hold.
        \return    Construction-time capacity.
        \note      ISR-safe.
    */
    size_t GetCapacity() const { return m_capacity; }

    /*! \brief     Get the size of each element in bytes.
        \return    Construction-time element size.
        \note      ISR-safe.
    */
    size_t GetElementSize() const { return m_element_size; }

    /*! \brief     Get the current number of elements in the pipe.
        \return    Point-in-time snapshot of the element count. May be stale by the time
                   the caller acts on it in a multi-task environment.
        \note      ISR-safe on targets where a size_t-aligned read is atomic.
    */
    size_t GetCount() const { return m_count; }

    /*! \brief     Get the number of free slots currently available.
        \return    Point-in-time snapshot of the free-slot count.
        \note      ISR-safe.
    */
    size_t GetSpace() const { return (m_capacity - m_count); }

    /*! \brief     Get a pointer to the raw backing buffer.
        \return    Pointer to the beginning of the element buffer.
        \note      ISR-safe.
    */
    uint8_t *GetBuffer() { return m_buffer; }

    /*! \brief     Check if the pipe is currently empty.
        \return    \c true if empty, otherwise \c false.
        \note      The returned value is a point-in-time snapshot.
        \note      ISR-safe.
    */
    bool IsEmpty() const { return (m_count == 0U); }

    /*! \brief     Check if the pipe is currently full.
        \return    \c true if full, otherwise \c false.
        \note      The returned value is a point-in-time snapshot.
        \note      ISR-safe.
    */
    bool IsFull() const { return (m_count == m_capacity); }

    /*! \brief     Verify that the backing storage is valid and the pipe is ready for use.
        \details   \c false only if the buffer pointer is \c nullptr (the constructor asserts against it).
        \return    \c true if the pipe is ready for use.
        \note      ISR-safe.
    */
    bool IsStorageValid() const { return (m_buffer != nullptr); }

private:
    STK_NONCOPYABLE_CLASS(Pipe);

    // Get a byte pointer to the raw storage for slot index \a idx.
    uint8_t *Slot(size_t idx) const { return m_buffer + (idx * m_element_size); }

    // Advance a ring-buffer index by one with wrap-around.
    size_t Next(size_t idx) const { return (idx + 1U) % m_capacity; }

    // Copy Min(count, m_count) elements into dst_bytes and update tail/count.
    // Caller MUST hold the critical section. Returns the number of elements copied.
    size_t DrainLocked(uint8_t *dst_bytes, size_t count);

    uint8_t          *m_buffer;       //!< flat byte ring-buffer: capacity slots of element_size bytes each
    const size_t      m_capacity;     //!< maximum number of elements stored in the pipe
    const size_t      m_element_size; //!< size of each element in bytes
    size_t            m_count;        //!< current number of elements stored in the pipe
    size_t            m_head;         //!< write index (next slot to be written by Write())
    size_t            m_tail;         //!< read  index (next slot to be read by Read())
    ConditionVariable m_cv_not_empty; //!< signaled by Write() when the pipe transitions from empty
    ConditionVariable m_cv_not_full;  //!< signaled by Read()/Reset() when the pipe is no longer full
};

// ---------------------------------------------------------------------------
// Pipe: Constructor
// ---------------------------------------------------------------------------

inline Pipe::Pipe(uint8_t *buf, size_t capacity, size_t element_size)
: m_buffer(buf),
  m_capacity(capacity),
  m_element_size(element_size),
  m_count(0U),
  m_head(0U),
  m_tail(0U)
{
    STK_ASSERT(buf          != nullptr);
    STK_ASSERT(capacity     >= 1U);
    STK_ASSERT(capacity     <= CAPACITY_MAX);
    STK_ASSERT(element_size >= 1U);
}

// ---------------------------------------------------------------------------
// Pipe: WriteEx
// ---------------------------------------------------------------------------

inline EWaitResult Pipe::WriteEx(const void *data, Timeout timeout_ticks)
{
    STK_ASSERT(data    != nullptr);
    STK_ASSERT(m_count <= (CAPACITY_MAX - 1U));

    ScopedCriticalSection cs_;
    EWaitResult result = WAIT_RESULT_SIGNAL;

    const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

    // capture an absolute deadline once (64-bit Ticks, so it stays valid across the 32-bit tick
    // wraparound), before entering the wait loop; a different
    // producer can steal the just-freed slot between NotifyOne_CS() and this task
    // re-acquiring the critical section (Mesa-style wakeup, not a direct handoff),
    // so re-passing the original timeout_ticks on each retry would silently restart it
    const Ticks deadline = (timed_wait ?
        (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

    while (m_count == m_capacity)
    {
        Timeout remaining = timeout_ticks;
        if (timed_wait)
        {
            // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
            const Ticks left = (deadline - GetTicks());
            remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
        }

        result = m_cv_not_full.WaitEx(cs_, remaining);
        if (result != WAIT_RESULT_SIGNAL)
        {
            // timeout, cancellation or kernel failure: nothing is written
            break;
        }
    }

    if (result == WAIT_RESULT_SIGNAL)
    {
        STK_MEMCPY(Slot(m_head), data, m_element_size);
        m_head = Next(m_head);
        m_count++;

        // notify consumer that data is ready
        m_cv_not_empty.NotifyOne_CS();
    }

    return result;
}

// ---------------------------------------------------------------------------
// Pipe: WriteBulkEx
// ---------------------------------------------------------------------------

inline EWaitResult Pipe::WriteBulkEx(const void *src, size_t count, size_t &written, Timeout timeout_ticks)
{
    written = 0U;

    // null source is rejected, an empty request is trivially satisfied
    EWaitResult result = ((src != nullptr) ? WAIT_RESULT_SIGNAL : WAIT_RESULT_FAIL);

    if ((src != nullptr) && (count != 0U))
    {
        const uint8_t *const src_bytes = static_cast<const uint8_t *>(src);
        const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

        // capture an absolute deadline once, before entering the wait loop,
        // preventing the timeout from resetting on intermediate partial writes
        const Ticks deadline = (timed_wait ?
            (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

        ScopedCriticalSection cs_;

        while (written < count)
        {
            // wait until there is at least 1 free slot
            while (m_count == m_capacity)
            {
                Timeout remaining = timeout_ticks;
                if (timed_wait)
                {
                    // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
                    const Ticks left = (deadline - GetTicks());
                    remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
                }

                const EWaitResult wr = m_cv_not_full.WaitEx(cs_, remaining);
                if (wr != WAIT_RESULT_SIGNAL)
                {
                    result = wr;
                    break; // break inner condition variable loop
                }
            }

            // if the wait failed (timeout, cancellation), drop out of the chunk processing loop
            if (result != WAIT_RESULT_SIGNAL)
            {
                break;
            }

            const size_t available  = m_capacity - m_count;
            const size_t to_write   = ((count - written) < available) ? (count - written) : available;
            const size_t first_part = m_capacity - m_head;

            if (to_write <= first_part)
            {
                STK_MEMCPY(Slot(m_head), src_bytes + (written * m_element_size), to_write * m_element_size);
            }
            else
            {
                STK_MEMCPY(Slot(m_head), src_bytes + (written                * m_element_size), first_part              * m_element_size);
                STK_MEMCPY(Slot(0U),     src_bytes + ((written + first_part) * m_element_size), (to_write - first_part) * m_element_size);
            }

            written += to_write;
            m_head   = (m_head + to_write) % m_capacity;
            m_count += to_write;

            // notify consumers that data is ready
            m_cv_not_empty.NotifyAll_CS();
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Pipe: ReadEx
// ---------------------------------------------------------------------------

inline EWaitResult Pipe::ReadEx(void *data, Timeout timeout_ticks)
{
    STK_ASSERT(data != nullptr);

    ScopedCriticalSection cs_;
    EWaitResult result = WAIT_RESULT_SIGNAL;

    const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

    // capture an absolute deadline once, before entering the wait loop; a different
    // consumer can steal the just-produced element between NotifyOne_CS() and this
    // task re-acquiring the critical section (Mesa-style wakeup, not a direct handoff),
    // so re-passing the original timeout_ticks on each retry would silently restart it
    const Ticks deadline = (timed_wait ?
        (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

    while (m_count == 0U)
    {
        Timeout remaining = timeout_ticks;
        if (timed_wait)
        {
            // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
            const Ticks left = (deadline - GetTicks());
            remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
        }

        result = m_cv_not_empty.WaitEx(cs_, remaining);
        if (result != WAIT_RESULT_SIGNAL)
        {
            // timeout, cancellation or kernel failure: nothing is read
            break;
        }
    }

    if (result == WAIT_RESULT_SIGNAL)
    {
        STK_MEMCPY(data, Slot(m_tail), m_element_size);
        m_tail = Next(m_tail);
        m_count--;

        // notify producer that space is now available
        m_cv_not_full.NotifyOne_CS();
    }

    return result;
}

// ---------------------------------------------------------------------------
// Pipe: DrainLocked  (private helper, caller must hold the critical section)
// ---------------------------------------------------------------------------

inline size_t Pipe::DrainLocked(uint8_t *const dst_bytes, const size_t count)
{
    const size_t to_read    = Min(count, m_count);
    const size_t first_part = m_capacity - m_tail;

    if (to_read <= first_part)
    {
        STK_MEMCPY(dst_bytes, Slot(m_tail), to_read * m_element_size);
    }
    else
    {
        STK_MEMCPY(dst_bytes,                                 Slot(m_tail), first_part             * m_element_size);
        STK_MEMCPY(dst_bytes + (first_part * m_element_size), Slot(0U),     (to_read - first_part) * m_element_size);
    }

    m_tail   = (m_tail + to_read) % m_capacity;
    m_count -= to_read;

    m_cv_not_full.NotifyAll_CS();
    return to_read;
}

// ---------------------------------------------------------------------------
// Pipe: ReadBulkEx
// ---------------------------------------------------------------------------

inline EWaitResult Pipe::ReadBulkEx(void *dst, size_t count, size_t &read_count, Timeout timeout_ticks)
{
    read_count = 0U;

    // null destination is rejected, an empty request is trivially satisfied
    EWaitResult result = ((dst != nullptr) ? WAIT_RESULT_SIGNAL : WAIT_RESULT_FAIL);

    if ((dst != nullptr) && (count != 0U))
    {
        uint8_t *const dst_bytes = static_cast<uint8_t *>(dst);
        const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

        // capture an absolute deadline once, before entering the wait loop,
        // preventing the timeout from resetting on intermediate partial reads
        const Ticks deadline = (timed_wait ?
            (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

        ScopedCriticalSection cs_;

        while (read_count < count)
        {
            // wait until there is at least 1 element available
            while (m_count == 0U)
            {
                Timeout remaining = timeout_ticks;
                if (timed_wait)
                {
                    // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
                    const Ticks left = (deadline - GetTicks());
                    remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
                }

                const EWaitResult wr = m_cv_not_empty.WaitEx(cs_, remaining);
                if (wr != WAIT_RESULT_SIGNAL)
                {
                    result = wr;
                    break; // break inner condition variable loop
                }
            }

            // if the wait failed (timeout, cancellation), drop out of the chunk processing loop
            if (result != WAIT_RESULT_SIGNAL)
            {
                break;
            }

            // drain data using the state tracker's relative byte offsets
            read_count += DrainLocked(dst_bytes + (read_count * m_element_size), count - read_count);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Pipe: ReadBulkTriggeredEx
// ---------------------------------------------------------------------------

inline EWaitResult Pipe::ReadBulkTriggeredEx(void *dst, size_t trigger, size_t max_count, size_t &read_count,
    Timeout timeout_ticks)
{
    read_count = 0U;

    // null destination is rejected, an empty request is trivially satisfied
    EWaitResult result = ((dst != nullptr) ? WAIT_RESULT_SIGNAL : WAIT_RESULT_FAIL);

    if ((dst != nullptr) && (max_count != 0U))
    {
        // trigger must be in [1, max_count]
        if (trigger == 0U)       { trigger = 1U; }
        if (trigger > max_count) { trigger = max_count; }

        uint8_t *const dst_bytes = static_cast<uint8_t *>(dst);
        const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

        // capture an absolute deadline once, before entering the wait loop,
        // preventing the timeout from resetting on intermediate spurious wakeups
        const Ticks deadline = (timed_wait ?
            (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

        ScopedCriticalSection cs_;

        while (m_count < trigger)
        {
            Timeout remaining = timeout_ticks;
            if (timed_wait)
            {
                // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
                const Ticks left = (deadline - GetTicks());
                remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
            }

            const EWaitResult wr = m_cv_not_empty.WaitEx(cs_, remaining);
            if (wr != WAIT_RESULT_SIGNAL)
            {
                result = wr;
                break; // break the waiting loop on timeout, cancellation or failure
            }
        }

        // whether we broke out via satisfying the trigger or a failed wait (timeout,
        // cancellation), we drain whatever is currently available up to max_count
        read_count = DrainLocked(dst_bytes, max_count);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Pipe: Reset
// ---------------------------------------------------------------------------

inline void Pipe::Reset()
{
    const ScopedCriticalSection cs_;

    m_count = 0U;
    m_head  = 0U;
    m_tail  = 0U;

    // wake all blocked producers: the pipe is now entirely empty, every slot is free
    // note: we do not release readers here
    m_cv_not_full.NotifyAll_CS();
}

// ---------------------------------------------------------------------------

/*! \class  PipeT
    \brief  Thread-safe, type-safe FIFO communication pipe with \b internal storage.
    \tparam T Data type of elements.
    \tparam N Capacity of the pipe (number of elements).

    PipeT provides a synchronized ring-buffer mechanism that allows tasks to
    exchange typed data safely. It implements blocking semantics:
     - \c Write() blocks if the pipe is full until space becomes available.
     - \c Read() blocks if the pipe is empty until data is produced.

    Unlike stk::sync::Pipe, which operates on raw byte buffers and uses
    \c memcpy for all transfers, PipeT is parameterised on a concrete element
    type \c T. This enables:
     - Direct typed assignment (no \c memcpy overhead for scalar types).
     - Compile-time constant propagation for capacity \c N and element size,
       allowing the compiler to strength-reduce modulo operations and
       eliminate dead branches in bulk transfer paths.
     - A fully type-safe call interface using \c T references and pointers.

    \note  The boolean / count-returning calls (\c Write(), \c Read(), \c WriteBulk(), \c ReadBulk())
           collapse a timeout and a cancellation (IKernel::CancelTaskWait()) into the same failure
           result. Use the \c *Ex() variants (\c WriteEx(), \c ReadEx(), \c WriteBulkEx(),
           \c ReadBulkEx()) if the caller needs to tell them apart.

    \code
    // Example: Producer-Consumer pattern
    stk::sync::PipeT<uint32_t, 8> g_DataPipe;

    void Task_Producer() {
        uint32_t value = 42;
        // blocks if pipe is full
        g_DataPipe.Write(value);
    }

    void Task_Consumer() {
        uint32_t received;
        // blocks until data is available, with a 1 s timeout
        if (g_DataPipe.Read(received, 1000)) {
            // ... process received value ...
        }
    }
    \endcode

    \see  Pipe, MessageQueueT, ConditionVariable
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
template <typename T, size_t N>
class PipeT
{
public:
    /*! \brief     Constructor.
    */
    explicit PipeT() : m_buffer(), m_head(0U), m_tail(0U), m_count(0U), m_cv_not_empty(), m_cv_not_full()
    {}

    /*! \brief     Write data to the pipe, preserving the full wait outcome.
        \details   Identical to \c Write(), except the caller gets the raw \c EWaitResult, so a
                   cancellation (via IKernel::CancelTaskWait()) can be distinguished from a timeout.
        \param[in] data: Reference to the data element to be copied into the pipe.
        \param[in] timeout_ticks: Maximum time to wait for space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if data was written, \c WAIT_RESULT_TIMEOUT if no space became
                   available before the timeout expired (or \a NO_WAIT was passed on a full pipe),
                   \c WAIT_RESULT_CANCELED if the wait was cancelled via IKernel::CancelTaskWait(),
                   \c WAIT_RESULT_FAIL if the kernel rejected the wait. Nothing is written on a
                   non-signal result.
    */
    EWaitResult WriteEx(const T &data, Timeout timeout_ticks = WAIT_INFINITE)
    {
        ScopedCriticalSection cs_;
        EWaitResult result = WAIT_RESULT_SIGNAL;

        const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

        // capture an absolute deadline once, before entering the wait loop; a different
        // producer can steal the just-freed slot between NotifyOne_CS() and this task
        // re-acquiring the critical section (Mesa-style wakeup, not a direct handoff),
        // so re-passing the original timeout_ticks on each retry would silently restart it
        const Ticks deadline = (timed_wait ?
            (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

        while (m_count == N)
        {
            Timeout remaining = timeout_ticks;
            if (timed_wait)
            {
                // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
                const Ticks left = (deadline - GetTicks());
                remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
            }

            result = m_cv_not_full.WaitEx(cs_, remaining);
            if (result != WAIT_RESULT_SIGNAL)
            {
                // timeout, cancellation or kernel failure: nothing is written
                break;
            }
        }

        if (result == WAIT_RESULT_SIGNAL)
        {
            m_buffer[m_head] = data;
            m_head = (m_head + 1U) % N;
            m_count += 1U;

            // notify consumer that data is ready
            m_cv_not_empty.NotifyOne_CS();
        }

        return result;
    }

    /*! \brief     Write data to the pipe.
        \details   Attempts to push an element into the FIFO queue. If pipe is full, the
                   calling task will be suspended until space is made available by a
                   consumer or the timeout expires.
        \param[in] data: Reference to the data element to be copied into the pipe.
        \param[in] timeout_ticks: Maximum time to wait for space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c true if data was successfully written, \c false otherwise (timeout, cancellation
                   or NO_WAIT on a full pipe). Use \c WriteEx() to tell these apart.
    */
    bool Write(const T &data, Timeout timeout_ticks = WAIT_INFINITE)
    {
        return (WriteEx(data, timeout_ticks) == WAIT_RESULT_SIGNAL);
    }

    /*! \brief     Attempt to write data to the pipe without blocking.
        \details   Enqueues the element only if a free slot is immediately available.
                   Returns \c false instantly if the pipe is full.
        \param[in] data: Reference to the data element to be copied into the pipe.
        \warning   ISR-safe.
        \return    \c true if data was successfully written, \c false if no space is available.
    */
    bool TryWrite(const T &data) { return Write(data, NO_WAIT); }

    /*! \brief     Write multiple elements to the pipe, preserving the full wait outcome.
        \details   Identical to \c WriteBulk(), except the caller gets the raw \c EWaitResult and the
                   number of elements written through \a written, so a cancellation (via
                   IKernel::CancelTaskWait()) can be distinguished from a timeout. Elements already
                   copied into the pipe before a timeout or cancellation stay in the pipe.
        \param[in] src: Pointer to the source array.
        \param[in] count: Number of elements to write.
        \param[out] written: Number of elements actually written (always set, 0 on \c WAIT_RESULT_FAIL).
        \param[in] timeout_ticks: Maximum time to wait for sufficient space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if all \a count elements were written (also for \a count = 0),
                   \c WAIT_RESULT_TIMEOUT if the timeout expired (or \a NO_WAIT was passed) before all
                   elements fitted, \c WAIT_RESULT_CANCELED if the wait was cancelled via
                   IKernel::CancelTaskWait(), \c WAIT_RESULT_FAIL if \a src is \c nullptr or the kernel
                   rejected the wait. On any non-signal result \a written holds the partial count.
    */
    EWaitResult WriteBulkEx(const T *src, size_t count, size_t &written,
                            Timeout timeout_ticks = WAIT_INFINITE)
    {
        written = 0U;

        // null source is rejected, an empty request is trivially satisfied
        EWaitResult result = ((src != nullptr) ? WAIT_RESULT_SIGNAL : WAIT_RESULT_FAIL);

        if ((src != nullptr) && (count != 0U))
        {
            const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

            // capture an absolute deadline once, before entering the wait loop,
            // preventing the timeout from resetting on intermediate partial writes
            const Ticks deadline = (timed_wait ?
                (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

            ScopedCriticalSection cs_;

            while (written < count)
            {
                // wait until there is at least 1 free slot
                while (m_count == N)
                {
                    Timeout remaining = timeout_ticks;
                    if (timed_wait)
                    {
                        // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
                        const Ticks left = (deadline - GetTicks());
                        remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
                    }

                    const EWaitResult wr = m_cv_not_full.WaitEx(cs_, remaining);
                    if (wr != WAIT_RESULT_SIGNAL)
                    {
                        result = wr;
                        break; // break inner condition variable loop
                    }
                }

                // if the wait failed (timeout, cancellation), drop out of the chunk processing loop
                if (result != WAIT_RESULT_SIGNAL)
                {
                    break;
                }

                // calculate how many we can copy in this contiguous stretch
                const size_t available = N - m_count;
                const size_t to_write  = ((count - written) < available) ? (count - written) : available;

                // copy from source
                // note: if value type is not scalar or queue is small we copy with a for loop,
                //       otherwise using faster memcpy version for large scalar arrays
                if (!std::is_scalar<T>::value || (N < 8U))
                {
                    for (size_t i = 0U; i < to_write; ++i)
                    {
                        m_buffer[m_head] = src[written++];
                        m_head           = (m_head + 1U) % N;
                        m_count         += 1U;
                    }
                }
                else
                {
                    const size_t first_part = N - m_head;

                    if (to_write <= first_part)
                    {
                        STK_MEMCPY(&m_buffer[m_head], &src[written], to_write * sizeof(T));
                    }
                    else
                    {
                        STK_MEMCPY(&m_buffer[m_head], &src[written],              first_part              * sizeof(T));
                        STK_MEMCPY(&m_buffer[0U],     &src[written + first_part], (to_write - first_part) * sizeof(T));
                    }

                    written += to_write;
                    m_head   = (m_head + to_write) % N;
                    m_count += to_write;
                }

                // notify consumers that data is ready
                m_cv_not_empty.NotifyAll_CS();
            }
        }

        return result;
    }

    /*! \brief     Write multiple elements to the pipe.
        \details   Copies a block of data into the FIFO. If the pipe does not have
                   enough space for the entire block, it will block until the full
                   amount can be written or the timeout expires.
        \param[in] src: Pointer to the source array.
        \param[in] count: Number of elements to write.
        \param[in] timeout_ticks: Maximum time to wait for sufficient space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    Number of elements actually written. Equal to \c count unless a timeout or a
                   cancellation occurred. Use \c WriteBulkEx() to tell these apart.
        \code
        // Example:
        Sample frame[64];
        FillFrame(frame);

        size_t result = g_Pipe.WriteBulk(frame, 64, 500);
        if (result < 64) {
            // handle partial write / timeout
        }
        \endcode
    */
    size_t WriteBulk(const T *src, size_t count, Timeout timeout_ticks = WAIT_INFINITE)
    {
        size_t written = 0U;
        const EWaitResult result = WriteBulkEx(src, count, written, timeout_ticks);
        STK_UNUSED(result);
        return written;
    }

    /*! \brief     Attempt to write multiple elements to the pipe without blocking.
        \details   Copies as many elements as possible without blocking. Elements that
                   do not fit are discarded.
        \param[in] src:   Pointer to the source array.
        \param[in] count: Number of elements to write.
        \warning   ISR-safe.
        \return    Number of elements actually written.
    */
    size_t TryWriteBulk(const T *src, size_t count) { return WriteBulk(src, count, NO_WAIT); }

    /*! \brief     Read data from the pipe, preserving the full wait outcome.
        \details   Identical to \c Read(), except the caller gets the raw \c EWaitResult, so a
                   cancellation (via IKernel::CancelTaskWait()) can be distinguished from a timeout.
        \param[out] data: Reference to the variable where the retrieved data will be stored.
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if an element was read, \c WAIT_RESULT_TIMEOUT if no data became
                   available before the timeout expired (or \a NO_WAIT was passed on an empty pipe),
                   \c WAIT_RESULT_CANCELED if the wait was cancelled via IKernel::CancelTaskWait(),
                   \c WAIT_RESULT_FAIL if the kernel rejected the wait. \a data is not modified on a
                   non-signal result.
    */
    EWaitResult ReadEx(T &data, Timeout timeout_ticks = WAIT_INFINITE)
    {
        ScopedCriticalSection cs_;
        EWaitResult result = WAIT_RESULT_SIGNAL;

        const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

        // capture an absolute deadline once, before entering the wait loop; a different
        // consumer can steal the just-produced element between NotifyOne_CS() and this
        // task re-acquiring the critical section (Mesa-style wakeup, not a direct handoff),
        // so re-passing the original timeout_ticks on each retry would silently restart it
        const Ticks deadline = (timed_wait ?
            (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

        while (m_count == 0U)
        {
            Timeout remaining = timeout_ticks;
            if (timed_wait)
            {
                // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
                const Ticks left = (deadline - GetTicks());
                remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
            }

            result = m_cv_not_empty.WaitEx(cs_, remaining);
            if (result != WAIT_RESULT_SIGNAL)
            {
                // timeout, cancellation or kernel failure: nothing is read
                break;
            }
        }

        if (result == WAIT_RESULT_SIGNAL)
        {
            data     = m_buffer[m_tail];
            m_tail   = (m_tail + 1U) % N;
            m_count -= 1U;

            // notify producer that space is available
            m_cv_not_full.NotifyOne_CS();
        }

        return result;
    }

    /*! \brief     Read data from the pipe.
        \details   Attempts to retrieve an element from the FIFO queue. If pipe is empty,
                   the calling task will be suspended until data is provided by a
                   producer or the timeout expires.
        \param[out] data: Reference to the variable where the retrieved data will be stored.
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c true if data was successfully read, \c false otherwise (timeout, cancellation or
                   NO_WAIT on an empty pipe). Use \c ReadEx() to tell these apart.
    */
    bool Read(T &data, Timeout timeout_ticks = WAIT_INFINITE)
    {
        return (ReadEx(data, timeout_ticks) == WAIT_RESULT_SIGNAL);
    }

    /*! \brief     Attempt to read data from the pipe without blocking.
        \details   Dequeues an element only if one is immediately available.
                   Returns \c false instantly if the pipe is empty.
        \param[out] data: Reference to the variable where the retrieved data will be stored.
        \warning   ISR-safe.
        \return    \c true if data was successfully read, \c false if the pipe was empty.
    */
    bool TryRead(T &data) { return Read(data, NO_WAIT); }

    /*! \brief     Read multiple elements from the pipe, preserving the full wait outcome.
        \details   Identical to \c ReadBulk(), except the caller gets the raw \c EWaitResult and the
                   number of elements read through \a read_count, so a cancellation (via
                   IKernel::CancelTaskWait()) can be distinguished from a timeout. Elements already
                   copied into \a dst before a timeout or cancellation stay valid and are counted.
        \param[out] dst: Pointer to the destination array.
        \param[in] count: Number of elements to read.
        \param[out] read_count: Number of elements actually read (always set, 0 on \c WAIT_RESULT_FAIL).
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    \c WAIT_RESULT_SIGNAL if all \a count elements were read (also for \a count = 0),
                   \c WAIT_RESULT_TIMEOUT if the timeout expired (or \a NO_WAIT was passed) before all
                   elements were available, \c WAIT_RESULT_CANCELED if the wait was cancelled via
                   IKernel::CancelTaskWait(), \c WAIT_RESULT_FAIL if \a dst is \c nullptr or the kernel
                   rejected the wait. On any non-signal result \a read_count holds the partial count.
    */
    EWaitResult ReadBulkEx(T *dst, size_t count, size_t &read_count,
                           Timeout timeout_ticks = WAIT_INFINITE)
    {
        read_count = 0U;

        // null destination is rejected, an empty request is trivially satisfied
        EWaitResult result = ((dst != nullptr) ? WAIT_RESULT_SIGNAL : WAIT_RESULT_FAIL);

        if ((dst != nullptr) && (count != 0U))
        {
            const bool timed_wait = (timeout_ticks != WAIT_INFINITE) && (timeout_ticks != NO_WAIT);

            // capture an absolute deadline once, before entering the wait loop,
            // preventing the timeout from resetting on intermediate partial reads
            const Ticks deadline = (timed_wait ?
                (GetTicks() + timeout_ticks) : static_cast<Ticks>(0));

            ScopedCriticalSection cs_;

            while (read_count < count)
            {
                // wait until there is at least 1 element available
                while (m_count == 0U)
                {
                    Timeout remaining = timeout_ticks;
                    if (timed_wait)
                    {
                        // left <= timeout_ticks < WAIT_INFINITE, so the narrowing cast is lossless
                        const Ticks left = (deadline - GetTicks());
                        remaining = ((left > 0) ? static_cast<Timeout>(left) : NO_WAIT);
                    }

                    const EWaitResult wr = m_cv_not_empty.WaitEx(cs_, remaining);
                    if (wr != WAIT_RESULT_SIGNAL)
                    {
                        result = wr;
                        break; // break inner condition variable loop
                    }
                }

                // if the wait failed (timeout, cancellation), drop out of the chunk processing loop
                if (result != WAIT_RESULT_SIGNAL)
                {
                    break;
                }

                // determine how many we can pull in this stretch
                const size_t to_read = (count - read_count) < m_count ? (count - read_count) : m_count;

                // note: if value type is not scalar or queue is small we copy with a for loop,
                //       otherwise using faster memcpy version for large scalar arrays
                if (!std::is_scalar<T>::value || (N < 8U))
                {
                    for (size_t i = 0U; i < to_read; ++i)
                    {
                        dst[read_count++] = m_buffer[m_tail];
                        m_tail            = (m_tail + 1U) % N;
                        m_count          -= 1U;
                    }
                }
                else
                {
                    const size_t first_part = N - m_tail;

                    if (to_read <= first_part)
                    {
                        STK_MEMCPY(&dst[read_count], &m_buffer[m_tail], to_read * sizeof(T));
                    }
                    else
                    {
                        STK_MEMCPY(&dst[read_count],              &m_buffer[m_tail], first_part              * sizeof(T));
                        STK_MEMCPY(&dst[read_count + first_part], &m_buffer[0U],     (to_read - first_part)  * sizeof(T));
                    }

                    read_count += to_read;
                    m_tail      = (m_tail + to_read) % N;
                    m_count    -= to_read;
                }

                // notify producers that space is now available
                m_cv_not_full.NotifyAll_CS();
            }
        }

        return result;
    }

    /*! \brief     Read multiple elements from the pipe.
        \details   Attempts to retrieve a block of data from the FIFO. If pipe does not
                   contain enough elements to satisfy the requested count, it will block
                   until the full amount is read or the timeout expires.
        \param[out] dst: Pointer to the destination array.
        \param[in] count: Number of elements to read.
        \param[in] timeout_ticks: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
        \return    Number of elements actually read. Equal to \c count unless a timeout or a
                   cancellation occurred. Use \c ReadBulkEx() to tell these apart.
        \code
        // Example:
        Sample frame[64];

        size_t result = g_Pipe.ReadBulk(frame, 64, 500);
        if (result == 64) {
            // process full frame
        } else {
            // handle partial read / timeout
        }
        \endcode
    */
    size_t ReadBulk(T *dst, size_t count, Timeout timeout_ticks = WAIT_INFINITE)
    {
        size_t read_count = 0U;
        const EWaitResult result = ReadBulkEx(dst, count, read_count, timeout_ticks);
        STK_UNUSED(result);
        return read_count;
    }

    /*! \brief     Attempt to read multiple elements from the pipe without blocking.
        \details   Reads as many elements as are currently available without blocking.
        \param[out] dst:   Pointer to the destination array.
        \param[in] count: Number of elements to read.
        \warning   ISR-safe.
        \return    Number of elements actually read.
    */
    size_t TryReadBulk(T *dst, size_t count) { return ReadBulk(dst, count, NO_WAIT); }

    /*! \brief     Discard all elements and reset the pipe to the empty state.
        \details   Resets head, tail and count to zero. Any tasks blocked in \c Write()
                   are woken so they can re-evaluate and enqueue into the now-empty pipe.
        \warning   Elements that were in the pipe are silently discarded.
                   Ensure no consumers depend on them before calling Reset().
        \warning   ISR-safe.
    */
    void Reset()
    {
        const ScopedCriticalSection cs_;

        m_count = 0U;
        m_head  = 0U;
        m_tail  = 0U;

        // wake all blocked producers: the pipe is now entirely empty, every slot is free
        // note: we do not release readers here
        m_cv_not_full.NotifyAll_CS();
    }

    /*! \brief     Get the maximum number of elements the pipe can hold.
        \return    Compile-time capacity \c N.
        \note      ISR-safe.
    */
    size_t GetCapacity() const { return N; }

    /*! \brief     Get the current number of elements in the pipe.
        \return    Point-in-time snapshot of the element count. May be stale by the time
                   the caller acts on it in a multi-task environment.
        \note      ISR-safe on targets where a size_t-aligned read is atomic.
    */
    size_t GetCount() const { return m_count; }

    /*! \brief     Get the number of free slots currently available.
        \return    Point-in-time snapshot of the free-slot count.
        \note      ISR-safe.
    */
    size_t GetSpace() const { return (N - m_count); }

    /*! \brief     Check if the pipe is currently empty.
        \return    \c true if empty, otherwise \c false.
        \note      The returned value is a point-in-time snapshot.
        \note      ISR-safe.
    */
    bool IsEmpty() const { return (m_count == 0U); }

    /*! \brief     Check if the pipe is currently full.
        \return    \c true if full, otherwise \c false.
        \note      The returned value is a point-in-time snapshot.
        \note      ISR-safe.
    */
    bool IsFull() const { return (m_count == N); }

private:
    STK_NONCOPYABLE_CLASS(PipeT);

    T                 m_buffer[N];    //!< static storage for FIFO elements
    size_t            m_head;         //!< index of the next slot to be written (producer)
    size_t            m_tail;         //!< index of the next slot to be read (consumer)
    size_t            m_count;        //!< current number of elements stored in the pipe
    ConditionVariable m_cv_not_empty; //!< condition variable signaled when the pipe is no longer empty
    ConditionVariable m_cv_not_full;  //!< condition variable signaled when the pipe is no longer full
};

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_PIPE_H_ */
