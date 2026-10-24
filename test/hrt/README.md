# HRT Test Suite

Test suite for the Hard Real-Time (HRT) mode of the kernel (`stk::KERNEL_HRT`), including detection and
reporting of deadline misses through `ITask::OnDeadlineMissed()` and `IPlatform::IEventOverrider::OnHardFault()`,
and the use of `stk::time::TimerHost` in HRT mode (polled mode: an HRT job drives the timers).  
Source: `test/hrt/test_hrt.cpp`

---

## API Summary

```cpp
#include <stk.h>
```

```cpp
// Registration of an HRT task: periodicity, deadline and start delay are given in ticks
void IKernel::AddTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc);

void stk::Yield();                                      // wraps IKernelService::SwitchToNext(): in HRT mode signals job completion, sleeps until the next period
void stk::Delay(Timeout ticks);                         // busy-wait (wall-clock based), used to simulate a workload
Time stk::GetTimeNowMs();                               // milliseconds since kernel start (used to timestamp jobs)
Ticks stk::GetTicksFromMs(Time ms, uint32_t resolution);// ms -> ticks, resolution = microseconds per tick (ISR-safe form)
void ITask::OnDeadlineMissed(uint32_t duration);        // called from the tick handler when a job overruns its deadline
void IPlatform::ProcessHardFault();                     // called by the kernel after OnDeadlineMissed()
bool IPlatform::IEventOverrider::OnHardFault();         // return true: fault is handled, the system is not halted
void IPlatform::SetEventOverrider(IEventOverrider *overrider, bool non_secure = false); // before Start()
```

The HRT `AddTask()` overload is only valid before `Start()` (it asserts after `Start()` or outside
`KERNEL_HRT` mode), because the HRT schedule is built from the complete task set. Periodicity and deadline
must be `> 0` and `< INT32_MAX`; a start delay of `0` activates the task immediately.

An HRT task is a periodic job: it is activated at `start_delay + i * periodicity`, does its work and calls
`Yield()` to report completion. The kernel then excludes the task from scheduling until its next period.
The deadline is measured from the moment of activation, so it also covers the time the task waits for the CPU.

| Call | Behaviour |
|------|-----------|
| `AddTask(task, period, deadline, delay)` | Registers an HRT task. First activation occurs after `delay` ticks, then every `period` ticks. |
| `Yield()` | In HRT mode signals completion of the current job and acts as a cooperation point; `NOSLEEP_YIELD_API` of the strategy is ignored. The task sleeps until its next period (absolute grid, no drift). Task context only, ISR-unsafe; the call busy-waits until the kernel has switched the task out. |
| `Delay(N)` | Busy-waits (spins in a loop) until the deadline of `N` ticks expires; a negative value asserts. `Sleep()` / `SleepUntil()` are unsupported in HRT mode (tasks sleep automatically by periodicity and workload), so the workload is simulated with `Delay()`. Use with care in HRT mode to avoid a missed deadline. |
| `GetTimeNowMs()` | Milliseconds since `IKernel::Start()`; with the default 1 ms tick it equals the tick count. Used to timestamp the start and end of every job. |
| `GetTicksFromMs(ms, resolution)` | Converts milliseconds to ticks without querying the kernel (ISR-safe). The suite wraps it as `MsToTicks()` with `PERIODICITY_DEFAULT` (1000 us per tick). |
| Return from `Run()` | Task exits (`KERNEL_DYNAMIC`): the slot is freed on the next tick and `ITask::OnExit()` is called. Remaining tasks keep their schedule. |
| `OnDeadlineMissed(duration)` | Called when the deadline is exceeded; `duration` is the elapsed active time in ticks and is always greater than the deadline. With the strict rule the first report is `deadline + 1`. Optional handler, intended for fault logging. |
| `ProcessHardFault()` | Invoked by the kernel after `OnDeadlineMissed()` returns, unless the switch strategy absorbs the overrun (`DEADLINE_MISSED_API`; `SwitchStrategyRoundRobin` does not). Enters the safe state of the system. |
| `OnHardFault()` | Optional platform event overrider hook. Returns `true` if the event is handled (the driver does not halt the system), `false` to let the driver handle it. |

**Key invariants:**

- Job starts stay on the `base + delay + i * period` grid: no accumulated drift over many periods.
- A healthy job (workload below its deadline) never triggers `OnDeadlineMissed()` or a hard fault.
- An overrun is detected with the strict rule `duration > deadline`; the first reported duration is `deadline + 1`.
- A deadline miss raises a hard fault; with a handling overrider the system survives and keeps running.
- An overrunning task does not disturb the schedule of a healthy task.
- A task may exit at any time (after `Yield()`, by returning from `Run()`, or without a single job) without
  affecting the remaining tasks.

---

### TimerHost in HRT mode (polled mode)

`TimerHost` in its default (task) mode registers kernel tasks that block on queues, which does not fit the HRT
model (the HRT `AddTask()` overload is not used and a job is not allowed to wait for events). In HRT mode the
host is therefore used in **polled mode**: no kernel tasks are created and an HRT job of the application drives
it by calling `Process()` once per job.

```cpp
#include <time/stk_time_timer.h>
```

```cpp
void    TimerHost::InitializePolled();                                // polled mode, no kernel tasks; before Start()
Timeout TimerHost::Process();                                         // one job: apply commands, scan deadlines, run expired callbacks inline
bool    TimerHost::Start(Timer &tmr, uint32_t delay, uint32_t period = 0);
bool    TimerHost::Stop(Timer &tmr);
bool    TimerHost::Restart(Timer &tmr, uint32_t delay, uint32_t period = 0);
bool    TimerHost::Shutdown();                                        // stops all timers, the host stays usable in polled mode
void    Timer::OnExpired(TimerHost *host);                            // callback, runs inline in the context which calls Process()
```

| Call | Behaviour |
|------|-----------|
| `InitializePolled()` | Selects polled mode. No kernel task slots are used, weights and stack buffers of `Config` do not apply. Mutually exclusive with `Initialize()`. |
| `Process()` | Never blocks (non-blocking queue operations only). Applies pending commands, evaluates the deadlines of the active timers and invokes `OnExpired()` of every expired timer **inline**. Returns the ticks until the earliest deadline (`NO_WAIT` if a timer is overdue, `WAIT_INFINITE` if none is active). Call it from one context only, not from a callback. |
| `Start()` / `Stop()` / `Restart()` / `Shutdown()` | May be issued from any task; the command is queued and takes effect at the **next** `Process()`, before the deadline scan. |
| `OnExpired()` | Runs inside `Process()`, i.e. inside the HRT job: its execution time counts against the deadline of that job. |

**Key invariants (TimerHost in HRT mode):**

- The resolution of the timers is the period of the driving job: a timer expires on the first job at or after its
  deadline, never earlier, and late by at most one job period.
- Periodic timers keep their phase (no accumulated drift), exactly like HRT jobs.
- Every callback runs inline from `Process()`; the worst-case execution time of all callbacks which can expire in
  the same job is part of the WCET of that job.
- A callback which makes the job exceed its deadline is an ordinary deadline miss (`OnDeadlineMissed()` +
  hard fault); it does not disturb the schedule of the following jobs.
- Commands from another HRT task are applied by the next job of the driver and keep their order.
- `TimerHost` queues are `sync::Pipe` based, the kernel must be built with `KERNEL_SYNC`.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_HRT_TEST_TASKS_MAX` | `4` | Kernel task capacity (and number of task objects created by `RunTest`) |
| `_STK_HRT_TEST_ITRS_MAX` | `20` | Maximum number of jobs recorded per task |
| `_STK_HRT_TEST_START_TOL_MS` | `10` ms | Allowed deviation of a job start from its expected time |
| `_STK_HRT_TEST_WORK_LO_MS` | `1` ms | Allowed "early" slack of a measured workload (measurement jitter) |
| `_STK_HRT_TEST_WORK_HI_MS` | `10` ms | Allowed "late" slack of a measured workload |
| `_STK_HRT_STACK_SIZE` | `384` (RISC-V) / `256` (others) | Per-task stack size in `size_t` words |
| `_STK_HRT_TH_JOBS_MAX` | `24` | Maximum number of driver jobs recorded by the TimerHost tests (15–18) |
| `_STK_HRT_TH_FIRES_MAX` | `8` | Maximum number of timer expirations recorded per timer (tests 15–18) |

Timing assumes the default 1 ms tick. A measured workload of `W` ms is accepted in `[W - 1, W + 10]`.
A job start is accepted within `±10` ms of `base + delay + i * period`.

The suite is built once, with the kernel configured as:

```
Kernel<KERNEL_DYNAMIC | KERNEL_HRT | KERNEL_SYNC, 4, SwitchStrategyRoundRobin, PlatformDefault>
```

`KERNEL_DYNAMIC` makes `Kernel::Start()` return once all tasks have exited, which is how each test completes
and is verified. `KERNEL_SYNC` is required by the queues of `TimerHost` (tests 15–18) and is active for all tests
of the suite. Task objects are `static` inside `RunTest` / `RunTimerTest`, so a removed task (and its stack) is
reused by the next test.

**Per-task configuration (`TaskCfg`, all times in ms):**

| Field | Purpose |
|-------|---------|
| `period` | HRT periodicity |
| `deadline` | HRT deadline |
| `delay` | Start delay |
| `work` | Workload of one job (busy delay) |
| `iterations` | Number of jobs before the task exits |
| `yield_last` | `Yield()` after the last job (`false`: return from `Run()` right away) |
| `expect_miss` | Workload exceeds the deadline: a deadline miss is expected |

**Test state:** all shared state lives in static arrays indexed by task id, and every element has a single writer:

| Variable | Purpose |
|----------|---------|
| `g_Cfg[id]` | Task configuration of the current scenario |
| `g_Start[id][job]` | Recorded start time (ms) of every job |
| `g_Diff[id][job]` | Measured workload (ms) of every job |
| `g_Count[id]` | Number of completed jobs |
| `g_MissCount[id]` | Number of `OnDeadlineMissed()` calls |
| `g_MissFirst[id]` | Duration reported by the first `OnDeadlineMissed()` call |
| `g_Overrider.m_faults` | Number of hard faults counted by the event overrider |

`ResetTestState()` clears all state before each test. `RunTest<T>(name, scenario)` fills `g_Cfg` through
`SetupScenario()`, adds the tasks with their periodicity / deadline / delay converted to ticks, runs the kernel
and verifies the recorded schedule.

**Hard fault handling.** `HrtOverrider` implements `IPlatform::IEventOverrider`; its `OnHardFault()` counts the
fault and returns `true` (handled) so the platform driver does not halt the system. It is installed once in
`main()` with `SetEventOverrider()` before `Start()`. In all scenarios without an expected miss the fault counter
must stay zero.

**TimerHost scenarios (`ThCfg`, all times in ms).** Tests 15–18 use an HRT *driver* job which calls
`TimerHost::Process()` and, optionally, an HRT *controller* task which issues timer commands from its own job.

| Field | Purpose |
|-------|---------|
| `period` | Driver periodicity (the timer resolution) |
| `deadline` | Driver deadline: covers `Process()` together with all callbacks it runs |
| `jobs` | Number of driver jobs before it exits |
| `work0` / `work1` | Workload (busy delay) of the callback of timer 0 / timer 1 |
| `ctrl_period` / `ctrl_deadline` / `ctrl_delay` | HRT parameters of the controller task |
| `ctrl_jobs` | Number of controller jobs (`0`: no controller task) |
| `expect_miss` | A callback exceeds the driver deadline: a deadline miss is expected |

| Variable | Purpose |
|----------|---------|
| `g_TimerHost` | The `TimerHost` under test (polled mode, shared by the tests, released at the end of every test) |
| `g_ThCfg` / `g_ThScenario` | Configuration and id of the current scenario |
| `g_ThInJob` | `true` while the driver is inside `Process()` (used to prove callbacks run inline) |
| `g_ThJobs`, `g_ThJobStart[job]`, `g_ThJobDur[job]` | Completed driver jobs, start time and duration (ms) of every job |
| `g_ThFireCount[timer]`, `g_ThInline[timer]` | Number of expirations / of those which ran inside `Process()` |
| `g_ThFireTime[timer][n]` | Start time (ms) of the n-th callback |
| `g_ThIssued[timer][0 / 1]` | Time (ms) at which `Start()` / `Restart()` was called |

`ResetTestState()` and `ResetThState()` clear all state before each test. The deadline-miss counters
(`g_MissCount`, `g_MissFirst`, id 0 = driver, id 1 = controller) and `g_Overrider` are shared with tests 1–14.
`RunTimerTest<T>(name, scenario)` fills `g_ThCfg` through `ThSetup()`, calls `InitializePolled()`, adds the driver
(and the controller) as HRT tasks, runs the kernel and checks the result with `VerifyTh()`. The driver starts the
timers of the scenario in its first job and shuts the host down after its last job.

---

## Verification

The kernel tick counter keeps running between tests, so expected times are relative to a base derived from the
first job of the first task that ran at least one job:

```
base   = start[ref][0] - delay[ref]
expect = base + delay + i * period        (expected start of job i)
```

`VerifyScenario()` checks, for every task:

| Check | Condition |
|-------|-----------|
| Jobs done | `g_Count == iterations` |
| Start time | `abs(start - expect) <= 10` ms |
| Workload | `diff` in `[work - 1, work + 10]` ms |
| Deadline (healthy tasks) | `diff <= deadline` |
| Misses (healthy tasks) | `g_MissCount == 0` |
| Misses (`expect_miss`) | `g_MissCount > 0` and `g_MissFirst == deadline + 1` |
| Hard faults | none if no miss expected, at least one if any miss expected |

**TimerHost tests (`VerifyTh()`).** Expected times are relative to the start of driver job 0
(`base = g_ThJobStart[0]`) and to the recorded `Start()` / `Restart()` call times. For the n-th expiration of a
timer `late = g_ThFireTime - deadline`, where `deadline = issued + delay + n * period`:

| Check | Condition |
|-------|-----------|
| Driver jobs done | `g_ThJobs == jobs` |
| Inline execution | `g_ThInline == g_ThFireCount` for every timer: all callbacks ran inside `Process()` |
| Driver schedule | `abs(start - (base + j * period)) <= 10` ms for every job `j` |
| Job duration | jobs which ran callbacks are counted; every other job lasts at most 4 ms |
| Deadline (healthy scenarios) | `duration <= deadline` for every job |
| Expiration count | exact per timer and scenario |
| Expiration time | `late` in `[-2, period + 10 + work0]` ms: never earlier than the deadline (2 ms jitter), late by at most one job period plus tolerance |
| Misses | only the driver and only if `expect_miss`: `g_MissCount > 0` and `g_MissFirst == deadline + 1`; none otherwise (controller included) |
| Hard faults | none if no miss expected, at least one otherwise |

---

## Platform Notes

The task stack is larger on RISC-V (`384` words) than on other architectures (`256` words). Tests 1–14 run on
every platform the suite is built for. The TimerHost tests (15–18) are excluded on Cortex-M0
(`__ARM_ARCH_6M__`, as in the TimerHost suite): `TimerHost` keeps its queues and internal task stacks in static
memory even in polled mode. Their build needs the include path of `time/stk_time_timer.h`.

---

## Tests

Tests 1–14 use the `HrtTask` class and differ only in the scenario passed to `RunTest`. Times below are in ms
(`period / deadline / delay / work / jobs`). Tests 15–18 (TimerHost in HRT mode) use `ThDriverTask` and
`ThControllerTask` through `RunTimerTest`, see the section after test 14.

### Test 1 — `Basic` (`SC_BASIC`)
**Tasks:** 0–2

Tasks are activated one after another every 100 ms with a varying workload of 10 / 20 / 30 ms, period 300 ms,
deadline 50 ms, 3 jobs each. This is the original test of the suite.

| Task | Period | Deadline | Delay | Work | Jobs |
|------|--------|----------|-------|------|------|
| 0 | 300 | 50 | 0 | 10 | 3 |
| 1 | 300 | 50 | 100 | 20 | 3 |
| 2 | 300 | 50 | 200 | 30 | 3 |

**Pass condition:** all jobs done, starts on the grid, workloads in range, no deadline miss, no hard fault

---

### Test 2 — `SingleTask` (`SC_SINGLE_TASK`)
**Tasks:** 0

One task alone in the system (`50 / 25 / 0 / 5 / 5`). The CPU enters the sleep trap every period.

**Pass condition:** as test 1

---

### Test 3 — `ZeroWork` (`SC_ZERO_WORK`)
**Tasks:** 0

A job completes immediately: `Yield()` without any work (`20 / 10 / 0 / 0 / 6`). The whole period is spent
sleeping.

**Pass condition:** as test 1

---

### Test 4 — `Simultaneous` (`SC_SIMULTANEOUS`)
**Tasks:** 0–2

Three tasks become active at the same moment (`60 / 40 / 0`, work 10 / 15 / 20, 3 jobs). The deadline also covers
the time spent waiting for the CPU. Workloads overlap because `Delay()` is wall-clock based.

**Pass condition:** as test 1

---

### Test 5 — `TightDeadline` (`SC_TIGHT_DEADLINE`)
**Tasks:** 0–1

The deadline is only 8 ms above the workload (`100 / 28 / 0 / 20 / 4` and `100 / 28 / 50 / 20 / 4`). No miss is
allowed.

**Pass condition:** as test 1

---

### Test 6 — `LongStartDelay` (`SC_LONG_START_DELAY`)
**Tasks:** 0–1

Task 1 first starts after 200 ms, longer than its own period of 80 ms, while task 0 runs 8 jobs of period 50 ms
(`50 / 20 / 0 / 5 / 8` and `80 / 30 / 200 / 10 / 2`).

**Pass condition:** as test 1

---

### Test 7 — `DifferentPeriods` (`SC_DIFFERENT_PERIODS`)
**Tasks:** 0–2

Tasks with different periodicities and workloads run side by side: periods 40 / 60 / 100, delays 0 / 10 / 20,
work 5 / 8 / 12, deadline 30.

**Pass condition:** as test 1

---

### Test 8 — `NoDrift` (`SC_NO_DRIFT`)
**Tasks:** 0

A single task runs 20 periods (`20 / 10 / 0 / 3 / 20`). Every start must stay on the `start + i * period` grid,
so the period is exact and no drift accumulates.

**Pass condition:** as test 1

---

### Test 9 — `StaggeredExit` (`SC_STAGGERED_EXIT`)
**Tasks:** 0–2

Tasks exit after 1, 3 and 6 jobs (`30 / 20`, delays 0 / 10 / 20, work 5). The remaining tasks must keep their
schedule while others leave.

**Pass condition:** as test 1

---

### Test 10 — `ExitWithoutYield` (`SC_EXIT_WITHOUT_YIELD`)
**Tasks:** 0–1

The last job ends by returning from `Run()` instead of calling `Yield()` (`yield_last = false`). Tasks
`40 / 20 / 0 / 5 / 3` and `40 / 20 / 20 / 5 / 3`.

**Pass condition:** as test 1

---

### Test 11 — `ImmediateExit` (`SC_IMMEDIATE_EXIT`)
**Tasks:** 0–1

Task 0 returns from `Run()` without running a single job (`iterations = 0`), task 1 runs 3 jobs. The reference
task for the time base is the first task that executed at least one job.

**Pass condition:** task 0 has zero jobs, task 1 as test 1

---

### Test 12 — `FullPool` (`SC_FULL_POOL`)
**Tasks:** 0–3 (all 4)

All kernel task slots are used. Period 200, deadline 40, delays 0 / 50 / 100 / 150, work 10 / 20 / 30 / 10,
3 jobs each. Activations are spread across the common period.

**Pass condition:** as test 1

---

### Test 13 — `DeadlineMiss` (`SC_DEADLINE_MISS`)
**Tasks:** 0

Workload 40 ms against a deadline of 20 ms (`100 / 20 / 0 / 40 / 2`, `expect_miss`). The overrun is detected by the
strict `duration > deadline` rule, so the first reported duration is `deadline + 1 = 21`, and a hard fault is
raised and survived through `HrtOverrider`.

**Pass condition:** `g_MissCount > 0`, `g_MissFirst == 21`, at least one hard fault, all jobs still completed

---

### Test 14 — `MissIsolated` (`SC_MISS_ISOLATED`)
**Tasks:** 0–1

Task 0 overruns (`100 / 20 / 0 / 40 / 3`, `expect_miss`), task 1 is healthy (`100 / 30 / 50 / 10 / 3`). Verifies
that the missing task does not disturb the schedule of the healthy one.

**Pass condition:** task 0 as test 13; task 1 as test 1 (no miss reported, start times and workloads in range)

---

### Test 15 — `TimerBasic` (`TH_BASIC`)
**Tasks:** 0 (driver)

The driver runs 20 jobs (`10 / 8 / 0`) and calls `Process()` in every job. In job 0 it starts timer 0 as a 35 ms
one-shot and timer 1 as a 25 ms periodic timer with period 50 ms. Expected: timer 0 expires once (deadline
`issued + 35`), timer 1 expires 4 times (deadlines `issued + 25 + 50 * k`, `k = 0..3`). Every callback runs inline
from `Process()`, no job exceeds 4 ms, no deadline miss.

**Pass condition:** counts 1 / 4, every expiration within `[-2, period + 10]` ms of its deadline, all callbacks
inline, driver schedule on the grid, no miss, no hard fault

---

### Test 16 — `TimerCrossTask` (`TH_CROSS_TASK`)
**Tasks:** 0 (driver), 1 (controller)

The driver (`10 / 8 / 0`, 20 jobs) only calls `Process()`. The controller (`50 / 20 / 5`, 4 jobs) issues timer
commands from its own HRT job:

| Controller job | Command |
|----------------|---------|
| 0 | `Start(timer 0, 40)` one-shot, `Start(timer 1, 20, 20)` periodic |
| 1 | `Stop(timer 1)` |
| 2 | `Restart(timer 0, 33)` one-shot |
| 3 | none |

Commands take effect at the next driver job, before its deadline scan. Expected: timer 0 expires twice (deadlines
`issued + 40` and `restart + 33`), timer 1 expires twice (`issued + 20` and `issued + 40`) and not a third time,
because `Stop()` is applied before the deadline `issued + 60` is scanned.

**Pass condition:** counts 2 / 2, expirations within `[-2, period + 10]` ms of their deadlines, all callbacks
inline, no miss (driver and controller), no hard fault

---

### Test 17 — `TimerBudget` (`TH_CALLBACK_BUDGET`)
**Tasks:** 0 (driver)

Two timers expire in the same job and each callback does 5 ms of work (`20 / 18 / 0`, 10 jobs, both timers started
with delay 45 and period 40). The job which runs both callbacks lasts about 10 ms and still fits the 18 ms
deadline: the worst-case time of all callbacks expiring in one call is part of the job WCET.

**Pass condition:** each timer expires 4 times (deadlines `issued + 45 + 40 * k`), exactly 4 jobs ran callbacks
(duration `>= 9` ms), all jobs within the deadline, no miss, no hard fault

---

### Test 18 — `TimerOverrun` (`TH_CALLBACK_OVERRUN`)
**Tasks:** 0 (driver)

A callback of 30 ms against a job deadline of 12 ms (`40 / 12 / 0`, 6 jobs, `expect_miss`). Timer 0 is a 25 ms
one-shot and expires in job 1. The callback exceeds the job deadline, so the driver is reported as missing it
(first duration `deadline + 1 = 13`) and a hard fault is raised and survived through `HrtOverrider`. The overrunning
job still ends before the next period, so the following jobs stay on the grid.

**Pass condition:** timer expires once, exactly 1 job lasted `>= 28` ms, `g_MissCount > 0` and
`g_MissFirst == 13` for the driver, at least one hard fault, all driver jobs on the grid

---

## Summary Table

| # | Test | Tasks | Pass condition | What it verifies |
|---|------|-------|----------------|------------------|
| 1 | `Basic` | 0–2 | all jobs, grid, workload, no miss | Staggered start delays, varying workload |
| 2 | `SingleTask` | 0 | as 1 | Single task, sleep trap entered every period |
| 3 | `ZeroWork` | 0 | as 1 | Job that completes immediately |
| 4 | `Simultaneous` | 0–2 | as 1 | Deadline covers waiting time for the CPU |
| 5 | `TightDeadline` | 0–1 | as 1 | Deadline only 8 ms above workload |
| 6 | `LongStartDelay` | 0–1 | as 1 | Start delay longer than the task's period |
| 7 | `DifferentPeriods` | 0–2 | as 1 | Different periodicities side by side |
| 8 | `NoDrift` | 0 | as 1 | 20 periods without accumulated drift |
| 9 | `StaggeredExit` | 0–2 | as 1 | Tasks exit after 1 / 3 / 6 jobs, others keep schedule |
| 10 | `ExitWithoutYield` | 0–1 | as 1 | Last job ends by returning from `Run()` |
| 11 | `ImmediateExit` | 0–1 | task 0 no jobs, task 1 as 1 | Exit without running a single job |
| 12 | `FullPool` | 0–3 | as 1 | All kernel task slots used |
| 13 | `DeadlineMiss` | 0 | miss reported, first duration `deadline + 1`, hard fault | Strict miss detection, `OnDeadlineMissed()` + `OnHardFault()` |
| 14 | `MissIsolated` | 0–1 | task 0 as 13, task 1 healthy | Miss isolated from a healthy task |
| 15 | `TimerBasic` | 0 | counts 1 / 4, expirations within one job period of their deadline, inline | `TimerHost` polled mode: one-shot and periodic timers driven by an HRT job |
| 16 | `TimerCrossTask` | 0–1 | counts 2 / 2, correct timing, `Stop()` effective | Commands from another HRT task applied by the next job |
| 17 | `TimerBudget` | 0 | counts 4 / 4, 4 long jobs within the deadline, no miss | Callbacks of timers expiring in one job fit the job deadline |
| 18 | `TimerOverrun` | 0 | miss reported (first duration `deadline + 1`), hard fault, grid kept | Callback overrun is a deadline miss, schedule not disturbed |
