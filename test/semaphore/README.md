# Semaphore Test Suite

Test suite for `stk::sync::SemaphoreT<T>` and its default instantiation `stk::sync::Semaphore`.  
Source: `test/semaphore/test_semaphore.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_semaphore.h>
```

`SemaphoreT<T>` is a counting semaphore that maintains an internal resource counter of
unsigned integer type `T`. `Semaphore` is the default instantiation:

```cpp
typedef SemaphoreT<uint16_t> Semaphore;
```

Unlike `ConditionVariable` or `Event`, it is **stateful**: a `Signal()` posted when
no task is waiting increments the counter so the signal is not lost — a subsequent
`Wait()` will drain it on the fast path without ever blocking.

This implementation uses a **Direct Handover** policy: when tasks are waiting,
`Signal()` passes the resource token directly to the first waiter without touching
the counter. The woken task owns the token upon return from `Wait()` without needing
to decrement `m_count` itself.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// Construction
stk::sync::Semaphore g_Sem;        // initial count = 0 (default)
stk::sync::Semaphore g_Sem(3);     // initial count = 3 (3 permits immediately available)
stk::sync::Semaphore g_Sem(0, 1);  // initial count = 0, max count = 1 (binary semaphore)

// Other counter types
stk::sync::SemaphoreT<uint32_t> g_Credits(70000); // counter wider than 16 bits
stk::sync::SemaphoreT<uint8_t>  g_Small;          // 8-bit counter (does not necessarily shrink the object)
```

### Counter type

`T` must be an unsigned integer type (a signed type is rejected at compile time).
The counter is bounded by `max_count` (default `SemaphoreT<T>::COUNT_MAX`), where `COUNT_MAX`
is the maximum value of `T` minus one:

| Instantiation | Counter type | `COUNT_MAX` |
|---------------|--------------|-------------|
| `SemaphoreT<uint8_t>` | `uint8_t` | `254` (`0xFE`) |
| `SemaphoreT<uint16_t>` = `Semaphore` | `uint16_t` | `65534` (`0xFFFE`) |
| `SemaphoreT<uint32_t>` | `uint32_t` | `4294967294` (`0xFFFFFFFE`) |
| `SemaphoreT<uint64_t>` | `uint64_t` | `18446744073709551614` (`0xFFFFFFFFFFFFFFFE`) |

All counter updates run under the critical section, so every counter type is safe for concurrent
`Signal()` / `Wait()`. `GetCount()` reads the counter directly when `T` is not wider than a pointer
(a single aligned load on all supported architectures) and under the critical section otherwise
(for example `uint64_t` on a 32-bit MCU), so the value can not be torn by a concurrent update.

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Wait` | `bool Wait(Timeout timeout = WAIT_INFINITE)` | Decrements counter if `> 0` (fast path, no blocking). If counter is `0` and `timeout == 0` (`NO_WAIT`), returns `false` immediately. Otherwise blocks until `Signal()` or timeout. Returns `true` if acquired, `false` on timeout **or cancellation** (use `WaitEx()` to tell them apart). ISR-unsafe. |
| `WaitEx` | `EWaitResult WaitEx(Timeout timeout = WAIT_INFINITE)` | Same as `Wait()` but reports the full outcome: `WAIT_RESULT_SIGNAL` (acquired, including the fast path), `WAIT_RESULT_TIMEOUT` (timeout expired, or `NO_WAIT` on an empty semaphore), `WAIT_RESULT_CANCELED` (wait cancelled via `IKernel::CancelTaskWait()` / `stk::SleepCancel()`), `WAIT_RESULT_FAIL` (kernel rejected the wait). ISR-unsafe. |
| `Signal` | `void Signal()` | If tasks are waiting, delivers token directly to the first waiter (Direct Handover — counter is not incremented). If no tasks are waiting, increments `m_count`; asserts if the counter is already at `max_count`. ISR-safe. |
| `TrySignal` | `bool TrySignal()` | Like `Signal()`, but returns `false` (counter unchanged) instead of asserting when the counter is at `max_count` and no task is waiting. With a waiter present it still succeeds by direct hand-off, even at `max_count`. ISR-safe. |
| `TrySignal` | `bool TrySignal(T ceiling)` | Like `TrySignal()`, but the counter is additionally limited by a per-call `ceiling`: the effective limit is the smaller of `ceiling` and `max_count`. Returns `false` (counter unchanged) if the counter is already at or above the effective limit and no task is waiting. With a waiter present it always succeeds by direct hand-off, whatever the ceiling. The check and the increment are atomic. Intended for APIs with a per-call ceiling (for example ThreadX `tx_semaphore_ceiling_put`). ISR-safe. |
| `GetCount` | `T GetCount() const` | Returns the current counter value (advisory snapshot). ISR-safe; a counter wider than a pointer is read under the critical section. |
| `~SemaphoreT` | (destructor) | Asserts `m_wait_list.IsEmpty()` in debug builds. Destroying a semaphore with active waiters is a logic error. |

**Fast path vs slow path:**

```
Wait() — m_count > 0  →  decrement m_count, return true immediately (no blocking)
Wait() — m_count == 0, timeout == 0  →  return false immediately (NO_WAIT)
Wait() — m_count == 0, timeout > 0  →  block until Signal() or timeout

Signal() — waiters present  →  wake first waiter directly (m_count unchanged)
Signal() — no waiters       →  ++m_count (signal remembered for future Wait())

TrySignal(ceiling) — waiters present  →  wake first waiter directly (ceiling ignored)
TrySignal(ceiling) — no waiters       →  ++m_count only if m_count < min(ceiling, max_count)
```

**Key invariants:**

- Every `Signal()` produces exactly one successful `Wait()` — signals are never lost.
- The counter value plus the number of tasks currently blocked equals the total
  number of signals posted minus the total number of successful acquisitions.
- Direct Handover means `m_count` is never incremented when a waiter is present;
  the waking task does not decrement it either — ownership is transferred implicitly.
- The counter never exceeds `max_count` and never wraps, including at the top of the range of `T`.
- `TrySignal(ceiling)` never raises the counter above `ceiling` or above `max_count`, and a rejected
  call leaves the counter unchanged; a waiting task always receives the token regardless of `ceiling`.
- A cancelled wait returns `WAIT_RESULT_CANCELED` (`Wait()` returns `false`) immediately,
  regardless of the deadline. The cancelled task is removed from the wait list and receives no
  token, so the counter is unchanged and the next `Signal()` is delivered to the next waiter or
  remembered in the counter, never swallowed by the cancelled task.
- If a `Signal()` hand-over already woke the waiter before the cancel arrives, the cancel is a
  no-op: the waiter owns the token and gets `WAIT_RESULT_SIGNAL`; the token is neither lost nor
  duplicated.
- Destroying a semaphore while tasks are waiting is a logic error; an assertion
  fires in debug builds.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_SEM_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_SEM_TEST_TIMEOUT` | `1000` ticks | Blocking timeout for `Wait()` calls that must succeed |
| `_STK_SEM_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_SEM_TEST_LONG_SLEEP` | `100` ticks | Sleep used by the verifier task to wait for workers |
| `_STK_SEM_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

`g_TestSemaphore` (a `sync::Semaphore`, i.e. `SemaphoreT<uint16_t>`) is **reconstructed in-place** via
placement-new inside `ResetTestState()` before each test. `ResetTestState()` accepts `initial_count` and
`max_count` parameters forwarded to the semaphore constructor, allowing each test to
start with the exact counter value and bound it requires.

Tests that need a different counter type or an extra semaphore (tests 10, 12, 13 and 14) construct local
semaphore objects on the stack of task 0 instead of using `g_TestSemaphore`.

**Compile-time checks:** `static_assert`s at the top of the test namespace verify `COUNT_MAX` of
`SemaphoreT<uint8_t>` (`254`), `SemaphoreT<uint16_t>` (`0xFFFE`), `SemaphoreT<uint32_t>`
(`0xFFFFFFFE`) and `SemaphoreT<uint64_t>` (`0xFFFFFFFFFFFFFFFE`), and that the default `Semaphore` keeps a
16-bit counter.

Cancellation tests (15–26) use additional shared state, all cleared by `ResetTestState()`:

| Variable | Purpose |
|----------|---------|
| `g_Victim` / `g_VictimTid` | Task (as `ITask *` and as `TId`) whose wait is cancelled; published by task 1 just before it blocks |
| `g_VictimResult[2]` | `EWaitResult` of the wait of task 1 / task 2 (`-100` = not set); for `CancelWait_BoolWait` task 1 stores `1` / `0` = `Wait()` returned `true` / `false` |
| `g_VictimElapsed[2]` | Milliseconds spent in the wait by task 1 / task 2 |
| `g_VictimSleepElapsed` | Milliseconds task 1 spent in `Sleep(60)` after its wait ended (`NoStaleWaiter` only) |
| `g_CancelParam` | Scenario selector, set by `main()` before each `CancelWait_*` `RunTest()` call |

`CancelVictim(use_sleep_cancel)` cancels the victim's wait either via
`g_Kernel.CancelTaskWait(g_Victim)` or via `stk::SleepCancel(g_VictimTid)`. The scenario is
selected by the global `g_CancelParam`; adding `CW_VIA_SLEEPCANCEL` (`100`) switches the cancel
path to `stk::SleepCancel()`.

The scenario is a global rather than a constructor argument on purpose: `RunTest()` declares tasks
0–2 as `static` on non-M0 platforms, and a static local is constructed only once per template
instantiation, so a constructor argument would silently keep the value of the first `CancelWait_*`
case for every later case. `CancelWaitTask` therefore reads `g_CancelParam` at the start of `Run()`.

Tasks 0–2 are always added by `RunTest()`; tasks 3–4 are added unless the test is `TimeoutWait`,
`ZeroTimeout`, `SignalBeforeWait`, `BoundedBuffer`, `TrySignalSaturation`, `AlreadySignaledOnInit`,
`TrySignalCeiling`, `TrySignalCeilingHandoff`, `WideCounter32`, `NarrowCounter8`, `WideCounter64`
or a `CancelWait_*` test.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link all the
distinct task class templates simultaneously. Tests 1–26 are skipped on M0 and only
`StressTest` (test 27) runs, under `#ifndef __ARM_ARCH_6M__`.

`StressTest` runs on M0 because it uses a single task class template (`StressTestTask`)
instantiated for all five task slots, fitting within the available memory. Tests 1–26
introduce distinct task class templates (tests 15–26 share one, `CancelWaitTask`), multiplying the static memory footprint
beyond what M0 can accommodate.

| Platform | `_STK_SEM_STACK_SIZE` |
|----------|-----------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `BasicSignalWait`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Param:** `iterations = 100` &nbsp;|&nbsp; **Initial count:** `0`

Task 0 is the producer; tasks 1–4 are consumers. The producer fires
`iterations × 4 = 400` signals total, pacing each with `Delay(1)`. Each of the four
consumers loops `Wait()` for `iterations = 100` cycles, incrementing `g_SharedCounter`
on each acquisition. Verifies that every signal produces exactly one successful `Wait()`
with no lost or duplicate deliveries.

**Pass condition:** `counter == 400`

---

### Test 2 — `InitialCount`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Initial count:** `4` (`_STK_SEM_TEST_TASKS_MAX - 1`)

The semaphore is pre-loaded with 4 permits. Tasks 1–4 each call
`Wait(_STK_SEM_TEST_SHORT_SLEEP)` immediately; all four must succeed on the fast path
(counter drains from 4 to 0). Task 0 sleeps briefly to let the drain complete, then
calls `Wait(_STK_SEM_TEST_SHORT_SLEEP)` itself. With the counter at 0 and no `Signal()`
forthcoming, task 0's `Wait()` must time out and return `false`. Verifies the
`initial_count` constructor parameter and the transition from fast-path to blocking.

**Pass condition:** `counter == 4` (all four consumers fast-pathed) and `acquired == false` (task 0 timed out)

---

### Test 3 — `TimeoutWait`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Initial count:** `0`

Task 1 calls `Wait(50)` after `_STK_SEM_TEST_SHORT_SLEEP`; the semaphore stays at zero
so it must time out and return `false` with elapsed time in `[45, 60]` ms. Task 0
withholds `Signal()` until tick 200. Task 2 sleeps until tick 210 then calls
`Wait(100)`, which must succeed on the now-signaled semaphore. Task 2 is the verifier.

**Pass condition:** `counter == 2`
(1 = timeout fired correctly within bounds; 2 = `Wait()` succeeded after `Signal()`)

---

### Test 4 — `ZeroTimeout`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Initial count:** `0`

Verifies `Wait(NO_WAIT)` (timeout == 0) non-blocking behavior on both paths. Task 1
calls `Wait(NO_WAIT)` on a zero-count semaphore; must return `false` immediately
(elapsed < `_STK_SEM_TEST_SHORT_SLEEP`). Task 2 calls `Signal()` to add one permit,
then `Wait(NO_WAIT)`; must return `true` immediately as the fast path takes effect.
Both checks contribute one increment each to `g_SharedCounter`.

**Pass condition:** `counter == 2`
(1 = `Wait(NO_WAIT)` on empty returned `false` immediately; 2 = `Wait(NO_WAIT)` on pre-signaled returned `true` immediately)

---

### Test 5 — `SignalBeforeWait`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Param:** `iterations = 200` &nbsp;|&nbsp; **Initial count:** `0`

Verifies the semaphore's stateful "signal remembered" property. Task 0 calls
`Signal()` 200 times consecutively before any consumer reaches `Wait()`, then reads
`GetCount()` to confirm the counter accumulated to 200. Task 1 then sleeps briefly to
ensure all signals are posted, and drains all 200 permits by calling `Wait()` 200
times — every call must fast-path (no blocking). Verifies that signals posted with no
waiters are never discarded.

**Pass condition:** `count_after_signals == 200` and `counter == 200`

---

### Test 6 — `FIFOOrder`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Initial count:** `0`

Verifies that blocked tasks are woken in FIFO arrival order. Tasks 1–4 stagger their
entry into `Wait()` by sleeping `SHORT_SLEEP × m_task_id` ticks, ensuring they queue
in ascending task-id order (1, 2, 3, 4). Task 0 waits 50 ticks for all consumers to
block, then calls `Signal()` four times with `Delay(1)` between each. As each
consumer wakes it records its `m_task_id` into `g_AcquisitionOrder[g_OrderIndex++]`.
The verifier checks the array is exactly `[1, 2, 3, 4]`.

**Pass condition:** `g_AcquisitionOrder == [1, 2, 3, 4]`

---

### Test 7 — `BoundedBuffer`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Param:** `iterations = 200` &nbsp;|&nbsp; **Initial count:** `0`

Models the classic single-producer / single-consumer bounded-buffer pattern. Task 0
(producer) calls `Delay(1)` then `Signal()` in a 200-iteration loop, simulating work
before each item is made available. Task 1 (consumer) calls `Wait(_STK_SEM_TEST_TIMEOUT)`
200 times, incrementing `g_SharedCounter` on each successful acquisition. Task 1 is
also the verifier. Verifies end-to-end gate semantics: no item is consumed before it
is produced.

**Pass condition:** `counter == 200`

---

### Test 8 — `TrySignalSaturation`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Initial count:** `0` &nbsp;|&nbsp; **Max count:** `1`

Verifies `TrySignal()` at the `max_count` boundary. Task 0 calls `TrySignal()` (0 → 1, succeeds), then
`TrySignal()` again: the counter is saturated and nobody waits, so it must return `false` and leave the
counter at 1. It drains the permit with `Wait(NO_WAIT)`, sleeps so that task 1 blocks in `Wait()`, then
calls `TrySignal()` once more: with a waiter present it must succeed by direct hand-off, without
incrementing the counter. Task 1 records that it acquired the permit.

**Pass condition:** first `TrySignal()` true, second false, `count == 1`, drain true, hand-off `TrySignal()`
true, `count == 0` after hand-off, and task 1 acquired the permit (`counter == 1`)

---

### Test 9 — `AlreadySignaledOnInit`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Initial count:** `1` &nbsp;|&nbsp; **Max count:** `1`

Verifies a semaphore constructed saturated (`Semaphore(1, 1)`) without any `Signal()` call. Task 0 checks that
`GetCount()` is already 1, that `TrySignal()` reports failure (saturated, no waiters) and leaves the counter
unchanged, that `Wait(NO_WAIT)` drains the pre-loaded permit (true), and that a second `Wait(NO_WAIT)` fails.

**Pass condition:** `count_at_init == 1`, `TrySignal()` false, count still 1, first `Wait(NO_WAIT)` true,
second false

---

### Test 10 — `TrySignalCeiling`
**Tasks:** 0–2 only (only task 0 does work) &nbsp;|&nbsp; **Initial count:** `0` &nbsp;|&nbsp; **Max count:** `10`

Verifies `TrySignal(ceiling)` without waiters. Task 0 runs five sub-checks, using `g_TestSemaphore` for (1), (4)
and (5) and local semaphores for (2) and (3):

1. **Ceiling below `max_count`:** five calls of `TrySignal(3)` on the `(0, 10)` semaphore; exactly 3 must
   succeed and the counter must stop at 3.
2. **Ceiling above `max_count`:** five calls of `TrySignal(100)` on a local `Semaphore(0, 2)`; the ceiling is
   clamped to `max_count`, so exactly 2 must succeed and the counter must stop at 2.
3. **Zero ceiling:** `TrySignal(0)` on an empty local `Semaphore(0, 2)` must return `false` and leave the counter at 0.
4. **Ceiling below the current count:** with the counter at 3, `TrySignal(2)` must return `false` and leave the
   counter at 3.
5. **Plain `TrySignal()` afterwards:** still uses the per-object maximum, so it must succeed and raise the counter
   to 4.

**Pass condition:** accepted / count are `3 / 3` for (1), `2 / 2` for (2), `false / 0` for (3), `false / 3` for (4)
and `true / 4` for (5)

---

### Test 11 — `TrySignalCeilingHandoff`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Initial count:** `0` &nbsp;|&nbsp; **Max count:** `10`

Verifies `TrySignal(ceiling)` with a waiting task. Task 1 blocks in `Wait(_STK_SEM_TEST_TIMEOUT)` after
`SHORT_SLEEP / 2`. Task 0 sleeps `SHORT_SLEEP`, then calls `TrySignal(0)`: the ceiling is already exceeded, but a
waiter is present, so the call must still succeed by direct hand-off and leave the counter at 0. After task 1 has
recorded the acquisition, task 0 calls `TrySignal(0)` again with nobody waiting; it must now return `false`.

**Pass condition:** hand-off `TrySignal(0)` true, `count == 0`, task 1 acquired the token (`counter == 1`),
second `TrySignal(0)` false, `count == 0`

---

### Test 12 — `WideCounter32`
**Tasks:** 0–2 only (only task 0 does work) &nbsp;|&nbsp; **Counter type:** `uint32_t`

Verifies `SemaphoreT<uint32_t>` beyond the 16-bit range, using local semaphores in task 0:

1. **Large initial count:** `SemaphoreT<uint32_t>(70000)` must report `GetCount() == 70000` (no truncation), and one
   `Wait(NO_WAIT)` must succeed and leave 69999.
2. **No wrap at 65535:** a semaphore constructed with `0xFFFF` accepts `TrySignal()` and then reports `0x10000`.
3. **Top of the range:** a semaphore constructed as `(COUNT_MAX, COUNT_MAX)` rejects `TrySignal()` and
   `TrySignal(COUNT_MAX)`; one `Wait(NO_WAIT)` must succeed and leave `COUNT_MAX - 1`; one `TrySignal()` must
   succeed and restore `COUNT_MAX`.

**Pass condition:** all of the values above, i.e. `70000` / `true` / `69999`, `true` / `65536`, and
`false`, `false`, `true`, `COUNT_MAX - 1`, `true`, `COUNT_MAX` for the saturated semaphore

---

### Test 13 — `NarrowCounter8`
**Tasks:** 0–2 only (only task 0 does work) &nbsp;|&nbsp; **Counter type:** `uint8_t`

Verifies `SemaphoreT<uint8_t>` at its counter limit, using a local default-constructed semaphore in task 0
(`max_count == COUNT_MAX == 254`). Task 0 calls `TrySignal()` 300 times; exactly 254 must succeed and
`GetCount()` must read 254 (no wrap). It then drains the semaphore with `Wait(NO_WAIT)` until it fails; exactly 254
waits must succeed and `GetCount()` must read 0. This exercises the integer-promotion casts of the 8-bit counter.

**Pass condition:** `accepted == 254`, `count == 254`, `drained == 254`, `count == 0`

---

### Test 14 — `WideCounter64`
**Tasks:** 0–2 only (only task 0 does work) &nbsp;|&nbsp; **Counter type:** `uint64_t`

Verifies `SemaphoreT<uint64_t>` beyond the 32-bit range, using local semaphores in task 0. Counts are printed as two
32-bit halves because a 64-bit `printf` conversion is not always available on embedded targets.

1. **Large initial count:** `SemaphoreT<uint64_t>(2^32 + 5)` must report `GetCount() == 2^32 + 5` (no truncation),
   and one `Wait(NO_WAIT)` must succeed and leave `2^32 + 4`.
2. **No wrap at 2^32:** a semaphore constructed with `2^32 - 1` accepts `TrySignal()` and then reports `2^32`.
3. **Ceiling above 32 bits:** on a semaphore holding `2^32 - 1`, `TrySignal(2^32)` must succeed (count `2^32`) and a
   second `TrySignal(2^32)` must fail.
4. **Top of the range:** a semaphore constructed as `(COUNT_MAX, COUNT_MAX)` rejects `TrySignal()` and
   `TrySignal(COUNT_MAX)`; one `Wait(NO_WAIT)` must succeed and leave `COUNT_MAX - 1`; one `TrySignal()` must
   succeed and restore `COUNT_MAX`.

On a 32-bit target every `GetCount()` in this test is taken under the critical section (the counter is wider than a
pointer), so it also exercises the torn-read protection.

**Pass condition:** all of the values above, i.e. `2^32 + 5` / `true` / `2^32 + 4`, `true` / `2^32`,
`true` / `false` / `2^32`, and `false`, `false`, `true`, `COUNT_MAX - 1`, `true`, `COUNT_MAX` for the saturated semaphore

---

### Tests 15–26 — `CancelWait_*`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Task class:** `CancelWaitTask` &nbsp;|&nbsp; **Initial count:** `0`

Verify that a task blocked on the semaphore can be cancelled. Task 0 is the canceller and checker, task 1 is
the victim blocked in `WaitEx()` from ~10 ms, and task 2 is a second waiter used only by the two-waiters
scenario. Task 0 cancels at ~60 ms. Because the semaphore starts at 0, a later `Signal()` either hands its
token to a waiter (counter stays 0, Direct Handover) or is remembered (counter + 1): which of the two
happens shows whether a cancelled task was correctly removed from the wait list. Tests 15–21 run through
`IKernel::CancelTaskWait()`; the `SleepCancel` variants (tests 22–26) repeat a subset through
`stk::SleepCancel()`.

If a cancel fails, task 0 signals the semaphore three times so any still-blocked waiter is released and the
test fails instead of hanging the kernel.

#### Test 15 — `CancelWait_Infinite`
Task 1 calls `WaitEx(WAIT_INFINITE)` and is cancelled by task 0.

**Pass condition:** result is `WAIT_RESULT_CANCELED`, elapsed time is in `[35, 75]` ms, and `GetCount() == 0`
(the cancel does not create a permit).

#### Test 16 — `CancelWait_NotTimeout`
Task 1 waits with a 300 ms timeout and is cancelled long before it expires.

**Pass condition:** result is `WAIT_RESULT_CANCELED` (not `WAIT_RESULT_TIMEOUT`), elapsed time is below
150 ms, and `GetCount() == 0`.

#### Test 17 — `CancelWait_NoStaleWaiter`
After the cancel, task 0 calls `Signal()` twice with nobody waiting. A stale wait entry would swallow the
first token (hand it to a task that is not waiting). Task 1 meanwhile sleeps 60 ms; a ghost wait entry would
wake it early.

**Pass condition:** result is `WAIT_RESULT_CANCELED`; `GetCount() == 2`; two `Wait(NO_WAIT)` calls succeed and
a third fails; task 1 slept at least 55 ms.

#### Test 18 — `CancelWait_TwoWaiters`
Tasks 1 and 2 both block on the semaphore; only task 1 is cancelled. Task 0 then calls `Signal()`.

**Pass condition:** task 1 gets `WAIT_RESULT_CANCELED`; task 2 gets `WAIT_RESULT_SIGNAL`; `GetCount() == 0`
(the token was handed over, not remembered); a following `Wait(NO_WAIT)` fails (nothing left over).

#### Test 19 — `CancelWait_SignalWins`
Task 0 calls `Signal()` and `CancelTaskWait()` back to back with no scheduling point between them. The waiter
already holds the token when the cancel arrives, so the cancel must be a no-op.

**Pass condition:** task 1 gets `WAIT_RESULT_SIGNAL`; `GetCount() == 0`; a following `Wait(NO_WAIT)` fails
(the token is neither lost nor duplicated).

#### Test 20 — `CancelWait_BoolWait`
Task 1 uses the boolean `Wait(WAIT_INFINITE)` instead of `WaitEx()`. A cancel is reported as `false`; since an
infinite wait can not time out, `false` after ~50 ms identifies the cancel. Task 0 then signals once with nobody
waiting.

**Pass condition:** `Wait()` returned `false` with elapsed time in `[35, 75]` ms; `GetCount() == 1` after the
`Signal()`; `Wait(NO_WAIT)` succeeds.

#### Test 21 — `CancelWait_RetryAfter`
After receiving `WAIT_RESULT_CANCELED`, task 1 immediately calls `WaitEx(WAIT_INFINITE)` again on the same
semaphore. Verifies that a cancelled task can re-enter the wait list cleanly.

**Pass condition:** first wait is `WAIT_RESULT_CANCELED`; task 1 is blocked again before the `Signal()`; its
second wait returns `WAIT_RESULT_SIGNAL`; `GetCount() == 0`.

#### Tests 22–26 — `CancelWait_SleepCancel*`
Same scenarios as tests 15, 17, 18, 19 and 20, but cancelled through `stk::SleepCancel(tid)`:

| # | Test | Repeats |
|---|------|---------|
| 22 | `CancelWait_SleepCancel` | Test 15 (`Infinite`) |
| 23 | `CancelWait_SleepCancelNoStale` | Test 17 (`NoStaleWaiter`) |
| 24 | `CancelWait_SleepCancelTwo` | Test 18 (`TwoWaiters`) |
| 25 | `CancelWait_SleepCancelSignal` | Test 19 (`SignalWins`) |
| 26 | `CancelWait_SleepCancelBool` | Test 20 (`BoolWait`) |

**Pass condition:** same as the corresponding test above.

---

### Test 27 — `StressTest`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 400` &nbsp;|&nbsp; **Initial count:** `0`

Task 0 is the sole producer, firing 400 signals with a `Delay(1)` every 10 iterations
to allow consumers to keep up. Tasks 1–4 are consumers, each draining
`iterations / 4 = 100` signals via `Wait(_STK_SEM_TEST_TIMEOUT)`. After all consumers
finish, the verifier (task 4) reads `GetCount()` to capture any signals that were
posted but not yet consumed. The invariant `consumed + remaining == iterations` must
hold exactly, proving no signal was lost or double-counted under contention.

**Pass condition:** `g_SharedCounter + g_TestSemaphore.GetCount() == 400`

---

## Summary Table

| # | Test | Tasks | Initial count | Pass condition | What it verifies |
|---|------|-------|---------------|----------------|------------------|
| 1 | `BasicSignalWaitTask` | 0–4 | `0` | `counter == 400` | Every `Signal()` produces exactly one successful `Wait()`; no signals lost or duplicated |
| 2 | `InitialCountTask` | 0–4 | `4` | `counter == 4`, `acquired == false` | Pre-loaded permits drain on the fast path; subsequent `Wait()` blocks when count reaches zero |
| 3 | `TimeoutWaitTask` | 0–2 | `0` | `counter == 2` | `Wait()` returns `false` within `[45, 60]` ms when no signal arrives; semaphore usable after timeout |
| 4 | `ZeroTimeoutTask` | 0–2 | `0` | `counter == 2` | `Wait(NO_WAIT)` returns `false` on empty and `true` on pre-signaled, both immediately without blocking |
| 5 | `SignalBeforeWaitTask` | 0–2 | `0` | `count_after_signals == 200`, `counter == 200` | Signals posted with no waiters are remembered in the counter; subsequent `Wait()` drains them on the fast path |
| 6 | `FIFOOrderTask` | 0–4 | `0` | order `[1,2,3,4]` | Blocked tasks are woken in FIFO arrival order |
| 7 | `BoundedBufferTask` | 0–2 | `0` | `counter == 200` | Classic single-producer / single-consumer gate: no item consumed before it is produced |
| 8 | `TrySignalSaturationTask` | 0–2 | `0` (max `1`) | `TrySignal()` true, false, hand-off true; counts 1 / 0; waiter acquired | `TrySignal()` rejects at `max_count` without a waiter and hands off to a waiter even at `max_count` |
| 9 | `AlreadySignaledOnInitTask` | 0–2 | `1` (max `1`) | `count == 1`, `TrySignal()` false, first `Wait(NO_WAIT)` true, second false | A semaphore constructed saturated reports its count, rejects `TrySignal()` and drains normally |
| 10 | `TrySignalCeilingTask` | 0–2 | `0` (max `10`) | accepted / count `3/3`, `2/2`, `false/0`, `false/3`, `true/4` | `TrySignal(ceiling)` limits the counter below `max_count`, is clamped to `max_count`, rejects a zero or too-low ceiling and leaves plain `TrySignal()` unaffected |
| 11 | `TrySignalCeilingHandoffTask` | 0–2 | `0` (max `10`) | hand-off `TrySignal(0)` true, `count == 0`, waiter acquired; without waiter false | A waiter always receives the token by direct hand-off regardless of the ceiling; no waiter and an exceeded ceiling is rejected |
| 12 | `WideCounter32Task` | 0–2 | n/a (local, `uint32_t`) | `70000 → 69999`, `0xFFFF → 0x10000`, saturated `COUNT_MAX` rejects, drops to `COUNT_MAX - 1`, restores | `SemaphoreT<uint32_t>` keeps values above 16 bits, does not wrap at 65535 and handles the top of the range |
| 13 | `NarrowCounter8Task` | 0–2 | n/a (local, `uint8_t`) | `accepted == 254`, `count == 254`, `drained == 254`, `count == 0` | `SemaphoreT<uint8_t>` accepts exactly `COUNT_MAX` signals, rejects the next without wrapping and drains correctly |
| 14 | `WideCounter64Task` | 0–2 | n/a (local, `uint64_t`) | `2^32 + 5 → 2^32 + 4`, `2^32 - 1 → 2^32`, ceiling `2^32` accepted once then rejected, saturated `COUNT_MAX` rejects, drops to `COUNT_MAX - 1`, restores | `SemaphoreT<uint64_t>` keeps values above 32 bits, does not wrap at 2^32, honours a ceiling above 32 bits and handles the top of the range |
| 15 | `CancelWait_Infinite` | 0–2 | `0` | `WAIT_RESULT_CANCELED`, elapsed `[35, 75]` ms, `count == 0` | Cancel ends an infinite wait with `WAIT_RESULT_CANCELED` and creates no permit |
| 16 | `CancelWait_NotTimeout` | 0–2 | `0` | `WAIT_RESULT_CANCELED`, elapsed `< 150` ms | A cancel is not reported as a timeout and ends a timed wait early |
| 17 | `CancelWait_NoStaleWaiter` | 0–2 | `0` | `count == 2` after two `Signal()`, victim sleep `>= 55` ms | Cancel leaves no wait entry that would swallow a token or wake the task early |
| 18 | `CancelWait_TwoWaiters` | 0–2 | `0` | first `CANCELED`, second `SIGNAL`, `count == 0`, no leftover | Cancelling one waiter leaves the other intact; the next token is handed to it |
| 19 | `CancelWait_SignalWins` | 0–2 | `0` | victim `SIGNAL`, `count == 0`, no leftover | A `Signal()` hand-over that arrived first wins over a late cancel; token neither lost nor duplicated |
| 20 | `CancelWait_BoolWait` | 0–2 | `0` | `Wait()` false after `[35, 75]` ms, `count == 1` after `Signal()` | Boolean `Wait()` collapses a cancel into `false`; no stale waiter afterwards |
| 21 | `CancelWait_RetryAfter` | 0–2 | `0` | first `CANCELED`, second `SIGNAL`, `count == 0` | A cancelled task can wait on the semaphore again and receives the next token |
| 22 | `CancelWait_SleepCancel` | 0–2 | `0` | as test 15 | Test 15 via `stk::SleepCancel()` |
| 23 | `CancelWait_SleepCancelNoStale` | 0–2 | `0` | as test 17 | Test 17 via `stk::SleepCancel()` |
| 24 | `CancelWait_SleepCancelTwo` | 0–2 | `0` | as test 18 | Test 18 via `stk::SleepCancel()` |
| 25 | `CancelWait_SleepCancelSignal` | 0–2 | `0` | as test 19 | Test 19 via `stk::SleepCancel()` |
| 26 | `CancelWait_SleepCancelBool` | 0–2 | `0` | as test 20 | Test 20 via `stk::SleepCancel()` |
| 27 | `StressTestTask` | 0–4 | `0` | `consumed + remaining == 400` | No signal lost or double-counted under full five-task contention; runs on all platforms including M0 |
