# FreeRTOS Wrapper (`interop/freertos`)

STK provides a complete **FreeRTOS** compatibility layer (`freertos_stk.cpp`) that maps the standard FreeRTOS C API onto the STK C++ kernel. This allows you to use STK as a drop-in RTOS backend in any project that targets the FreeRTOS interface, letting you migrate existing FreeRTOS codebases to STK with minimal or no application changes.

---

## Why Migrate a FreeRTOS Project to STK?

If you have an existing FreeRTOS codebase, this wrapper offers a compelling path to better performance, lower RAM usage, and access to a far richer kernel — with zero application-level changes required. Here is why teams choose STK through this wrapper.

### Higher Task Throughput

Measured on **STM32F407G-DISC1** (Cortex-M4 @ 168 MHz, GCC 14.2.1, April 2026), STK outperforms FreeRTOS across all task counts and compiler optimization levels:

| Scenario | STK Throughput | FreeRTOS Throughput | STK Advantage |
|---|---|---|---|
| 16 tasks, `-Ofast` | **993,008** | 966,017 | **+2.8%** |
| 8 tasks, `-Ofast` | **988,862** | 932,654 | **+6.0%** |
| 4 tasks, `-Ofast` | **989,465** | 881,082 | **+12.3%** |
| 16 tasks, `-Os` | **752,136** | 735,342 | **+2.3%** |
| 4 tasks, `-Os` | **753,459** | 673,845 | **+11.8%** |

*Score = CRC32 calculations completed within a fixed time window. Higher = more CPU time available for application work.*

At 16 tasks under `-Ofast`, STK delivers approximately **12% more total computational throughput** and **~17% lower scheduling jitter** than FreeRTOS. Throughput remains nearly flat as task count rises from 4 to 16, indicating near-zero context-switch friction at scale.

### Lower RAM Footprint

STK uses **~21% less RAM** than FreeRTOS in 4-task scenarios and maintains an ~8.5% RAM advantage at 16 tasks — without sacrificing any functionality.

| Scenario | STK RAM | FreeRTOS RAM |
|---|---|---|
| 4 tasks | **6.9 KB** | 8.8 KB |
| 8 tasks | **11.4 KB** | 13.3 KB |
| 16 tasks | **20.3 KB** | 22.2 KB |

### Composable, Brick-Block Kernel Design

FreeRTOS was designed as a complete, opinionated RTOS with a tightly coupled API. STK was designed differently: its kernel is a set of composable building blocks. Features are enabled selectively via compile-time flags — `KERNEL_SYNC` for synchronization primitives, `KERNEL_TICKLESS` for tickless mode, `KERNEL_HRT` for hard real-time guarantees. Code not in use is stripped by the compiler, keeping Flash and RAM usage minimal.

This means once you migrate your project via this wrapper, you can incrementally unlock STK-native capabilities — advanced scheduling, hard real-time deadlines, memory protection — without touching your existing application code.

### Advanced Scheduling Strategies

FreeRTOS implements a single scheduling policy: fixed-priority preemptive with round-robin within each level. STK is the only known RTOS that implements all major scheduling strategies:

| Strategy | Description |
|---|---|
| `SwitchStrategyRoundRobin` | Equal time-slicing, 100% CPU utilization |
| `SwitchStrategySWRR` | Smooth Weighted Round-Robin for proportional CPU allocation |
| `SwitchStrategyFixedPriority` | FreeRTOS-equivalent, used by this wrapper |
| `SwitchStrategyRM` | Rate-Monotonic for hard real-time HRT tasks |
| `SwitchStrategyDM` | Deadline-Monotonic for hard real-time HRT tasks |
| `SwitchStrategyEDF` | Earliest-Deadline-First, provably optimal for single-processor |
| `SwitchStrategyMCAS` 🔒 | Mixed-Criticality Adaptive Scheduler (2-level) |
| `SwitchStrategyMCAS4` 🔒 | Mixed-Criticality Adaptive Scheduler (4-level, EWMA-based) |
| Custom | Plug in your own via `ITaskSwitchStrategy` |

The wrapper uses `SwitchStrategyFP32` to exactly replicate FreeRTOS scheduling semantics. When you are ready, switching to EDF, SWRR, or a mixed-criticality strategy requires changing a single template parameter.

### Hard Real-Time Mode

FreeRTOS is a soft real-time system — it provides no deadline enforcement. STK's `KERNEL_HRT` mode adds guaranteed execution windows, WCRT schedulability analysis, and a per-task `OnDeadlineMissed()` callback called with the exact overrun amount when a deadline is violated. For time-critical control loops, sensor fusion, or motor drive tasks, this turns runtime errors into deterministic, debuggable events.

### Memory Protection Unit (MPU) Support

STK supports explicit hardware privilege separation on Cortex-M3/M4/M7/M33 and newer cores. Tasks can be individually marked `ACCESS_PRIVILEGED` or `ACCESS_USER`, giving hardware-enforced isolation between driver code and application logic. FreeRTOS has limited MPU support that requires a separate `FreeRTOS-MPU` port; in STK it is a first-class feature of the standard kernel.

### Tickless Mode — First Class

STK's tickless mode (`KERNEL_TICKLESS`) dynamically programs the next interrupt based on the nearest upcoming event, eliminating unnecessary CPU wakeups. Enable it in this wrapper by defining `STK_TICKLESS_IDLE=1` — no other changes required.

### No Heap Allocation in the Kernel

STK's kernel itself performs zero dynamic heap allocation. All kernel objects — task control blocks, synchronization primitives, timer hosts — use static or caller-supplied storage. This makes STK appropriate for safety-critical systems operating under MISRA C++ Rule 18-4-1 and similar standards. The wrapper maps `pvPortMalloc` / `vPortFree` to STK's memory allocator, giving you full control over the allocation strategy.

### Thread-Local Storage Built Into the CPU Register

STK's native TLS uses a dedicated CPU register for zero-overhead per-task data access. The wrapper also supports FreeRTOS-style TLS pointer arrays (`configNUM_THREAD_LOCAL_STORAGE_POINTERS`) via per-task storage in the task control block, guarded by a scoped critical section for strict correctness on dual-core targets.

### SEGGER SystemView Integration

STK's scheduler is fully traceable with SEGGER SystemView. Existing FreeRTOS projects that use SystemView can continue using it — STK's tracing infrastructure is compatible and provides per-task visibility into scheduling decisions, context switches, and sleep/wake events.

### Development and Testing Without Hardware

STK includes a full scheduling emulator for Windows (x86 development mode). Run the same threaded application on a PC, debug tasks in Visual Studio or Eclipse, and unit-test without a physical target — something FreeRTOS does not offer natively.

### 100% Test Coverage and Automated QEMU CI

Every line of STK scheduler logic is covered by unit tests, with all commits automatically tested under QEMU for both Cortex-M and RISC-V targets. This level of verification is documented and publicly visible.

### MIT License — No Strings Attached

STK is released under the MIT license. It may be used freely in commercial, closed-source, and safety-regulated products. Professional services, IP indemnification, and safety-certification assistance (IEC 61508, ISO 26262, DO-178C) are available separately.

---

## Supported API Groups

The following table documents every FreeRTOS API function and whether it is **fully implemented**, **partially implemented** (with a noted limitation), or a **stub** (link-compatible but functionally limited).

### Coverage Summary

| API Group | Implemented | Partial / Stub | Total | Coverage |
|---|---|---|---|---|
| Heap / Port | 5 | 0 | 5 | **100%** |
| Kernel Control | 10 | 0 | 10 | **100%** |
| Task Management | 18 | 3 | 21 | **86%** |
| Queue | 17 | 0 | 17 | **100%** |
| Queue Sets | 5 | 0 | 5 | **100%** |
| Semaphore / Mutex | 14 | 0 | 14 | **100%** |
| Software Timers | 18 | 0 | 18 | **100%** |
| Event Groups | 8 | 0 | 8 | **100%** |
| Task Notifications | 15 | 0 | 15 | **100%** |
| Thread-Local Storage | 2 | 0 | 2 | **100%** |
| Stream Buffers | 13 | 0 | 13 | **100%** |
| Message Buffers | 11 | 0 | 11 | **100%** |
| **Total** | **137** | **2** | **139** | **~99%** |

### Heap / Port API

| Function | Status | Notes |
|---|---|---|
| `pvPortMalloc` | ✅ Full | Delegates to `stk::memory::MemoryAllocator::Allocate`; `__stk_weak` override point |
| `vPortFree` | ✅ Full | Delegates to `stk::memory::MemoryAllocator::Free`; `__stk_weak` override point |
| `xPortGetFreeHeapSize` | ✅ Full | Reads from internal `s_MemStats` accounting |
| `xPortGetMinimumEverFreeHeapSize` | ✅ Full | Watermark maintained inside `Allocate()` |
| `vPortGetHeapStats` | ✅ Full | All `HeapStats_t` fields populated; free-block subdivision fields conservatively reported as `0`/`1` because the allocator delegates to `malloc` (no block-list traversal) |

### Kernel Control API

| Function | Status | Notes |
|---|---|---|
| `vTaskStartScheduler` | ✅ Full | Instantiates `FrtosKernel` with `KERNEL_DYNAMIC | KERNEL_SYNC`, `SwitchStrategyFP32`, tickless if `STK_TICKLESS_IDLE=1` |
| `vTaskEndScheduler` | ✅ Full | Enumerates all active tasks via `EnumerateTasksT`, calls `ScheduleTaskRemoval` on each, then calls `stk::Yield()` to let the scheduler drain them. `g_StkKernel.Start()` returns once all `KERNEL_DYNAMIC` tasks have exited |
| `vTaskSuspendAll` | ✅ Full | Maps to `stk::hw::CriticalSection::Enter()` |
| `xTaskResumeAll` | ✅ Full | Maps to `stk::hw::CriticalSection::Exit()`; always returns `pdFALSE` — no wrapper-level pending-switch tracking |
| `xTaskGetTickCount` | ✅ Full | |
| `xTaskGetTickCountFromISR` | ✅ Full | |
| `uxTaskGetNumberOfTasks` | ✅ Full | |
| `xTaskGetSchedulerState` | ✅ Full | Returns `taskSCHEDULER_RUNNING`, `taskSCHEDULER_SUSPENDED`, or `taskSCHEDULER_NOT_STARTED` |
| `vPortEnterCritical` | ✅ Full | `stk::hw::CriticalSection::Enter()` |
| `vPortExitCritical` | ✅ Full | `stk::hw::CriticalSection::Exit()` |

### Task Management API

| Function | Status | Notes |
|---|---|---|
| `xTaskCreate` | ✅ Full | |
| `xTaskCreateStatic` | ✅ Full | Accepts caller-supplied TCB and stack buffers; falls back to heap when `NULL` is passed |
| `xTaskCreateRestricted` | ✅ Full | MPU region parameters accepted and passed through; privilege enforced via `ACCESS_PRIVILEGED` / `ACCESS_USER` |
| `xTaskCreateRestrictedStatic` | ✅ Full | Static variant of the above |
| `vTaskDelete` | ✅ Full | |
| `vTaskSuspend` | ✅ Full | |
| `vTaskResume` | ✅ Full | |
| `xTaskResumeFromISR` | ✅ Full | Always returns `pdFALSE`; STK handles preemption internally |
| `xTaskAbortDelay` | ✅ Full | |
| `vTaskDelay` | ✅ Full | |
| `vTaskDelayUntil` | ✅ Full | |
| `xTaskDelayUntil` | ✅ Full | |
| `vTaskPrioritySet` | ✅ Full | |
| `uxTaskPriorityGet` | ✅ Full | |
| `uxTaskPriorityGetFromISR` | ✅ Full | |
| `eTaskGetState` | ✅ Full | Returns `eRunning`, `eReady`, `eBlocked`, `eSuspended`, `eDeleted` |
| `xTaskGetCurrentTaskHandle` | ✅ Full | |
| `xTaskGetHandle` | ✅ Full | Linear scan over active task table by name |
| `pcTaskGetName` | ✅ Full | |
| `uxTaskGetStackHighWaterMark` | ✅ Full | Counts untouched `stk::Word` slots filled with `STK_STACK_MEMORY_FILLER` |
| `uxTaskGetStackHighWaterMark2` | ✅ Full | Same as above; `configSTACK_DEPTH_TYPE` return variant |
| `uxTaskGetSystemState` | ✅ Full | Populates `TaskStatus_t` array; `ulRunTimeCounter` and `ulRunTimeStamps` are always `0` (see `vTaskGetRunTimeStats`) |
| `vTaskList` | ✅ Full | Produces standard FreeRTOS column format (`Name / State / Prio / Stack / Num`) |
| `vTaskGetRunTimeStats` | ⚠️ Stub | Produces correct table format with task names; `Abs Time` and `% Time` columns are always `0` — STK has no per-task CPU run-time accumulator. Provided for link compatibility with middleware and diagnostic tools |
| `vTaskSetThreadLocalStoragePointer` | ✅ Full | Up to `configNUM_THREAD_LOCAL_STORAGE_POINTERS` slots per task |
| `pvTaskGetThreadLocalStoragePointer` | ✅ Full | |

### Queue API

| Function | Status | Notes |
|---|---|---|
| `xQueueCreate` | ✅ Full | Backed by `stk::sync::MessageQueue` |
| `xQueueCreateStatic` | ✅ Full | Accepts caller-supplied storage; falls back to heap when `NULL` |
| `vQueueDelete` | ✅ Full | |
| `xQueueSend` | ✅ Full | Alias for `xQueueSendToBack` |
| `xQueueSendToBack` | ✅ Full | |
| `xQueueSendToFront` | ✅ Full | Backed by `stk::sync::MessageQueue::PutFront()` — inserts the message at the head of the ring buffer so it is returned first by the next `Get()`. Blocking with configurable timeout |
| `xQueueReceive` | ✅ Full | |
| `xQueuePeek` | ✅ Full | Backed by `stk::sync::MessageQueue::Peek()` — copies the oldest message atomically without consuming it. Blocking with configurable timeout. Safe under concurrent consumers |
| `xQueuePeekFromISR` | ✅ Full | Backed by `TryPeek()` (= `Peek(NO_WAIT)`) — ISR-safe non-destructive read |
| `xQueueReset` | ✅ Full | |
| `xQueueOverwrite` | ✅ Full | Drops the oldest item if full, then sends |
| `xQueueOverwriteFromISR` | ✅ Full | ISR-safe variant |
| `uxQueueMessagesWaiting` | ✅ Full | |
| `uxQueueMessagesWaitingFromISR` | ✅ Full | |
| `uxQueueSpacesAvailable` | ✅ Full | |
| `xQueueSendFromISR` | ✅ Full | |
| `xQueueSendToBackFromISR` | ✅ Full | |
| `xQueueSendToFrontFromISR` | ✅ Full | Backed by `TryPutFront()` (= `PutFront(NO_WAIT)`) — ISR-safe non-blocking head insert. `pxHigherPriorityTaskWoken` always `pdFALSE` |
| `xQueueReceiveFromISR` | ✅ Full | |
| `xQueueIsQueueEmptyFromISR` | ✅ Full | |
| `xQueueIsQueueFullFromISR` | ✅ Full | |
| `xQueueGetMutexHolder` | ✅ Full | |
| `xQueueGetMutexHolderFromISR` | ✅ Full | |

### Queue Set API *(requires `configUSE_QUEUE_SETS=1`)*

| Function | Status | Notes |
|---|---|---|
| `xQueueCreateSet` | ✅ Full | Backed by a dedicated `stk::sync::MessageQueue` token FIFO |
| `xQueueAddToSet` | ✅ Full | Supports queues and binary/counting semaphores; mutexes rejected per FreeRTOS API contract |
| `xQueueRemoveFromSet` | ✅ Full | |
| `xQueueSelectFromSet` | ✅ Full | Blocking with configurable timeout |
| `xQueueSelectFromSetFromISR` | ✅ Full | Non-blocking `TryGet` |

### Semaphore / Mutex API

| Function | Status | Notes |
|---|---|---|
| `xSemaphoreCreateBinary` | ✅ Full | Backed by `stk::sync::Semaphore` (max=1, initial=0) |
| `xSemaphoreCreateBinaryStatic` | ✅ Full | |
| `xSemaphoreCreateCounting` | ✅ Full | Backed by `stk::sync::Semaphore` |
| `xSemaphoreCreateCountingStatic` | ✅ Full | |
| `xSemaphoreCreateMutex` | ✅ Full | Backed by `stk::sync::Mutex`; STK mutex is always recursive |
| `xSemaphoreCreateMutexStatic` | ✅ Full | |
| `xSemaphoreCreateRecursiveMutex` | ✅ Full | Equivalent to `xSemaphoreCreateMutex` — STK mutex is natively recursive |
| `xSemaphoreCreateRecursiveMutexStatic` | ✅ Full | |
| `vSemaphoreDelete` | ✅ Full | |
| `xSemaphoreTake` | ✅ Full | |
| `xSemaphoreTakeFromISR` | ✅ Full | |
| `xSemaphoreTakeRecursive` | ✅ Full | |
| `xSemaphoreGive` | ✅ Full | |
| `xSemaphoreGiveRecursive` | ✅ Full | |
| `xSemaphoreGiveFromISR` | ✅ Full | `pxHigherPriorityTaskWoken` always set to `pdFALSE` |
| `uxSemaphoreGetCount` | ✅ Full | |
| `xSemaphoreGetMutexHolder` | ✅ Full | |
| `xSemaphoreGetMutexHolderFromISR` | ✅ Full | |

### Software Timer API

| Function | Status | Notes |
|---|---|---|
| `xTimerCreate` | ✅ Full | Backed by `stk::time::TimerHost`; host object created lazily on first call using static storage |
| `xTimerCreateStatic` | ✅ Full | |
| `xTimerDelete` | ✅ Full | `xTicksToWait` accepted for API compatibility but ignored — timer commands are synchronous |
| `xTimerStart` | ✅ Full | `xTicksToWait` ignored |
| `xTimerStop` | ✅ Full | `xTicksToWait` ignored |
| `xTimerReset` | ✅ Full | `xTicksToWait` ignored |
| `xTimerChangePeriod` | ✅ Full | `xTicksToWait` ignored |
| `xTimerStartFromISR` | ✅ Full | `pxHigherPriorityTaskWoken` is not required by STK — preemption is handled internally by the scheduler. Always set to `pdFALSE`; `portYIELD_FROM_ISR(pdFALSE)` is a no-op |
| `xTimerStopFromISR` | ✅ Full | Same note on `pxHigherPriorityTaskWoken` |
| `xTimerResetFromISR` | ✅ Full | Same note on `pxHigherPriorityTaskWoken` |
| `xTimerChangePeriodFromISR` | ✅ Full | Same note on `pxHigherPriorityTaskWoken` |
| `xTimerPendFunctionCall` | ✅ Full | Executes deferred function via the timer task context |
| `xTimerPendFunctionCallFromISR` | ✅ Full | ISR-safe variant |
| `xTimerIsTimerActive` | ✅ Full | |
| `pvTimerGetTimerID` | ✅ Full | |
| `vTimerSetTimerID` | ✅ Full | |
| `pcTimerGetName` | ✅ Full | |
| `xTimerGetPeriod` | ✅ Full | |
| `xTimerGetExpiryTime` | ✅ Full | |

### Event Group API

| Function | Status | Notes |
|---|---|---|
| `xEventGroupCreate` | ✅ Full | Backed by `stk::sync::EventFlags` (32-bit, bits 0–30 available; bit 31 reserved) |
| `xEventGroupCreateStatic` | ✅ Full | |
| `vEventGroupDelete` | ✅ Full | |
| `xEventGroupSetBits` | ✅ Full | |
| `xEventGroupClearBits` | ✅ Full | |
| `xEventGroupGetBits` | ✅ Full | |
| `xEventGroupWaitBits` | ✅ Full | Supports `ANY`/`ALL`/`NO_CLEAR` wait options |
| `xEventGroupSetBitsFromISR` | ✅ Full | `pxHigherPriorityTaskWoken` always `pdFALSE` |
| `xEventGroupClearBitsFromISR` | ✅ Full | |
| `xEventGroupSync` | ✅ Full | |

### Task Notification API

| Function | Status | Notes |
|---|---|---|
| `xTaskNotifyGive` | ✅ Full | Index-0 alias |
| `xTaskNotifyGiveIndexed` | ✅ Full | Only index 0 is functional |
| `ulTaskNotifyTake` | ✅ Full | Uses per-task `stk::sync::Semaphore` for efficient blocking |
| `ulTaskNotifyTakeIndexed` | ✅ Full | Only index 0 is functional |
| `xTaskNotify` | ✅ Full | |
| `xTaskNotifyIndexed` | ✅ Full | Only index 0 is functional |
| `xTaskNotifyWait` | ✅ Full | |
| `xTaskNotifyWaitIndexed` | ✅ Full | Only index 0 is functional |
| `xTaskNotifyFromISR` | ✅ Full | `pxHigherPriorityTaskWoken` always `pdFALSE` |
| `xTaskNotifyFromISRIndexed` | ✅ Full | Only index 0 is functional |
| `xTaskNotifyAndQuery` | ✅ Full | |
| `xTaskNotifyAndQueryIndexed` | ✅ Full | |
| `xTaskNotifyAndQueryFromISR` | ✅ Full | |
| `xTaskNotifyAndQueryFromISRIndexed` | ✅ Full | |
| `xTaskNotifyStateClear` | ✅ Full | |
| `xTaskNotifyStateClearIndexed` | ✅ Full | |
| `ulTaskNotifyValueClear` | ✅ Full | |
| `ulTaskNotifyValueClearIndexed` | ✅ Full | All indices up to `configTASK_NOTIFICATION_ARRAY_ENTRIES - 1` are supported; out-of-range indices return `0` |

### Stream Buffer API *(requires `configUSE_STREAM_BUFFERS=1`)*

Backed by `stk::sync::Pipe` (element size = 1 byte), which provides native `WriteBulk` / `ReadBulk` / `TryWriteBulk` / `TryReadBulk` for efficient multi-byte transfers. Trigger-level blocking reuses `Pipe`'s own condition-variable wait loop — no busy-spin, no lost-wakeup race.

| Function | Status |
|---|---|
| `xStreamBufferCreate` | ✅ Full |
| `xStreamBufferCreateStatic` | ✅ Full |
| `xStreamBufferCreateWithCallback` | ✅ Full |
| `xStreamBufferCreateStaticWithCallback` | ✅ Full |
| `vStreamBufferDelete` | ✅ Full |
| `xStreamBufferSend` | ✅ Full |
| `xStreamBufferSendFromISR` | ✅ Full |
| `xStreamBufferReceive` | ✅ Full |
| `xStreamBufferReceiveFromISR` | ✅ Full |
| `xStreamBufferBytesAvailable` | ✅ Full |
| `xStreamBufferSpacesAvailable` | ✅ Full |
| `xStreamBufferIsEmpty` | ✅ Full |
| `xStreamBufferIsFull` | ✅ Full |
| `xStreamBufferReset` | ✅ Full |
| `xStreamBufferResetFromISR` | ✅ Full |
| `xStreamBufferSetTriggerLevel` | ✅ Full |
| `xStreamBufferGetTriggerLevel` | ✅ Full |
| `xStreamBufferNextMessageLengthBytes` | ✅ Full |

### Message Buffer API *(requires `configUSE_STREAM_BUFFERS=1`)*

Backed by a `stk::sync::MessageQueue` envelope FIFO paired with a `stk::memory::BlockMemoryPool` for zero-copy payload management. `TryPeek()` provides non-destructive length inspection without a `Get` + `PutFront` workaround.

| Function | Status |
|---|---|
| `xMessageBufferCreate` | ✅ Full |
| `xMessageBufferCreateStatic` | ✅ Full |
| `xMessageBufferCreateWithCallback` | ✅ Full |
| `xMessageBufferCreateStaticWithCallback` | ✅ Full |
| `vMessageBufferDelete` | ✅ Full |
| `xMessageBufferSend` | ✅ Full |
| `xMessageBufferSendFromISR` | ✅ Full |
| `xMessageBufferReceive` | ✅ Full |
| `xMessageBufferReceiveFromISR` | ✅ Full |
| `xMessageBufferIsEmpty` | ✅ Full |
| `xMessageBufferIsFull` | ✅ Full |
| `xMessageBufferSpacesAvailable` | ✅ Full |
| `xMessageBufferReset` | ✅ Full |
| `xMessageBufferResetFromISR` | ✅ Full |
| `xMessageBufferNextLengthBytes` | ✅ Full |

---

## Design & Mapping

**Kernel configuration:** The wrapper instantiates one global `stk::Kernel` with `KERNEL_DYNAMIC | KERNEL_SYNC` flags and the `SwitchStrategyFP32` (32-level fixed-priority) scheduler. This exactly mirrors FreeRTOS scheduling semantics: the highest-priority ready task always runs immediately, with round-robin within each priority level. Tickless mode is enabled automatically when `STK_TICKLESS_IDLE=1` is defined.

**Task capacity:** Controlled by the `FREERTOS_STK_MAX_TASKS` macro (default: 16). Increase it if more concurrent tasks are needed.

**Priority mapping:** FreeRTOS priorities (`0` = idle/lowest … `configMAX_PRIORITIES-1` = highest) map directly to STK FP32 priority levels 0–31 via a 1:1 clamp (`stk_priority = clamp(freertos_priority, 0, 31)`). `configMAX_PRIORITIES` must not exceed 32 — a compile-time `static_assert` enforces this.

**Timeouts:** FreeRTOS tick values are passed through directly as STK ticks (1:1 when tick resolution is 1 ms / `PERIODICITY_DEFAULT`). `portMAX_DELAY` is translated to `stk::WAIT_INFINITE`; `0` is translated to `stk::NO_WAIT`.

**Queues:** Backed by `stk::sync::MessageQueue` — STK's native fixed-capacity, fixed-message-size ring-buffer with integrated blocking `Put`/`Get`, front-insert `PutFront`/`TryPutFront`, non-destructive `Peek`/`TryPeek`, ISR-safe `TryPut`/`TryGet`/`TryPutFront`/`TryPeek`, and configurable timeouts. `xQueueSendToFront` and `xQueueSendToFrontFromISR` use `PutFront`/`TryPutFront` and deliver true head-of-queue insertion. `xQueuePeek` and `xQueuePeekFromISR` use `Peek`/`TryPeek` for a fully atomic, non-destructive read.

**Semaphores:** Binary and counting semaphores are backed by `stk::sync::Semaphore`. Mutexes and recursive mutexes are backed by `stk::sync::Mutex` — STK's mutex is always recursive, so `xSemaphoreCreateMutex` and `xSemaphoreCreateRecursiveMutex` are equivalent.

**Event groups:** Backed by `stk::sync::EventFlags` — STK's native 32-bit multi-flag synchronization primitive with ISR-safe `Set`/`Clear` and `ANY`/`ALL`/`NO_CLEAR` wait options. FreeRTOS conventionally uses only bits 0–23; bits 0–30 are available (bit 31 is reserved by STK for error sentinels).

**Software timers:** Backed by `stk::time::TimerHost`. A single global `TimerHost` is created lazily on the first `xTimerCreate()` call using static storage (no heap for the host object itself). The `xTicksToWait` parameter on timer command functions is accepted for API compatibility but not used — STK timer operations are synchronous.

**Task notifications:** Modelled as a hybrid. Semaphore-based blocking (`ulTaskNotifyTake` / `xTaskNotifyGive`) uses the per-task `stk::sync::Semaphore` for efficient blocking. Bit-manipulation operations (`xTaskNotify` / `xTaskNotifyWait`) operate on a `uint32_t` notification word guarded by a critical section and wake the task via the same notification semaphore. Only notification index 0 is supported.

**Static vs. heap allocation:** `xTaskCreateStatic` and `xQueueCreateStatic` accept caller-supplied memory for zero-heap-usage deployments, falling back to `operator new` when no buffer is supplied.

**Stack memory:** Task stacks can be caller-supplied (`xTaskCreateStatic`) or heap-allocated. The default stack size when `usStackDepth == 0` is `FREERTOS_STK_DEFAULT_STACK_WORDS` (default: 256 words). Stack high-water mark is available via `uxTaskGetStackHighWaterMark`, which counts untouched `stk::Word` slots filled with `STK_STACK_MEMORY_FILLER` at initialization.

**Critical sections:** `taskENTER_CRITICAL` / `taskEXIT_CRITICAL` map to `stk::hw::CriticalSection::Enter()` / `Exit()`. `vTaskSuspendAll` / `xTaskResumeAll` use the same mechanism; `xTaskResumeAll` always returns `pdFALSE` as no pending-switch tracking is performed at the wrapper level.

**Memory allocator:** `pvPortMalloc` and `vPortFree` are declared `__stk_weak` and delegate to `stk::memory::MemoryAllocator::Allocate` / `::Free`, which is backed by a `configTOTAL_HEAP_SIZE`-bounded wrapper around the platform `malloc`. Override both weak symbols to redirect all allocations — including internal wrapper allocations — to a custom pool allocator.

**Thread-local storage:** Each task control block carries `configNUM_THREAD_LOCAL_STORAGE_POINTERS` `void*` slots. Reads and writes are guarded by a `stk::sync::ScopedCriticalSection` for correctness on dual-core targets (e.g. RP2040, dual-core Cortex-M33).

**Stream buffers:** Backed by `stk::sync::Pipe` (byte-width instance). Trigger-level blocking uses `Pipe::ReadBulk(dst, trigger, timeout)` — no busy-polling and no lost-wakeup race — with a non-blocking `TryReadBulk` drain for any additional bytes requested beyond the trigger count.

**Message buffers:** Backed by a `stk::sync::MessageQueue` envelope FIFO paired with a `stk::memory::BlockMemoryPool`. Each send allocates a pool block, copies the payload, and enqueues a `{len, blk}` envelope. Each receive dequeues the envelope, copies out, and frees the block. Pool and queue are always 1:1, and the pool acts as the natural capacity limiter.

---

## Known Limitations

| Area | Behaviour |
|---|---|
| `xQueueSendToFrontFromISR` `pxHigherPriorityTaskWoken` | Always `pdFALSE`. STK handles preemption internally; no explicit yield from ISR is required |
| Task notifications (index > 0) | Only notification index 0 is supported. Calls using higher indices compile and link successfully but operate on index 0 only. |
| `pxHigherPriorityTaskWoken` | Always set to `pdFALSE` on all ISR-variant functions. STK handles preemption internally on the next scheduler tick; explicit `portYIELD_FROM_ISR` calls are accepted but produce no additional effect. |
| `xTaskResumeAll` | Always returns `pdFALSE`. No wrapper-level pending context-switch tracking. |
| `vTaskGetRunTimeStats` | Produces the expected table format with task names; `Abs Time` and `% Time` columns are always `0`. STK has its own tracing infrastructure (SEGGER SystemView). |
| `uxTaskGetSystemState` `ulRunTimeCounter` | Always `0` for the same reason as above. |
| `vPortGetHeapStats` free-block fields | `xSizeOfSmallestFreeBlockInBytes` and `xNumberOfFreeBlocks` are conservatively reported as `1` / `0` because the allocator delegates to platform `malloc` with no block-list traversal. |
| SMP affinity | Ignored. STK uses a per-core AMP model. |
| Trace / stats (`configGENERATE_RUN_TIME_STATS`) | Not implemented. Use SEGGER SystemView integration for task-level profiling. |

---

## Quick Integration

### 1. Add the wrapper source to your build

```make
SRCS += libs/stk/interop/freertos/src/freertos_stk.cpp
```

### 2. Add include paths

```make
INCLUDES += -Ilibs/stk/include
INCLUDES += -Ilibs/stk/interop/freertos/include
```

### 3. Provide a minimal `FreeRTOSConfig.h`

The wrapper reads `configMAX_PRIORITIES` from `FreeRTOS.h` / `FreeRTOSConfig.h`. A minimal configuration only needs:

```c
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configMAX_PRIORITIES                    8U    // must be <= 32
#define configMINIMAL_STACK_SIZE                128U  // words; used by application code, not enforced by wrapper
#define configTOTAL_HEAP_SIZE                   (32 * 1024U)
#define portMAX_DELAY                           0xFFFFFFFFUL

// Optional: enable stream/message buffer support
#define configUSE_STREAM_BUFFERS                1

// Optional: enable queue set support
#define configUSE_QUEUE_SETS                    1

// Optional: number of thread-local storage slots per task (default: 0)
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 4U

#endif /* FREERTOS_CONFIG_H */
```

### 4. (Optional) Override defaults in your `stk_config.h`

```cpp
// Maximum number of concurrent FreeRTOS tasks (default: 16)
#define FREERTOS_STK_MAX_TASKS           32U

// Default stack size in Words when usStackDepth == 0 (default: 256)
#define FREERTOS_STK_DEFAULT_STACK_WORDS 512U

// Enable tickless idle (maps to KERNEL_TICKLESS)
#define STK_TICKLESS_IDLE                1
```

### 5. Initialize and start the scheduler

```c
#include "FreeRTOS.h"
#include "task.h"

static void app_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        // application code
        vTaskDelay(100);
    }
}

int main(void)
{
    // Board / clock init here ...

    xTaskCreate(app_task, "app", 256, NULL, 2, NULL);  // create at least one task

    vTaskStartScheduler();                              // never returns
}
```

All subsequent FreeRTOS API calls (`xQueueCreate`, `xSemaphoreCreateMutex`, `xTimerCreate`, `xStreamBufferCreate`, etc.) work as documented in the FreeRTOS reference manual.

---

## What to Expect After Migration

The following table summarizes the observable differences after swapping FreeRTOS for STK via this wrapper — without changing any application code.

| Metric | FreeRTOS | STK (via wrapper) |
|---|---|---|
| Task throughput (4 tasks, -Ofast) | baseline | **+12%** |
| Task throughput (16 tasks, -Ofast) | baseline | **+2.8%** |
| Scheduling jitter (16 tasks, -Ofast) | baseline | **~17% lower** |
| RAM usage (4 tasks) | baseline | **~21% lower** |
| RAM usage (16 tasks) | baseline | **~8.5% lower** |
| Hard real-time deadline enforcement | ❌ | ✅ (`KERNEL_HRT`) |
| MPU privilege separation | limited | ✅ native |
| Tickless idle | ✅ | ✅ (`STK_TICKLESS_IDLE=1`) |
| x86 development / unit test mode | ❌ | ✅ |
| Multiple scheduling strategies | ❌ | ✅ (8 built-in + custom) |
| 100% scheduler unit test coverage | ❌ | ✅ |
| SEGGER SystemView tracing | ✅ | ✅ |
| MIT license | MIT (since FreeRTOS v10) | MIT |
