# STK Time Module (`stk::time`)

**STK Time Module** provides lightweight, zero-allocation time utilities for embedded systems. It includes a single-task periodic polling trigger, a full-featured kernel-backed software timer framework capable of managing many independent timers at constant task overhead (running on its own kernel tasks or, in polled mode, driven by the application, including from `KERNEL_HRT` jobs), and a lightweight stopwatch for measuring elapsed cycles between events.

## Features

- **Zero Dynamic Allocation**: All objects are allocated statically or on the stack — no heap usage.
- **Constant Task Overhead**: `TimerHost` uses a fixed set of kernel tasks regardless of how many timers are active (none at all in polled mode).
- **Drift-Compensated Firing**: Both `PeriodicTrigger` and `TimerHost` advance deadlines by the configured period rather than resetting to the current time, preserving long-term firing accuracy even when individual callbacks are delayed.
- **One-shot and Periodic Modes**: Timers fire once or reload automatically, selectable per timer instance.
- **Atomic Operations**: `Restart()` and `StartOrReset()` perform compound state changes atomically with respect to the tick task, eliminating TOCTOU races.
- **Low-Power Aware**: The tick task sleeps until the nearest deadline; handler tasks block until a timer expires. In polled mode `Process()` returns the ticks until the nearest deadline, so the caller can sleep exactly that long.
- **Two Operating Modes**: `TimerHost` runs either on its own kernel tasks (task mode) or without any kernel tasks (polled mode), where the application calls `Process()` periodically. Polled mode is the way to use timers with a `KERNEL_HRT` kernel.
- **Configurable Tasks**: In task mode `Initialize()` accepts a `Config` with the weights of the tick and handler tasks and optional external stack buffers, and reports a failed registration through its `EAddResult` result.
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

**Operating modes:**

| Mode | Initialization | Kernel tasks | Callbacks run in |
|---|---|---|---|
| Task mode | `Initialize()` | 1 tick task + `STK_TIMER_THREADS_COUNT` handler task(s) | A handler task |
| Polled mode | `InitializePolled()` | None | The context which calls `Process()` |

**Requirements:** the kernel must be built with `KERNEL_SYNC` (the command and ready queues are `stk::sync::Pipe` based). Task mode registers tasks which block on those queues: it needs `KERNEL_DYNAMIC` (the tasks exit on `Shutdown()`) and cannot be used with `KERNEL_HRT` — the kernel rejects the soft real-time `AddTask()` with `ADD_FAIL_INVALID`. Polled mode creates no tasks and has no such restriction. In task mode the kernel needs `1 + STK_TIMER_THREADS_COUNT` free task slots.

**Polled mode:** `InitializePolled()` creates no kernel tasks. The application calls `Process()` periodically from one context of its own (a task loop or an HRT job). Each call applies pending commands, evaluates the deadlines and runs the `OnExpired()` callbacks of expired timers **inline**, then returns the ticks until the earliest deadline (`NO_WAIT` if a timer is overdue, `WAIT_INFINITE` if none is active). `Process()` never blocks.

- The call period is the timer resolution: a timer fires on the first call at or after its deadline, i.e. late by at most one call period.
- Callbacks run in the caller's context and count against its time budget (e.g. the deadline of an HRT job). The worst-case time of all callbacks which can expire in one call is part of the caller's WCET. A callback must not call `Process()`.
- `Start()`, `Stop()` and the other commands may be issued from any task, callbacks included; they take effect at the next `Process()`.
- A periodic timer which is overdue by more than its period fires once per call until its schedule has caught up (no drift is accumulated).
- `Shutdown()` stops and releases all timers; there are no tasks to exit and the host stays usable.

**Configuration macros:**

| Macro | Default | Description |
|---|---|---|
| `STK_TIMER_THREADS_COUNT` | `1` | Number of handler tasks. |
| `STK_TIMER_HANDLER_STACK_SIZE` | `256` | Stack size (words) for each handler task. Increase if callbacks use more. |
| `STK_TIMER_COUNT_MAX` | `32` | Maximum number of concurrently active timers. |

**`TimerHost::Config`** (optional argument of `Initialize()`, task mode only; a default-constructed `Config` keeps the fixed behavior of earlier versions):

| Field | Default | Description |
|---|---|---|
| `tick_weight` | `DEFAULT_WEIGHT` | Weight of the tick task. |
| `handler_weight` | `DEFAULT_WEIGHT` | Weight of every handler task. |
| `tick_stack`, `tick_stack_size` | `nullptr`, `0` | External stack of the tick task (size in `Word` elements, at least `STK_STACK_SIZE_MIN`). `nullptr` selects the internal buffer. |
| `handler_stack[]`, `handler_stack_size` | all `nullptr`, `0` | External stack of each handler task (a `nullptr` entry selects the internal buffer) and the size of every non-null entry. |

A weight is **strategy-specific** and forwarded to the kernel unchanged (only `NO_WEIGHT` is rejected by `TimerHost`):

| Task-switching strategy | Meaning of the weight |
|---|---|
| Round-robin, EDF, Monotonic | Ignored. |
| Fixed priority | The priority level. |
| Smooth weighted round-robin | A CPU share; a larger value gives more slices but does not preempt. |
| Preemption threshold | `MakeWeight(priority, threshold)`. `DEFAULT_WEIGHT` decodes as priority 0 / threshold 1, i.e. every timer task would be *held* while it runs, so the weights **must** be set explicitly; use threshold == priority unless holding is intended. |

Recommended: the tick task at least as urgent as the handler tasks, otherwise a long callback delays the evaluation of all other deadlines. External stack buffers must outlive the host and be distinct from each other and from every other task's stack (the kernel rejects a duplicate with `ADD_FAIL_DUPLICATE`).

**`TimerHost` methods:**

| Method | Description |
|---|---|
| `Initialize(kernel, mode, cfg=Config())` | Task mode: registers tick and handler tasks with the kernel, using the weights and stacks of `cfg`. Must be called before any timers are started. Returns `ADD_OK` or the first failure of `IKernel::AddTask()` (e.g. `ADD_FAIL_POOL_FULL`, `ADD_FAIL_DUPLICATE`, `ADD_FAIL_INVALID`); after a failure the host is unusable. `mode` applies to the handler tasks, the tick task is always `ACCESS_USER`. |
| `InitializePolled()` | Polled mode: no kernel tasks are created and `Config` does not apply. Mutually exclusive with `Initialize()`; after `Shutdown()` the host can be initialized again in either mode. |
| `Process()` | Polled mode only. Applies pending commands, evaluates deadlines and runs the callbacks of expired timers inline. Never blocks. Returns ticks until the earliest deadline (`NO_WAIT` if a timer is overdue, `WAIT_INFINITE` if none is active). Call from one context only. |
| `Start(timer, delay, period=0)` | Starts a timer. `period=0` means one-shot; non-zero means periodic. Fails if timer is already active. |
| `Stop(timer)` | Stops a running timer. Fails if already inactive. |
| `Reset(timer)` | Resets a periodic timer's deadline to `now + period`. Fails if inactive or one-shot. |
| `Restart(timer, delay, period=0)` | Atomically stops and re-starts a timer. Safe regardless of current state. Consumes a single command queue slot. |
| `StartOrReset(timer, delay, period=0)` | Starts if inactive; resets deadline if active and periodic; no-ops if active one-shot. Eliminates TOCTOU race of manual `IsActive()` + `Start()`/`Reset()`. |
| `SetPeriod(timer, period)` | Changes the reload period of an active periodic timer. Takes effect on the next reload; call `Reset()` afterward to apply immediately. |
| `Shutdown()` | Stops all timers. Task mode: terminates the tick and handler tasks. Polled mode: the host stays usable. |
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
| `OnExpired(host)` | Pure virtual. Called when the timer fires: by a handler task (task mode) or inline from `Process()` (polled mode). |
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
    if (g_TimerHost.Initialize(kernel, stk::ACCESS_USER) != stk::ADD_OK)
    {
        return; // e.g. no free task slots in the kernel
    }

    // start 500 ms periodic heartbeat
    uint32_t period = static_cast<uint32_t>(stk::GetTicksFromMs(500));
    g_TimerHost.Start(g_Heartbeat, period, period);

    // start a one-shot notification after 1 s
    uint32_t delay = static_cast<uint32_t>(stk::GetTicksFromMs(1000));
    g_TimerHost.Start(g_Notify, delay);
}
```

### Watchdog refresh with `Restart()`

```cpp
// Safe to call from any task context: atomically re-arms the timer
// whether it is currently active or not, consuming a single queue slot.
g_TimerHost.Restart(g_WatchdogTimer, static_cast<uint32_t>(stk::GetTicksFromMs(5000)));
```

### Debounce with `StartOrReset()`

```cpp
// On each button event: start the debounce window if not already running,
// or push the deadline out if it is — without a TOCTOU race.
void OnButtonEvent()
{
    uint32_t debounce = static_cast<uint32_t>(stk::GetTicksFromMs(50));
    g_TimerHost.StartOrReset(g_DebounceTimer, debounce);
}
```

### Changing a periodic timer's interval at runtime

```cpp
// Switch heartbeat from 500 ms to 250 ms.
// SetPeriod() takes effect on the next reload.
// Follow with Reset() to apply the new interval immediately.
uint32_t newPeriod = static_cast<uint32_t>(stk::GetTicksFromMs(250));
g_TimerHost.SetPeriod(g_Heartbeat, newPeriod);
g_TimerHost.Reset(g_Heartbeat);
```

### Task weights and external stacks

```cpp
// Weights are strategy-specific: set the ones of the strategy the kernel uses.
stk::time::TimerHost::Config cfg;

// SwitchStrategyFP32: real preemption priority
cfg.tick_weight    = stk::SwitchStrategyFP32::PRIORITY_HIGH;
cfg.handler_weight = stk::SwitchStrategyFP32::PRIORITY_ABOVE_NORMAL;

// SwitchStrategyPT32: keep threshold == priority (no hold)
cfg.tick_weight    = stk::SwitchStrategyPT32::MakeWeight(5, 5);
cfg.handler_weight = stk::SwitchStrategyPT32::MakeWeight(4, 4);

// SwitchStrategySWRR: no preemption, only a larger share
cfg.tick_weight    = 4;
cfg.handler_weight = 1;

// Optional: external stack buffer for the tick task
static stk::StackMemoryDef<256>::Type g_TickStack;
cfg.tick_stack      = reinterpret_cast<stk::Word *>(&g_TickStack);
cfg.tick_stack_size = 256;

if (g_TimerHost.Initialize(kernel, stk::ACCESS_USER, cfg) != stk::ADD_OK)
{
    // registration failed, e.g. ADD_FAIL_POOL_FULL or ADD_FAIL_DUPLICATE
}
```

### Polled mode in an HRT kernel

```cpp
#include <time/stk_time_timer.h>

// A periodic HRT job drives the timers: its period is the timer resolution.
class TimerJob : public stk::Task<256, stk::ACCESS_USER>
{
public:
    void Run()
    {
        for (;;)
        {
            g_TimerHost.Process(); // runs expired callbacks inline
            stk::Yield();          // job completed, sleep until the next period
        }
    }
};

TimerJob g_TimerJob;

void SetupTimers(stk::IKernel *kernel) // kernel: KERNEL_HRT | KERNEL_SYNC
{
    g_TimerHost.InitializePolled();

    // 5 ms timer resolution, 2 ms budget (callbacks included) per job
    kernel->AddTask(&g_TimerJob, stk::GetTicksFromMsClampedToTimeout(5), stk::GetTicksFromMsClampedToTimeout(2), 0);
}
```

Timer commands (`Start()`, `Stop()`, ...) may be issued from any other task; they take effect at the next job.

### Polled mode in a plain task loop

```cpp
// Sleep only as long as needed. The cap bounds the delay with which commands
// issued meanwhile are picked up.
void TimerLoop()
{
    for (;;)
    {
        const stk::Timeout next = g_TimerHost.Process();

        if (next != stk::NO_WAIT)
        {
            stk::Sleep((next < 100) ? next : 100);
        }
    }
}
```

---

## Choosing Between `PeriodicTrigger` and `TimerHost`

| Criterion | `PeriodicTrigger` | `TimerHost::Timer` |
|---|---|---|
| Kernel required | No | Yes (`KERNEL_SYNC`) |
| `KERNEL_HRT` kernel | Yes (no kernel involvement) | Polled mode only (`InitializePolled()` + `Process()`) |
| Task overhead | None (polling) | 2+ kernel tasks (shared); none in polled mode |
| Number of timers | One per instance | Many, sharing fixed tasks |
| Callback model | Inline in calling task | Separate handler task (task mode) or inline from `Process()` (polled mode) |
| ISR safe | Yes (single task/ISR only) | Command API from task context only |
| Timeout support | No | No (fire-and-forget deadlines) |
| Typical use | Simple periodic work inside one task | Multiple independent timers across the system |
