# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# mbedTLS port: threading (MBEDTLS_THREADING_ALT), time (mbedtls_ms_time), entropy/PSA random.
# Enable MBEDTLS_THREADING_C + MBEDTLS_THREADING_ALT in the mbedTLS config of the application, see README.md.
#
#   -DSTK_MBEDTLS_PSA_NO_STATIC=ON  compile other/psa_crypto_driver_wrappers_no_static.c (works around "missing symbol"
#                                   link errors of three static inline PSA driver wrapper functions on some toolchains,
#                                   needs psa_crypto_driver_wrappers_no_static.h from your PSA crypto tree)

option(STK_MBEDTLS_PSA_NO_STATIC "Compile the non-static PSA driver wrappers of the mbedTLS port" OFF)

add_library(stk_mbedtls_port INTERFACE)
target_sources(stk_mbedtls_port INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/stk_mbedtls_other.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/stk_mbedtls_threading.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/stk_mbedtls_time.cpp)
if (STK_MBEDTLS_PSA_NO_STATIC)
    target_sources(stk_mbedtls_port INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/other/psa_crypto_driver_wrappers_no_static.c)
endif()
target_include_directories(stk_mbedtls_port INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(stk_mbedtls_port INTERFACE stk_headers)
