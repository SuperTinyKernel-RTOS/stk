# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# Deps
list(APPEND TARGET_DEPS stk)
list(APPEND TARGET_LIBS stk)

# Target
if (VENDOR_STM32)
    include(${ROOT_DIR}/deps/target/stm32fx/ProjectInclude.cmake)
    include(${ROOT_DIR}/deps/target/stm32fx/ProjectIncludeLib.cmake)
    set(LINKER_FLAGS "-Wl,-Map=${TARGET_NAME}.map")
    set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} ${LINKER_FLAGS}")
    set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} ${LINKER_FLAGS}")
    set(TARGET_BINARY ${TARGET_NAME}.elf)
elseif (RISCV)
    include(${ROOT_DIR}/deps/target/risc-v/ProjectInclude.cmake)
    include(${ROOT_DIR}/deps/target/risc-v/ProjectIncludeLib.cmake)
    set(LINKER_FLAGS "-Wl,-Map=${TARGET_NAME}.map")
    set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} ${LINKER_FLAGS}")
    set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} ${LINKER_FLAGS}")
    set(TARGET_BINARY ${TARGET_NAME}.elf)
else()
    set(TARGET_BINARY ${TARGET_NAME})
endif()

# Make target
add_executable(${TARGET_BINARY} ${TEST_SRC})
include(${ROOT_DIR}/deps/target/PostBuild.cmake)

# Link deps
add_dependencies(${TARGET_BINARY} ${TARGET_DEPS})
target_link_libraries(${TARGET_BINARY} ${TARGET_LIBS})

# Tests
if (BUILD_TESTS)
    # Add into tests
    add_test(NAME ${TARGET_BINARY} COMMAND ${TARGET_NAME})

    # Coverage report
    if (ENABLE_COVERAGE AND GCC AND NOT CMAKE_CROSSCOMPILING)
        target_add_coverage_compiler_options(${TARGET_BINARY})
        target_add_coverage_linker_options(${TARGET_BINARY})
    endif()
endif()

install(TARGETS ${TARGET_BINARY} DESTINATION ${LIBRARY_OUTPUT_PATH})