# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# Pico SDK backends of STK: pico_async_context, pico_cyw43_arch, pico_lwip. Names follow the SDK convention
# (compare pico_async_context_freertos, pico_cyw43_arch_lwip_sys_freertos, pico_lwip_freertos).
#
#   pico_async_context_stk          async_context driven by an STK task
#   pico_lwip_stk                   lwIP (NO_SYS=0) bound to an async_context_stk, with the STK sys_arch
#   pico_cyw43_arch_sys_stk         CYW43 Wi-Fi/Bluetooth driver architecture on STK (no lwIP)
#   pico_cyw43_arch_lwip_sys_stk    CYW43 + lwIP on STK (the network stack of build/example/httpd*)
#
# lwIP needs the Pico SDK lwIP submodule/path (-DPICO_LWIP_PATH=...), see rp2_common/pico_lwip. Without it only
# the targets which do not need lwIP are defined.

# async_context
pico_add_library(pico_async_context_stk NOFLAG)
target_sources(pico_async_context_stk INTERFACE ${CMAKE_CURRENT_LIST_DIR}/pico_async_context/async_context_stk.cpp)
target_include_directories(pico_async_context_stk_headers SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/pico_async_context/include)
pico_mirrored_target_link_libraries(pico_async_context_stk INTERFACE pico_async_context_base)
target_link_libraries(pico_async_context_stk INTERFACE stk_headers)

# cyw43_arch (the base pico_cyw43_arch exists only for CYW43 boards, e.g. pico2_w)
if (TARGET pico_cyw43_arch)
    target_include_directories(pico_cyw43_arch_headers SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/pico_cyw43_arch/include)

    pico_add_library(pico_cyw43_arch_sys_stk NOFLAG)
    target_sources(pico_cyw43_arch_sys_stk INTERFACE ${CMAKE_CURRENT_LIST_DIR}/pico_cyw43_arch/cyw43_arch_stk.cpp)
    pico_mirrored_target_link_libraries(pico_cyw43_arch_sys_stk INTERFACE
            pico_cyw43_arch
            pico_async_context_stk)
    # pico/cyw43_arch.h includes __XSTRING(PICO_CYW43_ARCH_HEADER), i.e. the value must not be quoted
    target_compile_definitions(pico_cyw43_arch_sys_stk_headers INTERFACE
            PICO_CYW43_ARCH_STK=1
            PICO_CYW43_ARCH_HEADER=pico/cyw43_arch/arch_stk.h)
endif()

# lwIP
if (TARGET pico_lwip)
    pico_add_library(pico_lwip_stk NOFLAG)
    target_sources(pico_lwip_stk INTERFACE ${CMAKE_CURRENT_LIST_DIR}/pico_lwip/lwip_stk.cpp)
    target_include_directories(pico_lwip_stk_headers SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/pico_lwip/include)
    pico_mirrored_target_link_libraries(pico_lwip_stk INTERFACE
            pico_async_context_stk
            pico_lwip_arch
            pico_lwip)
    # STK sys_arch for lwIP (plain INTERFACE library without *_headers, so not part of the mirrored list above)
    target_link_libraries(pico_lwip_stk INTERFACE stk_lwip_port)

    if (TARGET pico_cyw43_arch_sys_stk)
        pico_add_library(pico_cyw43_arch_lwip_sys_stk NOFLAG)
        pico_mirrored_target_link_libraries(pico_cyw43_arch_lwip_sys_stk INTERFACE
                pico_lwip_stk
                pico_cyw43_arch_sys_stk)
        target_compile_definitions(pico_cyw43_arch_lwip_sys_stk_headers INTERFACE
                CYW43_LWIP=1
                LWIP_PROVIDE_ERRNO=1)
    endif()
endif()
