# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

set(TARGET_NAME doc)
project(${TARGET_NAME})
message(STATUS "- " ${TARGET_NAME})

find_package(Doxygen)

if (DOXYGEN_FOUND)
    set(DOXYGEN_CONFIG_IN ${CMAKE_CURRENT_SOURCE_DIR}/../../Doxyfile)
    set(DOXYGEN_CONFIG ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    
    set(DOXYGEN_INPUT 
        ${CMAKE_CURRENT_SOURCE_DIR}/../../stk 
        ${CMAKE_CURRENT_SOURCE_DIR}/../../interop 
        ${CMAKE_CURRENT_SOURCE_DIR}/../../test
        ${CMAKE_CURRENT_SOURCE_DIR}/../../README.md
    )
    configure_file(${DOXYGEN_CONFIG_IN} ${DOXYGEN_CONFIG} @ONLY)
    message(STATUS "Doxygen config file: ${DOXYGEN_CONFIG}")

    add_custom_target(${TARGET_NAME} ALL
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_CONFIG}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating Documentation in: ${CMAKE_CURRENT_BINARY_DIR}"
        VERBATIM
    )
endif()