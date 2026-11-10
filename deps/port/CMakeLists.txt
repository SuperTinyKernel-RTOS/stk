# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# STK ports for 3rd-party middleware (lwIP, mbedTLS, TinyUSB) and the Raspberry Pi Pico SDK.
#
# Every port is an INTERFACE library: its sources are compiled inside the consuming application, which owns the
# configuration of the middleware (lwipopts.h, mbedtls config, tusb_config.h) and provides the include paths of the
# middleware itself. The ports do not fetch or vendor lwIP / mbedTLS / TinyUSB.
#
#   target_link_libraries(my_app stk_lwip_port)      # + your lwIP (include paths, lwipopts.h)
#   target_link_libraries(my_app stk_mbedtls_port)   # + your mbedTLS
#   target_link_libraries(my_app stk_tinyusb_osal)   # + your TinyUSB (CFG_TUSB_OS=OPT_OS_CUSTOM, see the README)

# Headers of the STK needed by the port sources (stk_config.h is generated into the build tree)
if (NOT TARGET stk_headers)
    add_library(stk_headers INTERFACE)
    target_include_directories(stk_headers INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../stk/include
        ${CMAKE_BINARY_DIR}/include)
    # the library is defined after deps/ by the top-level CMakeLists.txt, linking is resolved at generation time
    target_link_libraries(stk_headers INTERFACE stk)
    if (ENABLE_SEGGER_SYSVIEW)
        target_include_directories(stk_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../segger)
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lwip)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/mbedtls)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tinyusb)

# Pico SDK backends (async_context, cyw43_arch, lwip) need the SDK, i.e. -DVENDOR_RPI=ON
if (VENDOR_RPI)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/rpi)
endif()
