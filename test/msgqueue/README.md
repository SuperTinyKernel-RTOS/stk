# MessageQueue Test Suite

Test suite for `stk::sync::MessageQueue` and `stk::sync::MessageQueueT`.  
Source: `test/msgqueue/test_msgqueue.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_msgqueue.h>
```

`MessageQueue` is a fixed-capacity, fixed-message-size FIFO ring buffer for
inter-task communication. Messages are opaque byte arrays copied with `memcpy`;
the message type is not required to be copyable via the C++ assignment operator.

`MessageQueueT<N, MSG>` is a thin template wrapper that owns its internal storage
and derives from `MessageQueue`. It is used throughout this test suite for
convenience when capacity and message size are known at compile time.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// External-storage form — caller owns the buffer
static uint8_t s_buf[8 * sizeof(SensorMsg)];
stk::sync::MessageQueue g_Q(s_buf, 8, sizeof(SensorMsg));

// Internal-storage form — storage embedded in the object
stk::sync::MessageQueueT<8, sizeof(SensorMsg)> g_Q;
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Put` | `bool Put(const void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Copies `msg_size` bytes from `msg_ptr` into the next free slot. Blocks if the queue is full until space is available or the timeout expires. Returns `true` on success, `false` on timeout. ISR-safe only with `timeout == NO_WAIT`. |
| `TryPut` | `bool TryPut(const void *msg_ptr)` | Non-blocking `Put(msg_ptr, NO_WAIT)`. Returns `false` immediately if the queue is full. ISR-safe. |
| `PutFront` | `bool PutFront(const void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Copies `msg_size` bytes from `msg_ptr` into the slot immediately before the current read pointer, making it the next message returned by `Get()`. Blocks if the queue is full until space is available or the timeout expires. Returns `true` on success, `false` on timeout. ISR-safe only with `timeout == NO_WAIT`. |
| `TryPutFront` | `bool TryPutFront(const void *msg_ptr)` | Non-blocking `PutFront(msg_ptr, NO_WAIT)`. Returns `false` immediately if the queue is full. ISR-safe. |
| `Get` | `bool Get(void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Copies `msg_size` bytes from the oldest slot into `msg_ptr`. Blocks if the queue is empty until a message is produced or the timeout expires. Returns `true` on success, `false` on timeout. ISR-safe only with `timeout == NO_WAIT`. |
| `TryGet` | `bool TryGet(void *msg_ptr)` | Non-blocking `Get(msg_ptr, NO_WAIT)`. Returns `false` immediately if the queue is empty. ISR-safe. |
| `Peek` | `bool Peek(void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Copies `msg_size` bytes from the oldest slot into `msg_ptr` **without** removing the message. Blocks if the queue is empty until a message is produced or the timeout expires. Returns `true` on success, `false` on timeout. ISR-safe only with `timeout == NO_WAIT`. |
| `TryPeek` | `bool TryPeek(void *msg_ptr)` | Non-blocking `Peek(msg_ptr, NO_WAIT)`. Returns `false` immediately if the queue is empty. ISR-safe. |
| `PeekFront` | `bool PeekFront(void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Copies `msg_size` bytes from the most recently front-inserted slot (i.e. `Prev(m_tail)`) into `msg_ptr` **without** removing the message. Blocks if the queue is empty until a message is produced or the timeout expires. Returns `true` on success, `false` on timeout. ISR-safe only with `timeout == NO_WAIT`. |
| `TryPeekFront` | `bool TryPeekFront(void *msg_ptr)` | Non-blocking `PeekFront(msg_ptr, NO_WAIT)`. Returns `false` immediately if the queue is empty. ISR-safe. |
| `Reset` | `void Reset()` | Discards all messages and resets head, tail and count to zero. Wakes all tasks blocked in `Put()` so they can re-evaluate. ISR-unsafe. |
| `GetCapacity` | `size_t GetCapacity() const` | Returns the construction-time capacity (maximum number of messages). ISR-safe. |
| `GetMsgSize` | `size_t GetMsgSize() const` | Returns the construction-time message size in bytes. ISR-safe. |
| `GetCount` | `size_t GetCount() const` | Returns the current number of messages in the queue (point-in-time snapshot). ISR-safe. |
| `GetSpace` | `size_t GetSpace() const` | Returns the number of free slots currently available (`capacity - count`). ISR-safe. |
| `GetBuffer` | `uint8_t *GetBuffer()` | Returns a pointer to the beginning of the backing byte buffer. ISR-safe. |
| `IsEmpty` | `bool IsEmpty() const` | Returns `true` if the queue contains no messages. ISR-safe. |
| `IsFull` | `bool IsFull() const` | Returns `true` if the queue contains `capacity` messages. ISR-safe. |
| `IsStorageValid` | `bool IsStorageValid() const` | Returns `true` if the backing buffer pointer is non-null. ISR-safe. |

**`Put()` flow:**

```
queue not full  →  memcpy into Slot(m_head), advance m_head, ++m_count,
                   NotifyOne on cv_not_empty, return true
queue full, timeout == NO_WAIT  →  return false immediately
queue full, timeout > 0         →  Wait on cv_not_full; retry or return false on expiry
```

**`PutFront()` flow:**

```
queue not full  →  retreat m_tail (m_tail = Prev(m_tail)), memcpy into Slot(m_tail),
                   ++m_count, NotifyOne on cv_not_empty, return true
queue full, timeout == NO_WAIT  →  return false immediately
queue full, timeout > 0         →  Wait on cv_not_full; retry or return false on expiry
```

**`Get()` flow:**

```
queue not empty  →  memcpy from Slot(m_tail), advance m_tail, --m_count,
                    NotifyOne on cv_not_full, return true
queue empty, timeout == NO_WAIT  →  return false immediately
queue empty, timeout > 0         →  Wait on cv_not_empty; retry or return false on expiry
```

**`Peek()` flow:**

```
queue not empty  →  memcpy from Slot(m_tail), m_tail and m_count unchanged, return true
queue empty, timeout == NO_WAIT  →  return false immediately
queue empty, timeout > 0         →  Wait on cv_not_empty; retry or return false on expiry
```

**`PeekFront()` flow:**

```
queue not empty  →  memcpy from Slot(Prev(m_tail)), m_tail and m_count unchanged, return true
queue empty, timeout == NO_WAIT  →  return false immediately
queue empty, timeout > 0         →  Wait on cv_not_empty; retry or return false on expiry
```

**`Reset()` flow:**

```
m_count = 0, m_head = 0, m_tail = 0
NotifyAll on cv_not_full  →  wakes all blocked Put() callers
```

**Key invariants:**

- The queue is parameterised on a byte count, not a C++ type; payload is always
  transferred with `memcpy`.
- `Put()` and `PutFront()` both signal `cv_not_empty` after each successful enqueue;
  `Get()` signals `cv_not_full` after each successful dequeue.
- `Peek()` and `PeekFront()` wait on `cv_not_empty` identically to `Get()` but
  leave `m_tail` and `m_count` unchanged; they never signal any condition variable.
- `PeekFront()` reads from `Slot(Prev(m_tail))`, i.e. the slot most recently written
  by `PutFront()`, without retreating `m_tail` or altering any state.
- `PutFront()` retreats `m_tail` using the `Prev()` helper (modulo `m_capacity`) and
  writes into that slot, leaving `m_head` unchanged. The message becomes the next item
  returned by `Get()`.
- `Reset()` signals `cv_not_full` with `NotifyAll` so every blocked producer is
  woken at once.
- Head and tail indices wrap modulo `m_capacity` via the `Next()` / `Prev()` helpers,
  forming a true ring buffer.
- Destroying a queue while tasks are waiting is a logic error; `ConditionVariable`
  destructors assert an empty wait list in debug builds.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_MQ_TEST_TASKS_MAX` | `5` | Kernel task-slot capacity (Stress test uses all five) |
| `_STK_MQ_TEST_TIMEOUT` | `1000` ticks | Reserved upper-bound timeout (not used directly in assertions) |
| `_STK_MQ_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_MQ_TEST_LONG_SLEEP` | `100` ticks | Sleep used to wait for concurrent tasks to complete |
| `_STK_MQ_CAPACITY` | `8` | Queue capacity for the shared test queue |
| `_STK_MQ_MSG_SIZE` | `16` bytes | Message size for the shared test queue |
| `_STK_MQ_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

A fresh `MessageQueueT<_STK_MQ_CAPACITY, _STK_MQ_MSG_SIZE>` is constructed on
the stack inside `RunTest()` for every test run and pointed to by `g_Queue`.
`ResetTestState()` resets `g_TestResult`, `g_InstancesDone` and `g_SharedCounter`
to zero before each test.

**Task-count gating** — `RunTest()` registers only the tasks actually required:

| Predicate | Tests | Tasks added |
|-----------|-------|-------------|
| `NeedsOnlyOneTask()` | TryPutGet, FillDrain, Accessors, WrapAround, TryPutFront, PutFrontWrapAround, Peek, PeekFront | task 0 only |
| *(default)* | BlockingGet, BlockingPut, TimedGetTimeout, TimedGetSuccess, TimedPutTimeout, Reset, PingPong, BlockingPutFront, TimedPutFrontTimeout | tasks 0–1 |
| `NeedsAllTasks()` | Stress | tasks 0–4 |

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) insufficient RAM prevents linking the full set of
distinct task class templates simultaneously. Tests 1–14 are skipped and only
`StressTask` (test 15) runs, under `#ifndef __ARM_ARCH_6M__`.

`StressTask` runs on M0 because it uses a single task class template instantiated
for all five task slots, fitting within available memory.

| Platform | `_STK_MQ_STACK_SIZE` |
|----------|-----------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `TryPutGet`
**Tasks:** 0 only &nbsp;|&nbsp; **Param:** none

Task 0 verifies the initial empty state of the queue — `IsEmpty()`, `GetCount() == 0`,
`GetSpace() == capacity`, `GetCapacity()`, `GetMsgSize()`, and `IsStorageValid()`.
It then enqueues one message filled with `0xAB` via `TryPut()`, checks that `GetCount()`
and `GetSpace()` update correctly, and dequeues it via `TryGet()`, verifying the full
16-byte payload with `memcmp`. A second `TryGet()` on the now-empty queue must return
`false` without blocking.

**Pass condition:** all accounting checks hold; payload survives the round trip intact;
`TryGet()` on empty returns `false`

---

### Test 2 — `FillDrain`
**Tasks:** 0 only

Task 0 enqueues all 8 slots via `TryPut()`, filling each message with the byte value
equal to its slot index (0, 1, … 7). After filling, `IsFull()` must be `true`,
`GetCount()` must equal `_STK_MQ_CAPACITY`, and `GetSpace()` must be `0`. A further
`TryPut()` on the full queue must return `false` immediately. The queue is then drained
in order via `TryGet()` and each 16-byte payload is checked with `memcmp` to confirm
FIFO ordering. After draining, `IsEmpty()` must be `true`.

**Pass condition:** `IsFull()` after fill; `TryPut()` on full returns `false`; all 8
messages dequeued in exact FIFO order; `IsEmpty()` after drain

---

### Test 3 — `BlockingGet`
**Tasks:** 0–1

Task 1 immediately calls `Get()` on an empty queue, which must block. Task 0 sleeps
`_STK_MQ_TEST_SHORT_SLEEP * 2` ticks to allow task 1 to enter the wait state, then
sends a message filled with `0xCD` via `Put()`. Task 1 unblocks, receives the message,
and verifies the full 16-byte payload with `memcmp`; on success it sets
`g_SharedCounter = 1`. Task 0 then uses a `g_InstancesDone < 2` barrier and checks
that `g_SharedCounter == 1` and the queue is empty.

**Pass condition:** `g_SharedCounter == 1` and queue empty after barrier

---

### Test 4 — `BlockingPut`
**Tasks:** 0–1

Task 0 fills the queue to capacity with 8 zero messages via `TryPut()`, then sleeps
`_STK_MQ_TEST_SHORT_SLEEP * 2` ticks. Task 1 attempts `Put()` on the full queue and
blocks. Task 0 then calls `Get()` to free one slot, which must wake task 1. After
sleeping again, task 0 drains any remaining messages. Task 1 sets `g_SharedCounter = 1`
on a successful return from `Put()`. Task 0 waits at a `g_InstancesDone < 2` barrier
and verifies the outcome.

**Pass condition:** `g_SharedCounter == 1` and queue empty after barrier

---

### Test 5 — `TimedGetTimeout`
**Tasks:** 0–1

Task 1 calls `Get(rx, 50)` on an always-empty queue (task 0 idles) and measures the
elapsed time with `GetTimeNowMs()`. The call must return `false` and the elapsed time
must fall in the window `[45, 65]` ms. `g_TestResult` is set directly inside task 1's
branch.

**Pass condition:** `Get()` returned `false` and elapsed ∈ `[45, 65]` ms

---

### Test 6 — `TimedGetSuccess`
**Tasks:** 0–1

Task 1 sleeps `_STK_MQ_TEST_SHORT_SLEEP` ticks then calls `Get(rx, 150)`. Task 0
sleeps 40 ticks then enqueues a message filled with `0x55` via `TryPut()` — well
within task 1's 150 ms timeout. Task 1 must return `true` from `Get()`. `g_TestResult`
is set directly inside task 1's branch.

**Pass condition:** `Get()` returned `true`

---

### Test 7 — `TimedPutTimeout`
**Tasks:** 0–1

Task 0 fills the queue to capacity via `TryPut()` then sleeps 200 ticks, holding it
full for longer than task 1's timeout. Task 1 sleeps `_STK_MQ_TEST_SHORT_SLEEP` to
let task 0 fill the queue first, then calls `Put(tx, 50)` and measures elapsed time.
The call must return `false` and elapsed must fall in `[45, 65]` ms. `g_TestResult`
is set directly inside task 1's branch.

**Pass condition:** `Put()` returned `false` and elapsed ∈ `[45, 65]` ms

---

### Test 8 — `Reset`
**Tasks:** 0–1

Task 0 fills the queue to capacity then sleeps `_STK_MQ_TEST_SHORT_SLEEP * 2` ticks
to allow task 1 to enter a blocking `Put()`. Task 0 then calls `Reset()`, which must
atomically discard all 8 messages and signal `cv_not_full` with `NotifyAll`, waking
task 1. Task 1's `Put()` returns `true` and sets `g_SharedCounter = 1`. Task 0 waits
at a `g_InstancesDone < 2` barrier, drains task 1's message, then verifies the result.

**Pass condition:** `g_SharedCounter == 1` and queue empty after barrier

---

### Test 9 — `Accessors`
**Tasks:** 0 only

Task 0 verifies every const accessor against construction-time values: `IsStorageValid()`,
`GetCapacity() == 8`, `GetMsgSize() == 16`, and `GetBuffer() != nullptr`. It then
performs one `TryPut()` / `TryGet()` cycle and checks that `GetCount()` and `GetSpace()`
track the transitions correctly (`1` / `capacity - 1` after put; `0` / `capacity` after
get). Additionally, a local `MessageQueueT<4, 8>` with internal storage is constructed
on the stack and the same accessor checks are applied to it, confirming that the template
wrapper's embedded storage is wired up correctly.

**Pass condition:** all accessor values match expectations for both the shared external
queue and the local `MessageQueueT<4, 8>`

---

### Test 10 — `WrapAround`
**Tasks:** 0 only

Task 0 exercises the ring-buffer modular arithmetic across the physical slot boundary.
Phase 1: enqueue `HALF = 4` messages (payloads `0x01`–`0x04`), then drain them, advancing
`m_tail` to index 4. Phase 2: enqueue all 8 slots again (payloads `0x80`–`0x87`), forcing
`m_head` to wrap from index 4 past the end of the buffer back to index 4. `IsFull()` is
checked after filling. All 8 messages are then dequeued and each 16-byte payload verified
with `memcmp` to confirm that the `Next()` wrap-around preserves FIFO ordering across
the ring boundary.

**Pass condition:** `IsFull()` after second fill; all 8 post-wrap payloads dequeued
in exact FIFO order; `IsEmpty()` after drain

---

### Test 10a — `TryPutFront`
**Tasks:** 0 only

Task 0 verifies `TryPutFront` correctness in four sub-cases:

1. **Single front-insert on an empty queue.** `TryPutFront()` fills a message with
   `0xF0` and enqueues it. `GetCount()` must rise to 1 and the message must come back
   intact via `TryGet()`.

2. **Mixed back/front ordering.** Two messages A (`0xAA`) and B (`0xBB`) are enqueued
   via `TryPut()`, then C (`0xCC`) is prepended via `TryPutFront()`. The dequeue order
   must be C → A → B.

3. **Consecutive front-inserts produce LIFO order.** Three messages D, E, F are each
   inserted at the front in that order; because every call retreats the tail one slot,
   the dequeue order must be F → E → D.

4. **`TryPutFront` on a full queue returns `false` immediately.** The queue is filled to
   capacity, a further `TryPutFront()` is called, and the return value must be `false`
   with `GetCount()` unchanged.

**Pass condition:** all ordering checks and accounting checks hold across all four sub-cases

---

### Test 10b — `PutFrontWrapAround`
**Tasks:** 0 only

Task 0 exercises `TryPutFront` specifically at the ring-buffer boundary where the tail
pointer must wrap from index 0 back to `CAPACITY - 1`. The buffer is first filled and
fully drained so both head and tail sit at 0. `HALF = 4` messages are then enqueued via
`TryPut()` so that `m_tail == 0` and `m_head == 4`. A single `TryPutFront()` is called;
the `Prev()` helper must wrap the tail from 0 to index 7 (i.e. `CAPACITY - 1`). The
front-inserted message (payload `0xFE`) must be the first item returned by `TryGet()`,
followed by the 4 back-inserted messages in their original FIFO order.

**Pass condition:** front-inserted payload retrieved first; remaining 4 messages in exact
FIFO order; `IsEmpty()` after full drain

---

### Test 10c — `BlockingPutFront`
**Tasks:** 0–1

Task 0 fills the queue to capacity with 8 messages (payloads `0x10`–`0x17`) then sleeps
`_STK_MQ_TEST_SHORT_SLEEP * 2` ticks. Task 1 calls `PutFront()` on the full queue with a
priority sentinel payload (`0x50`) and must block. Task 0 calls `Get()` to consume the
oldest message, freeing one slot; this must unblock task 1's `PutFront()`. Task 1 sets
`g_SharedCounter = 1` on a successful return. After a second sleep, task 0 waits at a
`g_InstancesDone < 2` barrier, calls `TryGet()` and verifies the payload is the `0x50`
sentinel (confirming the message landed at the front), then drains the remaining messages.

**Pass condition:** `g_SharedCounter == 1`; first dequeued message is the `0x50` sentinel;
queue empty after drain

---

### Test 10d — `TimedPutFrontTimeout`
**Tasks:** 0–1

Mirrors `TimedPutTimeout` but targets `PutFront()`. Task 0 fills the queue via `TryPut()`
and holds it full for 200 ms. Task 1 sleeps `_STK_MQ_TEST_SHORT_SLEEP` to let task 0
fill first, then calls `PutFront(tx, 50)` and measures elapsed time with `GetTimeNowMs()`.
The call must return `false` and the elapsed time must fall in `[45, 65]` ms. `g_TestResult`
is set directly inside task 1's branch.

**Pass condition:** `PutFront()` returned `false` and elapsed ∈ `[45, 65]` ms

---

### Test 13a — `Peek`
**Tasks:** 0 only

Task 0 exercises `Peek()` and `TryPeek()` across four sub-cases:

1. **`TryPeek` on an empty queue returns `false` immediately.** No message is written; `TryPeek()` must return `false` and `IsEmpty()` must remain `true`.

2. **`Peek` is non-destructive.** One message (`0xA5`) is enqueued. `TryPeek()` must return `true` with the correct payload, and `GetCount()` must remain `1` afterwards. A subsequent `TryGet()` must return the same payload, confirming the message was not consumed by the peek.

3. **`Peek` is idempotent across multiple calls.** One message (`0x3C`) is enqueued. Two consecutive `TryPeek()` calls must each return the correct payload and leave `GetCount() == 1`. A final `TryGet()` drains the slot.

4. **`Peek` obeys FIFO order.** Two messages (`0x11`, `0x22`) are enqueued in that order. `TryPeek()` must return `0x11` (the oldest), not `0x22`.

**Pass condition:** all sub-cases pass; queue empty after each drain

---

### Test 13b — `PeekFront`
**Tasks:** 0 only

Task 0 exercises `PeekFront()` and `TryPeekFront()` across four sub-cases:

1. **`TryPeekFront` on an empty queue returns `false` immediately.**

2. **`PeekFront` on a pure-`Put` queue.** With no `PutFront()` ever called, `PeekFront()` reads `Slot(Prev(m_tail))`, which is the most-recently-written back slot. The returned payload must match and `GetCount()` must remain `1`.

3. **`PeekFront` returns the `PutFront` message while `Peek` agrees.** Messages A and B are back-inserted; C is front-inserted via `TryPutFront()`. Both `TryPeek()` and `TryPeekFront()` must return C (since C is both the oldest in the logical sequence and the most recently front-inserted). Both calls must leave `GetCount() == 3`. The queue is then fully drained in the expected order: C → A → B.

4. **`PeekFront` tracks the newest `PutFront`.** Messages D then E are inserted at the front in that order. `TryPeekFront()` must return E (the most recent front-insert) and leave `GetCount() == 2` unchanged.

**Pass condition:** all sub-cases pass; queue empty after each drain

---

### Test 11 — `PingPong`
**Tasks:** 0–1 &nbsp;|&nbsp; **Param:** `iterations = 30`

Task 0 is the producer and task 1 is the consumer. Task 0 sends 30 messages via
blocking `Put()`, encoding the loop counter `i` as a `int32_t` in the first 4 bytes
of each 16-byte message. Task 1 receives 30 messages via blocking `Get()`, decodes
the counter with `memcpy`, and increments `g_SharedCounter` only when the decoded
value matches the expected sequence index `i`. Task 0 waits at a `g_InstancesDone < 2`
barrier then verifies that `g_SharedCounter == 30` and the queue is empty.

**Pass condition:** `counter == 30` (every message received in sequence) and queue empty

---

### Test 12 — `Stress`
**Tasks:** 0–4 (all 5) — **runs on all platforms including Cortex-M0** &nbsp;|&nbsp; **Param:** `iterations = 200`

All five tasks run 200 iterations each. On even iterations a task acts as producer,
cycling through three `Put` strategies by index: `i % 3 == 0` → `TryPut()`,
`i % 3 == 1` → blocking `Put()`, `i % 3 == 2` → `Put(buf, 20)`. On odd iterations
a task acts as consumer, cycling through the symmetric `Get` strategies. Each successful
`Put` increments `g_SharedCounter`; each successful `Get` decrements it. A `Delay(1)`
is injected every 8 iterations to allow preemption. Task 4 uses a `g_InstancesDone`
completion barrier, then drains any residual messages left by asymmetric put/get counts
(each drain decrements `g_SharedCounter`). The pass condition requires the net
outstanding count to reach exactly zero, confirming no messages were lost or duplicated
and no deadlock occurred.

**Pass condition:** `g_SharedCounter == 0` and `IsEmpty()` after full barrier and drain

---

## Summary Table

| #   | Test | Tasks | Stack | Pass condition | What it verifies |
|-----|------|-------|-------|----------------|------------------|
| 1   | `TryPutGetTask` | 0 | `_STK_MQ_STACK_SIZE` | payload intact; `TryGet()` on empty returns `false` | `TryPut()` / `TryGet()` basic cycle; all accounting accessors |
| 2   | `FillDrainTask` | 0 | `_STK_MQ_STACK_SIZE` | FIFO order across all 8 slots; `TryPut()` on full returns `false` | `IsFull()`, `GetCount()`, `GetSpace()`, FIFO ordering, capacity enforcement |
| 3   | `BlockingGetTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `g_SharedCounter == 1`; queue empty | `Get()` blocks on empty queue; producer wakes consumer; payload survives inter-task transfer |
| 4   | `BlockingPutTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `g_SharedCounter == 1`; queue empty | `Put()` blocks on full queue; consumer wakes producer by freeing a slot |
| 5   | `TimedGetTimeoutTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `Get()` returned `false`; elapsed ∈ `[45, 65]` ms | `Get()` with timeout expires in the correct window when queue stays empty |
| 6   | `TimedGetSuccessTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `Get()` returned `true` | `Get()` with timeout succeeds when message arrives before expiry |
| 7   | `TimedPutTimeoutTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `Put()` returned `false`; elapsed ∈ `[45, 65]` ms | `Put()` with timeout expires in the correct window when queue stays full |
| 8   | `ResetTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `g_SharedCounter == 1`; queue empty | `Reset()` discards all messages and wakes blocked producers via `NotifyAll` |
| 9   | `AccessorsTask` | 0 | `_STK_MQ_STACK_SIZE` | all accessors match construction values | `GetCapacity`, `GetMsgSize`, `GetBuffer`, `GetCount`, `GetSpace`, `IsStorageValid` for both external and internal (`MessageQueueT`) storage |
| 10  | `WrapAroundTask` | 0 | `_STK_MQ_STACK_SIZE` | all 8 post-wrap payloads in FIFO order; `IsEmpty()` after drain | Ring-buffer `Next()` wrap-around preserves payload integrity across the physical slot boundary |
| 10a | `TryPutFrontTask` | 0 | `_STK_MQ_STACK_SIZE` | all ordering and accounting checks pass across four sub-cases | `TryPutFront()` basic ordering (single insert, mixed back/front, consecutive front-inserts, full-queue rejection) |
| 10b | `PutFrontWrapAroundTask` | 0 | `_STK_MQ_STACK_SIZE` | front-inserted payload first; remaining 4 in FIFO order; `IsEmpty()` after drain | `PutFront()` tail `Prev()` wrap-around from index 0 to `CAPACITY-1`; ring-buffer integrity across the boundary |
| 10c | `BlockingPutFrontTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `g_SharedCounter == 1`; sentinel payload at front; queue empty | `PutFront()` blocks on full queue; consumer `Get()` wakes blocked front-producer; priority message lands at head |
| 10d | `TimedPutFrontTimeoutTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `PutFront()` returned `false`; elapsed ∈ `[45, 65]` ms | `PutFront()` with timeout expires in the correct window when queue stays full |
| 11  | `PingPongTask` | 0–1 | `_STK_MQ_STACK_SIZE` | `counter == 30`; queue empty | Blocking `Put()` / `Get()` sustain correct in-order delivery across 30 iterations of single-producer / single-consumer traffic |
| 12  | `StressTask` | 0–4 | `_STK_MQ_STACK_SIZE` | `g_SharedCounter == 0`; queue empty | No message loss, duplication, or deadlock under full five-task contention mixing `TryPut`, `Put`, `Put(timeout)`, `TryGet`, `Get`, and `Get(timeout)`; runs on all platforms |
| 13  | `PeekTask` | 0 | `_STK_MQ_STACK_SIZE` | all sub-cases pass; queue empty after each drain | `TryPeek()` on empty returns `false`; `Peek()` is non-destructive and idempotent; respects FIFO order |
| 13a | `PeekFrontTask` | 0 | `_STK_MQ_STACK_SIZE` | all sub-cases pass; queue empty after each drain | `TryPeekFront()` on empty returns `false`; `PeekFront()` is non-destructive; returns the front-inserted slot (`Prev(m_tail)`) without altering queue state |

