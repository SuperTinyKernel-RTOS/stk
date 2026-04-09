# BlockMemoryPool Test Suite

Test suite for `stk::memory::BlockMemoryPool`.  
Source: `test/blockpool/test_blockpool.cpp`

---

## API Summary

```cpp
#include <memory/stk_memory_blockpool.h>
```

`BlockMemoryPool` is a fixed-size block allocator providing O(1) alloc and free with
proper task-blocking semantics. It uses an intrusive singly-linked free-list overlaid
on the storage array — when a block is free, its first `sizeof(void*)` bytes hold a
pointer to the next free block; no separate metadata array is needed.

**Two storage modes** are supported:

| Mode | Constructor | Frees storage? |
|------|-------------|----------------|
| External storage | `BlockMemoryPool(cap, blksz, buf, bufsz [, name])` | No — caller owns buffer |
| Heap storage | `BlockMemoryPool(cap, blksz [, name])` | Yes — freed in destructor |

**Blocking semantics**: `TimedAlloc()` suspends the calling task via an internal
`ConditionVariable` until `Free()` returns a block or the timeout expires. `TryAlloc()`
and `Free()` are ISR-safe; `Alloc()` and `TimedAlloc()` with a non-zero timeout must
only be called from task context.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// External-storage construction (zero-heap, static buffer)
alignas(sizeof(void *)) static uint8_t
    g_Buf[N * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(MyType))];

stk::memory::BlockMemoryPool g_Pool(N, sizeof(MyType), g_Buf, sizeof(g_Buf));

// Heap-storage construction
stk::memory::BlockMemoryPool g_Pool(N, sizeof(MyType));
// Always check after heap construction:
assert(g_Pool.IsStorageValid());
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Alloc` | `void *Alloc()` | Blocks indefinitely until a block is available. Never returns `nullptr`. ISR-unsafe. |
| `AllocT<T>` | `T *AllocT<T>()` | Typed wrapper around `Alloc()`. ISR-unsafe. |
| `TryAlloc` | `void *TryAlloc()` | Non-blocking; returns `nullptr` immediately if the pool is empty. ISR-safe. |
| `TryAllocT<T>` | `T *TryAllocT<T>()` | Typed wrapper around `TryAlloc()`. ISR-safe. |
| `TimedAlloc` | `void *TimedAlloc(Timeout timeout = WAIT_INFINITE)` | Blocks up to `timeout` ticks. `NO_WAIT` is identical to `TryAlloc()`. Returns `nullptr` on timeout. ISR-safe only when `timeout == NO_WAIT`. |
| `TimedAllocT<T>` | `T *TimedAllocT<T>(Timeout timeout = WAIT_INFINITE)` | Typed wrapper around `TimedAlloc()`. Asserts `sizeof(T) <= GetBlockSize()`. |
| `Free` | `bool Free(void *ptr)` | Returns a block to the free-list in O(1) and wakes one blocked allocator. Returns `false` for `nullptr`, out-of-range, or misaligned pointers. ISR-safe. |
| `IsStorageValid` | `bool IsStorageValid() const` | `true` if backing storage is initialised. For heap pools, `false` if `operator new` failed. |
| `GetCapacity` | `size_t GetCapacity() const` | Total number of blocks in the pool. |
| `GetBlockSize` | `size_t GetBlockSize() const` | Aligned block size in bytes (`>= BLOCK_ALIGN`). |
| `GetUsedCount` | `size_t GetUsedCount() const` | Number of currently allocated (outstanding) blocks. |
| `GetFreeCount` | `size_t GetFreeCount() const` | Number of available blocks (`GetCapacity() - GetUsedCount()`). |
| `IsFull` | `bool IsFull() const` | `true` when no blocks are available. |
| `IsEmpty` | `bool IsEmpty() const` | `true` when no blocks are outstanding. |
| `AlignBlockSize` | `static size_t AlignBlockSize(size_t raw_size)` | Rounds `raw_size` up to the nearest multiple of `BLOCK_ALIGN` (= `sizeof(void*)`), with a minimum of `BLOCK_ALIGN`. Use to size external storage buffers. |

**`TimedAlloc()` paths:**

```
pool not empty        →  pop free-list head, return block immediately (O(1))
pool empty, NO_WAIT   →  return nullptr immediately (TryAlloc fast-path)
pool empty, timeout   →  suspend via ConditionVariable; wake when Free() signals,
                          or return nullptr if timeout expires first
```

**`Free()` path:**

```
ptr == nullptr        →  return false (safe no-op)
ptr out of range      →  assert + return false
ptr misaligned        →  assert + return false
used_count == 0       →  assert + return false (double-free guard)
otherwise             →  push to free-list head (O(1)), NotifyOne() to wake one waiter
```

**Key invariants:**

- Every `Alloc()` / `TryAlloc()` / `TimedAlloc()` that returns non-`nullptr` must be
  paired with exactly one `Free()` call.
- `Free()` validates bounds and alignment in all builds; double-free is additionally
  detected in debug builds with an O(n) free-list walk.
- `GetBlockSize()` always equals `AlignBlockSize(raw_block_size)` passed at construction.
- `GetFreeCount() + GetUsedCount() == GetCapacity()` at all times.
- Destroying a pool while tasks are blocked in `Alloc()` or `TimedAlloc()` is a logic
  error; an assertion fires in debug builds inside the `ConditionVariable` destructor.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_POOL_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_POOL_TEST_TIMEOUT` | `1000` ticks | Reference blocking timeout |
| `_STK_POOL_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_POOL_TEST_LONG_SLEEP` | `100` ticks | Sleep used by verifier tasks to wait for workers |
| `_STK_POOL_BLOCK_SIZE` | `32` bytes | Raw block size passed to each pool constructor |
| `_STK_POOL_CAPACITY` | `8` blocks | Pool capacity used in all tests |
| `_STK_POOL_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

A fresh `BlockMemoryPool` instance backed by the static `g_PoolStorage` buffer is
constructed at the start of every `RunTest()` call and destroyed at the end. This
ensures every test starts with a fully empty pool in a known state. `ResetTestState()`
resets `g_TestResult`, `g_InstancesDone`, and `g_SharedCounter` between tests.

All eleven tests add all five tasks (0–4) unconditionally — there is no
`NeedsExtendedTasks` gating in this suite.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link ten
distinct task class templates simultaneously. Tests 1–10 are skipped on M0 and only
`Stress` (test 11) runs, under `#ifndef __ARM_ARCH_6M__`.

`StressTask` runs on M0 because it uses a single task class template instantiated for
all five task slots, fitting within the available memory.

| Platform | `_STK_POOL_STACK_SIZE` | Tests run |
|----------|------------------------|-----------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words | Test 11 only |
| All others | `256` words | Tests 1–11 |

---

## Tests

### Test 1 — `TryAllocFree`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 verifies the pool accounting from construction through a full alloc/free cycle.
It checks `IsEmpty()`, `GetFreeCount() == 8`, and `GetUsedCount() == 0` on entry, then
calls `TryAlloc()` and confirms the returned pointer is non-`nullptr`,
`GetUsedCount() == 1`, and `GetFreeCount() == 7`. It writes `0xAB` across the full
block to confirm it is writable memory, then calls `Free()` and re-checks `IsEmpty()`.
A second `TryAlloc()` / `Free()` pair verifies the recycled block is available again
immediately after return.

**Pass condition:** all accounting checks pass across both alloc/free cycles

---

### Test 2 — `ExhaustPool`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 calls `TryAlloc()` in a loop until all 8 blocks are outstanding, confirming each
call returns a non-`nullptr` pointer. It then asserts `IsFull() == true`,
`GetFreeCount() == 0`, and `GetUsedCount() == 8`. A ninth `TryAlloc()` call must return
`nullptr` immediately without blocking. All 8 blocks are then freed and `IsEmpty()` is
verified.

**Pass condition:** all 8 `TryAlloc()` calls succeed; the 9th returns `nullptr`; `IsEmpty()` after freeing all

---

### Test 3 — `BlockingAlloc`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 0 drains all 8 blocks via `TryAlloc()` to force task 1 into a blocking path, then
sleeps `2 × SHORT_SLEEP` ticks to allow task 1 to enter `Alloc()` and suspend. Task 0
then frees one block — this must wake task 1 via `ConditionVariable::NotifyOne()`. Task 1
receives the block from its `Alloc()` call, sets `g_SharedCounter = 1`, and frees the
block. Task 0 waits on a `g_InstancesDone` completion barrier then verifies both that
`g_SharedCounter == 1` (task 1 unblocked successfully) and `IsEmpty()` (no outstanding
blocks).

**Pass condition:** `g_SharedCounter == 1` and `pool.IsEmpty()`

---

### Test 4 — `TimedAllocTimeout`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 0 fills the pool and holds all 8 blocks for 200 ticks. Task 1 sleeps
`SHORT_SLEEP` to let task 0 establish ownership, then calls `TimedAlloc(50)` and
measures elapsed time. Since the pool remains full for the entire 50-tick window,
`TimedAlloc()` must return `nullptr` with elapsed time in `[45, 65]` ms. `g_TestResult`
is set directly inside task 1's branch.

**Pass condition:** `TimedAlloc(50)` returned `nullptr` and elapsed in `[45, 65]` ms

---

### Test 5 — `TimedAllocSuccess`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 0 fills the pool then frees one block after 40 ticks — before task 1's 150-tick
timeout expires. Task 1 sleeps `SHORT_SLEEP`, then calls `TimedAlloc(150)`. Since task 0
releases a block within the timeout window, `TimedAlloc()` must return a valid non-`nullptr`
pointer. Task 1 frees the acquired block and sets `g_TestResult` on success.

**Pass condition:** `TimedAlloc(150)` returned non-`nullptr`

---

### Test 6 — `ConcurrentAllocFree`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Param:** `iterations = 20`

All five tasks compete for pool blocks for 20 iterations each using the blocking
`AllocT<int32_t>()`. Each task writes `1` into the allocated block, accumulates it
into `g_SharedCounter`, then frees the block and calls `Yield()`. Because the pool has
8 blocks and only 5 tasks, every `Alloc()` is guaranteed to succeed (no task can
exhaust the pool alone). The `g_SharedCounter` increment is intentionally
unsynchronised — any pool corruption would cause tasks to read stale or overlapping
data, producing an incorrect total. Task 0 uses a `g_InstancesDone` completion barrier.

**Pass condition:** `counter == 100` (`5 tasks × 20 iterations`)

---

### Test 7 — `TypedAlloc`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 exercises all three typed allocation wrappers against a `TestRecord` struct
(`{uint32_t id; uint32_t value;}`). It calls `TryAllocT<TestRecord>()`, writes
`id = 42` and `value = 0xDEADBEEF`, reads them back to confirm the fields are
preserved, and frees the block. It then repeats the cycle with `TimedAllocT<TestRecord>
(WAIT_INFINITE)` and `AllocT<TestRecord>()`, verifying each returns a non-`nullptr`
pointer. All three wrappers must return correctly typed pointers without asserting.

**Pass condition:** all three typed wrappers return non-`nullptr`; field values survive until `Free()`

---

### Test 8 — `FreeNull`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 records `GetFreeCount()` before calling `Free(nullptr)`. The call must return
`false` (the `nullptr` guard in the `Free()` implementation) and must leave the pool
in an identical state — `GetFreeCount()` unchanged, no crash, no assertion in release
builds.

**Pass condition:** `Free(nullptr) == false` and `GetFreeCount()` unchanged

---

### Test 9 — `AlignBlockSize`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 exercises `BlockMemoryPool::AlignBlockSize()` against four representative
inputs, where `align = sizeof(void*)` (the `BLOCK_ALIGN` constant):
`AlignBlockSize(1)` must return a value `>= align` that is a multiple of `align`;
`AlignBlockSize(align)` must equal `align` exactly (already aligned);
`AlignBlockSize(align + 1)` must round up to `2 × align`;
`AlignBlockSize(3 × align)` must equal `3 × align` (already aligned).
Additionally, `g_Pool->GetBlockSize()` must equal `AlignBlockSize(_STK_POOL_BLOCK_SIZE)`,
confirming the constructor applies the same rounding.

**Pass condition:** all four rounding checks pass; `GetBlockSize() == AlignBlockSize(32)`

---

### Test 10 — `StorageMode`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 checks the external-storage pool (`g_Pool`) via `IsStorageValid()` and
`GetCapacity() == 8`. It then constructs a second pool on the heap (`capacity = 4`,
`raw_block_size = 16`) and verifies `IsStorageValid() == true`, `GetCapacity() == 4`,
`GetBlockSize() >= 16`, `GetBlockSize() % sizeof(void*) == 0`, and `IsEmpty() == true`.
A quick `TryAlloc()` / `Free()` cycle confirms the heap pool allocates correctly and
returns to the empty state. The heap pool is destroyed at the end of the task scope;
its destructor must not fire any assertion (no blocked waiters).

**Pass condition:** all accessor checks pass for both storage modes; heap pool alloc/free cycle succeeds

---

### Test 11 — `Stress`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 200`

All five tasks run 200 iterations each, cycling through three allocation strategies
by iteration index: `i % 3 == 0` uses `TryAlloc()` (may return `nullptr` under
contention), `i % 3 == 1` uses blocking `Alloc()` (always succeeds), and
`i % 3 == 2` uses `TimedAlloc(20)` (may time out under contention). Each successful
allocation calls `memset()` across the full block with a task-and-iteration-derived
byte value, then frees the block and increments `g_SharedCounter`. A `Delay(1)` is
inserted every 8 iterations to allow other tasks to run. Task 4 uses a
`g_InstancesDone` completion barrier, then verifies both that at least one allocation
succeeded and that the pool is fully empty (no leaked blocks).

**Pass condition:** `counter > 0` and `pool.IsEmpty()`

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `TryAllocFreeTask` | 0 only | all accounting checks pass | `TryAlloc()` / `Free()` cycle; `GetUsedCount`, `GetFreeCount`, `IsEmpty` remain consistent |
| 2 | `ExhaustPoolTask` | 0 only | all 8 succeed; 9th returns `nullptr`; `IsEmpty()` after drain | Pool exhaustion: `IsFull()`, overallocation returns `nullptr`, full drain + free |
| 3 | `BlockingAllocTask` | 0–1 | `counter == 1` and `IsEmpty()` | `Alloc()` blocks when pool is full and is woken by `Free()` via `ConditionVariable` |
| 4 | `TimedAllocTimeoutTask` | 0–1 | `nullptr`, elapsed in `[45, 65]` ms | `TimedAlloc()` expires correctly when no block becomes available within the timeout |
| 5 | `TimedAllocSuccessTask` | 0–1 | non-`nullptr` returned | `TimedAlloc()` succeeds and returns a valid block when one is freed within the timeout window |
| 6 | `ConcurrentAllocFreeTask` | 0–4 | `counter == 100` | No allocation lost or doubled under five-task concurrency with blocking `AllocT<T>()` |
| 7 | `TypedAllocTask` | 0 only | non-`nullptr` from all three; fields preserved | `AllocT<T>()`, `TryAllocT<T>()`, `TimedAllocT<T>()` return correct typed pointers and block is writable |
| 8 | `FreeNullTask` | 0 only | `false` returned; `GetFreeCount()` unchanged | `Free(nullptr)` is a safe no-op that does not corrupt pool state |
| 9 | `AlignBlockSizeTask` | 0 only | all four rounding checks pass; `GetBlockSize()` matches | `AlignBlockSize()` rounds up to `BLOCK_ALIGN` multiples correctly across boundary inputs |
| 10 | `StorageModeTask` | 0 only | all accessor checks pass; heap pool cycle succeeds | Both storage constructors initialise correctly; `IsStorageValid`, `GetCapacity`, `GetBlockSize` report expected values |
| 11 | `StressTask` | 0–4 | `counter > 0` and `IsEmpty()` | No corruption, leak, or deadlock under five-task contention mixing `TryAlloc()`, `Alloc()`, and `TimedAlloc(20)` — runs on all platforms |
