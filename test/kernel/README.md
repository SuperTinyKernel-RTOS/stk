# Kernel Test Suite

Test suite for kernel functions running on a real kernel instance (target or simulator). It currently covers the
scheduler lock API (`stk::LockScheduler()`, `stk::UnlockScheduler()`, `stk::RestoreSchedulerLock()` and
`stk::ScopedSchedulerLock`); further kernel level tests are added to the same suite.  
Source: `test/kernel/test_kernel.cpp`

---

## API Summary

```cpp
#include <stk.h>
```

```cpp
ELockResult stk::LockScheduler();                  // disable task switching, returns the PREVIOUS state
ELockResult stk::UnlockScheduler();                // enable task switching, returns the PREVIOUS state
ELockResult stk::RestoreSchedulerLock(bool locked);// set the lock to a saved state, returns the NEW state
class stk::ScopedSchedulerLock;                    // RAII: locks, restores the state before construction on exit
IKernel::EKernelState IKernel::GetState();         // KSTATE_LOCKED while the scheduler is locked
```

The scheduler lock disables **task switching only**: ticks, sleep timers, timeouts and ISRs keep running. It is a
soft critical section among the tasks of one kernel, it does not protect data shared with ISRs or other cores (use
`sync::ScopedCriticalSection` / `hw::CriticalSection` for that).

| Call | Result |
|------|--------|
| `LockScheduler()` | `LOCK_WAS_UNLOCKED` / `LOCK_WAS_LOCKED` (previous state), `LOCK_FAIL`, `LOCK_FAIL_ISR` |
| `UnlockScheduler()` | `LOCK_WAS_UNLOCKED` / `LOCK_WAS_LOCKED` (previous state), `LOCK_FAIL`, `LOCK_FAIL_ISR` |
| `RestoreSchedulerLock(locked)` | `LOCK_WAS_LOCKED` / `LOCK_WAS_UNLOCKED` (**new** state), `LOCK_FAIL`, `LOCK_FAIL_ISR` |

**Rules of the lock holder:**

- The lock is **not nested**. To nest, save the result of `LockScheduler()` and pass `(prev == LOCK_WAS_LOCKED)`
  to `RestoreSchedulerLock()`; `ScopedSchedulerLock` does this itself.
- The holder must keep the CPU: `Sleep()`, `SleepUntil()`, `Yield()`, `Wait()`, suspending itself and exiting
  assert in a debug build and are no-ops (or fail) in a release build. `Delay()` (busy-wait) and suspending
  another task are allowed.
- A task woken (`Wake()`), or a sleeper whose timer expires, while the scheduler is locked is scheduled only after
  the unlock (on the next tick).
- `LOCK_FAIL` is returned in `KERNEL_HRT` mode and when the kernel is not running; `LOCK_FAIL_ISR` from an ISR.

**Key invariants:**

- While locked, `GetState()` is `KSTATE_LOCKED`, otherwise `KSTATE_RUNNING`.
- No other task runs while the scheduler is locked, however long the holder runs.
- The tick is not stopped by the lock: time advances and sleep timers expire, only the switch is deferred.
- An inner guard / `RestoreSchedulerLock()` with a saved state never releases an outer lock.
- A section guarded by `ScopedSchedulerLock` is never entered by two tasks at a time.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_KERNEL_TEST_TASKS_MAX` | `5` | Kernel task capacity (and number of task objects created by `RunTest`) |
| `_STK_KERNEL_TEST_SHORT_SLEEP` | `10` ms | Polling interval of the verifier, also the time the observer gets to start |
| `_STK_KERNEL_TEST_LO_MS` | `2` ms | Allowed "early" slack of a measured time (measurement jitter) |
| `_STK_KERNEL_TEST_HI_MS` | `25` ms | Allowed "late" slack of a measured time; tune for slow simulators/targets |
| `_STK_KERNEL_TEST_LOCK_MS` | `40` ms | Time the lock holder keeps the scheduler locked (busy-wait) |
| `_STK_KERNEL_TEST_SETTLE_MS` | `20` ms | Time given to the other tasks to run after the unlock |
| `_STK_KERNEL_TEST_REGION_LOOPS` | `5000` | Busy loop inside of a guarded section (widens the race window, test 6) |
| `_STK_KERNEL_TEST_REGION_ITERS` | `200` | Guarded section entries per task (test 6) |
| `_STK_KERNEL_STACK_SIZE` | `256` (M0, others) / `384` (RISC-V) | Per-task stack size in `size_t` words |

Timing assumes the default 1 ms tick. A lock that is held for `LOCK_MS` must show at least
`LOCK_MS - LO_MS` elapsed ticks; a task that was waiting for the unlock must resume in
`[unlock - LO_MS, unlock + HI_MS]`.

The suite is built once, with the kernel configured as:

```
Kernel<KERNEL_DYNAMIC | KERNEL_SYNC (| KERNEL_TICKLESS when STK_TICKLESS_IDLE), 5, SwitchStrategyRR, PlatformDefault>
```

`KERNEL_SYNC` is required by `Wait()` / `Wake()` (test 5). Tasks terminate by returning from `Run()`, which is how
`Start()` returns after each test.

**Local sync primitives.** To avoid a dependency on the sync primitives headers, the suite defines:

| Class | Purpose |
|-------|---------|
| `TestSync` | Minimal sync object (`SyncObjectBase`, wait list only), signalled explicitly with `IKernelService::Wake()` |
| `TestMutex` | No-op `IMutex` handed to `Wait()`; sufficient because only the kernel's contract is under test |

**Test state:** all shared state lives in static arrays indexed by task id, and every element has a single writer
(except the mutual exclusion test, see below):

| Variable | Purpose |
|----------|---------|
| `g_Counter[id]` | Per-task progress counter (spinner progress; observer state: 1 = about to sleep / wait, 2 = returned) |
| `g_Done[id]` | Per-task completion flag |
| `g_WakeMs[id]` | Absolute time (ms) at which the observer resumed, `0` = has not resumed |
| `g_Result[id]` | Result of the observer's `Wait()` (`EWaitResult`) |
| `g_Flag` | Raised by the holder to stop the spinner (tests 2, 3) |
| `g_Inside`, `g_Violations`, `g_Shared`, `g_Spin` | Mutual exclusion test: tasks inside the section, overlaps detected, protected value, busy loop sink |
| `g_TaskCount` | Number of tasks that must report completion before a verifier proceeds |

Completion is tracked with per-task flags rather than a shared counter: a shared volatile `++` can lose an update
if a tick preempts it, which would leave the verifier waiting forever. `ResetTestState(expected_done)` clears all
state before each test.

`RunTest<T>(name, tasks_added, param)` constructs all five task objects but adds only the first `tasks_added` to the
kernel. `param` is the scenario id for `LockTask` (unused for `LockApiTask`).

**Roles in `LockTask`:** task 0 is the **lock holder** (and verifier), task 1 is the **observer** (spinner,
sleeper or waiter, depending on the scenario). Test 6 uses all tasks equally.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link several distinct task class templates
simultaneously, as in the sleep suite. Tests 2–6 (`LockTask`) are compiled under `#ifndef __ARM_ARCH_6M__` and
skipped; only test 1 (`LockApiTask`) runs. On M0 the task objects are constructed on the stack of `RunTest`
instead of being `static` (`STK_TASK` expands to nothing).

| Platform | `_STK_KERNEL_STACK_SIZE` | Tests run |
|----------|--------------------------|-----------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `256` words | 1 only |
| RISC-V (`_STK_ARCH_RISC_V`) | `384` words | 1–6 |
| All others | `256` words | 1–6 |

---

## Not Covered Here

A contract violation of the lock holder asserts in a debug build and can not run to completion on a real target, so
the following are covered by the mock based unit tests of the kernel (`stktest_kernel.cpp`) instead:

- `Sleep()`, `SleepUntil()`, `Yield()`, `Wait()` and `SuspendTask()` of the calling task while locked,
- exit of the lock holder (the lock is released when it leaves the CPU),
- `LOCK_FAIL_ISR` (call from an ISR) and `LOCK_FAIL` (`KERNEL_HRT`, kernel not running, suspended),
- time-slice accounting while locked, tickless suspend dropping the lock.

---

## Tests

### Test 1 — `SchedulerLockApi`
**Tasks:** 0 &nbsp;|&nbsp; **Class:** `LockApiTask`

A single task walks through the whole API and checks both the returned `ELockResult` and `GetState()` after each
step:

| Step | Expectation |
|------|-------------|
| initial state | `KSTATE_RUNNING` |
| `LockScheduler()` twice | `LOCK_WAS_UNLOCKED`, then `LOCK_WAS_LOCKED`; `KSTATE_LOCKED` (not nested) |
| `UnlockScheduler()` twice | `LOCK_WAS_LOCKED`, then `LOCK_WAS_UNLOCKED`; one unlock releases the lock |
| `RestoreSchedulerLock(true)` twice, then `(false)` twice | the **new** state is reported: `LOCK_WAS_LOCKED`, `LOCK_WAS_LOCKED`, `LOCK_WAS_UNLOCKED`, `LOCK_WAS_UNLOCKED` |
| emulated nesting: `outer = Lock()`, `inner = Lock()` | `outer == LOCK_WAS_UNLOCKED`, `inner == LOCK_WAS_LOCKED`; restoring `inner` keeps the lock, restoring `outer` releases it |
| `ScopedSchedulerLock` in a scope | locked inside, `KSTATE_RUNNING` after |
| two nested `ScopedSchedulerLock` | still locked after the inner one, released after the outer one |
| `ScopedSchedulerLock` inside of a manual `LockScheduler()` | the manual lock stays held after the guard; `UnlockScheduler()` returns `LOCK_WAS_LOCKED` |
| `Sleep(5 ms)` afterwards | the task still sleeps and wakes normally |

**Pass condition:** every check above holds

---

### Test 2 — `SchedulerLockNoSwitch`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_NO_SWITCH` &nbsp;|&nbsp; **Class:** `LockTask`

Task 1 is a **spinner**: it increments `g_Counter[1]` without ever yielding. Task 0 sleeps 10 ms so the spinner
gets the CPU, locks the scheduler and busy-waits with `Delay(40 ms)` (allowed while locked), then unlocks and
sleeps 20 ms.

**Pass condition:**
- before the lock `g_Counter[1] > 0`
- `LockScheduler()` returns `LOCK_WAS_UNLOCKED`, state is `KSTATE_LOCKED`
- the spinner made **no progress** while locked (counter unchanged)
- time advanced by at least `40 - 2` ticks (the tick is not stopped)
- `UnlockScheduler()` returns `LOCK_WAS_LOCKED`, state is `KSTATE_RUNNING`
- the spinner progressed again after the unlock

---

### Test 3 — `SchedulerLockNestedGuards`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_NESTED_GUARDS` &nbsp;|&nbsp; **Class:** `LockTask`

As test 2, but the holder uses two nested `ScopedSchedulerLock` guards. The inner guard is released first and the
holder then busy-waits under the outer one. Verifies that restoring the saved state of the inner guard does not
release the outer lock.

**Pass condition:** as test 2; additionally the state is still `KSTATE_LOCKED` right after the inner guard has been
released, and `KSTATE_RUNNING` after the outer one

---

### Test 4 — `SchedulerLockSleeperDeferred`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_SLEEPER_DEFERRED` &nbsp;|&nbsp; **Class:** `LockTask`

Task 1 (the **sleeper**) calls `Sleep(10 ms)` and records the time it resumed. Once the sleeper has started its
sleep, the holder locks the scheduler and busy-waits for 40 ms, i.e. the sleeper's timer expires **while the
scheduler is locked**. Verifies that the tick keeps counting sleep timers under the lock and that the expired
sleeper is only scheduled after the unlock.

**Pass condition:**
- time advanced by at least `40 - 2` ms under the lock
- the sleeper has **not** run before the unlock (`g_WakeMs[1] == 0`)
- the sleeper resumed in `[unlock - 2, unlock + 25]` ms

---

### Test 5 — `SchedulerLockWakeDeferred`
**Tasks:** 0–1 &nbsp;|&nbsp; **Scenario:** `SC_WAKE_DEFERRED` &nbsp;|&nbsp; **Class:** `LockTask`

Task 1 (the **waiter**) blocks in `Wait()` on the test sync object with `WAIT_INFINITE`. The holder locks the
scheduler, wakes the waiter with `IKernelService::Wake(&sync, false)` (inside a critical section) and
busy-waits for 40 ms. Verifies that a wake-up under the lock only changes the state of the waiter, the waiter is
not scheduled until the unlock.

**Pass condition:**
- before the lock the waiter is still blocked (`g_Counter[1] == 1`)
- during the lock the waiter has not returned (`g_Counter[1] == 1`, `g_WakeMs[1] == 0`)
- after the unlock `g_Counter[1] == 2`, result `WAIT_RESULT_SIGNAL`, resumed in `[unlock - 2, unlock + 25]` ms

---

### Test 6 — `SchedulerLockMutualExclusion`
**Tasks:** 0–3 (4 tasks) &nbsp;|&nbsp; **Scenario:** `SC_MUTUAL_EXCLUSION` &nbsp;|&nbsp; **Class:** `LockTask`

Every task enters a section guarded by `ScopedSchedulerLock` 200 times. Inside the section it increments
`g_Inside` (and counts a violation if it is not 1), does a **non-atomic** read-modify-write of `g_Shared` with a
5000-iteration busy loop between the read and the write, and leaves. Every 8th iteration the task sleeps 1 tick
outside of the section to let the other tasks interleave. A lost update or an overlap shows a missing
exclusion. Task 3 (the last task) acts as verifier.

**Pass condition:** `g_Shared == 4 * 200`, `g_Violations == 0` and `g_Inside == 0`

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `LockApiTask` | 0 | all results and `GetState()` values as specified | Results (previous / new state), `KSTATE_LOCKED`, nesting with the saved state, `ScopedSchedulerLock` |
| 2 | `LockTask` (`SC_NO_SWITCH`) | 0–1 | spinner counter frozen under the lock, ticks advance, spinner resumes | No task switch while locked |
| 3 | `LockTask` (`SC_NESTED_GUARDS`) | 0–1 | as test 2, still locked after the inner guard | Nested guards do not release an outer lock |
| 4 | `LockTask` (`SC_SLEEPER_DEFERRED`) | 0–1 | sleeper not run under the lock, resumes `[unlock-2, unlock+25]` ms | Sleep timers keep running, wake-up deferred to the unlock |
| 5 | `LockTask` (`SC_WAKE_DEFERRED`) | 0–1 | waiter blocked under the lock, `SIGNAL` after the unlock | A wake-up under the lock is deferred to the unlock |
| 6 | `LockTask` (`SC_MUTUAL_EXCLUSION`) | 0–3 | `g_Shared == 800`, no violations | Mutual exclusion of a guarded section |
