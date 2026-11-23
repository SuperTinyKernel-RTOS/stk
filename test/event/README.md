# Event Test Suite

Test suite for `stk::sync::Event`.  
Source: `test/event/test_event.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_event.h>
```

`Event` is a binary synchronization primitive (signaled / non-signaled). Two operation
modes are selected at construction time:

- **Auto-reset** (default): `Set()` wakes one waiting task; the event resets to
  non-signaled immediately after that task is released.
- **Manual-reset**: `Set()` wakes all waiting tasks; the signaled state persists until
  `Reset()` is called explicitly.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// Construction
stk::sync::Event g_Evt;                 // auto-reset, initially non-signaled (default)
stk::sync::Event g_Evt(true);           // manual-reset, initially non-signaled
stk::sync::Event g_Evt(false, true);    // auto-reset, initially signaled
stk::sync::Event g_Evt(true,  true);    // manual-reset, initially signaled
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Set` | `bool Set()` | Transitions to signaled state. Auto-reset: wakes one waiter and auto-resets. Manual-reset: wakes all waiters, stays signaled. Returns `true` if state changed, `false` if already signaled. ISR-safe. |
| `Reset` | `bool Reset()` | Transitions to non-signaled state. Returns `true` if state changed, `false` if already non-signaled. ISR-safe. |
| `WaitEx` | `EWaitResult WaitEx(Timeout timeout = WAIT_INFINITE)` | Same as `Wait()`, but preserves the full outcome: `WAIT_RESULT_SIGNAL` (signaled), `WAIT_RESULT_TIMEOUT` (timeout expired, or `NO_WAIT` on a non-signaled event), `WAIT_RESULT_CANCELED` (wait cancelled via `IKernel::CancelTaskWait()` or `SleepCancel()`; the event state is **not** consumed), `WAIT_RESULT_DELETED` (the event was destroyed while waiting; it must not be accessed afterwards), `WAIT_RESULT_FAIL` (kernel rejected the wait). ISR-safe only with `NO_WAIT`. |
| `Wait` | `bool Wait(Timeout timeout = WAIT_INFINITE)` | Blocks until signaled or timeout expires. Takes fast path (returns immediately) if already signaled, auto-resetting in auto-reset mode. Returns `true` if signaled, `false` otherwise (timeout, cancellation, or `NO_WAIT` on a non-signaled event); use `WaitEx()` to tell these apart. ISR-unsafe. |
| `TryWait` | `bool TryWait()` | Non-blocking poll. Returns `true` if signaled at the time of call (auto-resets in auto-reset mode); `false` otherwise. Never blocks. ISR-safe. |
| `Pulse` | `void Pulse()` | Win32 `PulseEvent()` semantics: wakes waiters (one for auto-reset, all for manual-reset) then forces non-signaled, regardless of whether anyone was waiting. ISR-safe. |
| `~Event` | (destructor) | Wakes all tasks still waiting with `WAIT_RESULT_DELETED` (done by `~SyncObjectBase`); they must not access the object afterwards, so a waiter that may outlive the event must use `WaitEx()` to observe it (`Wait()` just returns `false`). |

**Behavior by mode:**

| Operation | Auto-reset | Manual-reset |
|-----------|------------|--------------|
| `Set()` with waiters | Wakes one; resets immediately | Wakes all; stays signaled |
| `Set()` with no waiters | Stays signaled; next `Wait()` fast-paths and resets | Stays signaled; `Wait()` fast-paths without resetting |
| `Reset()` | Clears signaled state; returns `false` if already clear | Clears signaled state; returns `false` if already clear |
| `Pulse()` with waiters | Wakes one; resets | Wakes all; resets |
| `Pulse()` with no waiters | Resets (state never persists) | Resets (state never persists) |
| `TryWait()` signaled | Returns `true`; auto-resets | Returns `true`; does **not** reset |
| `TryWait()` not signaled | Returns `false` immediately | Returns `false` immediately |

**Key invariants:**

- `Set()` and `Reset()` return values reflect whether the state **actually changed**.
- `Reset()` on an already non-signaled event is a no-op returning `false`.
- Auto-reset happens inside `RemoveWaitObject()` when the waiter is released, not
  inside `Set()` itself — the signaled state is cleared atomically with wake delivery.
- A cancelled wait (`WAIT_RESULT_CANCELED`) never consumes the signaled state and leaves
  no stale entry in the wait list: the cancel is recorded before the waiter is unlinked,
  so the auto-reset in `RemoveWaitObject()` only applies to waiters released by a signal.
- If a signal reaches a waiter before a cancel does, the signal wins (`WAIT_RESULT_SIGNAL`)
  and the cancel is a no-op.
- Destroying an `Event` while tasks are waiting releases them with `WAIT_RESULT_DELETED`: they neither
  consume a signal nor time out, and `WaitEx()` returns without reading or writing any member of the
  destroyed object (the auto-reset in `RemoveWaitObject()` does not run, the base-class implementation
  is used because the derived part is already gone).

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_EVT_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_EVT_TEST_TIMEOUT` | `300` ticks | Blocking timeout for `Wait()` calls that must succeed |
| `_STK_EVT_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_EVT_TEST_LONG_SLEEP` | `100` ticks | Sleep used by the verifier task to wait for workers |
| `_STK_EVT_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

`g_TestEvent` is **reconstructed in-place** via placement-new inside `ResetTestState()`
before each test. `ResetTestState()` accepts `manual_reset` and `initial_state`
parameters that are forwarded directly to the `Event` constructor, so the same helper
serves all tests without any additional setup code per test.

The delete-with-waiter test (19) destroys a **separate** event: `g_DeleteEvent` points to an object constructed
(auto-reset, non-signaled) with placement-new in the static `g_DeleteEventStorage`, so `g_TestEvent` is never
destroyed by the test itself. It reuses `g_Victim`, `g_VictimResult[]` and `g_VictimElapsed[]` (see below):
index 0 holds how the wait on the destroyed event ended, index 1 the outcome of the follow-up wait on
`g_TestEvent`. `g_DeleteEvent` is set back to `nullptr` right after the destruction.

The cancellation tests (9–18) additionally use a small amount of shared state, reset by
`ResetTestState()`:

| Variable | Purpose |
|----------|---------|
| `g_Victim` / `g_VictimTid` | Task (and its `TId`) whose wait is cancelled; published by task 1 right before it blocks |
| `g_VictimResult[2]` | `EWaitResult` observed by task 1 / task 2 (`-100` = not set) |
| `g_VictimElapsed[2]` | Milliseconds spent inside `WaitEx()` by task 1 / task 2 |
| `g_VictimSleepElapsed` | Milliseconds task 1 spends in a `Sleep(60)` after its wait was cancelled |

The helper `CancelVictim(use_sleep_cancel)` cancels the victim either with
`g_Kernel.CancelTaskWait(g_Victim)` or with `stk::SleepCancel(g_VictimTid)`.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link all ten
distinct task class templates simultaneously. Test 1 (`AutoResetBasic`) runs on all
platforms; tests 2–19 are skipped on M0 under `#ifndef __ARM_ARCH_6M__`.

`AutoResetBasic` runs on M0 because it uses a single task class template
(`AutoResetBasicTask`) instantiated for all five task slots, fitting within the
available memory. Tests 2–8 each introduce a distinct task class template, tests 9–18
add one more (`CancelWaitTask`, shared by all ten cancellation tests) and test 19 another
(`DeleteWithWaiterTask`), multiplying the
static memory footprint beyond what M0 can accommodate.

| Platform | `_STK_EVT_STACK_SIZE` |
|----------|-----------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `AutoResetBasic`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `iterations = 20` &nbsp;|&nbsp; **Runs on M0: yes**

Task 0 is the producer; tasks 1–4 are consumers. All four consumers loop calling
`Wait(_STK_EVT_TEST_TIMEOUT)`. The producer calls `Set()` 20 times, pacing each with
`Delay(1)` so each signal lands on exactly one blocked consumer before the next fires.
Each successful wake increments `g_SharedCounter`. Verifies that after every `Set()`
the event auto-resets, keeping the remaining consumers blocked.

**Pass condition:** `counter == 20`

---

### Test 2 — `ManualResetBasic`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Mode:** manual-reset

All four consumers (tasks 1–4) block on `Wait(_STK_EVT_TEST_TIMEOUT)`. After
`_STK_EVT_TEST_SHORT_SLEEP` ticks the producer (task 0) fires a single `Set()`.
All four must be released simultaneously. The producer then sleeps
`_STK_EVT_TEST_LONG_SLEEP` and calls `TryWait()` to confirm the event is **still
signaled** (manual-reset does not auto-clear), before explicitly calling `Reset()`.

**Pass condition:** `counter == 4` and `still_signaled == true`

---

### Test 3 — `InitialState`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Mode:** auto-reset, `initial_state = true`

Task 1 calls `Wait(_STK_EVT_TEST_SHORT_SLEEP)` immediately. Because the event was
constructed pre-signaled, `Wait()` takes the fast path and returns `true` without
blocking, and the event auto-resets. Task 1 then calls `Wait()` a second time; the
event is now non-signaled so this must time out and return `false`. Verifies the
`initial_state` constructor parameter and the fast-path auto-reset behavior.

**Pass condition:** `counter == 2`
(1 = first `Wait()` succeeded immediately on pre-signaled event; 2 = second `Wait()` timed out after auto-reset)

---

### Test 4 — `TimeoutWait`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Mode:** auto-reset

Task 1 calls `Wait(50)` after `_STK_EVT_TEST_SHORT_SLEEP`; no producer fires within
the 50-tick window so it must return `false` with elapsed time in `[45, 60]` ms.
Task 0 withholds `Set()` until tick 200. Task 2 sleeps until tick 210 then calls
`Wait(100)`, which must succeed on the now-signaled event. Task 2 is the verifier.

**Pass condition:** `counter == 2`
(1 = timeout fired correctly within bounds; 2 = `Wait()` succeeded after `Set()`)

---

### Test 5 — `TryWait`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Mode:** auto-reset

Task 1 calls `TryWait()` on a non-signaled event; must return `false` immediately
(elapsed < `_STK_EVT_TEST_SHORT_SLEEP`). Task 2 calls `Set()` then `TryWait()`;
must return `true` immediately (event just signaled) and auto-reset. Task 2 then
calls `TryWait()` a second time to confirm the reset: must return `false`. Covers
both the non-signaled and signaled fast paths, and the auto-reset `TryWait()` performs
on success.

**Pass condition:** `counter == 3`
(1 = non-signaled `TryWait()` returned `false` immediately; 2 = signaled `TryWait()` returned `true` immediately; 3 = post-reset `TryWait()` returned `false`)

---

### Test 6 — `ResetManual`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Mode:** manual-reset

Task 0 calls `Set()` then immediately `Reset()` before tasks 1 and 2 have entered
`Wait()`. Verifies three return values: `Set()` returns `true` (state changed),
first `Reset()` returns `true` (state changed), second `Reset()` returns `false`
(already non-signaled). Tasks 1 and 2 sleep briefly then call
`Wait(_STK_EVT_TEST_SHORT_SLEEP)`; both must time out because the event was cleared
before they arrived. Any unexpected wake increments `g_SharedCounter`.

**Pass condition:** `set_changed == true`, `reset_changed == true`, `reset_again == false`, `counter == 0`

---

### Test 7 — `PulseAutoReset`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `iterations = 20`

Task 0 is the producer; tasks 1–4 are consumers. Each consumer loops waiting for
`iterations / 4 = 5` pulses. The producer calls `Pulse()` 20 times with `Delay(1)`
between calls, each waking exactly one consumer. After all consumers exit, the
producer fires one more `Pulse()` with no waiters present, then calls `TryWait()` to
confirm the event is non-signaled — `Pulse()` always resets regardless of whether
anyone was waiting.

**Pass condition:** `counter == 20` and `still_signaled == false`

---

### Test 8 — `PulseManualReset`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Mode:** manual-reset

All four consumers (tasks 1–4) block on `Wait(_STK_EVT_TEST_TIMEOUT)`. After
`_STK_EVT_TEST_SHORT_SLEEP` ticks the producer fires a single `Pulse()`, which must
release all four simultaneously. Unlike `Set()` on a manual-reset event, `Pulse()`
always resets to non-signaled — verified by `TryWait()` returning `false` immediately
after. A second `Pulse()` with no waiters must also leave the event non-signaled.

**Pass condition:** `counter == 4`, `still_signaled == false`, `after_empty_pulse == false`

---

### Cancellation tests (Tests 9–18)

Tests 9–18 verify that a wait on an `Event` can be cancelled and that nothing is left
behind. All use the same task class (`CancelWaitTask`) and **tasks 0–2 only**
(`"CancelWait*"` names are excluded from the extended task set). The cancel is issued by
task 0 after `Sleep(60)`; task 1 is the *victim* that blocks in `WaitEx()` after
`_STK_EVT_TEST_SHORT_SLEEP` ticks; task 2 is a second waiter used only by the two-waiters
tests. Tests 9–14 cancel with `IKernel::CancelTaskWait()`; tests 15–18 repeat the key
scenarios with `stk::SleepCancel()`. The scenario is selected by `param` (`1..6`);
adding `100` selects `SleepCancel`.

> **Safety net:** if a cancel does not work, the victim would stay blocked forever and the
> kernel would never stop. On failure task 0 therefore calls `Set()` twice to release any
> stuck waiter, so a broken cancel is reported as a test failure rather than a hang.

> **Kernel requirement:** tests 15–18 depend on `SleepCancel()` also cancelling a wait on a
> sync object (`OnTaskSleepCancel()` routes through `WaitObject::Cancel()`). Against a kernel
> where `SleepCancel()` only wakes the task, they fail by design: that is the stale wait
> entry regression they exist to catch.

---

### Test 9 — `CancelWait_Infinite`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `1` &nbsp;|&nbsp; **Runs on M0: no**

Task 1 calls `WaitEx(WAIT_INFINITE)` at about tick 10. Task 0 cancels it at about tick 60
via `CancelTaskWait()` and waits `2 × _STK_EVT_TEST_SHORT_SLEEP` for the victim to record
its result.

**Pass condition:** `result == WAIT_RESULT_CANCELED` and elapsed in `[35, 75]` ms

---

### Test 10 — `CancelWait_NotTimeout`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `2`

Same as test 9 but task 1 waits with a finite 300-tick timeout. Verifies that a cancel is
reported as `WAIT_RESULT_CANCELED`, not `WAIT_RESULT_TIMEOUT`, and that the wait really
ends early instead of running to its deadline.

**Pass condition:** `result == WAIT_RESULT_CANCELED` and elapsed `< 150` ms

---

### Test 11 — `CancelWait_NoStaleWaiter`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `3`

After the cancel, task 1 starts a `Sleep(60)` and measures it. Task 0 then calls `Set()`
with nobody waiting, followed by two `TryWait()` calls. Verifies that the cancelled waiter
was unlinked: the signal must be kept in the event (not lost to a ghost waiter), consumed
exactly once by `TryWait()`, and the victim's sleep must not be cut short by a leftover
wait entry.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, `Set() == true`, first `TryWait() == true`,
second `TryWait() == false`, victim sleep `>= 55` ms

---

### Test 12 — `CancelWait_ManualReset`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** manual-reset &nbsp;|&nbsp; **Param:** `4`

Cancel a waiter on a manual-reset event, then walk the event through its states:
`TryWait()` (must be `false`: the cancel did not signal it), `Set()`, `TryWait()` twice
(stays signaled), `Reset()`, `TryWait()`.

**Pass condition:** `result == WAIT_RESULT_CANCELED`, `TryWait()` after cancel `== false`,
`Set() == true`, both following `TryWait()` `== true`, `Reset() == true`, last `TryWait() == false`

---

### Test 13 — `CancelWait_TwoWaiters`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `5`

Task 1 blocks first, task 2 blocks behind it (`Sleep(20)` before `WaitEx(WAIT_INFINITE)`).
Task 0 cancels only task 1, then calls `Set()`. The signal must reach task 2, which proves
the wait list stayed intact after the removal of its first entry, and the auto-reset must
have been applied for it.

**Pass condition:** task 1 `WAIT_RESULT_CANCELED`, task 2 `WAIT_RESULT_SIGNAL`, `Set() == true`,
leftover `TryWait() == false`

---

### Test 14 — `CancelWait_SignalWins`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `6`

Task 0 calls `Set()` and immediately cancels the victim, with no scheduling point in
between. The victim is already signaled and unlinked when the cancel arrives, so the cancel
must be a no-op and exactly one outcome must occur.

**Pass condition:** `result == WAIT_RESULT_SIGNAL` and leftover `TryWait() == false`
(auto-reset was applied for the released waiter)

---

### Test 15 — `CancelWait_SleepCancel`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `101`

Test 9 using `stk::SleepCancel(tid)` instead of `CancelTaskWait()`.

**Pass condition:** same as test 9

---

### Test 16 — `CancelWait_SleepCancelNoStale`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `103`

Test 11 using `SleepCancel()`. This is the primary regression test for the
`OnTaskSleepCancel()` fix: without it the `WaitObject` stays linked in the event's wait
list after the task is woken.

**Pass condition:** same as test 11

---

### Test 17 — `CancelWait_SleepCancelTwo`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `105`

Test 13 using `SleepCancel()`.

**Pass condition:** same as test 13

---

### Test 18 — `CancelWait_SleepCancelSignal`
**Tasks:** 0–2 &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Param:** `106`

Test 14 using `SleepCancel()`. The task is no longer waiting but is still marked as
sleeping for one tick after the wake, so this also checks that `SleepCancel()` falls
through to a plain wake without disturbing the already-delivered signal.

**Pass condition:** same as test 14

---

### Test 19 — `DeleteWithWaiter`
**Tasks:** 0–1 active, task 2 added but idle (`"DeleteWithWaiter"` is excluded from the extended task set) &nbsp;|&nbsp; **Mode:** auto-reset &nbsp;|&nbsp; **Task class:** `DeleteWithWaiterTask` &nbsp;|&nbsp; **Runs on M0: no**

Verifies that destroying an event which a task is blocked on releases the waiter with
`WAIT_RESULT_DELETED`. Task 0 constructs a dedicated event (`g_DeleteEvent`, placement-new into
`g_DeleteEventStorage`) and sleeps 60 ticks. Task 1 sleeps `_STK_EVT_TEST_SHORT_SLEEP`, publishes its `ITask`
pointer in `g_Victim` and blocks in `WaitEx(WAIT_INFINITE)` on that event: nobody signals it, so only its
destruction can end the wait. Task 0 then destroys the event while it is waited for, and overwrites its
storage with `0xDD`, so that any access to the destroyed object by the woken waiter (or by the kernel) would
read garbage. Task 1 records the result and the elapsed time without touching the destroyed object, then
performs a `WaitEx(20)` on `g_TestEvent` to show that the task and the kernel are in a sane state: nobody
signals `g_TestEvent`, so the wait must run to its deadline and return `WAIT_RESULT_TIMEOUT`; a stale wait entry
left behind would end it early or with another result. Task 0 sleeps 40 more ticks and checks the
outcome. If the waiter was not released, task 0 cancels its wait with `g_Kernel.CancelTaskWait()`, so the test
fails instead of hanging the kernel.

**Pass condition:** task 1's first result is `WAIT_RESULT_DELETED` (not a signal, a timeout or a cancellation);
elapsed time is in `[35, 75]` ms (started at ~10 ms, destroyed at ~60 ms); the follow-up `WaitEx(20)` on
`g_TestEvent` returned `WAIT_RESULT_TIMEOUT` after at least 15 ms; `g_TestEvent.TryWait()` is `false` at the end

---

## Summary Table

| # | Test | Tasks | Mode | Pass condition | What it verifies |
|---|------|-------|------|----------------|------------------|
| 1 | `AutoResetBasicTask` | 0–4 | auto-reset | `counter == 20` | `Set()` wakes exactly one waiter per call and auto-resets; runs on all platforms including M0 |
| 2 | `ManualResetBasicTask` | 0–4 | manual-reset | `counter == 4`, `still_signaled == true` | `Set()` wakes all waiters simultaneously; state stays signaled until `Reset()` is called |
| 3 | `InitialStateTask` | 0–2 | auto-reset, pre-signaled | `counter == 2` | `initial_state=true` causes `Wait()` to fast-path immediately; auto-reset clears the state for the next caller |
| 4 | `TimeoutWaitTask` | 0–2 | auto-reset | `counter == 2` | `Wait()` returns `false` within the correct timing window `[45, 60]` ms; event usable after timeout |
| 5 | `TryWaitTask` | 0–2 | auto-reset | `counter == 3` | `TryWait()` returns `false` on non-signaled; returns `true` and auto-resets on signaled; subsequent `TryWait()` returns `false` |
| 6 | `ResetManualTask` | 0–2 | manual-reset | `set/reset_changed` correct, `counter == 0` | `Reset()` return values reflect actual state change; waiters arriving after reset time out correctly |
| 7 | `PulseAutoResetTask` | 0–4 | auto-reset | `counter == 20`, `still_signaled == false` | `Pulse()` wakes exactly one waiter and always resets, including when no waiters are present |
| 8 | `PulseManualResetTask` | 0–4 | manual-reset | `counter == 4`, both booleans `false` | `Pulse()` wakes all waiters and always resets — unlike `Set()`, which leaves manual-reset events signaled |
| 9 | `CancelWait_Infinite` | 0–2 | auto-reset | `CANCELED`, elapsed `[35, 75]` ms | Cancelling an infinite `WaitEx()` via `CancelTaskWait()` returns `WAIT_RESULT_CANCELED` |
| 10 | `CancelWait_NotTimeout` | 0–2 | auto-reset | `CANCELED`, elapsed `< 150` ms | A cancel is distinct from a timeout and ends a finite wait early |
| 11 | `CancelWait_NoStaleWaiter` | 0–2 | auto-reset | `CANCELED`, `Set`/`TryWait` sequence, sleep `>= 55` ms | Cancel leaves no stale wait entry; the signal is neither lost nor consumed by the cancel |
| 12 | `CancelWait_ManualReset` | 0–2 | manual-reset | `CANCELED`, event not signaled by cancel | Cancel does not change manual-reset state; `Set`/`Reset` still behave normally |
| 13 | `CancelWait_TwoWaiters` | 0–2 | auto-reset | first `CANCELED`, second `SIGNAL` | Wait list stays intact; the signal reaches the remaining waiter |
| 14 | `CancelWait_SignalWins` | 0–2 | auto-reset | `SIGNAL`, no leftover signal | A signal delivered before the cancel wins; the cancel is a no-op |
| 15 | `CancelWait_SleepCancel` | 0–2 | auto-reset | as test 9 | `SleepCancel()` cancels a sync-object wait |
| 16 | `CancelWait_SleepCancelNoStale` | 0–2 | auto-reset | as test 11 | `SleepCancel()` leaves no stale wait entry (regression for `OnTaskSleepCancel()`) |
| 17 | `CancelWait_SleepCancelTwo` | 0–2 | auto-reset | as test 13 | `SleepCancel()` keeps the wait list intact |
| 18 | `CancelWait_SleepCancelSignal` | 0–2 | auto-reset | as test 14 | `SleepCancel()` after a signal is a harmless no-op |
| 19 | `DeleteWithWaiterTask` | 0–1 (+ idle 2) | auto-reset | `DELETED`, elapsed `[35, 75]` ms, follow-up wait on `g_TestEvent` times out `>= 15` ms | Destroying an event which a task waits for releases the waiter with `WAIT_RESULT_DELETED`; the waiter does not touch the destroyed object and leaves no stale wait entry |
