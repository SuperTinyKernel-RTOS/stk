# Yield Test Suite

Test suite for `stk::Yield()`.  
Source: `test/yield/test_yield.cpp`

---

## API Summary

```cpp
#include <stk.h>
```

```cpp
void stk::Yield();   // end the current time slice and let other tasks run
```

The calling task gives up the rest of its time slice and the kernel forces an immediate context switch (it does not wait
for the next tick). What happens to the yielding task depends on the task-switching strategy, selected at
compile time through `TASK_YIELD_API`:

| Variant | Strategy | `TASK_YIELD_API` | Behaviour |
|---------|----------|------------------|-----------|
| Legacy | `SwitchStrategyRR` | `0` | The kernel puts the yielding task to sleep for a tick. The strategy receives `OnTaskSleep()` at yield time and `OnTaskWake()` on the next tick; the task re-enters the runnable list at the tail. |
| Native | `SwitchStrategyRRY` | `1` | The kernel calls `OnTaskYield()`. The task stays runnable and keeps its position in the rotation. The strategy pins its cursor to the yielder so that the following `GetNext()` returns the successor (or the task itself when it is the only runnable task). |

**Native path in the kernel:**

```
OnTaskSwitch() → (critical section) task not sleeping → strategy.OnTaskYield(task)
  returns true   →  STATE_YIELD_PENDING set, ForceContextSwitch(), busy-wait until flag cleared
  returns false  →  legacy path: task is put to sleep (STATE_SLEEP_PENDING)

ForceContextSwitch (SVC) or next tick, whichever comes first:
  clears STATE_YIELD_PENDING, runs UpdateFsmState() → strategy.GetNext()
```

**Key invariants:**

- `Yield()` always returns; the yielding task is scheduled again after the other runnable tasks
  have had their turn (native) or after the current tick boundary (legacy).
- A pending yield is consumed exactly once, by the forced switch or by the tick, whichever comes
  first. A tick landing between `OnTaskYield()` and the forced switch must not cause a second
  rotation step.
- Native: a lone runnable task that yields keeps running (`GetNext()` returns the same task, no
  context switch, no idle period).
- Native: each task advances at most one position per pass of the ring, so tasks that yield after
  every step progress in lockstep.
- Yielding must not disturb the sleep bookkeeping of other tasks.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `STK_YIELD_TEST_NATIVE` | `0` (default) / `1` | Selects the strategy under test: `0` = `SwitchStrategyRR` (legacy), `1` = `SwitchStrategyRRY` (native) |
| `_STK_YIELD_TEST_TASKS_MAX` | `5` | Kernel task capacity (and number of task objects created by `RunTest`) |
| `_STK_YIELD_TEST_SHORT_SLEEP` | `10` ms | Polling interval used by verifier tasks while waiting for the others |
| `_STK_YIELD_TEST_PEER_SLEEP` | `300` ms | Sleep time of the peers in test 4 |
| `_STK_YIELD_TEST_ADD_ITERS` | `50` | Iterations of the task added at run time in test 7 |
| `_STK_YIELD_TEST_FAST_MS` | `50` ms | Upper bound for 100 yields of a lone runnable task in the native build; override with `-D_STK_YIELD_TEST_FAST_MS=...` |
| `_STK_YIELD_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

The suite is built twice from one source file because the strategy is a compile-time property
of the kernel:

```
# legacy yield (default)
SwitchStrategyRR   — kernel puts the yielder to sleep for a tick

# native yield
-DSTK_YIELD_TEST_NATIVE=1
SwitchStrategyRRY  — OnTaskYield() / STATE_YIELD_PENDING
```

Functional tests must pass in both builds, including test 6, which is compiled into both builds and
uses the same pass thresholds in each. In tests 3
and 4 the timing assertions apply to the native build only; the legacy build checks completion
and correct counts.

**Test state:** all shared state lives in static arrays indexed by task id, and every element has
a single writer:

| Variable | Purpose |
|----------|---------|
| `g_Counter[id]` | Per-task progress counter |
| `g_Done[id]` | Per-task completion flag |
| `g_Awake[id]` | Set by a sleeping peer when it wakes up (test 4) |
| `g_Spread[id]` | Per-task maximum observed progress spread (test 6) |
| `g_InOrder[id]` | Per-task count of steps made right after the ring predecessor (test 6) |
| `g_OutOfOrder[id]` | Per-task count of steps made after any other task (test 6) |
| `g_Last` | Id of the task that made the previous step (test 6); written by every task, a rare misclassification is tolerated |
| `g_TaskCount` | Number of tasks that must report completion before a verifier proceeds |

Completion is tracked with per-task flags rather than a shared counter: a shared volatile `++`
can lose an update if a tick preempts it, which would leave the verifier waiting forever.
`ResetTestState(expected_done)` clears all state before each test.

`RunTest<T>(name, tasks_added, param, expected_done)` constructs all five task objects but adds
only the first `tasks_added` to the kernel. `expected_done` defaults to `tasks_added` and is larger
only when a task adds more tasks at run time (test 7).

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link several distinct
task class templates simultaneously. Tests 1–7 are skipped on M0 and only `StressTest` (test 8)
runs; tests 1–7 are compiled under `#ifndef __ARM_ARCH_6M__`. On M0 the task objects are
constructed on the stack of `RunTest` instead of being `static` (`STK_TASK` expands to nothing).

Test 7 additionally owns a statically allocated task (`g_ExtraTask`) which is not defined on M0.

| Platform | `_STK_YIELD_STACK_SIZE` | Tests run |
|----------|-------------------------|-----------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words | 8 only |
| All others, legacy build | `256` words | 1, 2, 3, 4, 5, 6, 7, 8 |
| All others, native build | `256` words | 1, 2, 3, 4, 5, 6, 7, 8 |

The kernel is `KERNEL_DYNAMIC` (plus `KERNEL_TICKLESS` when `STK_TICKLESS_IDLE` is set). Tasks
terminate by returning from `Run()`, which is how `Start()` returns after each test.

---

## Tests

### Test 1 — `BasicYield`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `iterations = 100` &nbsp;|&nbsp; **Builds:** both

Each task increments its own counter and calls `Yield()`, 100 times. Task 0 acts as verifier:
after marking itself done it waits for the other tasks, then checks every counter. Verifies that
`Yield()` always returns and that no iteration is lost or repeated.

**Pass condition:** `g_Counter[i] == 100` for tasks 0–2

---

### Test 2 — `PingPong`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `iterations = 30` &nbsp;|&nbsp; **Builds:** both

The tasks pass a token around a ring (`g_Turn`). A task whose turn has not come spins on
`Yield()`; when it holds the token it increments its counter and the shared total, then hands
the token to the next task. Verifies that `Yield()` really hands the CPU to the other tasks (so
the awaited task gets to run) and that a yielding task neither monopolises the CPU nor drops out
of the rotation. Task 0 acts as verifier.

**Pass condition:** `g_Shared == 90` (`3 tasks × 30`) and `g_Counter[i] == 30` for tasks 0–2

---

### Test 3 — `LoneTask`
**Tasks:** 0 only &nbsp;|&nbsp; **Param:** `iterations = 100` &nbsp;|&nbsp; **Builds:** both (timing: native only)

A single task in the system yields 100 times and measures the elapsed time. With legacy yield
every call costs about one tick (the task sleeps and the kernel idles); with native yield the
strategy returns the same task, no context switch is needed and the loop only pays the cost of the
forced-switch request.

**Pass condition:**
- both builds: `g_Counter[0] == 100`
- native build: and elapsed `< _STK_YIELD_TEST_FAST_MS` (50 ms)

---

### Test 4 — `LoneRunnableWithSleepers`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `iterations = 100` &nbsp;|&nbsp; **Builds:** both (timing: native only)

Task 0 yields 100 times while tasks 1 and 2 are asleep (`Sleep(300)`). In the native build task 0
is the only runnable task, with the others in the strategy's sleep list; it must finish long
before the peers wake. Task 0 records the elapsed time and how many peers were awake when its loop
ended, then waits for the peers to finish.

**Pass condition:**
- both builds: `g_Counter[0] == 100`
- native build: and elapsed `< _STK_YIELD_TEST_FAST_MS` (50 ms) and no peer awake at the end of the loop

---

### Test 5 — `YieldersAndSleeper`
**Tasks:** 0–2 &nbsp;|&nbsp; **Builds:** both

Task 0 sleeps 50 ms, measures the elapsed time and raises `g_Flag`. Tasks 1 and 2 spin on
`Yield()`, counting iterations, until the flag is raised. Verifies that yielding does not disturb
the sleep bookkeeping of another task: the sleeper wakes on time and the yielders make progress
meanwhile.

**Pass condition:** elapsed in `[45, 75]` ms and `g_Counter[1] > 0` and `g_Counter[2] > 0`

---

### Test 6 — `RotationFairness`
**Tasks:** 0–2 &nbsp;|&nbsp; **Param:** `iterations = 200` &nbsp;|&nbsp; **Builds:** both

Each task counts a step and yields. Two scale-free properties of the rotation are checked, instead
of a small absolute bound on the progress spread. A forced switch does not restart the tick, so a
tick may cut the next task's slice short (or advance the cursor again before the first pick has
run) and that task loses its step for the pass. Lost steps are never made up, therefore the
absolute spread between tasks drifts with the number of ticks and cannot be bounded by a small
constant.

- **Ring adherence.** Before each step the task reads `g_Last` (the previous stepper) and publishes
  its own id. If the previous stepper was its ring predecessor (`(id + tasks - 1) % tasks`) the
  step counts as in-order (`g_InOrder[id]`), otherwise as out-of-order (`g_OutOfOrder[id]`). A lost
  step costs a single out-of-order transition, so the ratio stays high. A preemption between
  the read and the write of `g_Last` can misclassify a step; this is rare and only lowers the
  measured ratio slightly.
- **No starvation.** After each step the task computes the spread (`max - min`) of the progress
  counters and records its maximum in `g_Spread[id]`, evaluated only while no task has finished
  (once a task is done the others naturally pull ahead). The spread must never exceed
  `iterations / 4`.

**Workload time.** Every task reads `hw::HiResClock::GetTimeUs()` when it enters `Run()`; task 0,
which acts as verifier, reads it again after `WaitAllDone()` and reports the elapsed time
(from the start of task 0 until all tasks have completed) together with the results. The time is
informational only and is not part of the pass condition. `HiResClock` must be called from a
privileged context; the test runs with `ACCESS_PRIVILEGED`.

Output (task 0):

```
fairness: ring adherence=<A>% (<in_order>/<transitions>, min 80%), max progress spread=<S> (limit <iterations/4>), workload time=<T> us
```

**Pass condition:** `g_Counter[i] == 200` for tasks 0–2, ring adherence `>= 80%`
(`in_order * 100 / (in_order + out_of_order)`) and `max(g_Spread[i]) <= 50` (`iterations / 4`)

---

### Test 7 — `AddTaskThenYield`
**Tasks:** 0–2 added before `Start()`, task 3 (`g_ExtraTask`) added at run time &nbsp;|&nbsp; **Param:** `iterations = 50` &nbsp;|&nbsp; **Builds:** both

All tasks yield in a loop. At iteration 5, task 0 calls `g_Kernel.AddTask(&g_ExtraTask)` and
continues yielding, so the strategy's task list changes under a running rotation, followed by
`Yield()` calls. This exercises the cursor handling of `AddTask()` combined with `Yield()`. The
added task runs the same loop (50 iterations). Task 0 acts as verifier and waits for all four
tasks.

**Pass condition:** `g_Counter[i] == 50` for tasks 0–3

---

### Test 8 — `StressTest`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 400` &nbsp;|&nbsp; **Builds:** both

Each task runs 400 iterations of: increment own counter, `Yield()`, then a busy `Delay(1)` every
10th iteration and a `Sleep(1)` every 25th. The mix puts ticks at arbitrary points of the yield
sequence, including between the strategy's `OnTaskYield()` and the forced switch, and mixes
runnable and sleeping tasks in the rotation. Task 4 (the last task) acts as verifier and waits for
the others.

**Pass condition:** `g_Counter[i] == 400` for tasks 0–4

---

## Summary Table

| # | Test | Tasks | Builds | Pass condition | What it verifies |
|---|------|-------|--------|----------------|------------------|
| 1 | `YieldBasicTask` | 0–2 | both | each counter `== 100` | `Yield()` returns; no iteration lost or repeated |
| 2 | `PingPongTask` | 0–2 | both | `shared == 90`, counters `== 30` | `Yield()` hands the CPU to the awaited task; strict token ring completes |
| 3 | `LoneYielderTask` | 0 | both (timing: native) | `counter == 100`; native: elapsed `< 50` ms | A lone task makes progress; native yield does not stall it for a tick |
| 4 | `LoneYielderTask` | 0–2 | both (timing: native) | `counter == 100`; native: elapsed `< 50` ms, no peer awake | The only runnable task keeps running while the others sleep |
| 5 | `SleeperTask` | 0–2 | both | sleeper elapsed `[45, 75]` ms, yielders progressed | Yielding does not disturb another task's sleep timing |
| 6 | `FairnessTask` | 0–2 | both | counters `== 200`, ring adherence `>= 80%`, spread `<= 50` | Tasks step in ring order; no task is starved; reports workload time |
| 7 | `AddTaskTask` | 0–2 (+1 at run time) | both | each counter `== 50` | `Yield()` after the task list changed under a running rotation |
| 8 | `StressTask` | 0–4 | both | each counter `== 400` | No hang or lost iteration under heavy rotation with ticks landing mid-yield; runs on all platforms |
