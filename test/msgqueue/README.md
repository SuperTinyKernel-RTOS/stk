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

**`*Ex` variants (cancelable waits):** every blocking call has an `EWaitResult`-returning
counterpart that lets the caller tell a timeout from a cancellation. The `bool` variants above
collapse both outcomes into `false`.

| Method | Signature | Returns |
|--------|-----------|---------|
| `PutEx` | `EWaitResult PutEx(const void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | `WAIT_RESULT_SIGNAL` if enqueued; `WAIT_RESULT_TIMEOUT`, `WAIT_RESULT_CANCELED`, `WAIT_RESULT_DELETED` (the queue was destroyed while waiting; it must not be accessed afterwards) or `WAIT_RESULT_FAIL` otherwise (nothing enqueued) |
| `PutFrontEx` | `EWaitResult PutFrontEx(const void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Same as `PutEx` |
| `GetEx` | `EWaitResult GetEx(void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | `WAIT_RESULT_SIGNAL` if a message was dequeued; otherwise timeout / canceled / deleted / fail (nothing dequeued or copied) |
| `PeekEx` | `EWaitResult PeekEx(void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Same as `GetEx`, message stays in the queue |
| `PeekFrontEx` | `EWaitResult PeekFrontEx(void *msg_ptr, Timeout timeout = WAIT_INFINITE)` | Same as `GetEx`, message stays in the queue |

**Cancellation semantics:** a blocked call can be cancelled with `IKernel::CancelTaskWait(ITask*)`
or `stk::SleepCancel(TId)`. Both routes unlink the task from the internal `ConditionVariable`
wait list and the `*Ex` call returns `WAIT_RESULT_CANCELED`.

- A cancelled `Put` / `PutFront` enqueues nothing and leaves head, tail and count unchanged.
- A cancelled `Get` / `Peek` / `PeekFront` copies nothing into `msg_ptr` and dequeues nothing.
- **Signal wins:** if the waiter was already signaled (removed from the wait list) when the cancel
  arrives, the cancel is a no-op and the call returns `WAIT_RESULT_SIGNAL`.
- A cancel leaves no stale wait entry: a later `Put` / `Get` is neither lost nor delivered twice,
  and a second waiter queued behind the cancelled one is still woken normally.

**Destruction semantics:** destroying a queue while tasks wait on it (consumers in `Get()` / `Peek()` /
`PeekFront()` on an empty queue, or producers in `Put()` / `PutFront()` on a full one; only one kind can exist at a time)
releases **all** of them with `WAIT_RESULT_DELETED` (done by the destructors of the two internal `ConditionVariable`s).
The released waiters neither transfer a message nor time out, and must not access the queue afterwards:

- A released `Put` / `PutFront` stores nothing and touches neither the indices nor the count.
- A released `Get` / `Peek` / `PeekFront` copies nothing into `msg_ptr` and dequeues nothing.
- The boolean `Put()` / `PutFront()` / `Get()` / `Peek()` / `PeekFront()` report it as `false`, the same as a timeout or a
  cancellation; use the `*Ex()` variants to tell it apart.
- The messages that were in the queue are discarded with it, as by `Reset()`.

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
- Destroying a queue while tasks are waiting releases **all** of them with `WAIT_RESULT_DELETED`
  (see Destruction semantics): no message is copied, stored or counted, and no member of the
  destroyed queue is read or written by a woken waiter.

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
| `NeedsThreeTasks()` | Cancel_* (tests 14–28), DeleteWithWaiter_* (tests 29–35) | tasks 0–2 |
| `NeedsAllTasks()` | Stress | tasks 0–4 |

### Cancellation tests (tests 14–28)

Cancellation tests run on the same `g_Kernel` and use a per-test `MessageQueueT` like all other
tests, driven by one task class, `CancelWaitTask`. They share the following state, reset by
`ResetTestState()` before each test:

| Variable | Purpose |
|----------|---------|
| `g_Victim` / `g_VictimTid` | Task whose wait is cancelled (task 1), as `ITask*` and as `TId` (for `SleepCancel`) |
| `g_VictimResult[2]` | `EWaitResult` of task 1 / task 2 (`-100` = not set) |
| `g_VictimElapsed[2]` | Milliseconds spent inside the blocking queue call by task 1 / task 2 |
| `g_VictimValue[2]` | `int32_t` decoded from the first 4 bytes of the message received by task 1 / task 2 (`-1` = nothing) |
| `g_VictimRxIntact` | `true` if task 1's receive buffer (pre-filled with `0x5A`) was left untouched by a non-signal result |
| `g_VictimSleepElapsed` | Milliseconds task 1 spent in `Sleep()` after its wait was cancelled |
| `g_CancelParam` | Scenario selector (see below) |

**Scenario selection.** `RunTest()` declares tasks 0–1 `static`, so each is constructed only once
per template instantiation; a constructor argument would silently keep the value of the first
case. The scenario is therefore passed through the global `g_CancelParam`, set by `main()` before
every `RunTest<CancelWaitTask>()` call. Values `1–9` select the scenario; adding
`CW_VIA_SLEEPCANCEL` (`100`) makes the canceller use `stk::SleepCancel(g_VictimTid)` instead of
`g_Kernel.CancelTaskWait(g_Victim)`.

| Constant | Value | Scenario |
|----------|-------|----------|
| `CW_GET_INFINITE` | 1 | Cancel an infinite `GetEx` on an empty queue |
| `CW_GET_NOT_TIMEOUT` | 2 | Cancel is reported as `CANCELED`, not `TIMEOUT` |
| `CW_PUT_FULL` | 3 | Cancel a `PutEx` on a full queue |
| `CW_NO_STALE_WAITER` | 4 | Cancel leaves no stale waiter behind |
| `CW_TWO_WAITERS` | 5 | Cancel one of two blocked getters |
| `CW_SIGNAL_WINS` | 6 | Message delivered first wins over a late cancel |
| `CW_PUT_FRONT_FULL` | 7 | Cancel a `PutFrontEx` on a full queue |
| `CW_PEEK_EMPTY` | 8 | Cancel a `PeekEx` on an empty queue |
| `CW_PEEK_FRONT_EMPTY` | 9 | Cancel a `PeekFrontEx` on an empty queue |
| `CW_VIA_SLEEPCANCEL` | 100 | Added to a scenario value: cancel via `stk::SleepCancel()` |

**Task roles.** Task 0 is the canceller/checker, task 1 is the victim (sleeps
`_STK_MQ_TEST_SHORT_SLEEP` = 10 ms, then blocks inside a queue call and records how the wait
ended), task 2 is a second getter used only by the two-waiters scenario. Timeline: victim blocks
at ~10 ms, canceller acts at ~60 ms. The victim's wait is infinite except in
`CW_GET_NOT_TIMEOUT` (300 ms). Messages carry an `int32_t` value in their first 4 bytes
(`MakeMsg()` / `MsgValue()`), the remaining bytes are filler.

`CancelVictim(bool use_sleep_cancel)` selects the cancel route. `ReleaseStuckWaiters()` is the
**safety net**: if the pass condition is not met, it pushes messages through the queue in both
directions so that any still-blocked getter, peeker or putter is released. A broken cancel is
therefore reported as a failed test instead of hanging the kernel (which only stops when all
tasks exit).

Task-count gating gains one predicate: `NeedsThreeTasks()` (name prefixes `"Cancel"` and `"DeleteWithWaiter"`)
registers tasks 0–2.

### Delete-with-waiter tests (tests 29–35)

These tests destroy a **separate** queue: `g_DeleteQueue` points to a `MessageQueueT<_STK_MQ_CAPACITY, _STK_MQ_MSG_SIZE>`
(type `DeleteQueueT`) constructed with placement-new in the static `g_DeleteQueueObj`, so the queue `g_Queue` of `RunTest()` is
never destroyed by the test itself. Its storage is internal to the object, so the whole queue, buffer included, is overwritten
with the byte `DELETED_FILL` (`0xDD`) after the destruction, and task 0 checks at the end that every byte still holds it: a woken
waiter that stored a message, advanced an index or changed the count of the destroyed queue would leave a trace. The tests reuse
`g_Victim`, `g_VictimTid`, `g_VictimResult[]`, `g_VictimElapsed[]`, `g_VictimValue[]` and `g_VictimRxIntact` (index 0: task 1,
index 1: task 2), the helpers `MakeMsg()`, `MsgValue()` and `TryPutVal()`, and add:

| Variable | Purpose |
|----------|---------|
| `g_DeleteQueue` | Pointer to the dedicated queue; set back to `nullptr` right after the destruction |
| `g_DeleteVictim2` | Task 2 (second getter), published right before it blocks; used by the safety net |
| `g_DeleteCheckResult` / `g_DeleteCheckElapsed` / `g_DeleteCheckIntact` | `EWaitResult`, milliseconds and receive-buffer integrity of the follow-up call (20 ticks) of task 1 on `g_Queue` (`-100` = not set) |
| `g_DeleteParam` | Scenario selector (`DW_GET = 1` … `DW_TWO_GETTERS = 7`, see the tests below); a global for the same reason as `g_CancelParam` |

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) insufficient RAM prevents linking the full set of
distinct task class templates simultaneously. All tests except `StressTask` (test 12),
including the cancellation tests 14–28 and the delete-with-waiter tests 29–35, are skipped on M0 under `#ifndef __ARM_ARCH_6M__`.
The cancellation tests add one more task class template (`CancelWaitTask`) and the delete-with-waiter tests
another (`DeleteWithWaiterTask`).

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

## Cancellation Tests (tests 14–28)

These tests verify that a task blocked inside a `MessageQueue` call can be cancelled, and that
the cancel is clean: the right `EWaitResult` is reported, nothing is enqueued, dequeued or
copied, no message is lost or duplicated, and the wait list stays consistent. `MessageQueue`
builds on `ConditionVariable`, so they also cover the cancel path of `ConditionVariable::WaitEx()`.
Shared state, scenario selection and task roles are described under
[Test Configuration](#cancellation-tests-tests-14–28). Tests 14–22 cancel through
`IKernel::CancelTaskWait()`; tests 23–28 repeat the key scenarios through `stk::SleepCancel()`.

> **Safety net:** if a cancel fails, the victim stays blocked and the kernel would never stop. On
> any failed pass condition the canceller calls `ReleaseStuckWaiters()` (puts and gets through the
> queue), so a broken cancel shows up as a failed test, not a hang.
>
> **Kernel requirement:** cancellation requires the `KERNEL_DYNAMIC | KERNEL_SYNC` kernel mode
> used by all tests in this suite.

All tests use tasks 0–2 only.

---

### Test 14 — `Cancel_GetInfinite`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `1`

Task 1 calls `GetEx(rx, WAIT_INFINITE)` on an empty queue at ~10 ms and blocks. Task 0 calls `CancelTaskWait(g_Victim)` at ~60 ms. The call must return `WAIT_RESULT_CANCELED` after roughly 50 ms, leave the receive buffer untouched, and the queue must still be empty. Basic cancel of a getter parked on `cv_not_empty`.

**Pass condition:** `result == CANCELED`, `elapsed ∈ [35, 75] ms`, receive buffer intact, `IsEmpty()`

---

### Test 15 — `Cancel_GetNotTimeout`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `2`

Same as test 14 but the victim uses a finite 300 ms timeout. The cancel at ~60 ms must end the wait early and be reported as `CANCELED`, not `TIMEOUT`. Guards against the cancel path being conflated with the timeout path in `GetEx`.

**Pass condition:** `result == CANCELED`, `elapsed < 150 ms`

---

### Test 16 — `Cancel_PutFull`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `3`

Task 0 pre-loads messages `0..7` so the queue is full; task 1 calls `PutEx(msg{999}, WAIT_INFINITE)` and blocks on `cv_not_full`. After the cancel the call must return `CANCELED`, the queue must still be full, and draining it must yield exactly `0..7` in order — the cancelled put stored nothing. A final put/get round trip confirms the queue still works.

**Pass condition:** `result == CANCELED`, `elapsed ∈ [35, 75] ms`, `IsFull()`, FIFO content `0..7` intact, queue usable afterwards

---

### Test 17 — `Cancel_GetNoStaleWaiter`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `4`

After the victim's get is cancelled, task 1 sleeps 60 ms and records the real duration. Task 0 then enqueues `321` with nobody waiting, reads it back with `TryGet()`, and polls once more. The message must stay in the queue (not lost to a ghost waiter), be consumed exactly once, and the victim's sleep must not be cut short by a stale wait entry.

**Pass condition:** `result == CANCELED`, put ok, first poll returns `321`, second poll empty, `victim_sleep >= 55 ms`

---

### Test 18 — `Cancel_GetTwoWaiters`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `5`

Task 1 and, 20 ms later, task 2 both block in `GetEx(WAIT_INFINITE)`. Task 0 cancels only task 1, then enqueues `77`. The cancel must leave the wait list consistent: task 2 is woken by the put with `SIGNAL` and receives `77`; task 1 stays `CANCELED` and its buffer untouched.

**Pass condition:** task 1 `CANCELED`, buffer intact; task 2 `SIGNAL`, `value[1] == 77`; queue empty

---

### Test 19 — `Cancel_GetSignalWins`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `6`

Task 0 calls `TryPut(777)` and `CancelTaskWait` back to back with no scheduling point between them. The put already signals the getter and removes it from the wait list, so the cancel must be a no-op. The getter returns `SIGNAL` with the message `777`; it is not cancelled and the message is not lost.

**Pass condition:** put ok, `result == SIGNAL`, `value == 777`, queue empty

---

### Test 20 — `Cancel_PutFrontFull`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `7`

Like test 16, but the victim calls `PutFrontEx()`. Because `PutFront` retreats the read pointer (`m_tail`) only after a successful wait, a cancelled call must neither store its message nor move `m_tail`. Draining the full queue must return `0..7` in order — a stray retreat would rotate the sequence or surface the `999` payload first.

**Pass condition:** `result == CANCELED`, `elapsed ∈ [35, 75] ms`, `IsFull()`, FIFO content `0..7` intact, queue usable afterwards

---

### Test 21 — `Cancel_PeekEmpty`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `8`

Task 1 calls `PeekEx(rx, WAIT_INFINITE)` on an empty queue and blocks on `cv_not_empty`. After the cancel it must return `CANCELED` without copying anything. A put/get round trip afterwards confirms the cancelled peeker left no stale wait entry or pending wake-up behind (`Peek` hands the wake-up on to the next waiter on success, which must not misfire here).

**Pass condition:** `result == CANCELED`, `elapsed ∈ [35, 75] ms`, receive buffer intact, `IsEmpty()`, queue usable afterwards

---

### Test 22 — `Cancel_PeekFrontEmpty`
**Cancel via:** `CancelTaskWait` &nbsp;|&nbsp; **`g_CancelParam`:** `9`

Identical to test 21 but the victim calls `PeekFrontEx()`.

**Pass condition:** same as test 21

---

### Test 23 — `Cancel_SleepCancelGet`
**Cancel via:** `SleepCancel` &nbsp;|&nbsp; **`g_CancelParam`:** `101`

Identical scenario to test 14 (`GetInfinite`), but the canceller calls `stk::SleepCancel(g_VictimTid)` instead of `CancelTaskWait()`. Verifies that `SleepCancel` routes through `OnTaskSleepCancel` into the wait object's `Cancel()` when the task is blocked in a `MessageQueue` call, with the same outcome as the direct kernel call.

**Pass condition:** same as test 14

---

### Test 24 — `Cancel_SleepCancelPut`
**Cancel via:** `SleepCancel` &nbsp;|&nbsp; **`g_CancelParam`:** `103`

Identical scenario to test 16 (`PutFull`), but the canceller calls `stk::SleepCancel(g_VictimTid)` instead of `CancelTaskWait()`. Verifies that `SleepCancel` routes through `OnTaskSleepCancel` into the wait object's `Cancel()` when the task is blocked in a `MessageQueue` call, with the same outcome as the direct kernel call.

**Pass condition:** same as test 16

---

### Test 25 — `Cancel_SleepCancelNoStale`
**Cancel via:** `SleepCancel` &nbsp;|&nbsp; **`g_CancelParam`:** `104`

Identical scenario to test 17 (`GetNoStaleWaiter`), but the canceller calls `stk::SleepCancel(g_VictimTid)` instead of `CancelTaskWait()`. Verifies that `SleepCancel` routes through `OnTaskSleepCancel` into the wait object's `Cancel()` when the task is blocked in a `MessageQueue` call, with the same outcome as the direct kernel call.

**Pass condition:** same as test 17

---

### Test 26 — `Cancel_SleepCancelTwo`
**Cancel via:** `SleepCancel` &nbsp;|&nbsp; **`g_CancelParam`:** `105`

Identical scenario to test 18 (`GetTwoWaiters`), but the canceller calls `stk::SleepCancel(g_VictimTid)` instead of `CancelTaskWait()`. Verifies that `SleepCancel` routes through `OnTaskSleepCancel` into the wait object's `Cancel()` when the task is blocked in a `MessageQueue` call, with the same outcome as the direct kernel call.

**Pass condition:** same as test 18

---

### Test 27 — `Cancel_SleepCancelSignal`
**Cancel via:** `SleepCancel` &nbsp;|&nbsp; **`g_CancelParam`:** `106`

Identical scenario to test 19 (`GetSignalWins`), but the canceller calls `stk::SleepCancel(g_VictimTid)` instead of `CancelTaskWait()`. Verifies that `SleepCancel` routes through `OnTaskSleepCancel` into the wait object's `Cancel()` when the task is blocked in a `MessageQueue` call, with the same outcome as the direct kernel call.

**Pass condition:** same as test 19

---

### Test 28 — `Cancel_SleepCancelPutFront`
**Cancel via:** `SleepCancel` &nbsp;|&nbsp; **`g_CancelParam`:** `107`

Identical scenario to test 20 (`PutFrontFull`), but the canceller calls `stk::SleepCancel(g_VictimTid)` instead of `CancelTaskWait()`. Verifies that `SleepCancel` routes through `OnTaskSleepCancel` into the wait object's `Cancel()` when the task is blocked in a `MessageQueue` call, with the same outcome as the direct kernel call.

**Pass condition:** same as test 20

---

## Delete-with-waiter Tests (tests 29–35)

These tests verify that destroying a `MessageQueue` which tasks are blocked on releases **all** of them with
`WAIT_RESULT_DELETED`, for each way to wait, without transferring a message and without touching the destroyed queue. Task 0
constructs a dedicated queue (`g_DeleteQueue`, placement-new into `g_DeleteQueueObj`). For the putting scenarios it fills the queue first
(messages `0..7`): a putter waits only on a full queue, a getter or peeker only on an empty one. It then sleeps 60 ticks. Task 1 sleeps
`_STK_MQ_TEST_SHORT_SLEEP`, publishes its `ITask` pointer in `g_Victim` and blocks inside the queue call with `WAIT_INFINITE`: nobody
puts or gets, so only the destruction can end the wait. Task 0 then destroys the queue while it is waited on, and overwrites its
storage with `0xDD`, so that any access to the destroyed queue by a woken waiter would read garbage, and any write would be detected.
Task 1 records the result, the elapsed time and whether its receive buffer (pre-filled with `0x5A`) is intact, without touching the
destroyed queue. It then repeats the same call with a 20 tick timeout on `g_Queue` (empty for the reading scenarios; task 0 fills it
with `0..7` for the putting ones): the call must run to its deadline and return `WAIT_RESULT_TIMEOUT` without copying anything; a
stale wait entry left behind would end it early or with another result. Task 0 checks the outcome at about tick 100. If a waiter was
not released, task 0 cancels its wait with `g_Kernel.CancelTaskWait()`, so the test fails instead of hanging the kernel.

**Common pass condition:** the dedicated queue was in the expected state (empty or full) before the destruction; task 1's elapsed time is
in `[35, 75]` ms (waiting from ~10 ms, destroyed at ~60 ms); for a getter or peeker its receive buffer is intact; the follow-up call on
`g_Queue` returned `WAIT_RESULT_TIMEOUT` after at least 15 ms with its receive buffer intact; `g_Queue` is unchanged (empty, or still
holding `0..7` in FIFO order); every byte of the storage of the destroyed queue still holds `0xDD`

All tests use tasks 0–2 only (`"DeleteWithWaiter_*"` names); task 2 is added by `RunTest()` but stays idle except in test 35.

---

### Test 29 — `DeleteWithWaiter_Get`
**`g_DeleteParam`:** `1` (`DW_GET`)

Task 1 blocks in `GetEx(rx, WAIT_INFINITE)` on the empty dedicated queue.

**Pass condition:** common, and `result == WAIT_RESULT_DELETED` (not `SIGNAL`, `TIMEOUT` or `CANCELED`)

---

### Test 30 — `DeleteWithWaiter_Put`
**`g_DeleteParam`:** `2` (`DW_PUT`)

The dedicated queue is full; task 1 blocks in `PutEx(msg{999}, WAIT_INFINITE)` on `cv_not_full`. The destroyed queue must receive
neither the message nor an index or count update.

**Pass condition:** common, and `result == WAIT_RESULT_DELETED`

---

### Test 31 — `DeleteWithWaiter_PutFront`
**`g_DeleteParam`:** `3` (`DW_PUT_FRONT`)

Like test 30, but task 1 calls `PutFrontEx()`, which retreats the read pointer (`m_tail`) only after a successful wait: a released
call must neither store its message nor move `m_tail`.

**Pass condition:** common, and `result == WAIT_RESULT_DELETED`

---

### Test 32 — `DeleteWithWaiter_Peek`
**`g_DeleteParam`:** `4` (`DW_PEEK`)

Task 1 blocks in `PeekEx(rx, WAIT_INFINITE)` on the empty dedicated queue. A successful `Peek` hands the wake-up on to the next
waiter; a released one must not do that or copy anything.

**Pass condition:** common, and `result == WAIT_RESULT_DELETED`

---

### Test 33 — `DeleteWithWaiter_PeekFront`
**`g_DeleteParam`:** `5` (`DW_PEEK_FRONT`)

Identical to test 32 but task 1 calls `PeekFrontEx()`.

**Pass condition:** same as test 32

---

### Test 34 — `DeleteWithWaiter_GetBool`
**`g_DeleteParam`:** `6` (`DW_GET_BOOL`)

Task 1 uses the boolean `Get(rx, WAIT_INFINITE)`, which collapses a destruction into `false`, the same as a timeout or a cancellation.
An infinite wait can not time out and nobody cancels it, so `false` after ~50 ms identifies the destruction.

**Pass condition:** common, and `Get()` returned `false`

---

### Test 35 — `DeleteWithWaiter_TwoGetters`
**`g_DeleteParam`:** `7` (`DW_TWO_GETTERS`)

Task 1 and, 20 ms later, task 2 both block in `GetEx(WAIT_INFINITE)` on the empty dedicated queue. The destruction must release both: the
wait list of the condition variable is walked completely, not just up to its first entry.

**Pass condition:** common, and task 2 also gets `WAIT_RESULT_DELETED` with its receive buffer intact and elapsed time in `[25, 65]` ms
(waiting from ~20 ms)

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
| 14  | `Cancel_GetInfinite` | 0–2 | `_STK_MQ_STACK_SIZE` | `CANCELED`, `elapsed ∈ [35, 75] ms`, buffer intact | `CancelTaskWait` ends an infinite `GetEx` on an empty queue |
| 15  | `Cancel_GetNotTimeout` | 0–2 | `_STK_MQ_STACK_SIZE` | `CANCELED`, `elapsed < 150 ms` | Cancel is reported as `CANCELED`, not `TIMEOUT` |
| 16  | `Cancel_PutFull` | 0–2 | `_STK_MQ_STACK_SIZE` | `CANCELED`, content `0..7` intact | Cancelled `PutEx` on a full queue stores nothing |
| 17  | `Cancel_GetNoStaleWaiter` | 0–2 | `_STK_MQ_STACK_SIZE` | `CANCELED`, message read once, `sleep >= 55 ms` | No stale waiter left: next message neither lost nor consumed twice |
| 18  | `Cancel_GetTwoWaiters` | 0–2 | `_STK_MQ_STACK_SIZE` | 1st `CANCELED`, 2nd `SIGNAL` with `77` | Cancelling one of two blocked getters leaves the other intact |
| 19  | `Cancel_GetSignalWins` | 0–2 | `_STK_MQ_STACK_SIZE` | `SIGNAL`, `value == 777` | Cancel after the waiter was already signaled is a no-op |
| 20  | `Cancel_PutFrontFull` | 0–2 | `_STK_MQ_STACK_SIZE` | `CANCELED`, content `0..7` intact | Cancelled `PutFrontEx` stores nothing and does not move the read pointer |
| 21  | `Cancel_PeekEmpty` | 0–2 | `_STK_MQ_STACK_SIZE` | `CANCELED`, buffer intact, queue usable | Cancelled `PeekEx` copies nothing and leaves no stale state |
| 22  | `Cancel_PeekFrontEmpty` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 21 | Cancelled `PeekFrontEx` copies nothing and leaves no stale state |
| 23  | `Cancel_SleepCancelGet` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 14 | `SleepCancel` cancels a blocked `GetEx` |
| 24  | `Cancel_SleepCancelPut` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 16 | `SleepCancel` cancels a blocked `PutEx` |
| 25  | `Cancel_SleepCancelNoStale` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 17 | `SleepCancel` leaves no stale waiter |
| 26  | `Cancel_SleepCancelTwo` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 18 | `SleepCancel` of one of two getters leaves the other intact |
| 27  | `Cancel_SleepCancelSignal` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 19 | `SleepCancel` after signal is a no-op |
| 28  | `Cancel_SleepCancelPutFront` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 20 | `SleepCancel` cancels a blocked `PutFrontEx` cleanly |
| 29  | `DeleteWithWaiter_Get` | 0–2 | `_STK_MQ_STACK_SIZE` | `DELETED`, elapsed `[35, 75]` ms, buffer intact, follow-up wait on `g_Queue` times out `>= 15` ms, storage untouched | Destroying a queue which a getter waits on releases it with `WAIT_RESULT_DELETED`; nothing is copied; no stale wait entry is left behind |
| 30  | `DeleteWithWaiter_Put` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 29, `g_Queue` content `0..7` intact | A putter on a full queue is released with `DELETED`; it stores nothing in, and does not touch, the destroyed queue |
| 31  | `DeleteWithWaiter_PutFront` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 30 | A released `PutFrontEx` stores nothing and does not retreat the read pointer |
| 32  | `DeleteWithWaiter_Peek` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 29 | A peeker is released with `DELETED`, copies nothing and passes no wake-up on |
| 33  | `DeleteWithWaiter_PeekFront` | 0–2 | `_STK_MQ_STACK_SIZE` | as test 29 | Same for `PeekFrontEx` |
| 34  | `DeleteWithWaiter_GetBool` | 0–2 | `_STK_MQ_STACK_SIZE` | `Get()` returns `false` after `[35, 75]` ms, as test 29 | The boolean `Get()` collapses a destruction into `false`; an infinite wait can not time out, so it identifies the destruction |
| 35  | `DeleteWithWaiter_TwoGetters` | 0–2 | `_STK_MQ_STACK_SIZE` | both `DELETED`, task 2 elapsed `[25, 65]` ms, as test 29 | The destruction releases every waiter, not only the first one |
