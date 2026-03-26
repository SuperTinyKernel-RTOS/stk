# 
#  SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

cmake_minimum_required(VERSION 3.14)

project(global)

# Vendor specific targeting
option(VENDOR_STM32   "Target STM32 devices" OFF)

# Add static lib option
option(BUILD_LIB      "build Library"        OFF)
option(BUILD_C_IFACE  "build C interface"    OFF)
#option(BUILD_EXAMPLES "build Examples"       OFF)
option(BUILD_TESTS    "build Tests"          OFF)
option(BUILD_DOC      "build Documentation"  OFF)

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
if (GCC)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
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
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/build/example/cmake)
endif()
if (BUILD_TESTS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/test)
endif()
if (BUILD_DOC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/build/doc)
endif()

# Declare tickles mode if enabled (see stk_config.h.in)
if (MODE_TICKLESS)
    message(STATUS "* tickless mode")
    set(STK_TICKLESS_IDLE 1)
endif()

# Create stk_config.h (note: must be defined after inclusion of the projects to catch value of _STK_DEVICE_INC)
if (WIN32)
    set(_STK_ARCH_X86_WIN32 TRUE)
    message(STATUS "Platform define: _STK_ARCH_X86_WIN32")
elseif (ARM)
    set(_STK_ARCH_ARM_CORTEX_M TRUE)
    message(STATUS "Platform define: _STK_ARCH_ARM_CORTEX_M")
elseif (RISCV)
    set(_STK_ARCH_RISC_V TRUE)
    message(STATUS "Platform define: _STK_ARCH_RISC_V")
else()
    message(WARNING "Platform does not have scheduling driver!")
endif()
set(STK_CONFIG_INC ${CMAKE_CURRENT_BINARY_DIR}/include/stk_config.h)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/stk_config.h.in ${STK_CONFIG_INC})
message(STATUS "STK config file: " ${STK_CONFIG_INC})

# Log CPU family
message(STATUS "Target CPU family: ${TARGET_CPU_FAMILY}")