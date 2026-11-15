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
]
