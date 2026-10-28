#
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
#
#  Source: https://github.com/SuperTinyKernel-RTOS
#
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#

# Generates stk_revision.h (git commit count + short hash) consumed by stk_version.h.
#
# Usage (call right after add_library()):
#
#   include(<path>/build/cmake/GenerateRevision.cmake)
#   stk_generate_revision(<target> ROOT <git repository root>)
#
# The generated header folder and the STK_REVISION_HEADER define are attached to <target> as
# PUBLIC, so everything that links <target> sees the same revision values (stk::GetVersion()
# is constexpr in the header, all translation units must agree).
#
# A missing git, Python or history is not an error: the revision falls back to 0 / "unknown".

option(ENABLE_REVISION "Embed git revision in stk_version.h" ON)

set(_STK_GEN_REVISION_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/gen_revision.py)

function(stk_generate_revision TARGET)
    cmake_parse_arguments(ARG "" "ROOT" "" ${ARGN})

    if (NOT TARGET ${TARGET})
        message(FATAL_ERROR "stk_generate_revision: target '${TARGET}' does not exist (call after add_library)")
    endif()
    if (NOT ARG_ROOT)
        message(FATAL_ERROR "stk_generate_revision: ROOT <git repository root> is required")
    endif()
    if (NOT ENABLE_REVISION)
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    if (NOT Python3_FOUND)
        message(WARNING "stk: Python3 not found, git revision will not be embedded")
        return()
    endif()

    set(rev_dir    ${CMAKE_CURRENT_BINARY_DIR}/../include)
    set(rev_header ${rev_dir}/stk_revision.h)
    set(rev_target ${TARGET}_revision)

    # Generate once at configure time so the file exists before the first compile.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${_STK_GEN_REVISION_SCRIPT} --output ${rev_header} --root ${ARG_ROOT}
        RESULT_VARIABLE rev_result)
    if (NOT rev_result EQUAL 0)
        message(WARNING "stk: gen_revision.py failed, revision will be 0")
    endif()

    # Refresh on every build (the script rewrites the header only if its content changed,
    # so an unchanged revision does not trigger a rebuild).
    add_custom_target(${rev_target}
        COMMAND ${Python3_EXECUTABLE} ${_STK_GEN_REVISION_SCRIPT} --output ${rev_header} --root ${ARG_ROOT}
        BYPRODUCTS ${rev_header}
        COMMENT "stk: updating revision header"
        VERBATIM)
    add_dependencies(${TARGET} ${rev_target})

    target_include_directories(${TARGET} PUBLIC $<BUILD_INTERFACE:${rev_dir}>)
    target_compile_definitions(${TARGET} PUBLIC STK_REVISION_HEADER)

    message(STATUS "STK_REVISION_HEADER: ${rev_header}")
endfunction()
