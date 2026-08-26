# TinyUSB `osal` port for SuperTinyKernel RTOS (STK)

Three files, dropped into TinyUSB's `src/portable/../osal/` (or any directory
on your include path) next to TinyUSB's other OSAL ports:

- **`tusb_os_custom.h`** — the file TinyUSB's `osal/osal.h` actually includes
  when `OPT_OS == OPT_OS_CUSTOM` (see setup step below). It's a one-line
  redirect to `osal_stk.h`, matching the indirection TinyUSB expects from a
  custom OS port.
- **`osal_stk.h`** — the OSAL surface TinyUSB calls. It can bind to either of
  STK's two APIs, selected at compile time via `CFG_TUH_OSAL_STK_USE_CPP`:
  - `0` (default) — STK's pure C API (`stk_c.h`). Every function is a
    `static inline` wrapper living entirely in this header, exactly like the
    header-only `osal_freertos.h` / `osal_threadx.h` / `osal_rtx4.h` ports
    already shipped with TinyUSB. No companion `.cpp` file is needed, and the
    header stays includable from TinyUSB's plain-C translation units directly.
  - `1` — STK's native C++ API (`stk::sync::Mutex/Semaphore/MessageQueue`,
    `stk::hw::CriticalSection`). Every STK-touching function is only
    *declared* here (as `extern "C"`) and *defined* in the companion
    `osal_stk.cpp`. Pick this mode only if something else in your build
    already requires STK's C++ API — the C backend above covers everything
    this OSAL port needs.
- **`osal_stk.cpp`** — the C++-backend implementation, needed and compiled
  **only** when `CFG_TUH_OSAL_STK_USE_CPP=1`. Must be compiled as C++ (it
  uses `stk::sync::Mutex/Semaphore/MessageQueue`, `stk::hw::CriticalSection`),
  but every function it defines is `extern "C"` so it links cleanly against
  TinyUSB's plain-C `.c` files. If you're on the default C backend, don't add
  this file to your build at all — it `#error`s out if compiled without
  `CFG_TUH_OSAL_STK_USE_CPP=1` to make a stray build-system inclusion obvious
  rather than silently linking a dead translation unit.

## One extra setup step: pointing TinyUSB at this port

Unlike lwIP, TinyUSB has no equivalent of `sys_thread_new()` — it never
spawns its own task. Your application creates the TinyUSB task itself and
drives it by calling `tuh_task()` / `tud_task()` from inside it, so there's
no kernel instance for this port to be handed up front: `osal_task_get_current_handle()`,
mutexes, semaphores, and queues all resolve against whichever `stk::Kernel`
happens to be running the calling task, via `stk::GetTid()` (C++ backend) or
`stk_tid()` (C backend). Nothing to register.

The one thing STK-specific this port does need is telling TinyUSB to use it
at all, since STK isn't one of TinyUSB's built-in `OPT_OS_*` choices. In your
`tusb_config.h`:

```c
#define OPT_OS               OPT_OS_CUSTOM
#define CFG_TUSB_OS_HEADER_FILE "tusb_os_custom.h"

// Optional: only if you specifically need STK's C++ API elsewhere in your
// build (see the file list above for when that applies).
// #define CFG_TUH_OSAL_STK_USE_CPP 1
```

That's the whole integration — `osal/osal.h` will pull in
`tusb_os_custom.h` → `osal_stk.h`, and (in C++ backend mode only) you add
`osal_stk.cpp` to your sources. Your application creates the TinyUSB task
itself the same way it creates any other STK task — see
[Examples](#examples) below for both a C++ and a C build of this.

## Examples

Two full runnable examples — TinyUSB's UAC2 `audio_test` device demo (blinky
+ USB device task + a simulated audio ISR task, all as STK tasks) — are
built alongside this port, one per language binding:

| Example | Path                                                                                                         | STK API used         |
|---------|--------------------------------------------------------------------------------------------------------------|----------------------|
| C       | [build\example\audio_mic_c](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/audio_mic_c) | `stk_c.h` (plain C)  |
| C++     | [build\example\audio_mic](https://github.com/SuperTinyKernel-RTOS/stk/tree/main/build/example/audio_mic)   | `stk.h` (native C++) |

Both build the identical demo — same three tasks, same priorities, same USB
descriptors — so which one to look at is purely a matter of whether your
own application is C or C++; this port's OSAL layer works identically
either way.

- **`audio_mic` (C++, `main.cpp`)** — STK's native C++ API. Each task is a
  `stk::Task<StackSize, AccessMode>` subclass with the body in `Run()`,
  priorities expressed via `GetWeight()`, and the kernel instantiated as a
  template (`Kernel<KERNEL_STATIC | KERNEL_SYNC, 3, SwitchStrategyFP32, PlatformDefault>`):

  ```cpp
  #include "stk.h"

  class BlinkyTask : public stk::Task<BLINKY_STACK_SIZE, stk::ACCESS_PRIVILEGED>
  {
      const char *GetTraceName() const override { return "blinky"; }
      stk::Weight GetWeight()    const override { return PRIO_BLINKY; }

      void Run() override
      {
          while (true)
          {
              stk::SleepMs((stk::Timeout) blink_interval_ms);
              board_led_write(led_state);
              led_state = 1 - led_state;
          }
      }
  };
  static BlinkyTask blinky_task;
  // ... UsbDeviceTask (calls tusb_init() + tud_task()), AudioIsrTask similarly ...

  int main()
  {
      static stk::Kernel<stk::KERNEL_STATIC | stk::KERNEL_SYNC, 3,
                          stk::SwitchStrategyFP32, stk::PlatformDefault> kernel;
      kernel.Initialize();
      kernel.AddTask(&blinky_task);
      // ... AddTask() for the other two ...
      kernel.Start();   // never returns
  }
  ```

- **`audio_mic_c` (C, `main.c`)** — STK's plain C API (`stk_c.h`). The same
  three tasks are free functions (entry point + `void *param`), created with
  `stk_task_create_privileged()` / `stk_task_create_user()`, given a name and
  priority, and registered with `stk_kernel_add_task()` before
  `stk_kernel_start()`:

  ```c
  #include "stk_c.h"

  static stk_word_t blinky_stack[BLINKY_STACK_SIZE];

  void led_blinking_task(void *param)
  {
      (void) param;
      while (1) {
          stk_sleep_ms((stk_timeout_t) blink_interval_ms);
          board_led_write(led_state);
          led_state = 1 - led_state;
      }
  }
  // ... usb_device_task() (calls tusb_init() + tud_task()), audio_isr_task() similarly ...

  int main(int argc, char *argv[])
  {
      stk_kernel_t *k = stk_kernel_create(0);
      stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

      stk_task_t *blinky = stk_task_create_privileged(led_blinking_task, NULL,
                                                        blinky_stack, BLINKY_STACK_SIZE);
      stk_task_set_name(blinky, "blinky");
      stk_task_set_priority(blinky, 1);
      stk_kernel_add_task(k, blinky);
      // ... create/add usb_device_task and audio_isr_task the same way ...

      stk_kernel_start(k);   // never returns
      return 0;
  }
  ```

Both variants create the same three tasks with the same priorities
(`blinky=1`, `usbd=30`, `audio=31`) and the same access modes (blinky/usbd
privileged, the simulated audio ISR task unprivileged).

## Primitive mapping

| TinyUSB `osal_*_t` | STK backing (`CFG_TUH_OSAL_STK_USE_CPP=1`) | STK backing (`CFG_TUH_OSAL_STK_USE_CPP=0`, default) | Allocation |
|---|---|---|---|
| `osal_semaphore_t` | `stk::sync::Semaphore`, placement-new'd | `stk_sem_t`, created via `stk_sem_create()` | static storage embedded in `osal_semaphore_def_t`, sized at the `OSAL_SEMAPHORE_DEF` call site |
| `osal_mutex_t` | `stk::sync::Mutex` (recursive), placement-new'd | `stk_mutex_t`, created via `stk_mutex_create()` | static storage embedded in `osal_mutex_def_t`, sized at the `OSAL_MUTEX_DEF` call site |
| `osal_queue_t` | `stk::sync::MessageQueue`, placement-new'd | `stk_msgq_t`, created via `stk_msgq_create()` | control block embedded in `osal_queue_def_t`; item ring buffer is a separate static array supplied by `OSAL_QUEUE_DEF` |
| `osal_spinlock_t` | `stk::hw::CriticalSection::Session` token | `stk_cs_session_t` token | n/a — 1-byte token only, no separate object |
| `osal_task_handle_t` | `stk::TId` (via `stk::GetTid()`) | `stk_tid_t` (via `stk_tid()`) | n/a |

Unlike the lwIP port, **there is no `stk::memory::BlockMemoryPool` in this
port** — and no dynamic allocation of any kind. TinyUSB's own
`OSAL_SEMAPHORE_DEF` / `OSAL_MUTEX_DEF` / `OSAL_QUEUE_DEF` macros already
produce statically-allocated control blocks at file scope (the same pattern
FreeRTOS's `configSUPPORT_STATIC_ALLOCATION` path uses), so every object this
port creates is placement-constructed directly into storage the *caller*
already owns. There's no bootstrapping-allocator problem to work around here
the way there is in lwIP's `mem_malloc()` — TinyUSB never allocates its OSAL
primitives from a heap in the first place.

## Notable implementation choices

- **Dual backend, one header.** `osal_stk.h` is written to serve both
  `CFG_TUH_OSAL_STK_USE_CPP` values from the same file: the C path
  (`#include "stk_c.h"`) is fully inline; the C++ path only declares
  `extern "C"` functions and relies on `osal_stk.cpp` for definitions. Pick
  the C backend unless something else in your build already needs STK's C++
  API — it avoids adding a second translation unit for no benefit.
- **Fixed-size byte storage, not a raw `sizeof`.** In the C++ backend, a
  plain C struct literal can't invoke a C++ constructor, so
  `osal_semaphore_def_t` / `osal_mutex_def_t` / `osal_queue_def_t` each
  reserve `uintptr_t storage[N]` sized by an `OSAL_STK_*_STORAGE_WORDS`
  macro, and `osal_stk.cpp` placement-constructs the real `stk::sync::*`
  object into it on first use. A `static_assert` there guards every size
  guess — raise the matching macro if one ever fires (e.g. after an STK
  upgrade changes a class's layout). The C backend has no such concern:
  `stk_c.h`'s own `stk_sem_mem_t` / `stk_mutex_mem_t` / `stk_msgq_mem_t` are
  already correctly-sized POD structs, so they're embedded directly.
- **Semaphores always start at 0** (not-yet-signaled) in both backends,
  matching every other OSAL backend TinyUSB ships. The C backend's
  `max_count 0` argument to `stk_sem_create()` means "use STK's default
  maximum" — this port doesn't otherwise cap the count the way the lwIP port
  deliberately caps its semaphores at binary (`SYS_SEM_MAX_COUNT = 1`).
- **`in_isr` selects a call, not a code path.** Like this project's other STK
  ports, STK's switch strategies re-evaluate scheduling internally as part of
  a primitive's `Signal()`/`Put()` itself, so there's no separate
  "please reschedule" step FreeRTOS-style `*FromISR()` calls would need.
  Blocking operations (`osal_mutex_lock`, `osal_semaphore_wait`,
  `osal_queue_receive`) are never called from ISR context by TinyUSB itself,
  so `in_isr` only changes behavior in `osal_semaphore_post` /
  `osal_queue_send`, where it picks the non-blocking primitive
  (`TrySignal()`/`TryPut()`) instead of a blocking one.
- **`osal_semaphore_reset()` drains rather than resets.** STK's semaphore has
  no dedicated reset operation, so both backends loop on `TryWait()` /
  `stk_sem_trywait()` until it returns false — mirroring what FreeRTOS's
  `xQueueReset()` does for a binary semaphore backing.
- **Spinlock is `CriticalSection`, deliberately not STK's bare `SpinLock`.**
  `osal_spinlock_t` carries the session token `CriticalSection::Enter()`
  returns through to the matching `Exit()` call — passing the default
  session there instead (rather than the token actually returned) restores
  the wrong path on unprivileged/TrustZone callers. STK's bare,
  non-interrupt-masking cross-core `SpinLock` is intentionally not used:
  it deadlocks if an ISR on the same core tries to acquire a `SpinLock` the
  interrupted task already holds, which is exactly the scenario
  `osal_spin_lock(..., in_isr=true)` has to support safely.
- **Mutex is recursive but TinyUSB never relies on that.** STK's
  `stk::sync::Mutex` / `stk_mutex_t` is already recursive; it's simply
  harmless here, not something this port added for TinyUSB's benefit.

## Compile-time configuration (define before including, or via build flags)

| Macro | Default | Meaning |
|---|---|---|
| `CFG_TUH_OSAL_STK_USE_CPP` | `0` | Select STK's C API (`0`, header-only) or C++ API (`1`, needs `osal_stk.cpp`) |
| `CFG_TUH_OSAL_STK_SYNC_DEBUG_NAMES` | `0` | Reserve one extra storage word per primitive for a debug name (C++ backend only) |
| `OSAL_STK_SEMAPHORE_STORAGE_WORDS` | `8` (+1 if debug names) | Backing storage size for a placement-constructed `stk::sync::Semaphore` (C++ backend only) |
| `OSAL_STK_MUTEX_STORAGE_WORDS` | `10` (+1 if debug names) | Backing storage size for a placement-constructed `stk::sync::Mutex` (C++ backend only) |
| `OSAL_STK_MSGQUEUE_STORAGE_WORDS` | `6 + 16` (+1 if debug names) | Backing storage size for a placement-constructed `stk::sync::MessageQueue` control block — the item ring buffer itself is separate, supplied by `OSAL_QUEUE_DEF` (C++ backend only) |

## Relevant `tusb_config.h` settings

```c
#define OPT_OS                  OPT_OS_CUSTOM
#define CFG_TUSB_OS_HEADER_FILE "tusb_os_custom.h"
```

Nothing else STK-specific is required — this port implements the full
`osal_*` surface TinyUSB's core expects from any `OPT_OS_CUSTOM` port, so no
other `tusb_config.h` options change based on which OSAL backend you pick.
