#
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
#
#  Source: https://github.com/SuperTinyKernel-RTOS
#
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#

# Generates an SPDX 2.3 (JSON) SBOM of the current build configuration of the STK library.
#
# Usage (call after the target and after the target specific ProjectInclude.cmake were processed, so that variables
# such as TARGET_CPU_FAMILY are final):
#
#   include(<path>/build/cmake/GenerateSbom.cmake)
#   stk_generate_sbom(<target> ROOT <STK repository root>)
#
# Enable with -DENABLE_SBOM=ON (requires -DBUILD_LIB=ON). The result is <build>/sbom/stk.spdx.json and it is installed
# to <prefix>/sbom. What is listed depends on the configuration (VENDOR_STM32, TARGET_CORTEX_M*, RISCV,
# ENABLE_SEGGER_SYSVIEW, ...), see build/sbom/README.md and build/sbom/components.json.
#
# Configurations which are not covered yet (e.g. VENDOR_RPI) are skipped with a status message, they do not fail.
# Errors in the SBOM metadata (build/sbom/components.json) DO fail the configuration: a wrong SBOM is worse than none.

set(_STK_GEN_SBOM_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/gen_sbom.py)

# CMake variables which describe the configuration, matched by the "when" conditions of build/sbom/components.json
set(_STK_SBOM_VARIABLES
    VENDOR_STM32 VENDOR_RPI ARM RISCV WIN32
    TARGET_CORTEX_M0 TARGET_CORTEX_M3 TARGET_CORTEX_M4 TARGET_CORTEX_M7 TARGET_RV32I TARGET_RV32E TARGET_CPU_FAMILY
    BUILD_C_IFACE ENABLE_SEGGER_SYSVIEW ENABLE_TICKLESS ENABLE_TLS ENABLE_MPU ENABLE_MPU_STACK_GUARD
    ENABLE_STRICT_COMPLIANCY)

function(stk_generate_sbom TARGET)
    cmake_parse_arguments(ARG "" "ROOT" "" ${ARGN})

    if (NOT ENABLE_SBOM)
        return()
    endif()
    if (NOT TARGET ${TARGET})
        message(FATAL_ERROR "stk_generate_sbom: target '${TARGET}' does not exist (call after add_library)")
    endif()
    if (NOT ARG_ROOT)
        message(FATAL_ERROR "stk_generate_sbom: ROOT <STK repository root> is required")
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    if (NOT Python3_FOUND)
        message(FATAL_ERROR "SBOM: ENABLE_SBOM requires Python3")
    endif()

    set(_sbom_dir    ${CMAKE_BINARY_DIR}/sbom)
    set(_sbom_file   ${_sbom_dir}/stk.spdx.json)
    set(_sbom_target ${TARGET}_sbom)

    # Configuration, as --define NAME=VALUE (only variables which are defined)
    set(_sbom_args --output ${_sbom_file} --root ${ARG_ROOT})
    foreach(var IN LISTS _STK_SBOM_VARIABLES)
        if (DEFINED ${var})
            list(APPEND _sbom_args --define "${var}=${${var}}")
        endif()
    endforeach()

    # Generate once at configure time so the file exists right after configuration.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${_STK_GEN_SBOM_SCRIPT} ${_sbom_args}
        RESULT_VARIABLE _sbom_result
        OUTPUT_VARIABLE _sbom_output
        ERROR_VARIABLE  _sbom_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if (NOT _sbom_result EQUAL 0)
        message(FATAL_ERROR "SBOM: generation failed: ${_sbom_error}")
    endif()
    if (_sbom_output MATCHES "^SKIPPED: (.*)")
        message(STATUS "SBOM: skipped, ${CMAKE_MATCH_1}")
        return()
    endif()

    # Refresh on every build (the script rewrites the file only if its content changed).
    add_custom_target(${_sbom_target} ALL
        COMMAND ${Python3_EXECUTABLE} ${_STK_GEN_SBOM_SCRIPT} ${_sbom_args}
        BYPRODUCTS ${_sbom_file}
        COMMENT "SBOM: updating SPDX SBOM"
        VERBATIM)

    install(FILES ${_sbom_file} DESTINATION ${LIBRARY_OUTPUT_PATH}/sbom OPTIONAL)

    message(STATUS "SBOM: ${_sbom_file}")
endfunction()
