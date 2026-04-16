# STK Synchronization Module (`stk::sync`)

**STK Synchronization Module** provides a set of high-performance synchronization primitives designed specifically for embedded systems. These classes facilitate task communication, resource protection, and signaling. Waiting operations are supported directly by the kernel.

## Features

- **Zero Dynamic Allocation**: Objects are allocated statically or on stack. The C API uses opaque memory containers (`stk_xxx_mem_t`) to guarantee alignment and size without heap usage.
- **Direct Resource Handover**: Resources are immediately assigned to the head of the wait list upon release. If the newly readied task is the highest-priority, an immediate context switch occurs.
- **Low-Power Optimization**: Blocking `Wait` operations remove tasks from the ready list, allowing the CPU to enter low-power sleep (e.g., `WFI`).
- **Strict FIFO Ordering**: Wait operations are processed chronologically. The kernel wakes the longest-waiting task first, ensuring absolute fairness.
- **Non-blocking Polling**: Supports `TryWait` and `TryLock` for checking status without yielding the CPU, ideal for zero-latency performance loops.
- **Nesting Support**: Both `sync::Mutex` and `sync::SpinLock` support recursive acquisition from the same thread.
- **C compatible**: While implemented in C++, a comprehensive C API is provided to allow these primitives to be used in pure C projects.

---

## Primitives

### 1. Scoped Critical Section (`sync::ScopedCriticalSection`)
An RAII-style low-level synchronization primitive that disables interrupts on the caller's CPU core and guards against concurrent access from other CPU cores in multi-core systems.
- **ISR-safe**: The only primitive (alongside `hw::CriticalSection`) safe for guarding code accessible by an ISR or another CPU core.
- **RAII**: Enters the critical section on construction and exits automatically when the object goes out of scope.
- **Always available**: Does not depend on `KERNEL_SYNC` configuration.
- **Use with care**: Has a global effect on the system — long sections will increase interrupt latency.

### 2. Mutex (`sync::Mutex`)
A recursive mutual exclusion primitive used to protect shared resources.
- **Features**: Supports `Lock`, `TryLock`, `Unlock`, and `TimedLock`.
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

### 3. Spinlock (`sync::SpinLock`)
A high-performance recursive spinlock for very short critical sections.
- **Features**: Supports `Lock`, `TryLock`, and `Unlock`.
- **Low Latency**: Bypasses the kernel wait-list logic for the "fast path" acquisition; suitable for sections where a context switch would be more expensive than spinning.
- **Recursive**: Allows the owning thread to acquire the lock multiple times without deadlocking. Max recursion depth is `0xFFFE`.

### 4. Condition Variable (`sync::ConditionVariable`)
Used in conjunction with a Mutex to wait for specific application states.
- **Features**: `Wait`, `NotifyOne`, and `NotifyAll`.
- **Real-time**: Releases mutex and suspends task atomically, ensuring no "lost wake-up" signals.
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

### 5. Event (`sync::Event`)
A binary signaling primitive supporting Auto-reset and Manual-reset modes.
- **Manual Reset**: Remains signaled until explicitly reset.
- **Auto Reset**: Resets automatically after waking a single waiting task.
- **Pulse**: Wakes waiting tasks and immediately resets the event (similar to Win32 API).
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

### 6. Semaphore (`sync::Semaphore`)
A counting signaling primitive used for resource tracking or producer-consumer patterns.
- **Direct Handover**: When semaphore is signaled, kernel immediately transfers the resource to the first waiting task (FIFO ordering).
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

### 7. Pipe (`sync::Pipe<T, Capacity>`)
A thread-safe FIFO communication channel for inter-task data passing, internally synchronized via a critical section and condition variables.
- **Template-based**: Supports any data type.
- **Bulk Operations**: Optimized `ReadBulk` and `WriteBulk` using `memcpy` for scalar types, with a per-element fallback for non-scalar types.
- **Blocking semantics**: `Write()` blocks if the pipe is full; `Read()` blocks if the pipe is empty, until the timeout expires.
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

### 8. Message Queue (`sync::MessageQueue` / `sync::MessageQueueT<N, MSG>`)
A fixed-capacity, fixed-message-size FIFO queue for inter-task communication over opaque byte messages.
- **Buffer flexibility**: `MessageQueue` operates over an externally supplied buffer; `MessageQueueT<N, MSG>` owns its storage internally with compile-time capacity and message size.
- **C-ABI friendly**: Message payload is always transferred via `memcpy`, so the message type does not need to be a C++ assignable type.
- **Blocking semantics**: `Put()` blocks if the queue is full; `Get()` blocks if the queue is empty, until the timeout expires.
- **Reset support**: `Reset()` discards all messages and wakes blocked producers.
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

### 9. Event Flags (`sync::EventFlags`)
A 32-bit multi-flag synchronization primitive for coordinating multiple independent events within a single object.
- **OR semantics** (`OPT_WAIT_ANY`): Unblocks when any one of the requested flag bits is set (default).
- **AND semantics** (`OPT_WAIT_ALL`): Unblocks only when all requested flag bits are simultaneously set.
- **Non-destructive read**: `Get()` returns a snapshot of the flags word without consuming any bit.
- **Selective clear**: By default matched bits are atomically cleared on a successful `Wait()`; pass `OPT_NO_CLEAR` to suppress this, allowing multiple concurrent waiters to each satisfy on the same `Set()`.
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

### 10. Reader-Writer Mutex (`sync::RWMutex`)
A synchronization primitive that allows multiple concurrent readers or one exclusive writer.
- **Writer Preference Policy**: Prevents writer starvation by blocking new readers when writers are waiting.
- **Shared Access**: Multiple tasks can acquire `ReadLock()` simultaneously for read-only operations.
- **Exclusive Access**: `Lock()` provides exclusive write access; blocks all other readers and writers.
- **Timeout Support**: `TimedReadLock()` and `TimedLock()` with configurable timeouts.
- **Low-Power Aware**: Waiting tasks are suspended by the kernel.

---

## ISR Safety

STK primitives follow strict rules for **Interrupt Service Routine (ISR)** contexts to ensure system stability.

The following operations are ISR-safe:
* **sync::Event**: `Set()`, `Pulse()`, `Reset()`, `TryWait()`
* **sync::EventFlags**: `Set()`, `Clear()`, `Get()`, `TryWait()`, `Wait(NO_WAIT)`
* **sync::Semaphore**: `Signal()`, `TryWait()`
* **sync::ConditionVariable**: `NotifyOne()`, `NotifyAll()`, `Wait(NO_WAIT)`
* **sync::Pipe**: `Write(NO_WAIT)`, `WriteBulk(NO_WAIT)`, `TryWrite()`, `TryWriteBulk()`, `Read(NO_WAIT)`, `ReadBulk(NO_WAIT)`, `TryRead()`, `TryReadBulk()`
* **sync::MessageQueue**: `Put(NO_WAIT)`, `TryPut()`, `Get(NO_WAIT)`, `TryGet()`

---

## Examples

* C++: [sync](https://github.com/dmitrykos/stk/tree/main/build/example/sync/example.cpp)
* C: [sync_c](https://github.com/dmitrykos/stk/tree/main/build/example/sync_c/example.c)

### Eclipse projects:
* STM32F407G-DISC1, C++: [sync-stm32f407g-disc1](https://github.com/dmitrykos/stk/tree/main/build/example/project/eclipse/stm/sync-stm32f407g-disc1)
* Raspberry RP2350 ARM Cortex-M (dual or single core cases), C++: [sync-rp2350w](https://github.com/dmitrykos/stk/tree/main/build/example/project/eclipse/rpi/sync-rp2350w)
* Raspberry RP2350 ARM Cortex-M (dual or single core cases), C: [sync_c-rp2350w](https://github.com/dmitrykos/stk/tree/main/build/example/project/eclipse/rpi/sync_c-rp2350w)
* Raspberry RP2350 RISC-V (dual or single core cases), C++: [sync-rp2350w-riscv](https://github.com/dmitrykos/stk/tree/main/build/example/project/eclipse/rpi/sync-rp2350w)

### C++ Usage Demo

```cpp
#include <sync/stk_sync.h>

stk::sync::Mutex      g_Mutex;
stk::sync::Event      g_DataReady(false); // auto-reset
stk::sync::EventFlags g_SensorFlags;

static const uint32_t FLAG_GPS = (1U << 0);
static const uint32_t FLAG_IMU = (1U << 1);

void TaskA() {
    g_Mutex.Lock();
    // ... access shared resource ...
    g_Mutex.Unlock();
    
    // signal TaskB
    g_DataReady.Set();
}

void TaskB() {
    // wait up to 1000ms for signaling
    if (g_DataReady.Wait(1000)) {
        // ... process data ...
    }
}

void ISR_GPS() {
    g_SensorFlags.Set(FLAG_GPS); // ISR-safe
}

void ISR_IMU() {
    g_SensorFlags.Set(FLAG_IMU); // ISR-safe
}

void TaskFusion() {
    // block until BOTH GPS and IMU flags are set simultaneously
    uint32_t raised = g_SensorFlags.Wait(FLAG_GPS | FLAG_IMU,
                                         stk::sync::EventFlags::OPT_WAIT_ALL, 5000);
    if (!stk::sync::EventFlags::IsError(raised)) {
        // ... process fused sensor data ...
    }
}
```

### Equivalent C Usage Demo

```c
#include <stk_c.h>

static stk_mutex_mem_t g_MtxMem;
static stk_event_mem_t g_EvtMem;
static stk_mutex_t *g_Mutex;
static stk_event_t *g_DataReady;

void Init() {
    g_Mutex = stk_mutex_create(&g_MtxMem, sizeof(g_MtxMem));
    g_DataReady = stk_event_create(&g_EvtMem, sizeof(g_EvtMem), false); // auto-reset
}

void TaskA(void *arg) {
    stk_mutex_lock(g_Mutex);    
    // ... access shared resource ...    
    stk_mutex_unlock(g_Mutex);
    
    // signal TaskB
    stk_event_set(g_DataReady);
}

void TaskB(void *arg) {
    while (true) {
        // wait up to 1000ms for signaling
        if (stk_event_wait(g_DataReady, 1000)) {
            // ... process data ...
        }
    }
}
```