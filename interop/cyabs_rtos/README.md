# Infineon Abstraction RTOS Wrapper (`interop/cyabs_rtos`)

STK provides an implementation of the **Infineon RTOS abstraction layer** ([abstraction-rtos](https://github.com/Infineon/abstraction-rtos), `cyabs_rtos.h`) on top of the STK C++ kernel (`cyabs_rtos_stk.cpp` + the `COMPONENT_STK` headers). Infineon middleware and libraries written against the `cy_rtos_*` API (e.g. the Wi-Fi Host Driver, connectivity and cloud libraries, `cy_worker_thread`) compile unchanged, so STK can be used as an RTOS backend in any project that targets the abstraction layer, next to the FreeRTOS, RTX, ThreadX and Zephyr components.

The headers are plain C (also valid C++). As in the other components, the `cy_*_t` types are allocated by the caller and hold the STK objects in an opaque storage member (placement-new). Only thread control blocks, stacks that were not supplied by the caller, and queue buffers are allocated from the heap (`stk::memory::MemoryAllocator` by default).

## Supported API Groups

| API Group          | Functions Covered |
|--------------------|-------------------|
| Kernel (STK port)  | `cy_rtos_stk_kernel_init`, `cy_rtos_stk_kernel_start`, `cy_rtos_stk_kernel_enter`, `cy_rtos_stk_get_kernel` (C++ only) - the abstraction layer has no "start scheduler" call, these are provided by the port |
| Error              | `cy_rtos_last_error` |
| Thread             | `cy_rtos_thread_create`, `cy_rtos_thread_exit`, `cy_rtos_thread_terminate`, `cy_rtos_thread_join`, `cy_rtos_thread_is_running`, `cy_rtos_thread_get_state`, `cy_rtos_thread_get_handle`, `cy_rtos_thread_get_name`, `cy_rtos_thread_wait_notification`, `cy_rtos_thread_set_notification` |
| Scheduler          | `cy_rtos_scheduler_suspend`, `cy_rtos_scheduler_resume`, `cy_rtos_scheduler_get_state` |
| Mutex              | `cy_rtos_mutex_init`, `cy_rtos_mutex_get`, `cy_rtos_mutex_set`, `cy_rtos_mutex_deinit` |
| Semaphore          | `cy_rtos_semaphore_init`, `cy_rtos_semaphore_get`, `cy_rtos_semaphore_set`, `cy_rtos_semaphore_get_count`, `cy_rtos_semaphore_deinit` |
| Event Flags        | `cy_rtos_event_init`, `cy_rtos_event_setbits`, `cy_rtos_event_clearbits`, `cy_rtos_event_getbits`, `cy_rtos_event_waitbits`, `cy_rtos_event_deinit` |
| Message Queue      | `cy_rtos_queue_init`, `cy_rtos_queue_put`, `cy_rtos_queue_get`, `cy_rtos_queue_count`, `cy_rtos_queue_space`, `cy_rtos_queue_reset`, `cy_rtos_queue_deinit` |
| Timer              | `cy_rtos_timer_init`, `cy_rtos_timer_start`, `cy_rtos_timer_stop`, `cy_rtos_timer_is_running`, `cy_rtos_timer_deinit` |
| Time               | `cy_rtos_time_get`, `cy_rtos_delay_milliseconds` |

## Design & Mapping

* **Kernel configuration:** The wrapper instantiates one global `stk::Kernel` with `KERNEL_DYNAMIC | KERNEL_SYNC | KERNEL_TELEMETRY` flags and the `sched::SwitchStrategyFixedPriority<CY_RTOS_STK_PRIORITY_LEVELS>` scheduler: fixed priority preemptive scheduling with round-robin inside a priority level. Tickless mode is enabled automatically when `STK_TICKLESS_IDLE=1` is defined. `KERNEL_TELEMETRY` is used to report thread states (`cy_rtos_thread_get_state()`).
* **Startup:** The abstraction layer has no call to start the scheduler, every RTOS is started by the application with its own API. For STK use `cy_rtos_stk_kernel_enter(app_init)`: it initializes the kernel, calls `app_init()` to create the first threads and starts the scheduler. Alternatively call `cy_rtos_stk_kernel_init()`, create the threads, then `cy_rtos_stk_kernel_start()`. `cy_rtos_stk_kernel_init()` must precede the first thread or timer; mutexes, semaphores, events and queues can be created at any time.
* **Thread capacity:** Controlled by the `CY_RTOS_STK_MAX_THREADS` macro (default: 16). The tick and handler tasks of the timer host are accounted separately.
* **Priority mapping:** `cy_thread_priority_t` is spread over `CY_RTOS_STK_PRIORITY_LEVELS` levels (default: 32, range: 8..1024) and maps directly to STK weights. As in STK a bigger value is more urgent: `CY_RTOS_PRIORITY_MIN` = `0` ... `CY_RTOS_PRIORITY_MAX` = `CY_RTOS_STK_PRIORITY_LEVELS - 1`. Above 32 levels the ready bitmap grows (64 bits up to 64 levels, multi-word above), which costs some RAM and lookup time.
* **Timeouts and ticks:** `0` maps to `stk::NO_WAIT` and `CY_RTOS_NEVER_TIMEOUT` to `stk::WAIT_INFINITE`. Other values are milliseconds, rounded **up** to ticks so a wait is never shorter than requested. The tick resolution is `CY_RTOS_STK_TICK_PERIOD_US` microseconds (default 1000 = 1 ms). A blocking call with a non-zero timeout from an ISR returns `CY_RTOS_BAD_PARAM`; while the scheduler is suspended a wait degrades to "no wait" instead of blocking.
* **Threads:** Backed by an `stk::ITask` object in a heap block (control block, plus the stack if `stack == NULL`); all threads are privileged. `cy_thread_t` is a pointer to the control block. `cy_rtos_thread_exit()` does not return (as in RTX/FreeRTOS), returning from the entry function is equivalent. `cy_rtos_thread_join()` blocks on a semaphore signaled from `ITask::OnExit()` (no polling, tickless friendly), then releases the control block and sets the handle to `NULL`. Every thread must be joined to release its memory. Thread states are derived from the kernel's own task state.
* **Thread notifications:** A binary semaphore owned by the thread: a notification sent while the thread is not waiting is kept and satisfies its next `cy_rtos_thread_wait_notification()`. `cy_rtos_thread_set_notification()` is ISR-safe.
* **Mutexes:** `stk::sync::Mutex` - recursive, with priority inheritance.
* **Semaphores:** `stk::sync::SemaphoreT<uint32_t>` - `cy_rtos_semaphore_set()` is an atomic signal limited by the maximum count given to `cy_rtos_semaphore_init()`; at the limit it returns `CY_RTOS_GENERAL_ERROR`.
* **Event flags:** `stk::sync::EventFlagsT<uint64_t>` - all 32 flags are usable (kept in the low half of a 64-bit word, the top bit is STK's reserved error marker). `cy_rtos_event_waitbits()` returns the flags word at the moment the request was satisfied, before the optional clear.
* **Message queues:** `stk::sync::MessageQueue` over a heap block of `length * itemsize` bytes. Any item size is supported, items are copied as they are. A full queue is reported as `CY_RTOS_QUEUE_FULL`, an empty one as `CY_RTOS_TIMEOUT`.
* **Timers:** Backed by `stk::time::TimerHost` in task mode. It is started by `cy_rtos_stk_kernel_init()` and runs a tick task (`CY_RTOS_STK_TIMER_TICK_PRIORITY`, default `HIGH`) and a handler task (`CY_RTOS_STK_TIMER_HANDLER_PRIORITY`, default `ABOVENORMAL`) in which the callbacks run. The `cy_timer_t` object embeds the timer, so the number of timers is not limited. `cy_rtos_timer_start()` on a running timer restarts it; `cy_rtos_timer_stop()` and `cy_rtos_timer_deinit()` take effect before they return, i.e. the callback will not run afterwards.
* **Scheduler suspend:** `cy_rtos_scheduler_suspend()` / `cy_rtos_scheduler_resume()` use `stk::LockScheduler()` with a nesting counter: task switching stops while ticks, timeouts and ISRs keep running.
* **Memory:** Heap functions are `stk::memory::MemoryAllocator::Allocate()` / `Free()` (the application supplies the backend), or `malloc` / `free` if STK has no heap support. Override with `CY_RTOS_STK_MALLOC(size)` / `CY_RTOS_STK_FREE(ptr)`.
* **Last error:** `cy_rtos_last_error()` returns a `CY_RTOS_STK_ERR_*` code (see `cyabs_rtos_stk.h`) explaining the last `CY_RTOS_GENERAL_ERROR` (wrong context, wait cancelled, semaphore at its limit, mutex not owned, kernel not initialized, ...).
* **Control block storage:** The size of the opaque storage in each `cy_*_t` type is set by `CY_RTOS_STK_MUTEX_WORDS`, `CY_RTOS_STK_SEMAPHORE_WORDS`, `CY_RTOS_STK_EVENT_WORDS`, `CY_RTOS_STK_QUEUE_WORDS` and `CY_RTOS_STK_TIMER_WORDS` (in `uint64_t` words, defaults are measured for 32-bit and 64-bit targets). The wrapper checks the fit with `static_assert`; if one fires (e.g. STK is built with extra debug members), increase the corresponding macro.

## Known Limitations

| Area                                                       | Behaviour |
|------------------------------------------------------------|-----------|
| Threads not created by `cy_rtos_thread_create()`           | `cy_rtos_thread_get_handle()` and the notification calls return `CY_RTOS_GENERAL_ERROR` for raw STK tasks and for timer callbacks. Only threads of the abstraction layer are known to it. |
| Mutex `recursive` argument                                 | Accepted but not honored. STK mutexes are always recursive and use priority inheritance. |
| Mutex before the scheduler runs                            | `cy_rtos_mutex_get()` / `cy_rtos_mutex_set()` return `CY_RTOS_GENERAL_ERROR` until the kernel is started. |
| Thread memory                                              | Control blocks (and stacks passed as `NULL`) are released by `cy_rtos_thread_join()`; a thread that is never joined keeps its memory. |
| Thread stack                                               | `stack_size` must be a multiple of 8 and at least `CY_RTOS_MIN_STACK_SIZE` (default 512 bytes, 2560 on RISC-V); a caller-supplied stack must be aligned to `STK_STACK_MEMORY_ALIGN`. `STK_MPU_STACK_GUARD` is not supported. |
| Priority range                                             | `CY_RTOS_STK_PRIORITY_LEVELS` must be in [8, 1024] (checked at compile time) so that all `CY_RTOS_PRIORITY_*` constants stay distinct. |
| Time slicing                                               | Threads of the same priority are rotated by the scheduler; there is no per-thread time slice setting (the abstraction layer has none). |
| Timeouts                                                   | Limited to `INT32_MAX` ticks (STK timeout range), larger values are clamped. `cy_rtos_time_get()` wraps after 49 days like `cy_time_t`. |
| Timer calls from an ISR or before the kernel starts        | The command is queued and `CY_RSLT_SUCCESS` is reported optimistically; `cy_rtos_timer_is_running()` returns an advisory snapshot. `cy_rtos_timer_deinit()` then fails with `CY_RTOS_GENERAL_ERROR` and the timer stays valid. |
| Timer callbacks                                            | Run in the handler task of the timer host, not in an ISR. Its stack size is set with `STK_TIMER_HANDLER_STACK_SIZE` of `stk_time_timer.h`. |
| Timer host task slots                                      | The timer host occupies `stk::time::TimerHost::TASK_COUNT` kernel task slots in addition to `CY_RTOS_STK_MAX_THREADS`. Set `CY_RTOS_STK_USE_TIMERS=0` to remove them (`cy_rtos_timer_*` then return `CY_RTOS_UNSUPPORTED`). |
| `deinit` with waiting threads                              | Deleting a mutex, semaphore, event or queue that has threads blocked on it violates the STK contract. |
| Deep-sleep RAM (`cyabs_rtos_dsram.*`)                      | Not provided; it is specific to the ThreadX component. |

---

## Examples

There is a working example for the `STM32F407G-DISC1` development board:

- [Blinky (C)](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/stm/blinky_c-cyabs_rtos-stm32f407g-disc1) ([code](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky_c-cyabs_rtos))

---

## Quick Integration

### 1. Add the wrapper source to your build

```make
SRCS += libs/stk/interop/cyabs_rtos/source/cyabs_rtos_stk.cpp
```

Optionally add the generic worker thread of the abstraction layer (it uses only the `cy_rtos_*` API):

```make
SRCS += libs/abstraction-rtos/source/cy_worker_thread.c
```

### 2. Add include paths

```make
INCLUDES += -Ilibs/stk/include
INCLUDES += -Ilibs/stk/interop/cyabs_rtos/include
INCLUDES += -Ilibs/abstraction-rtos/include
```

> `cyabs_rtos_impl.h` and `cyabs_rtos_types.h` here replace the ones of the other RTOS components; do not put another `COMPONENT_*` include path (FreeRTOS, ThreadX, ...) in the same build. `cyabs_rtos.h` also needs `cy_result.h` from the Infineon `core-lib`.

### 3. (Optional) Override defaults

Pass them with `-D`. Use the same values for the wrapper and the application. STK's own settings (e.g. `STK_TICKLESS_IDLE`) stay in `stk_config.h`.

```cpp
// Maximum number of concurrent threads (default: 16)
#define CY_RTOS_STK_MAX_THREADS               32U

// Number of priority levels, 8..1024 (default: 32)
#define CY_RTOS_STK_PRIORITY_LEVELS           64U

// Tick period in microseconds (default: 1000 = 1 ms)
#define CY_RTOS_STK_TICK_PERIOD_US            1000U

// Software timers: 1 = enabled (default), 0 = no timer tasks
#define CY_RTOS_STK_USE_TIMERS                1U

// Priority of the timer tick and handler tasks (tick must be >= handler)
#define CY_RTOS_STK_TIMER_TICK_PRIORITY       CY_RTOS_STK_PRIO_HIGH
#define CY_RTOS_STK_TIMER_HANDLER_PRIORITY    CY_RTOS_STK_PRIO_ABOVENORMAL

// Minimum thread stack size in bytes (default: 512, 2560 on RISC-V)
#define CY_RTOS_STK_MIN_STACK_BYTES           512U
```

### 4. Create the threads and start the kernel

```c
#include "cyabs_rtos.h"
#include "cyabs_rtos_stk.h"

static cy_thread_t g_thread;

static void app_thread(cy_thread_arg_t arg)
{
    (void)arg;

    for (;;)
    {
        cy_rtos_delay_milliseconds(100);
    }
}

static void app_init(void)                    // plays the role of tx_application_define()
{
    // stack == NULL: the stack is allocated together with the thread control block
    cy_rtos_thread_create(&g_thread, app_thread, "app", NULL, 1024,
                          CY_RTOS_PRIORITY_NORMAL, 0);
}

int main(void)
{
    // Board / clock init here ...

    cy_rtos_stk_kernel_enter(app_init);       // does not return while threads are alive
}
```

or, FreeRTOS style, create everything first and start afterwards:

```c
cy_rtos_stk_kernel_init();
cy_rtos_thread_create(&g_thread, app_thread, "app", NULL, 1024, CY_RTOS_PRIORITY_NORMAL, 0);
cy_rtos_stk_kernel_start();
```

All subsequent abstraction layer calls (`cy_rtos_mutex_init`, `cy_rtos_queue_put`, `cy_rtos_timer_start`, etc.) work as documented in the [abstraction-rtos API reference](https://infineon.github.io/abstraction-rtos/html/index.html), within the limitations above.
