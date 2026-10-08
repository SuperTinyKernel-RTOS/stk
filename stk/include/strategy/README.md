# STK Scheduling Strategies

**Path:** `includes/stk/strategy/`

Each strategy is a standalone header-only class that implements the `ITaskSwitchStrategy` interface. The kernel is templated on the strategy type, so you select a scheduling policy at compile time with zero runtime overhead.

---

## Strategy Overview

| File                        | Class                                          | Alias                                                  | Availability  |
|-----------------------------|------------------------------------------------|--------------------------------------------------------|---------------|
| `stk_strategy_rrobin.h`     | `SwitchStrategyRoundRobin`                     | `SwitchStrategyRR` / `SwitchStrategyRR_NSY` ¹          | ✅ Free (MIT)  |
| `stk_strategy_swrrobin.h`   | `SwitchStrategySmoothWeightedRoundRobin`       | `SwitchStrategySWRR` / `SwitchStrategySWRR_NSY` ¹      | ✅ Free (MIT)  |
| `stk_strategy_fpriority.h`  | `SwitchStrategyFixedPriority<N>`               | `SwitchStrategyFP32` / `SwitchStrategyFP32_NSY` ¹      | ✅ Free (MIT)  |
| `stk_strategy_pthreshold.h` | `SwitchStrategyPreemptionThreshold<N>`         | `SwitchStrategyPT32`                                   | ✅ Free (MIT)  |
| `stk_strategy_edf.h`        | `SwitchStrategyEDF`                            | —                                                      | ✅ Free (MIT)  |
| `stk_strategy_monotonic.h`  | `SwitchStrategyMonotonic<Type>`                | `SwitchStrategyRM` / `SwitchStrategyDM`                | ✅ Free (MIT)  |
| `stk_strategy_mcas.h`       | `SwitchStrategyMCAS`                           | —                                                      | 🔒 Commercial |
| `stk_strategy_mcas4.h`      | `SwitchStrategyMCAS4`                          | —                                                      | 🔒 Commercial |

¹ `_NSY` = **N**o-**S**leep **Y**ield variant: `Yield()` keeps the task runnable instead of putting it to sleep for one tick. The long form of the Round-Robin variant is `SwitchStrategyRoundRobinNoSleepYield`. See [Yield Behaviour](#yield-behaviour).

---

## Free Strategies

### Round-Robin (`SwitchStrategyRR`)
**File:** `stk_strategy_rrobin.h`

The simplest fair scheduling policy. Each runnable task is given one time slice (one kernel tick) in turn, cycling continuously. Tasks that call `Sleep()` are moved to a separate sleep list and skipped until they wake.

**Algorithm:** An intrusive circular list with a single cursor (`m_prev`). `GetNext()` advances the cursor one step — O(1) per tick. When the task under the cursor leaves the runnable set (sleep/removal) the cursor is repositioned so the rotation continues uninterrupted; removing any other task leaves the rotation untouched.

**No-sleep yield (`SwitchStrategyRR_NSY`):** `Yield()` keeps the task runnable and the cursor is set to the yielding task, so the next tick goes to its successor (or back to the task itself if it is the only runnable one).

**Capability flags:** `WEIGHT_API = 0`, `SLEEP_EVENT_API = 1`, `NOSLEEP_YIELD_API = 0` (`1` for `_NSY`)

**Best for:**
- Simple systems where all tasks are considered equal.
- Prototyping, early bring-up, and systems with no real-time deadline requirements.
- Small, fixed task counts.

---

### Smooth Weighted Round-Robin (`SwitchStrategySWRR`)
**File:** `stk_strategy_swrrobin.h`

Proportional CPU distribution with burst-free interleaving. A task with weight 3 receives three times as much CPU time as one with weight 1, but the extra slices are spread evenly across time rather than delivered in a block.

**Algorithm:** Each task carries a static weight and a dynamic *current weight*. Per `GetNext()` call: (1) increment every runnable task's current weight by its static weight; (2) select the task with the highest current weight; (3) deduct the total runnable weight sum from the winner. O(n) per tick. On wake-up, the task receives a boost (`current_weight = total_weight`) so it is scheduled on the very next tick, preventing I/O-bound starvation.

**No-sleep yield (`SwitchStrategySWRR_NSY`):** `Yield()` keeps the task runnable. The next selection excludes the yielding task (one-shot), unless it is the only runnable task. It still accrues its static weight in that step, so the proportional accounting is preserved.

**Capability flags:** `WEIGHT_API = 1`, `SLEEP_EVENT_API = 1`, `NOSLEEP_YIELD_API = 0` (`1` for `_NSY`)

**Best for:**
- Mixed workloads with meaningfully different CPU demands.
- Systems that need proportional fairness without bursty scheduling artefacts.
- Applications upgrading from plain Round-Robin that need finer CPU share control.

---

### Fixed-Priority (`SwitchStrategyFP32`)
**File:** `stk_strategy_fpriority.h`

Classic fixed-priority preemptive scheduling. Tasks are assigned a numeric priority (0 = lowest, 31 = highest for the `FP32` alias). The highest-priority runnable task always executes. Equal-priority tasks share the CPU in round-robin.

**Algorithm:** A 32-bit ready bitmap where bit *i* is set whenever priority level *i* has at least one runnable task. `GetNext()` finds the highest set bit in O(1) via `__builtin_clz`, then advances the per-level round-robin cursor. The template parameter `MAX_PRIORITIES` can be 1–32.

**No-sleep yield (`SwitchStrategyFP32_NSY`):** `Yield()` keeps the task runnable and hands the CPU to the next task at the **same** priority level (or continues if the task is alone at its level). A yield never hands the CPU to a lower priority level, unlike the legacy sleep-based yield.

**Capability flags:** `WEIGHT_API = 1` (weight = priority level), `SLEEP_EVENT_API = 1`, `NOSLEEP_YIELD_API = 0` (`1` for `_NSY`)

**Best for:**
- Systems with clearly defined task importance tiers.
- Hard real-time workloads where specific tasks must always take precedence.
- Developers migrating from FreeRTOS, Zephyr, or ThreadX.

---

### Preemption-Threshold (`SwitchStrategyPT32`)
**File:** `stk_strategy_pthreshold.h`

Fixed-priority scheduling with ThreadX-style preemption-threshold. Each task has a priority *P* and a threshold *T* ≥ *P*; while it runs it can be preempted only by tasks whose priority is strictly greater than *T*. With *T* = *P* a task behaves exactly like a normal fixed-priority task. Priorities follow the STK convention (higher number = higher priority); use `FromThreadX()` to convert ThreadX (priority, threshold) pairs.

**Algorithm:** Same per-level round-robin structure as Fixed-Priority (ready bitmap, O(1) level selection) plus a *held* bitmap. A task becomes *held* when it is selected while *T* > *P*. A held task keeps (or resumes) the CPU as long as no ready task is above its threshold, and is never time-sliced. The hold ends when the task sleeps, is removed, yields, or its threshold is lowered to its priority. Priority and threshold are packed into the task weight as `(priority << 8) | threshold` (`MakeWeight()` / `FromThreadX()`). Priority inheritance raises the effective priority only; the effective threshold is `max(configured threshold, current priority)`. The threshold can be changed at run-time with `ChangeThreshold()` (equivalent of `tx_thread_preemption_change`). `Yield()` is always handled by the strategy (`tx_thread_relinquish` semantics): the hold is released and the CPU goes to the next task of the same priority.

**Capability flags:** `WEIGHT_API = 1` (weight = encoded priority/threshold), `SLEEP_EVENT_API = 1`, `PRIORITY_INHERITANCE_API = 1`, `NOSLEEP_YIELD_API = 1`

**Differences from ThreadX:** STK priority numbering, at most 32 priority levels, tick-based time slices (1 tick) with no per-task time-slice length.

**Best for:**
- Protecting short sequences of cooperating tasks from preemption by mid-priority tasks without using mutexes, and reducing needless context switches.
- Migrating ThreadX applications that rely on preemption-threshold.

---

### Earliest Deadline First (`SwitchStrategyEDF`)
**File:** `stk_strategy_edf.h`

Dynamic scheduling that always runs the task closest to missing its deadline. EDF is provably optimal for single-processor systems — if a feasible schedule exists, EDF will find it.

**Algorithm:** O(n) linear scan over runnable tasks each tick, comparing `GetHrtRelativeDeadline()` (= deadline − elapsed duration). The task with the minimum value wins, ties broken by insertion order. No per-task state is maintained between ticks. Requires `KERNEL_HRT` mode.

**Capability flags:** `WEIGHT_API = 0`, `SLEEP_EVENT_API = 1`

**Best for:**
- Hard real-time systems where deadlines vary and fixed-priority assignment is difficult.
- Systems that need to push processor utilisation close to 100%.
- Tasks with similar priorities but differing deadlines.

---

### Rate-Monotonic & Deadline-Monotonic (`SwitchStrategyRM` / `SwitchStrategyDM`)
**File:** `stk_strategy_monotonic.h`

Two classic static-priority real-time algorithms in a single templated class:

- **Rate-Monotonic (RM)** — shorter period → higher fixed priority. Optimal among all fixed-priority policies for independent periodic tasks.
- **Deadline-Monotonic (DM)** — shorter deadline → higher fixed priority. Generalises RM, optimal when deadlines ≤ periods.

**Algorithm:** `AddTask()` performs an O(n) insertion sort by `GetHrtPeriodicity()` (RM) or `GetHrtDeadline()` (DM). `GetNext()` simply returns the first non-sleeping task from the sorted list. Sleeping tasks stay in-place and are skipped inline — no separate sleep list. Includes a `SchedulabilityCheck::IsSchedulableWCRT()` companion utility for offline WCRT analysis. Requires `KERNEL_HRT` mode.

**Capability flags:** `WEIGHT_API = 0`, `SLEEP_EVENT_API = 0`

**Best for:**
- Strictly periodic hard real-time workloads (control loops, sensor sampling, comms protocols).
- Systems requiring formal schedulability guarantees and offline WCRT analysis.
- Safety-critical applications where static, certified priority assignment is mandatory.

---

## Commercial Strategies

The following strategies are **not included** in the open-source repository. They are available to commercial licensees only.

For licensing enquiries contact: **[contact@supertinykernel.org](mailto:contact@supertinykernel.org)**

---

### Mixed-Criticality Adaptive Scheduler — 2-level (`SwitchStrategyMCAS`)
**File:** `stk_strategy_mcas.h` | 🔒 **Commercial License**

A two-level mixed-criticality scheduler combining SWRR fairness within each criticality group with automatic escalation to a protected safety mode on budget overrun. Tasks are tagged LO or HI in the upper byte of their weight value. In normal mode both groups share CPU at a configurable ratio (default 60 % HI / 40 % LO) via token-bucket interleaving. When a HI task overruns its budget for more than a configurable streak of consecutive ticks, the scheduler escalates: LO tasks are suspended in-place (no list mutation) and only HI tasks run until a cooldown period passes.

**Capability flags:** `WEIGHT_API = 1`, `SLEEP_EVENT_API = 1`, `DEADLINE_MISSED_API = 1`, `NOSLEEP_YIELD_API = 0`

**Best for:** Systems mixing safety-critical and best-effort tasks — automotive ECUs, industrial controllers, medical devices — that need guaranteed CPU isolation for critical work under overload.

---

### Mixed-Criticality Adaptive Scheduler — 4-level with Elastic Shares (`SwitchStrategyMCAS4`)
**File:** `stk_strategy_mcas4.h` | 🔒 **Commercial License**

A generalisation of MCAS to four criticality levels (CRIT_0–CRIT_3) with cascade escalation, cascade recovery, and per-group elastic share adaptation driven by an EWMA execution-pressure estimator. Overruns at level K raise the mode to K (not unconditionally to the top); recovery decrements one level at a time with per-level cooldown counters. Under load, a group can borrow token-percent from its immediate lower-criticality neighbour, increasing effective CPU share without altering WCRT guarantees. Includes a `SchedulabilityCheckMCAS4` companion for offline WCRT analysis.

**Capability flags:** `WEIGHT_API = 1`, `SLEEP_EVENT_API = 1`, `DEADLINE_MISSED_API = 1`, `NOSLEEP_YIELD_API = 0`

**Best for:** Multi-function domain controllers, high-integrity industrial automation, and platform software hosting both safety and non-safety partitions — wherever more than two criticality tiers are needed or static share allocation is too conservative.

---

## Choosing a Strategy
```
Is CPU time equal for all tasks?
├── Yes → SwitchStrategyRR
└── No
    ├── Static weights, proportional fairness  → SwitchStrategySWRR
    ├── Hard priorities, classic RTOS model    → SwitchStrategyFP32
    ├── Hard priorities + preemption-threshold → SwitchStrategyPT32
    ├── Periodic tasks, formal guarantees      → SwitchStrategyRM / SwitchStrategyDM
    ├── Dynamic deadlines, max utilisation     → SwitchStrategyEDF
    ├── Mixed-criticality (2-level)            → SwitchStrategyMCAS   [commercial]
    └── Mixed-criticality (4-level)            → SwitchStrategyMCAS4  [commercial]
```

---

## Yield Behaviour

`Yield()` behaves differently depending on the kernel mode and the strategy (`NOSLEEP_YIELD_API`):

| Mode / strategy                                  | Behaviour of `Yield()`                                                                                       |
|--------------------------------------------------|--------------------------------------------------------------------------------------------------------------|
| Non-HRT, `NOSLEEP_YIELD_API = 0` (default)       | Legacy yield: the task sleeps for one tick (`YIELD_TICKS`); lower-priority tasks may run during that tick.   |
| Non-HRT, `MCAS`, `MCAS4`                         | Legacy yield (`NOSLEEP_YIELD_API = 0`).                                                                      |
| Non-HRT, `RR_NSY`                                | Task stays runnable; the CPU goes to the next task in rotation.                                              |
| Non-HRT, `SWRR_NSY`                              | Task stays runnable; it is skipped in the next selection (unless it is the only runnable task).              |
| Non-HRT, `FP32_NSY`                              | Task stays runnable; the CPU goes to the next task of the **same** priority only.                            |
| Non-HRT, `PT32`                                  | Always handled by the strategy; releases the task's threshold hold, then as `FP32_NSY`.                      |
| HRT (`EDF`, `RM`, `DM`)                          | `Yield()` signals completion of the current job: the task sleeps until its next period. Flag is ignored.     |

If a strategy declines a yield (for example the task is not in its runnable set), the kernel falls back to the legacy one-tick sleep.

---

## Capability Flags Reference

| Flag              | Value | Meaning                                                                |
|-------------------|-------|------------------------------------------------------------------------|
| `WEIGHT_API`      | `0`   | Strategy ignores `GetWeight()` / `SetCurrentWeight()`                  |
| `WEIGHT_API`      | `1`   | Strategy uses `GetWeight()` for priority or SWRR weighting             |
| `SLEEP_EVENT_API` | `0`   | Strategy does **not** use `OnTaskSleep()` / `OnTaskWake()`             |
| `SLEEP_EVENT_API` | `1`   | Strategy **requires** `OnTaskSleep()` / `OnTaskWake()` from the kernel |
| `PRIORITY_INHERITANCE_API` | `0` | Strategy does not handle `OnTaskWeightChange()` events           |
| `PRIORITY_INHERITANCE_API` | `1` | Strategy handles `OnTaskWeightChange()` (priority inheritance, `SetWeight()`); requires `WEIGHT_API = 1` |
| `NOSLEEP_YIELD_API` | `0` | Kernel performs the legacy yield: the task sleeps for one tick       |
| `NOSLEEP_YIELD_API` | `1` | Strategy implements `OnTaskYield()`; the yielding task stays runnable (non-HRT modes only) |

---

## License

Free strategies are distributed under the **MIT License** — see `LICENSE` at the repository root.

Commercial strategies (`stk_strategy_mcas.h`, `stk_strategy_mcas4.h`) are subject to a separate commercial license agreement. Contact [contact@supertinykernel.org](mailto:contact@supertinykernel.org).