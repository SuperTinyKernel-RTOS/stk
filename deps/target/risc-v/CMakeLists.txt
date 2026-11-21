# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

set(TARGET_NAME riscv)
project(${TARGET_NAME})
message(STATUS "- " ${TARGET_NAME})

# Definitions, includes
set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
include(ProjectInclude.cmake)

# Sources
file(GLOB RISCV_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/init/*.c)

# Target
add_library(${TARGET_NAME} STATIC ${RISCV_SRC})

# MPU (PMP) support: startup code initializes the .stk_mpu_xxx regions of stk-mpu.ld
if (STK_MPU)
    target_compile_definitions(${TARGET_NAME} PRIVATE STK_MPU=1)
endif()

install(TARGETS ${TARGET_NAME} DESTINATION ${LIBRARY_OUTPUT_PATH})