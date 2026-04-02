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

#include <type_traits>
#include <cstring> // for memcpy

#include "stk_sync_cv.h"

/*! \file  stk_sync_pipe.h
    \brief Implementation of synchronization primitive: stk::sync::Pipe.
*/

namespace stk {
namespace sync {

/*! \class  Pipe
    \brief  Thread-safe FIFO communication pipe for inter-task data passing.
    \tparam T Data type of elements.
    \tparam N Capacity of the pipe (number of elements).

    Pipe provides a synchronized ring-buffer mechanism that allows tasks to
    exchange data safely. It implements blocking semantics:
     - \c Write() blocks if the pipe is full until space becomes available.
     - \c Read() blocks if the pipe is empty until data is produced.

    \code
    // Example: Producer-Consumer pattern
    stk::sync::Pipe<uint32_t, 8> g_DataPipe;

    void Task_Producer() {
        uint32_t value = 42;
        // blocks if pipe is full
        g_DataPipe.Write(value);
    }

    void Task_Consumer() {
        uint32_t received;
        // blocks until data is available, with a 1s timeout
        if (g_DataPipe.Read(received, 1000)) {
            // ... process received value ...
        }
    }
    \endcode

    \see  Mutex, ConditionVariable
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
template <typename T, size_t N>
class Pipe
{
public:
    /*! \brief     Constructor.
    */
    explicit Pipe() : m_buffer(), m_head(0U), m_tail(0U), m_count(0U), m_cv_empty(), m_cv_full()
    {}

    /*! \brief     Write data to the pipe.
        \details   Attempts to push an element into the FIFO queue. If pipe is full, the
                   calling task will be suspended until space is made available by a
                   consumer or the timeout expires.
        \param[in] data: Reference to the data element to be copied into the pipe.
        \param[in] timeout: Maximum time to wait for space (ticks). Default: \c WAIT_INFINITE.
        \warning   ISR-safe only with timeout=NO_WAIT, ISR-unsafe otherwise.
        \return    \c true if data was successfully written, \c false if timeout expired
                   before space became available.
    */
    bool Write(const T &data, Timeout timeout = WAIT_INFINITE)
    {
        ScopedCriticalSection cs_;

        while (m_count == N)
        {
            if (!m_cv_full.Wait(cs_, timeout))
                return false;
        }

        m_buffer[m_head] = data;
        m_head = (m_head + 1U) % N;
        m_count += 1U;

        // notify consumer that data is available
        m_cv_empty.NotifyOne();

        return true;
    }

    /*! \brief     Attempt to write data to the pipe.
        \details   Attempts to push an element into the FIFO queue. If pipe is full, a false
                   will be returned without waiting for a free space.
        \param[in] data: Reference to the data element to be copied into the pipe.
        \warning   ISR-safe.
        \return    \c true if data was successfully written, \c false if no space is available.
    */
    bool TryWrite(const T &data) { return Write(data, NO_WAIT); }

    /*! \brief     Write multiple elements to the pipe.
        \details   Copies a block of data into the FIFO. If the pipe does not have
                   enough space for the entire block, it will block until the full
                   amount can be written or the timeout expires.
        \param[in] src: Pointer to the source array.
        \param[in] count: Number of elements to write.
        \param[in] timeout: Maximum time to wait for sufficient space (ticks).
        \warning   ISR-safe only with timeout=NO_WAIT, ISR-unsafe otherwise.
        \return    Number of elements actually written. This will be equal to \c count
                   unless a timeout occurred.
        \code
        // Example:
        Sample frame[64];
        FillFrame(frame); // e.g., from decoder

        // move the whole block
        size_t result = g_Pipe.WriteBulk(frame, 64, 500);
        if (result < 64) {
            // handle buffer underrun/timeout
        }
        \endcode
    */
    size_t WriteBulk(const T *src, size_t count, Timeout timeout = WAIT_INFINITE)
    {
        if ((src == nullptr) || (count == 0U))
            return 0U;

        size_t written = 0U;
        ScopedCriticalSection cs_;

        while (written < count)
        {
            // wait until there is at least 1 slot available
            while (m_count == N)
            {
                if (!m_cv_full.Wait(cs_, timeout))
                    return written; // Return partial count on timeout
            }

            // calculate how many we can copy in this contiguous stretch
            size_t available = N - m_count;
            size_t to_write = ((count - written) < available ? (count - written) : available);

            // copy from source
            // note: if value type is not scalar or queue is small we copy with a for loop,
            //       otherwise using faster memcpy version for large scalar arrays
            if (!std::is_scalar<T>::value && (N < 8))
            {
                for (size_t i = 0; i < to_write; ++i)
                {
                    m_buffer[m_head] = src[written++];
                    m_head = (m_head + 1U) % N;
                    m_count += 1U;
                }
            }
            else
            {
                size_t first_part = N - m_head;
                if (to_write <= first_part)
                {
                    memcpy(&m_buffer[m_head], &src[written], (to_write * sizeof(T)));
                }
                else
                {
                    memcpy(&m_buffer[m_head], &src[written], (first_part * sizeof(T)));
                    memcpy(&m_buffer[0], &src[written + first_part], ((to_write - first_part) * sizeof(T)));
                }
                written += to_write;
                m_head = (m_head + to_write) % N;
                m_count += to_write;
            }

            // notify consumers that data is ready
            m_cv_empty.NotifyAll();
        }

        return written;
    }

    /*! \brief     Attempt to write multiple elements to the pipe.
        \details   Copies as many elements as possible into the FIFO without blocking.
                   If the pipe does not have enough space for the entire block, only
                   the elements that fit are written and the rest are discarded.
        \param[in] src: Pointer to the source array.
        \param[in] count: Number of elements to write.
        \warning   ISR-safe.
        \return    Number of elements actually written. This will be equal to \c count
                   if all elements were written successfully, or less if the pipe was full
                   or became full before all elements could be written.
    */
    size_t TryWriteBulk(const T *src, size_t count) { return WriteBulk(src, count, NO_WAIT); }

    /*! \brief      Read data from the pipe.
        \details    Attempts to retrieve an element from the FIFO queue. If pipe is empty,
                    the calling task will be suspended until data is provided by a
                    producer or the timeout expires.
        \param[out] data: Reference to the variable where the retrieved data will be stored.
        \param[in]  timeout: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning    ISR-safe only with timeout=NO_WAIT, ISR-unsafe otherwise.
        \return     \c true if data was successfully read, \c false if timeout expired before
                    any data became available.
    */
    bool Read(T &data, Timeout timeout = WAIT_INFINITE)
    {
        ScopedCriticalSection cs_;

        while (m_count == 0U)
        {
            if (!m_cv_empty.Wait(cs_, timeout))
                return false;
        }

        data = m_buffer[m_tail];
        m_tail = (m_tail + 1U) % N;
        m_count -= 1U;

        // notify producer that space is available
        m_cv_full.NotifyOne();

        return true;
    }

    /*! \brief      Attempt to read data from the pipe.
        \details    Checks if an element is available in the FIFO and retrieves it
                    immediately without blocking. If the pipe is empty, returns \c false
                    without yielding the CPU or touching the kernel wait list.
        \param[out] data: Reference to the variable where the retrieved data will be stored.
        \warning    ISR-safe.
        \return     \c true if data was successfully read, \c false if the pipe was empty.
    */
    bool TryRead(T &data) { return Read(data, NO_WAIT); }

    /*! \brief      Read multiple elements from the pipe.
        \details    Attempts to retrieve a block of data from the FIFO. If pipe does not
                    contain enough elements to satisfy the requested count, it will block
                    until the full amount is read or the timeout expires.
        \param[out] dst: Pointer to the destination array.
        \param[in]  count: Number of elements to read.
        \param[in]  timeout: Maximum time to wait for data (ticks). Default: \c WAIT_INFINITE.
        \warning    ISR-safe only with timeout=NO_WAIT, ISR-unsafe otherwise.
        \return     Number of elements actually read. This will be equal to \c count
                    unless a timeout occurred.
        \code
        // Example:
        Sample frame[64];

        // read the whole block
        size_t result = g_Pipe.ReadBulk(frame, 64, 500);
        if (result == 64) {
            // handle data processing
        } else {
            // handle partial data due to underrun/timeout
        }
        \endcode
    */
    size_t ReadBulk(T *dst, size_t count, Timeout timeout = WAIT_INFINITE)
    {
        if ((dst == nullptr) || (count == 0U))
            return 0U;

        size_t read_count = 0U;

        ScopedCriticalSection cs_;

        while (read_count < count)
        {
            // wait until there is at least 1 element available
            while (m_count == 0U)
            {
                if (!m_cv_empty.Wait(cs_, timeout))
                    return read_count; // return partial count on timeout
            }

            // determine how many we can pull in this stretch
            size_t to_read = (count - read_count) < m_count ? (count - read_count) : m_count;

            // copy to destination
            // note: if value type is not scalar or queue is small we copy with a for loop,
            //       otherwise using faster memcpy version for large scalar arrays
            if (!std::is_scalar<T>::value || (N < 8))
            {
                for (size_t i = 0U; i < to_read; ++i)
                {
                    dst[read_count++] = m_buffer[m_tail];
                    m_tail = (m_tail + 1) % N;
                    m_count -= 1U;
                }
            }
            else
            {
                size_t first_part = N - m_tail;
                if (to_read <= first_part)
                {
                    memcpy(&dst[read_count], &m_buffer[m_tail], (to_read * sizeof(T)));
                }
                else
                {
                    memcpy(&dst[read_count], &m_buffer[m_tail], (first_part * sizeof(T)));
                    memcpy(&dst[read_count + first_part], &m_buffer[0], ((to_read - first_part) * sizeof(T)));
                }
                read_count += to_read;
                m_tail = (m_tail + to_read) % N;
                m_count -= to_read;
            }

            // notify producers that space is now available
            m_cv_full.NotifyAll();
        }

        return read_count;
    }

    /*! \brief      Attempt to read multiple elements from the pipe.
        \details    Reads as many elements as are currently available in the FIFO without
                    blocking. If fewer elements than requested are available, only the
                    available elements are read.
        \param[out] dst: Pointer to the destination array.
        \param[in]  count: Number of elements to read.
        \warning    ISR-safe.
        \return     Number of elements actually read. This will be equal to \c count
                    if all requested elements were available, or less if the pipe was empty
                    or became empty before all elements could be read.
    */
    size_t TryReadBulk(T *dst, size_t count) { return ReadBulk(dst, count, NO_WAIT); }

    /*! \brief     Get the current number of elements in the pipe.
        \return    The number of elements currently stored in the FIFO buffer.

        \note      The returned value is a point-in-time snapshot. In a multi-tasking
                   environment, queue size may change immediately after function returns if
                   a producer or consumer is active in another task.
    */
    size_t GetSize() const { return m_count; }

    /*! \brief     Check if queue is currently empty.
        \return    True if empty, otherwise false.

        \note      The returned value is a point-in-time snapshot. In a multi-tasking
                   environment, queue size may change immediately after function returns if
                   a producer or consumer is active in another task.
    */
    bool IsEmpty() const { return (GetSize() == 0); }

private:
    STK_NONCOPYABLE_CLASS(Pipe);

    T                 m_buffer[N]; //!< static storage for FIFO elements
    size_t            m_head;      //!< index of the next slot to be written (producer)
    size_t            m_tail;      //!< index of the next slot to be read (consumer)
    size_t            m_count;     //!< current number of elements stored in the pipe
    ConditionVariable m_cv_empty;  //!< condition variable signaled when the pipe is no longer empty
    ConditionVariable m_cv_full;   //!< condition variable signaled when the pipe is no longer full
};

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_PIPE_H_ */

