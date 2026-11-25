# ConditionVariable Test Suite

Test suite for `stk::sync::ConditionVariable`.  
Source: `test/condvar/test_condvar.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_cv.h>
#include <sync/stk_sync_mutex.h>
```

`ConditionVariable` follows the **Monitor pattern**: `Wait()` atomically releases the
associated `Mutex` and suspends the calling task. When the task wakes (via signal or
timeout, cancellation or destruction of the condition variable) the `Mutex` is re-acquired before `Wait()` returns. All operations must be
performed with the mutex held by the caller, except `NotifyOne()` and `NotifyAll()`
which are ISR-safe.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// Construction — both primitives are globals, reconstructed per test via placement-new
stk::sync::Mutex             g_Mtx;
stk::sync::ConditionVariable g_Cond;
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `WaitEx` | `EWaitResult WaitEx(IMutex &mutex, Timeout timeout = WAIT_INFINITE)` | Same as `Wait()`, but preserves the full outcome: `WAIT_RESULT_SIGNAL` (notified), `WAIT_RESULT_TIMEOUT` (timeout expired, or `NO_WAIT`), `WAIT_RESULT_CANCELED` (wait cancelled via `IKernel::CancelTaskWait()` or `SleepCancel()`), `WAIT_RESULT_DELETED` (the condition variable was destroyed while waiting; it must not be accessed afterwards). `mutex` is re-acquired in all four cases: it is a separate object and stays valid. ISR-safe only with `NO_WAIT`. |
| `Wait` | `bool Wait(IMutex &mutex, Timeout timeout = WAIT_INFINITE)` | Atomically releases `mutex` and blocks the calling task. Re-acquires `mutex` before returning, whatever ended the wait. Returns `true` if signaled, `false` otherwise (timeout, cancellation, destruction of the condition variable, or `NO_WAIT`); use `WaitEx()` to tell these apart. Returns `false` immediately without blocking if `timeout == NO_WAIT`. ISR-unsafe unless `timeout == NO_WAIT`. |
| `NotifyOne` | `void NotifyOne()` | Wakes one waiting task in FIFO arrival order. No-op if no tasks are waiting. ISR-safe. |
| `NotifyAll` | `void NotifyAll()` | Wakes all waiting tasks simultaneously. No-op if no tasks are waiting. ISR-safe. |
| `~ConditionVariable` | (destructor) | Wakes all tasks still waiting with `WAIT_RESULT_DELETED` (done by `~SyncObjectBase`); they must not access the object afterwards. Each of them still re-acquires its `mutex` before `WaitEx()` / `Wait()` returns. `Wait()` reports it as `false`, use `WaitEx()` to tell it from a timeout. |

**Canonical usage pattern** (spurious-wakeup-safe):

```cpp
// Consumer
g_Mtx.Lock();
while (!condition_met)
    g_Cond.Wait(g_Mtx, WAIT_INFINITE);
// ... consume shared state ...
g_Mtx.Unlock();

// Producer
g_Mtx.Lock();
condition_met = true;
g_Cond.NotifyOne();
g_Mtx.Unlock();
```

**Key invariants:**

- `Wait()` must be called with the mutex **locked** by the calling task.
- The mutex is released **atomically** with task suspension — no window exists where
  the mutex is held and the task is not yet blocked.
- The mutex is always re-acquired before `Wait()` returns, on the signaled, timeout and
  cancelled paths.
- `Wait(mutex, NO_WAIT)` returns `false` immediately; the implementation short-circuits
  before calling `StartWaiting()`.
- A cancelled wait (`WAIT_RESULT_CANCELED`) is distinct from a timeout, re-acquires the
  mutex before `WaitEx()` returns, and leaves no stale entry in the wait list.
- Mutex re-acquisition is not cancelable: `Mutex::Lock()` absorbs a cancel and retries.
  If a notification reaches a waiter before a cancel does, the signal wins
  (`WAIT_RESULT_SIGNAL`) and the cancel, which then lands while the waiter re-locks the
  mutex, is a no-op.
- A `ConditionVariable` keeps no signaled state: a notification with no waiter is lost,
  and a cancel never consumes one.
- Destroying a `ConditionVariable` while tasks are still waiting releases **all** of them with
  `WAIT_RESULT_DELETED`: they neither consume a notification nor time out, and `WaitEx()` does not
  read or write any member of the destroyed object. The mutex is a separate object, so it is
  re-acquired on this path too, even if another task holds it at the time of the destruction
  (the woken waiter then stays blocked in the mutex until it is released).

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_CV_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_CV_TEST_TIMEOUT` | `300` ticks | Blocking timeout for `Wait()` calls that must succeed |
| `_STK_CV_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_CV_TEST_LONG_SLEEP` | `100` ticks | Sleep used by the verifier task to wait for workers |
| `_STK_CV_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

Both `g_TestMutex` and `g_TestCond` are **reconstructed in-place** via placement-new
inside `ResetTestState()` before each test, ensuring no wait-list or ownership state
leaks between runs.

The cancellation tests (9–19) additionally use a small amount of shared state, reset by
`ResetTestState()`:

| Variable | Purpose |
|----------|---------|
| `g_Victim` / `g_VictimTid` | Task (and its `TId`) whose wait is cancelled; published by task 1 right before it blocks |
| `g_VictimResult[2]` | `EWaitResult` observed by task 1 / task 2 (`-100` = not set) |
| `g_VictimElapsed[2]` | Milliseconds spent inside `WaitEx()` by task 1 / task 2 |
| `g_VictimOwnsMutex[2]` | `1` if task 1 / task 2 owned `g_TestMutex` when `WaitEx()` returned |
| `g_VictimSleepElapsed` | Milliseconds task 1 spends in a `Sleep(60)` after its wait was cancelled |
| `g_CancelParam` | Scenario selector (see below); a global rather than a constructor argument because tasks 0–2 are `static` and would keep the value of the first case |

The helper `CancelVictim(use_sleep_cancel)` cancels the victim either with
`g_Kernel.CancelTaskWait(g_Victim)` or with `stk::SleepCancel(g_VictimTid)`.

The delete-with-waiter tests (20–22) destroy a **separate** condition variable: `g_DeleteCond` points to an object
constructed with placement-new in the static `g_DeleteCondStorage`, so `g_TestCond` and `g_TestMutex` are never
destroyed by the test itself. They reuse `g_Victim`, `g_VictimResult[]`, `g_VictimElapsed[]` and
`g_VictimOwnsMutex[]` (index 0: task 1, index 1: task 2), and add:

| Variable | Purpose |
|----------|---------|
| `g_DeleteCond` | Pointer to the dedicated condition variable; set back to `nullptr` right after the destruction |
| `g_DeleteVictim2` | Task 2 (second waiter), published right before it blocks; used by the safety net |
| `g_DeleteCheckResult` / `g_DeleteCheckElapsed` / `g_DeleteCheckOwns` | `EWaitResult`, milliseconds and mutex ownership of the follow-up `WaitEx(20)` of task 1 on `g_TestCond` (`-100` = not set) |
| `g_DeleteParam` | Scenario selector (`DW_BASIC = 1`, `DW_MUTEX_HELD = 2`, `DW_TWO_WAITERS = 3`); a global for the same reason as `g_CancelParam` |

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link all ten
distinct task class templates simultaneously. Tests 1–7, the cancellation tests 9–19 and the
delete-with-waiter tests 20–22 are skipped on M0 and only `StressTest` (test 8) runs. `StressTest` uses a single task
class template instantiated five times, which fits within the available memory.
`CancelWaitTask` adds one more template (shared by all eleven cancellation tests) and
`DeleteWithWaiterTask` another (shared by the three delete-with-waiter tests) to the
static footprint.

Stack sizes differ between platforms because the `sync::Mutex` + `ConditionVariable` +
`IKernelService::StartWaiting()` call chain is deeper than a bare `SpinLock` acquire:

| Platform | `_STK_CV_STACK_SIZE` |
|----------|----------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `NotifyOneWakes`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Param:** `iterations = 20`

Task 0 is the producer; tasks 1–4 are consumers. All four consumers block on
`Wait(WAIT_INFINITE)`. The producer calls `NotifyOne()` 20 times, pacing each call
with `Delay(1)` so each notification has time to land on exactly one blocked waiter
before the next fires. Each successful wake increments `g_SharedCounter`.

**Pass condition:** `counter == 20`

---

### Test 2 — `NotifyAllWakes`
**Tasks:** 0–4 (all 5)

All four consumers (tasks 1–4) block on `Wait(_STK_CV_TEST_TIMEOUT)`. After
`_STK_CV_TEST_SHORT_SLEEP` ticks the producer (task 0) calls `NotifyAll()` once.
Every consumer must be released by that single call.

**Pass condition:** `counter == 4`

---

### Test 3 — `TimeoutExpires`
**Tasks:** 0–2 only

Task 1 calls `Wait(50)` with no producer present; the call must return `false` and
the measured elapsed time must fall within `[45, 65]` ms. Task 2 then blocks with
a generous timeout; the producer (task 0) fires `NotifyOne()` at tick 130 to wake it.
Verifies both the timeout return value and that the condition variable remains usable
after a timeout has occurred.

**Pass condition:** `counter == 2`
(1 = timeout fired correctly in bounds; 2 = subsequent `NotifyOne()` still woke task 2)

---

### Test 4 — `MutexReacquired`
**Tasks:** 0–2 only

Directly verifies the Monitor pattern atomicity guarantee on both wakeup paths:

- **Signaled path** (task 1): holds mutex, calls `Wait(TIMEOUT)`; producer (task 0)
  acquires the mutex while task 1 is suspended — proving it was released — increments
  the counter, then calls `NotifyOne()`. Task 1 resumes and calls `Unlock()`; if the
  mutex was not re-acquired the ownership assert would fire.
- **Timeout path** (task 2): calls `Wait(50)` with no notification; on timeout calls
  `Unlock()` — same ownership assert check on the timeout path.

**Pass condition:** `counter == 3`
(1 = mutex was free during `Wait()`; 2 = signaled path re-acquired; 3 = timeout path re-acquired)

---

### Test 5 — `PredicateLoop`
**Tasks:** 0–4 (all 5)

Exercises the canonical spurious-wakeup-safe `while (!cond) Wait()` pattern. Each
consumer (tasks 1–4) loops: `while (g_SharedCounter < m_task_id) Wait()`. The producer
increments `g_SharedCounter` and calls `NotifyOne()` once per consumer, paced with
`Delay(1)`. Consumers that wake and find the predicate unsatisfied re-enter `Wait()`,
correctly tolerating both spurious wakeups and out-of-order delivery.

**Pass condition:** `counter == 4` (all four consumers exited their predicate loops)

---

### Test 6 — `NotifyOneOrder`
**Tasks:** 0–4 (all 5)

Verifies FIFO wake ordering. Tasks 1–4 block in scheduling order (1, 2, 3, 4). The
producer calls `NotifyOne()` four times with `Delay(1)` between calls. Each woken
consumer records its `m_task_id` into `g_AcquisitionOrder[g_OrderIndex++]`. After all
consumers have been woken, the verifier checks that the recorded IDs are strictly
ascending.

**Pass condition:** `order == [1,2,3,4]` and `counter == 4`

---

### Test 7 — `NoWaitTimeout`
**Tasks:** 0–2 only

Verifies the `NO_WAIT` fast-path in `ConditionVariable::Wait()`. Task 1 calls
`Wait(g_TestMutex, NO_WAIT)`; per the implementation this returns `false` immediately
without calling `StartWaiting()`. Elapsed time must be less than
`_STK_CV_TEST_SHORT_SLEEP`. Task 2 then performs a normal blocking `Wait()` which the
producer wakes correctly, confirming the condition variable is fully functional after
a `NO_WAIT` call.

**Pass condition:** `counter == 2`
(1 = `NO_WAIT` returned `false` immediately; 2 = subsequent blocking `Wait()` woken correctly)

---

### Test 8 — `StressTest`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 100`

All five tasks alternate producer and consumer roles each iteration. Even iterations
(0, 2, 4, …): acquire mutex, increment `g_SharedCounter`, call `NotifyOne()`, release.
Odd iterations (1, 3, 5, …): acquire mutex, call `Wait(_STK_CV_TEST_SHORT_SLEEP)`,
release. Consumer `Wait()` calls may time out under contention — this is expected and
does not count as a failure. Task 4 uses a `g_InstancesDone` completion barrier to
ensure all five tasks finish before the result is checked.

Minimum guaranteed count: producer iterations only = `5 × ⌈100/2⌉` = `5 × 50 = 250`.

**Pass condition:** `counter >= 250`

---

### Cancellation tests (Tests 9–19)

Tests 9–19 verify that a wait on a `ConditionVariable` can be cancelled and that nothing
is left behind: no stale wait entry, and the mutex always back in the victim's hands. All
use the same task class (`CancelWaitTask`) and **tasks 0–2 only** (`"CancelWait*"` names
are excluded from the extended task set). The cancel is issued by task 0 after
`Sleep(60)`; task 1 is the *victim* that locks the mutex and blocks in `WaitEx()` after
`_STK_CV_TEST_SHORT_SLEEP` ticks; task 2 is a second waiter used only by the two-waiters
tests. Tests 9–14 cancel with `IKernel::CancelTaskWait()`; tests 15–19 repeat the key
scenarios with `stk::SleepCancel()`. The scenario is selected by `param` (`1..6`);
adding `100` selects `SleepCancel`. In every scenario the victim must own the mutex when
`WaitEx()` returns, regardless of how the wait ended.

`StressTest` keeps the number 8 but runs last in `main()`, after the cancellation tests and the
delete-with-waiter tests (20–22).

> **Safety net:** if a cancel does not work, the victim would stay blocked forever and the
> kernel would never stop. On failure task 0 therefore calls `NotifyAll()` twice to release
> any stuck waiter, so a broken cancel is reported as a test failure rather than a hang.

> **Kernel requirement:** tests 15–19 depend on `SleepCancel()` also cancelling a wait on a
> sync object (`OnTaskSleepCancel()` routes through `WaitObject::Cancel()`). Against a kernel
> where `SleepCancel()` only wakes the task, they fail by design: that is the stale wait
> entry regression they exist to catch.

---

### Test 9 — `CancelWait_Infinite`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `1` &nbsp;|&nbsp; **Runs on M0: no**

Task 1 locks the mutex and calls `WaitEx(WAIT_INFINITE)` at about tick 10. Task 0 cancels
it at about tick 60 via `CancelTaskWait()` and waits `2 × _STK_CV_TEST_SHORT_SLEEP` for
the victim to record its result.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, elapsed in `[35, 75]` ms, victim owns the mutex

---

### Test 10 — `CancelWait_NotTimeout`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `2`

Same as test 9 but task 1 waits with a finite 300-tick timeout. Verifies that a cancel is
reported as `WAIT_RESULT_CANCELED`, not `WAIT_RESULT_TIMEOUT`, and that the wait really
ends early instead of running to its deadline.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, elapsed `< 150` ms, victim owns the mutex

---

### Test 11 — `CancelWait_NoStaleWaiter`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `3`

After the cancel, task 1 releases the mutex and starts a `Sleep(60)`, which it measures.
Task 0 then calls `NotifyAll()` with nobody waiting. A condition variable keeps no state,
so there is nothing to poll afterwards; instead, a ghost wait entry left behind by the
cancel would be woken by that notification and cut the victim's sleep short.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, victim owns the mutex, victim sleep `>= 55` ms

---

### Test 12 — `CancelWait_MutexHeld`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `4`

Verifies that a cancelled waiter still goes through mutex re-acquisition. Task 0 locks the
mutex (free, because the victim released it inside `WaitEx()`), cancels the victim, then
sleeps `_STK_CV_TEST_SHORT_SLEEP` ticks while still holding it. The victim has been woken
as cancelled but must stay blocked re-acquiring the mutex: its result is still unset and
`GetOwner()` still names task 0. Task 0 then calls `Unlock()`, which hands the mutex over
to the victim.

**Pass condition:** victim still blocked while task 0 held the mutex, `GetOwner()` was task 0,
then `result == WAIT_RESULT_CANCELED` and victim owns the mutex

---

### Test 13 — `CancelWait_TwoWaiters`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `5`

Task 1 blocks first, task 2 blocks behind it (`Sleep(20)` before locking and calling
`WaitEx(WAIT_INFINITE)`). Task 0 cancels only task 1, then locks the mutex and calls
`NotifyOne()`. The notification must reach task 2, which proves the wait list stayed
intact after the removal of its first entry and that no ghost of task 1 absorbed it.

**Pass condition:** task 1 `WAIT_RESULT_CANCELED`, task 2 `WAIT_RESULT_SIGNAL`, both own the mutex on return

---

### Test 14 — `CancelWait_SignalWins`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `6`

Task 0 locks the mutex, calls `NotifyOne()` and immediately cancels the victim, then
unlocks. The victim is already notified and unlinked from the wait list when the cancel
arrives; it is only blocked re-locking the mutex held by task 0. The cancel must be
absorbed (`Mutex::Lock()` is not cancelable) and the result must stay `WAIT_RESULT_SIGNAL`.

**Pass condition:** `result == WAIT_RESULT_SIGNAL` and victim owns the mutex

---

### Test 15 — `CancelWait_SleepCancel`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `101`

Test 9 using `stk::SleepCancel(tid)` instead of `CancelTaskWait()`.

**Pass condition:** same as test 9

---

### Test 16 — `CancelWait_SleepCancelNoStale`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `103`

Test 11 using `SleepCancel()`. This is the primary regression test for the
`OnTaskSleepCancel()` fix: without it the `WaitObject` stays linked in the condition
variable's wait list after the task is woken.

**Pass condition:** same as test 11

---

### Test 17 — `CancelWait_SleepCancelMutex`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `104`

Test 12 using `SleepCancel()`.

**Pass condition:** same as test 12

---

### Test 18 — `CancelWait_SleepCancelTwo`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `105`

Test 13 using `SleepCancel()`.

**Pass condition:** same as test 13

---

### Test 19 — `CancelWait_SleepCancelSignal`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `106`

Test 14 using `SleepCancel()`. The cancel hits a task that is no longer waiting on the
condition variable but is blocked on the mutex, so this also checks that the cancel is
absorbed there without disturbing the already-delivered notification.

**Pass condition:** same as test 14

---

### Tests 20–22 — `DeleteWithWaiter*`
**Tasks:** 0–2 &nbsp;|&nbsp; **Task class:** `DeleteWithWaiterTask` &nbsp;|&nbsp; **Runs on M0: no**

Verify that destroying a condition variable which tasks are blocked on releases **all** waiters with
`WAIT_RESULT_DELETED`, and that the mutex is re-acquired on this path like on every other. Task 0 constructs a
dedicated condition variable (`g_DeleteCond`, placement-new into `g_DeleteCondStorage`) and sleeps 60 ticks. Task 1
sleeps `_STK_CV_TEST_SHORT_SLEEP`, publishes its `ITask` pointer in `g_Victim`, locks `g_TestMutex` and blocks in
`WaitEx(WAIT_INFINITE)` on that condition variable: nobody notifies it, so only its destruction can end the wait.
Task 0 then destroys the condition variable while it is waited on, and overwrites its storage with `0xDD`, so that any
access to the destroyed object by a woken waiter (or by the kernel) would read garbage. Task 1 records the result,
the elapsed time and whether it owns `g_TestMutex`, without touching the destroyed object. It then performs a
`WaitEx(g_TestMutex, 20)` on `g_TestCond` to show that the task and the kernel are in a sane state: nobody notifies
`g_TestCond`, so the wait must run to its deadline and return `WAIT_RESULT_TIMEOUT` with the mutex re-acquired; a stale
wait entry left behind would end it early or with another result. Task 0 sleeps 40 more ticks and checks the outcome.
If a waiter was not released, task 0 cancels its wait with `g_Kernel.CancelTaskWait()`, so the test fails instead of
hanging the kernel.

**Common pass condition:** task 1's first result is `WAIT_RESULT_DELETED` (not a signal, a timeout or a cancellation)
and it owns `g_TestMutex` on return; the follow-up `WaitEx(g_TestMutex, 20)` on `g_TestCond` returned
`WAIT_RESULT_TIMEOUT` after at least 15 ms with the mutex owned; `g_TestMutex` is free at the end.

#### Test 20 — `DeleteWithWaiter`
**Param:** `DW_BASIC` &nbsp;|&nbsp; Task 2 is added by `RunTest()` but stays idle.

One waiter, the mutex is free when the condition variable is destroyed.

**Pass condition:** common, and the elapsed time of task 1 is in `[35, 75]` ms (started at ~10 ms, destroyed at ~60 ms).

#### Test 21 — `DeleteWithWaiterMutexHeld`
**Param:** `DW_MUTEX_HELD` &nbsp;|&nbsp; Task 2 is added by `RunTest()` but stays idle.

Task 0 locks `g_TestMutex` (free, the waiter released it inside `WaitEx()`) and destroys the condition variable while
holding it, then sleeps `_STK_CV_TEST_SHORT_SLEEP` ticks. The waiter has been woken as deleted but must stay blocked
re-acquiring the mutex: its result is still unset and `GetOwner()` still names task 0. Task 0 then calls `Unlock()`,
which hands the mutex over to the waiter.

**Pass condition:** common, and the waiter was still blocked while task 0 held the mutex, `GetOwner()` was task 0,
and the elapsed time of task 1 is in `[45, 90]` ms (it returns only after the mutex is handed over at ~70 ms).

#### Test 22 — `DeleteWithWaiterTwo`
**Param:** `DW_TWO_WAITERS`

Task 1 blocks first, task 2 blocks behind it (`Sleep(20)`, then locks the mutex and calls `WaitEx(WAIT_INFINITE)`).
The destruction must release both: the wait list of the condition variable is walked completely, not just up to its
first entry.

**Pass condition:** common, and task 2 also gets `WAIT_RESULT_DELETED` with the mutex owned; elapsed time of task 1 is in
`[35, 75]` ms and of task 2 in `[25, 65]` ms (started at ~20 ms)

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `NotifyOneWakesTask` | 0–4 | `counter == 20` | `NotifyOne()` wakes exactly one waiter per call; no over- or under-delivery |
| 2 | `NotifyAllWakesTask` | 0–4 | `counter == 4` | `NotifyAll()` wakes every blocked task in a single call |
| 3 | `TimeoutExpiresTask` | 0–2 | `counter == 2` | `Wait()` returns `false` within the correct timing window; CV remains usable after a timeout |
| 4 | `MutexReacquiredTask` | 0–2 | `counter == 3` | Mutex released atomically inside `Wait()` and re-acquired before return, on both signaled and timeout paths |
| 5 | `PredicateLoopTask` | 0–4 | `counter == 4` | Spurious-wakeup-safe `while (!cond) Wait()` pattern drives all consumers to completion |
| 6 | `NotifyOneOrderTask` | 0–4 | order `[1,2,3,4]`, `counter == 4` | `NotifyOne()` releases waiters in FIFO arrival order |
| 7 | `NoWaitTimeoutTask` | 0–2 | `counter == 2` | `Wait(NO_WAIT)` returns `false` immediately without blocking; CV functional afterwards |
| 8 | `StressTestTask` | 0–4 | `counter >= 250` | No corruption under full five-task contention with alternating producer/consumer roles; runs on all platforms |
| 9 | `CancelWait_Infinite` | 0–2 | `CANCELED`, elapsed `[35, 75]` ms, mutex owned | Cancelling an infinite `WaitEx()` via `CancelTaskWait()` returns `WAIT_RESULT_CANCELED` with the mutex re-acquired |
| 10 | `CancelWait_NotTimeout` | 0–2 | `CANCELED`, elapsed `< 150` ms, mutex owned | A cancel is distinct from a timeout and ends a finite wait early |
| 11 | `CancelWait_NoStaleWaiter` | 0–2 | `CANCELED`, mutex owned, sleep `>= 55` ms | Cancel leaves no stale wait entry for a later `NotifyAll()` to wake |
| 12 | `CancelWait_MutexHeld` | 0–2 | blocked while mutex held, then `CANCELED`, mutex owned | A cancelled waiter still re-acquires the mutex and blocks until it is released |
| 13 | `CancelWait_TwoWaiters` | 0–2 | first `CANCELED`, second `SIGNAL`, both own mutex | Wait list stays intact; the notification reaches the remaining waiter |
| 14 | `CancelWait_SignalWins` | 0–2 | `SIGNAL`, mutex owned | A notification delivered before the cancel wins; the cancel is absorbed during mutex re-lock |
| 15 | `CancelWait_SleepCancel` | 0–2 | as test 9 | `SleepCancel()` cancels a sync-object wait |
| 16 | `CancelWait_SleepCancelNoStale` | 0–2 | as test 11 | `SleepCancel()` leaves no stale wait entry (regression for `OnTaskSleepCancel()`) |
| 17 | `CancelWait_SleepCancelMutex` | 0–2 | as test 12 | `SleepCancel()` still lets the waiter re-acquire the mutex |
| 18 | `CancelWait_SleepCancelTwo` | 0–2 | as test 13 | `SleepCancel()` keeps the wait list intact |
| 19 | `CancelWait_SleepCancelSignal` | 0–2 | as test 14 | `SleepCancel()` after a notification is a harmless no-op |
| 20 | `DeleteWithWaiter` | 0–2 | `DELETED`, mutex owned, elapsed `[35, 75]` ms, follow-up wait on `g_TestCond` times out `>= 15` ms | Destroying a condition variable which a task waits on releases it with `WAIT_RESULT_DELETED`; the mutex is re-acquired; the waiter does not touch the destroyed object and leaves no stale wait entry |
| 21 | `DeleteWithWaiterMutexHeld` | 0–2 | blocked while mutex held, then `DELETED`, mutex owned | A waiter released by the destruction still re-acquires the mutex and blocks until it is handed over |
| 22 | `DeleteWithWaiterTwo` | 0–2 | both `DELETED`, both own mutex | The destruction releases every waiter, not only the first one |
