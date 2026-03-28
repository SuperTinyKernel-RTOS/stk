# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

project(deps)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/target)

# Note: exclude cpputest from small builds as they are missing exceptions and RTTI
if (BUILD_TESTS AND NOT ENABLE_SMALL)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cpputest)
endif()
