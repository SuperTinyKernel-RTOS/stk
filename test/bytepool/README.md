# BytePool Test Suite

Test suite for `stk::memory::BytePool`.  
Source: `test/bytepool/test_bytepool.cpp`

---

## API Summary

```cpp
#include <memory/stk_memory_bytepool.h>
```

`BytePool` is a variable-size (first-fit) allocator with block splitting, block coalescing
and proper task-blocking semantics. It keeps an implicit block list inside the storage
area: every block, free or used, is preceded by an 8-byte header (`size` + state marker),
and blocks follow each other back to back — no separate metadata array is needed. Returned
pointers are aligned to 8 bytes.

Unlike `BlockMemoryPool`, allocation time is proportional to the number of blocks scanned
(not O(1)) and the pool can fragment; in exchange, requests of any size are served from one
area.

**Two storage modes** are supported:

| Mode | Constructor | Frees storage? |
|------|-------------|----------------|
| External storage | `BytePool(buf, bufsz [, name])` | No — caller owns buffer |
| Heap storage | `BytePool(bufsz [, name])` | Yes — freed in destructor |

**Blocking semantics**: `TimedAlloc()` suspends the calling task via an internal
`ConditionVariable` until enough contiguous memory is released by `Free()`, or the timeout
expires. Every `Free()` wakes **all** waiters (their requests differ in size, so waking only
one could leave a satisfiable request asleep); each waiter re-scans for its own request and
goes back to sleep if it still does not fit. `TryAlloc()` and `Free()` are ISR-safe (but not
constant time); `Alloc()` and `TimedAlloc()` with a non-zero timeout must only be called from
task context.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// External-storage construction (zero-heap, static buffer)
alignas(8) static uint8_t g_Buf[4096];

stk::memory::BytePool g_Pool(g_Buf, sizeof(g_Buf));

// Heap-storage construction
stk::memory::BytePool g_Pool(4096U);
// Always check after heap construction:
assert(g_Pool.IsStorageValid());
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Alloc` | `void *Alloc(size_t size)` | Blocks indefinitely until `size` bytes are available. Returns `nullptr` only if the wait is cancelled via `IKernel::CancelTaskWait()` or `SleepCancel()`, or if the request is invalid — never on timeout (there is none). Use `TimedAllocEx()` to tell these apart. ISR-unsafe. |
| `AllocT<T>` | `T *AllocT<T>()` | Typed wrapper around `Alloc(sizeof(T))`; `nullptr` only on cancel. ISR-unsafe. |
| `TryAlloc` | `void *TryAlloc(size_t size)` | Non-blocking; returns `nullptr` immediately if no suitable free block exists. ISR-safe. |
| `TryAllocT<T>` | `T *TryAllocT<T>()` | Typed wrapper around `TryAlloc()`. ISR-safe. |
| `TimedAllocEx` | `EWaitResult TimedAllocEx(void **block, size_t size, Timeout timeout = WAIT_INFINITE)` | Same as `TimedAlloc()`, but preserves the full outcome: `WAIT_RESULT_SIGNAL` (a block was allocated, immediately or after waiting; `*block` is set), `WAIT_RESULT_TIMEOUT` (nothing suitable within the timeout, or `NO_WAIT` with no suitable block), `WAIT_RESULT_CANCELED` (wait cancelled via `IKernel::CancelTaskWait()` or `SleepCancel()`; nothing is allocated), `WAIT_RESULT_DELETED` (the pool was destroyed while waiting; nothing is allocated and the pool must not be accessed afterwards), `WAIT_RESULT_FAIL` (null `block`, `size` is `0` or exceeds `GetMaxRequest()`, no valid storage, or the kernel rejected the wait). `*block` is `nullptr` for every result except `SIGNAL`. ISR-safe only with `NO_WAIT`. |
| `TimedAlloc` | `void *TimedAlloc(size_t size, Timeout timeout = WAIT_INFINITE)` | Blocks up to `timeout` ticks. `NO_WAIT` is identical to `TryAlloc()`. Returns `nullptr` on timeout, cancellation or invalid request (use `TimedAllocEx()` to tell these apart). ISR-safe only when `timeout == NO_WAIT`. |
| `TimedAllocT<T>` | `T *TimedAllocT<T>(Timeout timeout = WAIT_INFINITE)` | Typed wrapper around `TimedAlloc()`. Asserts `alignof(T) <= 8`. |
| `Free` | `bool Free(void *ptr)` | Marks the block free, merges it with the free blocks that follow it, and wakes all blocked allocators. Returns `false` for `nullptr`, out-of-range, misaligned or not-in-use pointers. ISR-safe (not constant time). |
| `GetInfo` | `void GetInfo(size_t &available, size_t &largest, size_t &fragments) const` | Consistent snapshot taken under a critical section: total allocatable payload bytes, payload of the largest free fragment (the biggest request that would succeed now), and the number of free fragments. Adjacent free blocks count as one fragment. O(n). |
| `IsStorageValid` | `bool IsStorageValid() const` | `true` if the pool is ready for use. `false` if a heap allocation failed or the external buffer is too small to hold even one block. |
| `GetSize` | `size_t GetSize() const` | Managed (aligned) area size in bytes, headers included; `0` if the storage is invalid. |
| `GetMaxRequest` | `size_t GetMaxRequest() const` | Largest payload that can ever be allocated (one block spanning the whole pool). |
| `AlignSize` | `static constexpr size_t AlignSize(size_t size)` | Rounds `size` up to a multiple of 8. |
| `RequiredSize` | `static constexpr size_t RequiredSize(size_t payload)` | Pool space consumed by one allocation: aligned payload plus the 8-byte header (`HEADER_SIZE`). Use to size external storage buffers. |

**`TimedAlloc()` / `TimedAllocEx()` paths** (`TimedAlloc()` is `TimedAllocEx()` with every
non-`SIGNAL` result mapped to `nullptr`):

```
size == 0 or > GetMaxRequest()  →  FAIL immediately (can never succeed, never blocks)
suitable free block exists      →  claim it (first-fit, split if the rest can hold a block), SIGNAL
none, NO_WAIT                   →  return nullptr immediately (TryAlloc fast-path), TIMEOUT
none, timeout                   →  suspend via ConditionVariable; after every wake-up re-scan;
                                    TIMEOUT if the absolute deadline passes first
                                    (memory freed at the very moment of expiry is still returned: SIGNAL)
none, cancelled                 →  wait ends early, return nullptr, CANCELED (nothing allocated)
```

**`Free()` path:**

```
ptr == nullptr        →  return false (safe no-op)
ptr out of range      →  assert + return false
ptr misaligned        →  assert + return false
block not USED        →  assert + return false (double-free / foreign pointer guard)
otherwise             →  mark FREE, merge with following free blocks, NotifyAll() to wake waiters
```

**Key invariants:**

- Every `Alloc()` / `TryAlloc()` / `TimedAlloc()` that returns non-`nullptr` must be
  paired with exactly one `Free()` call.
- Returned pointers are 8-byte aligned and blocks never overlap.
- Adjacent free blocks are always reported (and allocated) as a single fragment: `Free()`
  merges forward immediately, a scan merges whatever it passes, and `GetInfo()` counts
  adjacent free blocks as one. A failed scan leaves the pool fully coalesced.
- A free remainder smaller than one header plus the minimum payload (16 bytes) is not split
  off; it stays part of the allocated block.
- A request that cannot fit in the whole pool (or is zero bytes) fails immediately instead of
  blocking forever.
- A woken waiter always re-checks its own request; a `Free()` that does not satisfy a waiter
  leaves it waiting, and the waiter's timeout is not extended by such wake-ups.
- A cancelled allocation (`WAIT_RESULT_CANCELED`) never takes memory from the pool and leaves
  no stale entry in the wait list: a later `Free()` simply releases the block (or wakes the
  next real waiter) and pool accounting stays exact.
- If a `Free()` reaches a waiter before a cancel does, the allocation wins
  (`WAIT_RESULT_SIGNAL`, block returned) and the cancel is a no-op.
- The timeout deadline of a timed wait is captured once as an absolute 64-bit tick value, so
  a wake-up that does not yield a block does not restart the timeout, and the deadline cannot
  wrap on long uptimes.
- Freed memory is not reserved for the longest-waiting task, so a large request can be
  starved by a stream of small ones (not tested; documented behaviour).
- Destroying a pool while tasks are blocked in an allocation releases **all** of them with
  `WAIT_RESULT_DELETED` (done by the destructor of the internal `ConditionVariable`), whatever
  their request sizes: they get no block, do not time out, and must not access the pool
  afterwards. `TimedAllocEx()` reports it, `TimedAlloc()` maps it to `nullptr` like every
  non-`SIGNAL` result. The blocks that are outstanding at that moment are not returned to the
  destroyed pool and their owners must not `Free()` them into it (with external storage the
  buffer stays owned by the caller).

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_POOL_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_POOL_TEST_TIMEOUT` | `1000` ticks | Reference blocking timeout |
| `_STK_POOL_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_POOL_TEST_LONG_SLEEP` | `100` ticks | Sleep used by verifier tasks to wait for workers |
| `_STK_POOL_STORAGE_SIZE` | `512` bytes | Size of the shared pool buffer (`g_PoolStorage`) |
| `_STK_POOL_BLK_PAYLOAD` | `56` bytes | Payload that makes a uniform 64-byte block (56 + 8-byte header) |
| `_STK_POOL_BLK_COUNT` | `8` blocks | Uniform blocks that exactly fill the pool (`static_assert`-checked) |
| `_STK_POOL_STACK_SIZE` | `128` (M0) / `512` (others) | Per-task stack size in `size_t` words |

The pool is sized so that eight 56-byte requests exhaust it exactly (8 × 64 = 512), which makes
exhaustion, adjacency (`2` blocks = `128` bytes = a `120`-byte request) and fragmentation easy
to reason about. A fresh `BytePool` instance backed by the static `g_PoolStorage` buffer is
constructed at the start of every `RunTest()` call and destroyed at the end. This ensures every
test starts with a fully free pool in a known state. `ResetTestState()` resets `g_TestResult`,
`g_InstancesDone`, `g_SharedCounter`, `g_Errors`, the waiter flags and the cancellation state
(below) between tests.

Task registration in `RunTest()` is gated by name: task 0 is always added; task 1 is
added when `NeedsTwoTasks()` matches (the `"DeleteWithWaiter*"` tests included); task 2 is added when `NeedsThreeTasks()`
(`WakeAllWaiters`, the `"CancelAlloc*"` and the `"DeleteWithWaiter*"` tests) or `NeedsAllTasks()` matches; tasks 3–4 are
added only when `NeedsAllTasks()` matches.

Because `BytePool` has no used-block counter, the tests inspect the pool through
`GetInfo()`-based helpers:

| Helper | Meaning |
|--------|---------|
| `FreeSpace()` | Free pool space with headers included: `available + fragments × 8` |
| `UsedBlocks()` | Outstanding uniform blocks: `(512 − FreeSpace()) / 64` (valid while only 56-byte requests are used) |
| `IsAllFree()` | Nothing allocated: exactly one free fragment equal to `GetMaxRequest()` |
| `IsExhausted()` | `available == 0`, `largest == 0`, `fragments == 0` |
| `DrainPool()` / `ReleaseAll()` | Allocate all eight uniform blocks / free every block still held |

Additional shared state used by the tests, all reset by `ResetTestState()`:

| Variable | Purpose |
|----------|---------|
| `g_Errors` | Corruption / misalignment count reported by worker tasks (`ConcurrentAllocFree`, `Stress`) |
| `g_WaiterDone[2]` | Set by the large (`[0]`) and small (`[1]`) waiter in `WakeAllWaiters` |
| `g_SmallStorage` | 128-byte buffer used for the local pool in `SplitCoalesce` |

The cancellation tests (17–27) additionally use:

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

The delete-with-waiter tests (28–30) destroy a **separate** pool: `g_DeletePool` points to a `BytePool` of two uniform
blocks (128 bytes, external buffer `g_DeleteStorage`) constructed with placement-new in the static `g_DeletePoolObj`, so
the pool `g_Pool` of `RunTest()` is never destroyed by the test itself. Task 0 allocates both blocks of the dedicated pool
and never returns them. After the destruction both the storage of the pool object and the external buffer are overwritten
with the byte `DELETED_FILL` (`0xDD`), and task 0 checks at the end that every byte still holds it: a woken waiter that
wrote to the destroyed pool, or re-scanned its block list (a scan merges free blocks, so it writes), would leave a trace.
The tests reuse `g_Victim`, `g_VictimTid`, `g_VictimResult[]`, `g_VictimGotBlock[]` and `g_VictimElapsed[]` (index 0:
task 1, index 1: task 2), the helpers `DrainPool()`, `ReleaseAll()` and `IsAllFree()`, and add:

| Variable | Purpose |
|----------|---------|
| `g_DeletePool` | Pointer to the dedicated pool; set back to `nullptr` right after the destruction |
| `g_DeleteVictim2` | Task 2 (second waiter), published right before it blocks; used by the safety net |
| `g_DeleteCheckResult` / `g_DeleteCheckGotBlock` / `g_DeleteCheckElapsed` | `EWaitResult`, whether a block was returned (not expected) and milliseconds of the follow-up `TimedAllocEx(20)` of task 1 on `g_Pool` (`-100` = not set) |
| `g_DeleteParam` | Scenario selector (`DW_EX = 1`, `DW_TIMED = 2`, `DW_TWO = 3`); a global for the same reason as `g_CancelParam` |

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link many
distinct task class templates simultaneously. Tests 1–15, the cancellation tests
(17–27) and the delete-with-waiter tests (28–30) are skipped on M0 under `#ifndef __ARM_ARCH_6M__`; only `Stress` (test 16) runs.
In the source the cancellation tests and then the delete-with-waiter tests execute after test 15 and before `Stress`.

The cancellation tests add one more task class template (`CancelAllocTask`, shared by
all eleven cases) and the delete-with-waiter tests another (`DeleteWithWaiterTask`, shared by
all three cases), further increasing the static memory footprint.

`StressTask` runs on M0 because it uses a single task class template instantiated for
all five task slots, fitting within the available memory.

The non-M0 stack size is larger than in the `BlockMemoryPool` suite (512 vs 256 words)
because some tests keep several `BytePool` objects and scratch arrays on a task stack.

| Platform | `_STK_POOL_STACK_SIZE` | Tests run |
|----------|------------------------|-----------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words | Test 16 only |
| All others | `512` words | Tests 1–30 |

---

## Tests

### Test 1 — `TryAllocFree`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 verifies the initial state — `IsStorageValid()`, `GetSize() == 512`,
`GetMaxRequest() == 504`, and `IsAllFree()` — then calls `TryAlloc(64)` and confirms the
pointer is non-`nullptr` and 8-byte aligned. `GetInfo()` must report a single fragment of
`512 − 72 − 8 = 432` bytes (the 64-byte payload plus its header were taken from the one free
block). It writes `0xAB` across the 64 bytes, frees the block (which merges with the free tail,
restoring `IsAllFree()`), and finally allocates `GetMaxRequest()` bytes — the entire pool as one
block — checking `IsExhausted()`, then frees it and re-checks `IsAllFree()`.

**Pass condition:** all accounting checks pass, including the whole-pool allocation

---

### Test 2 — `ExhaustPool`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 drains all 8 uniform blocks via `TryAlloc(56)`, requires each to be non-`nullptr` and
8-byte aligned, and fills each with its own pattern (`0x10 + i`). It then checks
`IsExhausted()` and `UsedBlocks() == 8`, and verifies that every pattern is intact (blocks do
not overlap). A further `TryAlloc(1)` must return `nullptr` without blocking, and
`TimedAllocEx(&p, 1, NO_WAIT)` must return `WAIT_RESULT_TIMEOUT` with `p == nullptr`. All 8 blocks
are then freed **in address order**: nothing is merged backwards at `Free()` time, but
`IsAllFree()` must already see one contiguous free run, and an allocation of `GetMaxRequest()`
bytes must succeed (the scan performs the lazy merge).

**Pass condition:** 8 distinct non-overlapping blocks; the 9th request fails; `IsAllFree()` after freeing; whole-pool allocation succeeds

---

### Test 3 — `SplitCoalesce`
**Tasks:** task 0 only (tasks 1–4 present but idle)

*Part 1 (main pool).* Task 0 allocates A, B, C of 24 bytes each (32-byte blocks) and requires
sequential first-fit placement (`B − A == 32`, `C − B == 32`). It frees B then A: A merges with
the free B that follows it into a 64-byte hole, so `GetInfo()` must report 2 fragments, a
`largest` equal to the tail (`512 − 96 − 8 = 408`) and `available == 56 + 408`. A request of
exactly `56` bytes (the hole's payload) must land in the hole (`D == A`, first-fit). Freeing D and
C then leaves the pool in one contiguous free run (`IsAllFree()`).

*Part 2 (local 128-byte pool on `g_SmallStorage`).* `TryAlloc(56)` takes the first 64-byte block;
`TryAlloc(48)` needs 56 bytes but the remaining block is 64, and the 8-byte remainder is too small
to hold a block — so the whole block is given out and `GetInfo()` reports nothing available
(`0 / 0 / 0`). The full 56-byte payload of that block is writable. After freeing it, `GetInfo()`
must report `56 / 56 / 1`; after freeing the first block (which merges forward) the pool must be
one fragment of `GetMaxRequest()` bytes.

**Pass condition:** placement, hole reuse, merge and no-split-of-tiny-remainder checks all pass

---

### Test 4 — `BlockingAlloc`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 0 drains all 8 blocks via `TryAlloc(56)` to force task 1 into a blocking path, then sleeps
`2 × SHORT_SLEEP` ticks to allow task 1 to enter `Alloc(56)` and suspend. Task 0 then frees one
block — this must wake task 1. Task 1 receives the block from its `Alloc()` call, sets
`g_SharedCounter = 1`, and frees the block. Task 0 waits on a `g_InstancesDone` completion barrier
then verifies both that `g_SharedCounter == 1` (task 1 unblocked successfully) and `IsAllFree()`
(no outstanding memory).

**Pass condition:** `g_SharedCounter == 1` and `IsAllFree()`

---

### Test 5 — `BlockingLarge`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 0 drains the pool. Task 1 calls `Alloc(120)`, which needs 128 bytes (two adjacent blocks).
Task 0 frees block 3 only: one 64-byte block cannot satisfy the request, so task 1 must be
woken, re-scan, and **keep waiting** (`g_SharedCounter` still `0`). Task 0 then frees the
adjacent block 4; the two free blocks form a 128-byte hole (merged by the scan) and task 1 must
now succeed. Task 1 fills its 120 bytes, sets `g_SharedCounter = 1` and frees the block.

**Pass condition:** task 1 still blocked after the first `Free()`; task 1 completed after the second; `IsAllFree()`

---

### Test 6 — `WakeAllWaiters`
**Tasks:** 0–2 active (tasks 3–4 present but idle)

Task 1 (at about tick 10) waits for a large request: `Alloc(248)`, which needs 4 blocks. Task 2
(at about tick 20) waits for a small one: `Alloc(56)`, one block. Task 0 drains the pool, sleeps
40 ticks, then frees a **single** block. That block is useless to task 1 (queued first) but is
exactly what task 2 needs, so task 2 must get it — proving that `Free()` wakes every waiter rather
than only the first. Task 2 returns the block immediately. Task 0 then frees the three neighbouring
blocks, completing a 256-byte hole at the pool start, and task 1 must succeed.

**Pass condition:** after the first `Free()` task 2 is done and task 1 is not; after the next three task 1 is done; `IsAllFree()`

---

### Test 7 — `TimedAllocTimeout`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 0 fills the pool and holds all 8 blocks for 200 ticks. Task 1 sleeps `SHORT_SLEEP` to let
task 0 establish ownership, then calls `TimedAllocEx(&blk, 56, 50)` and measures elapsed time.
Since the pool remains full for the entire 50-tick window, the call must report
`WAIT_RESULT_TIMEOUT` with `blk == nullptr` and elapsed time in `[45, 65]` ms. `g_TestResult` is
set from task 1's branch.

**Pass condition:** `WAIT_RESULT_TIMEOUT`, `blk == nullptr`, elapsed in `[45, 65]` ms

---

### Test 8 — `TimedAllocSuccess`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 0 fills the pool then frees one block after 40 ticks — before task 1's 150-tick
timeout expires. Task 1 sleeps `SHORT_SLEEP`, then calls `TimedAlloc(56, 150)`. Since task 0
releases a block within the timeout window, the call must return a valid non-`nullptr`
pointer. Task 1 frees the acquired block and sets `g_TestResult` on success.

**Pass condition:** `TimedAlloc(56, 150)` returned non-`nullptr`

---

### Test 9 — `TimedAllocDeadline`
**Tasks:** 0–1 active (tasks 2–4 present but idle)

Task 1 waits up to 100 ticks for 120 bytes (two adjacent blocks) starting at about tick 10, so
its deadline is about tick 110. Task 0 drains the pool, then frees a single block (block 1) at
about tick 40 and another **non-adjacent** one (block 5) at about tick 70. Each `Free()` wakes
task 1, but neither can satisfy it. The wait must still expire at about 100 ticks after it began:
if every wake-up restarted the full timeout the call would return after about 170 ticks. Task 0
holds on until about tick 220 before releasing the rest, so task 1 can only have ended by timing out.

**Pass condition:** `WAIT_RESULT_TIMEOUT`, `blk == nullptr`, elapsed in `[95, 125]` ms; `IsAllFree()` at the end

---

### Test 10 — `ConcurrentAllocFree`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Param:** `iterations = 20`

All five tasks compete for pool memory for 20 iterations each using the blocking `Alloc()`. In
each iteration a task requests a task- and iteration-dependent size between 8 and 64 bytes, checks
the pointer is 8-byte aligned, fills the block with a task-specific pattern, calls `Yield()` so the
other tasks allocate around it, then verifies the whole pattern is intact before freeing the block.
Any overlap of blocks, or corruption of a block header, shows up as a pattern mismatch or
misalignment (counted in `g_Errors`) or as a pool that does not return to a single free fragment.
The worst case (five outstanding 72-byte blocks in 512 bytes) can never exhaust the pool or leave
it too fragmented for a 64-byte request, so every `Alloc()` is guaranteed to complete. The
`g_SharedCounter` increment is intentionally unsynchronised. Task 0 uses a `g_InstancesDone`
completion barrier.

**Pass condition:** `counter == 100` (`5 tasks × 20 iterations`), `g_Errors == 0`, `IsAllFree()`

---

### Test 11 — `TypedAlloc`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 exercises all three typed allocation wrappers against a `TestRecord` struct
(`{uint32_t id; uint32_t value;}`). It calls `TryAllocT<TestRecord>()`, checks the pointer is
8-byte aligned, writes `id = 42` and `value = 0xDEADBEEF`, reads them back to confirm the fields
are preserved, and frees the block. It then repeats the cycle with
`TimedAllocT<TestRecord>(WAIT_INFINITE)` and `AllocT<TestRecord>()`, verifying each returns a
non-`nullptr` pointer. All three wrappers must return correctly typed pointers without asserting,
and the pool must be `IsAllFree()` at the end.

**Pass condition:** all three typed wrappers return non-`nullptr`; field values survive until `Free()`; `IsAllFree()`

---

### Test 12 — `FreeNull`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 calls `Free(nullptr)`. The call must return `false` (the `nullptr` guard in the `Free()`
implementation) and must leave the pool in an identical state — still `IsAllFree()`, no crash,
no assertion in release builds.

**Pass condition:** `Free(nullptr) == false` and `IsAllFree()`

---

### Test 13 — `InvalidRequest`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Requests that can never succeed must fail at once, even with an infinite timeout. Task 0 checks
that `TryAlloc(0)` and `TryAlloc(GetMaxRequest() + 1)` return `nullptr`; that
`TimedAllocEx(&blk, 0, WAIT_INFINITE)` and `TimedAllocEx(&blk, GetMaxRequest() + 1, WAIT_INFINITE)`
return `WAIT_RESULT_FAIL` with `blk == nullptr`; and that `Alloc(GetMaxRequest() + 1)` and
`TimedAlloc(GetMaxRequest() + 1, 500)` return `nullptr`. All of this together must take less than
20 ms — no call may wait. Finally `TryAlloc(GetMaxRequest())`, the largest valid request, must still
succeed.

**Pass condition:** every invalid request fails with the documented result in `< 20` ms total; the largest valid request succeeds; `IsAllFree()`

---

### Test 14 — `AlignHelpers`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 verifies the static helpers and constants: `HEADER_SIZE == 8`;
`AlignSize(0, 1, 8, 9, 24)` returns `0, 8, 8, 16, 24`; `RequiredSize(1, 8, 9, 56)` returns
`16, 16, 24, 64`; and the pool reports `GetSize() == 512` and `GetMaxRequest() == 504`.

**Pass condition:** all helper checks pass

---

### Test 15 — `StorageMode`
**Tasks:** task 0 only (tasks 1–4 present but idle)

Task 0 checks the external-storage pool (`g_Pool`): `IsStorageValid()` and `GetSize() == 512`.
It then exercises three more pools, each in its own scope so the destructor runs:

- **Heap storage** (`BytePool(256)`): `IsStorageValid()`, `GetSize()` within one alignment step of
  256, a 100-byte alloc/free cycle, and a single full-size free fragment afterwards. The destructor
  must not fire any assertion (no blocked waiters).
- **Smallest valid external buffer** (16 bytes: one header plus 8 bytes payload):
  `GetMaxRequest() == 8`, `TryAlloc(8)` succeeds, a further `TryAlloc(1)` returns `nullptr`.
- **Misaligned external buffer** (`raw + 1`, 71 bytes): `IsStorageValid()`, the 7 unaligned head
  bytes are skipped so `GetSize() == 64` and `GetMaxRequest() == 56`, the returned pointer is
  8-byte aligned and lies inside the buffer, and the full 56 bytes are writable.

**Pass condition:** all accessor checks pass for every storage mode; every alloc/free cycle succeeds

---

### Test 16 — `Stress`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 200`

All five tasks run 200 iterations each, cycling through three allocation strategies
by iteration index: `i % 3 == 0` uses `TryAlloc()` (may return `nullptr` under
contention), `i % 3 == 1` uses blocking `Alloc()` (always succeeds), and
`i % 3 == 2` uses `TimedAlloc(size, 20)` (may time out under contention). Request sizes vary
between 8 and 64 bytes depending on task and iteration. Each successful allocation fills the
block with a task-and-iteration-derived byte value, occasionally yields, verifies the pattern is
intact (a mismatch is counted in `g_Errors`), then frees the block and increments
`g_SharedCounter`. A `Delay(1)` is inserted every 8 iterations to allow other tasks to run.
Task 4 uses a `g_InstancesDone` completion barrier, then verifies that at least one allocation
succeeded, that no corruption was seen, and that the pool is back to a single free fragment
(no leaked memory).

**Pass condition:** `counter > 0`, `g_Errors == 0` and `IsAllFree()`

---

### Cancellation tests (Tests 17–27)

Tests 17–27 verify that a blocked allocation can be cancelled, that a cancel is reported
as `WAIT_RESULT_CANCELED` (never as a timeout and never with a block), and that nothing
is left behind (no lost memory, no stale waiter). All use the same task class
(`CancelAllocTask`) and **tasks 0–2 only** (`"CancelAlloc*"` names). The pool is a
fresh 512-byte pool and every victim request is `56` bytes (one uniform block).

Task 0 is the canceller: it drains the pool with `TryAlloc(56)`, sleeps 60 ticks, cancels,
verifies, and finally releases every block it still holds. Task 1 is the *victim*: after
`_STK_POOL_TEST_SHORT_SLEEP` ticks it blocks in `TimedAllocEx()` (or `Alloc()`). Task 2
is a second waiter used only by the two-waiters tests. Tests 17–22 cancel with
`IKernel::CancelTaskWait()`; tests 23–27 repeat the key scenarios with
`stk::SleepCancel()`. The scenario is selected by `g_CancelParam` (`1..6`); adding `100`
selects `SleepCancel`.

Every test also requires `leak_free`: after task 0 releases its blocks, `IsAllFree()` must
be `true`.

> **Safety net:** if a cancel does not work, the victim would stay blocked forever and the
> kernel would never stop. Task 0 therefore always frees every block it holds at the end
> (`ReleaseAll()`), which wakes any stuck waiter, so a broken cancel is reported as a test
> failure rather than a hang.

> **Kernel requirement:** tests 23–27 depend on `SleepCancel()` also cancelling a wait on a
> sync object. Against a kernel where `SleepCancel()` only wakes the task, they fail by
> design: that is the stale wait entry regression they exist to catch.

---

### Test 17 — `CancelAlloc_Infinite`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `1` &nbsp;|&nbsp; **Runs on M0: no**

Task 1 calls `TimedAllocEx(&blk, 56, WAIT_INFINITE)` on the exhausted pool at about tick 10. Task 0
cancels it at about tick 60 via `CancelTaskWait()` and waits `2 × _STK_POOL_TEST_SHORT_SLEEP`
for the victim to record its result.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, no block returned, elapsed in `[35, 75]` ms, `IsExhausted()` (the cancel allocated nothing)

---

### Test 18 — `CancelAlloc_NotTimeout`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `2`

Same as test 17 but task 1 waits with a finite 300-tick timeout. Verifies that a cancel is
reported as `WAIT_RESULT_CANCELED`, not `WAIT_RESULT_TIMEOUT`, and that the wait ends early
instead of running to its deadline.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, no block returned, elapsed `< 150` ms

---

### Test 19 — `CancelAlloc_PlainAlloc`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `3`

Task 1 uses the plain blocking `Alloc(56)`, which has no result code. Verifies the documented
contract that `Alloc()` returns `nullptr` when the wait was cancelled.

**Pass condition:** `Alloc() == nullptr`, elapsed in `[35, 75]` ms, `IsExhausted()`

---

### Test 20 — `CancelAlloc_NoStaleWaiter`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `4`

After the cancel, task 1 starts a `Sleep(60)` and measures it. Task 0 then calls `Free()`
on one block with nobody waiting, checks `UsedBlocks() == 7`, and confirms the freed
memory is allocatable again exactly once (`TryAlloc(56)` succeeds, the next `TryAlloc(56)`
returns `nullptr`). Verifies that the cancelled waiter was unlinked: the freed block must
not be handed to a ghost waiter, and the victim's sleep must not be cut short by a leftover
wait entry.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, no block returned, `Free() == true`, `UsedBlocks() == 7`, re-`TryAlloc()` succeeds, extra `TryAlloc() == nullptr`, victim sleep `>= 55` ms

---

### Test 21 — `CancelAlloc_TwoWaiters`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `5`

Task 1 blocks first, task 2 blocks behind it (`Sleep(2 × _STK_POOL_TEST_SHORT_SLEEP)`
before `TimedAllocEx(&blk, 56, WAIT_INFINITE)`). Task 0 cancels only task 1, then calls `Free()`.
The block must reach task 2, which proves the wait list stayed intact after the removal of
its first entry; task 2 returns the block, so only task 0's remaining 7 blocks stay
outstanding.

**Pass condition:** task 1 `WAIT_RESULT_CANCELED` with no block, task 2 `WAIT_RESULT_SIGNAL` with a block, `Free() == true`, `UsedBlocks() == 7`

---

### Test 22 — `CancelAlloc_SignalWins`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `6`

Task 0 calls `Free()` and immediately cancels the victim, with no scheduling point in
between. The victim is already signaled and unlinked when the cancel arrives, so the cancel
must be a no-op and exactly one outcome must occur: the victim receives the block.

**Pass condition:** `Free() == true`, `result == WAIT_RESULT_SIGNAL`, block returned, `UsedBlocks() == 7` (the victim returned it)

---

### Test 23 — `CancelAlloc_SleepCancel`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `101`

Test 17 using `stk::SleepCancel(tid)` instead of `CancelTaskWait()`.

**Pass condition:** same as test 17

---

### Test 24 — `CancelAlloc_SleepCancelPlain`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `103`

Test 19 using `SleepCancel()`: plain `Alloc()` must return `nullptr`.

**Pass condition:** same as test 19

---

### Test 25 — `CancelAlloc_SleepCancelNoStale`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `104`

Test 20 using `SleepCancel()`. This is the primary regression test for `SleepCancel()`
cancelling sync-object waits: without it the wait entry stays linked in the pool's
`ConditionVariable` after the task is woken.

**Pass condition:** same as test 20

---

### Test 26 — `CancelAlloc_SleepCancelTwo`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `105`

Test 21 using `SleepCancel()`.

**Pass condition:** same as test 21

---

### Test 27 — `CancelAlloc_SleepCancelSignal`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `106`

Test 22 using `SleepCancel()`. The task is no longer waiting but may still be marked as
sleeping after the wake, so this also checks that `SleepCancel()` falls through to a plain
wake without disturbing the already-delivered block.

**Pass condition:** same as test 22

---

### Tests 28–30 — `DeleteWithWaiter*`
**Tasks:** 0–2 &nbsp;|&nbsp; **Task class:** `DeleteWithWaiterTask` &nbsp;|&nbsp; **Runs on M0: no**

Verify that destroying a pool which tasks are blocked on releases **all** waiters with `WAIT_RESULT_DELETED`, without giving
them a block and without touching the destroyed pool. Task 0 constructs a dedicated pool (`g_DeletePool`, two uniform
64-byte blocks in the external buffer `g_DeleteStorage`, placement-new into `g_DeletePoolObj`), allocates **both** blocks with
`TryAlloc()` (`GetInfo()` must then report `0 / 0 / 0`) and sleeps 60 ticks; the blocks are never returned. Task 1 sleeps
`_STK_POOL_TEST_SHORT_SLEEP`, publishes its `ITask` pointer in `g_Victim` and blocks on the pool with `WAIT_INFINITE`: nobody frees
memory, so only the destruction can end the wait. Task 0 then destroys the pool while it is waited on, and overwrites both the
storage of the pool object and the external buffer with `0xDD`, so that any access to the destroyed pool by a woken waiter would
read garbage, and any write would be detected. A `BytePool` waiter is normally woken to **re-scan** its request, and a scan merges
the free blocks it passes; a waiter woken by the destruction must not do that, which the untouched buffer proves. Task 1
records the result, the elapsed time and whether it got a block, without touching the destroyed pool. It then performs a
`TimedAllocEx(&blk, 56, 20)` on `g_Pool`, which task 0 has drained right after the destruction: the wait must run to its
deadline and return `WAIT_RESULT_TIMEOUT` without a block; a stale wait entry left behind would end it early or with another
result. Task 0 returns the blocks of `g_Pool` at about tick 100 and checks the outcome at about tick 140. If a waiter was not
released, task 0 cancels its wait with `g_Kernel.CancelTaskWait()`, so the test fails instead of hanging the kernel.

**Common pass condition:** the dedicated pool was exhausted before the destruction; task 1's elapsed time is in `[35, 75]` ms
(waiting from ~10 ms, destroyed at ~60 ms) and it got no block; the follow-up `TimedAllocEx(20)` on `g_Pool` returned
`WAIT_RESULT_TIMEOUT` without a block after at least 15 ms; `IsAllFree()` for `g_Pool` at the end (no leak); every byte of the
storage of the destroyed pool object and of its external buffer still holds `0xDD`

#### Test 28 — `DeleteWithWaiter`
**Param:** `DW_EX` &nbsp;|&nbsp; Task 2 is added by `RunTest()` but stays idle.

Task 1 blocks in `TimedAllocEx(&blk, 56, WAIT_INFINITE)`.

**Pass condition:** common, and the result is `WAIT_RESULT_DELETED` (not a block, a timeout or a cancellation)

#### Test 29 — `DeleteWithWaiterTimed`
**Param:** `DW_TIMED` &nbsp;|&nbsp; Task 2 is added by `RunTest()` but stays idle.

Task 1 uses `TimedAlloc(56, WAIT_INFINITE)`, which maps every non-`SIGNAL` result to `nullptr`. An infinite wait can not time out
and nobody cancels it, so `nullptr` after ~50 ms identifies the destruction.

**Pass condition:** common, and `TimedAlloc()` returned `nullptr`

#### Test 30 — `DeleteWithWaiterTwo`
**Param:** `DW_TWO`

Task 1 blocks first for one block (56 bytes), task 2 blocks behind it (`Sleep(20)`, then `TimedAllocEx(&blk, 120, WAIT_INFINITE)`) for
the whole pool, the largest valid request. The waiters have different request sizes, as in `WakeAllWaiters`: the destruction must
release both whatever they asked for, and the wait list of the pool is walked completely, not just up to its first entry.

**Pass condition:** common, and task 1 and task 2 both get `WAIT_RESULT_DELETED` without a block; the elapsed time of task 2 is in
`[25, 65]` ms (waiting from ~20 ms)

The plain `Alloc()` is not covered: it has no result code.

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `TryAllocFreeTask` | 0 only | all accounting checks pass | Initial state, `TryAlloc()` / `Free()` cycle, `GetInfo()` accounting, whole-pool allocation |
| 2 | `ExhaustPoolTask` | 0 only | 8 non-overlapping blocks; 9th fails; `IsAllFree()` after drain | Pool exhaustion, `NO_WAIT` reports `TIMEOUT`, address-order frees merge lazily |
| 3 | `SplitCoalesceTask` | 0 only | placement, hole reuse, merge, no tiny split | First-fit, splitting, forward merge, hole reuse, no split of a remainder below the minimum block |
| 4 | `BlockingAllocTask` | 0–1 | `counter == 1` and `IsAllFree()` | `Alloc()` blocks when the pool is exhausted and is woken by `Free()` via `ConditionVariable` |
| 5 | `BlockingLargeTask` | 0–1 | still blocked after 1st free; done after 2nd; `IsAllFree()` | A large waiter re-checks after every wake-up and needs adjacent memory |
| 6 | `WakeAllWaitersTask` | 0–2 | small waiter done first; large waiter done later | `Free()` wakes all waiters, each with its own request size |
| 7 | `TimedAllocTimeoutTask` | 0–1 | `TIMEOUT`, `nullptr`, elapsed in `[45, 65]` ms | `TimedAllocEx()` expires correctly when no memory becomes available |
| 8 | `TimedAllocSuccessTask` | 0–1 | non-`nullptr` returned | `TimedAlloc()` succeeds when memory is freed within the timeout window |
| 9 | `TimedAllocDeadlineTask` | 0–1 | `TIMEOUT`, elapsed in `[95, 125]` ms | Unsuccessful wake-ups do not restart the timeout |
| 10 | `ConcurrentAllocFreeTask` | 0–4 | `counter == 100`, `g_Errors == 0`, `IsAllFree()` | No overlap, corruption or leak under five-task concurrency with varying sizes |
| 11 | `TypedAllocTask` | 0 only | non-`nullptr` from all three; fields preserved; `IsAllFree()` | `AllocT<T>()`, `TryAllocT<T>()`, `TimedAllocT<T>()` return correct typed pointers |
| 12 | `FreeNullTask` | 0 only | `false` returned; `IsAllFree()` | `Free(nullptr)` is a safe no-op that does not corrupt pool state |
| 13 | `InvalidRequestTask` | 0 only | invalid requests fail in `< 20` ms; max request succeeds | Zero-sized and oversized requests never block |
| 14 | `AlignHelpersTask` | 0 only | all helper checks pass | `AlignSize()`, `RequiredSize()`, `HEADER_SIZE`, `GetSize()`, `GetMaxRequest()` |
| 15 | `StorageModeTask` | 0 only | all accessor checks pass for every storage mode | External, heap, minimal (16-byte) and misaligned buffers |
| 16 | `StressTask` | 0–4 | `counter > 0`, `g_Errors == 0`, `IsAllFree()` | No corruption, leak, or deadlock under five-task contention mixing `TryAlloc()`, `Alloc()`, `TimedAlloc()` with varying sizes — runs on all platforms |
| 17 | `CancelAllocTask` (`Infinite`) | 0–2 | `CANCELED`, no block, elapsed in `[35, 75]` ms, `IsExhausted()` | `CancelTaskWait()` ends an infinite `TimedAllocEx()` without allocating |
| 18 | `CancelAllocTask` (`NotTimeout`) | 0–2 | `CANCELED`, elapsed `< 150` ms | Cancel is reported as `CANCELED`, not `TIMEOUT`, and ends a finite wait early |
| 19 | `CancelAllocTask` (`PlainAlloc`) | 0–2 | `Alloc() == nullptr`, elapsed in `[35, 75]` ms | `Alloc()` returns `nullptr` when cancelled |
| 20 | `CancelAllocTask` (`NoStaleWaiter`) | 0–2 | `Free()` ok, `UsedBlocks() == 7`, block re-allocatable once, victim sleep `>= 55` ms | Cancelled waiter is unlinked; no memory lost to a ghost waiter |
| 21 | `CancelAllocTask` (`TwoWaiters`) | 0–2 | task 1 `CANCELED`, task 2 `SIGNAL` with block, `UsedBlocks() == 7` | Cancelling one waiter leaves the other's wait list entry intact |
| 22 | `CancelAllocTask` (`SignalWins`) | 0–2 | `SIGNAL` with block, `UsedBlocks() == 7` | A `Free()` that arrived first wins over a late cancel |
| 23 | `CancelAllocTask` (`SleepCancel`) | 0–2 | same as 17 | `SleepCancel()` cancels a pool wait like test 17 |
| 24 | `CancelAllocTask` (`SleepCancelPlain`) | 0–2 | same as 19 | `SleepCancel()` with plain `Alloc()` |
| 25 | `CancelAllocTask` (`SleepCancelNoStale`) | 0–2 | same as 20 | `SleepCancel()` leaves no stale wait entry |
| 26 | `CancelAllocTask` (`SleepCancelTwo`) | 0–2 | same as 21 | `SleepCancel()` keeps the second waiter intact |
| 27 | `CancelAllocTask` (`SleepCancelSignal`) | 0–2 | same as 22 | `SleepCancel()` after a `Free()` is a no-op |
| 28 | `DeleteWithWaiterTask` (`DeleteWithWaiter`) | 0–2 | `DELETED`, no block, elapsed `[35, 75]` ms, follow-up wait on `g_Pool` times out `>= 15` ms, `IsAllFree()`, pool object and buffer untouched | Destroying a pool which a task waits on releases it with `WAIT_RESULT_DELETED`; no block is handed out; the waiter neither writes to the destroyed pool nor re-scans its blocks, and leaves no stale wait entry |
| 29 | `DeleteWithWaiterTask` (`DeleteWithWaiterTimed`) | 0–2 | `TimedAlloc()` returns `nullptr` after `[35, 75]` ms, as test 28 | `TimedAlloc()` maps a destruction to `nullptr`; an infinite wait can not time out, so it identifies the destruction |
| 30 | `DeleteWithWaiterTask` (`DeleteWithWaiterTwo`) | 0–2 | both `DELETED`, task 2 elapsed `[25, 65]` ms, as test 28 | The destruction releases every waiter whatever its request size, not only the first one |
