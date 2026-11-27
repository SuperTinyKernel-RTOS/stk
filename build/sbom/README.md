# SBOM (SPDX 2.3)

STK can generate a Software Bill of Materials in SPDX 2.3 JSON format for **one build configuration** of the library.
The list of third-party code depends on how STK is configured (target, CPU family, tracing), so there is no single
static SBOM file: it is generated per configuration.

## Generate

```
cmake -S . -B build-sbom -DBUILD_LIB=ON -DENABLE_SBOM=ON <your usual options, toolchain file, ...>
cmake --build build-sbom
```

The result is `build-sbom/sbom/stk.spdx.json` (and `<prefix>/sbom/stk.spdx.json` after `cmake --install`). Python 3 is
required, which STK needs anyway. `ENABLE_SBOM` is OFF by default.

For `VENDOR_STM32=ON` the vendor sources are fetched, not vendored, so fetch them before configuring (the SBOM fails if
a listed path is missing), e.g. `python3 deps/scripts/fetch_deps.py stm32-f4`. The versions listed in the SBOM are the
ones pinned in `deps/scripts/deps.py`.

Typical differences between configurations:

| Configuration | Third-party packages listed |
|---|---|
| generic / host, generic ARM or RISC-V without `VENDOR_*` | none (STK only) |
| `-DVENDOR_STM32=ON` | CMSIS-Core headers, STK Cortex-M glue (`deps/target/stm32`), and the ST CMSIS device files + HAL driver of the selected family (F0 / F1 / F4 / F7) |
| RISC-V (`RISCV`) | CSR encoding header, startup code, STK linker script |
| `-DENABLE_SEGGER_SYSVIEW=ON` (any target) | SEGGER RTT and SystemView (`LicenseRef-SEGGER-SystemView`) |

## Scope

In: the STK library and the third-party code its build configuration brings in.

Out (for now): examples, benchmarks, tests (CppUTest), the interop adapters for other RTOS APIs, middleware ports
(lwIP, mbedTLS, TinyUSB) and the **Raspberry Pi** target. `-DVENDOR_RPI=ON` prints `STK SBOM: skipped` and generates
nothing.

## Maintain

`components.json` is the single source of truth; `gen_sbom.py` (in `build/cmake`) only reads it.

* **New vendored component:** add an entry with its license (SPDX expression), supplier, paths and a `when` condition
  over CMake variables (`all` / `any` / `none`). Paths are checked on every run, so stale metadata fails the build.
* **Custom license** (no SPDX identifier): add a `LicenseRef-...` text under `LICENSES/` (repository root, where REUSE expects it) and register it in `license_refs`.
* **Per-file licenses** are declared in the files themselves (`SPDX-License-Identifier`) or in `REUSE.toml`;
  check with `reuse lint`.
* **Fetched STM32 dependencies** (`deps/stm32/...`): the `version`, `download` and `purl` of the `st-*` entries in
  `components.json` must be kept in sync with the pinned `revision` in `deps/scripts/deps.py` (same names).
* The package version comes from `version.txt`; the git revision is recorded in `sourceInfo` and the download location.
  The creation time is the commit date (or `SOURCE_DATE_EPOCH`), so the output is reproducible.

Validate a generated file with `pip install spdx-tools` and `pyspdxtools -i build-sbom/sbom/stk.spdx.json`.

## Open items

* `deps/target/risc-v/include/risc-v/encoding.h` has no license header; it is listed with `NOASSERTION` until the upstream
  license (riscv-opcodes) is confirmed.
* The Raspberry Pi target (Pico SDK, cyw43 driver with its device-restricted license) still has to be added.
