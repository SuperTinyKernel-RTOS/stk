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
| `Alloc` | `void *Alloc()` | Blocks indefinitely until a block is available. Returns `nullptr` only if the wait is cancelled via `IKernel::CancelTaskWait()` or `SleepCancel()` — never on timeout (there is none). Use `TimedAllocEx()` to tell a cancel from other failures. ISR-unsafe. |
| `AllocT<T>` | `T *AllocT<T>()` | Typed wrapper around `Alloc()`; `nullptr` only on cancel. ISR-unsafe. |
| `TryAlloc` | `void *TryAlloc()` | Non-blocking; returns `nullptr` immediately if the pool is empty. ISR-safe. |
| `TryAllocT<T>` | `T *TryAllocT<T>()` | Typed wrapper around `TryAlloc()`. ISR-safe. |
| `TimedAllocEx` | `EWaitResult TimedAllocEx(void **block, Timeout timeout = WAIT_INFINITE)` | Same as `TimedAlloc()`, but preserves the full outcome: `WAIT_RESULT_SIGNAL` (a block was allocated, immediately or after waiting; `*block` is set), `WAIT_RESULT_TIMEOUT` (no block within the timeout, or `NO_WAIT` on an empty pool), `WAIT_RESULT_CANCELED` (wait cancelled via `IKernel::CancelTaskWait()` or `SleepCancel()`; nothing is allocated), `WAIT_RESULT_DELETED` (the pool was destroyed while waiting; nothing is allocated and the pool must not be accessed afterwards), `WAIT_RESULT_FAIL` (null `block` or the kernel rejected the wait). `*block` is `nullptr` for every result except `SIGNAL`. ISR-safe only with `NO_WAIT`. |
| `TimedAlloc` | `void *TimedAlloc(Timeout timeout = WAIT_INFINITE)` | Blocks up to `timeout` ticks. `NO_WAIT` is identical to `TryAlloc()`. Returns `nullptr` on timeout or cancellation (use `TimedAllocEx()` to tell these apart). ISR-safe only when `timeout == NO_WAIT`. |
| `TimedAllocT<T>` | `T *TimedAllocT<T>(Timeout timeout = WAIT_INFINITE)` | Typed wrapper around `TimedAlloc()`. Asserts `sizeof(T) <= GetBlockSize()`. |
| `Free` | `bool Free(void *ptr)` | Returns a block to the free-list in O(1) and wakes one blocked allocator. Returns `false` for `nullptr`, out-of-range, or misaligned pointers. ISR-safe. |
| `IsStorageValid` | `bool IsStorageValid() const` | `true` if backing storage is initialised. For heap pools, `false` if the storage allocation failed. |
| `GetCapacity` | `size_t GetCapacity() const` | Total number of blocks in the pool. |
| `GetBlockSize` | `size_t GetBlockSize() const` | Aligned block size in bytes (`>= BLOCK_ALIGN`). |
| `GetUsedCount` | `size_t GetUsedCount() const` | Number of currently allocated (outstanding) blocks. |
| `GetFreeCount` | `size_t GetFreeCount() const` | Number of available blocks (`GetCapacity() - GetUsedCount()`). |
| `IsFull` | `bool IsFull() const` | `true` when no blocks are available. |
| `IsEmpty` | `bool IsEmpty() const` | `true` when no blocks are outstanding. |
| `AlignBlockSize` | `static size_t AlignBlockSize(size_t raw_size)` | Rounds `raw_size` up to the nearest multiple of `BLOCK_ALIGN` (= `sizeof(void*)`), with a minimum of `BLOCK_ALIGN`. Use to size external storage buffers. |

**`TimedAlloc()` / `TimedAllocEx()` paths** (`TimedAlloc()` is `TimedAllocEx()` with every
non-`SIGNAL` result mapped to `nullptr`):

```
pool not empty        →  pop free-list head, return block immediately (O(1)), SIGNAL
pool empty, NO_WAIT   →  return nullptr immediately (TryAlloc fast-path), TIMEOUT
pool empty, timeout   →  suspend via ConditionVariable; wake when Free() signals,
                          or return nullptr / TIMEOUT if the timeout expires first
                          (a block freed at the very moment of expiry is still returned: SIGNAL)
pool empty, cancelled →  wait ends early, return nullptr, CANCELED (nothing allocated)
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
- A cancelled allocation (`WAIT_RESULT_CANCELED`) never takes a block from the pool and
  leaves no stale entry in the wait list: a later `Free()` simply pushes the block back
  (or wakes the next real waiter) and pool accounting stays exact.
- If a `Free()` reaches a waiter before a cancel does, the allocation wins
  (`WAIT_RESULT_SIGNAL`, block returned) and the cancel is a no-op.
- The timeout deadline of a timed wait is captured once as an absolute 64-bit tick value, so
  a wake-up that loses the block to another allocator does not restart the timeout, and the
  deadline cannot wrap on long uptimes.
- Destroying a pool while tasks are blocked in an allocation releases **all** of them with
  `WAIT_RESULT_DELETED` (done by the destructor of the internal `ConditionVariable`): they get no
  block, do not time out, and must not access the pool afterwards. `TimedAllocEx()` reports it,
  `TimedAlloc()` maps it to `nullptr` like every non-`SIGNAL` result. A task can wait only for an
  empty pool, so every block is outstanding at that moment: those blocks are not returned to the
  destroyed pool and their owners must not `Free()` them into it (with external storage the buffer
  stays owned by the caller).

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
resets `g_TestResult`, `g_InstancesDone`, `g_SharedCounter` and the cancellation
state (below) between tests.

Task registration in `RunTest()` is gated by name: task 0 is always added; task 1 is
added when `NeedsTwoTasks()` matches (the same two groups included); task 2 is added when `NeedsThreeTasks()` (the
`"CancelAlloc*"` and `"DeleteWithWaiter*"` tests) or `NeedsAllTasks()` matches; tasks 3–4 are added only when
`NeedsAllTasks()` matches.

The cancellation tests (12–22) additionally use a small amount of shared state, reset by
`ResetTestState()`:

| Variable | Purpose |
|----------|---------|
| `g_Victim` / `g_VictimTid` | Task (and its `TId`) whose wait is cancelled; published by task 1 right before it blocks |
| `g_VictimResult[2]` | `EWaitResult` observed by task 1 / task 2 (`-100` = not set) |
| `g_VictimGotBlock[2]` | Whether task 1 / task 2 received a block from the allocation call |
| `g_VictimElapsed[2]` | Milliseconds spent inside the allocation call by task 1 / task 2 |
| `g_VictimSleepElapsed` | Milliseconds task 1 spends in a `Sleep(60)` after its wait was cancelled |
| `g_CancelParam` | Scenario selector (`1..6`, `+100` = `SleepCancel`); a global because the tasks are `static` and constructed once, so a constructor argument would keep the first case's value |

The helper `CancelVictim(use_sleep_cancel)` cancels the victim either with
`g_Kernel.CancelTaskWait(g_Victim)` or with `stk::SleepCancel(g_VictimTid)`.

The delete-with-waiter tests (23–25) destroy a **separate** pool: `g_DeletePool` points to a `BlockMemoryPool`
(capacity `_STK_POOL_DELETE_CAPACITY` = 4, external storage `g_DeleteBlocks`) constructed with placement-new in the
static `g_DeletePoolObj`, so the pool `g_Pool` of `RunTest()` is never destroyed by the test itself. Task 0 drains the
dedicated pool (only an empty pool can have waiters) and never returns its blocks. After the destruction the storage of
the pool object is overwritten with the byte `DELETED_FILL` (`0xDD`), and task 0 checks at the end that every byte still holds
it: a woken waiter that wrote to the destroyed pool would leave a trace. The tests reuse `g_Victim`, `g_VictimTid`,
`g_VictimResult[]`, `g_VictimGotBlock[]` and `g_VictimElapsed[]` (index 0: task 1, index 1: task 2), and add:

| Variable | Purpose |
|----------|---------|
| `g_DeletePool` | Pointer to the dedicated pool; set back to `nullptr` right after the destruction |
| `g_DeleteVictim2` | Task 2 (second waiter), published right before it blocks; used by the safety net |
| `g_DeleteCheckResult` / `g_DeleteCheckGotBlock` / `g_DeleteCheckElapsed` | `EWaitResult`, whether a block was returned (not expected) and milliseconds of the follow-up `TimedAllocEx(20)` of task 1 on `g_Pool` (`-100` = not set) |
| `g_DeleteParam` | Scenario selector (`DW_EX = 1`, `DW_TIMED = 2`, `DW_TWO = 3`); a global for the same reason as `g_CancelParam` |

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link ten
distinct task class templates simultaneously. Tests 1–10, the cancellation tests
(12–22) and the delete-with-waiter tests (23–25) are skipped on M0 under `#ifndef __ARM_ARCH_6M__`; only `Stress` (test 11) runs.
In the source the cancellation tests and the delete-with-waiter tests execute after test 10 and before `Stress`.

The cancellation tests add one more task class template (`CancelAllocTask`, shared by
all eleven cases) and the delete-with-waiter tests another (`DeleteWithWaiterTask`, shared by
all three cases), further increasing the static memory footprint.

`StressTask` runs on M0 because it uses a single task class template instantiated for
all five task slots, fitting within the available memory.

| Platform | `_STK_POOL_STACK_SIZE` | Tests run |
|----------|------------------------|-----------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words | Test 11 only |
| All others | `256` words | Tests 1–25 |

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

### Cancellation tests (Tests 12–22)

Tests 12–22 verify that a blocked allocation can be cancelled, that a cancel is reported
as `WAIT_RESULT_CANCELED` (never as a timeout and never with a block), and that nothing
is left behind (no lost block, no stale waiter). All use the same task class
(`CancelAllocTask`) and **tasks 0–2 only** (`"CancelAlloc*"` names). The pool is a
fresh 8-block pool.

Task 0 is the canceller: it drains the pool with `TryAlloc()`, sleeps 60 ticks, cancels,
verifies, and finally releases every block it still holds. Task 1 is the *victim*: after
`_STK_POOL_TEST_SHORT_SLEEP` ticks it blocks in `TimedAllocEx()` (or `Alloc()`). Task 2
is a second waiter used only by the two-waiters tests. Tests 12–17 cancel with
`IKernel::CancelTaskWait()`; tests 18–22 repeat the key scenarios with
`stk::SleepCancel()`. The scenario is selected by `g_CancelParam` (`1..6`); adding `100`
selects `SleepCancel`.

Every test also requires `leak_free`: after task 0 releases its blocks, `IsEmpty()` must
be `true`.

> **Safety net:** if a cancel does not work, the victim would stay blocked forever and the
> kernel would never stop. Task 0 therefore always frees every block it holds at the end
> (`ReleaseAll()`), which wakes any stuck waiter, so a broken cancel is reported as a test
> failure rather than a hang.

> **Kernel requirement:** tests 18–22 depend on `SleepCancel()` also cancelling a wait on a
> sync object. Against a kernel where `SleepCancel()` only wakes the task, they fail by
> design: that is the stale wait entry regression they exist to catch.

---

### Test 12 — `CancelAlloc_Infinite`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `1` &nbsp;|&nbsp; **Runs on M0: no**

Task 1 calls `TimedAllocEx(&blk, WAIT_INFINITE)` on the empty pool at about tick 10. Task 0
cancels it at about tick 60 via `CancelTaskWait()` and waits `2 × _STK_POOL_TEST_SHORT_SLEEP`
for the victim to record its result.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, no block returned, elapsed in `[35, 75]` ms, `IsFull()` (the cancel allocated nothing)

---

### Test 13 — `CancelAlloc_NotTimeout`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `2`

Same as test 12 but task 1 waits with a finite 300-tick timeout. Verifies that a cancel is
reported as `WAIT_RESULT_CANCELED`, not `WAIT_RESULT_TIMEOUT`, and that the wait ends early
instead of running to its deadline.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, no block returned, elapsed `< 150` ms

---

### Test 14 — `CancelAlloc_PlainAlloc`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `3`

Task 1 uses the plain blocking `Alloc()`, which has no result code. Verifies the documented
contract that `Alloc()` returns `nullptr` only when the wait was cancelled.

**Pass condition:** `Alloc() == nullptr`, elapsed in `[35, 75]` ms, `IsFull()`

---

### Test 15 — `CancelAlloc_NoStaleWaiter`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `4`

After the cancel, task 1 starts a `Sleep(60)` and measures it. Task 0 then calls `Free()`
on one block with nobody waiting, checks `GetUsedCount() == 7`, and confirms the freed
block is allocatable again exactly once (`TryAlloc()` succeeds, the next `TryAlloc()`
returns `nullptr`). Verifies that the cancelled waiter was unlinked: the freed block must
not be handed to a ghost waiter, and the victim's sleep must not be cut short by a leftover
wait entry.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, no block returned, `Free() == true`, `GetUsedCount() == 7`, re-`TryAlloc()` succeeds, extra `TryAlloc() == nullptr`, victim sleep `>= 55` ms

---

### Test 16 — `CancelAlloc_TwoWaiters`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `5`

Task 1 blocks first, task 2 blocks behind it (`Sleep(2 × _STK_POOL_TEST_SHORT_SLEEP)`
before `TimedAllocEx(WAIT_INFINITE)`). Task 0 cancels only task 1, then calls `Free()`.
The block must reach task 2, which proves the wait list stayed intact after the removal of
its first entry; task 2 returns the block, so only task 0's remaining 7 blocks stay
outstanding.

**Pass condition:** task 1 `WAIT_RESULT_CANCELED` with no block, task 2 `WAIT_RESULT_SIGNAL` with a block, `Free() == true`, `GetUsedCount() == 7`

---

### Test 17 — `CancelAlloc_SignalWins`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `6`

Task 0 calls `Free()` and immediately cancels the victim, with no scheduling point in
between. The victim is already signaled and unlinked when the cancel arrives, so the cancel
must be a no-op and exactly one outcome must occur: the victim receives the block.

**Pass condition:** `Free() == true`, `result == WAIT_RESULT_SIGNAL`, block returned, `GetUsedCount() == 7` (the victim returned it)

---

### Test 18 — `CancelAlloc_SleepCancel`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `101`

Test 12 using `stk::SleepCancel(tid)` instead of `CancelTaskWait()`.

**Pass condition:** same as test 12

---

### Test 19 — `CancelAlloc_SleepCancelPlain`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `103`

Test 14 using `SleepCancel()`: plain `Alloc()` must return `nullptr`.

**Pass condition:** same as test 14

---

### Test 20 — `CancelAlloc_SleepCancelNoStale`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `104`

Test 15 using `SleepCancel()`. This is the primary regression test for `SleepCancel()`
cancelling sync-object waits: without it the wait entry stays linked in the pool's
`ConditionVariable` after the task is woken.

**Pass condition:** same as test 15

---

### Test 21 — `CancelAlloc_SleepCancelTwo`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `105`

Test 16 using `SleepCancel()`.

**Pass condition:** same as test 16

---

### Test 22 — `CancelAlloc_SleepCancelSignal`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `106`

Test 17 using `SleepCancel()`. The task is no longer waiting but may still be marked as
sleeping after the wake, so this also checks that `SleepCancel()` falls through to a plain
wake without disturbing the already-delivered block.

**Pass condition:** same as test 17

---

### Tests 23–25 — `DeleteWithWaiter*`
**Tasks:** 0–2 &nbsp;|&nbsp; **Task class:** `DeleteWithWaiterTask` &nbsp;|&nbsp; **Runs on M0: no**

Verify that destroying a pool which tasks are blocked on releases **all** waiters with `WAIT_RESULT_DELETED`, without giving
them a block and without touching the destroyed object. A task can wait only for an empty pool, so task 0 constructs a
dedicated pool (`g_DeletePool`, 4 blocks, placement-new into `g_DeletePoolObj`, external storage `g_DeleteBlocks`),
**drains** it with `TryAlloc()` and sleeps 60 ticks; the blocks are never returned. Task 1 sleeps `_STK_POOL_TEST_SHORT_SLEEP`,
publishes its `ITask` pointer in `g_Victim` and blocks on the pool with `WAIT_INFINITE`: nobody frees a block, so only the
destruction can end the wait. Task 0 then destroys the pool while it is waited on, and overwrites the storage of the pool
object with `0xDD`, so that any access to the destroyed pool by a woken waiter would read garbage, and any write would be
detected. Task 1 records the result, the elapsed time and whether it got a block, without touching the destroyed pool.
It then performs a `TimedAllocEx(20)` on `g_Pool`, which task 0 has drained right after the destruction: the wait must
run to its deadline and return `WAIT_RESULT_TIMEOUT` without a block; a stale wait entry left behind would end it early or
with another result. Task 0 returns the blocks of `g_Pool` at about tick 100 and checks the outcome at about tick 140. If a waiter
was not released, task 0 cancels its wait with `g_Kernel.CancelTaskWait()`, so the test fails instead of hanging the kernel.

**Common pass condition:** the dedicated pool was drained and full before the destruction; task 1's elapsed time is in `[35, 75]` ms
(waiting from ~10 ms, destroyed at ~60 ms) and it got no block; the follow-up `TimedAllocEx(20)` on `g_Pool` returned
`WAIT_RESULT_TIMEOUT` without a block after at least 15 ms; `g_Pool->IsEmpty()` at the end (no leak); every byte of the
storage of the destroyed pool object still holds `0xDD`

#### Test 23 — `DeleteWithWaiter`
**Param:** `DW_EX` &nbsp;|&nbsp; Task 2 is added by `RunTest()` but stays idle.

Task 1 blocks in `TimedAllocEx(&blk, WAIT_INFINITE)`.

**Pass condition:** common, and the result is `WAIT_RESULT_DELETED` (not a block, a timeout or a cancellation)

#### Test 24 — `DeleteWithWaiterTimed`
**Param:** `DW_TIMED` &nbsp;|&nbsp; Task 2 is added by `RunTest()` but stays idle.

Task 1 uses `TimedAlloc(WAIT_INFINITE)`, which maps every non-`SIGNAL` result to `nullptr`. An infinite wait can not time out and
nobody cancels it, so `nullptr` after ~50 ms identifies the destruction.

**Pass condition:** common, and `TimedAlloc()` returned `nullptr`

#### Test 25 — `DeleteWithWaiterTwo`
**Param:** `DW_TWO`

Task 1 blocks first, task 2 blocks behind it (`Sleep(20)`, then `TimedAllocEx(WAIT_INFINITE)`). The destruction must release
both: the wait list of the pool is walked completely, not just up to its first entry.

**Pass condition:** common, and task 1 and task 2 both get `WAIT_RESULT_DELETED` without a block; the elapsed time of task 2 is in
`[25, 65]` ms (waiting from ~20 ms)

The plain `Alloc()` is not covered: it has no result code.

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
| 12 | `CancelAllocTask` (`Infinite`) | 0–2 | `CANCELED`, no block, elapsed in `[35, 75]` ms, `IsFull()` | `CancelTaskWait()` ends an infinite `TimedAllocEx()` without allocating |
| 13 | `CancelAllocTask` (`NotTimeout`) | 0–2 | `CANCELED`, elapsed `< 150` ms | Cancel is reported as `CANCELED`, not `TIMEOUT`, and ends a finite wait early |
| 14 | `CancelAllocTask` (`PlainAlloc`) | 0–2 | `Alloc() == nullptr`, elapsed in `[35, 75]` ms | `Alloc()` returns `nullptr` only when cancelled |
| 15 | `CancelAllocTask` (`NoStaleWaiter`) | 0–2 | `Free()` ok, `GetUsedCount() == 7`, block re-allocatable once, victim sleep `>= 55` ms | Cancelled waiter is unlinked; no block lost to a ghost waiter |
| 16 | `CancelAllocTask` (`TwoWaiters`) | 0–2 | task 1 `CANCELED`, task 2 `SIGNAL` with block, `GetUsedCount() == 7` | Cancelling one waiter leaves the other's wait list entry intact |
| 17 | `CancelAllocTask` (`SignalWins`) | 0–2 | `SIGNAL` with block, `GetUsedCount() == 7` | A `Free()` that arrived first wins over a late cancel |
| 18 | `CancelAllocTask` (`SleepCancel`) | 0–2 | same as 12 | `SleepCancel()` cancels a pool wait like test 12 |
| 19 | `CancelAllocTask` (`SleepCancelPlain`) | 0–2 | same as 14 | `SleepCancel()` with plain `Alloc()` |
| 20 | `CancelAllocTask` (`SleepCancelNoStale`) | 0–2 | same as 15 | `SleepCancel()` leaves no stale wait entry |
| 21 | `CancelAllocTask` (`SleepCancelTwo`) | 0–2 | same as 16 | `SleepCancel()` keeps the second waiter intact |
| 22 | `CancelAllocTask` (`SleepCancelSignal`) | 0–2 | same as 17 | `SleepCancel()` after a `Free()` is a no-op |
| 23 | `DeleteWithWaiterTask` (`DeleteWithWaiter`) | 0–2 | `DELETED`, no block, elapsed `[35, 75]` ms, follow-up wait on `g_Pool` times out `>= 15` ms, no leak, storage untouched | Destroying a pool which a task waits on releases it with `WAIT_RESULT_DELETED`; no block is handed out; the waiter writes nothing to the destroyed pool and leaves no stale wait entry |
| 24 | `DeleteWithWaiterTask` (`DeleteWithWaiterTimed`) | 0–2 | `TimedAlloc()` returns `nullptr` after `[35, 75]` ms, as test 23 | `TimedAlloc()` maps a destruction to `nullptr`; an infinite wait can not time out, so it identifies the destruction |
| 25 | `DeleteWithWaiterTask` (`DeleteWithWaiterTwo`) | 0–2 | both `DELETED`, task 2 elapsed `[25, 65]` ms, as test 23 | The destruction releases every waiter, not only the first one |
