# ThreadX Wrapper (`interop/threadx`)

STK provides an **Eclipse ThreadX** (Azure RTOS ThreadX) compatibility layer (`threadx_stk.cpp` + `tx_api.h`) that maps the standard ThreadX C API (`tx_api.h`, ThreadX 6.x) onto the STK C++ kernel. Application code written against the `tx_*` API compiles unchanged, so STK can be used as a drop-in RTOS backend in any project or middleware stack that targets ThreadX.

The header is plain C (also valid C++). As in native ThreadX, control blocks (`TX_THREAD`, `TX_MUTEX`, ...) are allocated by the caller; the STK objects live in the opaque storage member of each control block (placement-new), so the wrapper **never uses the heap**.

## Supported API Groups

| API Group          | Functions Covered                                                                                                                                                                                                                                                                                                                                                                                  |
|--------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Kernel / System    | `tx_kernel_enter`, `tx_application_define` (provided by the application), `tx_interrupt_control`, `tx_time_get`, `tx_time_set`, `TX_DISABLE` / `TX_RESTORE`                                                                                                                                                                                                                                        |
| Thread             | `tx_thread_create`, `tx_thread_delete`, `tx_thread_entry_exit_notify`, `tx_thread_identify`, `tx_thread_info_get`, `tx_thread_preemption_change`, `tx_thread_priority_change`, `tx_thread_relinquish`, `tx_thread_reset`, `tx_thread_resume`, `tx_thread_sleep`, `tx_thread_stack_error_notify`, `tx_thread_suspend`, `tx_thread_terminate`, `tx_thread_time_slice_change`, `tx_thread_wait_abort` |
| Mutex              | `tx_mutex_create`, `tx_mutex_delete`, `tx_mutex_get`, `tx_mutex_put`, `tx_mutex_info_get`, `tx_mutex_prioritize`                                                                                                                                                                                                                                                                                   |
| Semaphore          | `tx_semaphore_create`, `tx_semaphore_delete`, `tx_semaphore_get`, `tx_semaphore_put`, `tx_semaphore_ceiling_put`, `tx_semaphore_info_get`, `tx_semaphore_prioritize`, `tx_semaphore_put_notify`                                                                                                                                                                                                    |
| Event Flags        | `tx_event_flags_create`, `tx_event_flags_delete`, `tx_event_flags_get`, `tx_event_flags_set`, `tx_event_flags_info_get`, `tx_event_flags_set_notify`                                                                                                                                                                                                                                               |
| Message Queue      | `tx_queue_create`, `tx_queue_delete`, `tx_queue_flush`, `tx_queue_info_get`, `tx_queue_prioritize`, `tx_queue_receive`, `tx_queue_send`, `tx_queue_front_send`, `tx_queue_send_notify`                                                                                                                                                                                                             |
| Block Memory Pool  | `tx_block_pool_create`, `tx_block_pool_delete`, `tx_block_allocate`, `tx_block_release`, `tx_block_pool_info_get`, `tx_block_pool_prioritize`                                                                                                                                                                                                                                                      |
| Byte Memory Pool   | `tx_byte_pool_create`, `tx_byte_pool_delete`, `tx_byte_allocate`, `tx_byte_release`, `tx_byte_pool_info_get`, `tx_byte_pool_prioritize`                                                                                                                                                                                                                                                            |
| Application Timer  | `tx_timer_create`, `tx_timer_delete`, `tx_timer_activate`, `tx_timer_deactivate`, `tx_timer_change`, `tx_timer_info_get`                                                                                                                                                                                                                                                                           |

## Design & Mapping

* **Kernel configuration:** The wrapper instantiates one global `stk::Kernel` with `KERNEL_DYNAMIC | KERNEL_SYNC | KERNEL_TIMESLICE | KERNEL_TELEMETRY` flags and the `sched::SwitchStrategyPreemptionThreshold<TX_MAX_PRIORITIES, true>` scheduler: fixed priority + preemption-threshold + per-thread time-slicing, which is the ThreadX scheduling model. Tickless mode is enabled automatically when `STK_TICKLESS_IDLE=1` is defined. `KERNEL_TELEMETRY` is used to report exact thread states (`tx_thread_info_get()`, `tx_thread_suspend()`, `tx_thread_wait_abort()`).
* **Startup:** `tx_kernel_enter()` initializes the kernel, calls `tx_application_define(NULL)` and starts the scheduler. STK works with caller-supplied memory only, so `first_unused_memory` is always `NULL`: allocate stacks, queue areas and pools statically (or from your own allocator).
* **Thread capacity:** Controlled by the `TX_STK_MAX_THREADS` macro (default: 16). The tick and handler tasks of the timer host are accounted separately.
* **Priority mapping:** ThreadX numbering is kept in the API (`0` = highest, `TX_MAX_PRIORITIES - 1` = lowest). The number of levels is set with `TX_MAX_PRIORITIES` (default: 32, maximum: 1024). It is converted to STK weights internally (`TX_MAX_PRIORITIES - 1 - priority`) together with the preemption-threshold. Above 32 levels the ready bitmap grows (64 bits up to 64 levels, multi-word above), which costs some RAM and lookup time.
* **Timeouts and ticks:** `TX_NO_WAIT` maps to `stk::NO_WAIT` and `TX_WAIT_FOREVER` to `stk::WAIT_INFINITE`; other values are passed as STK ticks. The tick resolution is `1000000 / TX_TIMER_TICKS_PER_SECOND` microseconds (default 1000 ticks/s = 1 ms).
* **Threads:** Backed by an `stk::ITask` object. A thread created with `TX_DONT_START` is passed to the kernel on the first `tx_thread_resume()`; `tx_thread_reset()` restores the creation parameters and leaves the thread suspended. Blocked states (`TX_SLEEP`, `TX_QUEUE_SUSP`, `TX_MUTEX_SUSP`, ...) are derived from the kernel's own task state, not tracked separately.
* **Mutexes:** `stk::sync::Mutex` - recursive, with priority inheritance.
* **Semaphores:** `stk::sync::SemaphoreT<ULONG>` - the counter has the same width as ThreadX's, `tx_semaphore_ceiling_put()` maps to an atomic ceiling-checked signal.
* **Event flags:** `stk::sync::EventFlagsT<uint64_t>` - all 32 ThreadX flags are usable (they are kept in the low half of a 64-bit word, the top bit is STK's reserved error marker). `TX_OR`/`TX_AND` with or without `_CLEAR`, `tx_event_flags_set()` with `TX_AND` clears flags.
* **Message queues:** `stk::sync::MessageQueue` - the message size is counted in `ULONG`s (1, 2, 4, 8, 16 as in ThreadX), the caller's queue area is used directly as the ring buffer. `tx_queue_front_send()` is supported.
* **Block pools:** `stk::memory::BlockMemoryPool` - O(1), zero-fragmentation. As in ThreadX, every block carries a hidden pointer-sized header holding the owning pool, so `tx_block_release()` needs only the block address.
* **Byte pools:** `stk::memory::BytePool` - first-fit allocator with block splitting and coalescing of free neighbours; allocations can block with a timeout. Pools are tracked in a small intrusive list so `tx_byte_release()` can find the owner by address.
* **Timers:** Backed by `stk::time::TimerHost` in task mode. It is started by the first `tx_timer_create()` and runs a tick task and a handler task at `TX_TIMER_THREAD_PRIORITY` (default `0`, as the timer thread of ThreadX). The `TX_TIMER` control block embeds the timer object, so the number of timers is not limited. Activation, deactivation and change calls take effect before they return.
* **Interrupt control:** `tx_interrupt_control()` emulates the posture with an STK critical section (absolute semantics: disable once, enable once, return the previous posture).
* **Control block storage:** The size of the opaque storage in each control block is set by `TX_STK_*_STORAGE_WORDS` (event flags and timers, which hold 64-bit values: `TX_STK_EVENT_FLAGS_STORAGE_DWORDS`, `TX_STK_TIMER_STORAGE_DWORDS`). The wrapper checks the fit with `static_assert`; if one fires, increase the corresponding macro.

## Known Limitations

| Area                                                              | Behaviour                                                                                                                                                                                                                      |
|-------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `*_prioritize()` (mutex, semaphore, queue, block pool, byte pool) | No-op returning success. Waiters are served in FIFO order.                                                                                                                                                                     |
| Mutex `inherit` argument (`TX_NO_INHERIT`)                        | Accepted but not honored. STK mutexes always use priority inheritance and are always recursive.                                                                                                                                |
| `tx_thread_stack_error_notify()`                                  | Accepted and ignored. STK reports a stack overflow through its kernel panic handler (`KERNEL_PANIC_STACK_CORRUPT`).                                                                                                            |
| `*_info_get()` `next_*` outputs                                   | Always `NULL`. The kernel does not expose its internal created-object lists.                                                                                                                                                   |
| `tx_thread_info_get()` `next_suspended_thread`                    | Always `NULL`. For sync objects the `first_suspended` / `suspended_count` outputs are reported.                                                                                                                                |
| `tx_timer_create()` with a `NULL` expiration function             | Returns `TX_FEATURE_NOT_ENABLED` (ThreadX uses `TX_FUNCTION_ERROR`, not defined in this API subset).                                                                                                                           |
| Timer and sleep tick values                                       | Limited to `INT32_MAX` (STK timeout range). Larger values are clamped; `tx_thread_sleep()` cannot sleep forever.                                                                                                               |
| Timer expiration functions                                        | Run in the handler task of the timer host, not in a ThreadX timer thread. Its stack size is set with `STK_TIMER_HANDLER_STACK_SIZE` of `stk_time_timer.h`; `TX_TIMER_THREAD_STACK_SIZE` is kept only for source compatibility. |
| Timer calls from an ISR or before the kernel starts               | The command is queued and `TX_SUCCESS` is reported optimistically; `tx_timer_info_get()` returns an advisory snapshot.                                                                                                         |
| Timer host task slots                                             | The timer host occupies `stk::time::TimerHost::TASK_COUNT` kernel task slots in addition to `TX_STK_MAX_THREADS`.                                                                                                              |
| Thread deletion and reset                                         | `tx_thread_delete()` / `tx_thread_reset()` wait (a few ticks) until the kernel has removed the completed or terminated thread; they return `TX_DELETE_ERROR` / `TX_NOT_DONE` if it is still referenced.                        |
| Priority range                                                    | `TX_MAX_PRIORITIES` must be in [1, 1024] (checked at compile time). Native ThreadX requires a multiple of 32; the wrapper does not.                                                                                            |
| Pool and queue creation                                           | Must be called from thread or initialization context (`TX_CALLER_ERROR` from an ISR).                                                                                                                                          |
| Other ThreadX services                                            | Not provided: ThreadX trace (`tx_trace_*`), run-time performance info (`tx_*_performance_info_get`), and optional add-on components (NetX, FileX, USBX, GUIX).                                                                 |

---

## Examples

There are working examples for the `STM32F407G-DISC1` development board:

- [Blinky (C)](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/stm/blinky_c-threadx-stm32f407g-disc1) ([code](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky_c-threadx))
- [Blinky](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/project/eclipse/stm/blinky-threadx-stm32f407g-disc1) ([code](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/blinky-threadx))

---

## Quick Integration

### 1. Add the wrapper source to your build

```make
SRCS += libs/stk/interop/threadx/src/threadx_stk.cpp
```

### 2. Add include paths

```make
INCLUDES += -Ilibs/stk/include
INCLUDES += -Ilibs/stk/interop/threadx/include
```

> `tx_api.h` here replaces the native ThreadX header; do not put the original ThreadX include path in the same build.

### 3. (Optional) Override defaults

Pass them with `-D`, or define `TX_INCLUDE_USER_DEFINE_FILE` and put them into a `tx_user.h` (included first by `tx_api.h`). Use the same values for the wrapper and the application. STK's own settings (e.g. `STK_TICKLESS_IDLE`) stay in `stk_config.h`.

```cpp
// Tick rate (default: 1000 ticks per second)
#define TX_TIMER_TICKS_PER_SECOND     1000UL

// Maximum number of concurrent threads (default: 16)
#define TX_STK_MAX_THREADS            32U

// Number of priority levels, 1..1024 (default: 32)
#define TX_MAX_PRIORITIES             64U

// Priority of the timer tick and handler tasks (default: 0 = highest)
#define TX_TIMER_THREAD_PRIORITY      0U
```

### 4. Define the application and start the kernel

```c
#include "tx_api.h"

static TX_THREAD g_thread;
static ULONG     g_stack[512];

static void app_thread(ULONG input)
{
    for (;;)
    {
        tx_thread_sleep(100);
    }
}

void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory; // always NULL with STK

    tx_thread_create(&g_thread, "app", app_thread, 0,
                     g_stack, sizeof(g_stack),
                     10, 10,                  // priority, preemption-threshold
                     TX_NO_TIME_SLICE, TX_AUTO_START);
}

int main(void)
{
    // Board / clock init here ...

    tx_kernel_enter();                        // never returns
}
```

All subsequent ThreadX calls (`tx_mutex_create`, `tx_queue_send`, `tx_timer_create`, etc.) work as documented in the ThreadX user guide, within the limitations above.
