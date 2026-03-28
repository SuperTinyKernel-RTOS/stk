# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

project(target)

if (VENDOR_STM32)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/stm32fx)
endif()

if (RISCV)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/risc-v)
endif()