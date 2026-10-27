# STK Time Module (`stk::time`)

**STK Time Module** provides lightweight, zero-allocation time utilities for embedded systems. It includes a single-task periodic polling trigger, a full-featured kernel-backed software timer framework capable of managing many independent timers at constant task overhead (running on its own kernel tasks or, in polled mode, driven by the application, including from `KERNEL_HRT` jobs), and a lightweight stopwatch for measuring elapsed cycles between events.

## Features

- **Zero Dynamic Allocation**: All objects are allocated statically or on the stack — no heap usage.
- **Constant Task Overhead**: `TimerHost` uses a fixed set of kernel tasks regardless of how many timers are active (none at all in polled mode).
- **Drift-Compensated Firing**: Both `PeriodicTrigger` and `TimerHost` advance deadlines by the configured period rather than resetting to the current time, preserving long-term firing accuracy even when individual callbacks are delayed.
- **One-shot and Periodic Modes**: Timers fire once or reload automatically, selectable per timer instance.
- **Atomic Operations**: `Restart()` and `StartOrReset()` perform compound state changes atomically with respect to the tick task, eliminating TOCTOU races.
- **Synchronous API**: `StartSync()`, `StopSync()`, `RestartSync()`, `ResetSync()`, `SetPeriodSync()`, `ActivateSync()`, `ChangeSync()` and `QuerySync()` block until the tick task has executed the command and return its exact outcome (`EResult`). `StopSync()` additionally guarantees that the host no longer references the timer (nothing queued, no callback running), so its storage can be released.
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

- **Tick task**: Maintains the active timer list, evaluates deadlines each wake cycle, and hands expired timers over to the handler tasks. Sleeps until the nearest deadline when no commands are pending.
- **Handler task(s)** (`STK_TIMER_THREADS_COUNT`, default 1): Take expired timers from the ready list and invoke their `OnExpired()` callbacks. They sleep on an event while the list is empty.
- **Ready list**: An intrusive list of expired timers, one node per `Timer`, protected by a short critical section. A timer is queued at most once, so the list has no capacity of its own and can never overflow. Commands which cancel or re-arm a timer (`Stop`, `Restart`, `Reset`, ...) unlink its node in O(1), so a cancelled expiration is never delivered.

The asynchronous control operations (`Start`, `Stop`, `Reset`, etc.) are issued as commands through a lock-free pipe to the tick task, keeping the caller non-blocking while the pipe has room (see [Command queue](#command-queue)). The synchronous variants queue the same commands together with a completion and block the caller until the result is known, see [Synchronous API](#synchronous-api).

**Operating modes:**

| Mode | Initialization | Kernel tasks | Callbacks run in |
|---|---|---|---|
| Task mode | `Initialize()` | 1 tick task + `STK_TIMER_THREADS_COUNT` handler task(s) | A handler task |
| Polled mode | `InitializePolled()` | None | The context which calls `Process()` |

**Requirements:** the kernel must be built with `KERNEL_SYNC` (the command queue is `stk::sync::Pipe` based, the handler tasks are woken by a `stk::sync::Event`). Task mode registers tasks which block on them: it needs `KERNEL_DYNAMIC` (the tasks exit on `Shutdown()`) and cannot be used with `KERNEL_HRT` — the kernel rejects the soft real-time `AddTask()` with `ADD_FAIL_INVALID`. Polled mode creates no tasks and has no such restriction. In task mode the kernel needs `1 + STK_TIMER_THREADS_COUNT` free task slots.

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
| `STK_TIMER_COMMAND_QUEUE_SIZE` | `32` | Capacity of the command queue: how many commands can wait for the tick task at once. It does **not** limit the number of timers (they are intrusive and owned by the application). See [Command queue](#command-queue). |
| `STK_TIMER_COUNT_MAX` | = `STK_TIMER_COMMAND_QUEUE_SIZE` | **Deprecated**, use `STK_TIMER_COMMAND_QUEUE_SIZE`. The old name suggested a limit on the number of timers. If an application still defines it (and not the new macro) its value is used as the queue size. |

**`TimerHost::Config`** (optional argument of `Initialize()`, task mode only; a default-constructed `Config` keeps the fixed behavior of earlier versions):

| Field | Default | Description |
|---|---|---|
| `tick_weight` | `DEFAULT_WEIGHT` | Weight of the tick task. |
| `handler_weight` | `DEFAULT_WEIGHT` | Weight of every handler task. |
| `tick_stack`, `tick_stack_size` | `nullptr`, `0` | External stack of the tick task (size in `Word` elements, at least `STK_STACK_SIZE_MIN`). `nullptr` selects the internal buffer. |
| `handler_stack[]`, `handler_stack_size` | all `nullptr`, `0` | External stack of each handler task (a `nullptr` entry selects the internal buffer) and the size of every non-null entry. |
| `tick_mode` | `ACCESS_USER` | Access mode of the tick task. It only touches host state, but a host used from privileged code (e.g. an RTOS compatibility layer) may need `ACCESS_PRIVILEGED`. |

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
| `Initialize(kernel, mode, cfg=Config())` | Task mode: registers tick and handler tasks with the kernel, using the weights, stacks and tick access mode of `cfg`. Must be called before any timers are started. Returns `ADD_OK` or the first failure of `IKernel::AddTask()` (e.g. `ADD_FAIL_POOL_FULL`, `ADD_FAIL_DUPLICATE`, `ADD_FAIL_INVALID`); after a failure the host is unusable. `mode` applies to the handler tasks, the tick task uses `Config::tick_mode` (`ACCESS_USER` by default). |
| `InitializePolled()` | Polled mode: no kernel tasks are created and `Config` does not apply. Mutually exclusive with `Initialize()`; after `Shutdown()` the host can be initialized again in either mode. |
| `Process()` | Polled mode only. Applies pending commands, evaluates deadlines and runs the callbacks of expired timers inline. Never blocks. Returns ticks until the earliest deadline (`NO_WAIT` if a timer is overdue, `WAIT_INFINITE` if none is active). Call from one context only. |
| `Start(timer, delay, period=0)` | Starts a timer. `period=0` means one-shot; non-zero means periodic. Returns `RES_OK` (queued), or `RES_ACTIVE` if the timer is already active (nothing queued). |
| `Stop(timer)` | Stops a running timer. Returns `RES_OK` (queued), or `RES_INACTIVE` if the timer is not active (nothing queued). |
| `Reset(timer)` | Resets a periodic timer's deadline to `now + period`. Returns `RES_OK` (queued), `RES_INACTIVE` or `RES_NOT_PERIODIC` (nothing queued in the last two cases). |
| `Restart(timer, delay, period=0)` | Atomically stops and re-starts a timer. Safe regardless of current state. Consumes a single command queue slot. Returns `RES_OK` (queued, or coalesced with a re-arm which is already waiting). |
| `StartOrReset(timer, delay, period=0)` | Starts if inactive; resets deadline if active and periodic; no-ops if active one-shot. Eliminates TOCTOU race of manual `IsActive()` + `Start()`/`Reset()`. Returns `RES_OK` (queued, or coalesced). |
| `SetPeriod(timer, period)` | Changes the reload period of an active periodic timer. Takes effect on the next reload; call `Reset()` afterward to apply immediately. Returns `RES_OK` (queued), `RES_INVALID_PARAM` (period 0 or larger than `WAIT_INFINITE`), `RES_INACTIVE` or `RES_NOT_PERIODIC`. |
| `StartSync(timer, delay, period=0)` | Synchronous `Start()`. Returns `RES_OK`, or `RES_ACTIVE` if the timer is already active (nothing is changed). |
| `ActivateSync(timer)` | Starts the timer with the delay and period stored in it (by the last start, restart or `ChangeSync()`). Returns `RES_OK` or `RES_ACTIVE`. |
| `StopSync(timer)` | Synchronous `Stop()`. Returns `RES_OK` if it was active, `RES_INACTIVE` otherwise. Cancels a queued expiration and waits for a callback which is already running, so the `Timer` can be released afterwards (see [Synchronous API](#synchronous-api)). |
| `RestartSync(timer, delay, period=0)` | Synchronous `Restart()`. Returns `RES_OK`. |
| `ResetSync(timer)` | Synchronous `Reset()`. Returns `RES_OK`, `RES_INACTIVE` or `RES_NOT_PERIODIC`. |
| `SetPeriodSync(timer, period)` | Synchronous `SetPeriod()`. Returns `RES_OK`, `RES_INACTIVE` or `RES_NOT_PERIODIC`; `RES_INVALID_PARAM` (nothing queued) if `period` is 0 or larger than `WAIT_INFINITE`. |
| `ChangeSync(timer, delay, period)` | Stores a new delay and period in an inactive timer without starting it (used by the next `ActivateSync()`). Returns `RES_OK` or `RES_ACTIVE`. |
| `QuerySync(timer, info)` | Fills a `TimerInfo` snapshot (`active`, `remaining`, `delay`, `period`) evaluated by the tick task, so all fields belong to the same moment. |
| `IsHostTid(tid)` | Returns `true` in task mode if `tid` is the tick task or a handler task (i.e. the caller is `TimerHost` code or an `OnExpired()` callback). |
| `Shutdown()` | Stops all timers. Task mode: terminates the tick and handler tasks. Polled mode: the host stays usable. Returns `RES_OK` (queued), or `RES_NOT_READY` if the host is already shut down. |

| `IsEmpty()` | Returns `true` if no timers are currently active (advisory). |
| `GetSize()` | Returns the count of currently active timers (advisory). |
| `GetTimeNow()` | Returns the last tick value written by the tick task. |

All commands (asynchronous and synchronous) return `EResult`, see [Result codes](#result-codes). Every command can additionally return `RES_NOT_READY` (host not initialized, or shut down) and, when the command queue is full, `RES_QUEUE_FULL` or `RES_CANCELLED`, see [Command queue](#command-queue).

`TimerHost::Timer` is a type alias of [`stk::time::Timer`](#3-timer-timetimer); both spellings can be used interchangeably.

---

### 3. Timer (`time::Timer`)

Abstract base class for a timer managed by `TimerHost`. Subclass it and override `OnExpired()` to implement timer behavior. Timer instances must be allocated in static storage and must outlive the `TimerHost`, or be explicitly stopped before destruction.

`Timer` is a namespace-level class (`stk::time::Timer`), independent of the definition of `TimerHost`: it can be forward-declared (`namespace stk { namespace time { class Timer; } }`) and used in headers which do not include `stk_time_timer.h`. The former nested name `TimerHost::Timer` is kept as a type alias of `stk::time::Timer`, so existing code keeps compiling unchanged.

Each `Timer` instance may be registered with at most one `TimerHost` at a time. The `Timer` carries two intrusive list hooks (one for the active list, one for the ready list), so it is larger than a plain list node and needs no allocation.

**Key methods:**

| Method | Description |
|---|---|
| `OnExpired(host)` | Pure virtual. Called when the timer fires: by a handler task (task mode) or inline from `Process()` (polled mode). |
| `IsActive()` | Returns `true` if the timer is currently active. |
| `GetDeadline()` | Returns the absolute expiration time in ticks. |
| `GetTimestamp()` | Returns the tick value at which the timer last expired. |
| `GetPeriod()` | Returns the reload period in ticks (0 = one-shot). |
| `GetDelay()` | Returns the initial delay stored by the last start, restart or change command (0 if none). Used by `ActivateSync()`. |
| `GetRemainingTicks()` | Returns ticks remaining until next expiration (advisory, up to one tick-task cycle stale). Returns 0 if expired or inactive. |

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

### Result codes

Every command returns `time::TimerHost::EResult`. Negative values are errors (nothing was queued or executed). `RES_OK` and the positive codes are not errors: the command was accepted or executed, or the timer is not in the state the command requires and nothing is changed.

| `EResult` | Value | Meaning |
|---|---|---|
| `RES_CANCELLED` | -5 | The wait for a free command queue slot was cancelled (`IKernel::CancelTaskWait()`). Nothing is queued. |
| `RES_INVALID_PARAM` | -4 | An argument is out of range (`SetPeriod()` / `SetPeriodSync()` with a period of 0 or larger than `WAIT_INFINITE`). Nothing is queued. |
| `RES_QUEUE_FULL` | -3 | The command queue is full and the caller cannot wait (ISR, kernel not started, polled mode, the tick task). Nothing is queued. |
| `RES_NOT_READY` | -2 | The host is not initialized (or its initialization failed), or it has been shut down. Nothing is queued. |
| `RES_FAIL` | -1 | Unspecified failure. |
| `RES_OK` | 0 | Synchronous API: the command was executed. Asynchronous API: the command was accepted into the command queue (or coalesced with an identical re-arm which is already waiting); its outcome is not reported. |
| `RES_ACTIVE` | 1 | The timer is active but the command requires it to be inactive (start, activate, change). Nothing is changed. |
| `RES_INACTIVE` | 2 | The timer is not active but the command requires it to be active (stop, reset, set period). |
| `RES_NOT_PERIODIC` | 3 | The timer is a one-shot but the command requires a periodic one (reset, set period). |
| `RES_QUEUED` | 4 | Synchronous API only: the calling context cannot wait (ISR, kernel not started, polled mode), the command was queued asynchronously and its outcome is unknown. |

The asynchronous commands check the preconditions against the state applied by the tick task so far (advisory, see `IsActive()`); the synchronous ones are judged by the tick task itself and are exact.

> **Migration**: the asynchronous commands used to return `bool`. `RES_OK` is 0, so a check written for the old return value, `if (host.Start(t, d))`, now means *failed* (and `if (!host.Start(t, d))` means *accepted*). Compare with `RES_OK` explicitly and handle the other codes as needed. A duplicate `Start()` of an active timer and `Stop()` of an inactive one used to return `true` and now return `RES_ACTIVE` / `RES_INACTIVE`. Before `Initialize()` (or after `Shutdown()`) the asynchronous commands used to be queued without a consumer; they now return `RES_NOT_READY`.

### Command queue

Commands of both APIs reach the tick task through a queue of `STK_TIMER_COMMAND_QUEUE_SIZE` entries. Size it for the burst of commands which can be issued before the tick task runs, not for the number of timers. When the queue is full:

- a caller which **can wait** (task context, task mode, kernel running, not the tick task) **blocks** until the tick task has drained a slot. No command is lost and nothing is asserted, so an asynchronous call is non-blocking only while the queue has room;
- a caller which **cannot wait** (ISR, kernel not started, polled mode, the tick task) gets `RES_QUEUE_FULL` and nothing is queued.

A blocking call must not be made from a critical section or with interrupts disabled. The wait fails with `RES_CANCELLED` only if it is cancelled (`IKernel::CancelTaskWait()`).

### Synchronous API

The asynchronous commands only queue a request: the caller gets `RES_OK` ("queued", or an error such as `RES_QUEUE_FULL` if it cannot wait for a free slot) and the result is evaluated later by the tick task. The `...Sync()` variants queue the same command with a completion and block until the tick task has executed it, so the returned `EResult` is the exact outcome. They also block while the command queue is full instead of failing, so the number of concurrent callers is not limited by `STK_TIMER_COMMAND_QUEUE_SIZE`.

The result codes are listed in [Result codes](#result-codes).

- **Where it waits**: only in task context, kernel running, task mode. From an ISR, before the kernel is started, or in polled mode, the command is queued asynchronously and `RES_QUEUED` is returned (`RES_QUEUE_FULL` if the queue is full, since it cannot wait there). `QuerySync()` then returns an advisory snapshot (`RES_OK`). If the host is not initialized or has been shut down, every call returns `RES_NOT_READY`.
- **Callbacks** may use the sync API in task mode (they run in handler tasks and the tick task never waits for them); in polled mode they get `RES_QUEUED`.
- **Priority**: the tick task must be at least as urgent as every task which uses the sync API, otherwise a more urgent caller is blocked by a less urgent tick task (priority inversion). Set the weights via `Config`.
- **No coalescing**: a command with a completion is never coalesced, every caller gets its own result.
- **Not abortable**: if the wait is cancelled (`IKernel::CancelTaskWait()`) the call keeps waiting, because the completion lives on the caller's stack.

**Stopping and releasing a timer**

An expired timer waits in the ready list until a handler task picks it up. Every command which cancels or re-arms the timer removes it from that list before it completes, so a cancelled expiration is dropped instead of being delivered, and the host keeps no queued pointer to the `Timer`.

- After `StopSync()` returns `RES_OK` or `RES_INACTIVE` the timer is not queued, no callback of it will start, and a callback which a handler task had already started has returned (`StopSync()` waits for it, polling once per tick). The `Timer` storage can be released.
- **Exception, stop from a callback**: when `StopSync()` is called from an `OnExpired()` callback it does not wait for running callbacks, because a callback which stops its own timer would wait for itself, and two callbacks stopping each other's timers would wait for each other. From a callback it guarantees only that no further callback of the timer starts. Do not release a timer from the callback of another timer.
- The asynchronous `Stop()` and the `RES_QUEUED` paths of the sync calls never wait. After them the timer must be kept alive until it is known to be stopped, for example by following with `StopSync()` from a task.
- `Restart()` / `RestartSync()` and the other commands drop a queued expiration as well, but do not wait for a callback which is already running (they re-arm the timer rather than release it).

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
class HeartbeatTimer : public stk::time::Timer
{
public:
    void OnExpired(stk::time::TimerHost *host) override
    {
        ToggleLed();
    }
};

// Concrete one-shot timer: signal an event after 1 s
class NotifyTimer : public stk::time::Timer
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

### Synchronous commands and safe release

```cpp
using stk::time::TimerHost;

// Exact outcome of each command, no polling of IsActive().
TimerHost::EResult res = g_TimerHost.StartSync(g_Notify, delay);
if (res == TimerHost::RES_ACTIVE)
{
    // already running, nothing was changed
}

// Change the interval of a running periodic timer and apply it right away.
if (g_TimerHost.SetPeriodSync(g_Heartbeat, newPeriod) == TimerHost::RES_OK)
{
    g_TimerHost.ResetSync(g_Heartbeat);
}

// Stop a timer and release it: once StopSync() returns, nothing is queued and no callback is running.
class DynamicTimer : public stk::time::Timer { /* ... */ };

void Destroy(DynamicTimer *timer)
{
    g_TimerHost.StopSync(*timer); // from a task, not from a timer callback
    g_TimerPool.Free(timer);      // e.g. a slot of an application pool: the storage can be reused now
}
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

| Criterion | `PeriodicTrigger` | `TimerHost` + `Timer` |
|---|---|---|
| Kernel required | No | Yes (`KERNEL_SYNC`) |
| `KERNEL_HRT` kernel | Yes (no kernel involvement) | Polled mode only (`InitializePolled()` + `Process()`) |
| Task overhead | None (polling) | 2+ kernel tasks (shared); none in polled mode |
| Number of timers | One per instance | Many, sharing fixed tasks |
| Callback model | Inline in calling task | Separate handler task (task mode) or inline from `Process()` (polled mode) |
| ISR safe | Yes (single task/ISR only) | Command API from task context only (sync variants queue and return `RES_QUEUED` from an ISR) |
| Timeout support | No | No (fire-and-forget deadlines) |
| Typical use | Simple periodic work inside one task | Multiple independent timers across the system |
