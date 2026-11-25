/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk.h>
#include <sync/stk_sync_msgqueue.h>
#include <assert.h>
#include <string.h>

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_MQ_TEST_TASKS_MAX   5
#define _STK_MQ_TEST_TIMEOUT     1000
#define _STK_MQ_TEST_SHORT_SLEEP 10
#define _STK_MQ_TEST_LONG_SLEEP  100
#define _STK_MQ_CAPACITY         8U
#define _STK_MQ_MSG_SIZE         16U   // bytes per message slot
#ifdef __ARM_ARCH_6M__
#define _STK_MQ_STACK_SIZE       128   // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_MQ_STACK_SIZE       256
#define STK_TASK                 static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::msgqueue
    \brief     Namespace of sync::MessageQueue / MessageQueueT test.
 */
namespace msgqueue {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_InstancesDone = 0;
static volatile int32_t g_SharedCounter = 0;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_MQ_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

// Shared queue pointer; the concrete queue is re-created inside each RunTest call
static stk::sync::MessageQueue *g_Queue = nullptr;

// ---------------------------------------------------------------------------
// Test 1 – TryPut / TryGet basic cycle (single task)
// ---------------------------------------------------------------------------

/*! \class TryPutGetTask
    \brief Verifies TryPut enqueues a message, TryGet retrieves it intact, and
           queue accounting stays consistent throughout.
*/
template <EAccessMode _AccessMode>
class TryPutGetTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryPutGetTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // Queue must start empty
            ok &= g_Queue->IsEmpty();
            ok &= (g_Queue->GetCount()    == 0U);
            ok &= (g_Queue->GetSpace()    == _STK_MQ_CAPACITY);
            ok &= (g_Queue->GetCapacity() == _STK_MQ_CAPACITY);
            ok &= (g_Queue->GetMsgSize()  == _STK_MQ_MSG_SIZE);
            ok &= g_Queue->IsStorageValid();

            // Enqueue one message
            uint8_t tx[_STK_MQ_MSG_SIZE];
            memset(tx, 0xAB, sizeof(tx));
            ok &= g_Queue->TryPut(tx);

            ok &= (g_Queue->GetCount() == 1U);
            ok &= (g_Queue->GetSpace() == _STK_MQ_CAPACITY - 1U);
            ok &= !g_Queue->IsEmpty();

            // Dequeue and verify payload
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryGet(rx);
            ok &= (memcmp(tx, rx, _STK_MQ_MSG_SIZE) == 0);
            ok &= g_Queue->IsEmpty();
            ok &= (g_Queue->GetCount() == 0U);

            // Second TryGet on empty queue must fail
            ok &= !g_Queue->TryGet(rx);

            printf("TryPutGet: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 2 – Fill queue to capacity, verify IsFull, then drain
// ---------------------------------------------------------------------------

/*! \class FillDrainTask
    \brief Fills the queue to capacity via TryPut, checks IsFull() and that a
           further TryPut returns false, then drains every message in FIFO order.
*/
template <EAccessMode _AccessMode>
class FillDrainTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    FillDrainTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // Fill queue
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t msg[_STK_MQ_MSG_SIZE];
                memset(msg, (uint8_t)i, sizeof(msg));
                ok &= g_Queue->TryPut(msg);
            }

            ok &= g_Queue->IsFull();
            ok &= (g_Queue->GetCount() == _STK_MQ_CAPACITY);
            ok &= (g_Queue->GetSpace() == 0U);

            // One more TryPut must fail without blocking
            uint8_t extra[_STK_MQ_MSG_SIZE] = {};
            ok &= !g_Queue->TryPut(extra);

            // Drain and verify FIFO order
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t rx[_STK_MQ_MSG_SIZE] = {};
                ok &= g_Queue->TryGet(rx);
                // Each slot was filled with byte value == slot index
                uint8_t expected[_STK_MQ_MSG_SIZE];
                memset(expected, (uint8_t)i, sizeof(expected));
                ok &= (memcmp(rx, expected, _STK_MQ_MSG_SIZE) == 0);
            }

            ok &= g_Queue->IsEmpty();

            printf("FillDrain: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 3 – Blocking Get: producer wakes a blocked consumer
// ---------------------------------------------------------------------------

/*! \class BlockingGetTask
    \brief Task 1 blocks in Get() on an empty queue; Task 0 puts one message
           and verifies Task 1 unblocks and receives the correct payload.
*/
template <EAccessMode _AccessMode>
class BlockingGetTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BlockingGetTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Let task 1 enter blocking Get() first
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            // Send a distinctive message
            uint8_t tx[_STK_MQ_MSG_SIZE];
            memset(tx, 0xCD, sizeof(tx));
            g_Queue->Put(tx);

            // Give task 1 time to process then verify
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);
        }
        else
        if (m_task_id == 1)
        {
            // Queue is empty; Get() must block until task 0 sends
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            bool got = g_Queue->Get(rx);

            if (got)
            {
                uint8_t expected[_STK_MQ_MSG_SIZE];
                memset(expected, 0xCD, sizeof(expected));
                if (memcmp(rx, expected, _STK_MQ_MSG_SIZE) == 0)
                    g_SharedCounter = 1; // signal success
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

            bool ok = (g_SharedCounter == 1) && g_Queue->IsEmpty();
            printf("BlockingGet: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 4 – Blocking Put: consumer wakes a blocked producer
// ---------------------------------------------------------------------------

/*! \class BlockingPutTask
    \brief Task 0 fills the queue; Task 1 blocks in Put(); Task 0 then calls
           Get() to free a slot and verifies Task 1 unblocks and enqueues.
*/
template <EAccessMode _AccessMode>
class BlockingPutTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BlockingPutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Fill queue completely
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t msg[_STK_MQ_MSG_SIZE] = {};
                g_Queue->TryPut(msg);
            }

            // Give task 1 time to enter blocking Put()
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            // Make room — must unblock task 1
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            g_Queue->Get(rx);

            // Wait for task 1, then drain everything
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);
        }
        else
        if (m_task_id == 1)
        {
            // Queue is full; Put() must block until task 0 reads a message
            uint8_t tx[_STK_MQ_MSG_SIZE];
            memset(tx, 0xEF, sizeof(tx));
            bool sent = g_Queue->Put(tx);

            if (sent)
                g_SharedCounter = 1; // signal success
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

            bool ok = (g_SharedCounter == 1) && g_Queue->IsEmpty();
            printf("BlockingPut: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 5 – Timed Get timeout: expires when queue remains empty
// ---------------------------------------------------------------------------

/*! \class TimedGetTimeoutTask
    \brief Task 1 calls Get() with a short timeout on an always-empty queue;
           verifies the call returns false within the expected time window.
*/
template <EAccessMode _AccessMode>
class TimedGetTimeoutTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedGetTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 1)
        {
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};

            int64_t start   = GetTimeNowMs();
            bool    got     = g_Queue->Get(rx, 50); // 50 ms timeout
            int64_t elapsed = GetTimeNowMs() - start;

            bool ok = !got && (elapsed >= 45) && (elapsed <= 65);
            g_SharedCounter = ok ? 1 : 0;

            printf("TimedGetTimeout: got=%s elapsed=%d %s\n",
                got ? "true" : "false", (int)elapsed, ok ? "PASS" : "FAIL");
        }

        ++g_InstancesDone;

        if (m_task_id == 1)
        {
            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 6 – Timed Get success: message arrives before timeout
// ---------------------------------------------------------------------------

/*! \class TimedGetSuccessTask
    \brief Task 0 sends a message after a short delay; Task 1 calls Get() with
           a generous timeout and must receive the message successfully.
*/
template <EAccessMode _AccessMode>
class TimedGetSuccessTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedGetSuccessTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            stk::Sleep(40); // send before task 1's 150 ms timeout

            uint8_t tx[_STK_MQ_MSG_SIZE];
            memset(tx, 0x55, sizeof(tx));
            g_Queue->TryPut(tx);

            stk::Sleep(_STK_MQ_TEST_LONG_SLEEP);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP); // let task 0 start first

            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            bool    got = g_Queue->Get(rx, 150); // ample timeout

            bool ok = got;
            g_SharedCounter = ok ? 1 : 0;

            printf("TimedGetSuccess: %s\n", ok ? "PASS" : "FAIL");
        }

        ++g_InstancesDone;

        if (m_task_id == 1)
        {
            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 7 – Timed Put timeout: expires when queue remains full
// ---------------------------------------------------------------------------

/*! \class TimedPutTimeoutTask
    \brief Task 0 fills the queue and holds it for longer than Task 1's timeout;
           Task 1's Put() must return false within the expected window.
*/
template <EAccessMode _AccessMode>
class TimedPutTimeoutTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedPutTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Fill queue
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t msg[_STK_MQ_MSG_SIZE] = {};
                g_Queue->TryPut(msg);
            }

            // Hold long enough for task 1 to time out, then drain
            stk::Sleep(200);

            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP); // let task 0 fill queue first

            uint8_t tx[_STK_MQ_MSG_SIZE] = {};

            int64_t start   = GetTimeNowMs();
            bool    sent    = g_Queue->Put(tx, 50); // 50 ms timeout
            int64_t elapsed = GetTimeNowMs() - start;

            bool ok = !sent && (elapsed >= 45) && (elapsed <= 65);
            g_SharedCounter = ok ? 1 : 0;

            printf("TimedPutTimeout: sent=%s elapsed=%d %s\n",
                sent ? "true" : "false", (int)elapsed, ok ? "PASS" : "FAIL");
        }

        ++g_InstancesDone;

        if (m_task_id == 1)
        {
            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 8 – Reset: discards messages and wakes blocked producers
// ---------------------------------------------------------------------------

/*! \class ResetTask
    \brief Task 0 fills the queue; Task 1 blocks in Put(); Task 0 calls Reset()
           which must drain the queue and wake Task 1 so it can re-enqueue.
*/
template <EAccessMode _AccessMode>
class ResetTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ResetTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Fill queue completely
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t msg[_STK_MQ_MSG_SIZE] = {};
                g_Queue->TryPut(msg);
            }

            // Give task 1 time to enter blocking Put()
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            // Reset must empty the queue and wake task 1
            g_Queue->Reset();

            // Allow task 1 to enqueue its message
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            // Drain whatever task 1 sent
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);
        }
        else
        if (m_task_id == 1)
        {
            uint8_t tx[_STK_MQ_MSG_SIZE];
            memset(tx, 0xAA, sizeof(tx));

            // Blocking Put on full queue – must be woken by Reset()
            bool sent = g_Queue->Put(tx);

            if (sent)
                g_SharedCounter = 1;
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

            bool ok = (g_SharedCounter == 1) && g_Queue->IsEmpty();
            printf("Reset: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 9 – Accessors: GetCapacity / GetMsgSize / GetBuffer / IsStorageValid
// ---------------------------------------------------------------------------

/*! \class AccessorsTask
    \brief Verifies that all const accessors report construction-time values
           and that GetBuffer() returns a non-null pointer to accessible memory.
*/
template <EAccessMode _AccessMode>
class AccessorsTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    AccessorsTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            ok &= g_Queue->IsStorageValid();
            ok &= (g_Queue->GetCapacity() == _STK_MQ_CAPACITY);
            ok &= (g_Queue->GetMsgSize()  == _STK_MQ_MSG_SIZE);
            ok &= (g_Queue->GetBuffer()   != nullptr);

            // Verify counts after one put/get cycle
            uint8_t tx[_STK_MQ_MSG_SIZE];
            memset(tx, 0x7E, sizeof(tx));

            g_Queue->TryPut(tx);
            ok &= (g_Queue->GetCount() == 1U);
            ok &= (g_Queue->GetSpace() == _STK_MQ_CAPACITY - 1U);

            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            g_Queue->TryGet(rx);
            ok &= (g_Queue->GetCount() == 0U);
            ok &= (g_Queue->GetSpace() == _STK_MQ_CAPACITY);

            // MessageQueueT<N,MSG> with internal storage must also be valid
            stk::sync::MessageQueueT<4U, 8U> local_q;
            ok &= local_q.IsStorageValid();
            ok &= (local_q.GetCapacity() == 4U);
            ok &= (local_q.GetMsgSize()  == 8U);
            ok &= (local_q.GetBuffer()   != nullptr);

            printf("Accessors: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 10 – Ring-buffer wrap-around: put/get across the slot boundary
// ---------------------------------------------------------------------------

/*! \class WrapAroundTask
    \brief Partially fills the queue, drains some messages to advance the tail,
           then fills past the end of the buffer to exercise the ring-buffer
           modular wrap-around logic.
*/
template <EAccessMode _AccessMode>
class WrapAroundTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WrapAroundTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;
            const size_t HALF = _STK_MQ_CAPACITY / 2U;

            // Fill first half
            for (size_t i = 0; i < HALF; ++i)
            {
                uint8_t tx[_STK_MQ_MSG_SIZE];
                memset(tx, (uint8_t)(i + 1U), sizeof(tx));
                ok &= g_Queue->TryPut(tx);
            }

            // Drain first half (tail advances to HALF)
            for (size_t i = 0; i < HALF; ++i)
            {
                uint8_t rx[_STK_MQ_MSG_SIZE] = {};
                ok &= g_Queue->TryGet(rx);

                uint8_t expected[_STK_MQ_MSG_SIZE];
                memset(expected, (uint8_t)(i + 1U), sizeof(expected));
                ok &= (memcmp(rx, expected, _STK_MQ_MSG_SIZE) == 0);
            }

            ok &= g_Queue->IsEmpty();

            // Fill again to capacity — head wraps around the ring buffer
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t tx[_STK_MQ_MSG_SIZE];
                memset(tx, (uint8_t)(0x80U + i), sizeof(tx));
                ok &= g_Queue->TryPut(tx);
            }

            ok &= g_Queue->IsFull();

            // Drain and verify FIFO payload after wrap
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t rx[_STK_MQ_MSG_SIZE] = {};
                ok &= g_Queue->TryGet(rx);

                uint8_t expected[_STK_MQ_MSG_SIZE];
                memset(expected, (uint8_t)(0x80U + i), sizeof(expected));
                ok &= (memcmp(rx, expected, _STK_MQ_MSG_SIZE) == 0);
            }

            ok &= g_Queue->IsEmpty();

            printf("WrapAround: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 11a – TryPutFront basic: priority ordering (single task)
// ---------------------------------------------------------------------------

/*! \class TryPutFrontTask
    \brief Verifies that TryPutFront inserts a message at the front of the queue,
           making it the next item returned by Get(). Also checks that a mix of
           Put() (back) and PutFront() (front) yields the correct dequeue order,
           and that TryPutFront on a full queue returns false immediately.
*/
template <EAccessMode _AccessMode>
class TryPutFrontTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryPutFrontTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // --- Part 1: single front-insert ---
            // Queue is empty; TryPutFront must succeed and be the only message.
            uint8_t front_msg[_STK_MQ_MSG_SIZE];
            memset(front_msg, 0xF0, sizeof(front_msg));
            ok &= g_Queue->TryPutFront(front_msg);
            ok &= (g_Queue->GetCount() == 1U);
            ok &= (g_Queue->GetSpace() == _STK_MQ_CAPACITY - 1U);

            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryGet(rx);
            ok &= (memcmp(rx, front_msg, _STK_MQ_MSG_SIZE) == 0);
            ok &= g_Queue->IsEmpty();

            // --- Part 2: interleaved Put/PutFront ordering ---
            // Enqueue via the back: A, B.  Then prepend C to the front.
            // Expected dequeue order: C, A, B.
            uint8_t msg_a[_STK_MQ_MSG_SIZE]; memset(msg_a, 0xAA, sizeof(msg_a));
            uint8_t msg_b[_STK_MQ_MSG_SIZE]; memset(msg_b, 0xBB, sizeof(msg_b));
            uint8_t msg_c[_STK_MQ_MSG_SIZE]; memset(msg_c, 0xCC, sizeof(msg_c));

            ok &= g_Queue->TryPut(msg_a);
            ok &= g_Queue->TryPut(msg_b);
            ok &= g_Queue->TryPutFront(msg_c);
            ok &= (g_Queue->GetCount() == 3U);

            uint8_t got[_STK_MQ_MSG_SIZE] = {};

            ok &= g_Queue->TryGet(got);
            ok &= (memcmp(got, msg_c, _STK_MQ_MSG_SIZE) == 0); // C first

            ok &= g_Queue->TryGet(got);
            ok &= (memcmp(got, msg_a, _STK_MQ_MSG_SIZE) == 0); // then A

            ok &= g_Queue->TryGet(got);
            ok &= (memcmp(got, msg_b, _STK_MQ_MSG_SIZE) == 0); // then B

            ok &= g_Queue->IsEmpty();

            // --- Part 3: multiple consecutive TryPutFront calls ---
            // Each front-insert becomes the new head, so dequeue order is LIFO
            // with respect to the front-insert sequence: D, E, F inserted at
            // front → dequeue order F, E, D.
            uint8_t msg_d[_STK_MQ_MSG_SIZE]; memset(msg_d, 0xD0, sizeof(msg_d));
            uint8_t msg_e[_STK_MQ_MSG_SIZE]; memset(msg_e, 0xE0, sizeof(msg_e));
            uint8_t msg_f[_STK_MQ_MSG_SIZE]; memset(msg_f, 0xFF, sizeof(msg_f));

            ok &= g_Queue->TryPutFront(msg_d);
            ok &= g_Queue->TryPutFront(msg_e);
            ok &= g_Queue->TryPutFront(msg_f);
            ok &= (g_Queue->GetCount() == 3U);

            ok &= g_Queue->TryGet(got);
            ok &= (memcmp(got, msg_f, _STK_MQ_MSG_SIZE) == 0);

            ok &= g_Queue->TryGet(got);
            ok &= (memcmp(got, msg_e, _STK_MQ_MSG_SIZE) == 0);

            ok &= g_Queue->TryGet(got);
            ok &= (memcmp(got, msg_d, _STK_MQ_MSG_SIZE) == 0);

            ok &= g_Queue->IsEmpty();

            // --- Part 4: TryPutFront on full queue must fail without blocking ---
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t fill[_STK_MQ_MSG_SIZE] = {};
                ok &= g_Queue->TryPut(fill);
            }
            ok &= g_Queue->IsFull();

            uint8_t extra[_STK_MQ_MSG_SIZE] = {};
            ok &= !g_Queue->TryPutFront(extra); // must return false
            ok &= (g_Queue->GetCount() == _STK_MQ_CAPACITY); // count unchanged

            // Drain for cleanliness
            while (!g_Queue->IsEmpty())
            {
                uint8_t tmp[_STK_MQ_MSG_SIZE] = {};
                g_Queue->TryGet(tmp);
            }

            printf("TryPutFront: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 11b – PutFront wrap-around: front-insert across the ring-buffer boundary
// ---------------------------------------------------------------------------

/*! \class PutFrontWrapAroundTask
    \brief Advances both head and tail into the middle of the ring buffer by
           filling and partially draining the queue, then exercises TryPutFront
           so that the retreated tail pointer wraps from index 0 back to
           CAPACITY-1, verifying correct modular arithmetic and payload
           integrity across the boundary.
*/
template <EAccessMode _AccessMode>
class PutFrontWrapAroundTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PutFrontWrapAroundTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;
            const size_t HALF = _STK_MQ_CAPACITY / 2U;

            // Step 1: fill the queue completely, then drain it entirely so that
            // both head and tail sit at 0.
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t tx[_STK_MQ_MSG_SIZE];
                memset(tx, (uint8_t)i, sizeof(tx));
                ok &= g_Queue->TryPut(tx);
            }
            while (!g_Queue->IsEmpty())
            {
                uint8_t rx[_STK_MQ_MSG_SIZE] = {};
                g_Queue->TryGet(rx);
            }
            ok &= g_Queue->IsEmpty();

            // Step 2: put HALF messages via the back so that head == HALF,
            // tail == 0 (tail is at the ring-buffer origin).
            for (size_t i = 0; i < HALF; ++i)
            {
                uint8_t tx[_STK_MQ_MSG_SIZE];
                memset(tx, (uint8_t)(0x10U + i), sizeof(tx));
                ok &= g_Queue->TryPut(tx);
            }
            ok &= (g_Queue->GetCount() == HALF);

            // Step 3: TryPutFront when tail == 0 must wrap to CAPACITY-1.
            // The front-inserted message should be retrieved first.
            uint8_t front[_STK_MQ_MSG_SIZE];
            memset(front, 0xFE, sizeof(front));
            ok &= g_Queue->TryPutFront(front);
            ok &= (g_Queue->GetCount() == HALF + 1U);

            // Step 4: Get() must return the front-inserted message first.
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryGet(rx);
            ok &= (memcmp(rx, front, _STK_MQ_MSG_SIZE) == 0);

            // Step 5: remaining messages must follow in original FIFO order.
            for (size_t i = 0; i < HALF; ++i)
            {
                uint8_t expected[_STK_MQ_MSG_SIZE];
                memset(expected, (uint8_t)(0x10U + i), sizeof(expected));
                memset(rx, 0, sizeof(rx));
                ok &= g_Queue->TryGet(rx);
                ok &= (memcmp(rx, expected, _STK_MQ_MSG_SIZE) == 0);
            }

            ok &= g_Queue->IsEmpty();

            printf("PutFrontWrapAround: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 11c – Blocking PutFront: woken by Get when queue is full
// ---------------------------------------------------------------------------

/*! \class BlockingPutFrontTask
    \brief Task 0 fills the queue completely and then waits; Task 1 calls
           PutFront() on the full queue so it blocks. Task 0 then calls Get()
           to free one slot, which must unblock Task 1. After Task 1 is unblocked
           and its message is front-inserted, Get() must return it before any of
           the originally-queued messages.
*/
template <EAccessMode _AccessMode>
class BlockingPutFrontTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BlockingPutFrontTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Fill queue with a recognizable pattern
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t msg[_STK_MQ_MSG_SIZE];
                memset(msg, (uint8_t)(0x10U + i), sizeof(msg));
                g_Queue->TryPut(msg);
            }

            // Give task 1 time to enter the blocking PutFront()
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            // Free one slot — must unblock task 1
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            g_Queue->Get(rx); // consumes slot 0 (value 0x10)

            // Give task 1 time to complete its PutFront
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);
        }
        else
        if (m_task_id == 1)
        {
            // Queue is full; PutFront() must block until task 0 calls Get()
            uint8_t priority[_STK_MQ_MSG_SIZE];
            memset(priority, 0x50, sizeof(priority)); // 0x50 ('P') sentinel
            bool sent = g_Queue->PutFront(priority);

            if (sent)
                g_SharedCounter = 1; // signal that PutFront eventually succeeded
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

            // The priority message must now be at the front of the queue.
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            bool got_priority = g_Queue->TryGet(rx);
            uint8_t expected[_STK_MQ_MSG_SIZE];
            memset(expected, 0x50, sizeof(expected)); // 'P'
            bool payload_ok = got_priority && (memcmp(rx, expected, _STK_MQ_MSG_SIZE) == 0);

            // Drain leftover messages
            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);

            bool ok = (g_SharedCounter == 1) && payload_ok && g_Queue->IsEmpty();
            printf("BlockingPutFront: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 11d – Timed PutFront timeout: expires when queue remains full
// ---------------------------------------------------------------------------

/*! \class TimedPutFrontTimeoutTask
    \brief Task 0 fills the queue and holds it full for longer than Task 1's
           timeout; Task 1's PutFront() with a short timeout must return false
           within the expected time window, mirroring the TimedPutTimeout test.
*/
template <EAccessMode _AccessMode>
class TimedPutFrontTimeoutTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedPutFrontTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Fill queue
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t msg[_STK_MQ_MSG_SIZE] = {};
                g_Queue->TryPut(msg);
            }

            // Hold full well past task 1's timeout, then drain
            stk::Sleep(200);

            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP); // let task 0 fill queue first

            uint8_t tx[_STK_MQ_MSG_SIZE] = {};

            int64_t start   = GetTimeNowMs();
            bool    sent    = g_Queue->PutFront(tx, 50); // 50 ms timeout
            int64_t elapsed = GetTimeNowMs() - start;

            bool ok = !sent && (elapsed >= 45) && (elapsed <= 65);
            g_SharedCounter = ok ? 1 : 0;

            printf("TimedPutFrontTimeout: sent=%s elapsed=%d %s\n",
                sent ? "true" : "false", (int)elapsed, ok ? "PASS" : "FAIL");
        }

        ++g_InstancesDone;

        if (m_task_id == 1)
        {
            if (g_SharedCounter == 1)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 11 – Multi-task ping-pong: counter incremented by producer + consumer
// ---------------------------------------------------------------------------

/*! \class PingPongTask
    \brief Task 0 (producer) sends sequential counters; Task 1 (consumer)
           receives each message, verifies the value, and increments g_SharedCounter.
           Final counter must equal the iteration count.
*/
template <EAccessMode _AccessMode>
class PingPongTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    PingPongTask(uint8_t task_id, int32_t iterations)
        : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Producer
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                uint8_t tx[_STK_MQ_MSG_SIZE] = {};
                memcpy(tx, &i, sizeof(i));
                g_Queue->Put(tx);
            }
        }
        else
        if (m_task_id == 1)
        {
            // Consumer
            for (int32_t i = 0; i < m_iterations; ++i)
            {
                uint8_t rx[_STK_MQ_MSG_SIZE] = {};
                if (g_Queue->Get(rx))
                {
                    int32_t val = 0;
                    memcpy(&val, rx, sizeof(val));
                    if (val == i)
                        ++g_SharedCounter;
                }
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

            bool ok = (g_SharedCounter == m_iterations) && g_Queue->IsEmpty();
            printf("PingPong: counter=%d (expected %d) %s\n",
                (int)g_SharedCounter, (int)m_iterations, ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 12 – Stress test: all tasks put/get concurrently
// ---------------------------------------------------------------------------

/*! \class StressTask
    \brief All tasks hammer the queue with interleaved TryPut / TryGet /
           blocking Put / blocking Get; verifies the queue never deadlocks and
           all enqueued messages are eventually consumed.
*/
template <EAccessMode _AccessMode>
class StressTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    StressTask(uint8_t task_id, int32_t iterations)
        : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run() override
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            uint8_t buf[_STK_MQ_MSG_SIZE];
            memset(buf, (uint8_t)(m_task_id + i), sizeof(buf));

            // Alternate between producer and consumer role each iteration
            if ((i % 2) == 0)
            {
                // Producer path: rotate between non-blocking, blocking, timed
                bool sent = false;
                switch (i % 3)
                {
                    case 0: sent = g_Queue->TryPut(buf);      break;
                    case 1: sent = g_Queue->Put(buf);         break;
                    case 2: sent = g_Queue->Put(buf, 20);     break;
                    default: break;
                }
                if (sent)
                    ++g_SharedCounter;
            }
            else
            {
                // Consumer path
                bool got = false;
                switch (i % 3)
                {
                    case 0: got = g_Queue->TryGet(buf);       break;
                    case 1: got = g_Queue->Get(buf);          break;
                    case 2: got = g_Queue->Get(buf, 20);      break;
                    default: break;
                }
                if (got)
                    --g_SharedCounter;
            }

            if ((i % 8) == 0)
                stk::Delay(1);
        }

        ++g_InstancesDone;

        if (m_task_id == (_STK_MQ_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_MQ_TEST_TASKS_MAX)
                stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

            // Drain any residual messages left by asymmetric put/get counts
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            while (!g_Queue->IsEmpty())
            {
                g_Queue->TryGet(rx);
                --g_SharedCounter;
            }

            // No queue corruption and no net outstanding messages
            bool ok = (g_SharedCounter == 0) && g_Queue->IsEmpty();

            printf("Stress: net_outstanding=%d pool_empty=%s %s\n",
                (int)g_SharedCounter,
                g_Queue->IsEmpty() ? "yes" : "no",
                ok ? "PASS" : "FAIL");

            if (ok) g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 13a – Peek: inspect next message without consuming it (single task)
// ---------------------------------------------------------------------------

/*! \class PeekTask
    \brief Verifies that Peek() copies the oldest message without removing it,
           that a subsequent Get() returns the same payload, and that TryPeek()
           returns false immediately on an empty queue.
*/
template <EAccessMode _AccessMode>
class PeekTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PeekTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // --- Part 1: TryPeek on empty queue must fail immediately ---
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            ok &= !g_Queue->TryPeek(rx);
            ok &= g_Queue->IsEmpty();

            // --- Part 2: Peek does not consume the message ---
            uint8_t tx[_STK_MQ_MSG_SIZE];
            memset(tx, 0xA5, sizeof(tx));
            ok &= g_Queue->TryPut(tx);
            ok &= (g_Queue->GetCount() == 1U);

            uint8_t peek_rx[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryPeek(peek_rx);
            ok &= (memcmp(peek_rx, tx, _STK_MQ_MSG_SIZE) == 0); // payload matches
            ok &= (g_Queue->GetCount() == 1U);                   // count unchanged
            ok &= !g_Queue->IsEmpty();                           // message still present

            // Get() must return the same message Peek() observed
            uint8_t get_rx[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryGet(get_rx);
            ok &= (memcmp(get_rx, tx, _STK_MQ_MSG_SIZE) == 0);
            ok &= g_Queue->IsEmpty();

            // --- Part 3: Peek is non-destructive across multiple calls ---
            uint8_t msg[_STK_MQ_MSG_SIZE];
            memset(msg, 0x3C, sizeof(msg));
            ok &= g_Queue->TryPut(msg);

            // Peek twice; count must remain 1 both times
            uint8_t p1[_STK_MQ_MSG_SIZE] = {};
            uint8_t p2[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryPeek(p1);
            ok &= g_Queue->TryPeek(p2);
            ok &= (memcmp(p1, msg, _STK_MQ_MSG_SIZE) == 0);
            ok &= (memcmp(p2, msg, _STK_MQ_MSG_SIZE) == 0);
            ok &= (g_Queue->GetCount() == 1U);

            // Consume the message and confirm the queue is empty
            uint8_t fin[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryGet(fin);
            ok &= (memcmp(fin, msg, _STK_MQ_MSG_SIZE) == 0);
            ok &= g_Queue->IsEmpty();

            // --- Part 4: Peek respects FIFO order (oldest message is returned) ---
            uint8_t m0[_STK_MQ_MSG_SIZE]; memset(m0, 0x11, sizeof(m0));
            uint8_t m1[_STK_MQ_MSG_SIZE]; memset(m1, 0x22, sizeof(m1));
            ok &= g_Queue->TryPut(m0);
            ok &= g_Queue->TryPut(m1);

            uint8_t fifo_peek[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryPeek(fifo_peek);
            ok &= (memcmp(fifo_peek, m0, _STK_MQ_MSG_SIZE) == 0); // oldest, not newest

            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);

            printf("Peek: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 13b – PeekFront: inspect front-inserted message without consuming it
// ---------------------------------------------------------------------------

/*! \class PeekFrontTask
    \brief Verifies that PeekFront() copies the most recently front-inserted
           message without removing it, that a subsequent Get() returns the
           same payload, and that TryPeekFront() returns false on an empty queue.
*/
template <EAccessMode _AccessMode>
class PeekFrontTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    PeekFrontTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // --- Part 1: TryPeekFront on empty queue must fail immediately ---
            uint8_t rx[_STK_MQ_MSG_SIZE] = {};
            ok &= !g_Queue->TryPeekFront(rx);
            ok &= g_Queue->IsEmpty();

            // --- Part 2: PeekFront on a pure-Put queue matches Peek ---
            // When no PutFront has been called, PeekFront reads Prev(m_tail)
            // which is the most-recently-written back slot.
            uint8_t back[_STK_MQ_MSG_SIZE];
            memset(back, 0xBB, sizeof(back));
            ok &= g_Queue->TryPut(back);

            uint8_t pf[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryPeekFront(pf);
            ok &= (memcmp(pf, back, _STK_MQ_MSG_SIZE) == 0);
            ok &= (g_Queue->GetCount() == 1U); // non-destructive

            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);

            // --- Part 3: PeekFront returns the PutFront message ---
            // Back-insert A and B, then front-insert C.
            // PeekFront must return C; Peek must still return A.
            uint8_t msg_a[_STK_MQ_MSG_SIZE]; memset(msg_a, 0xAA, sizeof(msg_a));
            uint8_t msg_b[_STK_MQ_MSG_SIZE]; memset(msg_b, 0xBB, sizeof(msg_b));
            uint8_t msg_c[_STK_MQ_MSG_SIZE]; memset(msg_c, 0xCC, sizeof(msg_c));

            ok &= g_Queue->TryPut(msg_a);
            ok &= g_Queue->TryPut(msg_b);
            ok &= g_Queue->TryPutFront(msg_c);
            ok &= (g_Queue->GetCount() == 3U);

            uint8_t peek_result[_STK_MQ_MSG_SIZE]      = {};
            uint8_t peek_front_result[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryPeek(peek_result);
            ok &= g_Queue->TryPeekFront(peek_front_result);
            ok &= (memcmp(peek_result,       msg_c, _STK_MQ_MSG_SIZE) == 0); // oldest = C (front)
            ok &= (memcmp(peek_front_result, msg_c, _STK_MQ_MSG_SIZE) == 0); // front-peek also C
            ok &= (g_Queue->GetCount() == 3U);                                // both non-destructive

            // Consume all three and verify the ordering: C, A, B
            uint8_t got[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryGet(got); ok &= (memcmp(got, msg_c, _STK_MQ_MSG_SIZE) == 0);
            ok &= g_Queue->TryGet(got); ok &= (memcmp(got, msg_a, _STK_MQ_MSG_SIZE) == 0);
            ok &= g_Queue->TryGet(got); ok &= (memcmp(got, msg_b, _STK_MQ_MSG_SIZE) == 0);
            ok &= g_Queue->IsEmpty();

            // --- Part 4: PeekFront tracks the most recent PutFront ---
            // Insert D at front, then E at front. PeekFront must return E
            // (the newest front insert), while Peek returns E as well since
            // E is now the oldest (head) of the logical sequence.
            uint8_t msg_d[_STK_MQ_MSG_SIZE]; memset(msg_d, 0xD0, sizeof(msg_d));
            uint8_t msg_e[_STK_MQ_MSG_SIZE]; memset(msg_e, 0xE0, sizeof(msg_e));

            ok &= g_Queue->TryPutFront(msg_d);
            ok &= g_Queue->TryPutFront(msg_e);
            ok &= (g_Queue->GetCount() == 2U);

            uint8_t pf2[_STK_MQ_MSG_SIZE] = {};
            ok &= g_Queue->TryPeekFront(pf2);
            ok &= (memcmp(pf2, msg_e, _STK_MQ_MSG_SIZE) == 0); // E is the newest front insert
            ok &= (g_Queue->GetCount() == 2U);                  // non-destructive

            while (!g_Queue->IsEmpty())
                g_Queue->TryGet(rx);

            printf("PeekFront: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Tests 14-28 – Cancelability of MessageQueue waits
// ---------------------------------------------------------------------------

// Cancellation test state (see CancelWaitTask) ------------------------------

static ITask * volatile g_Victim             = nullptr;      // task whose wait gets cancelled (task 1)
static volatile TId     g_VictimTid          = TID_NONE;     // same, as TId (for SleepCancel)
static volatile int32_t g_VictimResult[2]    = {-100, -100}; // EWaitResult of task 1 / task 2 (-100 = not set)
static volatile int64_t g_VictimElapsed[2]   = {0, 0};       // ms spent inside the blocking queue call by task 1 / task 2
static volatile int32_t g_VictimValue[2]     = {-1, -1};     // value decoded from the message received by task 1 / task 2 (-1 = nothing)
static volatile bool    g_VictimRxIntact     = false;        // receive buffer of task 1 left untouched by a non-signal result
static volatile int64_t g_VictimSleepElapsed = 0;            // ms spent in Sleep() by task 1 after its wait was cancelled

// Scenario selector of CancelWaitTask. It is a global, not a constructor argument, because RunTest()
// declares tasks 0-1 as 'static': a static local is constructed only once per template instantiation (on the
// first RunTest<CancelWaitTask> call), so a constructor argument would silently keep the value of the first
// case. Set by main() before each RunTest() call.
static volatile int32_t g_CancelParam        = 0;

// Delete-with-waiter test state (see DeleteWithWaiterTask): a dedicated queue (with internal storage) which is destroyed
// while tasks wait on it, g_Queue must stay untouched. The queue lives in raw storage which is overwritten after the
// destruction: a waiter which wrote to the destroyed queue (a message stored by a putter, a changed index or count) would
// leave a trace in it. The outcome of the waits on the destroyed queue is reported through g_VictimResult[] /
// g_VictimElapsed[] / g_VictimValue[] / g_VictimRxIntact below (index 0: task 1, index 1: task 2).
typedef stk::sync::MessageQueueT<_STK_MQ_CAPACITY, _STK_MQ_MSG_SIZE> DeleteQueueT;

alignas(DeleteQueueT) static uint8_t        g_DeleteQueueObj[sizeof(DeleteQueueT)];
static stk::sync::MessageQueue * volatile   g_DeleteQueue         = nullptr;
static const uint8_t                        DELETED_FILL          = 0xDD;
static ITask * volatile                     g_DeleteVictim2       = nullptr;      // second waiter (task 2), published right before it blocks
static volatile int32_t                     g_DeleteCheckResult   = -100;         // EWaitResult of the follow-up wait of task 1 on g_Queue (-100 = not set)
static volatile int64_t                     g_DeleteCheckElapsed  = 0;            // ms spent in that follow-up wait
static volatile bool                        g_DeleteCheckIntact   = false;        // receive buffer of the follow-up wait left untouched

// Scenario selector of DeleteWithWaiterTask, a global for the same reason as g_CancelParam
static volatile int32_t                     g_DeleteParam         = 0;

static const int32_t CW_GET_INFINITE        = 1;
static const int32_t CW_GET_NOT_TIMEOUT     = 2;
static const int32_t CW_PUT_FULL            = 3;
static const int32_t CW_NO_STALE_WAITER     = 4;
static const int32_t CW_TWO_WAITERS         = 5;
static const int32_t CW_SIGNAL_WINS         = 6;
static const int32_t CW_PUT_FRONT_FULL      = 7;
static const int32_t CW_PEEK_EMPTY          = 8;
static const int32_t CW_PEEK_FRONT_EMPTY    = 9;
static const int32_t CW_VIA_SLEEPCANCEL     = 100;

// Build / decode a message that carries an int32_t value in its first 4 bytes (the rest is a filler)
static void MakeMsg(uint8_t *msg, int32_t value)
{
    memset(msg, 0xEE, _STK_MQ_MSG_SIZE);
    memcpy(msg, &value, sizeof(value));
}

static int32_t MsgValue(const uint8_t *msg)
{
    int32_t value = 0;
    memcpy(&value, msg, sizeof(value));
    return value;
}

static bool TryPutVal(int32_t value)
{
    uint8_t msg[_STK_MQ_MSG_SIZE];
    MakeMsg(msg, value);
    return g_Queue->TryPut(msg);
}

// Cancel the wait of g_Victim either via IKernel::CancelTaskWait() or via stk::SleepCancel()
static void CancelVictim(bool use_sleep_cancel)
{
    if (use_sleep_cancel)
        stk::SleepCancel(g_VictimTid);
    else
        g_Kernel.CancelTaskWait(g_Victim);
}

// Safety net for a failed cancel test: a victim that is still blocked would keep the kernel running forever,
// so push messages through the queue in both directions to release any stuck getter, peeker or putter.
// This turns a broken cancel into a reported test failure instead of a hang.
static void ReleaseStuckWaiters()
{
    for (int32_t round = 0; round < 2; ++round)
    {
        // frees blocked getters / peekers
        for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
        {
            TryPutVal(-2);
            stk::Sleep(2);
        }

        // frees blocked putters
        for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
        {
            uint8_t tmp[_STK_MQ_MSG_SIZE];
            g_Queue->TryGet(tmp);
            stk::Sleep(2);
        }
    }
}

/*! \class CancelWaitTask
    \brief Tests cancelability of MessageQueue waits (IKernel::CancelTaskWait() and stk::SleepCancel()).
    \note  Task 0 is the canceller/checker, task 1 is the victim (blocked inside a queue call), task 2 is a
           second getter used by the two-waiters scenario. The scenario is selected by g_CancelParam:
           1 = cancel an infinite GetEx(), 2 = a cancel is reported as CANCELED, not TIMEOUT,
           3 = cancel a PutEx() on a full queue, nothing is enqueued,
           4 = cancel leaves no stale waiter behind, the next message is neither lost nor consumed twice,
           5 = cancelling one of two blocked getters leaves the other intact,
           6 = a message that arrived first wins over a late cancel,
           7 = cancel a PutFrontEx() on a full queue, nothing is enqueued and the read pointer is untouched,
           8 = cancel a PeekEx() on an empty queue, 9 = cancel a PeekFrontEx() on an empty queue.
           Adding 100 to g_CancelParam cancels via stk::SleepCancel() instead of IKernel::CancelTaskWait().
    \note  MessageQueue wraps ConditionVariable, so this suite also covers the cancel path of ConditionVariable::WaitEx().
*/
template <EAccessMode _AccessMode>
class CancelWaitTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_scenario;
    bool    m_use_sleep_cancel;

public:
    CancelWaitTask(uint8_t task_id, int32_t) // scenario is taken from g_CancelParam in Run(), see g_CancelParam
        : m_task_id(task_id), m_scenario(0), m_use_sleep_cancel(false)
    {}

private:
    void Run() override
    {
        m_scenario         = g_CancelParam % CW_VIA_SLEEPCANCEL;
        m_use_sleep_cancel = (g_CancelParam >= CW_VIA_SLEEPCANCEL);

        if (m_task_id == 0)
            RunCanceller();
        else
        if (m_task_id == 1)
            RunVictim();
        else
            RunSecondWaiter();

        ++g_InstancesDone;
    }

    // Task 1: block inside a queue call, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        const Timeout timeout = (m_scenario == CW_GET_NOT_TIMEOUT ? 300 : WAIT_INFINITE);

        uint8_t     msg[_STK_MQ_MSG_SIZE];
        uint8_t     rx[_STK_MQ_MSG_SIZE];
        uint8_t     rx_ref[_STK_MQ_MSG_SIZE];
        EWaitResult r = WAIT_RESULT_FAIL;

        MakeMsg(msg, 999);
        memset(rx, 0x5A, sizeof(rx));        // receive buffer filler: must survive a non-signal result
        memcpy(rx_ref, rx, sizeof(rx_ref));

        const int64_t start = GetTimeNowMs();

        switch (m_scenario)
        {
            case CW_PUT_FULL:         r = g_Queue->PutEx(msg, timeout);      break;
            case CW_PUT_FRONT_FULL:   r = g_Queue->PutFrontEx(msg, timeout); break;
            case CW_PEEK_EMPTY:       r = g_Queue->PeekEx(rx, timeout);      break;
            case CW_PEEK_FRONT_EMPTY: r = g_Queue->PeekFrontEx(rx, timeout); break;
            default:                  r = g_Queue->GetEx(rx, timeout);       break;
        }

        g_VictimElapsed[0] = GetTimeNowMs() - start;
        g_VictimRxIntact   = (memcmp(rx, rx_ref, sizeof(rx)) == 0);
        g_VictimValue[0]   = (g_VictimRxIntact ? -1 : MsgValue(rx));
        g_VictimResult[0]  = static_cast<int32_t>(r);

        if (m_scenario == CW_NO_STALE_WAITER)
        {
            // a stale wait entry left behind by the cancel would cut this sleep short
            const int64_t sleep_start = GetTimeNowMs();
            stk::Sleep(60);
            g_VictimSleepElapsed = GetTimeNowMs() - sleep_start;
        }
    }

    // Task 2: second getter, queued behind task 1
    void RunSecondWaiter()
    {
        if (m_scenario != CW_TWO_WAITERS)
            return;

        stk::Sleep(2 * _STK_MQ_TEST_SHORT_SLEEP); // block after task 1

        uint8_t rx[_STK_MQ_MSG_SIZE];
        memset(rx, 0x5A, sizeof(rx));

        const int64_t start = GetTimeNowMs();
        const EWaitResult r = g_Queue->GetEx(rx, WAIT_INFINITE);

        g_VictimElapsed[1] = GetTimeNowMs() - start;
        g_VictimValue[1]   = (r == WAIT_RESULT_SIGNAL ? MsgValue(rx) : -1);
        g_VictimResult[1]  = static_cast<int32_t>(r);
    }

    // Drain the queue without blocking and verify it held exactly the values 0..n-1 in FIFO order
    static bool DrainAndExpectSequence(size_t n)
    {
        bool ok = true;

        for (size_t i = 0; i < n; ++i)
        {
            uint8_t rx[_STK_MQ_MSG_SIZE];
            if (!g_Queue->TryGet(rx) || (MsgValue(rx) != (int32_t)i))
                ok = false;
        }

        return ok && g_Queue->IsEmpty();
    }

    // The queue must still work after the cancel: one message goes in and comes out intact
    static bool QueueStillWorks()
    {
        uint8_t rx[_STK_MQ_MSG_SIZE];
        return TryPutVal(5) && g_Queue->TryGet(rx) && (MsgValue(rx) == 5) && g_Queue->IsEmpty();
    }

    // Task 0: cancel and verify
    void RunCanceller()
    {
        bool pass = false;

        // putter scenarios: fill the queue before the victim blocks (victim starts at ~10 ms)
        if ((m_scenario == CW_PUT_FULL) || (m_scenario == CW_PUT_FRONT_FULL))
        {
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
                TryPutVal((int32_t)i);
        }

        stk::Sleep(60); // task 1 (and task 2) are blocked by now

        if (m_scenario == CW_GET_INFINITE)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            // started waiting at ~10 ms, cancelled at ~60 ms; nothing was received
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   g_VictimRxIntact && g_Queue->IsEmpty();
        }
        else
        if (m_scenario == CW_GET_NOT_TIMEOUT)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            // 300 ms timeout is far away: the wait must end early and as CANCELED, not TIMEOUT
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && (g_VictimElapsed[0] < 150);
        }
        else
        if ((m_scenario == CW_PUT_FULL) || (m_scenario == CW_PUT_FRONT_FULL))
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            const bool full = g_Queue->IsFull();

            // the cancelled Put/PutFront must not have stored its message (999) nor moved the read
            // pointer: original FIFO content 0..7 intact and in order
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   full && DrainAndExpectSequence(_STK_MQ_CAPACITY) && QueueStillWorks();
        }
        else
        if (m_scenario == CW_NO_STALE_WAITER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP); // victim returns and starts its Sleep(60)

            const bool wrote = TryPutVal(321); // nobody waits: message must stay in the queue

            uint8_t rx1[_STK_MQ_MSG_SIZE] = {};
            uint8_t rx2[_STK_MQ_MSG_SIZE] = {};
            const bool poll1 = g_Queue->TryGet(rx1); // gets the message
            const bool poll2 = g_Queue->TryGet(rx2); // nothing left

            stk::Sleep(70); // let the victim finish its sleep

            printf("cancel no stale waiter: result=%d wrote=%d poll1=%d(v=%d) poll2=%d victim_sleep=%d ms\n",
                (int)g_VictimResult[0], (int)wrote, (int)poll1, (int)MsgValue(rx1), (int)poll2, (int)g_VictimSleepElapsed);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   wrote && poll1 && (MsgValue(rx1) == 321) && !poll2 &&
                   (g_VictimSleepElapsed >= 55); // not woken early by a ghost wait entry
        }
        else
        if (m_scenario == CW_TWO_WAITERS)
        {
            CancelVictim(m_use_sleep_cancel);              // cancel the first getter only
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

            const bool wrote = TryPutVal(77);              // must wake the second getter
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            printf("cancel two waiters: first=%d second=%d wrote=%d value=%d\n",
                (int)g_VictimResult[0], (int)g_VictimResult[1], (int)wrote, (int)g_VictimValue[1]);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && g_VictimRxIntact &&
                   (g_VictimResult[1] == WAIT_RESULT_SIGNAL)   && (g_VictimValue[1] == 77) &&
                   wrote && g_Queue->IsEmpty(); // the second getter consumed the message
        }
        else
        if (m_scenario == CW_SIGNAL_WINS)
        {
            // back to back, no scheduling point in between: the getter is already signaled and removed
            // from the wait list when the cancel arrives, so the cancel must be a no-op
            const bool wrote = TryPutVal(777);
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            pass = wrote && (g_VictimResult[0] == WAIT_RESULT_SIGNAL) &&
                   (g_VictimValue[0] == 777) && g_Queue->IsEmpty();
        }
        else
        if ((m_scenario == CW_PEEK_EMPTY) || (m_scenario == CW_PEEK_FRONT_EMPTY))
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 2);

            printf("cancel peek: result=%d elapsed=%d intact=%d\n",
                (int)g_VictimResult[0], (int)g_VictimElapsed[0], (int)g_VictimRxIntact);

            // nothing copied; the cancelled peeker leaves the queue fully usable
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   g_VictimRxIntact && g_Queue->IsEmpty() && QueueStillWorks();
        }

        // Safety net: if the cancel did not work a task is still blocked forever and the kernel would
        // never stop, release any such task so that the test fails instead of hanging.
        if (!pass)
            ReleaseStuckWaiters();

        printf("cancel wait (scenario=%d, via %s): result[0]=%d elapsed[0]=%d ms\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
            (int)g_VictimResult[0], (int)g_VictimElapsed[0]);

        if (pass)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------
// Tests 29-35 – Destruction of a queue which tasks are blocked on (WAIT_RESULT_DELETED)
// ---------------------------------------------------------------------------

static const int32_t DW_GET         = 1; // a getter waits on an empty queue: GetEx()
static const int32_t DW_PUT         = 2; // a putter waits on a full queue: PutEx()
static const int32_t DW_PUT_FRONT   = 3; // a putter waits on a full queue: PutFrontEx()
static const int32_t DW_PEEK        = 4; // a peeker waits on an empty queue: PeekEx()
static const int32_t DW_PEEK_FRONT  = 5; // a peeker waits on an empty queue: PeekFrontEx()
static const int32_t DW_GET_BOOL    = 6; // a getter in the boolean Get(): the destruction is reported as false
static const int32_t DW_TWO_GETTERS = 7; // two getters wait on an empty queue, both must be released

/*! \class DeleteWithWaiterTask
    \brief Tests destruction of a MessageQueue which tasks are blocked on (WAIT_RESULT_DELETED).
    \note  Task 0 creates a dedicated queue and destroys it while task 1 (and task 2 in the two-getters scenario) wait on it
           with an infinite timeout: a getter or peeker on an empty queue, or a putter on a full one (task 0 fills it
           first). Nobody puts or gets, so only the destruction can end the wait: the waiter must get WAIT_RESULT_DELETED
           (neither SIGNAL, a timeout nor a cancellation) without touching the destroyed queue: no message is copied
           into the receive buffer of a getter or peeker (the buffer is pre-filled and must stay intact), and a putter must
           not store its message or touch an index or the count, which is checked on the storage of the queue (internal to
           the object), overwritten after the destruction, so it must still hold that pattern when the waiters are back.
           The internal condition variables are separate for consumers (queue empty) and producers (queue full); only one
           kind of waiter can exist at a time. Afterwards the kernel and g_Queue must be fully usable: the victim repeats the
           same call with a 20 tick timeout on g_Queue, which must run to its deadline and return WAIT_RESULT_TIMEOUT
           (a stale wait entry would end it early), without copying anything; g_Queue itself must be unchanged (empty for the
           reading scenarios, still holding 0..7 in order for the putting ones). The scenario is selected by g_DeleteParam
           (DW_*). In DW_GET_BOOL the victim uses the boolean Get(), which reports the destruction as false: an infinite wait
           can not time out, so false after ~50 ms identifies it. Task 2 is added by RunTest() but stays idle unless the
           scenario is DW_TWO_GETTERS.
*/
template <EAccessMode _AccessMode>
class DeleteWithWaiterTask : public Task<_STK_MQ_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_scenario;

public:
    DeleteWithWaiterTask(uint8_t task_id, int32_t) // scenario is taken from g_DeleteParam in Run(), see g_DeleteParam
        : m_task_id(task_id), m_scenario(0)
    {}

private:
    void Run() override
    {
        m_scenario = g_DeleteParam;

        if (m_task_id == 0)
            RunDestroyer();
        else
        if (m_task_id == 1)
            RunVictim();
        else
            RunSecondWaiter();

        ++g_InstancesDone;
    }

    // True if the victim waits for a free slot (the queue is full), false if it waits for a message (the queue is empty)
    bool IsPutter() const
    {
        return (m_scenario == DW_PUT) || (m_scenario == DW_PUT_FRONT);
    }

    // Task 1: block on the dedicated queue, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        stk::sync::MessageQueue *const q = g_DeleteQueue; // created by task 0 before it went to sleep

        uint8_t msg[_STK_MQ_MSG_SIZE];
        uint8_t rx[_STK_MQ_MSG_SIZE];
        uint8_t rx_ref[_STK_MQ_MSG_SIZE];

        MakeMsg(msg, 999);
        memset(rx, 0x5A, sizeof(rx));        // receive buffer filler: must survive the destruction
        memcpy(rx_ref, rx, sizeof(rx_ref));

        int32_t result = static_cast<int32_t>(WAIT_RESULT_FAIL);

        const int64_t start = GetTimeNowMs();

        // blocks until the queue is destroyed; nothing but the result is read afterwards
        if (q != nullptr)
        {
            switch (m_scenario)
            {
                case DW_PUT:        result = static_cast<int32_t>(q->PutEx(msg, WAIT_INFINITE));       break;
                case DW_PUT_FRONT:  result = static_cast<int32_t>(q->PutFrontEx(msg, WAIT_INFINITE));  break;
                case DW_PEEK:       result = static_cast<int32_t>(q->PeekEx(rx, WAIT_INFINITE));      break;
                case DW_PEEK_FRONT: result = static_cast<int32_t>(q->PeekFrontEx(rx, WAIT_INFINITE)); break;
                case DW_GET_BOOL:   result = (q->Get(rx, WAIT_INFINITE) ? 1 : 0);                      break; // 1 = received / 0 = not
                default:            result = static_cast<int32_t>(q->GetEx(rx, WAIT_INFINITE));       break;
            }
        }

        g_VictimElapsed[0] = GetTimeNowMs() - start;
        g_VictimRxIntact   = (memcmp(rx, rx_ref, sizeof(rx)) == 0);
        g_VictimValue[0]   = (g_VictimRxIntact ? -1 : MsgValue(rx));
        g_VictimResult[0]  = result;

        // the kernel is in a sane state: the same call with a short timeout on another queue works, runs to its
        // deadline and copies nothing (g_Queue is empty for the reading scenarios and full for the putting ones)
        uint8_t rx2[_STK_MQ_MSG_SIZE];
        uint8_t rx2_ref[_STK_MQ_MSG_SIZE];
        memset(rx2, 0x5A, sizeof(rx2));
        memcpy(rx2_ref, rx2, sizeof(rx2_ref));

        EWaitResult r2;

        const int64_t start2 = GetTimeNowMs();

        switch (m_scenario)
        {
            case DW_PUT:        r2 = g_Queue->PutEx(msg, 20);       break;
            case DW_PUT_FRONT:  r2 = g_Queue->PutFrontEx(msg, 20);  break;
            case DW_PEEK:       r2 = g_Queue->PeekEx(rx2, 20);      break;
            case DW_PEEK_FRONT: r2 = g_Queue->PeekFrontEx(rx2, 20); break;
            default:            r2 = g_Queue->GetEx(rx2, 20);       break;
        }

        g_DeleteCheckElapsed = GetTimeNowMs() - start2;
        g_DeleteCheckIntact  = (memcmp(rx2, rx2_ref, sizeof(rx2)) == 0);
        g_DeleteCheckResult  = static_cast<int32_t>(r2);
    }

    // Task 2: second getter, queued behind task 1
    void RunSecondWaiter()
    {
        if (m_scenario != DW_TWO_GETTERS)
            return;

        stk::Sleep(2 * _STK_MQ_TEST_SHORT_SLEEP); // block after task 1

        g_DeleteVictim2 = static_cast<ITask *>(this);

        stk::sync::MessageQueue *const q = g_DeleteQueue;

        uint8_t rx[_STK_MQ_MSG_SIZE];
        uint8_t rx_ref[_STK_MQ_MSG_SIZE];
        memset(rx, 0x5A, sizeof(rx));
        memcpy(rx_ref, rx, sizeof(rx_ref));

        const int64_t start = GetTimeNowMs();
        const EWaitResult r = ((q != nullptr) ? q->GetEx(rx, WAIT_INFINITE) : WAIT_RESULT_FAIL);

        g_VictimElapsed[1] = GetTimeNowMs() - start;
        g_VictimValue[1]   = ((memcmp(rx, rx_ref, sizeof(rx)) == 0) ? -1 : MsgValue(rx)); // -1 = nothing was copied
        g_VictimResult[1]  = static_cast<int32_t>(r);
    }

    // True if the storage of the destroyed queue still holds the pattern written after the destruction
    static bool StorageUntouched()
    {
        for (size_t i = 0; i < sizeof(g_DeleteQueueObj); ++i)
        {
            if (g_DeleteQueueObj[i] != DELETED_FILL)
                return false;
        }

        return true;
    }

    // Drain g_Queue without blocking and verify it held exactly the values 0..n-1 in FIFO order
    static bool DrainAndExpectSequence(size_t n)
    {
        bool ok = true;

        for (size_t i = 0; i < n; ++i)
        {
            uint8_t rx[_STK_MQ_MSG_SIZE];
            if (!g_Queue->TryGet(rx) || (MsgValue(rx) != (int32_t)i))
                ok = false;
        }

        return ok && g_Queue->IsEmpty();
    }

    // Task 0: create, fill if needed, destroy and verify
    void RunDestroyer()
    {
        DeleteQueueT *const q = ::new (g_DeleteQueueObj) DeleteQueueT();
        g_DeleteQueue = q;

        // putters wait only on a full queue: fill the dedicated queue before the victim blocks (at ~10 ms)
        bool prepared = q->IsStorageValid();
        if (IsPutter())
        {
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
            {
                uint8_t m[_STK_MQ_MSG_SIZE];
                MakeMsg(m, (int32_t)i);
                prepared &= q->TryPut(m);
            }
            prepared &= q->IsFull();
        }
        else
        {
            prepared &= q->IsEmpty();
        }

        stk::Sleep(60); // task 1 (and task 2) block on it in the meantime

        // destroy the queue which is waited on: the waiters are released by the destructors of its condition variables
        q->~DeleteQueueT();
        memset(g_DeleteQueueObj, DELETED_FILL, sizeof(g_DeleteQueueObj));
        g_DeleteQueue = nullptr;

        // putters repeat the call on g_Queue: make it full so that the follow-up put has to wait for its deadline
        if (IsPutter())
        {
            for (size_t i = 0; i < _STK_MQ_CAPACITY; ++i)
                TryPutVal((int32_t)i);
        }

        stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 4); // let the waiters resume and task 1 finish its check on g_Queue

        // safety net: if a waiter was not released it would block forever and the kernel would never stop,
        // cancel it so that the test fails instead of hanging
        const bool released = (g_VictimResult[0] != -100) &&
                              ((m_scenario != DW_TWO_GETTERS) || (g_VictimResult[1] != -100));
        if (!released)
        {
            ITask *const victim1 = g_Victim;
            ITask *const victim2 = g_DeleteVictim2;

            if (victim1 != nullptr)
                g_Kernel.CancelTaskWait(victim1);
            if (victim2 != nullptr)
                g_Kernel.CancelTaskWait(victim2);

            stk::Sleep(_STK_MQ_TEST_SHORT_SLEEP * 5);
        }

        const bool untouched = StorageUntouched();

        // g_Queue must be unchanged: still empty (reading scenarios) or still holding 0..7 in order (putting ones)
        const bool queue_ok = (IsPutter() ? DrainAndExpectSequence(_STK_MQ_CAPACITY) : g_Queue->IsEmpty());

        printf("delete with waiter (scenario=%d): first=%d elapsed=%d ms intact=%d, second=%d elapsed=%d ms value=%d, "
            "g_Queue wait: result=%d elapsed=%d ms intact=%d, queue_ok=%d storage_untouched=%d\n",
            (int)m_scenario,
            (int)g_VictimResult[0], (int)g_VictimElapsed[0], (int)g_VictimRxIntact,
            (int)g_VictimResult[1], (int)g_VictimElapsed[1], (int)g_VictimValue[1],
            (int)g_DeleteCheckResult, (int)g_DeleteCheckElapsed, (int)g_DeleteCheckIntact,
            (int)queue_ok, (int)untouched);

        // task 1 started waiting at ~10 ms, task 2 at ~20 ms, the queue was destroyed at ~60 ms
        bool pass = prepared && released && (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75);

        if (m_scenario == DW_GET_BOOL)
        {
            // false, and an infinite wait can not time out: it was the destruction that ended it
            pass = pass && (g_VictimResult[0] == 0);
        }
        else
        {
            pass = pass && (g_VictimResult[0] == WAIT_RESULT_DELETED);
        }

        // a getter or peeker copies nothing into its receive buffer
        if (!IsPutter())
            pass = pass && g_VictimRxIntact;

        if (m_scenario == DW_TWO_GETTERS)
        {
            pass = pass && (g_VictimResult[1] == WAIT_RESULT_DELETED) && (g_VictimValue[1] == -1) &&
                   (g_VictimElapsed[1] >= 25) && (g_VictimElapsed[1] <= 65);
        }

        // the other queue was not affected: the 20 ms attempt on it ran to its deadline (a stale wait entry would cut
        // it short) and copied nothing, and it holds what it held before; the destroyed queue was not written to
        pass = pass &&
               (g_DeleteCheckResult == WAIT_RESULT_TIMEOUT) && (g_DeleteCheckElapsed >= 15) && g_DeleteCheckIntact &&
               queue_ok && untouched;

        if (pass)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------
// Helper – reset shared state between tests
// ---------------------------------------------------------------------------

static void ResetTestState()
{
    g_TestResult    = 0;
    g_InstancesDone = 0;
    g_SharedCounter = 0;

    g_Victim             = nullptr;
    g_VictimTid          = TID_NONE;
    g_VictimRxIntact     = false;
    g_VictimSleepElapsed = 0;

    g_DeleteQueue        = nullptr;
    g_DeleteVictim2      = nullptr;
    g_DeleteCheckResult  = -100;
    g_DeleteCheckElapsed = 0;
    g_DeleteCheckIntact  = false;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]  = -100;
        g_VictimElapsed[i] = 0;
        g_VictimValue[i]   = -1;
    }
}

} // namespace msgqueue
} // namespace test
} // namespace stk

// ---------------------------------------------------------------------------
// Task-count predicates
// ---------------------------------------------------------------------------

/*! \fn    NeedsOnlyOneTask
    \brief Returns true if the test is entirely single-task (task 0 only).
    \note  TryPutGet, FillDrain, Accessors and WrapAround perform all work
           inside task 0 and do not interact with any other task.
*/
static bool NeedsOnlyOneTask(const char *test_name)
{
    return (strcmp(test_name, "TryPutGet")           == 0) ||
           (strcmp(test_name, "FillDrain")           == 0) ||
           (strcmp(test_name, "Accessors")           == 0) ||
           (strcmp(test_name, "WrapAround")          == 0) ||
           (strcmp(test_name, "TryPutFront")         == 0) ||
           (strcmp(test_name, "PutFrontWrapAround")  == 0) ||
           (strcmp(test_name, "Peek")                == 0) ||
           (strcmp(test_name, "PeekFront")           == 0);
}

/*! \fn    NeedsThreeTasks
    \brief Returns true if the test uses tasks 0-2 (the cancellation and delete-with-waiter tests: canceller or
           destroyer, victim, second waiter).
*/
static bool NeedsThreeTasks(const char *test_name)
{
    return (strncmp(test_name, "Cancel", 6) == 0) ||
           (strncmp(test_name, "DeleteWithWaiter", 16) == 0);
}

/*! \fn    NeedsAllTasks
    \brief Returns true if the test requires all five tasks (0-4).
    \note  Only the Stress test references task_id == (_STK_MQ_TEST_TASKS_MAX - 1)
           and waits for g_InstancesDone == _STK_MQ_TEST_TASKS_MAX.
*/
static bool NeedsAllTasks(const char *test_name)
{
    return (strcmp(test_name, "Stress") == 0);
}

// ---------------------------------------------------------------------------
// RunTest helper
// ---------------------------------------------------------------------------

template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::msgqueue;

    printf("Test: %s\n", test_name);

    ResetTestState();

    // Recreate the shared queue fresh for every test using MessageQueueT
    stk::sync::MessageQueueT<_STK_MQ_CAPACITY, _STK_MQ_MSG_SIZE> queue;
    g_Queue = &queue;

    // Tasks are always constructed (their type may need it), but only
    // registered with the kernel up to the count actually required.
    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_Kernel.AddTask(&task0);

    if (!NeedsOnlyOneTask(test_name))
        g_Kernel.AddTask(&task1);

    if (NeedsThreeTasks(test_name))
        g_Kernel.AddTask(&task2);

    if (NeedsAllTasks(test_name))
    {
        g_Kernel.AddTask(&task2);
        g_Kernel.AddTask(&task3);
        g_Kernel.AddTask(&task4);
    }

    g_Kernel.Start();

    g_Queue = nullptr;

    int32_t result = (g_TestResult
        ? TestContext::SUCCESS_EXIT_CODE
        : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("Result: %s\n", result == TestContext::SUCCESS_EXIT_CODE ? "PASS" : "FAIL");
    printf("--------------\n");

    return result;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/*! \fn    main
    \brief Entry point for the sync::MessageQueue / MessageQueueT test suite.
*/
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    stk::test::msgqueue::g_Kernel.Initialize();

#define RUN(TestClass, name, param) \
    do { \
        if (RunTest<TestClass<ACCESS_PRIVILEGED>>(name, param) \
                != TestContext::SUCCESS_EXIT_CODE) \
            total_failures++; \
        else \
            total_success++; \
    } while (0)

#ifndef __ARM_ARCH_6M__

    // Test 1: TryPut / TryGet basic cycle with accounting checks
    RUN(stk::test::msgqueue::TryPutGetTask,             "TryPutGet",        0);

    // Test 2: Fill to capacity, verify IsFull, drain in FIFO order
    RUN(stk::test::msgqueue::FillDrainTask,             "FillDrain",        0);

    // Test 3: Blocking Get unblocked by Put from another task
    RUN(stk::test::msgqueue::BlockingGetTask,           "BlockingGet",      0);

    // Test 4: Blocking Put unblocked by Get from another task
    RUN(stk::test::msgqueue::BlockingPutTask,           "BlockingPut",      0);

    // Test 5: Timed Get expires when queue stays empty
    RUN(stk::test::msgqueue::TimedGetTimeoutTask,       "TimedGetTimeout",  0);

    // Test 6: Timed Get succeeds when message arrives within timeout
    RUN(stk::test::msgqueue::TimedGetSuccessTask,       "TimedGetSuccess",  0);

    // Test 7: Timed Put expires when queue stays full
    RUN(stk::test::msgqueue::TimedPutTimeoutTask,       "TimedPutTimeout",  0);

    // Test 8: Reset drains queue and wakes blocked producers
    RUN(stk::test::msgqueue::ResetTask,                 "Reset",            0);

    // Test 9: Accessor correctness (GetCapacity, GetMsgSize, GetBuffer, etc.)
    RUN(stk::test::msgqueue::AccessorsTask,             "Accessors",        0);

    // Test 10: Ring-buffer wrap-around preserves FIFO payload integrity
    RUN(stk::test::msgqueue::WrapAroundTask,            "WrapAround",            0);

    // Test 10a: TryPutFront basic ordering and TryPutFront-on-full returns false
    RUN(stk::test::msgqueue::TryPutFrontTask,           "TryPutFront",           0);

    // Test 10b: PutFront tail wrap-around across the ring-buffer boundary
    RUN(stk::test::msgqueue::PutFrontWrapAroundTask,    "PutFrontWrapAround",    0);

    // Test 10c: Blocking PutFront woken by Get when queue is full
    RUN(stk::test::msgqueue::BlockingPutFrontTask,      "BlockingPutFront",      0);

    // Test 10d: Timed PutFront expires when queue remains full
    RUN(stk::test::msgqueue::TimedPutFrontTimeoutTask,  "TimedPutFrontTimeout",  0);

    // Test 13a: Peek inspects oldest message without consuming it
    RUN(stk::test::msgqueue::PeekTask,                  "Peek",                  0);

    // Test 13b: PeekFront inspects front-inserted message without consuming it
    RUN(stk::test::msgqueue::PeekFrontTask,             "PeekFront",             0);

    // Test 11: Single-producer / single-consumer ping-pong (30 iterations)
    RUN(stk::test::msgqueue::PingPongTask,              "PingPong",              30);

    // Tests 14-28: cancelability of MessageQueue waits (tasks 0-2 only), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        using namespace stk::test::msgqueue;

        struct CancelCase { const char *name; int32_t param; };

        static const CancelCase cases[] =
        {
            { "Cancel_GetInfinite",            CW_GET_INFINITE },
            { "Cancel_GetNotTimeout",          CW_GET_NOT_TIMEOUT },
            { "Cancel_PutFull",                CW_PUT_FULL },
            { "Cancel_GetNoStaleWaiter",       CW_NO_STALE_WAITER },
            { "Cancel_GetTwoWaiters",          CW_TWO_WAITERS },
            { "Cancel_GetSignalWins",          CW_SIGNAL_WINS },
            { "Cancel_PutFrontFull",           CW_PUT_FRONT_FULL },
            { "Cancel_PeekEmpty",              CW_PEEK_EMPTY },
            { "Cancel_PeekFrontEmpty",         CW_PEEK_FRONT_EMPTY },
            { "Cancel_SleepCancelGet",         CW_VIA_SLEEPCANCEL + CW_GET_INFINITE },
            { "Cancel_SleepCancelPut",         CW_VIA_SLEEPCANCEL + CW_PUT_FULL },
            { "Cancel_SleepCancelNoStale",     CW_VIA_SLEEPCANCEL + CW_NO_STALE_WAITER },
            { "Cancel_SleepCancelTwo",         CW_VIA_SLEEPCANCEL + CW_TWO_WAITERS },
            { "Cancel_SleepCancelSignal",      CW_VIA_SLEEPCANCEL + CW_SIGNAL_WINS },
            { "Cancel_SleepCancelPutFront",    CW_VIA_SLEEPCANCEL + CW_PUT_FRONT_FULL }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_CancelParam = cases[i].param;

            if (RunTest<CancelWaitTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param)
                    != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

    // Tests 29-35: queue destroyed while tasks wait on it (WAIT_RESULT_DELETED, tasks 0-2 only)
    {
        using namespace stk::test::msgqueue;

        struct DeleteCase { const char *name; int32_t param; };

        static const DeleteCase cases[] =
        {
            { "DeleteWithWaiter_Get",        DW_GET         },
            { "DeleteWithWaiter_Put",        DW_PUT         },
            { "DeleteWithWaiter_PutFront",   DW_PUT_FRONT   },
            { "DeleteWithWaiter_Peek",       DW_PEEK        },
            { "DeleteWithWaiter_PeekFront",  DW_PEEK_FRONT  },
            { "DeleteWithWaiter_GetBool",    DW_GET_BOOL    },
            { "DeleteWithWaiter_TwoGetters", DW_TWO_GETTERS }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_DeleteParam = cases[i].param;

            if (RunTest<DeleteWithWaiterTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param)
                    != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

#endif // __ARM_ARCH_6M__

    // Test 12: Stress – all tasks produce and consume concurrently (ARM-M0 compatible)
    RUN(stk::test::msgqueue::StressTask,           "Stress",           200);

#undef RUN

    int32_t final_result = (total_failures == 0
        ? TestContext::SUCCESS_EXIT_CODE
        : TestContext::DEFAULT_FAILURE_EXIT_CODE);

    printf("##############\n");
    printf("Total tests: %d\n", total_failures + total_success);
    printf("Failures: %d\n", (int)total_failures);

    TestContext::ShowTestSuiteEpilogue(final_result);
    return final_result;
}
