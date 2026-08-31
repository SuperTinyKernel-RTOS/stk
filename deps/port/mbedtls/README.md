# mbedTLS port for SuperTinyKernel (STK)

```
deps/port/mbedtls/
│   stk_mbedtls_other.cpp
│   stk_mbedtls_threading.cpp
│   stk_mbedtls_time.cpp
│
├───include
│       threading_alt.h
│
└───other
        psa_crypto_driver_wrappers.h
        psa_crypto_driver_wrappers_no_static.c
```

- **`include/threading_alt.h`** — the `MBEDTLS_THREADING_ALT` opaque storage
  types (`mbedtls_platform_mutex_t`, `mbedtls_platform_condition_variable_t`)
  mbedTLS's `threading.h` expects, each a raw `uintptr_t[]` buffer sized to
  hold a placement-constructed `stk::sync::Mutex` / `stk::sync::
  ConditionVariable`. Stays valid C so core mbedTLS `.c` translation units
  can include it directly.
- **`stk_mbedtls_threading.cpp`** — the implementation behind that header:
  registers `stk_mutex_*`/`stk_cond_*` with `mbedtls_threading_set_alt()`.
  **Must be compiled as C++** (it placement-constructs `stk::sync::Mutex`/
  `ConditionVariable` into the storage above), but every function it
  registers is a plain `extern "C"` callback, so it links cleanly against
  mbedTLS's C sources.
- **`stk_mbedtls_time.cpp`** — implements `mbedtls_ms_time()` via
  `stk::GetTimeNowMs()`. Compiled as C++ for consistency with the rest of
  the port; the entry point is `extern "C"`.
- **`stk_mbedtls_other.cpp`** — implements `mbedtls_platform_get_entropy()`
  and `mbedtls_psa_external_get_random()`, backed by a per-MCU-family
  hardware RNG where one is available and by a CPU-cycle-jitter fallback
  otherwise.
- **`other/psa_crypto_driver_wrappers_no_static.c`** — hand-edited,
  non-`static` copies of `psa_driver_wrapper_get_key_buffer_size()`,
  `psa_driver_wrapper_export_public_key()`, and
  `psa_driver_wrapper_get_builtin_key()`, built against a matching
  `psa_crypto_driver_wrappers_no_static.h` (not part of this port — it
  lives in your PSA crypto tree). Needed because some embedded toolchains
  emit "missing symbol" link errors for these three functions when they're
  left as `static inline` in the auto-generated
  `psa_crypto_driver_wrappers.h`.
- **`other/psa_crypto_driver_wrappers.h`** — the unmodified, auto-generated
  driver-wrapper header (from `psa_crypto_driver_wrappers.jinja`), kept here
  for reference only, so the three functions pulled out above can be
  diffed against their generated originals whenever the codegen template
  changes upstream. Not compiled as part of this port.

## One extra setup step: initializing before any mbedTLS call

Unlike the lwIP port, this one has no kernel-registration step — none of
`stk_mbedtls_*` needs an `stk::IKernel*` pointer. What it does need is to run
**before any other mbedTLS API call**, per the contract of
`mbedtls_threading_set_alt()`:

```cpp
#include "threading_alt.h"   // stk_mbedtls_threading_init()

int main()
{
    // No STK task context required yet: this only registers function
    // pointers and placement-constructs mbedTLS's static global mutexes
    // (readdir/gmtime/PSA key-slot/globaldata/rngdata, depending on which
    // are compiled in) - it doesn't touch stk::sync::Mutex's lock/wait path.
    stk_mbedtls_threading_init();

    // ... now safe to call mbedtls_x509_*, mbedtls_ssl_*, psa_crypto_init(), ...

    g_kernel.Initialize();
    g_kernel.AddTask(&g_tls_task);
    g_kernel.Start();   // mbedtls_threading_* callbacks assert a valid task
                         // TId from here on - see "Notable implementation
                         // choices" below
}
```

If your application tears mbedTLS down, pair this with
`mbedtls_threading_free_alt()` at shutdown.

## Required `mbedtls_config.h` settings

```c
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
```

No other STK-specific defines are required to build; `MBEDTLS_PSA_CRYPTO_C`
is optional but typical, and just means `stk_mbedtls_threading_init()` also
initializes the PSA key-slot/globaldata/rngdata mutexes (see
`threading_alt.h`).

## Primitive mapping

| mbedTLS need                              | STK backing                          | Allocation |
|--------------------------------------------|----------------------------------------|------------|
| `mbedtls_platform_mutex_t`                 | `stk::sync::Mutex`                     | placement-new into fixed `uintptr_t[STK_MBEDTLS_MUTEX_STORAGE_SIZE]` storage embedded in the struct |
| `mbedtls_platform_condition_variable_t`    | `stk::sync::ConditionVariable`         | placement-new into fixed `uintptr_t[STK_MBEDTLS_COND_STORAGE_SIZE]` storage embedded in the struct |
| `mbedtls_ms_time()`                        | `stk::GetTimeNowMs()`                  | n/a |
| `mbedtls_platform_get_entropy()` / `mbedtls_psa_external_get_random()` | on-chip HW RNG when available, else CPU-cycle-jitter fallback | n/a |

Unlike the lwIP port's mailbox/thread/semaphore pools, there is no
fixed-capacity pool here to size: mbedTLS only ever creates a small, fixed
number of mutexes/condition variables (its own static globals, plus
whatever your application layer allocates), so each lives in storage
embedded directly in its `mbedtls_platform_*_t`, sized by the
`STK_MBEDTLS_*_STORAGE_SIZE` macros below.

## Notable implementation choices

- **Storage sizes are guarded at compile time, not guessed at.**
  `sizeof(stk::sync::Mutex)`/`ConditionVariable` depend on your STK build
  configuration (32- vs 64-bit `stk::Word`, `STK_ARCH_CPU_COUNT`, TrustZone
  variant, ...), so `threading_alt.h`'s defaults
  (`STK_MBEDTLS_MUTEX_STORAGE_SIZE = 11`, `STK_MBEDTLS_COND_STORAGE_SIZE =
  8`) are generous, not exact. `stk_mbedtls_threading.cpp` `static_assert`s
  both the size and alignment of the reserved storage against the real STK
  types; if either fails, override the macro (build define, or edit
  `threading_alt.h`) and rebuild.
- **mbedTLS's threading calls must happen from task context.** `Lock()`,
  `Wait()`, etc. on `stk::sync::Mutex`/`ConditionVariable` assert on a valid,
  non-ISR `stk::TId`. Don't drive TLS/X.509/PK code from an ISR.
- **Mutex recursion support goes unused.** `stk::sync::Mutex` is recursive,
  but mbedTLS's contract never re-locks a mutex the calling thread already
  holds, so `stk_mutex_lock()`/`unlock()` forward straight through with no
  extra bookkeeping.
- **Destroy contracts are enforced by assertion, not by this port.**
  `~Mutex()` asserts it isn't locked and has no waiters; `~ConditionVariable()`
  asserts no task is waiting on it — matching `threading.h`'s documented
  contract for `mutex_free`/`cond_free`.
- **Condition-variable wait failure is surfaced, not swallowed.**
  `stk_cond_wait()` uses `stk::WAIT_INFINITE` and tolerates spurious
  wakeups (mbedTLS's callers already loop on their own predicate), but if
  `ConditionVariable::Wait()` itself returns false — an internal STK error,
  not a timeout, since `WAIT_INFINITE` never produces one — that's reported
  back as `MBEDTLS_ERR_THREADING_USAGE_ERROR` rather than silently claimed
  as success.
- **Entropy source depends on your target, detected at compile time** via
  `STM32F0`/`F1`/.../`WL`, `__MCUXPRESSO__`, and `_PICO_H` macros:
  - **STM32** (`UID_BASE` defined): `HAL_RNG_GenerateRandomNumber()`, lazily
    `HAL_RNG_Init()`'d if the HAL handle (`hrng`, provided by your
    application's startup code) is found in `HAL_RNG_STATE_RESET`, and
    `HAL_RNG_DeInit()`'d again afterward only if this call was the one that
    brought it up.
  - **NXP** (`SIM` defined): `RNGA_GetRandomData()`, lazily initialized once
    and temporarily woken from `kRNGA_ModeSleep` if needed.
  - **RP2040/RP2350** (`_PICO_H` defined): raw ring-oscillator bits
    (`rosc_hw->randombit`), bit-banged into bytes.
  - **Anything else**: a CPU-cycle-jitter fallback (`stk::hw::HiResClock`
    timing variance across a short delay loop, mixed via FNV-1a), whitened
    with the MCU's unique ID plus the calling task's `TId`/tick count as
    defense-in-depth. This path is **not SP 800-90B qualified and has no
    online health testing**, so `mbedtls_platform_get_entropy()` reports a
    conservative, non-full entropy estimate (1 bit/byte) for it — real HW
    RNG paths report full entropy credit.
- **`psa_crypto_driver_wrappers_no_static.c` is a linkage workaround, not a
  behavior change.** The three functions it defines keep the exact bodies
  their auto-generated, `static inline` counterparts would have; only the
  linkage changes, to work around toolchains that fail to resolve them when
  left `static inline` in the generated header.

## Compile-time configuration (define before including, or via build flags)

| Macro | Default | Meaning |
|---|---|---|
| `STK_MBEDTLS_MUTEX_STORAGE_SIZE` | `11` | Size (in `uintptr_t`s) of the embedded storage for a placement-constructed `stk::sync::Mutex`. Raise if the `static_assert` in `stk_mbedtls_threading.cpp` fails for your target. |
| `STK_MBEDTLS_COND_STORAGE_SIZE` | `8` | Size (in `uintptr_t`s) of the embedded storage for a placement-constructed `stk::sync::ConditionVariable`. Raise if the corresponding `static_assert` fails. |

## Requirements on the kernel instance

- **`stk::KERNEL_SYNC`** is mandatory — the port is built entirely on
  `stk::sync::Mutex`/`ConditionVariable`.
- No particular kernel instantiation order is required relative to
  `stk_mbedtls_threading_init()` itself (see setup step above); it's only
  the *use* of TLS/X.509/PK/PSA crypto APIs afterward that requires the
  calling task to be running under a started kernel.
