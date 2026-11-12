# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# Example driver (LED BSP) as a stand-alone static library. The examples compile the driver sources directly
# (see build/cmake/Example.cmake), this target is useful to verify that the selected BSP (EXAMPLE_BSP) builds.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Example.cmake)

if (NOT VENDOR_STM32 AND NOT VENDOR_RPI AND NOT RISCV AND NOT WIN32)
    message(WARNING "example 'driver' skipped: no supported target (see build/cmake/Example.cmake)")
else()
    set(TARGET_NAME example_driver)
    message(STATUS "- " ${TARGET_NAME})

    set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

    include_directories(${ROOT_DIR}/stk/include ${CMAKE_CURRENT_SOURCE_DIR})
    if (VENDOR_STM32)
        include(${ROOT_DIR}/deps/target/stm32fx/ProjectInclude.cmake)
    elseif (RISCV AND NOT VENDOR_RPI)
        include(${ROOT_DIR}/deps/target/risc-v/ProjectInclude.cmake)
    endif()

    set(DRIVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/c_wrapper.cpp ${CMAKE_CURRENT_SOURCE_DIR}/${_STK_EXAMPLE_BSP}/led.cpp)
    if (_STK_EXAMPLE_BSP STREQUAL "rp2350w")
        list(APPEND DRIVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/rp2350w/cpu.cpp)
    endif()

    add_library(${TARGET_NAME} STATIC ${DRIVER_SRC})
    if (VENDOR_RPI)
        # Pico SDK headers (pico/cyw43_arch.h, pico/multicore.h) of the rp2350w driver
        foreach (_DRV_LIB pico_stdlib pico_cyw43_arch_none pico_multicore)
            if (TARGET ${_DRV_LIB})
                target_link_libraries(${TARGET_NAME} ${_DRV_LIB})
            endif()
        endforeach()
        target_link_libraries(${TARGET_NAME} stk_headers)
    endif()
endif()
