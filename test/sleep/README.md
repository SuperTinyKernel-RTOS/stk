# Sleep Test Suite

Test suite for `stk::Sleep()` and `stk::SleepUntil()`, including cancellation of sleeping and waiting tasks
with `stk::SleepCancel()` and `IKernel::CancelTaskWait()`.  
Source: `test/sleep/test_sleep.cpp`

---

## API Summary

```cpp
#include <stk.h>
```

```cpp
void stk::Sleep(Timeout ticks);                       // sleep for a number of ticks
bool stk::SleepUntil(Ticks timestamp);                // sleep until an absolute tick timestamp
void stk::SleepCancel(TId task_id);                   // end a sleep / cancel a sync-object wait early
void IKernel::CancelTaskWait(ITask *user_task);       // cancel a wait on a sync object (KERNEL_SYNC)
EWaitResult IKernelService::Wait(sobj, mutex, ticks); // wait on a sync object (used directly by the suite)
```

The calling task is excluded from scheduling until its sleep expires or it is woken early. What happens
to a task that is cancelled depends on what it is blocked in:

| Call | Target blocked in | Behaviour |
|------|-------------------|-----------|
| `Sleep(N)` | - | Never wakes earlier than `N` ticks after the call; typically after `N + 1` or `N + 2` ticks (entry tick plus scheduling latency). `Sleep(0)` neither sleeps nor yields. |
| `SleepUntil(t)` | - | Sleeps for `t - now` ticks. Returns `true` if the task slept, `false` immediately if `t` is already reached or passed. Absolute timestamps do not accumulate drift in periodic loops. |
| `SleepCancel(tid)` | `Sleep()` / `SleepUntil()` | The sleep ends early. |
| `SleepCancel(tid)` | `Wait()` on a sync object | The wait is cancelled, `Wait()` returns `WAIT_RESULT_CANCELED`. |
| `CancelTaskWait(task)` | `Wait()` on a sync object | The wait is cancelled, `Wait()` returns `WAIT_RESULT_CANCELED`. |
| `CancelTaskWait(task)` | `Sleep()` / `SleepUntil()` | **No-op**, the sleep continues in full. |

**Cancel path in the kernel:**

```
SleepCancel(tid)        →  OnTaskSleepCancel(): (critical section)
  task waits on sync obj →  WaitObject::Cancel(): unlink from wait list, state = CANCELLED, task->Wake()
  task plain sleeping    →  task->Wake()  (m_time_sleep = -1, wakes on the next tick)

CancelTaskWait(task)    →  (critical section)
  task waits on sync obj →  WaitObject::Cancel()
  otherwise              →  nothing
```

**Key invariants:**

- A sleep is never shorter than requested.
- Sleeping tasks do not disturb each other: the wake order follows the sleep durations, and busy tasks do not
  delay a sleeper beyond scheduling latency.
- `SleepUntil()` with a timestamp that is not in the future returns `false` without sleeping.
- A cancelled wait is distinguishable from a signal and from a timeout (`WAIT_RESULT_CANCELED`), and leaves no
  stale state: the next `Wait()` of the same task behaves normally.
- Cancelling one waiter does not affect other waiters of the same sync object.
- `CancelTaskWait()` is specific to sync-object waits; a plain sleep is ended with `SleepCancel()`.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_SLEEP_TEST_TASKS_MAX` | `5` | Kernel task capacity (and number of task objects created by `RunTest`) |
| `_STK_SLEEP_TEST_SHORT_SLEEP` | `10` ms | Polling interval used by verifier tasks while waiting for the others |
| `_STK_SLEEP_TEST_LO_MS` | `2` ms | Allowed "early" slack of a measured sleep (measurement jitter) |
| `_STK_SLEEP_TEST_HI_MS` | `25` ms | Allowed "late" slack of a measured sleep; tune for slow simulators/targets |
| `_STK_SLEEP_TEST_FAST_MS` | `50` ms | Upper bound for 100 calls of `Sleep(0)` |
| `_STK_SLEEP_TEST_LONG_MS` | `500` ms | Sleep / wait time of a task that is going to be cancelled |
| `_STK_SLEEP_TEST_CANCEL_AT_MS` | `20` ms | Time at which the canceller acts |
| `_STK_SLEEP_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

Timing assumes the default 1 ms tick. A measured sleep of `N` ms is accepted in
`[N - LO_MS, N + HI_MS]`. A cancelled sleep or wait is accepted if it ended in
`[CANCEL_AT_MS - LO_MS, LONG_MS / 2)`, i.e. after the canceller acted and long before the full time.

The suite is built once, with the kernel configured as:

```
Kernel<KERNEL_DYNAMIC | KERNEL_SYNC (| KERNEL_TICKLESS when STK_TICKLESS_IDLE), 5, sched::SwitchStrategyRR, PlatformDefault>
```

`KERNEL_SYNC` is required by `Wait()` and `CancelTaskWait()`. Tasks terminate by returning from `Run()`, which
is how `Start()` returns after each test.

**Local sync primitives.** To avoid a dependency on the sync primitives headers, the suite defines:

| Class | Purpose |
|-------|---------|
| `TestSync` | Minimal sync object (`SyncObjectBase`, wait list only), signalled explicitly with `IKernelService::Wake()` |
| `TestMutex` | No-op `IMutex` handed to `Wait()`; sufficient because only the kernel's `Wait()` contract is under test |

**Test state:** all shared state lives in static arrays indexed by task id, and every element has
a single writer:

| Variable | Purpose |
|----------|---------|
| `g_Counter[id]` | Per-task progress counter |
| `g_Done[id]` | Per-task completion flag |
| `g_Elapsed[id]` | Measured sleep / wait time (ms) |
| `g_WakeMs[id]` | Absolute wake time (ms), used to check the wake order (test 1) |
| `g_Early[id]` | Per-task count of early wakeups |
| `g_Result[id]` | Per-task result (`EWaitResult` or a boolean) |
| `g_Result2`, `g_Elapsed2` | Result and time of the second wait of the victim (test 15); single writer |
| `g_Tid[id]`, `g_TaskPtr[id]` | Identity of each task of the cancellation tests, published at the start of `Run()` |
| `g_Flag` | Raised by the sleeper to stop the spinners (test 3) |
| `g_TaskCount` | Number of tasks that must report completion before a verifier proceeds |

Completion is tracked with per-task flags rather than a shared counter: a shared volatile `++`
can lose an update if a tick preempts it, which would leave the verifier waiting forever.
`ResetTestState(expected_done)` clears all state before each test.

`RunTest<T>(name, tasks_added, param)` constructs all five task objects but adds
only the first `tasks_added` to the kernel. `param` is an iteration count, or the scenario id for `CancelTask`.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link several distinct
task class templates simultaneously. Tests 1–15 are skipped on M0 and only `SleepStress` (test 16)
runs; tests 1–15 are compiled under `#ifndef __ARM_ARCH_6M__`. On M0 the task objects are
constructed on the stack of `RunTest` instead of being `static` (`STK_TASK` expands to nothing).

| Platform | `_STK_SLEEP_STACK_SIZE` | Tests run |
|----------|-------------------------|-----------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words | 16 only |
| All others | `256` words | 1–16 |

---

## Tests

### Test 1 — `SleepDurationAndOrder`
**Tasks:** 0–2 &nbsp;|&nbsp; **Class:** `SleepDurationTask`

Tasks sleep 30 ms (task 0), 10 ms (task 1) and 20 ms (task 2) and record their elapsed time and absolute
wake time. Task 0 acts as verifier. Verifies the length of a sleep and that tasks wake in order of their
sleep durations.

**Pass condition:** every elapsed time in `[d - 2, d + 25]` ms and wake order `t1 <= t2 <= t0`

---

### Test 2 — `SleepZeroAndOneTick`
**Tasks:** 0 &nbsp;|&nbsp; **Param:** `iterations = 100` &nbsp;|&nbsp; **Class:** `SleepEdgeTask`

A single task calls `Sleep(0)` 100 times, then `Sleep(1)` 20 times and measures both loops. `Sleep(0)` must
return promptly (it neither sleeps nor yields); each `Sleep(1)` must really sleep for at least one tick.

**Pass condition:** `g_Counter[0] == 100`, `Sleep(0)` loop `< 50` ms, `Sleep(1)` loop in `[20, 85]` ms

---

### Test 3 — `SleeperAmongSpinners`
**Tasks:** 0–2 &nbsp;|&nbsp; **Class:** `SleeperAmongSpinnersTask`

Task 0 sleeps 50 ms, measures the elapsed time and raises `g_Flag`. Tasks 1 and 2 busy-spin (no `Yield()`),
counting iterations, until the flag is raised. Verifies that a fully loaded CPU does not delay the sleeper
beyond scheduling latency and that the spinners keep running.

**Pass condition:** elapsed in `[48, 75]` ms and `g_Counter[1] > 0` and `g_Counter[2] > 0`

---

### Test 4 — `SleepUntilBasic`
**Tasks:** 0 &nbsp;|&nbsp; **Class:** `SleepUntilBasicTask`

A single task calls `SleepUntil()` with four kinds of timestamps:

| Case | Expectation |
|------|-------------|
| `now + 40` | Returns `true`, wakes at or after the timestamp and no later than 25 ticks past it |
| `now - 5` (past) | Returns `false`, takes at most 2 ticks |
| `now` (current) | Returns `false`, takes at most 2 ticks |
| `now + 1` (next tick) | Never wakes before the timestamp (the result may be either, depending on the tick boundary) |

**Pass condition:** all cases above hold

---

### Test 5 — `SleepUntilPeriodic`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `iterations = 10` &nbsp;|&nbsp; **Class:** `SleepUntilPeriodicTask`

Each task runs a periodic loop with its own period (5, 10 and 15 ms), where every next target is derived from
the previous target rather than from the current time. Verifies that no wakeup is early and that no drift
accumulates. Task 0 acts as verifier.

**Pass condition:** `g_Counter[i] == 10`, `g_Early[i] == 0` and total elapsed in
`[10 * period, 10 * period + 25]` ms for tasks 0–2

---

### Test 6 — `SleepCancelOnSleep`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_SLEEP_CANCEL` &nbsp;|&nbsp; **Class:** `CancelTask`

Cancellation tests 6–15 share the `CancelTask` class. Task 0 is the **victim** (it sleeps or waits, and
acts as verifier), task 1 is the **canceller / signaller** which acts after 20 ms, task 2 is a **bystander**
(test 14 only). Each task publishes its `TId` and `ITask *` at the start of `Run()`.

Here the victim calls `Sleep(500)` and the canceller calls `SleepCancel(tid0)`.

**Pass condition:** victim elapsed in `[18, 250)` ms

---

### Test 7 — `SleepCancelOnSleepUntil`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_SLEEPUNTIL_CANCEL` &nbsp;|&nbsp; **Class:** `CancelTask`

The victim calls `SleepUntil(now + 500)`, the canceller calls `SleepCancel(tid0)`. The return value of the
cancelled `SleepUntil()` is printed but not asserted (the contract does not define it for a cancelled sleep).

**Pass condition:** victim elapsed in `[18, 250)` ms

---

### Test 8 — `CancelTaskWaitOnPlainSleepIsNoop`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_CANCELWAIT_ON_SLEEP` &nbsp;|&nbsp; **Class:** `CancelTask`

The victim calls `Sleep(100)`, the canceller calls `g_Kernel.CancelTaskWait(task0)` after 20 ms. The task is
not blocked on a sync object, so the call must have no effect and the sleep must run in full.

**Pass condition:** victim elapsed in `[98, 125]` ms

---

### Test 9 — `WaitTimeout`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_WAIT_TIMEOUT` &nbsp;|&nbsp; **Class:** `CancelTask`

Control case. The victim calls `Wait(30 ms)` on the test sync object; nobody acts. Establishes the timeout
result that a cancel must be distinguishable from.

**Pass condition:** result `WAIT_RESULT_TIMEOUT` and elapsed in `[28, 55]` ms

---

### Test 10 — `WaitSignal`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_WAIT_SIGNAL` &nbsp;|&nbsp; **Class:** `CancelTask`

Control case. The victim calls `Wait(500 ms)`; the signaller wakes one waiter with
`IKernelService::Wake(&sync, false)` after 20 ms. Establishes the signal result.

**Pass condition:** result `WAIT_RESULT_SIGNAL` and victim elapsed in `[18, 250)` ms

---

### Test 11 — `CancelTaskWaitTimedWait`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_WAIT_CANCEL` &nbsp;|&nbsp; **Class:** `CancelTask`

The victim calls `Wait(500 ms)`, the canceller calls `CancelTaskWait(task0)` after 20 ms. The wait must end
long before its timeout and report a cancel, not a timeout or a signal.

**Pass condition:** result `WAIT_RESULT_CANCELED` and victim elapsed in `[18, 250)` ms

---

### Test 12 — `CancelTaskWaitInfiniteWait`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_WAIT_CANCEL_INFINITE` &nbsp;|&nbsp; **Class:** `CancelTask`

As test 11, but the victim waits with `WAIT_INFINITE`. Without the cancel the wait would never end.

**Pass condition:** result `WAIT_RESULT_CANCELED` and victim elapsed in `[18, 250)` ms

---

### Test 13 — `SleepCancelOnWait`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_WAIT_SLEEPCANCEL` &nbsp;|&nbsp; **Class:** `CancelTask`

The victim calls `Wait(500 ms)`, the canceller calls `SleepCancel(tid0)` (not `CancelTaskWait()`). `SleepCancel()`
must cancel a sync-object wait the same way, i.e. unlink the wait object and report `WAIT_RESULT_CANCELED`.

**Pass condition:** result `WAIT_RESULT_CANCELED` and victim elapsed in `[18, 250)` ms

---

### Test 14 — `CancelTaskWaitSelective`
**Tasks:** 0–2 &nbsp;|&nbsp; **Scenario:** `SC_SELECTIVE_CANCEL` &nbsp;|&nbsp; **Class:** `CancelTask`

Task 0 and the bystander (task 2) both wait 150 ms on the same sync object. The canceller cancels task 0
only. Verifies that cancelling one waiter leaves the other waiter in the wait list with its own timeout intact.

**Pass condition:** task 0: `WAIT_RESULT_CANCELED` and elapsed in `[18, 250)` ms; task 2:
`WAIT_RESULT_TIMEOUT` and elapsed in `[148, 175]` ms

---

### Test 15 — `CancelThenRewait`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_CANCEL_THEN_REWAIT` &nbsp;|&nbsp; **Class:** `CancelTask`

The victim's `Wait(500 ms)` is cancelled by `CancelTaskWait()`, then the victim immediately calls
`Wait(30 ms)` on the same sync object. Verifies that a cancelled wait leaves no stale state (wait list entry,
wait result, sleep counter) which could corrupt the next wait of the same task.

**Pass condition:** first wait: `WAIT_RESULT_CANCELED` and elapsed in `[18, 250)` ms; second wait:
`WAIT_RESULT_TIMEOUT` and elapsed in `[28, 55]` ms

---

### Test 16 — `SleepStress`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 100` &nbsp;|&nbsp; **Class:** `SleepStressTask`

Each task runs 100 iterations alternating `Sleep(d)` (odd iterations) and `SleepUntil(now + d)` (even
iterations), with `d` in 1..5 ticks chosen differently per task and iteration. The mix puts wakeups at
arbitrary points of the rotation, with several tasks sleeping and waking at once. Task 4 (the last task) acts
as verifier and waits for the others.

**Pass condition:** `g_Counter[i] == 100` for tasks 0–4 and no early wakeup in any task

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `SleepDurationTask` | 0–2 | elapsed `[d-2, d+25]` ms, wake order by duration | Sleep length and wake ordering |
| 2 | `SleepEdgeTask` | 0 | `Sleep(0)` loop `< 50` ms; `Sleep(1)` takes at least a tick | Edge values of `Sleep()` |
| 3 | `SleeperAmongSpinnersTask` | 0–2 | sleeper elapsed `[48, 75]` ms, spinners progressed | Sleeper wakes on time on a loaded CPU |
| 4 | `SleepUntilBasicTask` | 0 | future `true` and on time; past/current `false` at once | `SleepUntil()` return value and edge timestamps |
| 5 | `SleepUntilPeriodicTask` | 0–2 | counters `== 10`, no early wake, total within `+25` ms | No drift in absolute-timestamp loops |
| 6 | `CancelTask` (`SC_SLEEP_CANCEL`) | 0–1 | elapsed `[18, 250)` ms | `SleepCancel()` ends `Sleep()` |
| 7 | `CancelTask` (`SC_SLEEPUNTIL_CANCEL`) | 0–1 | elapsed `[18, 250)` ms | `SleepCancel()` ends `SleepUntil()` |
| 8 | `CancelTask` (`SC_CANCELWAIT_ON_SLEEP`) | 0–1 | full sleep, elapsed `[98, 125]` ms | `CancelTaskWait()` does not touch a plain sleep |
| 9 | `CancelTask` (`SC_WAIT_TIMEOUT`) | 0–1 | `TIMEOUT`, elapsed `[28, 55]` ms | Control: timeout result |
| 10 | `CancelTask` (`SC_WAIT_SIGNAL`) | 0–1 | `SIGNAL`, ended early | Control: signal result |
| 11 | `CancelTask` (`SC_WAIT_CANCEL`) | 0–1 | `CANCELED`, ended early | `CancelTaskWait()` on a timed wait |
| 12 | `CancelTask` (`SC_WAIT_CANCEL_INFINITE`) | 0–1 | `CANCELED`, ended early | `CancelTaskWait()` on an infinite wait |
| 13 | `CancelTask` (`SC_WAIT_SLEEPCANCEL`) | 0–1 | `CANCELED`, ended early | `SleepCancel()` also cancels a sync-object wait |
| 14 | `CancelTask` (`SC_SELECTIVE_CANCEL`) | 0–2 | task 0 `CANCELED`, task 2 `TIMEOUT` `[148, 175]` ms | Only the targeted waiter is cancelled |
| 15 | `CancelTask` (`SC_CANCEL_THEN_REWAIT`) | 0–1 | `CANCELED`, then `TIMEOUT` `[28, 55]` ms | No stale wait state after a cancel |
| 16 | `SleepStressTask` | 0–4 | each counter `== 100`, no early wake | No hang, lost iteration or early wake under heavy sleep/wake traffic; runs on all platforms |
