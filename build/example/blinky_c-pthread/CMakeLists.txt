# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Example.cmake)

stk_add_example(blinky_c-pthread
    SOURCES        example.c
    INTEROP        c posix newlib
    C_KERNEL_TASKS 6
    C_KERNEL_TYPE  "Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, sched::SwitchStrategyRR, PlatformDefault>"
    REQUIRES       ENABLE_TLS
    EMBEDDED_ONLY)
