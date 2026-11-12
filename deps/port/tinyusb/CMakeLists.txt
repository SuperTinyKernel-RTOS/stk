# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# TinyUSB OSAL port. Set CFG_TUSB_OS=OPT_OS_CUSTOM in tusb_config.h (see README.md), the default backend is header-only.
#
#   -DSTK_TINYUSB_OSAL_USE_CPP=ON   use the C++ API of STK (compiles osal_stk.cpp, defines CFG_TUH_OSAL_STK_USE_CPP=1)

option(STK_TINYUSB_OSAL_USE_CPP "TinyUSB OSAL port: bind to the C++ API of STK instead of stk_c.h" OFF)

add_library(stk_tinyusb_osal INTERFACE)
# tusb_os_custom.h / osal_stk.h live in src/osal; stk_c.h (default backend) comes with the C interface of STK
target_include_directories(stk_tinyusb_osal INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/osal
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../interop/c/include)
target_link_libraries(stk_tinyusb_osal INTERFACE stk_headers)
if (STK_TINYUSB_OSAL_USE_CPP)
    target_sources(stk_tinyusb_osal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/osal/osal_stk.cpp)
    target_compile_definitions(stk_tinyusb_osal INTERFACE CFG_TUH_OSAL_STK_USE_CPP=1)
endif()
