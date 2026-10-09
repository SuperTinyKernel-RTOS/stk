# Mutex Test Suite

Test suite for `stk::sync::Mutex`.  
Source: `test/mutex/test_mutex.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_mutex.h>
```

`Mutex` is a **recursive** mutex: the owning task may call `Lock()` again without
deadlocking. Each nested acquisition increments an internal recursion counter; the
lock is only fully released when `Unlock()` has been called an equal number of times.

Ownership is tracked by task ID (`m_owner_tid`). On `Unlock()`, if other tasks are
waiting, ownership is transferred directly to the first waiter in FIFO order — the
count is set to `1` and `m_owner_tid` is updated atomically before the waiter is
woken, so the waiter owns the lock before it even resumes execution.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

The maximum nesting depth is `sync::Mutex::RECURSION_MAX` (`0xFFFE`); exceeding it is a contract violation.

```cpp
// Construction
stk::sync::Mutex g_Mtx;    // unlocked, no owner
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Lock` | `void Lock()` | Acquires the lock, blocking indefinitely. If already owned by the calling task, increments recursion count and returns immediately. ISR-unsafe. |
| `TryLock` | `bool TryLock()` | Acquires the lock without blocking (`timeout == NO_WAIT`). Returns `true` if acquired, `false` if held by another task. Recursive re-entry by the owner always returns `true`. ISR-unsafe. |
| `TimedLock` | `bool TimedLock(Timeout timeout)` | Acquires the lock, blocking up to `timeout` ticks. Returns `true` if acquired, `false` on timeout. `timeout == 0` is equivalent to `TryLock()`. ISR-unsafe. |
| `Unlock` | `void Unlock()` | Decrements recursion count. When count reaches zero, transfers ownership to the first waiter (FIFO) or marks the mutex free. Asserts the caller is the current owner. ISR-unsafe. |
| `GetOwner` | `TId GetOwner() const` | Returns the owner's task ID, or `TID_NONE` if free. ISR-safe. |
| `GetRecursionCount` | `uint16_t GetRecursionCount() const` | Returns the current recursion depth: `0` if free, otherwise the number of nested `Lock()` calls made by the owner (`1..RECURSION_MAX`). Stable only for the owner; a snapshot for any other caller. ISR-safe. |
| `~Mutex` | (destructor) | Asserts `m_wait_list.IsEmpty()` in debug builds. Destroying a mutex with active waiters is a logic error. |

**Lock paths in `TimedLock()`:**

```
caller == owner (recursive)  →  ++m_count, return true immediately
m_count == 0   (free)        →  m_count = 1, m_owner_tid = caller, return true immediately
timeout == 0   (try-lock)    →  return false immediately (NO_WAIT fast-fail)
otherwise      (contended)   →  block until Unlock() transfers ownership or timeout
```

**Ownership transfer in `Unlock()`:**

```
--m_count > 0          →  still held recursively, return (no release)
--m_count == 0, waiters present  →  m_count = 1, m_owner_tid = waiter->GetTid(), Wake(waiter)
--m_count == 0, no waiters       →  m_owner_tid = 0 (fully free)
```

**Key invariants:**

- The same task may acquire the lock multiple times without deadlocking; each `Lock()`
  must be paired with exactly one `Unlock()`.
- `Unlock()` asserts ownership — only the task that holds the lock may release it.
- After ownership transfer in `Unlock()`, `m_count == 1` and `m_owner_tid` is already
  set to the woken task's ID before that task resumes.
- Destroying a `Mutex` while tasks are waiting is a logic error; an assertion fires
  in debug builds.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_MUTEX_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_MUTEX_TEST_TIMEOUT` | `1000` ticks | Blocking timeout for `TimedLock()` calls that must succeed |
| `_STK_MUTEX_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_MUTEX_TEST_LONG_SLEEP` | `100` ticks | Sleep used by verifier tasks to wait for workers |
| `_STK_MUTEX_TIMED_LOCK_TIMEOUT` | `50` ticks | Timeout used by `TimedLockTask` (test 4) |
| `_STK_MUTEX_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

`g_TestMutex` is a plain static — unlike other test suites, `ResetTestState()` does
**not** reconstruct it via placement-new. The mutex is shared across all test runs in
its naturally unlocked state after each test completes. `ResetTestState()` resets only
the counters and flags (`g_TestResult`, `g_SharedCounter`, `g_ExpectedCounter`, `g_OrderIndex`,
`g_InstancesDone`, `g_Task1Tid`, `g_Task1Acquired`, `g_WaiterTask`, `g_AcquisitionOrder[]`).

Because the mutex cannot be reset from outside, `RunTest()` guards against a failing test
poisoning the next ones: it fails a test immediately (without running it) if the mutex was left
locked by the previous test (`IsMutexFree()`: no owner and recursion depth 0), and fails a test
that finishes with the mutex still locked.

Task completion is counted with `MarkDone()`, which increments `g_InstancesDone` inside a
`sync::ScopedCriticalSection`, so that a preempted read-modify-write cannot lose an update and
leave a verifier task waiting forever.

The number of tasks added per test is selected by test name in `RunTest()`: tasks 0–1 are always
added; task 2 is added unless `IsTwoTaskTest()` is true; tasks 3–4 are added unless
`IsTwoTaskTest()` is true or the test is `TimedLock` (tasks 0–2 only). Two-task tests are
`TryLock`, `OwnerState`, `OwnershipHandoff`, `CancelledWait`, `RecursiveContended` and
`RecursionMax`; all other tests except `TimedLock` use all five tasks.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link all the
distinct task class templates simultaneously. Tests 1–13 are skipped on M0 and only
`StressTest` (test 14) runs, under `#ifndef __ARM_ARCH_6M__`.

`StressTest` runs on M0 because it uses a single task class template (`StressTestTask`)
instantiated for all five task slots, fitting within the available memory.

| Platform | `_STK_MUTEX_STACK_SIZE` |
|----------|-------------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

All task classes, including `RecursiveDepthTask` (`DEPTH = 3`), use `_STK_MUTEX_STACK_SIZE`.

---

## Tests

### Test 1 — `BasicLockUnlock`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Param:** `iterations = 100`

All five tasks race to increment `g_SharedCounter` inside a `Lock()` / `Unlock()`
critical section for 100 iterations each. The increment is deliberately non-atomic:
each task reads `temp = g_SharedCounter`, then writes `g_SharedCounter = temp + 1`,
with a `Delay(1)` injected every 4th increment to widen the race window. Task 0
uses a `g_InstancesDone` completion barrier to wait for all five tasks to finish
before verifying the total. If mutual exclusion is broken, any concurrent read-modify-
write will produce a lower total than expected.

**Pass condition:** `counter == 500` (`5 tasks × 100 iterations`)

---

### Test 2 — `RecursiveLock`
**Tasks:** 0–4 (all 5)

Each task acquires the mutex three times in nested scope (recursion depth 3), increments
`g_SharedCounter` in the innermost scope, then releases three times. Verifies that the
recursive re-entry path in `TimedLock()` (`m_owner_tid == current_tid`) increments
`m_count` and returns immediately without blocking, and that full release only occurs
after the matching number of `Unlock()` calls. Task 0 uses a completion barrier.

**Pass condition:** `counter == 5` (each of 5 tasks incremented exactly once)

---

### Test 3 — `TryLock`
**Tasks:** 0–1 (2 tasks)

Task 0 acquires the mutex with `Lock()`, sets `g_SharedCounter = 1`, then sleeps
`_STK_MUTEX_TEST_LONG_SLEEP` ticks while holding it. Task 1 sleeps briefly to let
task 0 establish ownership, then calls `TryLock()` and measures elapsed time. Since
the mutex is held by another task, `TryLock()` must return `false` immediately
(elapsed < `_STK_MUTEX_TEST_SHORT_SLEEP`). Task 1 then sleeps `2 × _STK_MUTEX_TEST_LONG_SLEEP`
ticks, by which time task 0 has released the lock, and calls `TryLock()` again: it must now
succeed with `GetOwner() == CurrentTid()` and depth 1. Finally, while owning the lock, task 1
calls `TryLock()` once more: the recursive re-entry by the owner must succeed and raise the depth to 2.

**Pass condition:** first `TryLock()` returned `false` with elapsed < `SHORT_SLEEP`, second
succeeded with owner/depth correct, recursive `TryLock()` succeeded with depth 2, and the mutex
is free at the end

---

### Test 4 — `TimedLock`
**Tasks:** 0–2 active (tasks 3–4 present but idle)

Task 0 holds the mutex for 200 ticks. Task 1 sleeps `_STK_MUTEX_TEST_SHORT_SLEEP`
then calls `TimedLock(50)`; with the mutex still held by task 0, it must time out and
return `false` with elapsed in `[_STK_MUTEX_TIMED_LOCK_TIMEOUT - 1, _STK_MUTEX_TIMED_LOCK_TIMEOUT + 25]` =
`[49, 75]` ms (the lower bound tolerates truncation of the two millisecond timestamps but rejects an
early wakeup; the upper bound tolerates tick alignment and scheduling jitter). Task 2 sleeps until tick 250 (after task
0 releases) then calls `TimedLock(100)`; the mutex is now free so it must succeed and
increment the counter. Task 2 is the verifier and sleeps `_STK_MUTEX_TEST_LONG_SLEEP`
before checking.

**Pass condition:** `counter == 2` and the mutex is free
(1 = `TimedLock(50)` timed out correctly; 2 = `TimedLock(100)` succeeded after release)

---

### Test 5 — `FIFOOrder`
**Tasks:** 0–4 (all 5)

Task 0 acquires the mutex and holds it for 50 ticks while tasks 1–4 queue up. Tasks
1–4 stagger their entry into `Lock()` by sleeping `SHORT_SLEEP × m_task_id` ticks,
ensuring they join the wait list in ascending task-id order (1, 2, 3, 4). As each
consumer acquires and immediately releases the lock, it records its `m_task_id` into
`g_AcquisitionOrder[g_OrderIndex++]`. Task 4 uses a `g_InstancesDone` completion
barrier then verifies the array is exactly `[1, 2, 3, 4]`.

**Pass condition:** `g_AcquisitionOrder == [1, 2, 3, 4]`

---

### Test 6 — `RecursiveDepth`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Depth:** `DEPTH = 3`

Each task calls `RecursiveLock(3)` — a recursive function that calls `Lock()`,
recurses one level deeper, increments `g_SharedCounter` on the way back up, then
calls `Unlock()`. This produces 3 nested acquisitions and 3 releases per task.
Verifies that the recursive path handles call depth correctly and that
`m_count` tracks each level precisely. Task 0 sleeps `_STK_MUTEX_TEST_LONG_SLEEP`
before verifying.

**Pass condition:** `counter == 15` (`5 tasks × 3 depth levels`)

---

### Test 7 — `InterTaskCoordination`
**Tasks:** 0–4 (all 5)

All five tasks increment `g_SharedCounter` in strict round-robin turn order for 10
rounds. Each task holds the mutex, checks `g_SharedCounter % 5 == m_task_id`, and
if not its turn, releases the mutex, yields with `Delay(1)`, and re-acquires — busy-
waiting under the mutex without a condition variable. This forces tasks to take turns
in exact task-id order (0, 1, 2, 3, 4, 0, 1, …). Task 4 uses a `g_InstancesDone`
completion barrier then verifies.

**Pass condition:** `counter == 50` (`10 rounds × 5 tasks`)

---

### Test 8 — `OwnerState`
**Tasks:** 0–1 (2 tasks)

Verifies `GetOwner()` and `GetRecursionCount()` through the full state cycle: free → owned by task 0
at depth 1 → depth 2 (visible to task 1) → depth 1 → free. Task 0 checks the initial state
(`GetOwner() == TID_NONE`, depth 0), locks twice checking owner and depth after each call, sleeps
50 ticks so task 1 can observe it, then unlocks once (owner unchanged, depth 1) and once more (free,
depth 0). Task 1 sleeps `SHORT_SLEEP`, then observes from a non-owner that the owner is set, is not
itself, and the depth is 2; after `LONG_SLEEP` more ticks it checks the mutex is free. Each task
increments `g_SharedCounter` only if all of its checks passed.

**Pass condition:** `counter == 2` (both the owner's and the observer's views were correct)

---

### Test 9 — `OwnershipHandoff`
**Tasks:** 0–1 (2 tasks)

Verifies that `Unlock()` hands ownership directly to the first waiter, with a fresh recursion depth,
so the releasing task cannot barge back in. Task 0 locks the mutex and sleeps 50 ticks. Task 1 sleeps
`SHORT_SLEEP`, records its ID in `g_Task1Tid`, and blocks in `Lock()`. When task 0 calls `Unlock()`, it
immediately checks that `GetOwner()` equals task 1's ID and the depth is 1. It then calls `TryLock()`:
this must fail; if it succeeds (barging) the test unlocks it again so the mutex is not left locked for
later tests, and fails. Task 1 holds the lock for 50 more ticks so task 0's checks can run, confirms it
is the owner with depth 1, and unlocks.

**Pass condition:** task 1 was owner with depth 1 after waking, task 0's handoff checks passed
(`counter == 1`), and the mutex is free at the end

---

### Test 10 — `CancelledWait`
**Tasks:** 0–1 (2 tasks)

Verifies that a wait cancelled with `IKernel::CancelTaskWait()` fails cleanly, including while the
owner holds the lock recursively. Task 0 locks the mutex twice (depth 2) and sleeps 50 ticks. Task 1
sleeps `SHORT_SLEEP`, publishes its `ITask` pointer in `g_WaiterTask` and blocks in
`TimedLock(WAIT_INFINITE)`. Task 0 then calls `g_Kernel.CancelTaskWait()` for task 1 and sleeps 20
more ticks. Task 1 must get `false` and must not own the mutex; task 0 must still be the owner at
depth 2, then depth 1 after one `Unlock()`. After the final `Unlock()` the mutex must be free (the
cancelled task was removed from the wait list, so ownership was not handed to it). Task 1 finally
sleeps `LONG_SLEEP` and checks the mutex is usable again: `TryLock()` succeeds with depth 1.

**Pass condition:** `counter == 2` (task 0's owner/depth/free checks passed, task 1's cancelled-wait
and re-lock checks passed)

---

### Test 11 — `MiddleTimeout`
**Tasks:** 0–4 (all 5)

Verifies that a waiter timing out in the middle of the wait list does not disturb the others. Task 0
holds the mutex for 100 ticks. Task 1 (at tick 10) and task 3 (at tick 30) block in `Lock()`; task 2
(at tick 20) calls `TimedLock(30)` between them and times out while the lock is still held. When task 0
unlocks, the lock must go to task 1, then to task 3, skipping task 2. Each acquirer records its id in
`g_AcquisitionOrder[]`; task 2 increments `g_SharedCounter` when it times out. Task 4 uses a
`g_InstancesDone` completion barrier then verifies.

**Pass condition:** exactly 2 acquisitions in order `[1, 3]`, `counter == 1` (task 2 timed out),
and the mutex is free

---

### Test 12 — `RecursiveContended`
**Tasks:** 0–1 (2 tasks)

Verifies that a nested `Unlock()` does not release the mutex to a waiter. Task 0 locks twice (depth 2)
and sleeps 50 ticks; task 1 records its ID in `g_Task1Tid` and blocks in `Lock()`. Task 0 unlocks once
and sleeps 20 ticks (task 1 would run now if the mutex was released prematurely), then checks that
task 1 has not acquired the lock (`g_Task1Acquired == false`) and that it still owns the mutex at
depth 1. It then unlocks the second time and checks that ownership passed to task 1 with a fresh depth
of 1. Task 1 holds the lock for 30 ticks so these checks can run, confirms it is the owner at depth 1,
and unlocks.

**Pass condition:** premature-release check and handoff check passed in task 0 (`counter == 1`),
task 1 was owner with depth 1, and the mutex is free at the end

---

### Test 13 — `RecursionMax`
**Tasks:** 0–1 (2 tasks)

Task 0 locks the mutex exactly `sync::Mutex::RECURSION_MAX` (`0xFFFE`) times (one more would be a
contract violation), checks that `GetOwner()` is itself and `GetRecursionCount() == RECURSION_MAX`,
then unlocks the same number of times.

**Pass condition:** owner and depth correct at the maximum, and the mutex is free after the matching
number of `Unlock()` calls

---

### Test 14 — `StressTest`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 400`

All five tasks run 400 iterations each, cycling through three lock strategies by
iteration index: `i % 3 == 0` uses `Lock()` / `Unlock()` (always succeeds),
`i % 3 == 1` uses `TryLock()` (may fail under contention),
`i % 3 == 2` uses `TimedLock(10)` (may time out under contention).
A `Delay(1)` is inserted every 10 iterations to allow other tasks to run.
Each successful acquisition increments `g_SharedCounter` and a task-local `successes`
count before releasing. When its loop ends, each task adds its `successes` to
`g_ExpectedCounter` under the mutex. Task 4 uses a `g_InstancesDone` completion barrier
and compares the two totals. The check is exact: every successful acquisition must be
accounted for, so a lost update is detected.

**Pass condition:** `counter > 0`, `counter == g_ExpectedCounter` (no lost updates), and the mutex is free

---

## Summary Table

| # | Test | Tasks | Stack | Pass condition | What it verifies |
|---|------|-------|-------|----------------|------------------|
| 1 | `BasicLockUnlockTask` | 0–4 | `_STK_MUTEX_STACK_SIZE` | `counter == 500` | `Lock()` / `Unlock()` provides mutual exclusion; no increment lost under deliberate race |
| 2 | `RecursiveLockTask` | 0–4 | `_STK_MUTEX_STACK_SIZE` | `counter == 5` | Recursive re-entry (depth 3) returns immediately without blocking; full release after matching `Unlock()` count |
| 3 | `TryLockTask` | 0–1 | `_STK_MUTEX_STACK_SIZE` | fails while held (elapsed < `SHORT_SLEEP`), succeeds when free, recursive re-entry succeeds | `TryLock()` returns `false` immediately when held by another task, succeeds once released, and always succeeds for the owner |
| 4 | `TimedLockTask` | 0–2 | `_STK_MUTEX_STACK_SIZE` | `counter == 2`, mutex free | `TimedLock()` times out in `[49, 75]` ms when contended; succeeds when mutex is free |
| 5 | `FIFOOrderTask` | 0–4 | `_STK_MUTEX_STACK_SIZE` | order `[1,2,3,4]` | Blocked tasks are granted ownership in FIFO arrival order |
| 6 | `RecursiveDepthTask` | 0–4 | `_STK_MUTEX_STACK_SIZE` | `counter == 15` | Recursive locking to depth 3 tracks `m_count` correctly across all levels and all tasks |
| 7 | `InterTaskCoordinationTask` | 0–4 | `_STK_MUTEX_STACK_SIZE` | `counter == 50` | Mutex correctly gates strict round-robin turn-taking across 10 rounds without a condition variable |
| 8 | `OwnerStateTask` | 0–1 | `_STK_MUTEX_STACK_SIZE` | `counter == 2` | `GetOwner()` / `GetRecursionCount()` report correctly for owner and non-owner across free → depth 2 → depth 1 → free |
| 9 | `OwnershipHandoffTask` | 0–1 | `_STK_MUTEX_STACK_SIZE` | handoff checks pass, `counter == 1`, mutex free | `Unlock()` passes ownership directly to the first waiter with depth 1; the releasing task cannot barge back in |
| 10 | `CancelledWaitTask` | 0–1 | `_STK_MUTEX_STACK_SIZE` | `counter == 2` | A wait cancelled via `CancelTaskWait()` returns `false`, does not acquire the lock, does not disturb the owner's depth and leaves no dangling waiter |
| 11 | `MiddleTimeoutTask` | 0–4 | `_STK_MUTEX_STACK_SIZE` | order `[1,3]`, `counter == 1`, mutex free | A waiter timing out mid-queue is skipped; the remaining waiters keep FIFO order |
| 12 | `RecursiveContendedTask` | 0–1 | `_STK_MUTEX_STACK_SIZE` | checks in tasks 0 and 1 pass, mutex free | A nested `Unlock()` does not release to a waiter; the final `Unlock()` hands over with depth 1 |
| 13 | `RecursionMaxTask` | 0–1 | `_STK_MUTEX_STACK_SIZE` | depth == `RECURSION_MAX`, mutex free after unlocks | Locking to the maximum depth and unwinding back to free |
| 14 | `StressTestTask` | 0–4 | `_STK_MUTEX_STACK_SIZE` | `counter > 0`, `counter == expected`, mutex free | No corruption, lost update or deadlock under full five-task contention mixing `Lock()`, `TryLock()`, and `TimedLock(10)`; runs on all platforms |
