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
    TARGET_CORTEX_M0 TARGET_CORTEX_M3 TARGET_CORTEX_M4 TARGET_RV32I TARGET_RV32E TARGET_CPU_FAMILY
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
        message(FATAL_ERROR "stk: ENABLE_SBOM requires Python3")
    endif()

    set(sbom_dir    ${CMAKE_BINARY_DIR}/sbom)
    set(sbom_file   ${sbom_dir}/stk.spdx.json)
    set(sbom_target ${TARGET}_sbom)

    # Configuration, as --define NAME=VALUE (only variables which are defined)
    set(sbom_args --output ${sbom_file} --root ${ARG_ROOT})
    foreach(var IN LISTS _STK_SBOM_VARIABLES)
        if (DEFINED ${var})
            list(APPEND sbom_args --define "${var}=${${var}}")
        endif()
    endforeach()

    # Generate once at configure time so the file exists right after configuration.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${_STK_GEN_SBOM_SCRIPT} ${sbom_args}
        RESULT_VARIABLE sbom_result
        OUTPUT_VARIABLE sbom_output
        ERROR_VARIABLE  sbom_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if (NOT sbom_result EQUAL 0)
        message(FATAL_ERROR "stk: SBOM generation failed: ${sbom_error}")
    endif()
    if (sbom_output MATCHES "^SKIPPED: (.*)")
        message(STATUS "STK SBOM: skipped, ${CMAKE_MATCH_1}")
        return()
    endif()

    # Refresh on every build (the script rewrites the file only if its content changed).
    add_custom_target(${sbom_target} ALL
        COMMAND ${Python3_EXECUTABLE} ${_STK_GEN_SBOM_SCRIPT} ${sbom_args}
        BYPRODUCTS ${sbom_file}
        COMMENT "stk: updating SPDX SBOM"
        VERBATIM)

    install(FILES ${sbom_file} DESTINATION sbom OPTIONAL)

    message(STATUS "STK SBOM: ${sbom_file}")
endfunction()
