# Building the examples with CMake

Examples are enabled with `BUILD_EXAMPLES` (the library is enabled automatically). Pick examples with
`-DEXAMPLE_<NAME>=ON` (see `build/example/CMakeLists.txt`) or use `-DEXAMPLE_ALL=ON`. Examples which are not supported by the
current configuration are skipped with a `WARNING` that says what is missing.

## Arm Cortex-M (STM32)

```
cmake -S . -B out -G Ninja -DCMAKE_TOOLCHAIN_FILE=build/cmake/toolchain/arm-none-eabi-gcc.cmake \
      -DVENDOR_STM32=ON -DBUILD_EXAMPLES=ON -DEXAMPLE_BLINKY=ON
cmake --build out        # out/build/example/blinky/blinky.elf + blinky.hex
```

The LED driver (BSP) is chosen from the target core (`EXAMPLE_BSP=auto`); override with `-DEXAMPLE_BSP=<stm32f0xx|stm32f1xx|stm32f4xx|nxpk66|nxpimxrt1050|rp2350w|portable>`.

## Examples that need extra options

| Example | Needs |
|---|---|
| `tls`, `blinky_c-pthread` | `-DENABLE_TLS=ON` |
| `blinky_mpu`, `blinky_mpu-hard` | `-DENABLE_MPU=ON -DENABLE_MPU_STACK_GUARD=ON` (MPU linker scripts from `EXAMPLE_MPU_LD_DIR`); `blinky_mpu-hard` also `-DEXAMPLE_BSP=nxpimxrt1050` |
| `low_power`, `low_power_c` | `-DENABLE_TICKLESS=ON`; `low_power` also `-DSTK_SLEEP_TRAP_STACK_SIZE=256` |
| `blinky_c-pthread` | toolchain with newlib built with `_RETARGETABLE_LOCKING` (e.g. Arm GNU Toolchain) |
| `blinky_mc`, `blinky-trustzone-*`, `blinky_mpu_armv8`, `blinky_mpu-trustzone-*`, `httpd*` | Raspberry Pi Pico SDK (`pico_sdk_init()` before this project) |
| `audio_mic`, `audio_mic_c` | TinyUSB targets (`tinyusb_device`, `tinyusb_board`) |

## Adding an example

Create `build/example/<name>/CMakeLists.txt` and call `stk_add_example()` (see `build/cmake/Example.cmake` for all arguments),
then add an `EXAMPLE_<NAME>` option and `add_subdirectory()` to `build/example/CMakeLists.txt`.
