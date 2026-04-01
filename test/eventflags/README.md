# EventFlags Test Suite

Test suite for `stk::sync::EventFlags`.  
Source: `test/eventflags/test_eventflags.cpp`

---

## API Summary

```cpp
#include <sync/stk_sync_eventflags.h>
```

`EventFlags` is a 32-bit multi-flag synchronization primitive. Each bit in the
internal flags word represents an independent boolean event. Tasks can set or clear
any combination of bits and block until any one (OR semantics) or all (AND semantics)
of a requested subset become set.

Unlike `Event`, which models a single binary signal, `EventFlags` coordinates up to
31 independent conditions in a single object. Bit 31 is reserved for error sentinels
and must never be set by the caller.

Requires kernel mode: `KERNEL_DYNAMIC | KERNEL_SYNC`.

```cpp
// Construction
stk::sync::EventFlags g_Flags;           // all flags clear (default)
stk::sync::EventFlags g_Flags(FLAG_A);  // constructed with FLAG_A pre-set
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Set` | `uint32_t Set(uint32_t flags)` | Atomically OR-sets the specified bits; wakes **all** current waiters so each can re-evaluate its predicate. Returns the flags word **after** setting. Returns `ERROR_PARAMETER` if `flags` is 0 or has bit 31 set. ISR-safe. |
| `Clear` | `uint32_t Clear(uint32_t flags)` | Atomically clears the specified bits. Returns the flags word **before** clearing. Returns `ERROR_PARAMETER` if `flags` is 0 or has bit 31 set. ISR-safe. |
| `Get` | `uint32_t Get() const` | Non-destructive point-in-time snapshot of the flags word. Never blocks or modifies state. ISR-safe. |
| `Wait` | `uint32_t Wait(uint32_t flags, uint32_t options = OPT_WAIT_ANY, Timeout timeout = WAIT_INFINITE)` | Blocks until the flag condition is satisfied or the timeout expires. On success, clears matched bits unless `OPT_NO_CLEAR` is set. Returns the matched bitmask on success, or an `ERROR_*` sentinel on failure. ISR-safe only with `timeout = NO_WAIT`. |
| `TryWait` | `uint32_t TryWait(uint32_t flags, uint32_t options = OPT_WAIT_ANY)` | Non-blocking poll; equivalent to `Wait(flags, options, NO_WAIT)`. Returns matched bitmask on success, or `ERROR_TIMEOUT` immediately if the condition is not met. Never blocks. ISR-safe. |
| `IsError` | `static bool IsError(uint32_t result)` | Returns `true` if the return value carries an error sentinel (bit 31 set). Use after every `Set()`, `Clear()`, and `Wait()` call. |
| `~EventFlags` | (destructor) | Asserts no dangling waiters via the internal `Mutex` and `ConditionVariable` destructors in debug builds. |

**Options bitmask (passed to `Wait()` / `TryWait()`):**

| Constant | Value | Meaning |
|----------|-------|---------|
| `OPT_WAIT_ANY` | `0x00000000` | Unblock when **any** requested bit is set (OR semantics, default) |
| `OPT_WAIT_ALL` | `0x00000001` | Unblock only when **all** requested bits are simultaneously set (AND semantics) |
| `OPT_NO_CLEAR` | `0x00000002` | Do not clear matched bits on a successful return |

Options may be combined: `OPT_WAIT_ALL | OPT_NO_CLEAR`.

**Error sentinels returned by `Set()`, `Clear()`, `Wait()`, `TryWait()`:**

| Constant | Value | Meaning |
|----------|-------|---------|
| `ERROR_PARAMETER` | `0x80000001` | `flags` argument is 0 or has bit 31 set |
| `ERROR_TIMEOUT` | `0x80000002` | Timeout expired before the flag condition was met |
| `ERROR_ISR` | `0x80000004` | `Wait()` called from an ISR with a blocking timeout |
| `ERROR_MASK` | `0x80000000` | Mask for testing any error; bit 31 set means error |

**Behavior by option:**

| Operation | `OPT_WAIT_ANY` (default) | `OPT_WAIT_ALL` |
|-----------|----------------------|------------|
| `Wait()` unblocks when | Any one requested bit is set | Every requested bit is simultaneously set |
| Matched bits cleared (default) | The bits that triggered the wake | All requested bits |
| `OPT_NO_CLEAR` effect | Matched bits stay set; next `Wait()` fast-paths | All requested bits stay set; next `Wait()` fast-paths |
| `TryWait()` not satisfied | Returns `ERROR_TIMEOUT` immediately | Returns `ERROR_TIMEOUT` immediately |
| `Set()` with waiters | Wakes **all** waiters; each re-evaluates its predicate | Same |
| `Set()` with no waiters | Bits stay set; next `Wait()` fast-paths | Same |
| `Clear()` | Returns pre-clear word; blocked `OPT_WAIT_ANY` waiters for cleared bits will time out | Same |

**Key invariants:**

- `Set()` always wakes **all** current waiters. Each waiter independently decides
  whether its own predicate is now satisfied, then clears only its matched bits.
  Concurrent waiters with disjoint flag masks do not interfere with each other.
- `Clear()` returns the flags word **before** the clear; the caller can determine
  which bits were actually set at the time of the call.
- `Get()` is purely observational. It never blocks, never modifies state, and never
  clears any bit. A subsequent `Wait()` for bits observed by `Get()` will still
  succeed if those bits have not been cleared by another task in the interim.
- `OPT_WAIT_ALL` re-evaluates the full predicate each time it is woken, guarding against
  spurious wakeups and races where a bit is set and cleared between two `Set()` calls.
- Destroying an `EventFlags` object while tasks are waiting is a logic error; an
  assertion fires in debug builds via the internal `Mutex` and `ConditionVariable`
  destructors.

---

## Test Configuration

| Constant | Value | Purpose |
|----------|-------|---------|
| `_STK_EF_TEST_TASKS_MAX` | `5` | Total tasks per test run |
| `_STK_EF_TEST_TIMEOUT` | `300` ticks | Blocking timeout for `Wait()` calls that must succeed |
| `_STK_EF_TEST_SHORT_SLEEP` | `10` ticks | Sleep used to pace task sequencing |
| `_STK_EF_TEST_LONG_SLEEP` | `100` ticks | Sleep used by the verifier task to wait for workers |
| `_STK_EF_STACK_SIZE` | `128` (M0) / `256` (others) | Per-task stack size in `size_t` words |

`g_Flags` is **reconstructed in-place** via placement-new inside `ResetTestState()`
before each test. `ResetTestState()` accepts an `initial_flags` parameter forwarded
directly to the `EventFlags` constructor, so the same helper serves all ten tests
without any additional per-test setup code.

Shared flag bit definitions used across all tests:

```cpp
static const uint32_t FLAG_A = (1U << 0);
static const uint32_t FLAG_B = (1U << 1);
static const uint32_t FLAG_C = (1U << 2);
static const uint32_t FLAG_D = (1U << 3);
```

---

## Platform Notes

On **Cortex-M0** (`__ARM_ARCH_6M__`) the device has insufficient RAM to link ten
distinct task class templates simultaneously. Test 1 (`SetWaitAny`) runs on all
platforms; tests 2–10 are skipped on M0 under `#ifndef __ARM_ARCH_6M__`.

`SetWaitAny` runs on M0 because it uses a single task class template
(`SetWaitAnyTask`) instantiated for all five task slots, fitting within the available
memory. Tests 2–10 each introduce a distinct task class template, multiplying the
static memory footprint beyond what M0 can accommodate.

| Platform | `_STK_EF_STACK_SIZE` |
|----------|-----------------------|
| Cortex-M0 (`__ARM_ARCH_6M__`) | `128` words |
| All others | `256` words |

---

## Tests

### Test 1 — `SetWaitAny`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY` &nbsp;|&nbsp; **Param:** `iterations = 20` &nbsp;|&nbsp; **Runs on M0: yes**

Task 0 is the producer; tasks 1–4 are consumers. All four consumers loop calling
`Wait(FLAG_A | FLAG_B | FLAG_C | FLAG_D, OPT_WAIT_ANY, _STK_EF_TEST_TIMEOUT)`. The
producer alternates between `FLAG_A` and `FLAG_B`, calling `Set()` 20 times with
`Delay(1)` between calls so each signal lands on exactly one blocked consumer before
the next fires. Each successful wake increments `g_SharedCounter`. Verifies that the
auto-clear behavior of `Wait()` consumes the matching bit, leaving the remaining
consumers blocked until the next `Set()`.

**Pass condition:** `counter == 20`

---

### Test 2 — `SetWaitAll`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Options:** `OPT_WAIT_ALL`

Task 1 blocks on `Wait(FLAG_A | FLAG_B | FLAG_C, OPT_WAIT_ALL, _STK_EF_TEST_TIMEOUT)`.
Task 0 sets the three flags one at a time with `_STK_EF_TEST_SHORT_SLEEP` delays
between each. Task 1 must remain blocked after each individual `Set()` and only wake
when all three bits are simultaneously present after the final `Set(FLAG_C)`. The
returned value must equal exactly `FLAG_A | FLAG_B | FLAG_C` with no extra bits set.

**Pass condition:** `counter == 1` and `result == (FLAG_A | FLAG_B | FLAG_C)`

---

### Test 3 — `Clear`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY`

Task 0 calls `Set(FLAG_A | FLAG_B)` then `Clear(FLAG_A)`. Verifies three properties of
`Clear()`: the return value equals the flags word **before** clearing (both bits
present); `FLAG_A` is absent from `Get()` after the call; `FLAG_B` remains set.
Task 1 then calls `Wait(FLAG_B)`, which must succeed immediately on the still-set
`FLAG_B`. Task 2 calls `Wait(FLAG_A)` with a short timeout; since `FLAG_A` was
cleared, this must time out — any non-error return is counted as a failure.

**Pass condition:** `pre_ok == true`, `post_ok == true`, `flag_b_ok == true`, `counter == 1`

---

### Test 4 — `NoClear`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY | OPT_NO_CLEAR`

Task 0 sleeps briefly to let tasks 1 and 2 reach their `Wait()` calls, then sets
`FLAG_A`. Both task 1 and task 2 call `Wait(FLAG_A, OPT_WAIT_ANY | OPT_NO_CLEAR, timeout)`.
Because neither clears the flag on return, the second waiter must also succeed on
the same bit. Verifies that `OPT_NO_CLEAR` makes a single `Set()` satisfy multiple
concurrent waiters — behaviour that would otherwise require a manual-reset `Event`.

**Pass condition:** `counter == 2`

---

### Test 5 — `Timeout`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY`

Task 1 calls `Wait(FLAG_A, OPT_WAIT_ANY, 50)` after `_STK_EF_TEST_SHORT_SLEEP`. No
producer fires within the 50-tick window so it must return `ERROR_TIMEOUT` with
measured elapsed time in `[45, 60]` ms. Task 0 withholds `Set()` until tick 200.
Task 2 sleeps until tick 210 then calls `Wait(FLAG_A, OPT_WAIT_ANY, 100)`, which must
succeed on the now-set flag. Task 2 is the verifier.

**Pass condition:** `counter == 2`  
(1 = `ERROR_TIMEOUT` returned within correct timing bounds; 2 = `Wait()` succeeded after `Set()`)

---

### Test 6 — `TryWait`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY`

Task 1 calls `TryWait(FLAG_A)` on a non-set flag; must return `ERROR_TIMEOUT`
immediately (elapsed < `_STK_EF_TEST_SHORT_SLEEP`). Task 2 calls `Set(FLAG_A)` then
`TryWait(FLAG_A)`; must return the matched bitmask immediately (event just set) and
auto-clear `FLAG_A`. Task 2 then calls `TryWait(FLAG_A)` a second time to confirm
the clear: must return `ERROR_TIMEOUT`. Covers both the non-set and set fast paths
and the auto-clear `TryWait()` performs on success.

**Pass condition:** `counter == 3`  
(1 = non-set `TryWait()` returned an error immediately; 2 = set `TryWait()` returned a match immediately; 3 = post-clear `TryWait()` returned an error)

---

### Test 7 — `Get`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY`

Task 0 calls `Get()` on an empty flags word (must be 0), sets `FLAG_A | FLAG_C`,
calls `Get()` again (must have both bits), clears `FLAG_C`, calls `Get()` a third
time (must equal exactly `FLAG_A`). Task 1 sleeps to let task 0 complete the setup,
calls `Get()` to observe `FLAG_A`, then calls `Wait(FLAG_A, OPT_WAIT_ANY, timeout)` to confirm that `Get()`
did not consume the flag — the `Wait()` must succeed immediately. Verifies that
`Get()` is purely observational at every stage.

**Pass condition:** `empty_ok == true`, `set_ok == true`, `clear_ok == true`, `counter == 1`

---

### Test 8 — `MultiWaiterAny`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY`

Each consumer (tasks 1–4) watches a unique flag bit: task 1 waits for `FLAG_A`,
task 2 for `FLAG_B`, task 3 for `FLAG_C`, task 4 for `FLAG_D`. After
`_STK_EF_TEST_SHORT_SLEEP` the producer calls `Set(FLAG_A | FLAG_B | FLAG_C | FLAG_D)`
once, which must wake all four simultaneously. Each consumer verifies that its own
expected bit is present in the returned bitmask. Demonstrates that concurrent
`OPT_WAIT_ANY` waiters with disjoint flag masks are all satisfied by a single `Set()`
without interfering with each other.

**Pass condition:** `counter == 4`

---

### Test 9 — `MultiWaiterAll`
**Tasks:** 0–4 (all 5) &nbsp;|&nbsp; **Options:** `OPT_WAIT_ALL` &nbsp;|&nbsp; **Param:** `consumers = 4`

All four consumers (tasks 1–4) block on `Wait(FLAG_A | FLAG_B, OPT_WAIT_ALL, timeout)`.
Because the default auto-clear behavior causes the first waker to consume both bits,
the producer loops, calling `Set(FLAG_A | FLAG_B)` with `Delay(1)` between iterations
— once per consumer — to restock the flags after each take. Every consumer must
eventually wake and verify that the returned value equals `FLAG_A | FLAG_B` exactly.

**Pass condition:** `counter == 4`

---

### Test 10 — `InitialFlags`
**Tasks:** 0–2 only &nbsp;|&nbsp; **Options:** `OPT_WAIT_ANY` &nbsp;|&nbsp; **Initial flags:** `FLAG_A`

The `EventFlags` object is constructed with `FLAG_A` pre-set via the `initial_flags`
constructor parameter. Task 1 calls `Wait(FLAG_A, OPT_WAIT_ANY, _STK_EF_TEST_SHORT_SLEEP)`
immediately; because the flag is already set, `Wait()` takes the fast path and returns
the matched mask without blocking, then auto-clears `FLAG_A`. Task 1 then calls
`Wait()` a second time; the flag has been consumed so this must return `ERROR_TIMEOUT`.
Verifies the `initial_flags` constructor parameter and the fast-path auto-clear
behavior.

**Pass condition:** `counter == 2`  
(1 = first `Wait()` succeeded immediately on pre-set flag; 2 = second `Wait()` timed out after auto-clear)

---

## Summary Table

| # | Test | Tasks | Options | Pass condition | What it verifies |
|---|------|-------|---------|----------------|------------------|
| 1 | `SetWaitAny` | 0–4 | `OPT_WAIT_ANY` | `counter == 20` | `Set()` unblocks exactly one `OPT_WAIT_ANY` waiter per call via auto-clear; runs on all platforms including M0 |
| 2 | `SetWaitAll` | 0–2 | `OPT_WAIT_ALL` | `counter == 1`, `result == FLAG_A\|FLAG_B\|FLAG_C` | `Wait()` holds until **all** requested bits are simultaneously present; returned mask equals the full request |
| 3 | `Clear` | 0–2 | `OPT_WAIT_ANY` | `pre/post/flag_b_ok == true`, `counter == 1` | `Clear()` returns the pre-clear word; cleared bits block subsequent waiters; un-cleared bits remain available |
| 4 | `NoClear` | 0–2 | `OPT_WAIT_ANY \| OPT_NO_CLEAR` | `counter == 2` | `OPT_NO_CLEAR` leaves matched bits set so multiple concurrent waiters can each succeed on the same `Set()` |
| 5 | `Timeout` | 0–2 | `OPT_WAIT_ANY` | `counter == 2` | `Wait()` returns `ERROR_TIMEOUT` within correct timing bounds `[45, 60]` ms; object remains usable after timeout |
| 6 | `TryWait` | 0–2 | `OPT_WAIT_ANY` | `counter == 3` | `TryWait()` returns an error on a non-set flag immediately; returns a match and auto-clears on a set flag; subsequent call returns error |
| 7 | `Get` | 0–2 | `OPT_WAIT_ANY` | `empty/set/clear_ok == true`, `counter == 1` | `Get()` is non-destructive at every stage; bits observed by `Get()` are still consumable by a subsequent `Wait()` |
| 8 | `MultiWaiterAny` | 0–4 | `OPT_WAIT_ANY` | `counter == 4` | Concurrent `OPT_WAIT_ANY` waiters with disjoint flag masks are all released by one `Set()` without interfering |
| 9 | `MultiWaiterAll` | 0–4 | `OPT_WAIT_ALL` | `counter == 4` | Multiple concurrent `OPT_WAIT_ALL` waiters each receive the full mask; producer re-stocks flags after each auto-clear |
| 10 | `InitialFlags` | 0–2 | `OPT_WAIT_ANY` | `counter == 2` | Constructor `initial_flags` pre-sets bits; first `Wait()` fast-paths immediately; auto-clear blocks the second call |
