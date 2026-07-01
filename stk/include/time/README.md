# STK Time Module (`stk::time`)

**STK Time Module** provides lightweight, zero-allocation time utilities for embedded systems. It includes a single-task periodic polling trigger, a full-featured kernel-backed software timer framework capable of managing many independent timers at constant task overhead, and a lightweight stopwatch for measuring elapsed cycles between events.

## Features

- **Zero Dynamic Allocation**: All objects are allocated statically or on the stack — no heap usage.
- **Constant Task Overhead**: `TimerHost` uses a fixed set of kernel tasks regardless of how many timers are active.
- **Drift-Compensated Firing**: Both `PeriodicTrigger` and `TimerHost` advance deadlines by the configured period rather than resetting to the current time, preserving long-term firing accuracy even when individual callbacks are delayed.
- **One-shot and Periodic Modes**: Timers fire once or reload automatically, selectable per timer instance.
- **Atomic Operations**: `Restart()` and `StartOrReset()` perform compound state changes atomically with respect to the tick task, eliminating TOCTOU races.
- **Low-Power Aware**: The tick task sleeps until the nearest deadline; handler tasks block until a timer expires.
- **ISR Context**: `PeriodicTrigger` is suitable for use inside a single task or ISR. `TimerHost` commands must be issued from task context.
- **Source-Agnostic Measurement**: `Stopwatch` measures elapsed counter units (e.g. CPU cycles) between calls using a caller-supplied time source, independent of any particular hardware counter.

---

## Classes

### 1. Periodic Trigger (`time::PeriodicTrigger`)

A lightweight, self-contained periodic poll trigger requiring no kernel involvement. Internally tracks the absolute tick value of the next scheduled firing (`m_next`) and advances it by exactly one period on each successful `Poll()`, keeping the long-term rate stable.

- **Usage**: Call `Poll()` on every task iteration; it returns `true` once per configured period.
- **Phase-preserving period change**: `SetPeriod()` adjusts the next deadline proportionally so progress already made toward the next firing is not lost.
- **Catch-up semantics**: If `Poll()` is called late and multiple full periods have elapsed, only a single `true` is returned per call; subsequent calls catch up one period at a time.

**Key methods:**

| Method | Description |
|---|---|
| `PeriodicTrigger(period, start=false)` | Construct with period in ticks. Pass `start=true` to arm immediately. |
| `Restart()` | Reset and start; next firing occurs no earlier than `period` ticks from now. |
| `Poll()` | Returns `true` once per period when polled regularly. Must be started first. |
| `GetPeriod()` | Returns the currently configured period in ticks. |
| `SetPeriod(period)` | Changes the period while preserving phase toward the next firing. |

> **Note**: Not thread-safe. Intended for use within a single task or ISR context. If constructed without `start=true`, `Restart()` must be called before the first `Poll()`.

---

### 2. Timer Host (`time::TimerHost`)

A software timer multiplexer that manages many independent `Timer` instances on top of a small, fixed set of kernel tasks. The total task overhead is constant — one tick task plus one or more handler tasks — regardless of the number of active timers.

**Internal architecture:**

- **Tick task**: Maintains the active timer list, evaluates deadlines each wake cycle, and enqueues expired timers. Sleeps until the nearest deadline when no commands are pending.
- **Handler task(s)** (`STK_TIMER_THREADS_COUNT`, default 1): Dequeue expired timers and invoke their `OnExpired()` callbacks.

All control operations (`Start`, `Stop`, `Reset`, etc.) are issued as commands through a lock-free pipe to the tick task, keeping the caller non-blocking.

**Configuration macros:**

| Macro | Default | Description |
|---|---|---|
| `STK_TIMER_THREADS_COUNT` | `1` | Number of handler tasks. |
| `STK_TIMER_HANDLER_STACK_SIZE` | `256` | Stack size (words) for each handler task. Increase if callbacks use more. |
| `STK_TIMER_COUNT_MAX` | `32` | Maximum number of concurrently active timers. |

**`TimerHost` methods:**

| Method | Description |
|---|---|
| `Initialize(kernel, mode)` | Registers tick and handler tasks with the kernel. Must be called before any timers are started. |
| `Start(timer, delay, period=0)` | Starts a timer. `period=0` means one-shot; non-zero means periodic. Fails if timer is already active. |
| `Stop(timer)` | Stops a running timer. Fails if already inactive. |
| `Reset(timer)` | Resets a periodic timer's deadline to `now + period`. Fails if inactive or one-shot. |
| `Restart(timer, delay, period=0)` | Atomically stops and re-starts a timer. Safe regardless of current state. Consumes a single command queue slot. |
| `StartOrReset(timer, delay, period=0)` | Starts if inactive; resets deadline if active and periodic; no-ops if active one-shot. Eliminates TOCTOU race of manual `IsActive()` + `Start()`/`Reset()`. |
| `SetPeriod(timer, period)` | Changes the reload period of an active periodic timer. Takes effect on the next reload; call `Reset()` afterward to apply immediately. |
| `Shutdown()` | Stops all timers and terminates tick and handler tasks. |
| `IsEmpty()` | Returns `true` if no timers are currently active (advisory). |
| `GetSize()` | Returns the count of currently active timers (advisory). |
| `GetTimeNow()` | Returns the last tick value written by the tick task. |

---

### 3. Timer (`time::TimerHost::Timer`)

Abstract base class for a timer managed by `TimerHost`. Subclass it and override `OnExpired()` to implement timer behavior. Timer instances must be allocated in static storage and must outlive the `TimerHost`, or be explicitly stopped before destruction.

Each `Timer` instance may be registered with at most one `TimerHost` at a time.

**Key methods:**

| Method | Description |
|---|---|
| `OnExpired(host)` | Pure virtual. Called by a handler task when the timer fires. |
| `IsActive()` | Returns `true` if the timer is currently active. |
| `GetDeadline()` | Returns the absolute expiration time in ticks. |
| `GetTimestamp()` | Returns the tick value at which the timer last expired. |
| `GetPeriod()` | Returns the reload period in ticks (0 = one-shot). |
| `GetRemainingTime()` | Returns ticks remaining until next expiration (advisory, up to one tick-task cycle stale). Returns 0 if expired or inactive. |

---

### 4. Stopwatch (`time::Stopwatch`)

A lightweight elapsed-cycle measurement utility. Measures the number of CPU cycles (or other monotonic counter units) elapsed between successive calls to `Update()`. The time source is supplied by the caller as a callable (function pointer, lambda, or functor), so the stopwatch itself is independent of any particular hardware counter.

- **Usage**: Call `Start()` once with a time-source callable to capture the reference point, then call `Update()` (with the same callable) to retrieve elapsed cycles since the last `Start()`/`Update()` call.
- **Implicit start**: If `Update()` is called before `Start()`, the first call is treated as the start point and returns 0.
- **Wrap-around safe**: Counter wrap-around is handled naturally via unsigned subtraction, provided elapsed time doesn't exceed one full counter period.

**Key methods:**

| Method | Description |
|---|---|
| `Stopwatch()` | Construct in the stopped state. |
| `Start(now_func)` | Capture the current reference point using the supplied time-source callable. |
| `Update(now_func)` | Returns elapsed `Cycles` since the last `Start()`/`Update()` call, and updates the reference point. Returns 0 on the first call if `Start()` was not called beforehand. |

> **Note**: Not thread-safe. Intended for use within a single task or ISR context. `now_func` must return a `Cycles` value and should be the same time source across `Start()`/`Update()` calls in a given measurement.

---

## Usage Examples

### `PeriodicTrigger` — Polling-based periodic task

```cpp
#include <time/stk_time_util.h>

// Trigger every 500 ticks
stk::time::PeriodicTrigger trigger(500, true);

void TaskRun()
{
    while (true)
    {
        if (trigger.Poll())
        {
            // executes once per 500-tick period
            ReadSensor();
        }
    }
}
```

### `Stopwatch` — Measuring elapsed cycles

```cpp
#include <time/stk_time_util.h>

stk::time::Stopwatch sw;

void MeasureWork()
{
    sw.Start([]() { return stk::hw::HiResClock::GetCycles(); });

    DoWork();

    stk::Cycles elapsed = sw.Update([]() { return stk::hw::HiResClock::GetCycles(); });
    LogCycles(elapsed);
}
```

### `TimerHost` — Kernel-backed software timers

```cpp
#include <time/stk_time_timer.h>

// Concrete periodic timer: toggle LED every 500 ms
class HeartbeatTimer : public stk::time::TimerHost::Timer
{
public:
    void OnExpired(stk::time::TimerHost *host) override
    {
        ToggleLed();
    }
};

// Concrete one-shot timer: signal an event after 1 s
class NotifyTimer : public stk::time::TimerHost::Timer
{
public:
    void OnExpired(stk::time::TimerHost *host) override
    {
        g_Event.Signal();
    }
};

// Static instances — no heap
stk::time::TimerHost g_TimerHost;
HeartbeatTimer       g_Heartbeat;
NotifyTimer          g_Notify;

void SetupTimers(stk::IKernel *kernel)
{
    g_TimerHost.Initialize(kernel, stk::ACCESS_USER);

    // start 500 ms periodic heartbeat
    uint32_t period = stk::GetTicksFromMsec(500);
    g_TimerHost.Start(g_Heartbeat, period, period);

    // start a one-shot notification after 1 s
    uint32_t delay = stk::GetTicksFromMsec(1000);
    g_TimerHost.Start(g_Notify, delay);
}
```

### Watchdog refresh with `Restart()`

```cpp
// Safe to call from any task context: atomically re-arms the timer
// whether it is currently active or not, consuming a single queue slot.
g_TimerHost.Restart(g_WatchdogTimer, stk::GetTicksFromMsec(5000));
```

### Debounce with `StartOrReset()`

```cpp
// On each button event: start the debounce window if not already running,
// or push the deadline out if it is — without a TOCTOU race.
void OnButtonEvent()
{
    uint32_t debounce = stk::GetTicksFromMsec(50);
    g_TimerHost.StartOrReset(g_DebounceTimer, debounce);
}
```

### Changing a periodic timer's interval at runtime

```cpp
// Switch heartbeat from 500 ms to 250 ms.
// SetPeriod() takes effect on the next reload.
// Follow with Reset() to apply the new interval immediately.
uint32_t newPeriod = stk::GetTicksFromMsec(250);
g_TimerHost.SetPeriod(g_Heartbeat, newPeriod);
g_TimerHost.Reset(g_Heartbeat);
```

---

## Choosing Between `PeriodicTrigger` and `TimerHost`

| Criterion | `PeriodicTrigger` | `TimerHost::Timer` |
|---|---|---|
| Kernel required | No | Yes |
| Task overhead | None (polling) | 2+ kernel tasks (shared) |
| Number of timers | One per instance | Many, sharing fixed tasks |
| Callback model | Inline in calling task | Separate handler task |
| ISR safe | Yes (single task/ISR only) | Command API from task context only |
| Timeout support | No | No (fire-and-forget deadlines) |
| Typical use | Simple periodic work inside one task | Multiple independent timers across the system |
