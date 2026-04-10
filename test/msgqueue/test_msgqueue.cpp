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
    return (strcmp(test_name, "TryPutGet")  == 0) ||
           (strcmp(test_name, "FillDrain")  == 0) ||
           (strcmp(test_name, "Accessors")  == 0) ||
           (strcmp(test_name, "WrapAround") == 0);
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
    RUN(stk::test::msgqueue::WrapAroundTask,       "WrapAround",       0);

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
