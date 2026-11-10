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

# lwIP (fork with LWIP_STK_* options, cloned to deps/lwip or -DPICO_LWIP_PATH=...) and mbedTLS are required,
# lwipopts.h / mbedtls_config.h / crypto_config.h come with the example
stk_add_example(httpd_tls
    SOURCES        example.cpp fsdata.c
    DEFINES        ENV_WIFI_SSID=\"${EXAMPLE_WIFI_SSID}\" ENV_WIFI_PASSWORD=\"${EXAMPLE_WIFI_PASSWORD}\"
    REQUIRES_TARGETS pico_cyw43_arch_lwip_sys_stk pico_lwip_http pico_lwip_mdns pico_lwip_mbedtls pico_mbedtls
    LINK_LIBS      pico_cyw43_arch_lwip_sys_stk pico_lwip_http pico_lwip_mdns pico_lwip_mbedtls pico_mbedtls stk_mbedtls_port
    REQUIRES_BSP   rp2350w)
