#
#  SuperTinyKernel RTOS (STK) repository dependency manifest, consumed by fetch_deps.py.
#
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#
"""Dependencies fetched by fetch_deps.py.

Fields:
    name      short unique name (used to select a dependency on the command line)
    url       git repository URL
    dest      destination directory, relative to the directory of fetch_deps.py
    branch    branch to track when no revision is pinned (falls back main <-> master)
    revision  commit hash, tag or branch to pin to, or None to track 'branch'
    depth     optional: shallow-clone depth (use with a tag/branch 'revision'; omit for full history)
    sparse    optional: list of directories to check out (everything else is not downloaded)
    group     optional: group name (or list of names) to fetch several dependencies at once (e.g. "stm32-f4")

STM32 (VENDOR_STM32=ON) builds take the CMSIS-Core headers, the CMSIS device files (start-up code, system file,
device headers) and the HAL driver straight from STMicroelectronics. Fetch what you need with, for example:
    python3 deps/scripts/fetch_deps.py stm32-f4        # F0: stm32-f0, F1: stm32-f1, F7: stm32-f7, everything: stm32
The versions pinned below are the ones the STK test-suite is validated with.

Values can be overridden for a single run with fetch_deps.py --revision NAME=REV
and --branch NAME=BRANCH; edit this file for permanent changes.
"""

DEPS = [
    {
        "name": "mbedtls",
        "url": "https://github.com/Mbed-TLS/mbedtls.git",
        "dest": "../mbedtls",
        "branch": "main",
        "revision": "mbedtls-4.2.0_mergeback",
    },
    {
        "name": "segger-rtt",
        "url": "https://github.com/SEGGERMicro/RTT.git",
        "dest": "../segger/rtt",
        "branch": "main",
        "revision": None,
    },
    {
        "name": "segger-systemview",
        "url": "https://github.com/SEGGERMicro/SystemView.git",
        "dest": "../segger/systemview",
        "branch": "main",
        "revision": None,
    },
    {
        "name": "tinyusb",
        "url": "https://github.com/hathach/tinyusb.git",
        "dest": "../tinyusb",
        "branch": "master",
        "revision": None,
    },
    {
        "name": "infineon-core-lib",
        "url": "https://github.com/Infineon/core-lib.git",
        "dest": "../infineon/core-lib",
        "branch": "master",
        "revision": None,
    },
    {
        "name": "lwip",
        "url": "https://github.com/SuperTinyKernel-RTOS/lwip.git",
        "dest": "../lwip",
        "branch": "stk",
        "revision": None,
    },

    # --- STMicroelectronics (STM32, VENDOR_STM32=ON) -------------------------------------------------------------
    # CMSIS-Core headers (Arm, Apache-2.0) as published by ST, shared by all families.
    {
        "name": "st-cmsis-core",
        "url": "https://github.com/STMicroelectronics/cmsis_core.git",
        "dest": "../stm32/cmsis-core",
        "branch": "master",
        "revision": "v5.9.0",
        "depth": 1,
        "sparse": ["Include"],
        "group": ["stm32", "stm32-f0", "stm32-f1", "stm32-f4", "stm32-f7"],
    },
    # STM32F0 (Cortex-M0)
    {
        "name": "st-cmsis-device-f0",
        "url": "https://github.com/STMicroelectronics/cmsis_device_f0.git",
        "dest": "../stm32/f0/cmsis-device",
        "branch": "master",
        "revision": "v2.3.4",
        "depth": 1,
        "group": ["stm32", "stm32-f0"],
    },
    {
        "name": "st-hal-f0",
        "url": "https://github.com/STMicroelectronics/stm32f0xx_hal_driver.git",
        "dest": "../stm32/f0/hal",
        "branch": "master",
        "revision": "v1.7.4",
        "depth": 1,
        "group": ["stm32", "stm32-f0"],
    },
    # STM32F1 (Cortex-M3)
    {
        "name": "st-cmsis-device-f1",
        "url": "https://github.com/STMicroelectronics/cmsis_device_f1.git",
        "dest": "../stm32/f1/cmsis-device",
        "branch": "master",
        "revision": "v4.3.2",
        "depth": 1,
        "group": ["stm32", "stm32-f1"],
    },
    {
        "name": "st-hal-f1",
        "url": "https://github.com/STMicroelectronics/stm32f1xx_hal_driver.git",
        "dest": "../stm32/f1/hal",
        "branch": "master",
        "revision": "v1.1.7",
        "depth": 1,
        "group": ["stm32", "stm32-f1"],
    },
    # STM32F4 (Cortex-M4)
    {
        "name": "st-cmsis-device-f4",
        "url": "https://github.com/STMicroelectronics/cmsis_device_f4.git",
        "dest": "../stm32/f4/cmsis-device",
        "branch": "master",
        "revision": "v2.6.6",
        "depth": 1,
        "group": ["stm32", "stm32-f4"],
    },
    {
        "name": "st-hal-f4",
        "url": "https://github.com/STMicroelectronics/stm32f4xx_hal_driver.git",
        "dest": "../stm32/f4/hal",
        "branch": "master",
        "revision": "v1.7.10",
        "depth": 1,
        "group": ["stm32", "stm32-f4"],
    },
    # STM32F7 (Cortex-M7)
    {
        "name": "st-cmsis-device-f7",
        "url": "https://github.com/STMicroelectronics/cmsis_device_f7.git",
        "dest": "../stm32/f7/cmsis-device",
        "branch": "master",
        "revision": "v1.2.10",
        "depth": 1,
        "group": ["stm32", "stm32-f7"],
    },
    {
        "name": "st-hal-f7",
        "url": "https://github.com/STMicroelectronics/stm32f7xx_hal_driver.git",
        "dest": "../stm32/f7/hal",
        "branch": "master",
        "revision": "v1.3.3",
        "depth": 1,
        "group": ["stm32", "stm32-f7"],
    },
]
