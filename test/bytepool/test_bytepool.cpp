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
#include <memory/stk_memory_bytepool.h>
#include <memory/stk_memory_allocator_malloc.h> // use malloc-based allocator

#include "stktest_context.h"

using namespace stk;
using namespace stk::test;

STK_TEST_DECL_ASSERT;

#define _STK_POOL_TEST_TASKS_MAX   5
#define _STK_POOL_TEST_TIMEOUT     1000
#define _STK_POOL_TEST_SHORT_SLEEP 10
#define _STK_POOL_TEST_LONG_SLEEP  100

// Main test pool: 512 bytes = 8 uniform blocks of 64 bytes (56 payload + 8 header) when all allocations
// use _STK_POOL_BLK_PAYLOAD, which makes exhaustion, adjacency and fragmentation easy to reason about.
#define _STK_POOL_STORAGE_SIZE     512U
#define _STK_POOL_BLK_PAYLOAD      56U
#define _STK_POOL_BLK_COUNT        8U
#ifdef __ARM_ARCH_6M__
#define _STK_POOL_STACK_SIZE       128 // ARM Cortex-M0
#define STK_TASK
#else
#define _STK_POOL_STACK_SIZE       512
#define STK_TASK                   static
#endif

static_assert((_STK_POOL_BLK_COUNT * (_STK_POOL_BLK_PAYLOAD + stk::memory::BytePool::HEADER_SIZE)) == _STK_POOL_STORAGE_SIZE,
    "uniform block layout must fill the test pool exactly");

namespace stk {
namespace test {

/*! \namespace stk::test::bytepool
    \brief     Namespace of BytePool test.
 */
namespace bytepool {

// Test results storage
static volatile int32_t g_TestResult    = 0;
static volatile int32_t g_InstancesDone = 0;
static volatile int32_t g_SharedCounter = 0;
static volatile int32_t g_Errors        = 0; // data corruption / misalignment detected by worker tasks

// Kernel
static Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0),
    _STK_POOL_TEST_TASKS_MAX, sched::SwitchStrategyRR, PlatformDefault> g_Kernel;

// Shared pool + external-storage buffer used by most tests
alignas(8) static uint8_t g_PoolStorage[_STK_POOL_STORAGE_SIZE];

// Small pool storage used by SplitCoalesce / StorageMode
alignas(8) static uint8_t g_SmallStorage[128];

static stk::memory::BytePool *g_Pool = nullptr;

// Blocking-wake tests state (see BlockingLargeTask, WakeAllWaitersTask)
static volatile bool    g_WaiterDone[2]          = {false, false};

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

// Cancel the wait of g_Victim either via IKernel::CancelTaskWait() or via stk::SleepCancel()
static void CancelVictim(bool use_sleep_cancel)
{
    if (use_sleep_cancel)
        stk::SleepCancel(g_VictimTid);
    else
        g_Kernel.CancelTaskWait(g_Victim);
}

// ---------------------------------------------------------------------------
// Pool inspection helpers (built on BytePool::GetInfo)
// ---------------------------------------------------------------------------

// Pool space (headers included) that is currently free
static size_t FreeSpace()
{
    size_t available, largest, fragments;
    g_Pool->GetInfo(available, largest, fragments);
    return (available + (fragments * stk::memory::BytePool::HEADER_SIZE));
}

// Number of outstanding uniform blocks (valid while only _STK_POOL_BLK_PAYLOAD-sized allocations are used)
static size_t UsedBlocks()
{
    return ((_STK_POOL_STORAGE_SIZE - FreeSpace()) / (_STK_POOL_BLK_PAYLOAD + stk::memory::BytePool::HEADER_SIZE));
}

// True if nothing is allocated: the whole pool forms exactly one free fragment
static bool IsAllFree()
{
    size_t available, largest, fragments;
    g_Pool->GetInfo(available, largest, fragments);
    return (fragments == 1U) && (available == g_Pool->GetMaxRequest()) && (largest == available);
}

// True if no byte is available for allocation
static bool IsExhausted()
{
    size_t available, largest, fragments;
    g_Pool->GetInfo(available, largest, fragments);
    return (available == 0U) && (largest == 0U) && (fragments == 0U);
}

// Allocate every uniform block of the pool (non-blocking)
static void DrainPool(void **blocks)
{
    for (size_t i = 0; i < _STK_POOL_BLK_COUNT; ++i)
        blocks[i] = g_Pool->TryAlloc(_STK_POOL_BLK_PAYLOAD);
}

// Free every block that is still held (also wakes any waiter that is still blocked)
static void ReleaseAll(void **blocks)
{
    for (size_t i = 0; i < _STK_POOL_BLK_COUNT; ++i)
    {
        if (blocks[i] != nullptr)
        {
            g_Pool->Free(blocks[i]);
            blocks[i] = nullptr;
        }
    }
}

static bool IsAligned8(const void *ptr)
{
    return ((reinterpret_cast<uintptr_t>(ptr) & 7U) == 0U);
}

// ---------------------------------------------------------------------------
// Test 1 – TryAlloc / Free basic cycle (single task)
// ---------------------------------------------------------------------------

/*! \class TryAllocFreeTask
    \brief Verifies the initial pool state, that TryAlloc returns a valid aligned block,
           that Free recycles it and that GetInfo accounting stays consistent.
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

            // Initial state: one free fragment spanning the whole pool
            ok &= g_Pool->IsStorageValid();
            ok &= (g_Pool->GetSize() == _STK_POOL_STORAGE_SIZE);
            ok &= (g_Pool->GetMaxRequest() == (_STK_POOL_STORAGE_SIZE - stk::memory::BytePool::HEADER_SIZE));
            ok &= IsAllFree();

            // Allocate one block non-blocking
            void *blk = g_Pool->TryAlloc(64U);
            ok &= (blk != nullptr);
            ok &= IsAligned8(blk);

            // 64-byte payload + header taken from the single free fragment
            size_t available, largest, fragments;
            g_Pool->GetInfo(available, largest, fragments);
            const size_t expected = (_STK_POOL_STORAGE_SIZE - stk::memory::BytePool::RequiredSize(64U) -
                                     stk::memory::BytePool::HEADER_SIZE);
            ok &= (available == expected) && (largest == expected) && (fragments == 1U);

            // Write to the block to verify it is usable memory
            memset(blk, 0xAB, 64U);

            // Return the block: it merges with the free tail, pool is back to a single fragment
            ok &= g_Pool->Free(blk);
            ok &= IsAllFree();

            // The whole pool must be allocatable again as one block
            void *whole = g_Pool->TryAlloc(g_Pool->GetMaxRequest());
            ok &= (whole != nullptr);
            ok &= IsExhausted();
            ok &= g_Pool->Free(whole);
            ok &= IsAllFree();

            printf("TryAllocFree: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 2 – Exhaust pool with TryAlloc then verify exhaustion / free all
// ---------------------------------------------------------------------------

/*! \class ExhaustPoolTask
    \brief Drains the entire pool via TryAlloc, verifies that blocks are distinct, aligned and do not
           overlap, that a further allocation fails (also via TimedAllocEx(NO_WAIT)), then frees every
           block in order, which leaves the pool fragmented in memory but fully reusable.
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
            void *blocks[_STK_POOL_BLK_COUNT] = {};

            // Drain every block, fill each with its own pattern
            DrainPool(blocks);
            for (size_t i = 0; i < _STK_POOL_BLK_COUNT; ++i)
            {
                ok &= (blocks[i] != nullptr);
                ok &= IsAligned8(blocks[i]);

                if (blocks[i] != nullptr)
                    memset(blocks[i], (int)(0x10 + i), _STK_POOL_BLK_PAYLOAD);
            }

            ok &= IsExhausted();
            ok &= (UsedBlocks() == _STK_POOL_BLK_COUNT);

            // Neighbouring blocks must not overlap: every pattern must be intact
            for (size_t i = 0; i < _STK_POOL_BLK_COUNT; ++i)
            {
                const uint8_t *p = static_cast<const uint8_t *>(blocks[i]);
                for (size_t k = 0; (p != nullptr) && (k < _STK_POOL_BLK_PAYLOAD); ++k)
                    ok &= (p[k] == (uint8_t)(0x10 + i));
            }

            // One more TryAlloc must fail without blocking
            void *extra = g_Pool->TryAlloc(1U);
            ok &= (extra == nullptr);

            // NO_WAIT must report a timeout and leave the output null
            void *extra2 = reinterpret_cast<void *>(1);
            ok &= (g_Pool->TimedAllocEx(&extra2, 1U, NO_WAIT) == WAIT_RESULT_TIMEOUT);
            ok &= (extra2 == nullptr);

            // Return all blocks in address order: nothing is merged backwards at Free() time, but the
            // statistics must already see a single contiguous free run
            for (size_t i = 0; i < _STK_POOL_BLK_COUNT; ++i)
                ok &= g_Pool->Free(blocks[i]);

            ok &= IsAllFree();

            // The lazy merge during the scan must make the whole pool allocatable as one block
            void *whole = g_Pool->TryAlloc(g_Pool->GetMaxRequest());
            ok &= (whole != nullptr);
            if (whole != nullptr)
                ok &= g_Pool->Free(whole);

            ok &= IsAllFree();

            printf("ExhaustPool: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 3 – Split and coalesce behaviour (single task)
// ---------------------------------------------------------------------------

/*! \class SplitCoalesceTask
    \brief Verifies first-fit placement, block splitting, merging of free neighbours, and that a
           too-small remainder is not split off.
*/
template <EAccessMode _AccessMode>
class SplitCoalesceTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    SplitCoalesceTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            // --- part 1: adjacent frees are merged, first-fit reuses the merged hole ---

            uint8_t *a = static_cast<uint8_t *>(g_Pool->TryAlloc(24U)); // 32-byte block
            uint8_t *b = static_cast<uint8_t *>(g_Pool->TryAlloc(24U));
            uint8_t *c = static_cast<uint8_t *>(g_Pool->TryAlloc(24U));

            ok &= (a != nullptr) && (b != nullptr) && (c != nullptr);

            // sequential first-fit placement: blocks follow each other, 32 bytes apart
            ok &= ((b - a) == 32) && ((c - b) == 32);

            // free B then A: A is merged with the free B that follows it (64-byte hole),
            // C stays in use, the rest of the pool is the second fragment
            ok &= g_Pool->Free(b);
            ok &= g_Pool->Free(a);

            size_t available, largest, fragments;
            g_Pool->GetInfo(available, largest, fragments);

            const size_t hole_payload = (64U - stk::memory::BytePool::HEADER_SIZE);
            const size_t tail_payload = (_STK_POOL_STORAGE_SIZE - (3U * 32U) - stk::memory::BytePool::HEADER_SIZE);

            ok &= (fragments == 2U);
            ok &= (largest == tail_payload);
            ok &= (available == (hole_payload + tail_payload));

            // a request that exactly fits the merged hole must land in it (first-fit)
            uint8_t *d = static_cast<uint8_t *>(g_Pool->TryAlloc(hole_payload));
            ok &= (d == a);

            // free D and C: C merges with the tail at Free() time, D is merged by the statistics
            if (d != nullptr)
                ok &= g_Pool->Free(d);
            ok &= g_Pool->Free(c);
            ok &= IsAllFree();

            // --- part 2: remainder smaller than a minimal block is not split off ---

            stk::memory::BytePool small_pool(g_SmallStorage, sizeof(g_SmallStorage)); // 2 x 64-byte blocks

            uint8_t *p1 = static_cast<uint8_t *>(small_pool.TryAlloc(56U)); // 64-byte block, 64 bytes remain
            uint8_t *p2 = static_cast<uint8_t *>(small_pool.TryAlloc(48U)); // needs 56: remainder 8 < 16, no split

            ok &= (p1 != nullptr) && (p2 != nullptr);

            size_t sa, sl, sf;
            small_pool.GetInfo(sa, sl, sf);
            ok &= (sa == 0U) && (sl == 0U) && (sf == 0U); // the whole second block went to p2

            if (p2 != nullptr)
            {
                memset(p2, 0xCD, 56U); // the full 56-byte payload must be usable
                ok &= small_pool.Free(p2);
            }

            small_pool.GetInfo(sa, sl, sf);
            ok &= (sa == 56U) && (sl == 56U) && (sf == 1U);

            if (p1 != nullptr)
                ok &= small_pool.Free(p1); // merges forward with the free second block

            small_pool.GetInfo(sa, sl, sf);
            ok &= (sa == small_pool.GetMaxRequest()) && (sl == sa) && (sf == 1U);

            printf("SplitCoalesce: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 4 – Blocking Alloc: Free wakes a blocked allocator
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
            void *blocks[_STK_POOL_BLK_COUNT] = {};
            DrainPool(blocks);

            // Give task 1 time to enter Alloc() and block
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // Free one block — this must wake task 1
            g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;

            // Wait for task 1 to finish then free the remaining blocks
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            ReleaseAll(blocks);
        }
        else
        if (m_task_id == 1)
        {
            // Pool is full; Alloc() must block until task 0 frees a block
            void *blk = g_Pool->Alloc(_STK_POOL_BLK_PAYLOAD); // blocking

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

            bool ok = (g_SharedCounter == 1) && IsAllFree();
            printf("BlockingAlloc: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 5 – Blocking Alloc of a large request: needs adjacent free blocks
// ---------------------------------------------------------------------------

/*! \class BlockingLargeTask
    \brief Task 0 holds all blocks; Task 1 requests 2 blocks worth of memory. Freeing one block must
           NOT satisfy the request (task 1 re-checks and keeps waiting); freeing a second, adjacent
           block must (the two free blocks are merged by the allocator).
*/
template <EAccessMode _AccessMode>
class BlockingLargeTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    BlockingLargeTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        bool still_blocked = false;
        bool woke          = false;

        if (m_task_id == 0)
        {
            void *blocks[_STK_POOL_BLK_COUNT] = {};
            DrainPool(blocks);

            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2); // task 1 is blocked by now

            // one 64-byte block is not enough for 120 bytes (needs 128 incl. header)
            g_Pool->Free(blocks[3]);
            blocks[3] = nullptr;
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            still_blocked = (g_SharedCounter == 0);

            // the adjacent block completes a 128-byte hole
            g_Pool->Free(blocks[4]);
            blocks[4] = nullptr;
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            woke = (g_SharedCounter == 1);

            ReleaseAll(blocks);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // let task 0 fill pool first

            uint8_t *blk = static_cast<uint8_t *>(g_Pool->Alloc(120U)); // blocking

            if (blk != nullptr)
            {
                memset(blk, 0x5A, 120U);
                g_SharedCounter = 1;
                g_Pool->Free(blk);
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            bool ok = still_blocked && woke && IsAllFree();
            printf("BlockingLarge: still_blocked=%d woke=%d %s\n",
                (int)still_blocked, (int)woke, ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 6 – Free wakes ALL waiters (different request sizes)
// ---------------------------------------------------------------------------

/*! \class WakeAllWaitersTask
    \brief Task 1 waits for a large request (4 blocks), task 2 waits for a small one (1 block), in
           that order. Freeing a single block must satisfy task 2 even though task 1, which was
           queued first, cannot use it. Freeing the neighbouring blocks later satisfies task 1.
*/
template <EAccessMode _AccessMode>
class WakeAllWaitersTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    WakeAllWaitersTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        bool pass_small = false;
        bool pass_large = false;

        if (m_task_id == 0)
        {
            void *blocks[_STK_POOL_BLK_COUNT] = {};
            DrainPool(blocks);

            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 4); // task 1 (at ~10 ms) and task 2 (at ~20 ms) are blocked

            // a single block: useless for task 1 (queued first), exactly what task 2 needs
            g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            pass_small = g_WaiterDone[1] && !g_WaiterDone[0];

            // blocks 0..3 now form a 256-byte hole for task 1
            for (size_t i = 1; i < 4U; ++i)
            {
                g_Pool->Free(blocks[i]);
                blocks[i] = nullptr;
            }
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            pass_large = g_WaiterDone[0];

            ReleaseAll(blocks);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            // 248 bytes payload + header = 4 blocks
            void *blk = g_Pool->Alloc((4U * (_STK_POOL_BLK_PAYLOAD + stk::memory::BytePool::HEADER_SIZE)) -
                                      stk::memory::BytePool::HEADER_SIZE);
            if (blk != nullptr)
            {
                g_WaiterDone[0] = true;
                g_Pool->Free(blk);
            }
        }
        else
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            void *blk = g_Pool->Alloc(_STK_POOL_BLK_PAYLOAD);
            if (blk != nullptr)
            {
                g_WaiterDone[1] = true;
                g_Pool->Free(blk);
            }
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 3)
                stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            bool ok = pass_small && pass_large && IsAllFree();
            printf("WakeAllWaiters: small=%d large=%d %s\n",
                (int)pass_small, (int)pass_large, ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 7 – TimedAlloc timeout: expires when no block becomes available
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
            void *blocks[_STK_POOL_BLK_COUNT] = {};
            DrainPool(blocks);

            // Hold long enough for task 1 to time out, then release
            stk::Sleep(200);

            ReleaseAll(blocks);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // let task 0 fill pool first

            void   *blk     = nullptr;
            int64_t start   = GetTimeNowMs();
            EWaitResult r   = g_Pool->TimedAllocEx(&blk, _STK_POOL_BLK_PAYLOAD, 50); // 50 ms timeout
            int64_t elapsed = GetTimeNowMs() - start;

            bool ok = (r == WAIT_RESULT_TIMEOUT) && (blk == nullptr) && (elapsed >= 45) && (elapsed <= 65);
            g_SharedCounter = ok ? 1 : 0;

            printf("TimedAllocTimeout: result=%d blk=%s elapsed=%d %s\n",
                (int)r, blk ? "non-null" : "null", (int)elapsed, ok ? "PASS" : "FAIL");
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
// Test 8 – TimedAlloc success: block released within timeout window
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
            void *blocks[_STK_POOL_BLK_COUNT] = {};
            DrainPool(blocks);

            stk::Sleep(40); // free before task 1's 150 ms timeout

            g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;

            stk::Sleep(_STK_POOL_TEST_LONG_SLEEP);

            ReleaseAll(blocks);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // let task 0 fill pool first

            void *blk = g_Pool->TimedAlloc(_STK_POOL_BLK_PAYLOAD, 150); // ample timeout

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
// Test 9 – TimedAlloc deadline is not restarted by unsuccessful wake-ups
// ---------------------------------------------------------------------------

/*! \class TimedAllocDeadlineTask
    \brief Task 1 waits up to 100 ms for 2 adjacent blocks. Task 0 keeps freeing single, non-adjacent
           blocks, so every Free() wakes task 1 without satisfying it. The wait must still expire
           after ~100 ms in total instead of being restarted by each wake-up.
*/
template <EAccessMode _AccessMode>
class TimedAllocDeadlineTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    TimedAllocDeadlineTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            void *blocks[_STK_POOL_BLK_COUNT] = {};
            DrainPool(blocks);

            stk::Sleep(40);                 // t = 40 ms: task 1 is waiting (since ~10 ms)
            g_Pool->Free(blocks[1]);        // wake-up #1, not enough
            blocks[1] = nullptr;

            stk::Sleep(30);                 // t = 70 ms
            g_Pool->Free(blocks[5]);        // wake-up #2, not adjacent to block 1, not enough
            blocks[5] = nullptr;

            stk::Sleep(150);                // t = 220 ms: task 1 must have timed out at ~110 ms

            ReleaseAll(blocks);
        }
        else
        if (m_task_id == 1)
        {
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // let task 0 fill pool first

            void   *blk     = nullptr;
            int64_t start   = GetTimeNowMs();
            EWaitResult r   = g_Pool->TimedAllocEx(&blk, 120U, 100); // needs 2 adjacent blocks
            int64_t elapsed = GetTimeNowMs() - start;

            // restarting the timeout on every wake-up would give ~170 ms
            bool ok = (r == WAIT_RESULT_TIMEOUT) && (blk == nullptr) && (elapsed >= 95) && (elapsed <= 125);
            g_SharedCounter = ok ? 1 : 0;

            printf("TimedAllocDeadline: result=%d elapsed=%d %s\n", (int)r, (int)elapsed, ok ? "PASS" : "FAIL");
        }

        ++g_InstancesDone;

        if (m_task_id == 0)
        {
            while (g_InstancesDone < 2)
                stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP);

            bool ok = (g_SharedCounter == 1) && IsAllFree();
            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 10 – Multi-task concurrent alloc/free of varying sizes: data integrity
// ---------------------------------------------------------------------------

/*! \class ConcurrentAllocFreeTask
    \brief All tasks race to allocate blocks of different sizes, fill them with a task-specific
           pattern, yield, verify the pattern and free. Any overlap of blocks or corruption of the
           block headers shows up as a pattern mismatch, a misaligned pointer or a pool that does
           not return to a single free fragment.
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
            const size_t  size = 8U + (((size_t)m_task_id * 13U + (size_t)i * 7U) % 57U); // 8..64 bytes
            const uint8_t pat  = (uint8_t)(m_task_id * 31 + i + 1);

            // Blocking alloc – each task competes for pool memory
            uint8_t *blk = static_cast<uint8_t *>(g_Pool->Alloc(size));

            if (blk)
            {
                if (!IsAligned8(blk))
                    ++g_Errors;

                memset(blk, pat, size);

                stk::Yield(); // let the others allocate around us

                for (size_t k = 0; k < size; ++k)
                {
                    if (blk[k] != pat)
                    {
                        ++g_Errors;
                        break;
                    }
                }

                ++g_SharedCounter; // intentionally unsynchronised to detect pool corruption
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

            bool ok = (g_SharedCounter == expected) && (g_Errors == 0) && IsAllFree();

            printf("ConcurrentAllocFree: counter=%d (expected %d) errors=%d %s\n",
                (int)g_SharedCounter, (int)expected, (int)g_Errors, ok ? "PASS" : "FAIL");

            if (ok)
                g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 11 – Typed helpers: AllocT / TryAllocT / TimedAllocT
// ---------------------------------------------------------------------------

struct TestRecord
{
    uint32_t id;
    uint32_t value;
};

/*! \class TypedAllocTask
    \brief Verifies that the typed wrappers AllocT<T>(), TryAllocT<T>(), and
           TimedAllocT<T>() return correctly typed, aligned pointers and that written
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
                ok &= IsAligned8(rec);
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

            ok &= IsAllFree();

            printf("TypedAlloc: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 12 – Free(nullptr) is a no-op and returns false
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

            // Free(nullptr) must be a safe no-op
            bool result = g_Pool->Free(nullptr);
            ok &= !result; // must return false

            // Pool state must be unchanged
            ok &= IsAllFree();

            printf("FreeNull: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 13 – Invalid requests fail immediately (never block)
// ---------------------------------------------------------------------------

/*! \class InvalidRequestTask
    \brief Zero-sized and oversized requests can never succeed: they must fail at once, even with an
           infinite timeout, instead of blocking the task forever.
*/
template <EAccessMode _AccessMode>
class InvalidRequestTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    InvalidRequestTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;
            const size_t max_req = g_Pool->GetMaxRequest();

            ok &= (g_Pool->TryAlloc(0U) == nullptr);
            ok &= (g_Pool->TryAlloc(max_req + 1U) == nullptr);

            const int64_t start = GetTimeNowMs();

            void *blk = reinterpret_cast<void *>(1);
            ok &= (g_Pool->TimedAllocEx(&blk, 0U, WAIT_INFINITE) == WAIT_RESULT_FAIL);
            ok &= (blk == nullptr);

            blk = reinterpret_cast<void *>(1);
            ok &= (g_Pool->TimedAllocEx(&blk, max_req + 1U, WAIT_INFINITE) == WAIT_RESULT_FAIL);
            ok &= (blk == nullptr);

            ok &= (g_Pool->Alloc(max_req + 1U) == nullptr);
            ok &= (g_Pool->TimedAlloc(max_req + 1U, 500) == nullptr);

            const int64_t elapsed = GetTimeNowMs() - start;
            ok &= (elapsed < 20); // no waiting involved

            // the largest valid request must still succeed
            void *whole = g_Pool->TryAlloc(max_req);
            ok &= (whole != nullptr);
            if (whole != nullptr)
                g_Pool->Free(whole);

            ok &= IsAllFree();

            printf("InvalidRequest: elapsed=%d %s\n", (int)elapsed, ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 14 – Static helpers AlignSize / RequiredSize
// ---------------------------------------------------------------------------

/*! \class AlignHelpersTask
    \brief Verifies AlignSize() / RequiredSize() / HEADER_SIZE and the size accessors of the pool.
*/
template <EAccessMode _AccessMode>
class AlignHelpersTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    AlignHelpersTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            bool ok = true;

            typedef stk::memory::BytePool BP;

            ok &= (BP::HEADER_SIZE == 8U);

            ok &= (BP::AlignSize(0U)  == 0U);
            ok &= (BP::AlignSize(1U)  == 8U);
            ok &= (BP::AlignSize(8U)  == 8U);
            ok &= (BP::AlignSize(9U)  == 16U);
            ok &= (BP::AlignSize(24U) == 24U);

            ok &= (BP::RequiredSize(1U)  == 16U);
            ok &= (BP::RequiredSize(8U)  == 16U);
            ok &= (BP::RequiredSize(9U)  == 24U);
            ok &= (BP::RequiredSize(_STK_POOL_BLK_PAYLOAD) == (_STK_POOL_BLK_PAYLOAD + BP::HEADER_SIZE));

            ok &= (g_Pool->GetSize() == _STK_POOL_STORAGE_SIZE);
            ok &= (g_Pool->GetMaxRequest() == (_STK_POOL_STORAGE_SIZE - BP::HEADER_SIZE));

            printf("AlignHelpers: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 15 – Storage modes: external, heap, minimal and misaligned buffers
// ---------------------------------------------------------------------------

/*! \class StorageModeTask
    \brief Verifies accessors and basic operation for external storage, heap storage, the smallest
           valid external buffer, and an external buffer that is not 8-byte aligned.
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
            ok &= (g_Pool->GetSize() == _STK_POOL_STORAGE_SIZE);

            // Heap-storage pool
            const size_t heap_size = 256U;
            {
                stk::memory::BytePool heap_pool(heap_size);

                ok &= heap_pool.IsStorageValid();
                ok &= (heap_pool.GetSize() >= (heap_size - 7U)) && (heap_pool.GetSize() <= heap_size);

                void *blk = heap_pool.TryAlloc(100U);
                ok &= (blk != nullptr);
                if (blk != nullptr)
                {
                    memset(blk, 0x3C, 100U);
                    ok &= heap_pool.Free(blk);
                }

                size_t a, l, f;
                heap_pool.GetInfo(a, l, f);
                ok &= (f == 1U) && (a == heap_pool.GetMaxRequest()) && (l == a);
            }

            // Smallest valid external buffer: one header + 8 bytes payload
            alignas(8) static uint8_t tiny[16];
            {
                stk::memory::BytePool tiny_pool(tiny, sizeof(tiny));

                ok &= tiny_pool.IsStorageValid();
                ok &= (tiny_pool.GetMaxRequest() == 8U);

                void *blk = tiny_pool.TryAlloc(8U);
                ok &= (blk != nullptr);
                ok &= (tiny_pool.TryAlloc(1U) == nullptr); // nothing left
                if (blk != nullptr)
                    ok &= tiny_pool.Free(blk);
            }

            // Misaligned external buffer: the unusable head/tail bytes are trimmed
            alignas(8) static uint8_t raw[72];
            {
                stk::memory::BytePool mis_pool(raw + 1, (sizeof(raw) - 1U)); // 71 bytes, start is off by one

                ok &= mis_pool.IsStorageValid();
                ok &= (mis_pool.GetSize() == 64U);       // 7 head bytes skipped, 64 bytes usable
                ok &= (mis_pool.GetMaxRequest() == 56U);

                void *blk = mis_pool.TryAlloc(56U);
                ok &= (blk != nullptr);
                ok &= IsAligned8(blk);
                ok &= (static_cast<uint8_t *>(blk) >= (raw + 8)) && (static_cast<uint8_t *>(blk) < (raw + sizeof(raw)));
                if (blk != nullptr)
                {
                    memset(blk, 0x77, 56U);
                    ok &= mis_pool.Free(blk);
                }
            }

            printf("StorageMode: %s\n", ok ? "PASS" : "FAIL");
            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 16 – Stress test: mixed TryAlloc / Alloc / TimedAlloc, varying sizes
// ---------------------------------------------------------------------------

/*! \class StressTask
    \brief All tasks hammer the pool with a mix of TryAlloc, blocking Alloc,
           and TimedAlloc operations of varying sizes; verifies the pool never leaks,
           corrupts data or deadlocks.
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
            const size_t size = 8U + (((size_t)m_task_id * 17U + (size_t)i * 13U) % 57U); // 8..64 bytes
            uint8_t *blk = nullptr;

            switch (i % 3)
            {
                case 0: blk = static_cast<uint8_t *>(g_Pool->TryAlloc(size));       break;
                case 1: blk = static_cast<uint8_t *>(g_Pool->Alloc(size));          break;
                case 2: blk = static_cast<uint8_t *>(g_Pool->TimedAlloc(size, 20)); break;
                default: break;
            }

            if (blk)
            {
                const uint8_t pat = (uint8_t)(m_task_id + i);

                memset(blk, pat, size);

                if ((i % 4) == 0)
                    stk::Yield();

                for (size_t k = 0; k < size; ++k)
                {
                    if (blk[k] != pat)
                    {
                        ++g_Errors;
                        break;
                    }
                }

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
            bool ok = (g_SharedCounter > 0) && (g_Errors == 0) && IsAllFree();

            printf("Stress: counter=%d errors=%d pool_free=%s %s\n",
                (int)g_SharedCounter, (int)g_Errors,
                IsAllFree() ? "yes" : "no",
                ok ? "PASS" : "FAIL");

            if (ok) g_TestResult = 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Tests 17+ – Cancellation of blocked allocations
// ---------------------------------------------------------------------------

static const int32_t CA_INFINITE        = 1;
static const int32_t CA_NOT_TIMEOUT     = 2;
static const int32_t CA_PLAIN_ALLOC     = 3;
static const int32_t CA_NO_STALE_WAITER = 4;
static const int32_t CA_TWO_WAITERS     = 5;
static const int32_t CA_SIGNAL_WINS     = 6;
static const int32_t CA_VIA_SLEEPCANCEL = 100;

/*! \class CancelAllocTask
    \brief Tests cancelability of blocked BytePool allocations (IKernel::CancelTaskWait()
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
            blk = g_Pool->Alloc(_STK_POOL_BLK_PAYLOAD);
            r   = (blk != nullptr ? WAIT_RESULT_SIGNAL : WAIT_RESULT_CANCELED);
        }
        else
        {
            const Timeout timeout = (m_scenario == CA_NOT_TIMEOUT ? 300 : WAIT_INFINITE);
            r = g_Pool->TimedAllocEx(&blk, _STK_POOL_BLK_PAYLOAD, timeout);
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
        const EWaitResult r = g_Pool->TimedAllocEx(&blk, _STK_POOL_BLK_PAYLOAD, WAIT_INFINITE);

        g_VictimElapsed[1]  = GetTimeNowMs() - start;
        g_VictimResult[1]   = static_cast<int32_t>(r);
        g_VictimGotBlock[1] = (blk != nullptr);

        if (blk != nullptr)
            g_Pool->Free(blk);
    }

    // Task 0: drain the pool, cancel and verify
    void RunCanceller()
    {
        bool pass = false;
        void *blocks[_STK_POOL_BLK_COUNT] = {};

        // drain the pool so that the victim (and task 2) block
        DrainPool(blocks);

        stk::Sleep(60); // task 1 (and task 2) are blocked in the allocation by now

        if (m_scenario == CA_INFINITE)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

            // started waiting at ~10 ms, cancelled at ~60 ms; nothing was allocated by the cancel
            pass = (g_VictimResult[0] == WAIT_RESULT_CANCELED) && !g_VictimGotBlock[0] &&
                   (g_VictimElapsed[0] >= 35) && (g_VictimElapsed[0] <= 75) &&
                   IsExhausted();
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
                   IsExhausted();
        }
        else
        if (m_scenario == CA_NO_STALE_WAITER)
        {
            CancelVictim(m_use_sleep_cancel);
            stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP); // victim returns and starts its Sleep(60)

            // nobody waits any more: Free() must just release the block, with no waiter left to wake
            const bool freed = g_Pool->Free(blocks[0]);
            blocks[0] = nullptr;

            const bool used_ok = (UsedBlocks() == (_STK_POOL_BLK_COUNT - 1U));

            // the freed memory must be allocatable again, exactly once
            blocks[0] = g_Pool->TryAlloc(_STK_POOL_BLK_PAYLOAD);
            const bool again_ok = (blocks[0] != nullptr);
            const bool extra_ok = (g_Pool->TryAlloc(_STK_POOL_BLK_PAYLOAD) == nullptr);

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
            const bool used_ok = (UsedBlocks() == (_STK_POOL_BLK_COUNT - 1U));

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
                   (UsedBlocks() == (_STK_POOL_BLK_COUNT - 1U));
        }

        // Release everything we still hold. This also acts as a safety net: if the cancel did not work a
        // waiter is still blocked forever and the kernel would never stop, so the freed blocks wake it
        // and the test fails instead of hanging.
        ReleaseAll(blocks);
        stk::Sleep(_STK_POOL_TEST_SHORT_SLEEP * 2);

        const bool leak_free = IsAllFree(); // all memory is back in the pool

        printf("cancel alloc (scenario=%d, via %s): result[0]=%d elapsed[0]=%d ms leak_free=%d\n",
            (int)m_scenario, m_use_sleep_cancel ? "SleepCancel" : "CancelTaskWait",
            (int)g_VictimResult[0], (int)g_VictimElapsed[0], (int)leak_free);

        if (pass && leak_free)
            g_TestResult = 1;
    }
};

// ---------------------------------------------------------------------------
// Test 17 – Diagnostics: GetStats / GetPeakUsedSize / GetAllocFailCount / GetMaxScanLength
// ---------------------------------------------------------------------------

/*! \class DiagnosticsTask
    \brief Verifies the O(1) statistics of the pool on a fresh instance: consumption (headers included),
           allocation/free counters, the high watermark and its derived min_ever_free, the failed-request
           counter (exhaustion, timeout and oversized requests count; zero-sized requests do not) and the
           block-list walk length.
*/
template <EAccessMode _AccessMode>
class DiagnosticsTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    DiagnosticsTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            typedef stk::memory::MemoryAllocator::Stats StatsT;

            bool ok = true;

            const size_t cap       = g_Pool->GetSize();
            const size_t blk_total = (_STK_POOL_BLK_PAYLOAD + stk::memory::BytePool::HEADER_SIZE);

            // fresh pool: every counter is zero, everything is available
            const StatsT s0 = g_Pool->GetStats();
            ok &= (cap == _STK_POOL_STORAGE_SIZE) && (s0.GetCapacity() == cap);
            ok &= (s0.allocated == 0U) && (s0.peak_allocated == 0U);
            ok &= (s0.allocate_count == 0U) && (s0.free_count == 0U) && (s0.alloc_fail_count == 0U);
            ok &= (s0.min_ever_free == cap) && (s0.GetAvailable() == cap);
            ok &= (g_Pool->GetPeakUsedSize() == 0U);
            ok &= (g_Pool->GetAllocFailCount() == 0U);
            ok &= (g_Pool->GetMaxScanLength() == 0U);

            // one allocation: consumption includes the header, the list walk is at least one block long
            void *one = g_Pool->TryAlloc(_STK_POOL_BLK_PAYLOAD);
            ok &= (one != nullptr);

            const StatsT s1 = g_Pool->GetStats();
            ok &= (s1.allocated == blk_total) && (s1.peak_allocated == blk_total);
            ok &= (s1.allocate_count == 1U) && (s1.free_count == 0U);
            ok &= (s1.GetAvailable() == (cap - blk_total));
            ok &= ((s1.allocated + FreeSpace()) == cap); // every byte belongs either to a used or to a free block
            ok &= (g_Pool->GetMaxScanLength() >= 1U);

            ok &= g_Pool->Free(one);

            // after the free the consumption drops, but the watermark stays
            const StatsT s2 = g_Pool->GetStats();
            ok &= (s2.allocated == 0U) && (s2.free_count == 1U) && (s2.allocate_count == 1U);
            ok &= (s2.peak_allocated == blk_total) && (s2.min_ever_free == (cap - blk_total));
            ok &= (g_Pool->GetPeakUsedSize() == blk_total);

            // exhaust the pool: the watermark reaches the capacity, the 8th allocation walked all blocks
            void *blocks[_STK_POOL_BLK_COUNT] = {};
            DrainPool(blocks);

            const StatsT s3 = g_Pool->GetStats();
            ok &= (s3.allocated == cap) && (s3.peak_allocated == cap) && (s3.GetAvailable() == 0U);
            ok &= (s3.allocate_count == (1U + _STK_POOL_BLK_COUNT)) && (s3.min_ever_free == 0U);
            ok &= (g_Pool->GetPeakUsedSize() == cap);
            ok &= (g_Pool->GetMaxScanLength() >= _STK_POOL_BLK_COUNT);
            ok &= (s3.alloc_fail_count == 0U) && (g_Pool->GetAllocFailCount() == 0U);

            // failed requests: exhaustion (NO_WAIT), oversized request and an expired timeout are counted ...
            ok &= (g_Pool->TryAlloc(_STK_POOL_BLK_PAYLOAD) == nullptr);
            ok &= (g_Pool->GetAllocFailCount() == 1U);

            ok &= (g_Pool->TryAlloc(g_Pool->GetMaxRequest() + 1U) == nullptr);
            ok &= (g_Pool->GetAllocFailCount() == 2U);

            void *blk = nullptr;
            const int64_t start = GetTimeNowMs();
            ok &= (g_Pool->TimedAllocEx(&blk, _STK_POOL_BLK_PAYLOAD, _STK_POOL_TEST_SHORT_SLEEP) == WAIT_RESULT_TIMEOUT);
            const int64_t elapsed = GetTimeNowMs() - start;
            ok &= (blk == nullptr) && (elapsed >= (_STK_POOL_TEST_SHORT_SLEEP / 2));
            ok &= (g_Pool->GetAllocFailCount() == 3U);

            // ... a zero-sized request is an argument error, not an exhaustion
            ok &= (g_Pool->TryAlloc(0U) == nullptr);
            ok &= (g_Pool->GetAllocFailCount() == 3U);

            ok &= (g_Pool->GetStats().alloc_fail_count == 3U); // the snapshot reports the same counter
            ok &= (g_Pool->GetMaxScanLength() >= _STK_POOL_BLK_COUNT);

            // release everything: consumption returns to zero, history (watermark, counters) is kept
            ReleaseAll(blocks);

            const StatsT s4 = g_Pool->GetStats();
            ok &= (s4.allocated == 0U) && (s4.peak_allocated == cap) && (s4.min_ever_free == 0U);
            ok &= (s4.allocate_count == (1U + _STK_POOL_BLK_COUNT)) && (s4.free_count == (1U + _STK_POOL_BLK_COUNT));
            ok &= (s4.alloc_fail_count == 3U);
            ok &= IsAllFree();

            printf("Diagnostics: peak=%d fails=%d max_scan=%d %s\n",
                (int)g_Pool->GetPeakUsedSize(), (int)g_Pool->GetAllocFailCount(),
                (int)g_Pool->GetMaxScanLength(), ok ? "PASS" : "FAIL");

            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 18 – Object helpers: TryNewT / NewT / TimedNewT / DeleteT / TryMakeT / MakeT / PoolPtr
// ---------------------------------------------------------------------------

/*! \brief Object with observable construction/destruction used by ObjectApiTask. */
struct TrackedObj
{
    static const uint32_t CANARY = 0xC0DEC0DEU;

    uint32_t a;
    uint32_t b;
    uint32_t canary;

    TrackedObj(uint32_t x = 0x1111U, uint32_t y = 0x2222U);
    ~TrackedObj();
};

static volatile int32_t g_ObjCtors         = 0;
static volatile int32_t g_ObjDtors         = 0;
static volatile bool    g_ObjDtorSawCanary = true; // false if a destructor ever ran on a damaged object

TrackedObj::TrackedObj(uint32_t x, uint32_t y) : a(x), b(y), canary(CANARY)
{
    ++g_ObjCtors;
}

TrackedObj::~TrackedObj()
{
    if (canary != CANARY)
        g_ObjDtorSawCanary = false;

    canary = 0U;
    ++g_ObjDtors;
}

/*! \class ObjectApiTask
    \brief Verifies construction in place with argument forwarding (TryNewT / NewT / TimedNewT), that nothing is
           constructed when the allocation fails, that DeleteT() runs the destructor before the block is returned,
           that trivially constructible types still receive their arguments, and the owning PoolPtr returned by
           TryMakeT() / MakeT(): scope exit, move construction/assignment, Release(), Reset() and empty pointers.
*/
template <EAccessMode _AccessMode>
class ObjectApiTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    ObjectApiTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            typedef stk::memory::PoolPtr<TrackedObj, stk::memory::BytePool> ObjPtr;

            bool ok = true;

            g_ObjCtors         = 0;
            g_ObjDtors         = 0;
            g_ObjDtorSawCanary = true;

            // ---- TryNewT: constructed in place with forwarded arguments
            TrackedObj *o1 = g_Pool->TryNewT<TrackedObj>(0x11U, 0x22U);
            ok &= (o1 != nullptr);
            if (o1 != nullptr)
            {
                ok &= IsAligned8(o1);
                ok &= (o1->a == 0x11U) && (o1->b == 0x22U) && (o1->canary == TrackedObj::CANARY);
            }
            ok &= (g_ObjCtors == 1) && (g_ObjDtors == 0);
            ok &= !IsAllFree();

            // ---- DeleteT: destructor runs on the intact object, then the block is returned
            g_Pool->DeleteT(o1);
            ok &= (g_ObjDtors == 1) && g_ObjDtorSawCanary;
            ok &= IsAllFree();

            // ---- DeleteT(nullptr) is a no-op
            g_Pool->DeleteT<TrackedObj>(nullptr);
            ok &= (g_ObjDtors == 1) && IsAllFree();

            // ---- NewT (default arguments) and TimedNewT (explicit arguments)
            TrackedObj *o2 = g_Pool->NewT<TrackedObj>();
            TrackedObj *o3 = g_Pool->TimedNewT<TrackedObj>(_STK_POOL_TEST_TIMEOUT, 7U, 8U);
            ok &= (o2 != nullptr) && (o3 != nullptr) && (o2 != o3);
            if ((o2 != nullptr) && (o3 != nullptr))
            {
                ok &= (o2->a == 0x1111U) && (o2->b == 0x2222U);
                ok &= (o3->a == 7U) && (o3->b == 8U);
            }
            ok &= (g_ObjCtors == 3);
            g_Pool->DeleteT(o2);
            g_Pool->DeleteT(o3);
            ok &= (g_ObjDtors == 3) && g_ObjDtorSawCanary && IsAllFree();

            // ---- trivially constructible types must still be constructed from the arguments
            uint32_t *pv = g_Pool->TryNewT<uint32_t>(0xC0FFEEU);
            ok &= (pv != nullptr) && (*pv == 0xC0FFEEU);
            g_Pool->DeleteT(pv);

            const TestRecord src = { 5U, 6U };
            TestRecord *pr = g_Pool->TryNewT<TestRecord>(src);
            ok &= (pr != nullptr) && (pr->id == 5U) && (pr->value == 6U);
            g_Pool->DeleteT(pr);

            // ... and without arguments the object is value-initialised (zeroed), not left with stale bytes
            void *dirty = g_Pool->TryAlloc(sizeof(TestRecord));
            ok &= (dirty != nullptr);
            if (dirty != nullptr)
            {
                memset(dirty, 0xFF, sizeof(TestRecord));
                g_Pool->Free(dirty);
            }
            TestRecord *pz = g_Pool->TryNewT<TestRecord>();
            ok &= (pz != nullptr) && (pz->id == 0U) && (pz->value == 0U);
            g_Pool->DeleteT(pz);
            ok &= IsAllFree();

            // ---- failed allocation constructs nothing
            {
                void *blocks[_STK_POOL_BLK_COUNT] = {};
                DrainPool(blocks);

                const int32_t ctors = g_ObjCtors;
                ok &= (g_Pool->TryNewT<TrackedObj>() == nullptr);
                ok &= (g_Pool->TimedNewT<TrackedObj>(_STK_POOL_TEST_SHORT_SLEEP, 1U, 2U) == nullptr);
                ok &= (g_ObjCtors == ctors);

                ObjPtr empty = g_Pool->TryMakeT<TrackedObj>();
                ok &= !empty && (empty.Get() == nullptr);
                empty.Reset(); // resetting an empty pointer is harmless
                ok &= (g_ObjCtors == ctors);

                ReleaseAll(blocks);
                ok &= IsAllFree();
            }

            // ---- PoolPtr: ownership, moves, Release(), Reset(), scope exit
            g_ObjCtors = 0;
            g_ObjDtors = 0;
            {
                ObjPtr p = g_Pool->TryMakeT<TrackedObj>(1U, 2U);
                ok &= static_cast<bool>(p) && (p.Get() != nullptr) && (p->a == 1U) && ((*p).b == 2U);
                ok &= (g_ObjCtors == 1) && (g_ObjDtors == 0);

                // move construction transfers ownership, nothing is destroyed
                ObjPtr q(static_cast<ObjPtr &&>(p));
                ok &= !p && static_cast<bool>(q) && (q->a == 1U);

                // move assignment into an empty pointer
                ObjPtr r;
                ok &= !r;
                r = static_cast<ObjPtr &&>(q);
                ok &= !q && static_cast<bool>(r) && (r->b == 2U);
                ok &= (g_ObjDtors == 0);

                // move assignment over a live object destroys the old one first
                ObjPtr s = g_Pool->MakeT<TrackedObj>(3U, 4U);
                ok &= static_cast<bool>(s) && (s->a == 3U) && (g_ObjCtors == 2);
                s = static_cast<ObjPtr &&>(r);
                ok &= (g_ObjDtors == 1) && static_cast<bool>(s) && (s->a == 1U) && !r;

                // Release(): ownership goes back to the caller, nothing is destroyed
                TrackedObj *raw = s.Release();
                ok &= !s && (raw != nullptr) && (g_ObjDtors == 1);
                g_Pool->DeleteT(raw);
                ok &= (g_ObjDtors == 2);

                // Reset() destroys at once, a second Reset() is a no-op
                ObjPtr t = g_Pool->TryMakeT<TrackedObj>();
                ok &= static_cast<bool>(t) && (g_ObjCtors == 3);
                t.Reset();
                ok &= !t && (g_ObjDtors == 3);
                t.Reset();
                ok &= (g_ObjDtors == 3);
            }
            ok &= (g_ObjCtors == g_ObjDtors) && IsAllFree();

            // leaving the scope returns the object to the pool
            {
                ObjPtr u = g_Pool->TryMakeT<TrackedObj>(9U, 9U);
                ok &= static_cast<bool>(u) && !IsAllFree() && (g_ObjDtors == 3);
            }
            ok &= (g_ObjCtors == 4) && (g_ObjDtors == 4) && g_ObjDtorSawCanary && IsAllFree();

            printf("ObjectApi: ctors=%d dtors=%d %s\n", (int)g_ObjCtors, (int)g_ObjDtors, ok ? "PASS" : "FAIL");

            if (ok)
                g_TestResult = 1;
        }

        ++g_InstancesDone;
    }
};

// ---------------------------------------------------------------------------
// Test 19 – StaticBytePool: pool with embedded storage
// ---------------------------------------------------------------------------

// Namespace-scope instance, as recommended for global pools: no heap, no separately declared buffer
static stk::memory::StaticBytePool<256U> g_StaticPool("static");

/*! \class StaticPoolTask
    \brief Verifies that StaticBytePool is usable as a regular BytePool with its own embedded, correctly
           aligned storage (nothing trimmed, nothing shared with the test pool) including the object helpers.
*/
template <EAccessMode _AccessMode>
class StaticPoolTask : public Task<_STK_POOL_STACK_SIZE, _AccessMode>
{
    uint8_t m_task_id;

public:
    StaticPoolTask(uint8_t task_id, int32_t) : m_task_id(task_id)
    {}

private:
    void Run() override
    {
        if (m_task_id == 0)
        {
            typedef stk::memory::BytePool BP;

            bool ok = true;

            BP &sp = g_StaticPool;

            ok &= sp.IsStorageValid();
            ok &= (sp.GetSize() == 256U); // the embedded buffer is aligned by construction: nothing is trimmed
            ok &= (sp.GetMaxRequest() == (256U - BP::HEADER_SIZE));

            // the largest request spans the whole pool; the pool is then exhausted
            void *whole = sp.TryAlloc(sp.GetMaxRequest());
            ok &= (whole != nullptr);
            if (whole != nullptr)
            {
                ok &= IsAligned8(whole);
                ok &= (sp.TryAlloc(1U) == nullptr);

                // the storage is private to this pool, the shared test pool is not involved
                const uint8_t *p = static_cast<const uint8_t *>(whole);
                ok &= (p < g_PoolStorage) || (p >= (g_PoolStorage + sizeof(g_PoolStorage)));

                memset(whole, 0x5A, sp.GetMaxRequest());
                ok &= sp.Free(whole);
            }

            size_t a, l, f;
            sp.GetInfo(a, l, f);
            ok &= (f == 1U) && (a == sp.GetMaxRequest()) && (l == a);

            // object helpers work through the derived class
            g_ObjCtors = 0;
            g_ObjDtors = 0;
            {
                stk::memory::PoolPtr<TrackedObj, BP> obj = sp.TryMakeT<TrackedObj>(21U, 42U);
                ok &= static_cast<bool>(obj) && (obj->a == 21U) && (obj->b == 42U) && IsAligned8(obj.Get());
                ok &= (sp.GetStats().allocated > 0U);
            }
            ok &= (g_ObjCtors == 1) && (g_ObjDtors == 1);
            ok &= (sp.GetStats().allocated == 0U);
            ok &= (sp.GetStats().GetCapacity() == 256U);

            sp.GetInfo(a, l, f);
            ok &= (f == 1U) && (a == sp.GetMaxRequest());

            ok &= IsAllFree(); // shared pool untouched

            printf("StaticPool: %s\n", ok ? "PASS" : "FAIL");
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
    g_Errors        = 0;

    g_Victim             = nullptr;
    g_VictimTid          = TID_NONE;
    g_VictimSleepElapsed = 0;

    for (int32_t i = 0; i < 2; ++i)
    {
        g_VictimResult[i]   = -100;
        g_VictimGotBlock[i] = false;
        g_VictimElapsed[i]  = 0;
        g_WaiterDone[i]     = false;
    }
}

} // namespace bytepool
} // namespace test
} // namespace stk

// ---------------------------------------------------------------------------
// Task-count predicates
// ---------------------------------------------------------------------------

/*! \fn    NeedsTwoTasks
    \brief Returns true if the test requires tasks 0 and 1.
    \note  Single-task tests (TryAllocFree, ExhaustPool, SplitCoalesce, TypedAlloc, FreeNull,
           InvalidRequest, AlignHelpers, StorageMode, Diagnostics, ObjectApi, StaticPool) use only task 0.
*/
static bool NeedsTwoTasks(const char *test_name)
{
    return (strcmp(test_name, "BlockingAlloc")       == 0) ||
           (strcmp(test_name, "BlockingLarge")       == 0) ||
           (strcmp(test_name, "WakeAllWaiters")      == 0) ||
           (strcmp(test_name, "TimedAllocTimeout")   == 0) ||
           (strcmp(test_name, "TimedAllocSuccess")   == 0) ||
           (strcmp(test_name, "TimedAllocDeadline")  == 0) ||
           (strcmp(test_name, "ConcurrentAllocFree") == 0) ||
           (strcmp(test_name, "Stress")              == 0) ||
           (strncmp(test_name, "CancelAlloc", 11)    == 0);
}

/*! \fn    NeedsThreeTasks
    \brief Returns true if the test requires tasks 0, 1 and 2 (but not 3-4).
    \note  WakeAllWaiters: releaser plus a large and a small waiter.
           Cancellation tests: canceller, victim and a second waiter.
*/
static bool NeedsThreeTasks(const char *test_name)
{
    return (strcmp(test_name, "WakeAllWaiters") == 0) ||
           (strncmp(test_name, "CancelAlloc", 11) == 0);
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
    \brief Helper function to run a single bytepool test case.
*/
template <class TaskType>
static int32_t RunTest(const char *test_name, int32_t param = 0)
{
    using namespace stk;
    using namespace stk::test;
    using namespace stk::test::bytepool;

    printf("Test: %s\n", test_name);

    ResetTestState();

    stk::memory::BytePool pool(
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
    \brief Entry point for the BytePool test suite.
*/
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    TestContext::ShowTestSuitePrologue();

    int total_failures = 0, total_success = 0;

    printf("--------------\n");

    stk::test::bytepool::g_Kernel.Initialize();

#define RUN(TestClass, name, param) \
    do { \
        if (RunTest<TestClass<ACCESS_PRIVILEGED>>(name, param) \
                != TestContext::SUCCESS_EXIT_CODE) \
            total_failures++; \
        else \
            total_success++; \
    } while (0)

#ifndef __ARM_ARCH_6M__

    // Test 1: TryAlloc / Free basic cycle, GetInfo accounting
    RUN(stk::test::bytepool::TryAllocFreeTask,        "TryAllocFree",        0);

    // Test 2: Exhaust pool, verify exhaustion, free all, lazy merge
    RUN(stk::test::bytepool::ExhaustPoolTask,         "ExhaustPool",         0);

    // Test 3: First-fit placement, splitting, coalescing, no split of tiny remainders
    RUN(stk::test::bytepool::SplitCoalesceTask,       "SplitCoalesce",       0);

    // Test 4: Blocking Alloc unblocked by Free from another task
    RUN(stk::test::bytepool::BlockingAllocTask,       "BlockingAlloc",       0);

    // Test 5: Blocked large request keeps waiting until adjacent memory is free
    RUN(stk::test::bytepool::BlockingLargeTask,       "BlockingLarge",       0);

    // Test 6: Free wakes all waiters, each re-checks its own request size
    RUN(stk::test::bytepool::WakeAllWaitersTask,      "WakeAllWaiters",      0);

    // Test 7: TimedAlloc expires when memory stays unavailable
    RUN(stk::test::bytepool::TimedAllocTimeoutTask,   "TimedAllocTimeout",   0);

    // Test 8: TimedAlloc succeeds when memory freed within timeout
    RUN(stk::test::bytepool::TimedAllocSuccessTask,   "TimedAllocSuccess",   0);

    // Test 9: TimedAlloc deadline is not restarted by unsuccessful wake-ups
    RUN(stk::test::bytepool::TimedAllocDeadlineTask,  "TimedAllocDeadline",  0);

    // Test 10: Concurrent alloc/free of varying sizes, data integrity (20 iterations per task)
    RUN(stk::test::bytepool::ConcurrentAllocFreeTask, "ConcurrentAllocFree", 20);

    // Test 11: Typed helpers AllocT / TryAllocT / TimedAllocT
    RUN(stk::test::bytepool::TypedAllocTask,          "TypedAlloc",          0);

    // Test 12: Free(nullptr) is a safe no-op
    RUN(stk::test::bytepool::FreeNullTask,            "FreeNull",            0);

    // Test 13: Zero-sized and oversized requests fail immediately
    RUN(stk::test::bytepool::InvalidRequestTask,      "InvalidRequest",      0);

    // Test 14: AlignSize / RequiredSize static helpers
    RUN(stk::test::bytepool::AlignHelpersTask,        "AlignHelpers",        0);

    // Test 15: External, heap, minimal and misaligned storage
    RUN(stk::test::bytepool::StorageModeTask,         "StorageMode",         0);

    // Test 17: GetStats / GetPeakUsedSize / GetAllocFailCount / GetMaxScanLength
    RUN(stk::test::bytepool::DiagnosticsTask,         "Diagnostics",         0);

    // Test 18: TryNewT / NewT / TimedNewT / DeleteT / TryMakeT / MakeT / PoolPtr
    RUN(stk::test::bytepool::ObjectApiTask,           "ObjectApi",           0);

    // Test 19: StaticBytePool (embedded storage)
    RUN(stk::test::bytepool::StaticPoolTask,          "StaticPool",          0);

    // Cancellation of blocked allocations (tasks 0-2 only), via IKernel::CancelTaskWait()
    // and via stk::SleepCancel() (param + 100)
    {
        using namespace stk::test::bytepool;

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

#endif // __ARM_ARCH_6M__

    // Test 16: Stress test (ARM Cortex-M0 compatible)
    RUN(stk::test::bytepool::StressTask, "Stress", 200);

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
