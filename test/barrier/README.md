# Barrier Test Suite

Test suite for `stk::sync::Barrier`.
Source: `test/barrier/test_barrier.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_barrier.h>
```

`Barrier` is a **cyclic rendezvous point** for a fixed-size group of tasks: each task calls
`Wait()` and blocks until the last member of the group also calls `Wait()`. Once the last task
arrives, every waiting task is released simultaneously and the barrier automatically resets
(generation counter incremented, count restored to the threshold), ready to be reused for the
next round.

Internally built on top of `Mutex` and `ConditionVariable`. A generation counter guards the
internal wait loop against spurious wakeups and distinguishes successive rounds, so a barrier
tripped in round *N* cannot be re-tripped by tasks still arriving late for round *N-1*.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// Construction
stk::sync::Barrier g_Barrier(5U);   // 5 tasks must call Wait() before any is released
```

| Method | Signature | Description |
|--------|-----------|--------------|
| `Wait` | `bool Wait()` | Blocks the calling task until `count` tasks (as passed to the constructor) have called `Wait()`. Returns `true` for the one task that triggered the release (i.e. the last to arrive), `false` for every other task. ISR-unsafe. |
| `GetThreshold` | `uint32_t GetThreshold() const` | Returns the number of tasks required to trip the barrier. ISR-safe. |
| `~Barrier` | (destructor) | The underlying `Mutex`/`ConditionVariable` members assert `IsEmpty()` on their own wait lists in debug builds if destroyed with tasks still waiting. |

**Round lifecycle in `Wait()`:**

```
--m_count > 0          →  block on m_cond until m_generation changes, then return false
--m_count == 0         →  ++m_generation, m_count = m_threshold, NotifyAll(), return true
```

**Key invariants:**

- No task returns from `Wait()` until exactly `count` tasks (as configured at construction) have
  called it for the current round.
- Exactly one `Wait()` call per round returns `true` — the call that observed the count reaching
  zero and performed the release.
- The barrier is cyclic: once a round completes, `m_count` is restored to `m_threshold` and
  `m_generation` is incremented, so the same `Barrier` instance can be waited on again for
  subsequent rounds without reconstruction.
- A task blocked in `Wait()` is only ever woken by the completion of *its own* generation; a
  wakeup intended for a different (later) generation cannot prematurely release it.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_BARRIER_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_BARRIER_TEST_TIMEOUT` | `1000` ticks | Reserved timeout constant for consistency with other suites (Barrier has no timed wait) |
| `_STK_BARRIER_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing / stagger arrivals |
| `_STK_BARRIER_TEST_LONG_SLEEP` | `100` ticks | Sleep used by a deliberately late arriver so a blocking task's wait time can be measured |
| `_STK_BARRIER_STRESS_ITERATIONS` | `100` rounds | Number of consecutive barrier rounds run by `StressTest` |
| `_STK_BARRIER_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

`g_TestBarrier5`, `g_TestBarrier3`, and `g_TestBarrier2` are plain statics constructed once with
fixed thresholds (`5`, `3`, `2`) matching the task-group sizes used by the different tests. Like
`g_TestMutex` in the mutex suite, none are reconstructed between tests — a `Barrier` naturally
returns to its reset state (`m_count == m_threshold`) once every participating task has completed
its final round, so reuse across tests is safe. `ResetTestState()` resets only the counters,
arrays, and flags used for verification.

`g_CounterMtx` is a small helper `Mutex` used purely for test bookkeeping (protecting shared
counters/arrays declared by the test suite itself). It is **not** the primitive under test —
`Barrier` provides synchronization, not mutual exclusion, for arbitrary shared state.

Tests 1, 2, 3, 5, 7, and 8 add all five tasks against `g_TestBarrier5`. Test 4
(`BlocksUntilLastArrives`) adds only tasks 0–1 against `g_TestBarrier2`. Test 6
(`PartialGroupThreshold`) adds only tasks 0–2 against `g_TestBarrier3`.

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link seven distinct task
class templates simultaneously. Tests 1–7 are skipped on M0 and only `StressTest` (test 8) runs,
under `#ifndef __ARM_ARCH_6M__`.

`StressTest` runs on M0 because it uses a single task class template (`StressTestTask`)
instantiated for all five task slots, fitting within the available memory.

| Platform | `_STK_BARRIER_STACK_SIZE` |
|----------|----------------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `BasicRendezvous`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Barrier:** `g_TestBarrier5` (threshold 5)

Each task locks `g_CounterMtx` and increments `g_ArrivedCount`, then calls `Wait()` on the shared
barrier. Immediately after `Wait()` returns, the task records the current value of
`g_ArrivedCount` into `g_ArrivalSnapshot[task_id]`. Since a task can only resume from `Wait()`
once all five tasks have already incremented the counter and entered `Wait()`, every recorded
snapshot must equal exactly `5` — if the barrier released any task early, that task's snapshot
would be less than `5`. Task 0 uses a `g_InstancesDone` completion barrier before verifying.

**Pass condition:** `g_ArrivalSnapshot[i] == 5` for all `i` in `0..4`

---

### Test 2 — `LastReturnsTrue`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Barrier:** `g_TestBarrier5` (threshold 5)

All five tasks call `Wait()` once. Whichever task observes the internal count reaching zero
increments `g_TrueCount` (under `g_CounterMtx`). Verifies the `Wait()` return-value contract:
exactly one of the five calls — the one that actually performs the release — must return `true`.
Task 0 uses a completion barrier then checks the tally.

**Pass condition:** `g_TrueCount == 1`

---

### Test 3 — `CyclicReuse`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Barrier:** `g_TestBarrier5` (threshold 5) &nbsp;|&nbsp; **Rounds:** `5`

Each task calls `Wait()` in a loop for 5 rounds, tallying `g_TrueCount` every time its own call
returns `true`. Verifies that the barrier fully resets after each round (count restored,
generation advanced) so that it can be waited on again without reconstruction, and that each of
the 5 rounds produces exactly one `true` release. Task 0 uses a completion barrier then verifies
the accumulated total.

**Pass condition:** `g_TrueCount == 5` (one release per round × 5 rounds)

---

### Test 4 — `BlocksUntilLastArrives`
**Tasks:** 0–1 active (tasks 2–4 present but idle) &nbsp;|&nbsp; **Barrier:** `g_TestBarrier2` (threshold 2)

Task 0 sleeps `_STK_BARRIER_TEST_LONG_SLEEP` ticks before calling `Wait()`, deliberately arriving
late. Task 1 calls `Wait()` immediately and measures elapsed time until it returns. Since the
barrier cannot trip until task 0 arrives, task 1's `Wait()` must block for roughly the duration of
task 0's sleep — a `Wait()` implementation that returned immediately (e.g. a no-op) would fail
this check. `g_TestResult` is set directly inside task 1's branch, analogous to the mutex suite's
`TryLock`/`TimedLock` timing tests.

**Pass condition:** elapsed time for task 1's `Wait()` ≥ `_STK_BARRIER_TEST_LONG_SLEEP - _STK_BARRIER_TEST_SHORT_SLEEP`

---

### Test 5 — `DataVisibility`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Barrier:** `g_TestBarrier5` (threshold 5)

Each task writes a per-task value into `g_Phase1[task_id]` (phase 1), then calls `Wait()`. After
the barrier releases everyone, each task reads its neighbor's phase-1 value
(`g_Phase1[(task_id + 1) % 5]`) and stores the sum into `g_Phase2Sum[task_id]`. This exercises the
classic barrier use case — a synchronization fence that makes all phase-1 writes visible to every
task before phase 2 begins. Task 0 uses a completion barrier then verifies every neighbor sum.

**Pass condition:** `g_Phase2Sum[i] == (i + 1) + ((i + 1) % 5 + 1)` for all `i` in `0..4`

---

### Test 6 — `PartialGroupThreshold`
**Tasks:** 0–2 active (tasks 3–4 present but idle) &nbsp;|&nbsp; **Barrier:** `g_TestBarrier3` (threshold 3)

Only three of the kernel's five tasks form the barrier group, using a barrier constructed with
`count = 3`. Each of the three tasks calls `Wait()` once; each increments `g_SharedCounter` after
release and, if its own call returned `true`, also increments `g_TrueCount`. Verifies that a
barrier's threshold is independent of the total kernel task pool and behaves correctly for a
smaller, explicitly-sized group. Task 0 uses a completion barrier scoped to the 3 active tasks.

**Pass condition:** `g_SharedCounter == 3` and `g_TrueCount == 1`

---

### Test 7 — `StaggeredArrival`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Barrier:** `g_TestBarrier5` (threshold 5) &nbsp;|&nbsp; **Rounds:** `3`

Each task runs 3 rounds; in every round it sleeps `_STK_BARRIER_TEST_SHORT_SLEEP × task_id` ticks
before calling `Wait()`, so tasks arrive at noticeably different times within each round instead
of all together. After each release, the task increments `g_SharedCounter`. This stresses the
generation-counter logic under uneven arrival timing, checking that no round's release count
leaks into an adjacent generation. Task 4 uses a completion barrier then verifies the total.

**Pass condition:** `g_SharedCounter == 15` (`3 rounds × 5 tasks`)

---

### Test 8 — `StressTest`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 100`

All five tasks run 100 consecutive barrier rounds. Every round: call `Wait()`, then increment
`g_SharedCounter` under `g_CounterMtx`. A `Delay(1)` is inserted every 20th iteration to vary
scheduling pressure across rounds. Task 4 uses a completion barrier then verifies the total,
confirming no round was lost, duplicated, or corrupted, and that no deadlock occurred under
repeated full-group synchronization.

**Pass condition:** `g_SharedCounter == 500` (`100 rounds × 5 tasks`)

---

## Summary Table

| # | Test | Tasks | Barrier | Pass condition | What it verifies |
|---|------|-------|---------|-----------------|-------------------|
| 1 | `BasicRendezvousTask` | 0–4 | `g_TestBarrier5` (5) | all snapshots `== 5` | No task proceeds past `Wait()` until every task has arrived |
| 2 | `LastReturnsTrueTask` | 0–4 | `g_TestBarrier5` (5) | `g_TrueCount == 1` | Exactly one `Wait()` call returns `true` per round |
| 3 | `CyclicReuseTask` | 0–4 | `g_TestBarrier5` (5) | `g_TrueCount == 5` | Barrier resets and can be reused across multiple rounds without reconstruction |
| 4 | `BlocksUntilLastArrivesTask` | 0–1 | `g_TestBarrier2` (2) | elapsed ≥ `LONG_SLEEP - SHORT_SLEEP` | `Wait()` genuinely blocks until the last task arrives, rather than returning immediately |
| 5 | `DataVisibilityTask` | 0–4 | `g_TestBarrier5` (5) | all neighbor sums correct | `Wait()` acts as a fence — pre-barrier writes are visible to all tasks post-barrier |
| 6 | `PartialGroupThresholdTask` | 0–2 | `g_TestBarrier3` (3) | `counter == 3`, `true_count == 1` | Barrier threshold works correctly for a subset of the kernel's task pool |
| 7 | `StaggeredArrivalTask` | 0–4 | `g_TestBarrier5` (5) | `counter == 15` | Generation counter stays correct across rounds despite staggered, uneven arrival timing |
| 8 | `StressTestTask` | 0–4 | `g_TestBarrier5` (5) | `counter == 500` | No corruption or deadlock under 100 consecutive full five-task barrier rounds; runs on all platforms |
