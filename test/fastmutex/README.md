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
| `Lock` | `void Lock()` | Acquires the lock, blocking indefinitely. Must not be called by the current owner. **Not cancelable:** a cancellation (`IKernel::CancelTaskWait()`, `stk::SleepCancel()`) is absorbed and the wait is retried, so it always returns with the lock held. ISR-unsafe. |
| `TryLock` | `bool TryLock()` | Acquires the lock without blocking (`timeout == NO_WAIT`). Returns `true` if acquired, `false` if held (including by the caller). ISR-safe. |
| `TimedLockEx` | `EWaitResult TimedLockEx(Timeout timeout)` | Acquires the lock, blocking up to `timeout` ticks, and reports the full outcome: `WAIT_RESULT_SIGNAL` (acquired), `WAIT_RESULT_TIMEOUT` (busy and timeout expired, or `NO_WAIT`), `WAIT_RESULT_CANCELED` (wait cancelled), `WAIT_RESULT_FAIL` (kernel rejected the wait). ISR-safe only with `NO_WAIT`. |
| `TimedLock` | `bool TimedLock(Timeout timeout)` | Acquires the lock, blocking up to `timeout` ticks. Returns `true` if acquired, `false` on timeout, cancellation or `NO_WAIT` on a busy mutex (use `TimedLockEx()` to tell them apart). Panics if the caller already owns the lock. ISR-safe only with `NO_WAIT`. |
| `Unlock` | `void Unlock()` | Transfers ownership to the first waiter (FIFO) or marks the mutex free. Asserts the caller is the current owner. ISR-safe. |
| `GetOwner` | `TId GetOwner() const` | Returns the owner's task ID, or `TID_NONE` if free. ISR-safe. |
| `IsLocked` | `bool IsLocked() const` | Returns `true` if the mutex has an owner. Snapshot only — may be stale immediately for a non-owner. ISR-safe. |
| `~FastMutex` | (destructor) | Asserts `m_wait_list.IsEmpty()` in debug builds. Destroying a mutex with active waiters is a logic error. |

**Lock paths in `TimedLockEx()` (`TimedLock()` and `TryLock()` are thin wrappers):**

```
owner == TID_NONE (free)       →  m_owner_tid = caller, return WAIT_RESULT_SIGNAL immediately
owner == caller   (self-lock)  →  contract violation: STK_KERNEL_PANIC(KERNEL_PANIC_SYNC_DEADLOCK)
timeout == NO_WAIT (try-lock)  →  return WAIT_RESULT_TIMEOUT immediately (fast-fail)
otherwise         (contended)  →  block until Unlock() transfers ownership (WAIT_RESULT_SIGNAL),
                                  the timeout expires (WAIT_RESULT_TIMEOUT) or the wait is
                                  cancelled (WAIT_RESULT_CANCELED)
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
- A cancelled `TimedLockEx()` returns `WAIT_RESULT_CANCELED` (`TimedLock()` returns `false`)
  immediately, regardless of the deadline. The cancelled task does not own the mutex and is
  removed from the wait list, so a later `Unlock()` frees the mutex or hands it to the next
  waiter rather than to the cancelled task. The cancel never takes the mutex away from its owner.
- If an `Unlock()` hand-over already woke the waiter before the cancel arrives, the cancel is a
  no-op: the waiter owns the mutex and gets `WAIT_RESULT_SIGNAL`.
- `Lock()` cannot report failure, so it is not cancelable: it absorbs a cancellation and retries.
  This also keeps the mutex re-acquisition at the end of `ConditionVariable::Wait()` reliable.
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

The kernel is `Kernel<KERNEL_DYNAMIC | KERNEL_SYNC | KERNEL_TICKLESS (if STK_TICKLESS_IDLE), 5, sched::SwitchStrategyRR, PlatformDefault>`.

`g_TestMutex` is a plain static — `ResetTestState()` does **not** reconstruct it via
placement-new. The mutex is shared across all test runs in its naturally unlocked state
after each test completes (each test also asserts `!IsLocked()` where it matters).
`ResetTestState()` resets only the counters and flags (`g_TestResult`, `g_SharedCounter`,
`g_ExpectedCounter`, `g_OrderIndex`, `g_InstancesDone`, `g_Task1Tid`, `g_WaiterTask`,
`g_AcquisitionOrder[]`) and the cancellation state described below.

Cancellation tests (10–21) use additional shared state, all cleared by `ResetTestState()`:

| Variable | Purpose |
|----------|---------|
| `g_Victim` / `g_VictimTid` | Task (as `ITask *` and as `TId`) whose wait is cancelled; published by task 1 just before it blocks |
| `g_SecondTid` | Task ID of the second waiter (task 2), used for hand-over checks |
| `g_VictimResult[2]` | `EWaitResult` of the lock attempt of task 1 / task 2 (`-100` = not set) |
| `g_VictimOwner[2]` | `1` / `0`: task 1 / task 2 owned the mutex after its attempt ended (`-1` = not set) |
| `g_VictimElapsed[2]` | Milliseconds spent in the lock attempt by task 1 / task 2 |
| `g_VictimSleepElapsed` | Milliseconds task 1 spent in `Sleep(60)` after its wait ended (`NoStaleWaiter` only) |

`CancelVictim(use_sleep_cancel)` cancels the victim's wait either via
`g_Kernel.CancelTaskWait(g_Victim)` or via `stk::SleepCancel(g_VictimTid)`. The scenario is
selected by the `param` of `RunTest()`; adding `CW_VIA_SLEEPCANCEL` (`100`) switches the cancel
path to `stk::SleepCancel()`.

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
distinct task class templates simultaneously. Tests 1–21 are skipped on M0 and only
`StressTest` (test 22) runs, under `#ifndef __ARM_ARCH_6M__`.

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

### Tests 10–21 — `CancelWait_*`
**Tasks:** 0–2 (3 tasks) &nbsp;|&nbsp; **Task class:** `CancelWaitTask`

Verify that a task blocked on the mutex can be cancelled. Task 0 owns the mutex for the whole test
and is the canceller and checker, task 1 is the victim blocked from ~10 ms, and task 2 is a second
waiter used only by the two-waiters scenario. Task 0 cancels at ~60 ms. Unlike `Event` and
`EventFlags`, nothing is signaled: the "signal" is the `Unlock()` of task 0, which hands the mutex
over to the first remaining waiter. The victim uses `TimedLockEx()` so that `WAIT_RESULT_CANCELED`
can be told apart from a timeout. All scenarios run through `IKernel::CancelTaskWait()`; the
`SleepCancel` variants (tests 17–21) repeat a subset through `stk::SleepCancel()`.

A waiter that ends up owning the mutex releases it after 10 ms, so a failing scenario can not leave
it locked. If a check fails, task 0 unlocks the mutex if it still owns it and waits for the
waiters to drain, so the test fails instead of hanging the kernel.

#### Test 10 — `CancelWait_Infinite`
Task 1 calls `TimedLockEx(WAIT_INFINITE)` and is cancelled by task 0.

**Pass condition:** result is `WAIT_RESULT_CANCELED`; task 1 does not own the mutex; elapsed time is in
`[35, 75]` ms; task 0 is still the owner after the cancel; the mutex is free after task 0 unlocks.

#### Test 11 — `CancelWait_NotTimeout`
Task 1 waits with a 300 ms timeout and is cancelled long before it expires.

**Pass condition:** result is `WAIT_RESULT_CANCELED` (not `WAIT_RESULT_TIMEOUT`); task 1 does not own the
mutex; elapsed time is below 150 ms; the mutex is free after task 0 unlocks.

#### Test 12 — `CancelWait_NoStaleWaiter`
After the cancel, task 0 calls `Unlock()`. A stale wait entry would receive the ownership here.
Task 1 meanwhile sleeps 60 ms; a ghost wait entry would wake it early.

**Pass condition:** result is `WAIT_RESULT_CANCELED`; after `Unlock()` the mutex is free with
`GetOwner() == TID_NONE`; `TryLock()` by task 0 succeeds and makes it the owner; task 1 slept at least 55 ms.

#### Test 13 — `CancelWait_TwoWaiters`
Tasks 1 and 2 both block on the mutex; only task 1 is cancelled. Task 0 then calls `Unlock()`.

**Pass condition:** task 1 gets `WAIT_RESULT_CANCELED` and does not own the mutex; right after
`Unlock()` the owner is task 2; task 2 gets `WAIT_RESULT_SIGNAL` and owns the mutex; the mutex is free
once task 2 releases it.

#### Test 14 — `CancelWait_SignalWins`
Task 0 calls `Unlock()` and `CancelTaskWait()` back to back with no scheduling point between them.
The waiter already owns the mutex when the cancel arrives, so the cancel must be a no-op.

**Pass condition:** the owner right after the cancel is task 1; task 1 gets `WAIT_RESULT_SIGNAL` and owns
the mutex; the mutex is free once task 1 releases it.

#### Test 15 — `CancelWait_LockAbsorbs`
Task 1 blocks in the void `Lock()` instead of `TimedLockEx()`. `Lock()` is not cancelable: it must
absorb the cancel (a returned cancellation would panic with `KERNEL_PANIC_ASSERT`) and keep waiting.
Task 0 unlocks at ~80 ms, after the cancel.

**Pass condition:** task 1 is still blocked after the cancel; task 0 is still the owner; after `Unlock()`
the owner is task 1; task 1's `Lock()` returned with the lock held after `[60, 100]` ms; the mutex is free
once task 1 releases it.

#### Test 16 — `CancelWait_RetryAfter`
After receiving `WAIT_RESULT_CANCELED`, task 1 immediately calls `TimedLockEx(WAIT_INFINITE)` again on
the same mutex. Verifies that a cancelled task can re-enter the wait list cleanly.

**Pass condition:** first attempt is `WAIT_RESULT_CANCELED` without ownership; task 1 is blocked again
before the unlock; after `Unlock()` the owner is task 1 and its second attempt returns `WAIT_RESULT_SIGNAL`;
the mutex is free once task 1 releases it.

#### Tests 17–21 — `CancelWait_SleepCancel*`
Same scenarios as tests 10, 12, 13, 14 and 15, but cancelled through `stk::SleepCancel(tid)`:

| # | Test | Repeats |
|---|------|---------|
| 17 | `CancelWait_SleepCancel` | Test 10 (`Infinite`) |
| 18 | `CancelWait_SleepCancelNoStale` | Test 12 (`NoStaleWaiter`) |
| 19 | `CancelWait_SleepCancelTwo` | Test 13 (`TwoWaiters`) |
| 20 | `CancelWait_SleepCancelSignal` | Test 14 (`SignalWins`) |
| 21 | `CancelWait_SleepCancelAbsorbs` | Test 15 (`LockAbsorbs`) |

**Pass condition:** same as the corresponding test above.

---

### Test 22 — `StressTest`
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
| 10 | `CancelWait_Infinite` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | `WAIT_RESULT_CANCELED`, elapsed `[35, 75]` ms, owner unchanged, free after `Unlock()` | Cancel ends an infinite wait with `WAIT_RESULT_CANCELED`; the owner keeps the mutex |
| 11 | `CancelWait_NotTimeout` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | `WAIT_RESULT_CANCELED`, elapsed `< 150` ms | A cancel is not reported as a timeout and ends a timed wait early |
| 12 | `CancelWait_NoStaleWaiter` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | free after `Unlock()`, `TryLock()` succeeds, victim sleep `>= 55` ms | Cancel leaves no wait entry that would receive the ownership or wake the task early |
| 13 | `CancelWait_TwoWaiters` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | first `CANCELED`, second `SIGNAL` and owner, free at end | Cancelling one waiter leaves the other intact; `Unlock()` hands over to it |
| 14 | `CancelWait_SignalWins` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | victim `SIGNAL` and owner, free at end | A hand-over that arrived first wins over a late cancel |
| 15 | `CancelWait_LockAbsorbs` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | still blocked after cancel, then `SIGNAL` and owner after `[60, 100]` ms | Void `Lock()` is not cancelable: it absorbs the cancel and returns with the lock held |
| 16 | `CancelWait_RetryAfter` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | first `CANCELED`, second `SIGNAL` and owner | A cancelled task can wait for the mutex again and receives it |
| 17 | `CancelWait_SleepCancel` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | as test 10 | Test 10 via `stk::SleepCancel()` |
| 18 | `CancelWait_SleepCancelNoStale` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | as test 12 | Test 12 via `stk::SleepCancel()` |
| 19 | `CancelWait_SleepCancelTwo` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | as test 13 | Test 13 via `stk::SleepCancel()` |
| 20 | `CancelWait_SleepCancelSignal` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | as test 14 | Test 14 via `stk::SleepCancel()` |
| 21 | `CancelWait_SleepCancelAbsorbs` | 0–2 | `_STK_FMUTEX_STACK_SIZE` | as test 15 | Test 15 via `stk::SleepCancel()` |
| 22 | `StressTestTask` | 0–4 | `_STK_FMUTEX_STACK_SIZE` | `counter > 0`, `counter == expected`, `!IsLocked()` | No corruption, lost update or deadlock under full five-task contention mixing `Lock()`, `TryLock()`, and `TimedLock(10)`; runs on all platforms |
