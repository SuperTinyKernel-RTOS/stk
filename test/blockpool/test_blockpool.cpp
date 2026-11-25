/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <assert.h>
#include <string.h>

#include <stk_config.h>
#include <stk.h>
#include <memory/stk_memory_blockpool.h>
#include <memory/stk_memory_allocator_malloc.h> // use malloc-based allocator

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_POOL_TEST_TASKS_MAX   5
#define _STK_POOL_TEST_TIMEOUT     1000
#define _STK_POOL_TEST_SHORT_SLEEP 10
#define _STK_POOL_TEST_LONG_SLEEP  100
#define _STK_POOL_BLOCK_SIZE       32U
#define _STK_POOL_CAPACITY         8U
#define _STK_POOL_DELETE_CAPACITY  4U // capacity of the pool destroyed by the delete-with-waiter tests
#ifdef __ARM_ARCH_6M__
#define _STK_POOL_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_POOL_STACK_SIZE       256
#define STK_TASK                   static
#endif

#ifndef _NEW
inline void *operator new(std::size_t, void *ptr) noexcept { return ptr; }
inline void operator delete(void *, void *) noexcept { /* nothing for placement delete */ }
#endif

namespace stk {
namespace test {

/*! \namespace stk::test::blockpool
    \brief     Namespace of BlockMemoryPool test.
 */
namespace blockpool {

// Test results storage
static volatile int32_t g_TestResult   = 0;
static volatile int32_t g_InstancesDone = 0;
static volatile int32_t g_SharedCounter = 0;

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_POOL_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

// Shared pool + external-storage buffer used by most tests
alignas(stk::memory::BlockMemoryPool::ALIGN_DEFAULT) static uint8_t
    g_PoolStorage[_STK_POOL_CAPACITY * stk::memory::BlockMemoryPool::AlignBlockSize(_STK_POOL_BLOCK_SIZE)];

static stk::memory::BlockMemoryPool *g_Pool = nullptr;

// Cancellation test state (see CancelAllocTask)
static ITask * volatile g_Victim                 = nullptr;      // task whose wait gets cancelled (task 1)
static volatile TId     g_VictimTid              = TID_NONE;     // same, as TId (for SleepCancel)
static volatile int32_t g_VictimResult[2]        = {-100, -100}; // EWaitResult of task 1 / task 2 (-100 = not set)
static volatile bool    g_VictimGotBlock[2]      = {false, false}; // true if task 1 / task 2 received a block
static volatile int64_t g_VictimElapsed[2]       = {0, 0};       // ms spent in the allocation call by task 1 / task 2
static volatile int64_t g_VictimSleepElapsed     = 0;            // ms spent in Sleep() by task 1 after its wait ended

// Scenario selector of CancelAllocTask. It is a global, not a constructor argument, because RunTest() declares
// tasks 0-1 as 'static': a static local is constructed only once per template instantiation (on the first
// RunTest<CancelAllocTask> call), so a constructor argument would silently keep the value of the first case.
// Set by main() before each RunTest() call.
static volatile int32_t g_CancelParam            = 0;

// Delete-with-waiter test state (see DeleteWithWaiterTask): a dedicated pool, with its own external block buffer, which is
// destroyed while tasks wait for a block, g_Pool must stay untouched. The pool object lives in raw storage which is
// overwritten after the destruction: a waiter which wrote to the destroyed pool would leave a trace in it. The outcome of
// the waits on the destroyed pool is reported through g_VictimResult[] / g_VictimGotBlock[] / g_VictimElapsed[] above
// (index 0: task 1, index 1: task 2).
alignas(stk::memory::BlockMemoryPool) static uint8_t g_DeletePoolObj[sizeof(stk::memory::BlockMemoryPool)];
alignas(stk::memory::BlockMemoryPool::ALIGN_DEFAULT) static uint8_t
    g_DeleteBlocks[_STK_POOL_DELETE_CAPACITY * stk::memory::BlockMemoryPool::AlignBlockSize(_STK_POOL_BLOCK_SIZE)];
static stk::memory::BlockMemoryPool * volatile g_DeletePool = nullptr;
static const uint8_t                    DELETED_FILL         = 0xDD;
static ITask * volatile                 g_DeleteVictim2      = nullptr;      // second waiter (task 2), published right before it blocks
static volatile int32_t                 g_DeleteCheckResult  = -100;         // EWaitResult of the follow-up wait of task 1 on g_Pool (-100 = not set)
static volatile bool                    g_DeleteCheckGotBlock = false;       // true if that follow-up wait returned a block (not expected)
static volatile int64_t                 g_DeleteCheckElapsed = 0;            // ms spent in that follow-up wait

// Scenario selector of DeleteWithWaiterTask, a global for the same reason as g_CancelParam
static volatile int32_t                 g_DeleteParam        = 0;

// Cancel the wait of g_Victim either via IKernel::CancelTaskWait() or via stk::SleepCancel()
static void CancelVictim(bool use_sleep_cancel)
{
    if (use_sleep_cancel)
        stk::SleepCancel(g_VictimTid);
    else
        g_Kernel.CancelTaskWait(g_Victim);
}

// ---------------------------------------------------------------------------
// Test 1 – TryAlloc / Free basic cycle (single task)
// ---------------------------------------------------------------------------

/*! \class TryAllocFreeTask
    \brief Verifies TryAlloc returns a valid block, Free recycles it, and
           pool accounting stays consistent throughout.
*/
template <EAccessMode _AccessMode>
class TryAllocFreeTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TryAllocFreeTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // Pool should start fully empty (no outstanding allocations)
            ok &= g_Pool->IsEmpty();
            ok &= (g_Pool->GetFreeCount() == _STK_POOL_CAPACITY);
            ok &= (g_Pool->GetUsedCount() == 0U);

            // Allocate one block non-blocking
            void *blk = g_Pool->TryAlloc();
            ok &= (blk != nullptr);
            ok &= (g_Pool->GetUsedCount() == 1U);
            ok &= (g_Pool->GetFreeCount() == (_STK_POOL_CAPACITY - 1U));

            // Write to the block to verify it is usable memory
            memset(blk, 0xAB, _STK_POOL_BLOCK_SIZE);

            // Return the block
            ok &= g_Pool->Free(blk);
            ok &= g_Pool->IsEmpty();
            ok &= (g_Pool->GetUsedCount() == 0U);

            // TryAlloc on empty-after-full-drain should still succeed
            void *blk2 = g_Pool->TryAlloc();
            ok &= (blk2 != nullptr);
            g_Pool->Free(blk2);

            printf("TryAllocFree: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 2 – Exhaust pool with TryAlloc then verify IsFull / free all
// ---------------------------------------------------------------------------

/*! \class ExhaustPoolTask
    \brief Drains the entire pool via TryAlloc, verifies IsFull() and that
           a further TryAlloc returns nullptr, then frees every block.
*/
template <EAccessMode _AccessMode>
class ExhaustPoolTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ExhaustPoolTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;
            void *blocks[_STK_POOL_CAPACITY] = {};

            // Drain every block
            for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
            {
                blocks[i] = g_Pool->TryAlloc();
                ok &= (blocks[i] != nullptr);
            }

            ok &= g_Pool->IsFull();
            ok &= (g_Pool->GetFreeCount() == 0U);
            ok &= (g_Pool->GetUsedCount() == _STK_POOL_CAPACITY);

            // One more TryAlloc must fail without blocking
            void *extra = g_Pool->TryAlloc();
            ok &= (extra == nullptr);

            // Return all blocks
            for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
                ok &= g_Pool->Free(blocks[i]);

            ok &= g_Pool->IsEmpty();

            printf("ExhaustPool: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 3 – Blocking Alloc: producer wakes a blocked consumer
// ---------------------------------------------------------------------------

/*! \class BlockingAllocTask
    \brief Task 0 holds all blocks; Task 1 blocks in Alloc(); Task 0 frees
           one block and verifies Task 1 unblocks and completes successfully.
*/
template <EAccessMode _AccessMode>
class BlockingAllocTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BlockingAllocTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Drain every block to force task 1 into blocking Alloc()
            void *blocks[_STK_POOL_CAPACITY] = {};
            for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
                blocks[i] = g_Pool->TryAlloc();

            // Give task 1 time to enter Alloc() and block
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // Free one block — this must wake task 1
            g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;

            // Wait for task 1 to finish then free the remaining blocks
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            for (size_t i = 1; i < _STK_POOL_CAPACITY; ++i)
                g_Pool->Free(blocks[i]);
        }
        else
        if (m_task_id == 1)
        {
            // Pool is full; Alloc() must block until task 0 frees a block
            void *blk = g_Pool->Alloc(); // blocking

            if (blk != nullptr)
            {
                g_SharedCounter = 1; // signal success
                g_Pool->Free(blk);
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            bool ok = (g_SharedCounter == 1) && g_Pool->IsEmpty();
            printf("BlockingAlloc: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 4 – TimedAlloc timeout: expires when no block becomes available
// ---------------------------------------------------------------------------

/*! \class TimedAllocTimeoutTask
    \brief Task 0 holds all blocks; Task 1 calls TimedAlloc with a short
           timeout that must expire, returning nullptr within the expected window.
*/
template <EAccessMode _AccessMode>
class TimedAllocTimeoutTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedAllocTimeoutTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            // Fill the pool
            void *blocks[_STK_POOL_CAPACITY] = {};
            for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
                blocks[i] = g_Pool->TryAlloc();

            // Hold long enough for task 1 to time out, then release
            stk::Sleep(200);

            for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
                g_Pool->Free(blocks[i]);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // let task 0 fill pool first

            int64_t start   = GetTimeNowMs();
            void   *blk     = g_Pool->TimedAlloc(50); // 50 ms timeout
            int64_t elapsed = GetTimeNowMs() - start;

            bool ok = (blk == nullptr) && (elapsed >= 45) && (elapsed <= 65);
            g_SharedCounter = ok ? 1 : 0;

            printf("TimedAllocTimeout: blk=%s elapsed=%d %s\n",
                blk ? "non-null" : "null", (int)elapsed, ok ? "PASS" : "FAIL");
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
// Test 5 – TimedAlloc success: block released within timeout window
// ---------------------------------------------------------------------------

/*! \class TimedAllocSuccessTask
    \brief Task 0 holds all blocks; Task 1 calls TimedAlloc with a generous
           timeout; Task 0 frees a block before the timeout so Task 1 succeeds.
*/
template <EAccessMode _AccessMode>
class TimedAllocSuccessTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedAllocSuccessTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            void *blocks[_STK_POOL_CAPACITY] = {};
            for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
                blocks[i] = g_Pool->TryAlloc();

            stk::Sleep(40); // free before task 1's 150 ms timeout

            g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;

            stk::Sleep(_STK_POOL_TEST_LONG_SLEEP);

            for (size_t i = 1; i < _STK_POOL_CAPACITY; ++i)
                g_Pool->Free(blocks[i]);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // let task 0 fill pool first

            void *blk = g_Pool->TimedAlloc(150); // ample timeout

            bool ok = (blk != nullptr);
            g_SharedCounter = ok ? 1 : 0;

            if (blk) g_Pool->Free(blk);

            printf("TimedAllocSuccess: %s\n", ok ? "PASS" : "FAIL");
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
// Test 6 – Multi-task concurrent alloc/free: counter integrity
// ---------------------------------------------------------------------------

/*! \class ConcurrentAllocFreeTask
    \brief All tasks race to alloc a block, increment a shared counter inside
           the block, copy it out and free; total counter must equal iterations * tasks.
*/
template <EAccessMode _AccessMode>
class ConcurrentAllocFreeTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_iterations;

public:
    ConcurrentAllocFreeTask(uint8_t task_id, int32_t iterations)
        : m_task_id(task_id), m_iterations(iterations)
    {}

private:
    void Run() override
    {
        for (int32_t i = 0; i < m_iterations; ++i)
        {
            // Blocking alloc – each task competes for pool blocks
            int32_t *blk = g_Pool->AllocT<int32_t>();

            if (blk)
            {
                *blk = 1;
                g_SharedCounter += *blk; // intentionally unsynchronised to detect pool corruption
                g_Pool->Free(blk);
            }

            stk::Yield();
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < _STK_POOL_TEST_TASKS_MAX)
                stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            int32_t expected = _STK_POOL_TEST_TASKS_MAX * m_iterations;

            printf("ConcurrentAllocFree: counter=%d (expected %d) %s\n",
                (int)g_SharedCounter, (int)expected,
                (g_SharedCounter == expected) ? "PASS" : "FAIL");

            if (g_SharedCounter == expected)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 7 – Typed helpers: AllocT / TryAllocT / TimedAllocT
// ---------------------------------------------------------------------------

struct TestRecord
{
    uint32_t id;
    uint32_t value;
};

/*! \class TypedAllocTask
    \brief Verifies that the typed wrappers AllocT<T>(), TryAllocT<T>(), and
           TimedAllocT<T>() return correctly typed pointers and that written
           fields are preserved until Free().
*/
template <EAccessMode _AccessMode>
class TypedAllocTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TypedAllocTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // TryAllocT
            TestRecord *rec = g_Pool->TryAllocT<TestRecord>();
            ok &= (rec != nullptr);
            if (rec)
            {
                rec->id    = 42U;
                rec->value = 0xDEADBEEFU;
                ok &= (rec->id == 42U) && (rec->value == 0xDEADBEEFU);
                g_Pool->Free(rec);
            }

            // TimedAllocT with infinite wait
            TestRecord *rec2 = g_Pool->TimedAllocT<TestRecord>(WAIT_INFINITE);
            ok &= (rec2 != nullptr);
            if (rec2)
            {
                rec2->id    = 7U;
                rec2->value = 99U;
                ok &= (rec2->id == 7U);
                g_Pool->Free(rec2);
            }

            // AllocT
            TestRecord *rec3 = g_Pool->AllocT<TestRecord>();
            ok &= (rec3 != nullptr);
            if (rec3) g_Pool->Free(rec3);

            printf("TypedAlloc: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 8 – Free(nullptr) is a no-op and returns false
// ---------------------------------------------------------------------------

/*! \class FreeNullTask
    \brief Ensures Free(nullptr) returns false and does not corrupt the pool.
*/
template <EAccessMode _AccessMode>
class FreeNullTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    FreeNullTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            size_t free_before = g_Pool->GetFreeCount();

            // Free(nullptr) must be a safe no-op
            bool result = g_Pool->Free(nullptr);
            ok &= !result; // must return false

            // Pool state must be unchanged
            ok &= (g_Pool->GetFreeCount() == free_before);

            printf("FreeNull: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 9 – AlignBlockSize helper
// ---------------------------------------------------------------------------

/*! \class AlignBlockSizeTask
    \brief Verifies AlignBlockSize() rounds up to BLOCK_ALIGN multiples and
           never returns a value smaller than BLOCK_ALIGN.
*/
template <EAccessMode _AccessMode>
class AlignBlockSizeTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    AlignBlockSizeTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            const size_t align = stk::memory::BlockMemoryPool::ALIGN_DEFAULT; // default block alignment (STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT, 8)

            // AlignBlockSize(1) must be at least BLOCK_ALIGN
            size_t a1 = stk::memory::BlockMemoryPool::AlignBlockSize(1U);
            ok &= (a1 >= align);
            ok &= (a1 % align == 0U);

            // AlignBlockSize(align) must equal align exactly (already aligned)
            size_t a2 = stk::memory::BlockMemoryPool::AlignBlockSize(align);
            ok &= (a2 == align);

            // AlignBlockSize(align + 1) must round up to 2 * align
            size_t a3 = stk::memory::BlockMemoryPool::AlignBlockSize(align + 1U);
            ok &= (a3 == 2U * align);

            // AlignBlockSize(3 * align) must equal 3 * align (already aligned)
            size_t a4 = stk::memory::BlockMemoryPool::AlignBlockSize(3U * align);
            ok &= (a4 == 3U * align);

            // Pool's GetBlockSize() must equal AlignBlockSize of the raw size at construction
            size_t expected_block_size = stk::memory::BlockMemoryPool::AlignBlockSize(_STK_POOL_BLOCK_SIZE);
            ok &= (g_Pool->GetBlockSize() == expected_block_size);

            printf("AlignBlockSize: a1=%u a2=%u a3=%u a4=%u expected_bs=%u %s\n",
                a1, a2, a3, a4, expected_block_size, ok ? "PASS" : "FAIL");

            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 10 – External vs heap storage: IsStorageValid, GetCapacity, GetBlockSize
// ---------------------------------------------------------------------------

/*! \class StorageModeTask
    \brief Creates a second pool using heap storage and verifies accessors report
           correct values for both storage modes.
*/
template <EAccessMode _AccessMode>
class StorageModeTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    StorageModeTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // External-storage pool (g_Pool) checks
            ok &= g_Pool->IsStorageValid();
            ok &= (g_Pool->GetCapacity() == _STK_POOL_CAPACITY);

            // Heap-storage pool
            const size_t  heap_cap  = 4U;
            const size_t  heap_bsz  = 16U;
            stk::memory::BlockMemoryPool heap_pool(heap_cap, heap_bsz);

            ok &= heap_pool.IsStorageValid();
            ok &= (heap_pool.GetCapacity()  == heap_cap);
            ok &= (heap_pool.GetBlockSize() >= heap_bsz);
            ok &= (heap_pool.GetBlockSize() % sizeof(void *) == 0U);
            ok &= heap_pool.IsEmpty();

            // Quick alloc/free cycle on heap pool
            void *blk = heap_pool.TryAlloc();
            ok &= (blk != nullptr);
            ok &= (heap_pool.GetUsedCount() == 1U);
            heap_pool.Free(blk);
            ok &= heap_pool.IsEmpty();

            printf("StorageMode: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 11 – Stress test: mixed TryAlloc / Alloc / TimedAlloc across all tasks
// ---------------------------------------------------------------------------

/*! \class StressTask
    \brief All tasks hammer the pool with a mix of TryAlloc, blocking Alloc,
           and TimedAlloc operations; verifies the pool never leaks or deadlocks.
*/
template <EAccessMode _AccessMode>
class StressTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
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
            void *blk = nullptr;

            switch (i % 3)
            {
                case 0: blk = g_Pool->TryAlloc();          break;
                case 1: blk = g_Pool->Alloc();             break;
                case 2: blk = g_Pool->TimedAlloc(20);      break;
                default: break;
            }

            if (blk)
            {
                memset(blk, (uint8_t)(m_task_id + i), _STK_POOL_BLOCK_SIZE);
                g_Pool->Free(blk);
                ++g_SharedCounter;
            }

            if ((i % 8) == 0)
                stk::Delay(1);
        }

        ++g_InstancesDone;

        if (m_task_id == (_STK_POOL_TEST_TASKS_MAX - 1))
        {
            while (g_InstancesDone < _STK_POOL_TEST_TASKS_MAX)
                stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            // At least some allocations must have succeeded
            bool ok = (g_SharedCounter > 0) && g_Pool->IsEmpty();

            printf("Stress: counter=%d pool_empty=%s %s\n",
                (int)g_SharedCounter,
                g_Pool->IsEmpty() ? "yes" : "no",
                ok ? "PASS" : "FAIL");

            if (ok) g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Tests 12+ – Cancellation of blocked allocations
// ---------------------------------------------------------------------------

static const int32_t CA_INFINITE        = 1;
static const int32_t CA_NOT_TIMEOUT     = 2;
static const int32_t CA_PLAIN_ALLOC     = 3;
static const int32_t CA_NO_STALE_WAITER = 4;
static const int32_t CA_TWO_WAITERS     = 5;
static const int32_t CA_SIGNAL_WINS     = 6;
static const int32_t CA_VIA_SLEEPCANCEL = 100;

/*! \class CancelAllocTask
    \brief Tests cancelability of blocked BlockMemoryPool allocations (IKernel::CancelTaskWait()
           and stk::SleepCancel()).
    \note  Task 0 is the canceller/checker: it drains the pool, cancels, verifies and finally releases
           every block it holds. Task 1 is the victim (blocked in TimedAllocEx() or Alloc()), task 2 is
           a second waiter used by the two-waiters scenario. The scenario is selected by g_CancelParam:
           1 = cancel an infinite TimedAllocEx(), 2 = cancel is not reported as a timeout,
           3 = plain Alloc() returns nullptr when cancelled, 4 = cancel leaves no stale waiter behind
           (Free() and pool accounting stay correct), 5 = cancelling one of two waiters leaves the other
           intact, 6 = a Free() that arrived first wins over a late cancel. Adding 100 to g_CancelParam
           cancels via stk::SleepCancel() instead of IKernel::CancelTaskWait().
*/
template <EAccessMode _AccessMode>
class CancelAllocTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;
    int32_t m_scenario;
    bool    m_use_sleep_cancel;

public:
    CancelAllocTask(uint8_t task_id, int32_t) // scenario is taken from g_CancelParam in Run(), see g_CancelParam
        : m_task_id(task_id), m_scenario(0), m_use_sleep_cancel(false)
    {}

private:
    void Run() override
    {
        m_scenario         = g_CancelParam % CA_VIA_SLEEPCANCEL;
        m_use_sleep_cancel = (g_CancelParam >= CA_VIA_SLEEPCANCEL);

        if (m_task_id == 0)
            RunCanceller();
        else
        if (m_task_id == 1)
            RunVictim();
        else
            RunSecondWaiter();
    }

    // Task 1: block on the (empty) pool, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        void *blk = nullptr;
        EWaitResult r;

        const int64_t start = GetTimeNowMs();

        if (m_scenario == CA_PLAIN_ALLOC)
        {
            // Alloc() has no result code: nullptr can only mean the wait was cancelled
            blk = g_Pool->Alloc();
            r   = (blk != nullptr ? WAIT_RESULT_SIGNAL : WAIT_RESULT_CANCELED);
        }
        else
        {
            const Timeout timeout = (m_scenario == CA_NOT_TIMEOUT ? 300 : WAIT_INFINITE);
            r = g_Pool->TimedAllocEx(&blk, timeout);
        }

        g_VictimElapsed[0]  = GetTimeNowMs() - start;
        g_VictimResult[0]   = static_cast<int32_t>(r);
        g_VictimGotBlock[0] = (blk != nullptr);

        if (blk != nullptr)
            g_Pool->Free(blk);

        if (m_scenario == CA_NO_STALE_WAITER)
        {
            // a stale wait entry left behind by the cancel would cut this sleep short
            const int64_t sleep_start = GetTimeNowMs();
            stk::Sleep(60);
            g_VictimSleepElapsed = GetTimeNowMs() - sleep_start;
        }
    }

    // Task 2: second waiter, queued behind task 1
    void RunSecondWaiter()
    {
        if (m_scenario != CA_TWO_WAITERS)
            return;

        stk::Sleep(2 * _STK_POOL_TEST_SHORT_SLEEP); // block after task 1

        void *blk = nullptr;

        const int64_t start = GetTimeNowMs();
        const EWaitResult r = g_Pool->TimedAllocEx(&blk, WAIT_INFINITE);

        g_VictimElapsed[1]  = GetTimeNowMs() - start;
        g_VictimResult[1]   = static_cast<int32_t>(r);
        g_VictimGotBlock[1] = (blk != nullptr);

        if (blk != nullptr)
            g_Pool->Free(blk);
    }

    // Free every block still held by the canceller (also wakes any waiter that is still blocked)
    static void ReleaseAll(void **blocks)
    {
        for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
        {
            if (blocks[i] != nullptr)
            {
                g_Pool->Free(blocks[i]);
                blocks[i] = nullptr;
            }
        }
    }

    // Task 0: drain the pool, cancel and verify
    void RunCanceller()
    {
        bool pass = false;
        void *blocks[_STK_POOL_CAPACITY] = {};

        // drain the pool so that the victim (and task 2) block
        for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
            blocks[i] = g_Pool->TryAlloc();

        stk::Sleep(60); // task 1 (and task 2) are blocked in the allocation by now

        if (m_scenario == CA_INFINITE)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // started waiting at ~10 ms, cancelled at ~60 ms; nothing was allocated by the cancel
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && !g_VictimGotBlock[0] &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   g_Pool->IsFull();
        }
        else
        if (m_scenario == CA_NOT_TIMEOUT)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // 300 ms timeout is far away: the wait must end early and as CANCELED, not TIMEOUT
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && !g_VictimGotBlock[0] &&
                   (g_VictimElapsed[0] < 150);
        }
        else
        if (m_scenario == CA_PLAIN_ALLOC)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // documented contract: Alloc() returns nullptr only when the wait was cancelled
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && !g_VictimGotBlock[0] &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   g_Pool->IsFull();
        }
        else
        if (m_scenario == CA_NO_STALE_WAITER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // victim returns and starts its Sleep(60)

            // nobody waits any more: Free() must just push the block, with no waiter left to wake
            const bool freed = g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;

            const bool used_ok = (g_Pool->GetUsedCount() == (_STK_POOL_CAPACITY - 1U));

            // the freed block must be allocatable again, exactly once
            blocks[0] = g_Pool->TryAlloc();
            const bool again_ok = (blocks[0] != nullptr);
            const bool extra_ok = (g_Pool->TryAlloc() == nullptr);

            stk::Sleep(70); // let the victim finish its sleep

            printf("cancel alloc no stale waiter: result=%d freed=%d used_ok=%d again=%d extra_null=%d victim_sleep=%d ms\n",
                (int)g_VictimResult[0], (int)freed, (int)used_ok, (int)again_ok, (int)extra_ok,
                (int)g_VictimSleepElapsed);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && !g_VictimGotBlock[0] &&
                   freed && used_ok && again_ok && extra_ok &&
                   (g_VictimSleepElapsed >= 55); // not woken early by a ghost wait entry
        }
        else
        if (m_scenario == CA_TWO_WAITERS)
        {
            CancelVictim(m_use_sleep_cancel);                   // cancel the first waiter only
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            const bool freed = g_Pool->Free(blocks[0]);         // must wake the second waiter
            blocks[0] = nullptr;
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // the second waiter took the block and returned it, so only our other blocks are outstanding
            const bool used_ok = (g_Pool->GetUsedCount() == (_STK_POOL_CAPACITY - 1U));

            printf("cancel alloc two waiters: first=%d second=%d freed=%d second_got=%d used_ok=%d\n",
                (int)g_VictimResult[0], (int)g_VictimResult[1], (int)freed,
                (int)g_VictimGotBlock[1], (int)used_ok);

            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && !g_VictimGotBlock[0] &&
                   (g_VictimResult[1] == WAIT_RESULT_SIGNAL) && g_VictimGotBlock[1] &&
                   freed && used_ok;
        }
        else
        if (m_scenario == CA_SIGNAL_WINS)
        {
            // back to back, no scheduling point in between: the waiter is already signaled and removed
            // from the wait list when the cancel arrives, so the cancel must be a no-op
            const bool freed = g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // the victim got the block and returned it, so only our other blocks are outstanding
            pass = freed && (g_VictimResult[0] == WAIT_RESULT_SIGNAL) && g_VictimGotBlock[0] &&
                   (g_Pool->GetUsedCount() == (_STK_POOL_CAPACITY - 1U));
        }

        // Release everything we still hold. This also acts as a safety net: if the cancel did not work a
        // waiter is still blocked forever and the kernel would never stop, so the freed blocks wake it
        // and the test fails instead of hanging.
        ReleaseAll(blocks);
        stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

        const bool leak_free = g_Pool->IsEmpty(); // every block is back in the pool

        printf("cancel alloc (scenario=%d, via %s): result[0]=%d elapsed[0]=%d ms leak_free=%d\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
            (int)g_VictimResult[0], (int)g_VictimElapsed[0], (int)leak_free);

        if (pass && leak_free)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------
// Tests 23+ – Destruction of a pool which tasks are blocked on (WAIT_RESULT_DELETED)
// ---------------------------------------------------------------------------

static const int32_t DW_EX    = 1; // one waiter in TimedAllocEx(): WAIT_RESULT_DELETED
static const int32_t DW_TIMED = 2; // one waiter in TimedAlloc(): nullptr (every non-SIGNAL result is mapped to nullptr)
static const int32_t DW_TWO   = 3; // two waiters in TimedAllocEx(), both must be released

/*! \class DeleteWithWaiterTask
    \brief Tests destruction of a BlockMemoryPool which tasks are blocked on (WAIT_RESULT_DELETED).
    \note  Task 0 creates a dedicated pool (external storage of its own), drains it and destroys it while task 1 (and
           task 2 in the two-waiters scenario) wait for a block with an infinite timeout. A task can wait only for an
           empty pool, so this is the one way to destroy a pool with waiters: all its blocks are outstanding and are
           simply not returned (the owner must not Free() them into the destroyed pool). Nobody frees a block, so only
           the destruction can end the waits: the waiters must get WAIT_RESULT_DELETED and no block (neither a block,
           a timeout nor a cancellation) without touching the destroyed object. The storage of the pool object is
           overwritten after the destruction and must still hold that pattern when the waiters are back, so any write
           by a woken waiter is detected. Afterwards the kernel and g_Pool must be fully usable: task 0 drains g_Pool
           and the victim performs a TimedAllocEx(20) on it, which must run to its deadline (a stale wait entry would
           end it early) and return no block; once task 0 has returned its blocks g_Pool must be empty (no leak).
           The scenario is selected by g_DeleteParam: 1 = a waiter in TimedAllocEx(), 2 = a waiter in TimedAlloc()
           (reports nullptr; an infinite wait can not time out, so nullptr after ~50 ms identifies the destruction),
           3 = two waiters in TimedAllocEx() are both released. Task 2 is added by RunTest() but stays idle unless the
           scenario is 3. The plain Alloc() is not covered.
*/
template <EAccessMode _AccessMode>
class DeleteWithWaiterTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
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
    }

    // Task 1: block on the dedicated pool, record how the wait ended
    void RunVictim()
    {
        stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

        g_Victim    = static_cast<ITask *>(this);
        g_VictimTid = stk::GetTid();

        stk::memory::BlockMemoryPool *const pool = g_DeletePool; // created by task 0 before it went to sleep

        void   *blk    = nullptr;
        int32_t result = static_cast<int32_t>(WAIT_RESULT_FAIL);

        const int64_t start = GetTimeNowMs();

        // blocks until the pool is destroyed; nothing but the result is read afterwards
        if (pool != nullptr)
        {
            if (m_scenario == DW_TIMED)
            {
                // TimedAlloc() maps every non-SIGNAL result to nullptr; stored as 1 = block / 0 = no block
                blk    = pool->TimedAlloc(WAIT_INFINITE);
                result = (blk != nullptr ? 1 : 0);
            }
            else
            {
                result = static_cast<int32_t>(pool->TimedAllocEx(&blk, WAIT_INFINITE));
            }
        }

        g_VictimElapsed[0]  = GetTimeNowMs() - start;
        g_VictimResult[0]   = result;
        g_VictimGotBlock[0] = (blk != nullptr); // a block, if any, is not returned: the pool is gone

        // the kernel is in a sane state: a wait on another pool (drained by task 0 at this moment) works, runs to
        // its deadline and returns no block
        void *blk2 = nullptr;

        const int64_t     start2 = GetTimeNowMs();
        const EWaitResult r2     = g_Pool->TimedAllocEx(&blk2, 20);

        g_DeleteCheckElapsed  = GetTimeNowMs() - start2;
        g_DeleteCheckGotBlock = (blk2 != nullptr);
        g_DeleteCheckResult   = static_cast<int32_t>(r2);

        if (blk2 != nullptr) // not expected: do not leak it
            g_Pool->Free(blk2);
    }

    // Task 2: second waiter, queued behind task 1
    void RunSecondWaiter()
    {
        if (m_scenario != DW_TWO)
            return;

        stk::Sleep(2 * _STK_POOL_TEST_SHORT_SLEEP); // block after task 1

        g_DeleteVictim2 = static_cast<ITask *>(this);

        stk::memory::BlockMemoryPool *const pool = g_DeletePool;

        void *blk = nullptr;
        EWaitResult r = WAIT_RESULT_FAIL;

        const int64_t start = GetTimeNowMs();

        if (pool != nullptr)
            r = pool->TimedAllocEx(&blk, WAIT_INFINITE);

        g_VictimElapsed[1]  = GetTimeNowMs() - start;
        g_VictimResult[1]   = static_cast<int32_t>(r);
        g_VictimGotBlock[1] = (blk != nullptr);
    }

    // True if the storage of the destroyed pool object still holds the pattern written after the destruction
    static bool StorageUntouched()
    {
        for (size_t i = 0; i < sizeof(g_DeletePoolObj); ++i)
        {
            if (g_DeletePoolObj[i] != DELETED_FILL)
                return false;
        }

        return true;
    }

    // Task 0: create, drain, destroy and verify
    void RunDestroyer()
    {
        stk::memory::BlockMemoryPool *const pool = ::new (g_DeletePoolObj) stk::memory::BlockMemoryPool(
            _STK_POOL_DELETE_CAPACITY, _STK_POOL_BLOCK_SIZE, g_DeleteBlocks, sizeof(g_DeleteBlocks));
        g_DeletePool = pool;

        // drain the dedicated pool so that the waiters block; the blocks are never returned (the pool is destroyed)
        bool drained = pool->IsStorageValid();
        for (size_t i = 0; i < _STK_POOL_DELETE_CAPACITY; ++i)
            drained &= (pool->TryAlloc() != nullptr);
        drained &= pool->IsFull();

        // make the follow-up allocation of task 1 block: it must wait on g_Pool until its deadline.
        // g_Pool is drained BEFORE the destruction: task 1 is runnable as soon as the destructor wakes it, and a
        // context switch (e.g. a tick on a slow/irregular target such as QEMU RISC-V) may run it before task 0
        // gets to drain g_Pool afterwards, in which case its follow-up wait would get a block immediately
        void *blocks[_STK_POOL_CAPACITY] = {};
        for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
            blocks[i] = g_Pool->TryAlloc();

        stk::Sleep(60); // task 1 (and task 2) block on the dedicated pool in the meantime

        // destroy the pool which is waited on: the waiters are released by the destructor of its condition variable
        pool->~BlockMemoryPool();
        memset(g_DeletePoolObj, DELETED_FILL, sizeof(g_DeletePoolObj));
        g_DeletePool = nullptr;

        stk::Sleep(40); // task 1's 20 ms wait on the drained g_Pool runs to its deadline in the meantime

        for (size_t i = 0; i < _STK_POOL_CAPACITY; ++i)
        {
            if (blocks[i] != nullptr)
                g_Pool->Free(blocks[i]);
        }

        stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 4); // let the waiters resume and task 1 finish its check on g_Pool

        // safety net: if a waiter was not released it would block forever and the kernel would never stop,
        // cancel it so that the test fails instead of hanging
        const bool released = (g_VictimResult[0] != -100) &&
                              ((m_scenario != DW_TWO) || (g_VictimResult[1] != -100));
        if (!released)
        {
            ITask *const victim1 = g_Victim;
            ITask *const victim2 = g_DeleteVictim2;

            if (victim1 != nullptr)
                g_Kernel.CancelTaskWait(victim1);
            if (victim2 != nullptr)
                g_Kernel.CancelTaskWait(victim2);

            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 5);
        }

        const bool leak_free = g_Pool->IsEmpty(); // every block of g_Pool is back
        const bool untouched = StorageUntouched();

        printf("delete with waiter (scenario=%d): first=%d got_block=%d elapsed=%d ms, second=%d got_block=%d elapsed=%d ms, "
            "g_Pool wait: result=%d got_block=%d elapsed=%d ms, leak_free=%d storage_untouched=%d\n",
            (int)m_scenario,
            (int)g_VictimResult[0], (int)g_VictimGotBlock[0], (int)g_VictimElapsed[0],
            (int)g_VictimResult[1], (int)g_VictimGotBlock[1], (int)g_VictimElapsed[1],
            (int)g_DeleteCheckResult, (int)g_DeleteCheckGotBlock, (int)g_DeleteCheckElapsed,
            (int)leak_free, (int)untouched);

        // task 1 started waiting at ~10 ms, task 2 at ~20 ms, the pool was destroyed at ~60 ms
        bool pass = drained && released && !g_VictimGotBlock[0] &&
                    (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75);

        if (m_scenario == DW_TIMED)
        {
            // nullptr, and an infinite wait can not time out: it was the destruction that ended it
            pass = pass && (g_VictimResult[0] == 0);
        }
        else
        {
            pass = pass && (g_VictimResult[0] == WAIT_RESULT_DELETED);
        }

        if (m_scenario == DW_TWO)
        {
            pass = pass && (g_VictimResult[1] == WAIT_RESULT_DELETED) && !g_VictimGotBlock[1] &&
                   (g_VictimElapsed[1] >= 25) && (g_VictimElapsed[1] <= 65);
        }

        // the other pool was not affected: a 20 ms wait on it ran to its deadline (a stale wait entry would cut it
        // short) without a block, and all its blocks are back; the destroyed pool was not written to
        pass = pass &&
               (g_DeleteCheckResult == WAIT_RESULT_TIMEOUT) && !g_DeleteCheckGotBlock && (g_DeleteCheckElapsed >= 15) &&
               leak_free && untouched;

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
    g_VictimSleepElapsed = 0;

    g_DeletePool          = nullptr;
    g_DeleteVictim2       = nullptr;
    g_DeleteCheckResult   = -100;
    g_DeleteCheckGotBlock = false;
    g_DeleteCheckElapsed  = 0;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]   = -100;
        g_VictimGotBlock[i] = false;
        g_VictimElapsed[i]  = 0;
    }
}

} // namespace blockpool
} // namespace test
} // namespace stk

// ---------------------------------------------------------------------------
// Task-count predicates
// ---------------------------------------------------------------------------

/*! \fn    NeedsTwoTasks
    \brief Returns true if the test requires tasks 0 and 1.
    \note  Single-task tests (TryAllocFree, ExhaustPool, TypedAlloc, FreeNull,
           AlignBlockSize, StorageMode) use only task 0.
*/
static bool NeedsTwoTasks(const char *test_name)
{
    return (strcmp(test_name, "BlockingAlloc")      == 0) ||
           (strcmp(test_name, "TimedAllocTimeout")  == 0) ||
           (strcmp(test_name, "TimedAllocSuccess")  == 0) ||
           (strcmp(test_name, "ConcurrentAllocFree")== 0) ||
           (strcmp(test_name, "Stress")             == 0) ||
           (strncmp(test_name, "CancelAlloc", 11)   == 0) ||
           (strncmp(test_name, "DeleteWithWaiter", 16) == 0);
}

/*! \fn    NeedsThreeTasks
    \brief Returns true if the test requires tasks 0, 1 and 2 (but not 3-4).
    \note  Cancellation and delete-with-waiter tests: canceller (destroyer), victim and a second waiter.
*/
static bool NeedsThreeTasks(const char *test_name)
{
    return (strncmp(test_name, "CancelAlloc", 11) == 0) ||
           (strncmp(test_name, "DeleteWithWaiter", 16) == 0);
}

/*! \fn    NeedsAllTasks
    \brief Returns true if the test requires all 5 tasks (0–4).
    \note  ConcurrentAllocFree and Stress both verify g_InstancesDone against
           _STK_POOL_TEST_TASKS_MAX and compute expected results from that count.
*/
static bool NeedsAllTasks(const char *test_name)
{
    return (strcmp(test_name, "ConcurrentAllocFree") == 0) ||
           (strcmp(test_name, "Stress")              == 0);
}

// ---------------------------------------------------------------------------
// RunTest helper
// ---------------------------------------------------------------------------

/*! \fn    RunTest
    \brief Helper function to run a single blockpool test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::blockpool;

    printf("Test: %s\n", test_name);

    ResetTestState();

    stk::memory::BlockMemoryPool pool(
        _STK_POOL_CAPACITY,
        _STK_POOL_BLOCK_SIZE,
        g_PoolStorage,
        sizeof(g_PoolStorage));

    g_Pool = &pool;

    STK_TASK TaskType task0(0, param);
    STK_TASK TaskType task1(1, param);
    TaskType task2(2, param);
    TaskType task3(3, param);
    TaskType task4(4, param);

    g_Kernel.AddTask(&task0);

    if (NeedsTwoTasks(test_name))
        g_Kernel.AddTask(&task1);

    if (NeedsThreeTasks(test_name) || NeedsAllTasks(test_name))
        g_Kernel.AddTask(&task2);

    if (NeedsAllTasks(test_name))
    {
        g_Kernel.AddTask(&task3);
        g_Kernel.AddTask(&task4);
    }

    g_Kernel.Start();

    g_Pool = nullptr;

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
    \brief Entry point for the BlockMemoryPool test suite.
*/
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    stk::test::blockpool::g_Kernel.Initialize();

#define RUN(TestClass, name, param) \
    do { \
        if (RunTest<TestClass<ACCESS_PRIVILEGED>>(name, param) \
                != TestContext::SUCCESS_EXIT_CODE) \
            total_failures++; \
        else \
            total_success++; \
    } while (0)

#ifndef __ARM_ARCH_6M__

    // Test 1: TryAlloc / Free basic cycle
    RUN(stk::test::blockpool::TryAllocFreeTask,        "TryAllocFree",        0);

    // Test 2: Exhaust pool, verify IsFull, free all
    RUN(stk::test::blockpool::ExhaustPoolTask,         "ExhaustPool",         0);

    // Test 3: Blocking Alloc unblocked by Free from another task
    RUN(stk::test::blockpool::BlockingAllocTask,       "BlockingAlloc",       0);

    // Test 4: TimedAlloc expires when pool remains full
    RUN(stk::test::blockpool::TimedAllocTimeoutTask,   "TimedAllocTimeout",   0);

    // Test 5: TimedAlloc succeeds when block freed within timeout
    RUN(stk::test::blockpool::TimedAllocSuccessTask,   "TimedAllocSuccess",   0);

    // Test 6: Concurrent alloc/free counter integrity (20 iterations per task)
    RUN(stk::test::blockpool::ConcurrentAllocFreeTask, "ConcurrentAllocFree", 20);

    // Test 7: Typed helpers AllocT / TryAllocT / TimedAllocT
    RUN(stk::test::blockpool::TypedAllocTask,          "TypedAlloc",          0);

    // Test 8: Free(nullptr) is a safe no-op
    RUN(stk::test::blockpool::FreeNullTask,            "FreeNull",            0);

    // Test 9: AlignBlockSize static helper
    RUN(stk::test::blockpool::AlignBlockSizeTask,      "AlignBlockSize",      0);

    // Test 10: External vs heap storage constructors + accessors
    RUN(stk::test::blockpool::StorageModeTask,         "StorageMode",         0);

    // Cancellation of blocked allocations (tasks 0-2 only), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        using namespace stk::test::blockpool;

        struct CancelCase { const char *name; int32_t param; };

        static const CancelCase cases[] =
        {
            { "CancelAlloc_Infinite",            CA_INFINITE                            },
            { "CancelAlloc_NotTimeout",          CA_NOT_TIMEOUT                         },
            { "CancelAlloc_PlainAlloc",          CA_PLAIN_ALLOC                         },
            { "CancelAlloc_NoStaleWaiter",       CA_NO_STALE_WAITER                     },
            { "CancelAlloc_TwoWaiters",          CA_TWO_WAITERS                         },
            { "CancelAlloc_SignalWins",          CA_SIGNAL_WINS                         },
            { "CancelAlloc_SleepCancel",         CA_VIA_SLEEPCANCEL + CA_INFINITE       },
            { "CancelAlloc_SleepCancelPlain",    CA_VIA_SLEEPCANCEL + CA_PLAIN_ALLOC    },
            { "CancelAlloc_SleepCancelNoStale",  CA_VIA_SLEEPCANCEL + CA_NO_STALE_WAITER},
            { "CancelAlloc_SleepCancelTwo",      CA_VIA_SLEEPCANCEL + CA_TWO_WAITERS    },
            { "CancelAlloc_SleepCancelSignal",   CA_VIA_SLEEPCANCEL + CA_SIGNAL_WINS    }
        };

        for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); ++i)
        {
            g_CancelParam = cases[i].param;

            if (RunTest<CancelAllocTask<ACCESS_PRIVILEGED>>(cases[i].name, cases[i].param)
                != TestContext::SUCCESS_EXIT_CODE)
                total_failures++;
            else
                total_success++;
        }
    }

    // Tests 23-25: pool destroyed while tasks wait for a block (WAIT_RESULT_DELETED, tasks 0-2 only)
    {
        using namespace stk::test::blockpool;

        struct DeleteCase { const char *name; int32_t param; };

        static const DeleteCase cases[] =
        {
            { "DeleteWithWaiter",       DW_EX    },
            { "DeleteWithWaiterTimed",  DW_TIMED },
            { "DeleteWithWaiterTwo",    DW_TWO   }
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

    // Test 11: Stress test (ARM Cortex-M0 compatible)
    RUN(stk::test::blockpool::StressTask, "Stress", 200);

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
