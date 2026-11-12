# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Example.cmake)

# Wi-Fi credentials (ENV_WIFI_SSID / ENV_WIFI_PASSWORD), set them with -DEXAMPLE_WIFI_SSID=... -DEXAMPLE_WIFI_PASSWORD=...
set(EXAMPLE_WIFI_SSID     "" CACHE STRING "Wi-Fi SSID for the httpd examples")
set(EXAMPLE_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password for the httpd examples")

# lwIP (fork with LWIP_STK_* options, cloned to deps/lwip or -DPICO_LWIP_PATH=...) is required. The example does not
# ship lwipopts.h: put it into the example folder or point EXAMPLE_LWIP_CONFIG_DIR to the folder which has it
# (the Eclipse project build/example/project/eclipse/rpi/httpd-rp2350w provides one).
set(EXAMPLE_LWIP_CONFIG_DIR "" CACHE PATH "Folder with lwipopts.h for build/example/httpd")

if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/lwipopts.h AND NOT EXAMPLE_LWIP_CONFIG_DIR)
    message(WARNING "example 'httpd' skipped: lwipopts.h not found, set -DEXAMPLE_LWIP_CONFIG_DIR=<folder with lwipopts.h>")
else()
    stk_add_example(httpd
        SOURCES        example.cpp fsdata.c
        INCLUDES       ${EXAMPLE_LWIP_CONFIG_DIR}
        DEFINES        ENV_WIFI_SSID=\"${EXAMPLE_WIFI_SSID}\" ENV_WIFI_PASSWORD=\"${EXAMPLE_WIFI_PASSWORD}\"
        REQUIRES_TARGETS pico_cyw43_arch_lwip_sys_stk pico_lwip_http pico_lwip_mdns
        LINK_LIBS      pico_cyw43_arch_lwip_sys_stk pico_lwip_http pico_lwip_mdns
        REQUIRES_BSP   rp2350w)
endif()
