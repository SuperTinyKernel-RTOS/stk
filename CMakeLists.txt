# 
#  SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

cmake_minimum_required(VERSION 3.14)

# Raspberry Pi RP2040/RP2350: the Pico SDK (vendored in deps/target/rpi) must be set up before project() as it selects
# the toolchain, see deps/target/rpi/ProjectInclude.cmake for the options (PICO_PLATFORM, PICO_BOARD, ...)
option(VENDOR_RPI                     "Target Raspberry Pi RP2040/RP2350 devices (Pico SDK)" OFF)
if (VENDOR_RPI)
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico SDK platform: rp2350-arm-s, rp2040, rp2350-riscv")
    set(PICO_BOARD    pico2_w      CACHE STRING "Pico SDK board")
    # picotool (UF2 generation for RP2350) is fetched and built by the SDK, which is not always wanted: ELF/BIN/HEX are produced anyway
    if (NOT DEFINED PICO_NO_PICOTOOL)
        set(PICO_NO_PICOTOOL 1 CACHE BOOL "Do not use picotool (set to 0 to produce UF2 files)")
    endif()
    # lwIP and mbedTLS are not vendored: use deps/lwip and deps/mbedtls when cloned there (see build/example/httpd*)
    if (NOT PICO_LWIP_PATH AND NOT DEFINED ENV{PICO_LWIP_PATH} AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/deps/lwip/src)
        set(PICO_LWIP_PATH ${CMAKE_CURRENT_LIST_DIR}/deps/lwip CACHE PATH "Path to lwIP")
    endif()
    if (NOT PICO_MBEDTLS_PATH AND NOT DEFINED ENV{PICO_MBEDTLS_PATH} AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/deps/mbedtls/library)
        set(PICO_MBEDTLS_PATH ${CMAKE_CURRENT_LIST_DIR}/deps/mbedtls CACHE PATH "Path to Mbed TLS")
    endif()
    include(${CMAKE_CURRENT_LIST_DIR}/deps/target/rpi/pico_sdk_init.cmake)
endif()

project(global)

if (VENDOR_RPI)
    enable_language(ASM)
    pico_sdk_init()
    include(${CMAKE_CURRENT_LIST_DIR}/deps/target/rpi/ProjectInclude.cmake)
endif()

# ==============================================================================
# Vendor & Target Options
# ==============================================================================
option(VENDOR_STM32                   "Target STM32 devices"                  OFF)

# ==============================================================================
# Core Build Options
# ==============================================================================
option(BUILD_LIB                      "Build Library"                         OFF)
option(BUILD_C_IFACE                  "Build C interface"                     OFF)
option(BUILD_EXAMPLES                 "Build Examples"                        OFF)
option(BUILD_TESTS                    "Build Tests"                           OFF)
option(BUILD_DOC                      "Build Documentation"                   OFF)

# ==============================================================================
# Feature Toggles (Mapped to stk_config.h.in)
# ==============================================================================
option(ENABLE_TICKLESS                "Enable tickless idle mode"             OFF)
option(ENABLE_TLS                     "Enable thread-local storage (TLS)"     OFF)
option(ENABLE_MPU                     "Enable hardware MPU support"           OFF)
option(ENABLE_MPU_STACK_GUARD         "Enable MPU stack guard per switch"     OFF)
option(ENABLE_STRICT_COMPLIANCY       "Disallow performance workarounds"      OFF)
option(ENABLE_SEGGER_SYSVIEW          "Enable SEGGER SystemView tracing"      OFF)

# ==============================================================================
# General
# ==============================================================================
set(STK_STACK_GUARD                   ""   CACHE STRING "Check stack health on context switch, empty = default (see stk_defs.h for details)")
set(STK_SYNC_DEBUG_NAMES              ""   CACHE STRING "Enable debug names for sync objects, empty = default (see stk_defs.h for details)")

# ==============================================================================
# Tickless & Low-Power Tuning Parameters
# ==============================================================================
set(STK_TICKLESS_TICKS_MAX            ""   CACHE STRING "Maximum suppressed ticks in tickless idle, empty = default (see stk_defs.h for details)")
set(STK_SLEEP_TRAP_STACK_SIZE         ""   CACHE STRING "Stack size (words) of the sleep trap, empty = default (see stk_defs.h for details)")

# ==============================================================================
# 5. MPU & Memory Protection Settings
# ==============================================================================
set(STK_MPU_TASK_REGIONS              ""   CACHE STRING "Number of MPU regions per task (2, 4, 6, 8, 12, 16), empty = default")

# ==============================================================================
# 6. Advanced Tuning & Stack Parameters
# ==============================================================================
set(STK_STACK_SIZE_MIN                ""   CACHE STRING "Minimum stack size in Word elements, empty = default (see stk_defs.h for details)")
set(STK_CS_NESTINGS_MAX               ""   CACHE STRING "Maximum critical section nesting depth, empty = default (see stk_defs.h for details)")
set(STK_TIMER_THREADS_COUNT           ""   CACHE STRING "Number of threads handling timers in TimerHost, empty = default (see stk_defs.h for details)")
set(STK_TIMER_COMMAND_QUEUE_SIZE      ""   CACHE STRING "Capacity of TimerHost command queue, empty = default (see stk_defs.h for details)")

# ==============================================================================
# ==============================================================================

# Python3 is required
find_package(Python3 COMPONENTS Interpreter)
if (NOT Python3_FOUND)
    message(FATAL_ERROR "stk: Python3 not found, install Python3 and continue")
endif()

# Examples are linked with the library
if (BUILD_EXAMPLES AND NOT BUILD_LIB AND (WIN32 OR VENDOR_STM32 OR VENDOR_RPI OR RISCV))
    message(STATUS "BUILD_EXAMPLES requires BUILD_LIB, enabling BUILD_LIB")
    set(BUILD_LIB ON CACHE BOOL "Build Library" FORCE)
endif()

# Include CTest for running tests
if (BUILD_TESTS)
    include(CTest)

	# Coverage report
    option(ENABLE_COVERAGE "enable Coverage report" OFF)
    if (ENABLE_COVERAGE)
        include(build/cmake/Coverage.cmake)
    endif()
endif()

# Compiler related
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CLANGC TRUE)
    set(GCC TRUE)
    set(CMAKE_COMPILER_IS_GNUCC TRUE)
    set(CMAKE_COMPILER_IS_GNUCXX TRUE)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(GCC TRUE)
    set(CMAKE_COMPILER_IS_GNUCC TRUE)
    set(CMAKE_COMPILER_IS_GNUCXX TRUE)
endif()
message(STATUS "System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
# Enforce C11 standard
if (GCC AND NOT VENDOR_RPI)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
elseif (VENDOR_RPI)
	# Pico SDK sources need GNU dialects (e.g. "asm" keyword), let CMake choose gnu11 / gnu++11
	set(CMAKE_C_STANDARD 11)
	set(CMAKE_CXX_STANDARD 11)
endif()
if (MSVC)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /std:c20")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++20")
    
    # Definitions
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)

    # Dependencies
	link_libraries("Winmm") # for CppUTest
endif()

# Output binaries path
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/lib)

# Configure file folder (note: must be defined before inclusion of the projects so that they could use it)
set(_STK_DEVICE_INC)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)

# Add projects
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/deps)
if (BUILD_LIB)
	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/stk)
endif()
if (BUILD_EXAMPLES)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/build/example)
endif()
if (BUILD_TESTS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/test)
endif()
if (BUILD_DOC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/build/doc)
endif()

# Create stk_config.h
include (build/cmake/ConfigureConfig.cmake)

# Log CPU family
message(STATUS "Target CPU family: ${TARGET_CPU_FAMILY}")