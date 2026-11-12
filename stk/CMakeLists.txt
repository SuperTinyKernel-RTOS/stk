# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

set(TARGET_NAME stk)
project(${TARGET_NAME})
message(STATUS "- " ${TARGET_NAME})

# Includes
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Add STM32 specific includes
if (VENDOR_STM32)
    set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../deps/target/stm32fx/ProjectInclude.cmake)
elseif (VENDOR_RPI)
    # Pico SDK is initialised by the top-level CMakeLists.txt (see deps/target/rpi/ProjectInclude.cmake)
elseif (RISCV)
    set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../deps/target/risc-v/ProjectInclude.cmake)
endif()

# SEGGER SystemView headers (stk.h uses them when STK_SEGGER_SYSVIEW=1)
if (ENABLE_SEGGER_SYSVIEW)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../deps/segger)
endif()

# C++ sources
file(GLOB STK_LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if (ARM)
    file(GLOB STK_LIB_PLATFORM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch/arm/cortex-m/*.cpp)
elseif (RISCV)
    file(GLOB STK_LIB_PLATFORM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch/risc-v/*.cpp)
else()
    # Workarounf for Coverage and Generic test: we shall give at least one source file to compile stk lib
    file(GLOB STK_LIB_PLATFORM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/arch/x86/win32/*.cpp)
endif()

# C interface
if (BUILD_C_IFACE)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../interop/c/include)
    file(GLOB STK_LIB_SRC_C_IFACE ${CMAKE_CURRENT_SOURCE_DIR}/../interop/c/src/*.cpp)
endif()

set(STK_LIB_SRC ${STK_LIB_SRC} ${STK_LIB_PLATFORM_SRC} ${STK_LIB_SRC_C_IFACE})

# Target
add_library(${TARGET_NAME} STATIC ${STK_LIB_SRC})

# Device header (_STK_DEVICE_INC) comes from the CMSIS stub of the Pico SDK
if (VENDOR_RPI AND ARM)
    target_link_libraries(${TARGET_NAME} PUBLIC cmsis_core_headers)
endif()

# Generate stk_revision.h (if enabled)
include(${CMAKE_CURRENT_SOURCE_DIR}/../build/cmake/GenerateRevision.cmake)
stk_generate_revision(${TARGET_NAME} ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Tests
if (BUILD_TESTS)
    # Coverage report
    if (ENABLE_COVERAGE AND GCC AND NOT CMAKE_CROSSCOMPILING)
        target_add_coverage_compiler_options(${TARGET_NAME})
    endif()
endif()

install(TARGETS ${TARGET_NAME} DESTINATION ${LIBRARY_OUTPUT_PATH})
