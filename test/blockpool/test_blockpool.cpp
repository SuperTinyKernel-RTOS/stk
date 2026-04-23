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
#ifdef __ARM_ARCH_6M__
#define _STK_POOL_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_POOL_STACK_SIZE       256
#define STK_TASK                   static
#endif

// Private memory allocators (we define malloc, free here to overcome absence of declaration in
// case of -ffreestanding compiler flag).
extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);
void *stk::memory::MemoryAllocator::Allocate(size_t size) { return malloc(size); }
void stk::memory::MemoryAllocator::Free(void *ptr) { free(ptr); }

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
    _STK_POOL_TEST_TASKS_MAX, SwitchStrategyRR, PlatformDefault> g_Kernel;

// Shared pool + external-storage buffer used by most tests
alignas(sizeof(void *)) static uint8_t
    g_PoolStorage[_STK_POOL_CAPACITY * stk::memory::BlockMemoryPool::AlignBlockSize(_STK_POOL_BLOCK_SIZE)];

static stk::memory::BlockMemoryPool *g_Pool = nullptr;

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

            const size_t align = sizeof(void *); // BLOCK_ALIGN = sizeof(MemoryBlock) = sizeof(void*)

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
// Helper – reset shared state between tests
// ---------------------------------------------------------------------------

static void ResetTestState()
{
    g_TestResult    = 0;
    g_InstancesDone = 0;
    g_SharedCounter = 0;
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
           (strcmp(test_name, "Stress")             == 0);
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

    if (NeedsAllTasks(test_name))
    {
        g_Kernel.AddTask(&task2);
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
