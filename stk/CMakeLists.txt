# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

cmake_minimum_required(VERSION 3.14)

set(TARGET_NAME stk)
project(${TARGET_NAME})
message(STATUS "- " ${TARGET_NAME})

# Includes
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# STM32 specific includes
if (VENDOR_STM32)
    set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../deps/target/stm32/ProjectInclude.cmake)
# Raspberry Pico SDK
elseif (VENDOR_RPI)
    if (RISCV)
        set(PICO_PLATFORM rp2350-riscv CACHE STRING "Pico SDK platform: rp2350-arm-s, rp2040, rp2350-riscv")
    else()
        set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico SDK platform: rp2350-arm-s, rp2040, rp2350-riscv")
    endif()
    set(PICO_BOARD    pico2_w      CACHE STRING "Pico SDK board")
    # picotool (UF2 generation for RP2350) is fetched and built by the SDK, which is not always wanted: ELF/BIN/HEX are produced anyway
    if (NOT DEFINED PICO_NO_PICOTOOL)
        set(PICO_NO_PICOTOOL 1 CACHE BOOL "Do not use picotool (set to 0 to produce UF2 files)")
    endif()
    # lwIP and mbedTLS are not vendored: use deps/lwip and deps/mbedtls when cloned there (see build/example/httpd*)
    if (NOT PICO_LWIP_PATH AND NOT DEFINED ENV{PICO_LWIP_PATH} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/lwip/src)
        set(PICO_LWIP_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../deps/lwip CACHE PATH "Path to lwIP")
    endif()
    if (NOT PICO_MBEDTLS_PATH AND NOT DEFINED ENV{PICO_MBEDTLS_PATH} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../deps/mbedtls/library)
        set(PICO_MBEDTLS_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../deps/mbedtls CACHE PATH "Path to Mbed TLS")
    endif()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../deps//target/rpi/pico_sdk_init.cmake)
    enable_language(ASM)
    pico_sdk_init()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../deps/target/rpi/ProjectInclude.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../deps/port/rpi/CMakeLists.txt)
# RISC-V minimal includes
elseif (RISCV)
    set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../deps/target/risc-v/ProjectInclude.cmake)
endif()

# SEGGER SystemView headers (stk.h uses them when STK_SEGGER_SYSVIEW=1)
if (ENABLE_SEGGER_SYSVIEW)
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps/segger
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps/segger/systemview/SEGGER
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps/segger/systemview/Config
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps/segger/systemview/SYSVIEW
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps/segger/rtt/RTT
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps/segger/rtt/Config
    )
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
if (ENABLE_REVISION)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../build/cmake/GenerateRevision.cmake)
    stk_generate_revision(${TARGET_NAME} ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()

# Generate the SPDX SBOM of this build configuration (if enabled with -DENABLE_SBOM=ON)
if (ENABLE_SBOM)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../build/cmake/GenerateSbom.cmake)
    stk_generate_sbom(${TARGET_NAME} ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()

# Tests
if (BUILD_TESTS)
    # Coverage report
    if (ENABLE_COVERAGE AND GCC AND NOT CMAKE_CROSSCOMPILING)
        target_add_coverage_compiler_options(${TARGET_NAME})
    endif()
endif()

install(TARGETS ${TARGET_NAME} DESTINATION ${LIBRARY_OUTPUT_PATH})
