# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# SEGGER RTT + SystemView (3rd party, see the license in the sources). Built only with -DENABLE_SEGGER_SYSVIEW=ON,
# which also sets STK_SEGGER_SYSVIEW=1 in stk_config.h. Executables created with build/cmake/MakeExecutable.cmake link
# the 'segger' library automatically.
#
# Options:
#   -DSEGGER_RTT_SYSCALLS=ON      compile SEGGER_RTT_Syscalls_GCC.c, which retargets the newlib _write()/_read() to RTT
#                                 (conflicts with other stdio backends, e.g. the UART/USB stdio of the Pico SDK)
#   -DSEGGER_CONFIG_SOURCES=<..>  extra sources of the application, e.g. a custom SEGGER_SYSVIEW_Config*.c or
#                                 SEGGER_SYSVIEW_X_*() hooks when SEGGER_SYSVIEW_Conf.h is set up to call them
#   -DSEGGER_CONFIG_INCLUDE_DIR=<..> folder with own SEGGER_RTT_Conf.h / SEGGER_SYSVIEW_Conf.h which must take
#                                 precedence over the ones of this folder

set(TARGET_NAME segger)
project(${TARGET_NAME} C)
message(STATUS "- " ${TARGET_NAME})

option(SEGGER_RTT_SYSCALLS "Retarget newlib stdio to SEGGER RTT" OFF)
set(SEGGER_CONFIG_SOURCES     "" CACHE STRING "Additional application sources compiled into the segger library")
set(SEGGER_CONFIG_INCLUDE_DIR "" CACHE PATH   "Folder with application-specific SEGGER_*_Conf.h files (searched first)")

set(SEGGER_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/SEGGER_RTT.c
    ${CMAKE_CURRENT_SOURCE_DIR}/SEGGER_RTT_printf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/SEGGER_SYSVIEW.c)
if (SEGGER_RTT_SYSCALLS)
    list(APPEND SEGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/SEGGER_RTT_Syscalls_GCC.c)
endif()
# Hand-written RTT write routine: SystemView selects it automatically on Cortex-M3 and newer with GCC (see
# SEGGER_RTT.h, _CORE_HAS_RTT_ASM_SUPPORT), the file is empty where it is not applicable (e.g. Cortex-M0)
if (ARM)
    # .S needs the preprocessor and the CPU flags of the C compiler driver (not the bare assembler)
    if (NOT VENDOR_RPI)
        set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
        set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS}")
    endif()
    enable_language(ASM)
    list(APPEND SEGGER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/SEGGER_RTT_ASM_ARMv7M.S)
endif()
list(APPEND SEGGER_SRC ${SEGGER_CONFIG_SOURCES})

add_library(${TARGET_NAME} STATIC ${SEGGER_SRC})

# Public, so that everything which includes stk.h (STK_SEGGER_SYSVIEW=1) sees SEGGER_SYSVIEW.h
if (SEGGER_CONFIG_INCLUDE_DIR)
    target_include_directories(${TARGET_NAME} BEFORE PUBLIC ${SEGGER_CONFIG_INCLUDE_DIR})
endif()
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS ${TARGET_NAME} DESTINATION ${LIBRARY_OUTPUT_PATH})
