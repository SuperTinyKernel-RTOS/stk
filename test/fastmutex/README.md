# FastMutex Test Suite

Test suite for `stk::sync::FastMutex`.  
Source: `test/fastmutex/test_fastmutex.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_fastmutex.h>
```

`FastMutex` is a **non-recursive** (binary) mutex: the only state is the owner task ID
(`m_owner_tid`, `TID_NONE` when free). There is no recursion counter, which keeps the
lock/unlock paths shorter and the object smaller than `stk::sync::Mutex`.

A task that already owns the mutex must not lock it again. This is an unrecoverable contract
violation: `Lock()`, `TryLock()` and `TimedLock()` call
`STK_KERNEL_PANIC(KERNEL_PANIC_SYNC_DEADLOCK)` in all build configurations (the panic handler
never returns), instead of dead-locking on themselves or failing silently.

On `Unlock()` with no waiters the mutex is simply marked free, without any kernel call (priority
restore and handover are done only on the contended path). If other tasks are waiting, ownership is transferred directly to the first
waiter in FIFO order — `m_owner_tid` is updated before the waiter is woken, so the waiter
owns the lock before it even resumes execution. Priority inheritance is supported in the
same way as in `Mutex`.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// Construction
stk::sync::FastMutex g_Mtx;    // unlocked, no owner
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Lock` | `void Lock()` | Acquires the lock, blocking indefinitely. Must not be called by the current owner. ISR-unsafe. |
| `TryLock` | `bool TryLock()` | Acquires the lock without blocking (`timeout == NO_WAIT`). Returns `true` if acquired, `false` if held (including by the caller). ISR-safe. |
| `TimedLock` | `bool TimedLock(Timeout timeout)` | Acquires the lock, blocking up to `timeout` ticks. Returns `true` if acquired, `false` on timeout (or if the caller already owns the lock). ISR-safe only with `NO_WAIT`. |
| `Unlock` | `void Unlock()` | Transfers ownership to the first waiter (FIFO) or marks the mutex free. Asserts the caller is the current owner. ISR-safe. |
| `GetOwner` | `TId GetOwner() const` | Returns the owner's task ID, or `TID_NONE` if free. ISR-safe. |
| `IsLocked` | `bool IsLocked() const` | Returns `true` if the mutex has an owner. Snapshot only — may be stale immediately for a non-owner. ISR-safe. |
| `~FastMutex` | (destructor) | Asserts `m_wait_list.IsEmpty()` in debug builds. Destroying a mutex with active waiters is a logic error. |

**Lock paths in `TimedLock()`:**

```
owner == TID_NONE (free)       →  m_owner_tid = caller, return true immediately
owner == caller   (self-lock)  →  contract violation: STK_KERNEL_PANIC(KERNEL_PANIC_SYNC_DEADLOCK)
timeout == NO_WAIT (try-lock)  →  return false immediately (fast-fail)
otherwise         (contended)  →  block until Unlock() transfers ownership or timeout
```

**Ownership transfer in `Unlock()`:**

```
waiters present  →  m_owner_tid = waiter->GetTid(), Wake(waiter)
no waiters       →  m_owner_tid = TID_NONE (fully free)
```

**Key invariants:**

- Each `Lock()` must be paired with exactly one `Unlock()`; the owner must never re-lock.
- `Unlock()` asserts ownership — only the task that holds the lock may release it.
- After ownership transfer in `Unlock()`, `m_owner_tid` is already set to the woken task's
  ID before that task resumes, so the releasing task cannot barge back in.
- Destroying a `FastMutex` while tasks are waiting is a logic error; an assertion fires
  in debug builds.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_FMUTEX_TEST_TASKS_MAX` | `5` | Maximum tasks per test run (kernel capacity) |
| `_STK_FMUTEX_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_FMUTEX_TEST_LONG_SLEEP` | `100` ticks | Sleep used to let other tasks run before checking |
| `_STK_FMUTEX_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

| `TIMED_LOCK_TIMEOUT` | `50` ticks | Timeout used by `TimedLockTask` (test 3) |

Other timeouts and hold times used by individual tests (`10`, `20`, `30`, `50`, `100`, `200`, `250` ticks)
are literals in the test bodies rather than named constants.

The kernel is `Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | KERNEL_TICKLESS (if STK_TICKLESS_IDLE), 5, SwitchStrategyRR, PlatformDefault>`.

`g_TestMutex` is a plain static — `ResetTestState()` does **not** reconstruct it via
placement-new. The mutex is shared across all test runs in its naturally unlocked state
after each test completes (each test also asserts `!IsLocked()` where it matters).
`ResetTestState()` resets only the counters and flags (`g_TestResult`, `g_SharedCounter`,
`g_ExpectedCounter`, `g_OrderIndex`, `g_InstancesDone`, `g_Task1Tid`, `g_WaiterTask`,
`g_AcquisitionOrder[]`).

Because the mutex cannot be reset from outside, `RunTest()` guards against a failing test
poisoning the next ones: it fails a test immediately (without running it) if the mutex was left
locked by the previous test, and fails a test that finishes with the mutex still locked.

Task completion is counted with `MarkDone()`, which increments `g_InstancesDone` inside a
`sync::ScopedCriticalSection`, so that a preempted read-modify-write cannot lose an update and
leave a verifier task waiting forever.

Unlike the `Mutex` suite, tasks are added per test through the `tasks_count` argument of
`RunTest()`: tasks 0–1 are always added, task 2 when `tasks_count > 2`, and tasks 3–4
when `tasks_count > 3`.

There are no recursion tests: `FastMutex` is non-recursive. The self-lock path is a contract
violation that triggers `STK_KERNEL_PANIC(KERNEL_PANIC_SYNC_DEADLOCK)`; the panic handler never
returns, so this path can not be exercised by the suite.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link all the
distinct task class templates simultaneously. Tests 1–9 are skipped on M0 and only
`StressTest` (test 10) runs, under `#ifndef __ARM_ARCH_6M__`.

`StressTest` runs on M0 because it uses a single task class template (`StressTestTask`)
instantiated for all five task slots, fitting within the available memory.

On M0 the `STK_TASK` macro expands to nothing, so tasks 0 and 1 are automatic (stack)
objects; on all other platforms they are `static`.

| Platform | `_STK_FMUTEX_STACK_SIZE` | `STK_TASK` |
|----------|--------------------------|------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words | *(empty)* |
| All others | `256` words | `static` |

---

## Tests

### Test 1 — `BasicLockUnlock`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Param:** `iterations = 100`

All five tasks race to increment `g_SharedCounter` inside a `Lock()` / `Unlock()`
critical section for 100 iterations each. The increment is deliberately non-atomic:
each task reads `temp = g_SharedCounter`, then writes `g_SharedCounter = temp + 1`,
with a `Delay(1)` injected inside the critical section every 4th increment to widen the
race window, and a `Yield()` after every `Unlock()`. Task 0 uses a `g_InstancesDone`
completion barrier to wait for all five tasks to finish before verifying. If mutual
exclusion is broken, any concurrent read-modify-write will produce a lower total than
expected.

**Pass condition:** `counter == 500` (`5 tasks × 100 iterations`) and `!IsLocked()`

---

### Test 2 — `TryLock`
**Tasks:** 0–1 (2 tasks)

Task 0 acquires the mutex with `Lock()` and sleeps `_STK_FMUTEX_TEST_LONG_SLEEP` ticks
while holding it. Task 1 sleeps briefly to let task 0 establish ownership, then calls
`TryLock()` and measures elapsed time. Since the mutex is held by another task,
`TryLock()` must return `false` immediately (elapsed < `_STK_FMUTEX_TEST_SHORT_SLEEP`).
Task 1 then sleeps `2 × _STK_FMUTEX_TEST_LONG_SLEEP` ticks, by which time task 0 has
released the lock, and calls `TryLock()` again: it must now succeed and `GetOwner()` must
equal the calling task's ID.

**Pass condition:** first `TryLock()` returned `false` with elapsed < `SHORT_SLEEP`, **and**
second `TryLock()` returned `true` with `GetOwner() == CurrentTid()`

---

### Test 3 — `TimedLock`
**Tasks:** 0–2 (3 tasks)

Task 0 holds the mutex for 200 ticks. Task 1 sleeps `_STK_FMUTEX_TEST_SHORT_SLEEP`
then calls `TimedLock(50)`; with the mutex still held by task 0, it must time out and
return `false` with elapsed in `[TIMED_LOCK_TIMEOUT - 1, TIMED_LOCK_TIMEOUT + 25]` = `[49, 75]` ms
(the lower bound tolerates truncation of the two millisecond timestamps but rejects an early wakeup; the
upper bound tolerates tick alignment and scheduling jitter). Task 2 sleeps until tick 250 (after task
0 releases) then calls `TimedLock(100)`; the mutex is now free so it must succeed, own
the lock (`GetOwner() == CurrentTid()`) and increment the counter. Task 2 is the verifier
and sleeps `_STK_FMUTEX_TEST_LONG_SLEEP` before checking. This also verifies that the
timed-out waiter from task 1 left the mutex in a usable state.

**Pass condition:** `counter == 2` and `!IsLocked()`
(1 = `TimedLock(50)` timed out correctly; 2 = `TimedLock(100)` succeeded after release)

---

### Test 4 — `FIFOOrder`
**Tasks:** 0–4 (all 5)

Task 0 acquires the mutex and holds it for 50 ticks while tasks 1–4 queue up. Tasks
1–4 stagger their entry into `Lock()` by sleeping `SHORT_SLEEP × m_task_id` ticks,
ensuring they join the wait list in ascending task-id order (1, 2, 3, 4). As each
consumer acquires and immediately releases the lock, it records its `m_task_id` into
`g_AcquisitionOrder[g_OrderIndex++]`. Task 4 uses a `g_InstancesDone` completion
barrier then verifies the array is exactly `[1, 2, 3, 4]`.

**Pass condition:** `g_AcquisitionOrder == [1, 2, 3, 4]`

---

### Test 5 — `OwnerState`
**Tasks:** 0–1 (2 tasks)

Verifies `GetOwner()` and `IsLocked()` through the full state cycle: free → owned by
task 0 (visible to task 1) → free again. Task 0 first checks the initial state
(`!IsLocked()`, `GetOwner() == TID_NONE`), calls `Lock()`, checks that it is now locked
and owned by itself, sleeps 50 ticks so task 1 can observe it, calls `Unlock()`, and checks
that the mutex is free again with no owner. Task 1 sleeps `SHORT_SLEEP`, then observes
from a non-owner that the mutex is locked, has an owner, and that owner is not itself;
after `LONG_SLEEP` more ticks it checks the mutex is free. Each task increments
`g_SharedCounter` only if all of its checks passed.

**Pass condition:** `counter == 2` (both the owner's and the observer's views were correct)

---

### Test 6 — `OwnershipHandoff`
**Tasks:** 0–1 (2 tasks)

Verifies that `Unlock()` hands ownership directly to the first waiter, so the releasing
task cannot barge back in. Task 0 locks the mutex and sleeps 50 ticks. Task 1 sleeps
`SHORT_SLEEP`, records its ID in `g_Task1Tid`, and blocks in `Lock()`. When task 0 calls
`Unlock()`, it immediately checks that the mutex is still locked, that `GetOwner()` equals
task 1's ID, and that its own `TryLock()` fails; if so it sets `g_SharedCounter = 1`.
Task 1 holds the lock for 50 more ticks so task 0's checks can run, confirms it is the
owner, and unlocks. If ownership was *not* handed off (barging), task 0's `TryLock()` would
succeed: the test then unlocks it again and fails, so the mutex is not left locked for later tests.

**Pass condition:** task 1 was owner after waking, `counter == 1` (task 0's handoff checks
passed), and `!IsLocked()` at the end

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

### Test 8 — `CancelledWait`
**Tasks:** 0–1 (2 tasks)

Verifies that a wait cancelled with `IKernel::CancelTaskWait()` fails cleanly. Task 0 locks
the mutex and sleeps 50 ticks. Task 1 sleeps `SHORT_SLEEP`, publishes its `ITask` pointer in
`g_WaiterTask` and blocks in `TimedLock(WAIT_INFINITE)`. Task 0 then calls
`g_Kernel.CancelTaskWait()` for task 1 and sleeps 20 more ticks. Task 1 must get `false` and must not
own the mutex; task 0 must still be the owner. After task 0 unlocks, the mutex must be free (the
cancelled task was removed from the wait list, so ownership was not handed to it). Task 1 finally
sleeps `LONG_SLEEP` and checks the mutex is usable again: `TryLock()` succeeds and it becomes the owner.

**Pass condition:** `counter == 2` (task 0's ownership/free checks passed, task 1's cancelled-wait and
re-lock checks passed)

---

### Test 9 — `MiddleTimeout`
**Tasks:** 0–4 (all 5)

Verifies that a waiter timing out in the middle of the wait list does not disturb the others. Task 0
holds the mutex for 100 ticks. Task 1 (at tick 10) and task 3 (at tick 30) block in `Lock()`; task 2
(at tick 20) calls `TimedLock(30)` between them and times out while the lock is still held. When task 0
unlocks, the lock must go to task 1, then to task 3, skipping task 2. Each acquirer records its id in
`g_AcquisitionOrder[]`; task 2 increments `g_SharedCounter` when it times out. Task 4 uses a
`g_InstancesDone` completion barrier then verifies.

**Pass condition:** exactly 2 acquisitions in order `[1, 3]`, `counter == 1` (task 2 timed out),
and `!IsLocked()`

---

### Test 10 — `StressTest`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 400`

All five tasks run 400 iterations each, cycling through three lock strategies by
iteration index: `i % 3 == 0` uses `Lock()` / `Unlock()` (always succeeds),
`i % 3 == 1` uses `TryLock()` (may fail under contention),
`i % 3 == 2` uses `TimedLock(10)` (may time out under contention).
A `Delay(1)` is inserted every 10 iterations to allow other tasks to run.
Each successful acquisition increments `g_SharedCounter` and a task-local `successes`
count before releasing. When its loop ends, each task adds its `successes` to
`g_ExpectedCounter` under the mutex. Task 4 uses a `g_InstancesDone` completion barrier
and compares the two totals. Unlike the `Mutex` suite, the check is exact rather than
permissive: every successful acquisition must be accounted for.

**Pass condition:** `counter > 0`, `counter == g_ExpectedCounter` (no lost updates), and `!IsLocked()`

---

## Summary Table

| # | Test | Tasks | Stack | Pass condition | What it verifies |
|---|------|-------|-------|----------------|------------------|
| 1 | `BasicLockUnlockTask` | 0–4 | `_STK_FMUTEX_STACK_SIZE` | `counter == 500`, `!IsLocked()` | `Lock()` / `Unlock()` provides mutual exclusion; no increment lost under deliberate race |
| 2 | `TryLockTask` | 0–1 | `_STK_FMUTEX_STACK_SIZE` | fails while held (elapsed < `SHORT_SLEEP`), succeeds and owns when free | `TryLock()` returns `false` immediately when held by another task and succeeds once released |
| 3 | `TimedLockTask` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | `counter == 2`, `!IsLocked()` | `TimedLock()` times out in `[49, 75]` ms when contended; succeeds when mutex is free; timed-out waiter leaves state intact |
| 4 | `FIFOOrderTask` | 0–4 | `_STK_FMUTEX_STACK_SIZE` | order `[1,2,3,4]` | Blocked tasks are granted ownership in FIFO arrival order |
| 5 | `OwnerStateTask` | 0–1 | `_STK_FMUTEX_STACK_SIZE` | `counter == 2` | `GetOwner()` / `IsLocked()` report correctly for owner and non-owner across free → locked → free |
| 6 | `OwnershipHandoffTask` | 0–1 | `_STK_FMUTEX_STACK_SIZE` | handoff checks pass, `counter == 1`, `!IsLocked()` | `Unlock()` passes ownership directly to the first waiter; the releasing task cannot barge back in |
| 7 | `InterTaskCoordinationTask` | 0–4 | `_STK_FMUTEX_STACK_SIZE` | `counter == 50` | Mutex correctly gates strict round-robin turn-taking across 10 rounds without a condition variable |
| 8 | `CancelledWaitTask` | 0–1 | `_STK_FMUTEX_STACK_SIZE` | `counter == 2` | A wait cancelled via `CancelTaskWait()` returns `false`, does not acquire the lock, and leaves no dangling waiter |
| 9 | `MiddleTimeoutTask` | 0–4 | `_STK_FMUTEX_STACK_SIZE` | order `[1,3]`, `counter == 1`, `!IsLocked()` | A waiter timing out mid-queue is skipped; the remaining waiters keep FIFO order |
| 10 | `StressTestTask` | 0–4 | `_STK_FMUTEX_STACK_SIZE` | `counter > 0`, `counter == expected`, `!IsLocked()` | No corruption, lost update or deadlock under full five-task contention mixing `Lock()`, `TryLock()`, and `TimedLock(10)`; runs on all platforms |
