# STK Memory Module (`stk::memory`)

**STK Memory Module** provides memory allocation primitives for embedded systems. It is designed for scenarios where dynamic heap allocation is undesirable or prohibited — offering fixed-size block pools with deterministic O(1) alloc/free (`BlockMemoryPool`) and variable-size byte pools with block coalescing (`BytePool`), both with ISR-safe non-blocking paths and task-blocking semantics backed by the STK kernel. A thin `MemoryAllocator` interface lets the rest of the code (and, optionally, the standard C++ `new` / `delete`) use either a heap of your choice or a deterministic static one.

## Features

- **Zero Fragmentation**: Fixed-size block allocator guarantees no heap fragmentation over time.
- **O(1) Alloc and Free**: Intrusive singly-linked free-list provides constant-time allocation and release with a minimal critical section.
- **Three Storage Modes**: Pools can embed their own storage (`StaticBlockPool`, `StaticBytePool` — zero heap, nothing to size by hand), use caller-supplied static storage, or use heap-allocated storage, selectable per instance.
- **Blocking Semantics**: `Alloc()` and `TimedAlloc()` suspend the calling task via a `ConditionVariable` until a block becomes available, giving up the CPU completely rather than spin-waiting.
- **ISR Safety**: `TryAlloc()` and `Free()` are ISR-safe. Blocking paths are task-context only.
- **Typed API**: Template wrappers (`AllocT<T>()`, `TryAllocT<T>()`, etc.) eliminate manual casts and refuse types that do not fit the block size or alignment.
- **Object Helpers and `PoolPtr`**: `NewT<T>(args...)` / `DeleteT()` construct and destroy objects in pool memory, and `MakeT<T>()` returns an owning `PoolPtr` that returns the object to its pool automatically.
- **Variable-Size Allocation**: `BytePool` serves requests of arbitrary size from a single area (first-fit, with block splitting and coalescing) when fixed-size blocks do not fit the workload.
- **Pool Sizing Diagnostics**: High watermarks, failed-request counters and a common `MemoryAllocator::Stats` snapshot (`GetStats()`) tell you how big a pool really needs to be; `BytePool` also reports the longest block-list walk, i.e. its worst-case critical section.
- **Debug Diagnostics**: Double-free detection, bounds and alignment validation, payload fill patterns, optional overrun guard words and an optional O(1) release-build double-free bitmap (see [Configuration Macros](#configuration-macros)).
- **Standard `new` / `delete` (opt-in)**: With `STK_MEMORY_OVERRIDE_NEW_DELETE=1` the global C++ allocation operators are routed to the STK allocator.

---

## Classes

### 1. Block Memory Pool (`memory::BlockMemoryPool`)

A fixed-size block allocator for scenarios where the same block size is repeatedly allocated and released — such as packet buffers, sensor sample records, or task-local state objects.

Internally, the pool maintains an intrusive singly-linked free-list inside the storage array itself. When a block is free, its first `sizeof(void*)` bytes hold a pointer to the next free block — no separate metadata array is needed.

**Memory layout (contiguous storage):**

```
[ block_0 | block_1 | ... | block_{n-1} ][ bitmap (optional) ]
^                                       ^
m_blocks                (m_blocks + n × aligned_block_size)
```

At construction all blocks are chained: `block_i->next = block_{i+1}`, `last->next = nullptr`. The free-list head points to `block_0`.

**Alignment:** every block starts on a multiple of the pool alignment — `ALIGN_DEFAULT` (8 bytes, 16 on RISC-V, so `double` / `uint64_t` members are safe on 32-bit targets) unless a different value is passed as the last constructor argument or set globally with `STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT`. Use a larger value for cache-line / DMA / SIMD buffers. A misaligned external buffer is corrected internally: the unaligned head is skipped and the capacity is trimmed to what fits (asserted in debug builds), so it never causes an unaligned access.

**Storage modes:**

| Mode | Constructor / type | Storage ownership |
|---|---|---|
| Embedded (zero heap) | `StaticBlockPool<blksz, cap [, align]>` | The pool object itself |
| External (static/stack) | `BlockMemoryPool(cap, blksz, buf, bufsz [, name [, align]])` | Caller — never freed by pool |
| Heap-allocated | `BlockMemoryPool(cap, blksz [, name [, align]])` | Pool — freed in destructor |

> **Note**: When using heap storage without exceptions (the typical embedded configuration), always call `IsStorageValid()` immediately after construction to detect allocation failure before first use. An invalid pool never blocks: every allocation fails immediately.

> **Note**: Do not create *global* pool objects with the heap constructor — they are constructed before the heap is usable (static initialization order). Use `StaticBlockPool` or external storage for globals.

**Configuration constants:**

| Constant | Value | Description |
|---|---|---|
| `CAPACITY_MAX` | `0xFFFE` | Maximum number of blocks per pool. |
| `ALIGN_DEFAULT` | `8` (`16` on RISC-V) | Default block alignment in bytes (`STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT`). |

---

#### Constructor — External Storage

```cpp
explicit BlockMemoryPool(size_t capacity, size_t raw_block_size,
                         uint8_t *storage, size_t storage_size,
                         const char *name = nullptr,
                         size_t alignment = ALIGN_DEFAULT);
```

Constructs a pool over caller-supplied storage. The pool holds a reference to `storage` without taking ownership. The buffer should be aligned to `alignment` and sized to hold at least `RequiredStorageSize(capacity, raw_block_size, alignment)` bytes — asserted at construction time. If the buffer is smaller, the capacity is trimmed to what fits; if it cannot hold even one block (or an argument is invalid), the pool is left invalid (`IsStorageValid() == false`).

---

#### Constructor — Heap Storage

```cpp
explicit BlockMemoryPool(size_t capacity, size_t raw_block_size,
                         const char *name = nullptr,
                         size_t alignment = ALIGN_DEFAULT);
```

Allocates the storage via `MemoryAllocator::AllocateAligned` (non-throwing, overflow-checked, start aligned to the pool alignment). On destruction the buffer is freed automatically with `FreeAligned`. Check `IsStorageValid()` after construction when exceptions are disabled. Not available when `STK_MEMORY_NO_HEAP` is `1`.

---

#### Allocation Methods

| Method | Blocking | ISR-safe | Description |
|---|---|---|---|
| `Alloc()` | Yes — indefinite | No | Blocks until a block is available. Returns `nullptr` only if the wait is cancelled or the pool is invalid. |
| `AllocT<T>()` | Yes — indefinite | No | Typed `Alloc()`. Returns `nullptr` if `T` does not fit the block size or alignment. |
| `TimedAlloc(timeout)` | Yes — until timeout | Only with `NO_WAIT` | Blocks up to `timeout` ticks. Returns `nullptr` on timeout. |
| `TimedAllocT<T>(timeout)` | Yes — until timeout | Only with `NO_WAIT` | Typed `TimedAlloc()`. |
| `TimedAllocEx(&block, timeout)` | Yes — until timeout | Only with `NO_WAIT` | Returns the full `EWaitResult` so timeout and cancellation can be told apart. |
| `TryAlloc()` | No | Yes | Returns a block or `nullptr` immediately. Never suspends and never touches the condition variable. |
| `TryAllocT<T>()` | No | Yes | Typed `TryAlloc()`. |

> `WAIT_INFINITE` and `NO_WAIT` are standard STK timeout constants. Passing `NO_WAIT` to `TimedAlloc()` is equivalent to calling `TryAlloc()`.

The memory returned by these methods is **uninitialized**. To allocate *and construct* an object, use the [object helpers](#4-object-helpers-and-poolptr).

---

#### Free

```cpp
bool Free(void *ptr);
```

Returns a previously allocated block to the pool in O(1). Pushes the block onto the free-list head and wakes exactly one task blocked inside `Alloc()` or `TimedAlloc()`, if any. ISR-safe.

Returns `true` on success. Returns `false` (and triggers an assertion in debug builds) if `ptr` is `nullptr`, out of the pool's range, misaligned, or already free. How reliably a double-free is detected depends on the configuration:

| Configuration | Double-free detection |
|---|---|
| `STK_MEMORY_BLOCKPOOL_BITMAP=1` | Always, O(1), including release builds. |
| `STK_MEMORY_POOL_CHECKS=1` | O(n) free-list walk (default in `_DEBUG` / `DEBUG` builds). |
| neither | Only when the pool is already completely free. |

> **Best practice**: Null the pointer after `Free()` to prevent accidental double-free.

---

#### Utility / Query Methods

| Method | ISR-safe | Description |
|---|---|---|
| `static AlignBlockSize(raw_size [, alignment])` | Yes | Rounds `raw_size` up to the nearest multiple of `alignment` (at least `alignment`). |
| `static RequiredStorageSize(cap, raw_size [, alignment])` | Yes | Buffer size needed for an external-storage pool (aligned blocks plus the optional bitmap). Add `alignment - 1` if the buffer is not guaranteed to be aligned. |
| `IsStorageValid()` | Yes | Returns `true` if the pool is ready for use. `false` after a failed heap allocation, invalid arguments, or a buffer too small for one block. |
| `GetCapacity()` | Yes | Total block capacity of the pool (may be lower than requested if the buffer was too small; `0` if invalid). |
| `GetBlockSize()` | Yes | Aligned block size in bytes. |
| `GetBlockAlignment()` | Yes | Alignment of every block in bytes. |
| `GetUsedCount()` | Yes | Number of currently allocated (outstanding) blocks. Advisory snapshot. |
| `GetFreeCount()` | Yes | Number of available blocks (`capacity - used`). Advisory snapshot. |
| `IsFull()` | Yes | Returns `true` if no blocks are available. |
| `IsEmpty()` | Yes | Returns `true` if no blocks are currently allocated. |
| `GetPeakUsedCount()` | Yes | High watermark: maximum number of blocks allocated at the same time. |
| `GetAllocFailCount()` | Yes | Number of requests that failed because the pool was exhausted (timeouts and `NO_WAIT` misses). |
| `GetStats()` | Yes | Consistent [`MemoryAllocator::Stats`](#statistics) snapshot (in bytes). |

---

### 2. Byte Memory Pool (`memory::BytePool`)

A variable-size allocator for workloads where allocation sizes vary — message payloads, strings, protocol frames. It trades the constant-time guarantee and fragmentation immunity of `BlockMemoryPool` for flexibility: allocation time depends on the number of blocks scanned, and the pool can fragment.

The pool keeps an implicit block list inside the storage area. Every block, free or used, is preceded by a header of `HEADER_SIZE` bytes (8 by default, 16 on RISC-V: `size` + state marker), and blocks follow each other back to back — no separate metadata is needed. Returned pointers are aligned to `BLOCK_ALIGN` (8 bytes by default, 16 on RISC-V).

**Memory layout (contiguous storage):**

```
[ hdr | payload ][ hdr | payload (free) ][ hdr | payload ] ...
^                                                          ^
m_first                                                    m_end
```

**Allocation policy:** first-fit (or next-fit with `STK_MEMORY_BYTEPOOL_NEXT_FIT=1`). While scanning, adjacent free blocks are merged, and a found block is split when the remainder can form a block of its own. `Free()` merges the freed block with the free blocks that follow it; merging with a preceding free block is done lazily by the next scan.

**Alignment:** `BLOCK_ALIGN` is a compile-time setting (`STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT`, a power of two ≥ 4, default 8 — 16 on RISC-V) because the payload directly follows the header: `HEADER_SIZE` is `max(8, BLOCK_ALIGN)`, so with 16/32/64-byte alignment every allocation costs that many bytes of overhead. Alignment 4 saves padding but gives only 4-byte-aligned payloads.

**Storage modes:**

| Mode | Constructor / type | Storage ownership |
|---|---|---|
| Embedded (zero heap) | `StaticBytePool<size>` | The pool object itself |
| External (static/stack) | `BytePool(buf, bufsz [, name])` | Caller — never freed by pool |
| Heap-allocated | `BytePool(bufsz [, name])` | Pool — freed in destructor |

> **Note**: As with `BlockMemoryPool`, call `IsStorageValid()` after construction with heap storage. It also returns `false` if the supplied external buffer is too small to hold even one block. A misaligned external buffer is corrected (the unaligned head and tail bytes are not used).

---

#### Allocation Methods

| Method | Blocking | ISR-safe | Description |
|---|---|---|---|
| `Alloc(size)` | Yes — indefinite | No | Blocks until `size` bytes are available. Returns `nullptr` only on cancellation or an invalid request. |
| `AllocT<T>()` | Yes — indefinite | No | Typed `Alloc()` for `sizeof(T)` bytes (`alignof(T) <= BLOCK_ALIGN`, checked at compile time). |
| `TimedAlloc(size, timeout)` | Yes — until timeout | Only with `NO_WAIT` | Blocks up to `timeout` ticks. Returns `nullptr` on timeout. |
| `TimedAllocT<T>(timeout)` | Yes — until timeout | Only with `NO_WAIT` | Typed `TimedAlloc()`. |
| `TimedAllocEx(&block, size, timeout)` | Yes — until timeout | Only with `NO_WAIT` | Returns the full `EWaitResult` so timeout and cancellation can be told apart. |
| `TryAlloc(size)` | No | Yes | Returns a block or `nullptr` immediately. |
| `TryAllocT<T>()` | No | Yes | Typed `TryAlloc()`. |

> A request of zero bytes, or larger than `GetMaxRequest()`, can never succeed and fails immediately instead of blocking. The timeout is a total budget: repeated wake-ups do not extend it. The memory returned is **uninitialized**; see the [object helpers](#4-object-helpers-and-poolptr) to construct objects.

---

#### Free

```cpp
bool Free(void *ptr);
```

Marks the block free, merges it with following free blocks, and wakes **all** tasks blocked in `Alloc()` / `TimedAlloc()` so each can re-check its own request (requests differ in size, so waking only one could leave a satisfiable request asleep). ISR-safe, but not constant time.

Returns `false` (and triggers an assertion in debug builds) if `ptr` is `nullptr`, outside the pool, misaligned, or not in use. Double-free is caught through the block's state marker in every build; builds with `STK_MEMORY_POOL_CHECKS=1` additionally verify that the pointer is the start of a real block. With `STK_MEMORY_BYTEPOOL_GUARD=1` a payload overrun (overwritten guard word) is also reported and the damaged block is kept rather than merged into a possibly corrupted list.

> **Best practice**: Null the pointer after `Free()`. Double-free detection is reliable only until the memory is handed out again.

---

#### Utility / Query Methods

| Method | ISR-safe | Description |
|---|---|---|
| `static AlignSize(size)` | Yes | Rounds `size` up to a multiple of `BLOCK_ALIGN`. |
| `static RequiredSize(payload)` | Yes | Pool space consumed by one allocation: aligned payload (plus the guard word if enabled) plus the header. Use to size external buffers. |
| `IsStorageValid()` | Yes | Returns `true` if the pool is ready for use. |
| `GetSize()` | Yes | Managed (aligned) area size in bytes, headers included. |
| `GetMaxRequest()` | Yes | Largest payload that can ever be allocated (one block spanning the whole pool). |
| `GetInfo(available, largest, fragments)` | Yes (O(n)) | Consistent snapshot: total allocatable payload, largest allocatable request right now, and the number of free fragments. |
| `GetStats()` | Yes (O(1)) | Consistent [`MemoryAllocator::Stats`](#statistics) snapshot; sizes include block headers. |
| `GetPeakUsedSize()` | Yes | High watermark of pool consumption in bytes (headers included). |
| `GetAllocFailCount()` | Yes | Number of requests that could not be satisfied: timeouts, `NO_WAIT` misses and requests larger than `GetMaxRequest()` (zero-sized requests are not counted). |
| `GetMaxScanLength()` | Yes | Longest block-list walk (in blocks) of any allocation so far. The walk runs inside the critical section, so this measures the allocator's worst-case interrupt/scheduler blocking time — use it to decide whether an ISR may call `TryAlloc()`. |

Constants: `BLOCK_ALIGN`, `HEADER_SIZE`.

---

### 3. Zero-Heap Pools (`memory::StaticBlockPool`, `memory::StaticBytePool`)

Pools that own their storage: no heap, and no separately declared buffer that can be mis-sized or misaligned — size and alignment are computed at compile time.

```cpp
template <size_t TBlockSize, size_t TCapacity, size_t TAlign = BlockMemoryPool::ALIGN_DEFAULT>
class StaticBlockPool : public BlockMemoryPool;

template <typename T, size_t N>
using StaticBlockPoolFor = StaticBlockPool<sizeof(T), N, /* max(alignof(T), ALIGN_DEFAULT) */>;

template <size_t TSize>
class StaticBytePool : public BytePool;     // TSize = managed area in bytes, headers included
```

Both derive from the pool class, so the complete API above is available. Their constructor takes only an optional trace name. `StaticBlockPoolFor<T, N>` picks the right block size *and* alignment for a type (available when `STK_MEMORY_PLACEMENT_NEW` is `1`, the default). To place the object in a specific RAM region (CCM, SDRAM, DMA memory), apply your toolchain's section attribute to the variable.

---

### 4. Object Helpers and `PoolPtr`

Both pools can allocate **and construct** objects (placement `new`, requires `STK_MEMORY_PLACEMENT_NEW=1`, the default) instead of returning raw memory:

| Method | Blocking | ISR-safe | Description |
|---|---|---|---|
| `TryNewT<T>(args...)` | No | If the constructor is | Allocate and construct, `nullptr` if the pool is exhausted. |
| `NewT<T>(args...)` | Yes — indefinite | No | Blocks until memory is available, then constructs. |
| `TimedNewT<T>(timeout, args...)` | Yes — until timeout | Only with `NO_WAIT` | Blocks up to `timeout` ticks. |
| `DeleteT(ptr)` | — | If the destructor is | Runs `~T()` and returns the memory. `nullptr` is a no-op. |
| `TryMakeT<T>(args...)` | No | If the constructor is | Like `TryNewT`, but returns an owning `PoolPtr`. |
| `MakeT<T>(args...)` | Yes — indefinite | No | Like `NewT`, but returns an owning `PoolPtr`. |

Nothing is constructed when the allocation fails. Arguments are always honored, even for trivially constructible types (`TryNewT<uint32_t>(5U)` stores `5`); with no arguments the object is value-initialized.

`PoolPtr<T, TPool>` is a minimal move-only owning pointer: it destroys the object and returns its memory to the pool when it goes out of scope, which prevents leaks and double-frees. It provides `Get()`, `operator->`, `operator*`, `explicit operator bool`, `Reset()` and `Release()` (hand ownership back to the caller, who then calls `DeleteT()`). It works with any type that provides `DeleteT()` (both pools).

---

### 5. Memory Allocator (`memory::MemoryAllocator`)

STK itself does not allocate dynamically, but some auxiliary classes (heap-storage pool constructors, the typed helpers below) do. The allocator has two layers:

- **Backend — `MemoryAllocator::Weak`**: a tiny **weak** interface that *you* implement, or get a ready-made one from `stk_memory_allocator_default.h` / `stk_memory_allocator_malloc.h`. It is not meant to be called directly.
- **Front-end — `MemoryAllocator::Allocate()`, `Free()`, `GetStats()`** and all typed/aligned helpers: checked wrappers that verify a backend is linked in before forwarding to it. Application code (and the rest of the STK memory module) uses only this layer.

```cpp
// Backend hooks (you implement these)
struct MemoryAllocator::Weak
{
    static void *Allocate(size_t size);   // aligned to at least MemoryAllocator::MIN_ALIGNMENT (8; 16 on RISC-V)
    static void  Free(void *ptr);         // nullptr is a no-op
    static Stats GetStats();              // optional
};

// Checked front-end (you call these)
static void *MemoryAllocator::Allocate(size_t size);   // nullptr if the allocator failed or no backend is available
static void  MemoryAllocator::Free(void *ptr);         // nullptr is a no-op
static Stats MemoryAllocator::GetStats();              // empty Stats if the backend provides none
static bool  MemoryAllocator::IsAvailable();           // true if Weak::Allocate() and Weak::Free() are linked in
```

> **Warning**: STK does not define the `Weak` hooks. If none are linked in, on most toolchains the symbols resolve to address 0 and calling them directly faults at run time — it is *not* a linker error. The front-end protects against this: `Allocate()` / `Free()` and everything built on them assert (debug) and fail gracefully otherwise (`Allocate()` returns `nullptr`, `Free()` does nothing, `GetStats()` returns empty `Stats`). `IsAvailable()` lets you check explicitly. Never call `Weak::*` yourself, as that bypasses the check.

**Contract for backend implementations:** `Weak::Allocate()` / `Weak::Free()` must be thread-safe (with newlib's `malloc`, implement `__malloc_lock`), must not be called from an ISR unless your implementation allows it, `Weak::Allocate(0)` may return `nullptr`, and `Weak::Free(nullptr)` must be a no-op.

```cpp
// Your trivial implementation:
void *MemoryAllocator::Weak::Allocate(size_t size) { return malloc(size); }
void  MemoryAllocator::Weak::Free(void *ptr)       { free(ptr); }
```

#### Default deterministic implementation

Include the optional header in **exactly one** translation unit to get an implementation backed by a static `BytePool` (no C library heap, no `__malloc_lock`, no linker heap region):

```cpp
#define STK_MEMORY_DEFAULT_HEAP_SIZE 16384U          // default 8192
#include <memory/stk_memory_allocator_default.h>
```

It defines the `MemoryAllocator::Weak` hooks. `Allocate()` is non-blocking (returns `nullptr` when exhausted), thread-safe, usable from an ISR (with `BytePool`'s non-constant execution time), returns memory aligned to at least `MemoryAllocator::MIN_ALIGNMENT` (8 bytes, 16 on RISC-V; enforced at compile time against `STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT`), and creates its pool on first use, so it has no static-initialization-order problem. `GetStats()` reports the heap usage.

#### C library `malloc` implementation

If you prefer the C library heap, include `stk_memory_allocator_malloc.h` in **exactly one** translation unit instead (never together with the default one). It defines the `Weak` hooks as thin wrappers around `malloc()` / `free()`: the heap is whatever your C library and linker script provide, thread-safety depends on the C library lock hooks (e.g. `__malloc_lock` in newlib), ISR safety is only as good as your `malloc`, and `GetStats()` returns empty `Stats` (`malloc` exposes no statistics).

#### Typed and aligned helpers

| Function | Description |
|---|---|
| `AllocateOneT<T>(args...)` | Allocate one element and construct it in place (`nullptr` on failure). |
| `AllocateArrayT<T>(count, args...)` | Allocate and construct an array; arguments are copied into every element. Overflow of `count * sizeof(T)` is rejected. |
| `AllocateTrivialArrayT<T>(count)` | Allocate an array of **trivial** types (`std::is_trivial`, checked at compile time) as raw, uninitialized memory — no construction loop, like `malloc`. Overflow of `count * sizeof(T)` is rejected. |
| `FreeOneT(ptr)` / `FreeArrayT(ptr, count)` | Destroy (array: in reverse order) and free. `nullptr` is a no-op. |
| `FreeTrivialArrayT(ptr)` | Free memory obtained from `AllocateTrivialArrayT()`: no destructor loop (plain `Free()` is equivalent). `nullptr` is a no-op. |
| `AllocateAligned(size, alignment)` | Allocation with an alignment above `MIN_ALIGNMENT` (e.g. a 32-byte DMA buffer). Pair with `FreeAligned()`, never `Free()`. |
| `FreeAligned(ptr)` | Free memory obtained from `AllocateAligned()`. |

Construction is skipped only when there are no arguments *and* `T` is trivially default constructible (memory left uninitialized, like `malloc`); in every other case the object is constructed. `T` must not require alignment above `MIN_ALIGNMENT` (compile-time check) — use `AllocateAligned()` for those. For plain data arrays that need no construction, `AllocateTrivialArrayT()` states that intent explicitly (checked at compile time) and, unlike `FreeArrayT()`, needs no `count` on release. These helpers are available when `STK_MEMORY_NO_HEAP` is `0` (and placement `new` is enabled).

#### Statistics

`MemoryAllocator::Stats` is the common statistics structure of the allocator interface **and** of both pools (`GetStats()`):

| Field | Description |
|---|---|
| `capacity` | Total capacity in bytes. |
| `allocated` | Bytes currently allocated. |
| `peak_allocated` | High watermark of `allocated`. |
| `allocate_count` / `free_count` | Number of successful allocations / frees. |
| `alloc_fail_count` | Number of requests that could not be satisfied. |
| `min_ever_free` | Minimum free bytes ever recorded (`capacity - peak_allocated`). |

Helper methods: `GetCapacity()`, `GetAvailable()`, `RecordAllocate(size)`, `RecordFree(size)`, `RecordAllocFail()`, `Reset(capacity)`. A default-constructed `Stats` has zero capacity; pass the capacity to the constructor (or call `Reset()`). The `Record*()` methods are not atomic — call them with your allocator's lock held. Units: `BlockMemoryPool` reports `blocks × block size`; `BytePool` reports bytes consumed **including block headers**.

#### Global `new` / `delete` (opt-in)

Build with `-DSTK_MEMORY_OVERRIDE_NEW_DELETE=1` and every global C++ allocation — `new`, `new[]`, `delete`, `delete[]`, the `std::nothrow`, sized and C++17 over-aligned forms, and therefore `std::string`, `std::vector`, `std::make_unique` — uses `MemoryAllocator::Allocate()` / `Free()`:

- `new(0)` returns a unique non-null pointer; on failure the installed `std::new_handler` is called and the allocation retried.
- Throwing `new` then throws `std::bad_alloc`, or — with exceptions disabled — calls `STK_MEMORY_NEW_OOM_ACTION()` (default `std::terminate()`, like libstdc++). This hook **must not return**; define it to your panic/reset handler. The `std::nothrow` forms return `nullptr`.
- Over-aligned types use `AllocateAligned()` / `FreeAligned()`.
- The operators are emitted **weak** in every translation unit that includes the header (the standard forbids `inline` replacements), so enabling the macro in the build system is safe, and a strong definition elsewhere takes precedence.
- The backend `MemoryAllocator::Weak::Allocate()` must not itself use `new` (infinite recursion). Placement `new` (used by `NewT()` and `AllocateOneT()`) is unaffected. Requires `STK_MEMORY_NO_HEAP=0`.

---

## Configuration Macros

Define these in your build system (not in a single source file): several of them change class layouts and must have the same value in every translation unit.

| Macro | Default | Description |
|---|---|---|
| `STK_MEMORY_PLACEMENT_NEW` | `1` | Enables placement `new`: the object helpers (`NewT`, `PoolPtr`, …), `StaticBlockPoolFor` and the `MemoryAllocator` typed helpers. |
| `STK_MEMORY_NO_HEAP` | `0` | `1` removes every reference to `MemoryAllocator::Allocate()` / `Free()`: heap-storage constructors, typed/aligned helpers and the `new`/`delete` override. Pools remain usable with embedded or external storage. Use it to prove (and enforce) a zero-heap configuration. |
| `STK_MEMORY_POOL_CHECKS` | `1` if `_DEBUG` or `DEBUG` is defined, else `0` | Expensive diagnostics: `BlockMemoryPool` O(n) double-free walk, payload fill patterns (`0xCD` on allocation, `0xDD` on free) and `BytePool` block-start validation. Set it explicitly — `_DEBUG`/`DEBUG` are not reliably defined by embedded toolchains. |
| `STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT` | `8` (`16` if `_STK_ARCH_RISC_V` is defined) | Default `BlockMemoryPool` block alignment. Define as `sizeof(void *)` for the smallest possible blocks (4 bytes on 32-bit targets) at the cost of 4-byte alignment there. |
| `STK_MEMORY_BLOCKPOOL_BITMAP` | `0` | One "allocated" bit per block: O(1) double-free detection in release builds (costs an integer division per alloc/free). Size external buffers with `RequiredStorageSize()`. |
| `STK_MEMORY_BYTEPOOL_ALIGN_DEFAULT` | `8` (`16` if `_STK_ARCH_RISC_V` is defined) | `BytePool` payload alignment, power of two ≥ 4. Also sets `HEADER_SIZE = max(8, alignment)`. |
| `STK_MEMORY_BYTEPOOL_GUARD` | `0` | 4-byte guard word after every allocation, verified on `Free()` (payload overrun detection). Included in `RequiredSize()` / `GetMaxRequest()`. |
| `STK_MEMORY_BYTEPOOL_NEXT_FIT` | `0` | Start searches where the previous allocation ended instead of at the beginning: usually a shorter critical section when many used blocks accumulate at the start, at the price of somewhat more fragmentation. |
| `STK_MEMORY_OVERRIDE_NEW_DELETE` | `0` | Route global `new` / `delete` to `MemoryAllocator` (see above). |
| `STK_MEMORY_NEW_OOM_ACTION()` | `std::terminate()` | No-return hook run by the replaced throwing `new` on exhaustion when exceptions are disabled. |
| `STK_MEMORY_DEFAULT_HEAP_SIZE` | `8192` | Size of the heap of `stk_memory_allocator_default.h`, in bytes. |

---

## Usage Examples

### Embedded storage — zero heap, ISR receiver

```cpp
#include <memory/stk_memory.h>
#include <sync/stk_sync_msgqueue.h>

static const uint32_t PKT_COUNT = 8U;

// Storage, size and alignment are part of the object: nothing to compute by hand
stk::memory::StaticBlockPool<sizeof(Packet), PKT_COUNT> g_PktPool;

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

// Block size and alignment are taken from the type
stk::memory::StaticBlockPoolFor<SensorRecord, SENSOR_BUF_COUNT> g_SensorPool;

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

### Objects with constructors and automatic release (`PoolPtr`)

```cpp
#include <memory/stk_memory.h>

struct Message
{
    Message(uint32_t id, uint32_t len);
    ~Message();
    // ...
};

stk::memory::StaticBlockPoolFor<Message, 8U> g_MsgPool;

void Task_Rx()
{
    // Blocks until a block is free, constructs Message(42, 16) in it, and
    // returns the object to the pool when 'msg' goes out of scope
    auto msg = g_MsgPool.MakeT<Message>(42U, 16U);
    if (msg)
    {
        Handle(*msg);
    }
}   // ~Message() runs and the block goes back to g_MsgPool here

void ISR_Event()
{
    // Non-blocking variant with manual ownership
    Message *m = g_MsgPool.TryNewT<Message>(7U, 0U);
    if (m)
    {
        Post(m);                             // the consumer calls g_MsgPool.DeleteT(m)
    }
}
```

### External storage (static buffer)

```cpp
#include <memory/stk_memory.h>

static const uint32_t PKT_COUNT = 8U;

alignas(stk::memory::BlockMemoryPool::ALIGN_DEFAULT) static uint8_t
    g_PktStorage[stk::memory::BlockMemoryPool::RequiredStorageSize(PKT_COUNT, sizeof(Packet))];

stk::memory::BlockMemoryPool g_PktPool(PKT_COUNT, sizeof(Packet),
                                       g_PktStorage, sizeof(g_PktStorage));
```

Use this form when the buffer must live in a specific memory section or be shared with other code.

### Heap-allocated pool with validity check

```cpp
#include <memory/stk_memory.h>

void Task_Worker()
{
    // 16 blocks of 64 bytes, heap-owned. Created at run time, once the heap is
    // ready — not as a global object (static initialization order).
    stk::memory::BlockMemoryPool pool(16U, 64U, "DataPool");

    // Always check after the heap constructor when exceptions are disabled
    STK_ASSERT(pool.IsStorageValid());

    // ...
}
```

### Timed allocation with timeout

```cpp
// Try to acquire a block for up to 100 ticks; proceed only if successful
void *blk = g_SensorPool.TimedAlloc(stk::GetTicksFromMsec(100));
if (blk != nullptr)
{
    UseBlock(blk);
    g_SensorPool.Free(blk);
    blk = nullptr;
}
else
{
    // Pool was exhausted for the entire timeout window
    ReportOverflow();
}
```

### Variable-size messages with `BytePool`

```cpp
#include <memory/stk_memory.h>
#include <sync/stk_sync_msgqueue.h>

stk::memory::StaticBytePool<4096> g_BytePool;

stk::sync::MessageQueueT<8, sizeof(void *)> g_TxQueue;

void Task_Producer()
{
    // Wait up to 100 ms for 64 contiguous bytes
    char *msg = static_cast<char *>(g_BytePool.TimedAlloc(64U, stk::GetTicksFromMsec(100)));
    if (msg != nullptr)
    {
        FormatMessage(msg, 64U);
        g_TxQueue.Put(&msg);
    }
}

void Task_Sender()
{
    void *msg = nullptr;
    if (g_TxQueue.Get(&msg))
    {
        Send(static_cast<const char *>(msg));
        g_BytePool.Free(msg);                // wakes all blocked allocators
        msg = nullptr;
    }
}

void Task_Monitor()
{
    size_t available, largest, fragments;
    g_BytePool.GetInfo(available, largest, fragments);
    // largest < available means the free space is fragmented
}
```

### Sizing a pool from measurements

Run your worst-case workload, then read the watermarks:

```cpp
void Task_Monitor()
{
    // Block pool: how many blocks were ever in use at the same time, and did any request fail?
    size_t peak_blocks = g_PktPool.GetPeakUsedCount();
    size_t failed      = g_PktPool.GetAllocFailCount();

    // Byte pool: the same in bytes (headers included) plus the worst-case critical section
    stk::memory::MemoryAllocator::Stats st = g_BytePool.GetStats();
    // st.peak_allocated   - bytes ever in use at once
    // st.min_ever_free    - tightest the pool ever got
    // st.alloc_fail_count - requests that could not be satisfied
    size_t worst_scan = g_BytePool.GetMaxScanLength();   // blocks walked while interrupts/scheduler were blocked
}
```

### General-purpose allocator and standard `new`

```cpp
// allocator.cpp - exactly one translation unit
#include <memory/stk_memory_allocator_default.h>      // deterministic heap, no malloc needed
```

```cpp
// Anywhere (compile with -DSTK_MEMORY_OVERRIDE_NEW_DELETE=1 for plain new/delete)
auto  *cfg = stk::memory::MemoryAllocator::AllocateOneT<Config>(1U, 2U);   // constructed in place
stk::memory::MemoryAllocator::FreeOneT(cfg);

void *dma = stk::memory::MemoryAllocator::AllocateAligned(512U, 32U);      // 32-byte aligned
stk::memory::MemoryAllocator::FreeAligned(dma);

std::vector<int> v(100);                                                    // uses STK's allocator
```

---

## Storage Sizing Reference

Prefer `StaticBlockPool` / `StaticBytePool`, which size their storage for you. For external buffers, `RequiredStorageSize()` accounts for the aligned block size (and the optional bitmap):

```cpp
// General form
alignas(stk::memory::BlockMemoryPool::ALIGN_DEFAULT) uint8_t buf[
    stk::memory::BlockMemoryPool::RequiredStorageSize(N, sizeof(MyType))];

// Concrete example: 8 blocks of Packet
alignas(stk::memory::BlockMemoryPool::ALIGN_DEFAULT) static uint8_t
    g_Buf[stk::memory::BlockMemoryPool::RequiredStorageSize(8U, sizeof(Packet))];
```

> The buffer should be aligned to the pool alignment (`ALIGN_DEFAULT`, or the value passed to the constructor). A misaligned buffer still works — the unaligned head is skipped — but then needs up to `alignment - 1` extra bytes, otherwise the capacity is trimmed.

For `BytePool`, every live allocation also consumes a header (`HEADER_SIZE`, 8 bytes by default, 16 on RISC-V), so add `RequiredSize()` for each block that must be alive at the same time:

```cpp
// Up to 4 blocks of 128 bytes and one block of 512 bytes alive simultaneously
alignas(stk::memory::BytePool::BLOCK_ALIGN) static uint8_t g_Heap[
    stk::memory::BytePool::RequiredSize(128U) * 4U +
    stk::memory::BytePool::RequiredSize(512U)];
```

> Variable-size allocation can fragment, so leave headroom beyond the sum of the peak sizes, or monitor `GetInfo()` / `GetStats()`.

---

## ISR Safety Summary

| Operation | ISR-safe |
|---|---|
| `TryAlloc()` / `TryAllocT<T>()` | Yes |
| `Free()` | Yes |
| `TryNewT<T>()` / `TryMakeT<T>()` / `DeleteT()` | Yes, if the constructor / destructor is |
| `TimedAlloc(NO_WAIT)` | Yes (equivalent to `TryAlloc()`) |
| `GetStats()` and the other query methods | Yes |
| `Alloc()` / `AllocT<T>()` / `NewT<T>()` / `MakeT<T>()` | **No** — task context only |
| `TimedAlloc(timeout > 0)` / `TimedNewT<T>(timeout > 0)` | **No** — task context only |
| Constructors / Destructor | **No** — init/task context only |

The same table applies to `BytePool` (`size` is the first argument of the alloc calls). Its `TryAlloc()`, `Free()` and `GetInfo()` are ISR-safe but take time proportional to the number of blocks, so avoid them in time-critical ISRs on pools with many blocks — `GetMaxScanLength()` measures the actual cost on your workload. `MemoryAllocator::Allocate()` / `Free()` (and therefore global `new` / `delete`) may be called from an ISR only if your backend (`MemoryAllocator::Weak`) implementation allows it; the default implementation does.

---

## Notes and Caveats

- **Lifetime**: Destroying a pool while tasks are blocked inside `Alloc()` or `TimedAlloc()` is a logical error. An assertion is triggered in debug builds by the internal `ConditionVariable` destructor. Destroying a pool with outstanding blocks is also asserted.
- **Double-free**: See the detection table under `BlockMemoryPool::Free()` and the `BytePool` description. Null pointers after `Free()` to avoid silent corruption.
- **Blocking paths require `KERNEL_SYNC`**: `Alloc()` and `TimedAlloc()` (with a non-zero timeout) depend on the `ConditionVariable` primitive and require the kernel to be compiled with `KERNEL_SYNC`. `TryAlloc()` and `Free()` are always available regardless of kernel mode.
- **Non-copyable**: `BlockMemoryPool` and `BytePool` are non-copyable and non-movable.
- **`BytePool` starvation**: freed memory is not reserved for the longest-waiting task, so a large request can be starved by a stream of small ones. Every `Free()` wakes all waiters, each of which re-scans the pool.
- **Global pool objects**: with heap storage they would be constructed before the heap is usable; use embedded or external storage.
- **`stk_memory_allocator_default.h`** and **`stk_memory_allocator_malloc.h`** define functions (the `MemoryAllocator::Weak` hooks): include only one of them, in exactly one translation unit, and not from a shared/umbrella header.

---

## Compatibility Notes

Behavior that differs from earlier versions of this module:

- **`BlockMemoryPool` block alignment** is now 8 bytes by default, 16 on RISC-V (was `sizeof(void*)`, i.e. 4 on 32-bit targets), so the smallest block on such targets is 8 bytes. Define `STK_MEMORY_BLOCKPOOL_ALIGN_DEFAULT` as `sizeof(void *)` to restore the old behavior.
- **External buffers** are now aligned internally and the capacity trimmed instead of relying on a misaligned write; size them with `RequiredStorageSize()`, especially with `STK_MEMORY_BLOCKPOOL_BITMAP=1`. Invalid arguments (zero capacity, capacity above `CAPACITY_MAX`, bad alignment) now yield an invalid pool in release builds as well, instead of only asserting.
- **Failure behavior**: an invalid pool fails every allocation immediately instead of waiting forever; `TryAlloc()` no longer touches the condition variable; `TimedAllocT<T>()` returns `nullptr` rather than an undersized or misaligned block.
- **`AllocateOneT()` / `AllocateArrayT()`** now always construct when arguments are passed (previously the arguments of trivially constructible types were silently ignored), copy their arguments into every array element, and reject `count * sizeof(T)` overflow. Over-aligned types are rejected at compile time.
- **`MemoryAllocator::Stats`**: `capacity` is no longer `const`, the structure gained `peak_allocated`, `alloc_fail_count` and `Reset()`, and a default-constructed `Stats` now has zero capacity.
- **`MemoryAllocator` constants**: `ALIGNMENT` is now `MIN_ALIGNMENT` (8, or 16 on RISC-V), and `CAPACITY_DEFAULT` was removed. New: `AllocateTrivialArrayT()` / `FreeTrivialArrayT()`.
- **`MemoryAllocator` backend split**: the weak hooks moved to `MemoryAllocator::Weak::Allocate()`, `Weak::Free()` and `Weak::GetStats()`. `MemoryAllocator::Allocate()`, `Free()` and `GetStats()` are now checked front-end wrappers. Custom backends must rename their definitions to `MemoryAllocator::Weak::*` (until then the hooks stay unresolved and `IsAvailable()` returns `false`). `stk_memory_allocator_malloc.h` is a new ready-made `malloc`-backed backend.
- **`STK_MEMORY_POOL_CHECKS`** replaces the hard-wired `_DEBUG` / `DEBUG` test (which remains its default).
- **`BytePool::BLOCK_ALIGN`** is public and configurable (default 8, 16 on RISC-V); `HEADER_SIZE` is `max(8, BLOCK_ALIGN)`.
- **Heap-storage constructors** are removed when `STK_MEMORY_NO_HEAP=1`.
