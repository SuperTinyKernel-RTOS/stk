# TimerHost Test Suite

Test suite for `stk::time::TimerHost`.  
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

Internally `TimerHost` runs two categories of tasks:
- One **tick task** that maintains the active timer list, evaluates deadlines every wake cycle, and queues expired timers for dispatch
- One or more **handler tasks** (see `_STK_TIMER_THREADS_COUNT`) that dequeue expired timers and invoke their `OnExpired()` callbacks

Maximum concurrent active timers: `_STK_TIMER_COUNT_MAX` (default 32).  
All API calls are **asynchronous** — they push commands to an internal queue and return immediately; the tick task processes commands on its next wake cycle.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

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

### TimerHost methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Initialize` | `void Initialize(IKernel *kernel, EAccessMode mode)` | Initialize timer host instance. Must be called before `Start()`. Creates tick task and handler task(s). |
| `Start` | `bool Start(Timer &timer, uint32_t delay, uint32_t period = 0)` | Start timer. `delay` = initial delay in ticks before first expiration. `period` = reload period (0 for one-shot). Returns `false` if timer already active or command queue full. Asynchronous. |
| `Stop` | `bool Stop(Timer &timer)` | Stop running timer. Returns `false` if timer not active or queue full. Asynchronous. |
| `Reset` | `bool Reset(Timer &timer)` | Reset periodic timer's deadline to `now + period`. Only works for active periodic timers. Returns `false` if timer inactive, one-shot, or queue full. Asynchronous. |
| `Restart` | `bool Restart(Timer &timer, uint32_t delay, uint32_t period = 0)` | Atomically stop and re-start timer with new parameters. Safe to call regardless of current active state. Only one command queue slot consumed. Returns `false` if queue full. Asynchronous. |
| `StartOrReset` | `bool StartOrReset(Timer &timer, uint32_t delay, uint32_t period = 0)` | Start timer if inactive, or reset deadline if already active and periodic. Atomic operation eliminates TOCTOU race. If timer is active one-shot, no action taken. Returns `false` if queue full. Asynchronous. |
| `SetPeriod` | `bool SetPeriod(Timer &timer, uint32_t period)` | Change reload period of running periodic timer. Current deadline unaffected; new period takes effect on next reload. Returns `false` if timer inactive, one-shot, or queue full. Asynchronous. |
| `Shutdown` | `bool Shutdown()` | Gracefully shut down timer host. Wakes all handler tasks with shutdown sentinels. Returns `false` if queue full. Asynchronous. |

### Timer (abstract base class)

| Method | Signature | Description |
|--------|-----------|-------------|
| `OnExpired` | `virtual void OnExpired(TimerHost *host) = 0` | Pure virtual callback invoked when timer expires. Override in concrete timer class. Called from handler task context. |
| `IsActive` | `bool IsActive() const` | Returns `true` if timer is currently active (started and not yet stopped or expired if one-shot). |
| `GetDeadline` | `Ticks GetDeadline() const` | Returns absolute expiration time in ticks. |
| `GetTimestamp` | `Ticks GetTimestamp() const` | Returns time at which timer last expired (updated by `TimerHost`). |
| `GetPeriod` | `uint32_t GetPeriod() const` | Returns reload period in ticks (0 for one-shot). |
| `GetRemainingTime` | `uint32_t GetRemainingTime() const` | Returns ticks remaining until next expiration, or 0 if already expired or inactive. Advisory only; computed from cached `m_now` which may be up to one tick cycle stale. |

**Key invariants:**

- **Asynchronous operations:** All `TimerHost` methods (`Start`, `Stop`, `Reset`, `Restart`, `StartOrReset`, `SetPeriod`) push commands to `m_commands` pipe and return immediately. The tick task (`UpdateTime`) processes commands asynchronously. Tests must account for this latency.
- **Deadline computation:** One-shot: `deadline = m_now + delay`. Periodic: first fire at `deadline = m_now + delay`, subsequent fires at `deadline = previous_deadline + period - drift`.
- **Command queue depth:** `_STK_TIMER_COUNT_MAX` (default 32). If queue is full, commands fail and return `false`.
- **Wake policy:** Expired timers are pushed to `m_queue` (capacity 32). Handler tasks (`ProcessTimers`) dequeue and invoke `OnExpired()` callbacks.

**Blocking behavior:**
- Tick task: blocks on `m_commands.Read(cmd, next_sleep)` where `next_sleep` is the shortest deadline among all active timers (or `WAIT_INFINITE` if no timers active).
- Handler tasks: block on `m_queue.Read(timer)` waiting for expired timers.

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

`g_TimerHost` is **initialized fresh** before each test via `g_TimerHost.Initialize(&g_Kernel, ACCESS_PRIVILEGED)` in `RunTest()`, and shut down via `g_TimerHost.Shutdown()` at the end of each test's verifier task.

`NeedsExtendedTasks` returns `false` for `StressTest` (uses all 5 tasks), `true` for all others (use only tasks 0–1).

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link nine distinct task class templates simultaneously. Tests 1–8 are skipped on M0 and only `StressTest` (test 9) runs, under `#ifndef __ARM_ARCH_6M__`.

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

### Test 9 — `StressTest`
**Tasks:** 0–4 — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 20`

All five tasks run 20 iterations each. Each task manages its own dedicated `TestTimer` instance (timer0–timer4 with corresponding IDs). On each iteration, a task starts its timer with a delay of `10 + (task_id × 10)` ms (task 0 = 10ms, task 1 = 20ms, ..., task 4 = 50ms), waits for that specific timer to fire (busy-wait on `g_LastExpired[m_task_id]` event), increments a per-task counter if the global `g_ExpiredCount` increased (confirming a firing occurred), stops its timer, and paces every 5th iteration with a 1ms sleep to avoid overwhelming the command queue. Task 4 is also the verifier: it waits for all 5 tasks to complete (waits on `g_SharedCounter == 5`), then verifies each task's per-task counter equals 20 (each task's timer fired all 20 times without loss). Confirms no data corruption or timer loss under full five-task contention with varying timer delays.

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
| 9 | `StressTestTask` | 0–4 | `all counters == 20` | No loss or corruption under full contention; 5 tasks × 20 iterations = 100 firings; runs on all platforms |
