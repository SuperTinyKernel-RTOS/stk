#
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
#
#  Source: https://github.com/SuperTinyKernel-RTOS
#
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#

if (ENABLE_TLS)
    message(STATUS "* enable TLS")
    set(STK_TLS 1)
endif()

if (ENABLE_TICKLESS)
    message(STATUS "* enable TICKLESS")
    set(STK_TICKLESS_IDLE 1)
else()
    set(STK_TICKLESS_IDLE 0)
endif()

if (ENABLE_TLS)
    message(STATUS "* enable TLS")
    set(STK_TLS 1)
else()
    set(STK_TLS 0)
endif()

if (ENABLE_MPU)
    message(STATUS "* enable MPU")
    set(STK_MPU 1)
else()
    set(STK_MPU 0)
endif()

if (ENABLE_MPU_STACK_GUARD)
    message(STATUS "* enable MPU STACK GUARD")
    set(STK_MPU_STACK_GUARD 1)
else()
    set(STK_MPU_STACK_GUARD 0)
endif()

if (ENABLE_STRICT_COMPLIANCY)
    message(STATUS "* enable STRICT COMPLIANCY")
    set(STK_STRICT_COMPLIANCY 1)
else()
    set(STK_STRICT_COMPLIANCY 0)
endif()

if (ENABLE_SEGGER_SYSVIEW)
    message(STATUS "* enable SEGGER SYSVIEW TRACING")
    set(STK_SEGGER_SYSVIEW 1)
else()
    set(STK_SEGGER_SYSVIEW 0)
endif()

# Pass other configured cache variables directly
set(STK_STACK_GUARD ${STK_STACK_GUARD})
set(STK_TICKLESS_TICKS_MAX ${STK_TICKLESS_TICKS_MAX})
set(STK_MPU_TASK_REGIONS ${STK_MPU_TASK_REGIONS})
set(STK_STACK_SIZE_MIN ${STK_STACK_SIZE_MIN})
set(STK_CS_NESTINGS_MAX ${STK_CS_NESTINGS_MAX})
set(STK_TIMER_THREADS_COUNT ${STK_TIMER_THREADS_COUNT})
set(STK_TIMER_COMMAND_QUEUE_SIZE ${STK_TIMER_COMMAND_QUEUE_SIZE})
set(STK_SYNC_DEBUG_NAMES ${STK_SYNC_DEBUG_NAMES})

# Create stk_config.h (note: must be defined after inclusion of the projects to catch the value of _STK_DEVICE_INC)
if (ARM)
    set(_STK_ARCH_ARM_CORTEX_M TRUE)
    message(STATUS "Platform define: _STK_ARCH_ARM_CORTEX_M")
elseif (RISCV)
    set(_STK_ARCH_RISC_V TRUE)
    message(STATUS "Platform define: _STK_ARCH_RISC_V")
else()
    message(STATUS "Platform define: _STK_ARCH_X86_WIN32")    
    if (WIN32)
        set(_STK_ARCH_X86_WIN32 TRUE)
    else()
        message(WARNING "Platform does not have scheduling driver! Using _STK_ARCH_X86_WIN32 as fallback.")
    endif()
endif()
set(STK_CONFIG_INC ${CMAKE_CURRENT_BINARY_DIR}/include/stk_config.h)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/stk_config.h.in ${STK_CONFIG_INC})
message(STATUS "STK config file: " ${STK_CONFIG_INC})
