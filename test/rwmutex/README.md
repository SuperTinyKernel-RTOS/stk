# RWMutex Test Suite

Test suite for `stk::sync::RWMutex`.  
Source: `test/rwmutex/test_rwmutex.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_rwmutex.h>
```

`RWMutex` is a **reader-writer lock** that allows **multiple concurrent readers** or **one exclusive writer**. It is optimized for resources that are read frequently but modified infrequently. The implementation uses a **writer preference policy** to prevent writer starvation: if a writer is waiting, new readers are blocked until all waiting writers have acquired and released the lock.

Internally `RWMutex` owns a `Mutex` (`m_mutex`), two `ConditionVariable` instances (`m_cv_readers`, `m_cv_writers`), and three counters:
- `m_readers` — number of active readers
- `m_writers_waiting` — number of writers blocked waiting to acquire
- `m_writer_active` — boolean; true if a writer currently holds exclusive access

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// Construction
stk::sync::RWMutex g_SettingsLock;  // zero readers, zero writers initially
```

### Reader (shared access) methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `ReadLock` | `void ReadLock()` | Acquires shared read lock. Blocks if a writer is active or if writers are waiting (writer preference). **Not cancelable:** a cancellation (`IKernel::CancelTaskWait()` / `SleepCancel()`) is absorbed and the wait retried; use `TimedReadLockEx()` to observe it. Panics (`KERNEL_PANIC_ASSERT`) if the lock is destroyed while waiting. ISR-unsafe. |
| `TimedReadLockEx` | `EWaitResult TimedReadLockEx(Timeout timeout)` | Same as `TimedReadLock()`, but preserves the full outcome: `WAIT_RESULT_SIGNAL` (lock acquired), `WAIT_RESULT_TIMEOUT` (timeout expired, or `NO_WAIT` on a busy lock), `WAIT_RESULT_CANCELED` (wait cancelled via `IKernel::CancelTaskWait()` or `SleepCancel()`; the lock is **not** taken and the reader count is unchanged), `WAIT_RESULT_DELETED` (the lock was destroyed while waiting; it must not be accessed afterwards, see Destruction behavior), `WAIT_RESULT_FAIL` (kernel rejected the wait). ISR-safe only with `NO_WAIT`. |
| `TimedReadLock` | `bool TimedReadLock(Timeout timeout)` | Acquires shared read lock with timeout. Returns `true` if acquired, `false` otherwise (timeout, cancellation, or `NO_WAIT` on a busy lock); use `TimedReadLockEx()` to tell these apart. Blocks if a writer is active or waiting. ISR-unsafe. |
| `TryReadLock` | `bool TryReadLock()` | Non-blocking attempt to acquire read lock. Returns `true` if acquired immediately, `false` if a writer is active or waiting. Equivalent to `TimedReadLock(NO_WAIT)`. ISR-unsafe. |
| `ReadUnlock` | `void ReadUnlock()` | Releases shared read lock. Decrements reader count. If this was the last reader, wakes one waiting writer via `m_cv_writers.NotifyOne()`. ISR-unsafe. |

### Writer (exclusive access) methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Lock` | `void Lock()` | Acquires exclusive write lock. Blocks until all active readers have released and no other writer is active. **Not cancelable:** a cancellation is absorbed and the wait retried (this also keeps the mutex re-acquisition at the end of `ConditionVariable::Wait()` reliable); use `TimedLockEx()` to observe it. Panics (`KERNEL_PANIC_ASSERT`) if the lock is destroyed while waiting. ISR-unsafe. Implements `IMutex` interface. |
| `TimedLockEx` | `EWaitResult TimedLockEx(Timeout timeout)` | Same as `TimedLock()`, but preserves the full outcome (`WAIT_RESULT_SIGNAL`, `WAIT_RESULT_TIMEOUT`, `WAIT_RESULT_CANCELED`, `WAIT_RESULT_DELETED`, `WAIT_RESULT_FAIL`, as for `TimedReadLockEx()`). On any result other than `SIGNAL` or `DELETED` the writer withdraws from the waiting-writers queue (see Cancellation behavior); on `DELETED` nothing is touched (see Destruction behavior). ISR-safe only with `NO_WAIT`. |
| `TimedLock` | `bool TimedLock(Timeout timeout)` | Acquires exclusive write lock with timeout. Returns `true` if acquired, `false` otherwise (timeout, cancellation, or `NO_WAIT` on a busy lock); use `TimedLockEx()` to tell these apart. Blocks until readers drain and no writer is active. ISR-unsafe. |
| `TryLock` | `bool TryLock()` | Non-blocking attempt to acquire write lock. Returns `true` if acquired immediately (no readers, no active writer), `false` otherwise. Equivalent to `TimedLock(NO_WAIT)`. ISR-unsafe. |
| `Unlock` | `void Unlock()` | Releases exclusive write lock. Wakes one waiting writer if any exist (`m_cv_writers.NotifyOne()`), otherwise wakes all waiting readers (`m_cv_readers.NotifyAll()`). ISR-unsafe. Implements `IMutex` interface. |

### RAII helper classes

| Class | Usage | Description |
|-------|-------|-------------|
| `ScopedTimedReadMutex` | `ScopedTimedReadMutex guard(rw, timeout)` | RAII wrapper for `TimedReadLock()`. Constructor attempts acquisition; destructor calls `ReadUnlock()` if lock was acquired. Provides `IsLocked()` to check success and `GetResult()` for the full `EWaitResult`. |
| `ScopedTimedLock` | `ScopedTimedLock guard(rw, timeout)` | RAII wrapper for `TimedLock()`. Constructor attempts acquisition; destructor calls `Unlock()` if lock was acquired. Provides `IsLocked()` to check success and `GetResult()` for the full `EWaitResult`. |

**Key invariants:**

- **Writer preference policy:** If `m_writers_waiting > 0`, new readers block on `m_cv_readers` even if no writer is currently active. This prevents a continuous stream of readers from starving writers.
- **Lock state transitions:**
  - Readers can acquire only when `!m_writer_active && m_writers_waiting == 0`.
  - Writers can acquire only when `!m_writer_active && m_readers == 0`.
- **Wake policy on release:**
  - `ReadUnlock()` wakes one writer if `m_readers` drops to zero.
  - `Unlock()` (writer release) wakes one writer if any are waiting, otherwise wakes **all** readers.

**Blocking behavior:**
- `ReadLock()` / `TimedReadLock()` loop on `m_cv_readers.Wait()` while `m_writer_active || m_writers_waiting > 0`.
- `Lock()` / `TimedLock()` increment `m_writers_waiting`, loop on `m_cv_writers.Wait()` while `m_writer_active || m_readers > 0`, then decrement `m_writers_waiting` and set `m_writer_active = true`.

**Cancellation behavior** (`WAIT_RESULT_CANCELED`, via `IKernel::CancelTaskWait()` or `SleepCancel()`):
- A cancelled `TimedReadLockEx()` leaves no trace: the lock is not taken and `m_readers` is not incremented.
- A cancelled `TimedLockEx()` withdraws from the waiting-writers queue (`m_writers_waiting` is decremented). Since waiting writers block new readers, the withdrawal also repairs what that preference caused:
  - if no writer is active and none is left waiting, **all** blocked readers are woken (`m_cv_readers.NotifyAll()`); otherwise they would sleep until their own timeout, or forever with `WAIT_INFINITE`;
  - if other writers are still waiting and no reader is active, the wake is passed on to the next writer (`m_cv_writers.NotifyOne()`), because the withdrawing writer may have consumed a notification meant for it;
  - if readers are still active nothing is needed: the last `ReadUnlock()` wakes the next writer.
- The void `ReadLock()` and `Lock()` are not cancelable: they absorb a cancel and retry, and only return with the lock held.
- If a release reaches a waiter before a cancel does, the signal wins: the waiter is granted the lock (`WAIT_RESULT_SIGNAL`) and the cancel is a no-op.
- Cancellation is distinct from a timeout: `WaitEx()` underneath reports `WAIT_RESULT_CANCELED`, not `WAIT_RESULT_TIMEOUT`.

**Destruction behavior** (`WAIT_RESULT_DELETED`, the lock is destroyed while tasks wait for it):
- A task can wait only for a *held* lock (a writer is active, or readers are active and a writer or reader waits), so destroying a lock with waiters means destroying it while it is held. The destructor allows exactly that: it asserts in debug builds if the lock is held and **nobody** is waiting for it (a logic error: dangling state), and accepts it when tasks are waiting. The holder must not unlock the destroyed lock.
- The destructors of the two internal condition variables release **all** waiters, the writers and the readers, with `WAIT_RESULT_DELETED`. The released waiters neither acquire the lock nor time out.
- `TimedReadLockEx()` and `TimedLockEx()` return `WAIT_RESULT_DELETED` without reading or writing any member of the destroyed lock: `m_readers` is not incremented, and a writer does **not** withdraw from `m_writers_waiting` or notify anybody, as it does on a timeout or a cancellation (there is nothing left to repair).
- `ScopedTimedLock` and `ScopedTimedReadMutex` report it through `GetResult()`; `IsLocked()` is `false`, so their destructors do not touch the destroyed lock.
- The void `ReadLock()` and `Lock()` cannot report it and panic (`KERNEL_PANIC_ASSERT`): use the timed variants where the lock may be destroyed while waited for.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_RWMUTEX_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_RWMUTEX_TEST_TIMEOUT` | `1000` ticks | Blocking timeout for operations that must succeed |
| `_STK_RWMUTEX_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_RWMUTEX_TEST_LONG_SLEEP` | `100` ticks | Sleep used by verifier tasks to wait for workers |
| `_STK_RWMUTEX_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

`g_TestRWMutex` is a **plain static** constructed once at program initialization. It is **not** reconstructed via placement-new between tests (unlike `Pipe` or `ConditionVariable`). The lock naturally returns to an unlocked state (zero readers, zero writers) after each test completes.

All tests add all five tasks unconditionally — there is no `NeedsExtendedTasks()` gating.

The cancellation tests (11–24) additionally use a small amount of shared state, reset by `ResetTestState()`:

| Variable | Purpose |
|----------|---------|
| `g_Victim` / `g_VictimTid` | Task (and its `TId`) whose wait is cancelled; published by task 1 right before it blocks |
| `g_VictimResult[2]` | `EWaitResult` observed by task 1 (victim) / task 3 (second waiter); `-100` = not set |
| `g_VictimElapsed[2]` | Milliseconds task 1 / task 3 spent acquiring the lock |
| `g_CancelParam` | Scenario selector (see below); a global rather than a constructor argument because tasks 0–2 are `static` and would keep the value of the first case |

The helper `CancelVictim(use_sleep_cancel)` cancels the victim either with `g_Kernel.CancelTaskWait(g_Victim)` or with `stk::SleepCancel(g_VictimTid)`. The helper `IsLockPristine()` checks that the lock ended up free with consistent counters: `TryReadLock()` must succeed (a stale waiting writer would block it through writer preference) and so must `TryLock()` (a stale reader or active writer would block it).

The delete-with-waiter tests (25–27) destroy a **separate** lock: `g_DeleteRW` points to an `RWMutex` constructed
with placement-new in the static `g_DeleteRWStorage`, so `g_TestRWMutex` is never destroyed by the test itself.
Task 0 holds the dedicated lock (a read lock, or the write lock in test 26) while it destroys it, because only a held
lock can have waiters. After the destruction the storage is overwritten with the byte `DELETED_FILL` (`0xDD`), and task 0
checks at the end that every byte still holds it: a woken waiter that wrote to the destroyed lock (for instance by
withdrawing from `m_writers_waiting`, as a timed-out writer does) would leave a trace. The tests reuse `g_Victim`,
`g_VictimTid`, `g_VictimResult[]` and `g_VictimElapsed[]` (index 0: task 1; index 1: task 2, the second waiter of test
27 only, unlike the cancellation tests, where it is task 3), `IsLockPristine()`, and add:

| Variable | Purpose |
|----------|---------|
| `g_DeleteRW` | Pointer to the dedicated lock; set back to `nullptr` right after the destruction |
| `g_DeleteVictim2` | Task 2 (second waiter), published right before it blocks; used by the safety net |
| `g_DeleteCheckResult` / `g_DeleteCheckElapsed` | `EWaitResult` and milliseconds of the follow-up timed lock (20 ticks) of task 1 on `g_TestRWMutex` (`-100` = not set) |
| `g_DeleteParam` | Scenario selector (`DW_WRITER = 1`, `DW_READER = 2`, `DW_BOTH = 3`); a global for the same reason as `g_CancelParam` |

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link all twelve distinct task class templates simultaneously. Tests 1–9, the cancellation tests 11–24 and the delete-with-waiter tests 25–27 are skipped on M0 and only `StressTest` (test 10) runs, under `#ifndef __ARM_ARCH_6M__`. `CancelWaitTask` is one more template, shared by all fourteen cancellation tests, and `DeleteWithWaiterTask` another, shared by the three delete-with-waiter tests.

`StressTest` runs on M0 because it uses a single task class template (`StressTestTask`) instantiated for all five task slots, fitting within the available memory.

| Platform | `_STK_RWMUTEX_STACK_SIZE` |
|----------|---------------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `ConcurrentReaders`
**Tasks:** 0–4 &nbsp;|&nbsp; **Param:** none

Tasks 1–4 are readers. Each acquires `ReadLock()`, increments `g_ReaderCount`, sleeps `_STK_RWMUTEX_TEST_SHORT_SLEEP` ticks while holding the lock, then decrements `g_ReaderCount` and releases. `g_MaxConcurrent` tracks the maximum value `g_ReaderCount` reaches. Task 0 is the verifier, waiting for all four readers to finish then checking the maximum. Confirms that multiple readers can hold the lock simultaneously.

**Pass condition:** `g_MaxConcurrent == 4` (all four reader tasks were concurrent)

---

### Test 2 — `WriterExclusivity`
**Tasks:** 0–4 &nbsp;|&nbsp; **Param:** `iterations = 100`

All five tasks act as writers. Each runs 100 iterations of: acquire exclusive lock via `Lock()`, read-modify-write `g_SharedCounter` with a deliberate race window (delay every 4th operation), then `Unlock()` and `Yield()`. Task 0 is also the verifier. Confirms that the write lock provides true mutual exclusion and no increments are lost.

**Pass condition:** `counter == 500` (`5 tasks × 100 iterations`)

---

### Test 3 — `WriterStarvation`
**Tasks:** 0–4

Tasks 1–3 are readers that continuously acquire/release read locks in a tight loop until `g_TestComplete` is set. Task 4 waits `_STK_RWMUTEX_TEST_SHORT_SLEEP` then attempts to acquire the write lock; with writer preference enforced, task 4 must acquire within a reasonable time (< 200ms) despite the continuous reader flood. Task 0 is the verifier. Confirms that writer preference prevents writer starvation under a heavy read workload.

**Pass condition:** `counter == 1` (writer acquired within 200ms)

---

### Test 4 — `TimedReadLock`
**Tasks:** 0–2

Task 1 holds the write lock for 200 ticks. Task 2 waits until signaled, then calls `TimedReadLock(50)`; this must time out with elapsed time in `[45, 65]` ms. After task 1 releases (task 2 sleeps 250ms total), task 2 calls `TimedReadLock(100)` which succeeds immediately. Task 0 is the verifier. Confirms that `TimedReadLock()` respects the timeout and returns `false` correctly.

**Pass condition:** `counter == 3`  
(1 = task 1 signaled; 2 = task 2 timed out correctly; 3 = task 2 acquired after release)

---

### Test 5 — `TimedWriteLock`
**Tasks:** 0–2

Task 1 holds a read lock for 200 ticks. Task 2 waits until signaled, then calls `TimedLock(50)`; this must time out with elapsed time in `[45, 65]` ms because a reader is active. After task 1 releases (task 2 sleeps 250ms total), task 2 calls `TimedLock(100)` which succeeds immediately. Task 0 is the verifier. Confirms that `TimedLock()` respects the timeout and returns `false` correctly when readers are active.

**Pass condition:** `counter == 3`  
(1 = task 1 signaled; 2 = task 2 timed out correctly; 3 = task 2 acquired after release)

---

### Test 6 — `TryReadWhileWriter`
**Tasks:** 0–2

Task 1 acquires write lock, signals task 2 via `g_SharedCounter = 1`, then holds the lock for `_STK_RWMUTEX_TEST_LONG_SLEEP` (100 ticks). Task 2 waits for the signal, then calls `TryReadLock()` and measures elapsed time; it must return `false` immediately (elapsed < `_STK_RWMUTEX_TEST_SHORT_SLEEP`). After waiting for task 1 to release, task 2 calls `TryReadLock()` again which succeeds. Task 0 is the verifier. Confirms non-blocking `TryReadLock()` semantics: fails instantly when a writer is active, succeeds after release.

**Pass condition:** `counter == 3`  
(1 = writer signaled; 2 = TryReadLock failed immediately; 3 = TryReadLock succeeded after release)

---

### Test 7 — `ReadUnlockWakesWriter`
**Tasks:** 0–3

Tasks 1 and 2 are readers. Each acquires `ReadLock()` and increments `g_ReaderCount`; they both wait until `g_ReaderCount == 2` to ensure both have acquired. Task 3 is a writer that waits until `g_ReaderCount == 2`, then calls `Lock()` and blocks. Tasks 1 and 2 sleep `_STK_RWMUTEX_TEST_SHORT_SLEEP`, then release one by one. When the last reader (`m_readers` drops to zero) calls `ReadUnlock()`, it wakes task 3 via `m_cv_writers.NotifyOne()`. Task 3 measures elapsed time from `Lock()` call to acquisition; it must be < 50ms. The last reader sets `g_SharedCounter = 1` before releasing. Task 0 is the verifier. Confirms that the last reader immediately wakes a waiting writer, not after a full scheduler tick.

**Pass condition:** `counter == 2`  
(1 = last reader signaled; 2 = writer woken within 50ms)

---

### Test 8 — `WriterPriority`
**Tasks:** 0–3

Task 1 is a reader that holds `ReadLock()`, signals via `g_SharedCounter = 1`, then holds for `_STK_RWMUTEX_TEST_LONG_SLEEP` (100 ticks). Task 2 is a writer that waits for the signal, sleeps `_STK_RWMUTEX_TEST_SHORT_SLEEP`, then calls `Lock()` and blocks (this increments `m_writers_waiting`). Task 3 is a second reader that waits for the signal, sleeps `2 × _STK_RWMUTEX_TEST_SHORT_SLEEP`, then attempts `ReadLock()`; with writer preference enforced, task 3 must block behind task 2 even though task 1 is a reader and could theoretically share access. When task 1 releases, task 2 (writer) acquires first, sets `g_WriterCount = 1`, then releases. Task 3 (reader) then acquires and checks `g_WriterCount`; if it's 1, the writer got priority. Task 0 is the verifier. Confirms the writer preference policy: new readers block when writers are waiting.

**Pass condition:** `counter == 2`  
(1 = reader 1 signaled; 2 = writer acquired before reader 3, confirming writer priority)

---

### Test 9 — `ReaderWriterAlternation`
**Tasks:** 0–4

Three distinct phases test alternating access patterns. Tasks 1–3 are reader/verifier hybrids; task 4 is a writer; task 0 is the final verifier.

**Phase 1 (concurrent readers):** Tasks 1–3 each acquire `ReadLock()`, increment `g_ReaderCount`, track `g_MaxConcurrent`, delay 5ms, decrement `g_ReaderCount`, then release.

**Phase 2 (exclusive writer):** Task 4 sleeps `_STK_RWMUTEX_TEST_SHORT_SLEEP`, then acquires `Lock()`, sets `g_SharedCounter = 1` to signal phase 2 complete, then releases.

**Phase 3 (concurrent readers again):** Tasks 1–3 wait for `g_SharedCounter == 1`, then each acquire `ReadLock()` again, increment `g_ReaderCount`, delay 5ms, decrement, and release.

All tasks except task 0 increment `g_InstancesDone` when finished. Task 0 waits for `g_InstancesDone == 4` then verifies `g_MaxConcurrent >= 2` (at least two readers were concurrent in phase 1 or 3). Confirms that readers can share access concurrently in read phases and that the writer gets exclusive access in the write phase.

**Pass condition:** `g_MaxConcurrent >= 2`

---

### Test 10 — `StressTest`
**Tasks:** 0–4 — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 100`

All five tasks alternate between reader and writer roles by iteration index. Even iterations (`i % 2 == 0`) use `ReadLock()` / `ReadUnlock()` to read `g_SharedCounter` (no modification). Odd iterations (`i % 2 == 1`) use `Lock()` / `Unlock()` to increment `g_SharedCounter`. Every 10th iteration delays 1ms. Task 4 is also the verifier: it waits for `g_InstancesDone == 5`, then verifies the counter equals the expected number of write operations. Expected writes = `5 tasks × (100 iterations / 2) = 250`. Confirms no data corruption under full five-task contention mixing both lock types.

**Pass condition:** `counter == 250` (`5 tasks × 50 write iterations`)

---

### Cancellation tests (Tests 11–24)

Tests 11–24 verify that a blocked `RWMutex` acquisition can be cancelled, that the internal counters (`m_readers`, `m_writers_waiting`) stay consistent, and that nothing is left blocked behind a cancelled waiter. All use the same task class (`CancelWaitTask`) and **tasks 0–3** (task 4 is added like in every other test but returns immediately). The scenario is selected by `param` (`1..9`); adding `100` selects `stk::SleepCancel()` instead of `IKernel::CancelTaskWait()`. Tests 11–19 cancel with `CancelTaskWait()`; tests 20–24 repeat the key scenarios with `SleepCancel()`.

Common timeline (ticks; deviations are noted per test):

| Task | Role | Timeline |
|------|------|----------|
| 0 | canceller / checker | cancels at ~60; snapshot at ~80 (holder still owns the lock); final check at ~200 (`IsLockPristine()`) |
| 1 | victim | locks at ~10 with `TimedLockEx()` / `TimedReadLockEx()` (or void `Lock()` / `ReadLock()`) and records result and elapsed time; releases the lock only if it was acquired |
| 2 | holder | takes the lock at 0 (read or write, depending on the test) and releases it at ~150 |
| 3 | second waiter | writer or reader queued at ~5 or ~20 depending on the test; records result and elapsed time |

`StressTest` keeps the number 10 but runs last in `main()`, after the cancellation tests and the delete-with-waiter tests (25–27).

> **Safety net:** if a cancel does not work, a waiter could stay blocked forever (for instance readers that are never woken after the withdrawal of a waiting writer) and the kernel would never stop. On failure task 0 therefore takes and drops the write lock twice with `TimedLock()` + `Unlock()`, which wakes all waiting readers (or the next writer), so a broken cancel is reported as a test failure rather than a hang.

> **Kernel requirement:** tests 20–24 depend on `SleepCancel()` also cancelling a wait on a sync object (`OnTaskSleepCancel()` routes through `WaitObject::Cancel()`). Against a kernel where `SleepCancel()` only wakes the task, they fail by design: that is the stale wait entry regression they exist to catch.

---

### Test 11 — `CancelWait_WriterInfinite`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `1` &nbsp;|&nbsp; **Runs on M0: no**

Task 2 holds a read lock. Task 1 calls `TimedLockEx(WAIT_INFINITE)` at about tick 10 and blocks behind the reader (`m_writers_waiting == 1`). Task 0 cancels it at about tick 60 via `CancelTaskWait()`. After the holder released, `IsLockPristine()` confirms the writer withdrew from the queue: `TryReadLock()` would fail forever if `m_writers_waiting` had leaked.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, elapsed in `[35, 75]` ms, lock pristine

---

### Test 12 — `CancelWait_WriterNotTimeout`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `2`

Same as test 11 but task 1 waits with a finite 300-tick timeout. Verifies that a cancel is reported as `WAIT_RESULT_CANCELED`, not `WAIT_RESULT_TIMEOUT`, and that the wait really ends early instead of running to its deadline.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, elapsed `< 150` ms, lock pristine

---

### Test 13 — `CancelWait_ReaderInfinite`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `3`

Task 2 holds the **write** lock. Task 1 calls `TimedReadLockEx(WAIT_INFINITE)` and blocks behind the writer. Task 0 cancels it at about tick 60. After the holder released, `IsLockPristine()` confirms the cancelled reader was not counted: `TryLock()` would fail if `m_readers` had been incremented.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, elapsed in `[35, 75]` ms, lock pristine

---

### Test 14 — `CancelWait_ReaderBehindWriter`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `4`

Task 2 holds a read lock. Task 3 queues as a writer at about tick 5; task 1 then calls `TimedReadLockEx(WAIT_INFINITE)` at about tick 10 and blocks only because of writer preference. Task 0 cancels the reader at about tick 60. The writer must be unaffected: still blocked while the holder reads, and granted the lock when the holder releases at about tick 150 (never, if the cancelled reader had leaked into `m_readers`).

**Pass condition:** victim `WAIT_RESULT_CANCELED` with elapsed in `[35, 75]` ms, writer still blocked at tick ~80, writer `WAIT_RESULT_SIGNAL` with elapsed `>= 100` ms, lock pristine

---

### Test 15 — `CancelWait_PreferenceReleased`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `5`

Task 2 holds a read lock. Task 1 blocks as a writer (`TimedLockEx(WAIT_INFINITE)`). Task 3 calls `TimedReadLockEx(WAIT_INFINITE)` at about tick 20 and blocks only because of the waiting writer's preference. When task 0 cancels the writer at about tick 60, the withdrawal must wake all blocked readers (`m_cv_readers.NotifyAll()`): task 3 gets its read lock at once, while the holder is still reading. Without that wake-up it would sleep until the holder released and then forever, since nobody would ever notify it.

**Pass condition:** victim `WAIT_RESULT_CANCELED`, reader `WAIT_RESULT_SIGNAL` already at tick ~80 with elapsed in `[25, 100]` ms, lock pristine

---

### Test 16 — `CancelWait_TwoWriters`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `6`

Task 2 holds a read lock. Task 1 blocks as a writer first, task 3 as a second writer at about tick 20. Task 0 cancels only task 1. The second writer must be unaffected (`m_writers_waiting` decremented exactly once, wait list intact): still blocked at tick ~80, then granted the lock when the holder releases at about tick 150.

**Pass condition:** victim `WAIT_RESULT_CANCELED`, second writer still blocked at tick ~80, then `WAIT_RESULT_SIGNAL` with elapsed `>= 100` ms, lock pristine

---

### Test 17 — `CancelWait_SignalWins`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `7`

Task 0 itself holds a read lock; task 1 blocks as a writer. At about tick 60 task 0 calls `ReadUnlock()` and immediately cancels the victim, with no scheduling point in between. The last `ReadUnlock()` has already woken the writer (a notified waiter is unlinked from the wait list), so the cancel must be a no-op and the writer must be granted the lock.

**Pass condition:** `result == WAIT_RESULT_SIGNAL` and lock pristine (the victim acquired and released it)

---

### Test 18 — `CancelWait_LockAbsorbs`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `8`

Task 2 holds a read lock. Task 1 uses the **void** `Lock()`. A cancel at about tick 60 must be absorbed: `Lock()` cannot report a failure, so it withdraws and retries. The victim stays blocked until the holder releases at about tick 150 and only then returns with the lock held.

**Pass condition:** victim still blocked at tick ~80, then returned with the lock after `>= 110` ms, lock pristine

---

### Test 19 — `CancelWait_ReadLockAbsorbs`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `9`

Test 18 for readers: task 2 holds the write lock and task 1 uses the **void** `ReadLock()`. The cancel must be absorbed and the victim must return with the read lock only after the holder released.

**Pass condition:** same as test 18

---

### Test 20 — `CancelWait_SleepCancelWriter`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `101`

Test 11 using `stk::SleepCancel(tid)` instead of `CancelTaskWait()`.

**Pass condition:** same as test 11

---

### Test 21 — `CancelWait_SleepCancelReader`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `103`

Test 13 using `SleepCancel()`.

**Pass condition:** same as test 13

---

### Test 22 — `CancelWait_SleepCancelPreference`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `105`

Test 15 using `SleepCancel()`. A stale wait entry or a missing reader wake-up after the withdrawal of the writer shows up here as a blocked reader.

**Pass condition:** same as test 15

---

### Test 23 — `CancelWait_SleepCancelTwoWriters`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `106`

Test 16 using `SleepCancel()`.

**Pass condition:** same as test 16

---

### Test 24 — `CancelWait_SleepCancelSignal`
**Tasks:** 0–3 &nbsp;|&nbsp; **Param:** `107`

Test 17 using `SleepCancel()`. The task is no longer waiting but is still marked as sleeping for one tick after the wake, so this also checks that `SleepCancel()` falls through to a plain wake without disturbing the already-granted lock.

**Pass condition:** same as test 17

---

### Tests 25–27 — `DeleteWithWaiter*`
**Tasks:** 0–2 used (tasks 3 and 4 are added by `RunTest()` but return immediately) &nbsp;|&nbsp; **Task class:** `DeleteWithWaiterTask` &nbsp;|&nbsp; **Runs on M0: no**

Verify that destroying an `RWMutex` which tasks are blocked on releases **all** waiters, the writers and the readers (the lock
has two internal condition variables), with `WAIT_RESULT_DELETED`, without acquiring the lock and without touching the destroyed
object. Only a held lock can have waiters, so task 0 constructs a dedicated lock (`g_DeleteRW`, placement-new into
`g_DeleteRWStorage`), **holds** it, sleeps 60 ticks and destroys it while it is still held. Nobody ever releases the lock,
so only its destruction can end the waits. Task 0 then overwrites the storage with `0xDD`, so that any access to the destroyed
lock by a woken waiter would read garbage, and any write would be detected. Task 1 sleeps `_STK_RWMUTEX_TEST_SHORT_SLEEP`,
publishes its `ITask` pointer in `g_Victim` and blocks on the lock with `WAIT_INFINITE`, then records the result and the
elapsed time without touching the destroyed lock. It then performs a timed lock of the same kind (20 ticks) on `g_TestRWMutex`,
which task 0 has write-locked right after the destruction: the attempt must run to its deadline and return
`WAIT_RESULT_TIMEOUT`; a stale wait entry left behind would end it early or with another result, and the writer's or reader's
withdrawal must leave `g_TestRWMutex` pristine. Task 0 releases `g_TestRWMutex` at about tick 100 and checks the outcome at
about tick 140. If a waiter was not released, task 0 cancels its wait with `g_Kernel.CancelTaskWait()`, so the test fails
instead of hanging the kernel.

**Common pass condition:** task 1's first result is `WAIT_RESULT_DELETED` (not `WAIT_RESULT_SIGNAL`, a timeout or a cancellation)
with elapsed time in `[35, 75]` ms (waiting from ~10 ms, destroyed at ~60 ms); the follow-up timed lock on `g_TestRWMutex`
returned `WAIT_RESULT_TIMEOUT` after at least 15 ms; `IsLockPristine()` holds for `g_TestRWMutex`; every byte of the
storage of the destroyed lock still holds `0xDD`

#### Test 25 — `DeleteWithWaiterWriter`
**Param:** `DW_WRITER`

Task 0 holds a **read** lock; task 1 blocks in `TimedLockEx(WAIT_INFINITE)` (`m_writers_waiting == 1`, the writer waits on
`m_cv_writers`).

**Pass condition:** common

#### Test 26 — `DeleteWithWaiterReader`
**Param:** `DW_READER`

Task 0 holds the **write** lock; task 1 blocks in `TimedReadLockEx(WAIT_INFINITE)` (the reader waits on `m_cv_readers`). The
destructor accepts the lock because the reader wait list is not empty.

**Pass condition:** common

#### Test 27 — `DeleteWithWaiterBoth`
**Param:** `DW_BOTH`

Task 0 holds a read lock. Task 1 blocks as a writer, task 2 as a reader at about tick 20, only because of the waiting writer's
preference. Both use the RAII guards (`ScopedTimedLock`, `ScopedTimedReadMutex`) with `WAIT_INFINITE`: the destruction must release
the waiters of both internal condition variables, `GetResult()` must report `WAIT_RESULT_DELETED`, and the guard destructors,
which run after the destruction, must not unlock the destroyed lock (`IsLocked()` is `false`).

**Pass condition:** common, and task 2 also gets `WAIT_RESULT_DELETED` with elapsed time in `[25, 65]` ms (waiting from ~20 ms)

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `ConcurrentReadersTask` | 0–4 | `max_concurrent == 4` | Multiple readers acquire `ReadLock()` simultaneously; shared access works |
| 2 | `WriterExclusivityTask` | 0–4 | `counter == 500` | Exclusive write lock provides mutual exclusion; no lost increments under contention |
| 3 | `WriterStarvationTask` | 0–4 | `counter == 1` | Writer preference: writer acquires within 200ms despite continuous reader flood |
| 4 | `TimedReadLockTask` | 0–2 | `counter == 3` | `TimedReadLock(50)` times out in `[45, 65]`ms when writer active; succeeds after release |
| 5 | `TimedWriteLockTask` | 0–2 | `counter == 3` | `TimedLock(50)` times out in `[45, 65]`ms when reader active; succeeds after release |
| 6 | `TryReadLockWhileWriterTask` | 0–2 | `counter == 3` | `TryReadLock()` fails immediately when writer active; succeeds after release |
| 7 | `ReadUnlockWakesWriterTask` | 0–3 | `counter == 2` | Last reader releasing wakes waiting writer within 50ms (not after full scheduler tick) |
| 8 | `WriterPriorityTask` | 0–3 | `counter == 2` | New readers blocked behind waiting writer (writer preference policy enforcement) |
| 9 | `ReaderWriterAlternationTask` | 0–4 | `max_concurrent >= 2` | Alternating read/write phases; readers share access, writer gets exclusivity |
| 10 | `StressTestTask` | 0–4 | `counter == 250` | No corruption under full contention; readers and writers alternate; runs on all platforms |
| 11 | `CancelWait_WriterInfinite` | 0–3 | `CANCELED`, elapsed `[35, 75]` ms, lock pristine | Cancelling an infinite `TimedLockEx()` returns `WAIT_RESULT_CANCELED`; the writer leaves the queue (no leaked `m_writers_waiting`) |
| 12 | `CancelWait_WriterNotTimeout` | 0–3 | `CANCELED`, elapsed `< 150` ms, lock pristine | A cancel is distinct from a timeout and ends a finite wait early |
| 13 | `CancelWait_ReaderInfinite` | 0–3 | `CANCELED`, elapsed `[35, 75]` ms, lock pristine | Cancelling `TimedReadLockEx()` returns `WAIT_RESULT_CANCELED` and leaves `m_readers` untouched |
| 14 | `CancelWait_ReaderBehindWriter` | 0–3 | reader `CANCELED`, writer `SIGNAL` after release, lock pristine | A cancelled reader queued behind a writer does not disturb that writer |
| 15 | `CancelWait_PreferenceReleased` | 0–3 | writer `CANCELED`, reader `SIGNAL` at once, lock pristine | Withdrawing the only waiting writer wakes the readers held back by writer preference |
| 16 | `CancelWait_TwoWriters` | 0–3 | first `CANCELED`, second `SIGNAL` after release, lock pristine | Waiting-writer count and wait list stay intact; the remaining writer still gets the lock |
| 17 | `CancelWait_SignalWins` | 0–3 | `SIGNAL`, lock pristine | A release that already woke the waiter wins; the cancel is a no-op |
| 18 | `CancelWait_LockAbsorbs` | 0–3 | blocked until release, `SIGNAL`, elapsed `>= 110` ms | Void `Lock()` absorbs a cancel and returns only with the lock held |
| 19 | `CancelWait_ReadLockAbsorbs` | 0–3 | as test 18 | Void `ReadLock()` absorbs a cancel and returns only with the lock held |
| 20 | `CancelWait_SleepCancelWriter` | 0–3 | as test 11 | `SleepCancel()` cancels a sync-object wait of a writer |
| 21 | `CancelWait_SleepCancelReader` | 0–3 | as test 13 | `SleepCancel()` cancels a sync-object wait of a reader |
| 22 | `CancelWait_SleepCancelPreference` | 0–3 | as test 15 | `SleepCancel()` leaves no stale entry and still releases the readers held back by the writer |
| 23 | `CancelWait_SleepCancelTwoWriters` | 0–3 | as test 16 | `SleepCancel()` keeps the wait list intact |
| 24 | `CancelWait_SleepCancelSignal` | 0–3 | as test 17 | `SleepCancel()` after a release is a harmless no-op |
| 25 | `DeleteWithWaiterWriter` | 0–2 | `DELETED`, elapsed `[35, 75]` ms, follow-up timed lock on `g_TestRWMutex` times out `>= 15` ms, lock pristine, storage untouched | Destroying a lock held by a reader releases a waiting writer with `WAIT_RESULT_DELETED`; the writer does not withdraw from `m_writers_waiting` of the destroyed lock and leaves no stale wait entry |
| 26 | `DeleteWithWaiterReader` | 0–2 | as test 25 | Destroying a lock held by a writer releases a waiting reader with `WAIT_RESULT_DELETED`; `m_readers` of the destroyed lock is not touched |
| 27 | `DeleteWithWaiterBoth` | 0–2 | as test 25, and the reader `DELETED` with elapsed `[25, 65]` ms | The destruction releases the waiters of both internal condition variables; the RAII guards report `DELETED` and do not unlock the destroyed lock |
