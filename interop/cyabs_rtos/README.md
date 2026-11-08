# STK port of the Infineon RTOS abstraction layer

Implements `cyabs_rtos.h` on SuperTinyKernel RTOS (STK).
Same layout as `COMPONENT_THREADX`; `cy_worker_thread.c` is used unchanged.

```
include/COMPONENT_STK/cyabs_rtos_impl.h        constants (min stack, alignment)
include/COMPONENT_STK/cyabs_rtos_types.h       cy_*_t types, priorities
include/COMPONENT_STK/cyabs_rtos_stk_config.h  build-time options (all overridable with -D)
include/COMPONENT_STK/cyabs_rtos_stk.h         kernel init/start (the abstraction has no such call)
source/COMPONENT_STK/cyabs_rtos_stk.cpp        implementation
```
Add `COMPONENT_STK` to the include/source paths. Build with C++17, STK headers and your `stk_config.h` on the include path.

## Start-up
```c
cy_rtos_stk_kernel_enter(app_init);   // init + app_init() + Start()
/* or */ cy_rtos_stk_kernel_init(); cy_rtos_thread_create(...); cy_rtos_stk_kernel_start();
```
`cy_rtos_stk_kernel_init()` must precede the first thread/timer. Mutexes, semaphores, events and
queues can be created at any time.

## Design
| Area | Implementation |
|---|---|
| Scheduler | `Kernel<DYNAMIC|SYNC|TELEMETRY>` + `SwitchStrategyFixedPriority<CY_RTOS_STK_PRIORITY_LEVELS>` (higher number = more urgent, round robin inside a level) |
| Threads | heap control block (+ stack if `stack == NULL`), registry of created threads; all tasks privileged |
| Join | polls (`Sleep(1)`) until the kernel has removed the task, then frees it; handle is set to `NULL` |
| Notification | per-thread binary semaphore (a notification sent early is kept) |
| Mutex | `sync::Mutex` (always recursive, priority inheritance) |
| Semaphore | `SemaphoreT<uint32_t>`, `set` fails with `GENERAL_ERROR` at max count |
| Event | `EventFlagsT<uint64_t>`, all 32 bits usable |
| Queue | `sync::MessageQueue`, any item size, heap buffer |
| Timer | `time::TimerHost` (tick + handler task, see `CY_RTOS_STK_USE_TIMERS`); callbacks run in the handler task, `start` on a running timer restarts it, `stop` is synchronous |
| Scheduler suspend | `stk::LockScheduler()` with a nesting counter |
| Time | ms rounded **up** to ticks; `cy_rtos_time_get` wraps at 49 days |

## Differences / caveats
* Only threads made by `cy_rtos_thread_create()` can use `get_handle`, `wait/set_notification`.
  Raw STK tasks (and timer callbacks) get `CY_RTOS_GENERAL_ERROR` there.
* `cy_rtos_thread_exit()` does not return (like RTX/FreeRTOS); returning from the entry also works.
* `queue_put` on a full queue returns `CY_RTOS_QUEUE_FULL`.
* Blocking with a timeout in an ISR -> `CY_RTOS_BAD_PARAM`; while the scheduler is locked a wait
  degrades to "no wait" instead of asserting.
* `deinit` of an object with waiting threads is an STK contract violation (as in the shim).
* `cy_thread_arg_t` / `cy_timer_callback_arg_t` are `uintptr_t` (hold pointers and integers).
* `cy_rtos_last_error()` returns `CY_RTOS_STK_ERR_*` (see `cyabs_rtos_stk.h`).
* Storage sizes in the `cy_*_t` types are checked by `static_assert`; raise
  `CY_RTOS_STK_*_WORDS` if STK is built with extra members.
* `malloc`/`free` (or your `CY_RTOS_STK_MALLOC/FREE`) must be thread-safe.

## Verification status
Compiled warning-free (`-Wall -Wextra`) with g++ for x86-64 and ARM 32-bit against your STK headers
using a stub platform, and the header was compiled from C. All 43 `cy_rtos_*` functions are
defined. **Not run**: no scheduler/hardware was available, so behaviour (timing, ISR paths, timer
callbacks, join) still needs a test on a real STK target.
