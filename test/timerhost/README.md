# TimerHost Test Suite

Test suite for `stk::time::TimerHost`: timer operations in task mode, `Initialize()` with `Config` (weights, external
stack buffers, result codes), the polled mode (`InitializePolled()` / `Process()`) and the synchronous API
(`StartSync()`, `StopSync()`, ... with the guarantees of `StopSync()`).  
Source: `test/timer/test_timerhost.cpp`

---

## API Summary

```cpp
#include <time/stk_time_timer.h>
```

`TimerHost` is a **software timer multiplexer** that manages multiple `Timer` instances on top of a small fixed set of kernel tasks. All timers share the same internal tick and handler tasks, so the total kernel task overhead is constant regardless of how many timers are active.

Two timer modes are supported:
- **One-shot**: fires once after `delay` ticks and becomes inactive automatically
- **Periodic**: fires every `period` ticks until explicitly stopped

`TimerHost` has two operating modes:
- **Task mode** (`Initialize()`): `TimerHost` registers its own kernel tasks and runs on its own
- **Polled mode** (`InitializePolled()`): `TimerHost` registers no kernel tasks; the application calls `Process()` periodically and expired timers are handled inline. This is the mode for kernels where tasks must not block on queues, e.g. `KERNEL_HRT` (see the HRT suite, `test_hrt.cpp`)

In task mode `TimerHost` internally runs two categories of tasks:
- One **tick task** that maintains the active timer list, evaluates deadlines every wake cycle, and hands expired timers over to the handler tasks through an intrusive **ready list** (one node per `Timer`)
- One or more **handler tasks** (see `_STK_TIMER_THREADS_COUNT`) that take expired timers from the ready list and invoke their `OnExpired()` callbacks

Command queue capacity: `STK_TIMER_COMMAND_QUEUE_SIZE` (default 32; the old name `STK_TIMER_COUNT_MAX` is deprecated); it does not limit the number of timers.  
The basic API calls are **asynchronous** — they push commands to an internal queue and return immediately (when the queue is full, a caller which can wait blocks for a free slot, any other caller gets `RES_QUEUE_FULL`; before `Initialize()` / after `Shutdown()` they return `RES_NOT_READY`); the tick task (task mode) or the next `Process()` call (polled mode) processes the commands. The `...Sync()` variants queue the same command with a completion and **block** until the result is known (task context, task mode only; otherwise they return `RES_QUEUED`, or `RES_NOT_READY` if the host is not initialized or has been shut down).

Requires kernel mode: `KERNEL_SYNC` (the command queue is `sync::Pipe` based, the handler tasks wait on a `sync::Event`) and, in task mode, `KERNEL_DYNAMIC` (the host tasks exit on `Shutdown()`). Task mode cannot be used with `KERNEL_HRT`: the kernel rejects the soft real-time `AddTask()` (`ADD_FAIL_INVALID`).

```cpp
// Construction
stk::time::TimerHost g_TimerHost;

// Concrete timer by overriding OnExpired()
class HeartbeatTimer : public stk::time::TimerHost::Timer
{
public:
    void OnExpired(stk::time::TimerHost *host) { ToggleLed(); }
};

HeartbeatTimer g_Heartbeat;

// Initialize before use
g_TimerHost.Initialize(&g_Kernel, stk::ACCESS_USER);

// Start 500ms periodic heartbeat
uint32_t period = stk::GetTicksFromMs(500);
g_TimerHost.Start(g_Heartbeat, period, period);
```

```cpp
// Optional: weights and stack buffers of the host tasks, the result reports a failed registration
stk::time::TimerHost::Config cfg;
cfg.tick_weight    = stk::SwitchStrategyPT32::MakeWeight(5, 5);  // strategy-specific, forwarded unchanged
cfg.handler_weight = stk::SwitchStrategyPT32::MakeWeight(4, 4);

if (g_TimerHost.Initialize(&g_Kernel, stk::ACCESS_USER, cfg) != stk::ADD_OK)
{
    // e.g. no free kernel task slots
}
```

```cpp
// Polled mode: no kernel tasks, the application drives the host
g_TimerHost.InitializePolled();

for (;;)
{
    const stk::Timeout next = g_TimerHost.Process(); // expired OnExpired() callbacks run inline here
    // sleep/yield until the next job; "next" = ticks to the earliest deadline
}
```

### TimerHost methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Initialize` | `EAddResult Initialize(IKernel *kernel, EAccessMode mode, const Config &cfg = Config())` | Initialize timer host in task mode. Must be called before `Start()`. Creates tick task and handler task(s) with the weights and stacks of `cfg`. Returns `ADD_OK` or the first failure of `IKernel::AddTask()` (e.g. `ADD_FAIL_POOL_FULL`, `ADD_FAIL_DUPLICATE`, `ADD_FAIL_INVALID`); after a failure the host is unusable. `mode` applies to the handler tasks, the tick task is always `ACCESS_USER`. |
| `InitializePolled` | `void InitializePolled()` | Initialize timer host in polled mode: no kernel tasks are created, `Config` does not apply. Mutually exclusive with `Initialize()`; after `Shutdown()` the host can be initialized again in either mode. |
| `Process` | `Timeout Process()` | Polled mode only. Applies pending commands, evaluates deadlines and invokes `OnExpired()` of every expired timer inline. Never blocks. Returns ticks until the earliest deadline: `NO_WAIT` if a timer is overdue, `WAIT_INFINITE` if none is active. Call from one context only, not from a callback. |
| `Start` | `EResult Start(Timer &timer, uint32_t delay, uint32_t period = 0)` | Start timer. `delay` = initial delay in ticks before first expiration. `period` = reload period (0 for one-shot). Returns `RES_OK` (queued), `RES_ACTIVE` if the timer is already active (nothing queued), `RES_QUEUE_FULL`, `RES_CANCELLED`, `RES_NOT_READY`. Asynchronous. |
| `Stop` | `EResult Stop(Timer &timer)` | Stop running timer. Returns `RES_OK` (queued), `RES_INACTIVE` if the timer is not active (nothing queued), `RES_QUEUE_FULL`, `RES_CANCELLED`, `RES_NOT_READY`. Asynchronous. |
| `Reset` | `EResult Reset(Timer &timer)` | Reset periodic timer's deadline to `now + period`. Only works for active periodic timers. Returns `RES_OK` (queued), `RES_INACTIVE`, `RES_NOT_PERIODIC` (nothing queued in the last two cases), `RES_QUEUE_FULL`, `RES_CANCELLED`, `RES_NOT_READY`. Asynchronous. |
| `Restart` | `EResult Restart(Timer &timer, uint32_t delay, uint32_t period = 0)` | Atomically stop and re-start timer with new parameters. Safe to call regardless of current active state. Only one command queue slot consumed. Returns `RES_OK` (also when merged into an earlier queued re-arm), `RES_QUEUE_FULL`, `RES_CANCELLED`, `RES_NOT_READY`. Asynchronous. |
| `StartOrReset` | `EResult StartOrReset(Timer &timer, uint32_t delay, uint32_t period = 0)` | Start timer if inactive, or reset deadline if already active and periodic. Atomic operation eliminates TOCTOU race. If timer is active one-shot, no action taken. Returns `RES_OK` (also when merged into an earlier queued re-arm), `RES_QUEUE_FULL`, `RES_CANCELLED`, `RES_NOT_READY`. Asynchronous. |
| `SetPeriod` | `EResult SetPeriod(Timer &timer, uint32_t period)` | Change reload period of running periodic timer. Current deadline unaffected; new period takes effect on next reload. Returns `RES_OK` (queued), `RES_INVALID_PARAM` (period 0 or larger than `WAIT_INFINITE`), `RES_INACTIVE`, `RES_NOT_PERIODIC` (nothing queued in these cases), `RES_QUEUE_FULL`, `RES_CANCELLED`, `RES_NOT_READY`. Asynchronous. |
| `StartSync` | `EResult StartSync(Timer &timer, uint32_t delay, uint32_t period = 0)` | Synchronous `Start()`. `RES_OK`, or `RES_ACTIVE` if the timer is already active (nothing changed). |
| `ActivateSync` | `EResult ActivateSync(Timer &timer)` | Start with the delay and period stored in the timer (last start / restart / `ChangeSync()`). `RES_OK` or `RES_ACTIVE`. |
| `StopSync` | `EResult StopSync(Timer &timer)` | Synchronous `Stop()`. `RES_OK` if it was active, `RES_INACTIVE` otherwise. Cancels a queued expiration and waits for a callback which is already running, so the `Timer` can be released afterwards (not from a callback: there it does not wait). |
| `RestartSync` | `EResult RestartSync(Timer &timer, uint32_t delay, uint32_t period = 0)` | Synchronous `Restart()`. `RES_OK`. |
| `ResetSync` | `EResult ResetSync(Timer &timer)` | Synchronous `Reset()`. `RES_OK`, `RES_INACTIVE` or `RES_NOT_PERIODIC`. |
| `SetPeriodSync` | `EResult SetPeriodSync(Timer &timer, uint32_t period)` | Synchronous `SetPeriod()`. `RES_OK`, `RES_INACTIVE` or `RES_NOT_PERIODIC`; `RES_INVALID_PARAM` (nothing queued) if `period` is 0 or larger than `WAIT_INFINITE`. |
| `ChangeSync` | `EResult ChangeSync(Timer &timer, uint32_t delay, uint32_t period)` | Store a new delay and period in an inactive timer without starting it. `RES_OK` or `RES_ACTIVE`. |
| `QuerySync` | `EResult QuerySync(Timer &timer, TimerInfo &info)` | Snapshot (`active`, `remaining`, `delay`, `period`) evaluated by the tick task; advisory (`RES_OK`) where it cannot wait. |
| `Shutdown` | `EResult Shutdown()` | Gracefully shut down timer host. Task mode: sets a shutdown flag and wakes the handler tasks, which drain the ready list and exit. Polled mode: stops and releases all timers at the next `Process()`, the host stays usable. Returns `RES_OK` (queued), `RES_QUEUE_FULL`, `RES_CANCELLED`, `RES_NOT_READY` (not initialized or already shut down). Asynchronous. |

### TimerHost::Config

Optional parameter of `Initialize()`. A default-constructed `Config` reproduces the fixed behavior of earlier versions (`DEFAULT_WEIGHT`, internal stack buffers).

| Field | Default | Description |
|-------|---------|-------------|
| `tick_weight` | `DEFAULT_WEIGHT` | Weight of the tick task |
| `handler_weight` | `DEFAULT_WEIGHT` | Weight of every handler task |
| `tick_stack`, `tick_stack_size` | `nullptr`, `0` | External stack of the tick task (size in `Word` elements, `>= STK_STACK_SIZE_MIN`); `nullptr` selects the internal buffer |
| `handler_stack[]`, `handler_stack_size` | all `nullptr`, `0` | External stack of each handler task (one entry per handler, a `nullptr` entry selects the internal buffer) and the size of every non-null entry |

A weight is **strategy-specific** and forwarded to the kernel unchanged (only `NO_WEIGHT` is rejected by `TimerHost`):

| Strategy | Meaning of the weight |
|----------|----------------------|
| Round-robin, EDF, Monotonic | ignored |
| Fixed priority | priority level |
| Smooth weighted round-robin | CPU share, larger value = more slices, no preemption |
| Preemption threshold | `MakeWeight(priority, threshold)`; `DEFAULT_WEIGHT` decodes as priority 0 / threshold 1 (every timer task is *held* while running), so the weights must be set explicitly, with threshold == priority unless holding is intended |

External stack buffers must outlive the host and be distinct from each other and from the stacks of all other tasks (the kernel rejects a duplicate stack with `ADD_FAIL_DUPLICATE`).

### Timer (abstract base class)

| Method | Signature | Description |
|--------|-----------|-------------|
| `OnExpired` | `virtual void OnExpired(TimerHost *host) = 0` | Pure virtual callback invoked when timer expires. Override in concrete timer class. Called from a handler task (task mode) or inline from `Process()` (polled mode). |
| `IsActive` | `bool IsActive() const` | Returns `true` if timer is currently active (started and not yet stopped or expired if one-shot). |
| `GetDeadline` | `Ticks GetDeadline() const` | Returns absolute expiration time in ticks. |
| `GetTimestamp` | `Ticks GetTimestamp() const` | Returns time at which timer last expired (updated by `TimerHost`). |
| `GetPeriod` | `uint32_t GetPeriod() const` | Returns reload period in ticks (0 for one-shot). |
| `GetDelay` | `uint32_t GetDelay() const` | Returns the initial delay stored by the last start, restart or change command (0 if none). Used by `ActivateSync()`. |
| `GetRemainingTicks` | `uint32_t GetRemainingTicks() const` | Returns ticks remaining until next expiration, or 0 if already expired or inactive. Advisory only; computed from cached `m_now` which may be up to one tick cycle stale. |

**Key invariants:**

- **Asynchronous operations:** The plain `TimerHost` methods (`Start`, `Stop`, `Reset`, `Restart`, `StartOrReset`, `SetPeriod`) push commands to `m_commands` pipe and return immediately. The tick task (`UpdateTime`) processes commands asynchronously; in polled mode they take effect at the next `Process()`. Tests of these methods must account for this latency. The `...Sync()` methods do not need it: the result is the exact outcome of the command.
- **Result codes:** every command returns `EResult`, `RES_OK` is 0. The asynchronous ones return `RES_OK` when the command was accepted into the queue (its outcome is not reported), `RES_ACTIVE` / `RES_INACTIVE` / `RES_NOT_PERIODIC` / `RES_INVALID_PARAM` when a precondition or an argument is wrong (nothing queued, judged against the state applied by the last tick-task pass), `RES_QUEUE_FULL` / `RES_CANCELLED` when the queue has no room, `RES_NOT_READY` when the host is not initialized or has been shut down. The synchronous ones return the exact outcome (`RES_OK` = executed, ...), `RES_QUEUED` from a context which cannot wait, or an error.
- **Synchronous operations:** a `...Sync()` call waits only in task context with the kernel running in task mode. From any other context it queues the command and returns `RES_QUEUED` (`QuerySync()` returns an advisory snapshot). In polled mode nothing completes a command between `Process()` calls, so every sync call returns `RES_QUEUED`. If the host is not initialized or has been shut down they return `RES_NOT_READY` without queuing anything.
- **`StopSync()` guarantees:** after `RES_OK` / `RES_INACTIVE` the timer is not in the ready list (the command unlinks it), no callback of it will start, and a callback which a handler had already started has returned (`StopSync()` waits for it). Called from a callback it does not wait (a callback which stops its own timer would wait for itself) and guarantees only that no further callback starts.
- **Deadline computation:** One-shot: `deadline = m_now + delay`. Periodic: first fire at `deadline = m_now + delay`, subsequent fires at `deadline = previous_deadline + period - drift`.
- **Command queue depth:** `STK_TIMER_COMMAND_QUEUE_SIZE` (default 32). If the queue is full, a caller which can wait (task context, task mode, kernel running, not the tick task) blocks until the tick task has drained a slot; a caller which cannot wait (ISR, kernel not started, polled mode) gets `RES_QUEUE_FULL` and nothing is queued. No assertion fires.
- **Wake policy:** An expired timer is linked into the ready list `m_ready_list` through its own node (one node per `Timer`, so it is queued at most once and the list cannot overflow) and a handler is woken through `m_ready_event`. Handler tasks (`ProcessTimers`) unlink the timer, claim the expiration and invoke `OnExpired()`. Commands which cancel or re-arm a timer unlink its node.
- **Polled mode:** no handler queue is used: `OnExpired()` runs inline inside `Process()`, one expiration per timer per call. A timer which is overdue by more than its period fires once per call until its schedule has caught up (no drift is accumulated). Commands issued by a callback take effect at the next call.
- **Re-initialization:** `Initialize()` selects task mode again, so one host object can be used in either mode in turn (after `Shutdown()`).

**Blocking behavior:**
- Tick task: blocks on `m_commands.Read(cmd, next_sleep)` where `next_sleep` is the shortest deadline among all active timers (or `WAIT_INFINITE` if no timers active).
- Handler tasks: block on `m_ready_event` while the ready list is empty. After `Shutdown()` they drain the list and exit (each exiting handler wakes the next).
- Polled mode: nothing blocks; `Process()` drains the command queue with non-blocking reads (an empty list of active timers does not turn this into an infinite wait).

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_TIMER_TEST_TASKS_MAX` | `5` | Test tasks (not including TimerHost's internal tasks) |
| `_STK_TIMER_TEST_TIMEOUT` | `1000` ticks | Unused in timer tests (no blocking wait operations) |
| `_STK_TIMER_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_TIMER_TEST_LONG_SLEEP` | `100` ticks | Sleep used by verifier tasks to wait for workers |
| `_STK_TIMER_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

**Kernel task count:** `_STK_TIMER_TEST_TASKS_MAX + TimerHost::TASK_COUNT` to account for TimerHost's internal tasks (1 tick task + `_STK_TIMER_THREADS_COUNT` handler tasks, default 2 total).

`g_TimerHost` is **initialized fresh** before each test. Tests 1–8 and 28–33 use the default path: `g_TimerHost.Initialize(&g_Kernel, ACCESS_PRIVILEGED)` in `RunTest()` before the test tasks are added, and `g_TimerHost.Shutdown()` at the end of each test's verifier task.

The `Config` group (tests 9–14) and the polled group (tests 15–27) select their own initialization through `g_HostInit` before `RunTest()`; `InitHost()` then initializes the host **after** the test tasks were added to the kernel (a duplicate stack can only be detected then), and `RunTest()` resets `g_HostInit` to the default afterwards:

| Variable | Purpose |
|----------|---------|
| `g_HostInit` | `HOST_TASKS_DEFAULT` (tests 1–8, 28–33), `HOST_TASKS_CONFIG` (tests 9–14), `HOST_POLLED` (tests 15–27) |
| `g_ConfigParam` / `g_PolledParam` / `g_SyncParam` | Scenario id of `ConfigTask` (`CF_*`) / `PolledTask` (`PL_*`) / `SyncTask` (`SY_*`) |
| `g_InitResult` | `EAddResult` returned by `Initialize()` in `InitHost()` |
| `g_ExtTickStack`, `g_ExtHandlerStack[]` | External stack buffers used by the `Config` scenarios |
| `g_InProcess`, `g_InlineCount` | `true` while the poller is inside `Process()` / number of callbacks which ran in that state (proves inline execution) |
| `g_PolledDone` | Event set by the poller when its scenario is complete |
| `g_SyncDone` | Event set by task 1 of `SyncTask` when its scenario is complete |
| `g_CbStarted`, `g_CbDone` | `SlowTimer` callback: event signaled on entry / flag set as its last action (proves a callback is running, and that it has returned) |
| `g_CbResult` | Result of the `StopSync()` which `SelfStopTimer` issues from its own callback |

All groups run in the same way as the others: task 1 does the work, task 0 verifies. Polled scenarios always end with `Shutdown()` and one more `Process()`, which releases all timers because `g_TimerHost` is shared with the following tests.

`NeedsExtendedTasks` returns `false` for `StressTest` (uses all 5 tasks), `true` for all others (use only tasks 0–1).

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link twelve distinct task class templates simultaneously. Tests 1–32 are skipped on M0 and only `StressTest` (test 33) runs; the rest is under `#ifndef __ARM_ARCH_6M__`.

`StressTest` runs on M0 because it uses a single task class template (`StressTestTask`) instantiated for all five task slots, fitting within the available memory.

| Platform | `_STK_TIMER_STACK_SIZE` |
|----------|------------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `OneShotTimer`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 starts a one-shot timer with 50ms delay. It waits for the timer to fire (busy-wait on `g_ExpiredCount == 0`), measures elapsed time from start to firing, verifies it's in the range `[45, 65]`ms, then sleeps `_STK_TIMER_TEST_LONG_SLEEP` and confirms the timer did not fire again and is inactive. Task 0 is the verifier. Confirms one-shot timers fire exactly once at the expected time and automatically become inactive.

**Pass condition:** `g_SharedCounter == 2`  
(1 = timer fired once within `[45, 65]`ms; 2 = no second firing and timer inactive)

---

### Test 2 — `PeriodicTimer`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 starts a periodic timer with 30ms initial delay and 30ms period. It sleeps 150ms (allowing approximately 5 firings: 150 / 30 ≈ 5), then calls `Stop()` and waits for the asynchronous stop command to complete (polls `timer.IsActive()` with `Yield()` up to 10 times). It verifies the timer fired 4–6 times (tolerance for scheduler jitter) and is now inactive. Task 0 is the verifier. Confirms periodic timers fire repeatedly at regular intervals and that `Stop()` works asynchronously.

**Pass condition:** `counter == 1` (verified 4–6 firings and timer stopped)

---

### Test 3 — `MultipleTimers`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 starts three concurrent timers: timer1 (20ms periodic), timer2 (35ms periodic), timer3 (60ms one-shot). It sleeps 100ms, then stops timer1 and timer2. Expected firings in 100ms: timer1 ≈ 5 (100/20), timer2 ≈ 3 (100/35), timer3 = 1 (one-shot at 60ms) → total 7–11 with jitter tolerance. It verifies total firings in range and that timer3 (one-shot) is inactive. Task 0 is the verifier. Confirms multiple timers with different periods fire independently without interference.

**Pass condition:** `counter == 1` (verified 7–11 total firings and one-shot inactive)

---

### Test 4 — `StopTimer`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 starts a 100ms one-shot timer, immediately sleeps 2ms to ensure the `Start()` command is processed, then calls `Stop()`. It sleeps `_STK_TIMER_TEST_LONG_SLEEP` (100ms) and verifies the timer never fired (`g_ExpiredCount == 0`) and is inactive. Task 0 is the verifier. Confirms `Stop()` cancels a pending timer before it fires.

**Pass condition:** `counter == 1` (no firings and timer inactive)

---

### Test 5 — `ResetPeriodicTimer`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 starts a 40ms periodic timer, waits for the first firing, then calls `Reset()` and captures the reset timestamp. `Reset()` reanchors the deadline to `now + period` (40ms from the `Reset()` call). It waits for the second firing and measures elapsed time from `reset_time` to the second firing. It verifies the timer fired exactly twice and the second firing occurred approximately 40ms after `Reset()` (tolerance `[35, 50]`ms). Task 0 is the verifier. Confirms `Reset()` resets the deadline to now rather than maintaining the original schedule.

**Pass condition:** `counter == 1` (2 firings, second firing ~40ms after `Reset()`)

---

### Test 6 — `RestartTimer`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 starts a 30ms periodic timer, waits for the first firing, captures the current count, then calls `Restart(50)` (converting to one-shot with 50ms delay). It sleeps 2ms to wait for the async `Restart()` command to process, captures `restart_time`, then waits for the next firing (count increases beyond the captured value). It measures elapsed time from `restart_time` to the new firing, sleeps `_STK_TIMER_TEST_LONG_SLEEP`, and verifies exactly one additional firing occurred (not two), elapsed time is in `[45, 65]`ms, and the timer is now inactive (one-shot completed). Task 0 is the verifier. Confirms `Restart()` atomically stops the old timer and starts a new one with fresh parameters.

**Pass condition:** `counter == 1` (one firing after restart, ~50ms elapsed, timer inactive)

---

### Test 7 — `StartOrReset`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 tests two scenarios in sequence. **Scenario 1 (inactive → start):** calls `StartOrReset(40, 40)` on an inactive timer, waits for the async command to process (polls `timer.IsActive()` up to 10 times with `Yield()`), verifies the timer is now active, waits for the first firing. **Scenario 2 (active periodic → reset):** calls `StartOrReset(999, 999)` on the now-active periodic timer (delay and period parameters are ignored for active timers), waits 2ms for the async command to process, captures `reset_time`, waits for the second firing, and measures elapsed time. It verifies the second firing occurred approximately 40ms after the `StartOrReset()` call (original period, not the 999 passed in). Task 0 is the verifier. Confirms `StartOrReset()` correctly starts an inactive timer and resets an active periodic timer, and that both paths work in a single test run.

**Pass condition:** `counter == 1` (2 firings, second firing ~40ms after reset with tolerance `[35, 55]`)

---

### Test 8 — `SetPeriod`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** none

Task 1 starts a 40ms periodic timer, waits for the first firing, then calls `SetPeriod(60)` to change the reload period to 60ms. It sleeps 2ms to wait for the async command to process. The second firing should still occur approximately 40ms after the first (old period, because `SetPeriod` does not affect the current deadline). It captures the second firing timestamp, waits for the third firing, and measures elapsed time from the second to third firing. This elapsed time should be approximately 60ms (new period). It verifies 3 total firings with elapsed time in `[55, 75]`ms. Task 0 is the verifier. Confirms `SetPeriod()` changes the reload period without affecting the current in-flight deadline.

**Pass condition:** `counter == 1` (3 firings, third firing ~60ms after second)

---

### Tests 9–14 — `Initialize()` with `Config` (`ConfigTask`)
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** scenario `CF_*` selected by `g_ConfigParam` &nbsp;|&nbsp; **Host init:** `HOST_TASKS_CONFIG`

The six tests share one task class and differ only in the `Config` which `InitHost()` builds. Unless noted, task 1 starts a 30ms one-shot timer and waits for it (up to 500ms); task 0 sleeps 150ms and then checks the result of `Initialize()`, that the timer fired exactly once and, for external stacks, that the kernel holds the buffers. It then shuts the host down.

External stacks are checked in a portable way: a probe task which reports the same buffer as its stack must be rejected by the kernel with `ADD_FAIL_DUPLICATE` (so the check does not depend on how a port initializes stacks).

| # | Test | Scenario | Config | Expected |
|---|------|----------|--------|----------|
| 9 | `Config_Default` | `CF_DEFAULT_CFG` | explicit `Config()` | `ADD_OK`, timer fires once |
| 10 | `Config_Weights` | `CF_WEIGHTS` | `tick_weight = 3`, `handler_weight = 2` | `ADD_OK`, timer fires once. The kernel uses `SwitchStrategyRoundRobin`, which ignores weights: the test proves they are accepted and forwarded without side effects, not that a strategy receives the values |
| 11 | `Config_ExtStacks` | `CF_EXT_STACKS` | external tick stack and external handler stacks | `ADD_OK`, timer fires once, all external buffers are held by the kernel |
| 12 | `Config_ExtTickStack` | `CF_EXT_TICK_STACK` | external tick stack only | as 11, handler tasks keep the internal buffers |
| 13 | `Config_ExtHandlerStack` | `CF_EXT_HANDLER_STACK` | external handler stacks only | as 11, tick task keeps the internal buffer |
| 14 | `Config_DuplicateStack` | `CF_DUP_STACK` | handler stack = the stack of a user task which is already in the kernel | `Initialize()` returns `ADD_FAIL_DUPLICATE` and no host task stays registered |

For test 14 the host is not usable and task 1 only reports success; no host task may stay in the kernel: a handler task would wait for timers forever and `Kernel::Start()` would never return, so the test completing also proves nothing was left registered.

**Pass condition (all six):** `g_TestResult == 1` — `Initialize()` result as expected, timer fired exactly once (except test 14), external buffers held by the kernel (tests 11–13)

---

### Tests 15–27 — Polled mode (`PolledTask`)
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** scenario `PL_*` selected by `g_PolledParam` &nbsp;|&nbsp; **Host init:** `HOST_POLLED` (`InitializePolled()`)

There are no host tasks. Task 1 is the only caller of `Process()` and runs the scenario, polling every 2 ticks (`PollOnce()` / `PollFor()` / `PollUntilCount()`); `PolledTimer` additionally counts the callbacks which ran while the poller was inside `Process()`. Task 0 only waits (up to 3000ms) for `g_PolledDone`. Each scenario returns a `bool` and always ends with `Shutdown()` plus one `Process()`.

| # | Test | Scenario | What it does | Pass condition |
|---|------|----------|--------------|----------------|
| 15 | `Polled_OneShotInline` | `PL_ONESHOT_INLINE` | 50ms one-shot, polled for 150ms | 1 expiration, executed inline, elapsed in `[45, 70]`ms, timer inactive |
| 16 | `Polled_Periodic` | `PL_PERIODIC` | 30ms periodic, polled 150ms, `Stop()`, polled 90ms more | 4–6 expirations, none after `Stop()`, all inline, timer inactive |
| 17 | `Polled_DeferredCommands` | `PL_DEFERRED_COMMANDS` | `Start(100)` and `Stop()` with `IsActive()` sampled before and after each `Process()` | inactive right after `Start()`, active after `Process()`, still active right after `Stop()`, inactive after `Process()`; never fired |
| 18 | `Polled_Restart` | `PL_RESTART` | 30ms periodic, after its first expiration `Restart(50)` (one-shot) | next expiration `[45, 65]`ms after the `Restart()` call, exactly one extra expiration, timer inactive |
| 19 | `Polled_Reset` | `PL_RESET` | 40ms periodic, after its first expiration `Reset()` | second expiration `[35, 55]`ms after `Reset()` (not on the original schedule), 2 expirations in total |
| 20 | `Polled_ReturnValue` | `PL_RETURN_VALUE` | `Process()` with nothing active; two timers (200 / 60ms); `Stop()` of the earlier one; `Stop()` of the other | `WAIT_INFINITE`; `[50, 60]`; `(100, 200]`; `WAIT_INFINITE`; no expiration |
| 21 | `Polled_Overdue` | `PL_OVERDUE` | 10ms periodic timer, **no polling for 55ms**, then `Process()` until it no longer returns `NO_WAIT` | at most one expiration per call, 4–7 expirations in total (at least 4 calls), last return in `(0, 10]` |
| 22 | `Polled_SelfReArm` | `PL_SELF_REARM` | the callback of a one-shot timer re-starts itself (20 ticks) until 3 expirations (`ReArmTimer`) | 3 expirations, all inline, timer inactive: commands issued from a callback are applied by the next `Process()` and a one-shot timer can restart itself |
| 23 | `Polled_ShutdownReuse` | `PL_SHUTDOWN_REUSE` | two periodic timers (30 / 40ms), `Shutdown()`, then `Start(20)` again | `GetSize() == 2` before and `0` after `Shutdown()`, both timers inactive, nothing fires for 100ms, then 1 expiration: the host stays usable |
| 24 | `Polled_IdleNoBlock` | `PL_IDLE_NO_BLOCK` | three `Process()` calls with no active timer and no command | each returns `WAIT_INFINITE`, total time `< 30`ms (a blocking read would hang): guards the polled-mode exception to the "sleep forever when nothing is active" rule |
| 25 | `Polled_SyncQueued` | `PL_SYNC_QUEUED` | `StartSync(100)`, `QuerySync()`, `Process()`, `QuerySync()`, `StopSync()`, `Process()` | both `StartSync()` and `StopSync()` return `RES_QUEUED` (no one can complete a command in this mode, so they do not wait); `QuerySync()` is an advisory snapshot: inactive before the first `Process()`, then active with `delay == 100`, `period == 0`, `remaining` in `(0, 100]`; inactive after the stop, never fired |
| 26 | `Polled_AsyncResults` | `PL_ASYNC_RESULTS` | Result codes of the asynchronous API: commands are applied only by `Process()`, so the precondition checks see a known state | `Start()` → `RES_OK` (queued), `Stop()` before the `Process()` → `RES_INACTIVE`; on an active one-shot: `Start()` → `RES_ACTIVE`, `Reset()` / `SetPeriod(50)` → `RES_NOT_PERIODIC`; `SetPeriod(0)`, `SetPeriod(WAIT_INFINITE + 1)` and `SetPeriodSync(0)` → `RES_INVALID_PARAM`; on an inactive timer: `Stop()` / `Reset()` / `SetPeriod(50)` → `RES_INACTIVE`; `Restart()` / `StartOrReset()` → `RES_OK` (the second is merged into the first); on an active periodic timer `Reset()` and `SetPeriod(50)` → `RES_OK`, and the period is 50 afterwards; no expiration |
| 27 | `Polled_QueueFull` | `PL_QUEUE_FULL` | `Start()` repeatedly on an inactive timer without calling `Process()` until the command queue is full | the queue fills within `STK_TIMER_COMMAND_QUEUE_SIZE` commands and the next call returns `RES_QUEUE_FULL` (polled mode cannot wait: no block, no assertion); `Restart()`, `StartOrReset()`, `StartSync()` and `StopSync()` also return `RES_QUEUE_FULL`; after `Process()` drained the queue (one timer active) `Restart()` and `Stop()` return `RES_OK` again, i.e. the failed `Restart()` left no stale "re-arm queued" mark; no expiration. The exact capacity is not asserted, only that it does not exceed the macro |

**Pass condition (all thirteen):** the scenario function returns `true` and `g_PolledDone` was set in time (`g_TestResult == 1`)

---

### Tests 28–32 — Synchronous API (`SyncTask`)
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** scenario `SY_*` selected by `g_SyncParam` &nbsp;|&nbsp; **Host init:** default (task mode)

Task 1 runs the scenario (`bool`), task 0 waits up to 3000ms for `g_SyncDone`, checks the result and calls `Shutdown()`. The tests which need a running callback use `SlowTimer`, whose `OnExpired()` signals `g_CbStarted`, sleeps 80 ticks and sets `g_CbDone` as its very last action; it does not touch `g_ExpiredCount`.

| # | Test | Scenario | What it does | Pass condition |
|---|------|----------|--------------|----------------|
| 28 | `Sync_Basic` | `SY_BASIC` | Runs every synchronous command and checks its result | `StartSync()` → `RES_OK`, again → `RES_ACTIVE`; `QuerySync()` consistent (`active`, `delay`, `period`, `remaining`); `ChangeSync()` of an active timer → `RES_ACTIVE`; `StopSync()` → `RES_OK` then `RES_INACTIVE`; `ResetSync()` / `SetPeriodSync()` of an inactive timer → `RES_INACTIVE`; `ChangeSync()` stores the values without starting; `ActivateSync()` fires 25–50ms later; `ResetSync()` / `SetPeriodSync()` of an active one-shot → `RES_NOT_PERIODIC`; of an active periodic timer → `RES_OK`; `SetPeriodSync(0)` → `RES_INVALID_PARAM`; `QuerySync()` then shows the new period |
| 29 | `Sync_StopWaits` | `SY_STOP_WAITS` | `StartSync(20)` of a `SlowTimer`, wait until its callback has started, then `StopSync()` | returns `RES_INACTIVE` (the one-shot already expired) only after the callback has returned (`g_CbDone` set, at least 50ms spent in the call), timer inactive |
| 30 | `Sync_StopSelf` | `SY_STOP_SELF` | a 20ms periodic `SelfStopTimer` stops itself with `StopSync()` from its own callback | no deadlock; the call returns `RES_OK` (stored in `g_CbResult`); exactly one expiration in the following 100ms, timer inactive |
| 31 | `Sync_StopQueued` | `SY_STOP_QUEUED` | the only handler task is busy in a `SlowTimer` callback while a 20ms one-shot expires and stays in the ready list; `StopSync()` of the one-shot | returns `RES_INACTIVE`; the cancelled callback never runs (`g_ExpiredCount == 0` after the slow callback finished); the timer can be started again and fires exactly once |
| 32 | `Sync_NotReady` | `SY_NOT_READY` | `Shutdown()` (the only command queued), 100ms pause until the tick task has exited, then every kind of call | `Shutdown()` → `RES_OK`; afterwards `Start()`, `Restart()`, `Shutdown()`, `StartSync()`, `StopSync()` and `QuerySync()` all return `RES_NOT_READY` and queue nothing (a sync call would hang on a command which nobody completes), the timer stays inactive, `GetSize() == 0`. The shared host is initialized again by the next test |

**Pass condition (all five):** the scenario function returns `true` and `g_SyncDone` was set in time (`g_TestResult == 1`)

> `Sync_StopQueued` assumes `_STK_TIMER_THREADS_COUNT == 1` (the default): with more handler tasks a second handler would run the queued timer instead of leaving it in the ready list.

---

### Test 33 — `StressTest`
**Tasks:** 0–4 — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 20`

All five tasks run 20 iterations each. Each task manages its own dedicated `TestTimer` instance (timer0–timer4 with corresponding IDs). On each iteration, a task starts its timer with a delay of `10 + (task_id × 10)` ms (task 0 = 10ms, task 1 = 20ms, ..., task 4 = 50ms), waits for that specific timer to fire (busy-wait on `g_LastExpired[m_task_id]` event), increments a per-task counter if the global `g_ExpiredCount` increased (confirming a firing occurred), stops its timer, and paces every 5th iteration with a 1ms sleep to avoid overwhelming the command queue. Task 4 is also the verifier: it waits for all 5 tasks to complete (waits on `g_SharedCounter == 5`), then verifies each task's per-task counter equals 20 (each task's timer fired all 20 times without loss). Confirms no data corruption or timer loss under full five-task contention with varying timer delays. It runs last, in task mode again after the polled and synchronous tests, which also confirms that `Initialize()` switches the host back from polled mode.

**Pass condition:** all five per-task counters equal 20 (100 total firings, no loss)

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `OneShotTimerTask` | 0–1 | `counter == 2` | One-shot fires once at expected time in `[45, 65]`ms; becomes inactive automatically |
| 2 | `PeriodicTimerTask` | 0–1 | `counter == 1` | Periodic timer (30ms period) fires 4–6 times in 150ms; `Stop()` works asynchronously |
| 3 | `MultipleTimersTask` | 0–1 | `counter == 1` | Three concurrent timers (20ms, 35ms periodic + 60ms one-shot) fire independently; 7–11 total firings |
| 4 | `StopTimerTask` | 0–1 | `counter == 1` | `Stop()` cancels pending 100ms timer before firing; no firings occur |
| 5 | `ResetPeriodicTimerTask` | 0–1 | `counter == 1` | `Reset()` reanchors 40ms periodic timer; second firing ~40ms after `Reset()` call |
| 6 | `RestartTimerTask` | 0–1 | `counter == 1` | `Restart()` atomically stops 30ms periodic, re-starts as 50ms one-shot; one new firing ~50ms later |
| 7 | `StartOrResetTask` | 0–1 | `counter == 1` | `StartOrReset()` starts inactive timer, resets active periodic; both paths work in one test |
| 8 | `SetPeriodTask` | 0–1 | `counter == 1` | `SetPeriod(60)` changes reload period; 3rd firing ~60ms after 2nd (new period applied) |
| 9 | `Config_Default` | 0–1 | `Initialize()` = `ADD_OK`, timer fires once | Explicit default `Config()` behaves as before |
| 10 | `Config_Weights` | 0–1 | `ADD_OK`, timer fires once | Custom weights accepted and forwarded (ignored by the round-robin strategy) |
| 11 | `Config_ExtStacks` | 0–1 | `ADD_OK`, fires once, buffers held by the kernel | External stacks for the tick and handler tasks |
| 12 | `Config_ExtTickStack` | 0–1 | as 11 | External tick stack only |
| 13 | `Config_ExtHandlerStack` | 0–1 | as 11 | External handler stacks only |
| 14 | `Config_DuplicateStack` | 0–1 | `ADD_FAIL_DUPLICATE`, no host task registered | Failed registration is reported and leaves nothing behind |
| 15 | `Polled_OneShotInline` | 0–1 | 1 expiration in `[45, 70]`ms, inline | Polled one-shot runs inline from `Process()` |
| 16 | `Polled_Periodic` | 0–1 | 4–6 expirations, quiet after `Stop()` | Polled periodic timer and `Stop()` |
| 17 | `Polled_DeferredCommands` | 0–1 | state changes only at `Process()` | Commands take effect at the next `Process()` |
| 18 | `Polled_Restart` | 0–1 | one extra expiration ~50ms after `Restart()` | `Restart()` in polled mode |
| 19 | `Polled_Reset` | 0–1 | second expiration ~40ms after `Reset()` | `Reset()` in polled mode |
| 20 | `Polled_ReturnValue` | 0–1 | `WAIT_INFINITE` / ticks to the earliest deadline | Return value of `Process()` |
| 21 | `Polled_Overdue` | 0–1 | ≤ 1 expiration per call until caught up | Overdue periodic timer is served one expiration per call, returns `NO_WAIT` |
| 22 | `Polled_SelfReArm` | 0–1 | 3 expirations, all inline | Callback re-starts its own one-shot timer |
| 23 | `Polled_ShutdownReuse` | 0–1 | timers released, host reusable | `Shutdown()` in polled mode |
| 24 | `Polled_IdleNoBlock` | 0–1 | `WAIT_INFINITE`, returns within 30ms | `Process()` never blocks when idle |
| 25 | `Polled_SyncQueued` | 0–1 | `RES_QUEUED` from the sync calls | Sync API cannot wait in polled mode; `QuerySync()` is advisory |
| 26 | `Polled_AsyncResults` | 0–1 | exact `EResult` of every asynchronous command | `RES_OK`, `RES_ACTIVE`, `RES_INACTIVE`, `RES_NOT_PERIODIC`, `RES_INVALID_PARAM` of the asynchronous API |
| 27 | `Polled_QueueFull` | 0–1 | `RES_QUEUE_FULL` from every command kind | Full command queue in a context which cannot wait gives an error (no block, no assert) and leaves no stale re-arm mark |
| 28 | `Sync_Basic` | 0–1 | exact `EResult` of every command | Results of `StartSync`, `StopSync`, `RestartSync`, `ResetSync`, `SetPeriodSync`, `ChangeSync`, `ActivateSync`, `QuerySync` |
| 29 | `Sync_StopWaits` | 0–1 | `StopSync()` returns after the running callback | `StopSync()` waits for a callback which is already running |
| 30 | `Sync_StopSelf` | 0–1 | no deadlock, `RES_OK`, one expiration | A callback can stop its own timer with `StopSync()` |
| 31 | `Sync_StopQueued` | 0–1 | queued expiration cancelled, restart works | `StopSync()` removes an expiration waiting in the ready list |
| 32 | `Sync_NotReady` | 0–1 | `RES_NOT_READY` from every call kind | A stopped host rejects asynchronous and synchronous commands without queuing them |
| 33 | `StressTestTask` | 0–4 | `all counters == 20` | No loss or corruption under full contention; 5 tasks × 20 iterations = 100 firings; runs on all platforms |
