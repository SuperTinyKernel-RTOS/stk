# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Example.cmake)

stk_add_example(blinky_mpu-hard
    SOURCES        example.cpp
    REQUIRES       ENABLE_MPU ENABLE_MPU_STACK_GUARD
    REQUIRES_BSP   nxpimxrt1050
    EMBEDDED_ONLY)
