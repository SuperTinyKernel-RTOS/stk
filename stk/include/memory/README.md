# STK Memory Module (`stk::memory`)

**STK Memory Module** provides deterministic, fragmentation-free memory allocation primitives for embedded systems. It is designed for scenarios where dynamic heap allocation is undesirable or prohibited — offering fixed-size block pools with O(1) alloc/free, full ISR safety, and task-blocking semantics backed by the STK kernel.

## Features

- **Zero Fragmentation**: Fixed-size block allocator guarantees no heap fragmentation over time.
- **O(1) Alloc and Free**: Intrusive singly-linked free-list provides constant-time allocation and release with a minimal critical section.
- **Two Storage Modes**: Pools can be backed by caller-supplied static storage (zero heap) or by heap-allocated storage, selectable per instance.
- **Blocking Semantics**: `Alloc()` and `TimedAlloc()` suspend the calling task via a `ConditionVariable` until a block becomes available, giving up the CPU completely rather than spin-waiting.
- **ISR Safety**: `TryAlloc()` and `Free()` are ISR-safe. Blocking paths are task-context only.
- **Typed API**: Template wrappers (`AllocT<T>()`, `TryAllocT<T>()`, etc.) eliminate manual casts and assert type–size compatibility at allocation time.
- **Debug Diagnostics**: Double-free detection (O(n) free-list walk), bounds checking, and alignment validation in debug builds; all checks compile away in release.

---

## Classes

### 1. Block Memory Pool (`memory::BlockMemoryPool`)

A fixed-size block allocator for scenarios where the same block size is repeatedly allocated and released — such as packet buffers, sensor sample records, or task-local state objects.

Internally, the pool maintains an intrusive singly-linked free-list inside the storage array itself. When a block is free, its first `sizeof(void*)` bytes hold a pointer to the next free block — no separate metadata array is needed.

**Memory layout (contiguous storage):**

```
[ block_0 | block_1 | ... | block_{n-1} ]
^                                       ^
m_storage               (m_storage + n × aligned_block_size)
```

At construction all blocks are chained: `block_i->next = block_{i+1}`, `last->next = nullptr`. `m_free_list` points to `block_0`.

**Storage modes:**

| Mode | Constructor | Storage ownership |
|---|---|---|
| External (static/stack) | `BlockMemoryPool(cap, blksz, buf, bufsz [, name])` | Caller — never freed by pool |
| Heap-allocated | `BlockMemoryPool(cap, blksz [, name])` | Pool — freed in destructor |

> **Note**: When using heap storage without exceptions (the typical embedded configuration), always call `IsStorageValid()` immediately after construction to detect allocation failure before first use.

**Configuration constants:**

| Constant | Value | Description |
|---|---|---|
| `CAPACITY_MAX` | `0xFFFE` | Maximum number of blocks per pool. |

---

#### Constructor — External Storage

```cpp
explicit BlockMemoryPool(size_t capacity, size_t raw_block_size,
                         uint8_t *storage, size_t storage_size,
                         const char *name = nullptr);
```

Constructs a pool over caller-supplied storage. The pool holds a reference to `storage` without taking ownership. The buffer must be aligned to at least `sizeof(void*)` and sized to hold at least `capacity × AlignBlockSize(raw_block_size)` bytes — asserted at construction time.

---

#### Constructor — Heap Storage

```cpp
explicit BlockMemoryPool(size_t capacity, size_t raw_block_size,
                         const char *name = nullptr);
```

Allocates a flat byte buffer via `operator new (std::nothrow)`. On destruction the buffer is freed automatically. Check `IsStorageValid()` after construction when exceptions are disabled.

---

#### Allocation Methods

| Method | Blocking | ISR-safe | Description |
|---|---|---|---|
| `Alloc()` | Yes — indefinite | No | Blocks until a block is available. Never returns `nullptr`. |
| `AllocT<T>()` | Yes — indefinite | No | Typed `Alloc()`. Asserts `sizeof(T) <= block_size`. |
| `TimedAlloc(timeout)` | Yes — until timeout | Only with `NO_WAIT` | Blocks up to `timeout` ticks. Returns `nullptr` on timeout. |
| `TimedAllocT<T>(timeout)` | Yes — until timeout | Only with `NO_WAIT` | Typed `TimedAlloc()`. |
| `TryAlloc()` | No | Yes | Returns a block or `nullptr` immediately. Never suspends. |
| `TryAllocT<T>()` | No | Yes | Typed `TryAlloc()`. |

> `WAIT_INFINITE` and `NO_WAIT` are standard STK timeout constants. Passing `NO_WAIT` to `TimedAlloc()` is equivalent to calling `TryAlloc()`.

---

#### Free

```cpp
bool Free(void *ptr);
```

Returns a previously allocated block to the pool in O(1). Pushes the block onto the free-list head and wakes exactly one task blocked inside `Alloc()` or `TimedAlloc()`, if any. ISR-safe.

Returns `true` on success. Returns `false` (and triggers an assertion in debug builds) if `ptr` is `nullptr`, out of the pool's range, or misaligned. Double-free detection is active in debug builds only (O(n) free-list walk).

> **Best practice**: Null the pointer after `Free()` to prevent accidental double-free.

---

#### Utility / Query Methods

| Method | ISR-safe | Description |
|---|---|---|
| `static AlignBlockSize(raw_size)` | Yes | Rounds `raw_size` up to the nearest `BLOCK_ALIGN` multiple. Use to size external storage buffers. |
| `IsStorageValid()` | Yes | Returns `true` if the backing storage is valid. Always `true` for external storage; check after heap constructor. |
| `GetCapacity()` | Yes | Total block capacity of the pool. |
| `GetBlockSize()` | Yes | Aligned block size in bytes (`>= BLOCK_ALIGN`). |
| `GetUsedCount()` | Yes | Number of currently allocated (outstanding) blocks. Advisory snapshot. |
| `GetFreeCount()` | Yes | Number of available blocks (`capacity - used`). Advisory snapshot. |
| `IsFull()` | Yes | Returns `true` if no blocks are available. |
| `IsEmpty()` | Yes | Returns `true` if no blocks are currently allocated. |

---

## Usage Examples

### External static storage — zero heap, ISR receiver

```cpp
#include <memory/stk_memory.h>
#include <sync/stk_sync_msgqueue.h>

static const uint32_t PKT_COUNT = 8U;
static const uint32_t PKT_SIZE  = sizeof(Packet);

// Compute the required storage size at compile time
alignas(sizeof(void *)) static uint8_t
    g_PktStorage[PKT_COUNT * stk::memory::BlockMemoryPool::AlignBlockSize(PKT_SIZE)];

stk::memory::BlockMemoryPool g_PktPool(PKT_COUNT, PKT_SIZE,
                                       g_PktStorage, sizeof(g_PktStorage));

// Queue carries raw block pointers (void*) between ISR and parser task.
// Capacity matches the pool so a pointer can always be enqueued after a
// successful TryAlloc().
stk::sync::MessageQueueT<PKT_COUNT, sizeof(void *)> g_ParseQueue;

void ISR_Receiver()
{
    void *blk = g_PktPool.TryAlloc();        // ISR-safe, non-blocking
    if (blk)
    {
        FillPacket(static_cast<Packet *>(blk));
        g_ParseQueue.TryPut(&blk);           // ISR-safe, non-blocking
    }
}

void Task_Parser()
{
    void *blk = nullptr;
    if (g_ParseQueue.Get(&blk))              // blocks until a pointer arrives
    {
        Parse(static_cast<Packet *>(blk));
        g_PktPool.Free(blk);                 // O(1), wakes one blocked allocator
        blk = nullptr;
    }
}
```

### Typed API — blocking allocation from task context

```cpp
#include <memory/stk_memory.h>
#include <sync/stk_sync_msgqueue.h>

static const uint32_t SENSOR_BUF_COUNT = 4U;

alignas(sizeof(void *)) static uint8_t
    g_SensorStorage[SENSOR_BUF_COUNT *
                    stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(SensorRecord))];

stk::memory::BlockMemoryPool g_SensorPool(SENSOR_BUF_COUNT, sizeof(SensorRecord),
                                          g_SensorStorage, sizeof(g_SensorStorage));

// Queue carries typed pointers (SensorRecord*) from the acquire task to the
// processing task.  Capacity matches the pool so Put() never blocks while
// the pool has free blocks.
stk::sync::MessageQueueT<SENSOR_BUF_COUNT, sizeof(SensorRecord *)> g_ProcessQueue;

void Task_Acquire()
{
    while (true)
    {
        // Block until a record slot is available — no CPU spin
        SensorRecord *rec = g_SensorPool.AllocT<SensorRecord>();

        ReadSensor(rec);
        g_ProcessQueue.Put(&rec);            // blocks until the processing task drains
    }
}

void Task_Process()
{
    SensorRecord *rec = nullptr;
    if (g_ProcessQueue.Get(&rec))            // blocks until a record is available
    {
        ProcessRecord(rec);
        g_SensorPool.Free(rec);
        rec = nullptr;
    }
}
```

### Heap-allocated pool with validity check

```cpp
#include <memory/stk_memory.h>

// 16 blocks of 64 bytes, heap-owned
stk::memory::BlockMemoryPool g_Pool(16U, 64U, "DataPool");

void Init()
{
    // Always check after heap constructor when exceptions are disabled
    STK_ASSERT(g_Pool.IsStorageValid());
}
```

### Timed allocation with timeout

```cpp
// Try to acquire a block for up to 100 ticks; proceed only if successful
void *blk = g_Pool.TimedAlloc(stk::GetTicksFromMsec(100));
if (blk != nullptr)
{
    UseBlock(blk);
    g_Pool.Free(blk);
    blk = nullptr;
}
else
{
    // Pool was exhausted for the entire timeout window
    ReportOverflow();
}
```

---

## Storage Sizing Reference

Use `AlignBlockSize()` to compute the correct external buffer size:

```cpp
// General form
uint8_t buf[N * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(MyType))];

// Concrete example: 8 blocks of Packet
alignas(sizeof(void *)) static uint8_t
    g_Buf[8U * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(Packet))];
```

> The buffer must be aligned to at least `sizeof(void*)`. The `alignas` specifier shown above guarantees this on all STK targets.

---

## ISR Safety Summary

| Operation | ISR-safe |
|---|---|
| `TryAlloc()` / `TryAllocT<T>()` | Yes |
| `Free()` | Yes |
| `TimedAlloc(NO_WAIT)` | Yes (equivalent to `TryAlloc()`) |
| `Alloc()` / `AllocT<T>()` | **No** — task context only |
| `TimedAlloc(timeout > 0)` | **No** — task context only |
| Constructors / Destructor | **No** — init/task context only |

---

## Notes and Caveats

- **Lifetime**: Destroying a pool while tasks are blocked inside `Alloc()` or `TimedAlloc()` is a logical error. An assertion is triggered in debug builds by the internal `ConditionVariable` destructor.
- **Double-free**: Debug builds perform an O(n) free-list walk to detect double-free. Release builds do not — null pointers after `Free()` to avoid silent corruption.
- **Blocking paths require `KERNEL_SYNC`**: `Alloc()` and `TimedAlloc()` (with a non-zero timeout) depend on the `ConditionVariable` primitive and require the kernel to be compiled with `KERNEL_SYNC`. `TryAlloc()` and `Free()` are always available regardless of kernel mode.
- **Non-copyable**: `BlockMemoryPool` is non-copyable and non-movable.
