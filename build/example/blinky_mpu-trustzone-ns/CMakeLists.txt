# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Example.cmake)

stk_add_example(blinky_mpu-trustzone-ns
    SOURCES        example.cpp
    REQUIRES_HEADER arch/arm/cortex-m/stk_arch_arm-tz.h
    REQUIRES       ENABLE_MPU
    REQUIRES_TARGETS pico_stdlib
    LINK_LIBS      pico_stdlib)
