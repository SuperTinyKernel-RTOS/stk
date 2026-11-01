# GCC Newlib Wrapper (`interop/newlib`)

STK provides a **Newlib retargetable locking** shim (`src/stk_newlib.cpp`, with the private header `src/stk_newlib.h`) that maps the Newlib [OS interface](https://sourceware.org/newlib/) lock API (`__retarget_lock_*`) onto the STK synchronization primitives. It makes the C library (`malloc`, `printf`, `fopen`, `atexit`, `getenv`, ...) safe to call from multiple STK tasks in any project built with GCC and Newlib / Newlib-nano, including `arm-none-eabi-gcc` based toolchains.

## Why Is This Shim Needed?

Newlib is designed to run on bare metal with no knowledge of the operating system. Its internal state (the heap free list, the list of open `FILE` streams, the `atexit` table, the environment, ...) is protected by locks, but Newlib does not implement them itself. Instead, it calls a small set of hook functions, `__retarget_lock_*`, and expects the RTOS to provide them.

Without these hooks the C library is **not thread-safe**: two tasks calling `malloc()` or `printf()` at the same time can corrupt the heap or interleave and corrupt stdio buffers. Depending on the toolchain, the missing hooks either fail at link time (undefined references) or are silently replaced by no-op stubs that provide no protection at all.

This shim supplies the hooks on top of `stk::sync::Mutex` and `stk::sync::FastMutex`, so Newlib serializes access to its shared state through the STK kernel with priority inheritance and FIFO hand-over.

## Prerequisites

| Requirement                                       | Details                                                                                                                                                                                       |
|---------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Newlib built with `_RETARGETABLE_LOCKING`         | This is a **build-time option of the C library** (`--enable-newlib-retargetable-locking`), not something you can enable with `-D` in your project. Newlib's `sys/lock.h` only exposes `_LOCK_T` and the `__retarget_lock_*` prototypes when it is on. Most `arm-none-eabi-gcc` distributions ship with it enabled, including the Newlib-nano variant selected by `--specs=nano.specs`. |
| STK kernel built with `KERNEL_SYNC`               | The shim relies on `stk::sync::Mutex` and `stk::sync::FastMutex`, which are only available when the kernel is compiled with `KERNEL_SYNC` mode enabled.                                       |
| C++11 compiler for `stk_newlib.cpp`               | The shim is C++ but exports C linkage symbols, so the rest of the application can be plain C.                                                                                                 |

`stk_newlib.h` stops the build with a clear `#error` if Newlib was built without `_RETARGETABLE_LOCKING`. To check a toolchain manually, search its `sys/lock.h` for `__retarget_lock_init`; if it is absent, the toolchain's Newlib is not retargetable and needs a different build or toolchain.

## Supported API

| API Group              | Functions Covered                                                                                                                  |
|------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| Lock (non-recursive)   | `__retarget_lock_init`, `__retarget_lock_close`, `__retarget_lock_acquire`, `__retarget_lock_try_acquire`, `__retarget_lock_release` |
| Lock (recursive)       | `__retarget_lock_init_recursive`, `__retarget_lock_close_recursive`, `__retarget_lock_acquire_recursive`, `__retarget_lock_try_acquire_recursive`, `__retarget_lock_release_recursive` |
| Newlib static locks    | `__lock___sinit_recursive_mutex`, `__lock___sfp_recursive_mutex`, `__lock___atexit_recursive_mutex`, `__lock___at_quick_exit_mutex`, `__lock___malloc_recursive_mutex`, `__lock___env_recursive_mutex`, `__lock___tz_mutex`, `__lock___dd_hash_mutex`, `__lock___arc4random_mutex` |

## Design & Mapping

* **Non-recursive locks:** `__retarget_lock_*` maps to `stk::sync::FastMutex`. Locking a mutex already owned by the caller is a contract violation and panics with `KERNEL_PANIC_SYNC_DEADLOCK` instead of silently dead-locking.
* **Recursive locks:** `__retarget_lock_*_recursive` maps to `stk::sync::Mutex` (maximum nesting depth `0xFFFE`).
* **Try-acquire:** returns `0` on success and `1` if the lock is owned by another task, as Newlib expects.
* **Dynamic locks:** Locks created by `__retarget_lock_init*` (for example one per open `FILE` stream) are taken from a static `stk::memory::BlockMemoryPool` of `STK_NEWLIB_MAX_LOCKS` blocks (default 8) and returned to it when closed. Allocation and release are O(1), fragmentation-free and never block, and the shim itself never uses the heap. The pool is constructed on first use, so it works before global constructors have run.
* **Static locks:** Newlib's own internal locks are defined by the shim as zero-initialized objects and are constructed lazily on first use, so they are safe to use before global constructors have run. Set `STK_NEWLIB_STATIC_LOCKS=0` if your Newlib build already defines them.
* **Before the kernel starts:** While no STK task is running (startup code, static constructors, `__libc_init_array()`), only one flow of execution exists, so all lock operations are no-ops. Locking becomes active as soon as the code runs in a task context.
* **Lock object layout:** Each lock holds either a `FastMutex` or a `Mutex` constructed in place in a single storage block sized for the larger of the two.

## Known Limitations

| Area                                             | Behaviour                                                                                                                                  |
|--------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|
| ISR usage                                        | Locks are ISR-unsafe. Do not call Newlib functions that take locks (`malloc`, `printf`, `fopen`, ...) from an ISR.                          |
| Per-thread Newlib state                          | Only locking is provided. Per-thread `_reent` state (such as `errno`) is not switched per task by this shim.                               |
| Lock held across kernel start                    | A lock acquired before the kernel starts (no-op) and released after it starts is ignored on release.                                       |
| Lock pool exhaustion                             | The number of simultaneously existing dynamic locks is capped by `STK_NEWLIB_MAX_LOCKS`. Newlib's init hooks cannot report failure, so exceeding it panics with `KERNEL_PANIC_NO_MEM`. |
| Kernel-running detection                         | Task context is detected through `IKernelService::GetInstance()` and `GetTid()`. Adjust `IsTaskContext()` if your port behaves differently before kernel start. |

---

## Quick Integration

### 1. Add the shim source to your build

```make
SRCS += libs/stk/interop/newlib/src/stk_newlib.cpp
```

### 2. Add include paths

```make
INCLUDES += -Ilibs/stk/include
```

The shim has no public header: `stk_newlib.h` is internal and is only included by `stk_newlib.cpp`, so there is nothing for application code to include and no newlib include path to add.

### 3. (Optional) Override defaults with compiler definitions

```make
# Do not define Newlib's static locks (provided elsewhere), default: 1
CXXFLAGS += -DSTK_NEWLIB_STATIC_LOCKS=0

# Capacity of the static pool for dynamic locks (standard streams + open FILE streams), default: 8
CXXFLAGS += -DSTK_NEWLIB_MAX_LOCKS=12U
```

Size `STK_NEWLIB_MAX_LOCKS` for the three standard streams plus the largest number of streams your application keeps open at the same time. The pool is static, so the RAM is reserved up front; if the limit is exceeded the shim panics with `KERNEL_PANIC_NO_MEM` rather than failing silently, so an undersized value is caught on the first run that reaches it.

### 4. Build and link

No code changes are required: the shim only provides symbols that Newlib already calls. Link `stk_newlib.cpp` into the application together with the STK kernel, built with `KERNEL_SYNC` enabled, and the C library becomes safe to use from multiple tasks.

```c
#include <stdio.h>
#include <stdlib.h>

void task_a(void *arg)
{
    for (;;)
    {
        void *p = malloc(64);   // serialized by the shim's malloc lock
        printf("task A: %p\n", p);
        free(p);
    }
}
```
