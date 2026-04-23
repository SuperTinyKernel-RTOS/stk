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
    _STK_MQ_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

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
// Helper – reset shared state between tests
// ---------------------------------------------------------------------------

static void ResetTestState()
{
    g_TestResult    = 0;
    g_InstancesDone = 0;
    g_SharedCounter = 0;
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
    RUN(stk::test::msgqueue::TryPutGetTask,        "TryPutGet",        0);

    // Test 2: Fill to capacity, verify IsFull, drain in FIFO order
    RUN(stk::test::msgqueue::FillDrainTask,        "FillDrain",        0);

    // Test 3: Blocking Get unblocked by Put from another task
    RUN(stk::test::msgqueue::BlockingGetTask,      "BlockingGet",      0);

    // Test 4: Blocking Put unblocked by Get from another task
    RUN(stk::test::msgqueue::BlockingPutTask,      "BlockingPut",      0);

    // Test 5: Timed Get expires when queue stays empty
    RUN(stk::test::msgqueue::TimedGetTimeoutTask,  "TimedGetTimeout",  0);

    // Test 6: Timed Get succeeds when message arrives within timeout
    RUN(stk::test::msgqueue::TimedGetSuccessTask,  "TimedGetSuccess",  0);

    // Test 7: Timed Put expires when queue stays full
    RUN(stk::test::msgqueue::TimedPutTimeoutTask,  "TimedPutTimeout",  0);

    // Test 8: Reset drains queue and wakes blocked producers
    RUN(stk::test::msgqueue::ResetTask,            "Reset",            0);

    // Test 9: Accessor correctness (GetCapacity, GetMsgSize, GetBuffer, etc.)
    RUN(stk::test::msgqueue::AccessorsTask,        "Accessors",        0);

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
    RUN(stk::test::msgqueue::PingPongTask,         "PingPong",         30);

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
