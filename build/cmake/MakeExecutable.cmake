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

# SEGGER SystemView tracing (must follow stk on the link line)
if (ENABLE_SEGGER_SYSVIEW AND TARGET segger)
    include_directories(${ROOT_DIR}/deps/segger)
    list(APPEND TARGET_DEPS segger)
    list(APPEND TARGET_LIBS segger)
endif()

# Target
if (VENDOR_STM32)
    include(${ROOT_DIR}/deps/target/stm32/ProjectInclude.cmake)
    include(${ROOT_DIR}/deps/target/stm32/ProjectIncludeLib.cmake)
    set(LINKER_FLAGS "-Wl,-Map=${TARGET_NAME}.map")
    set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} ${LINKER_FLAGS}")
    set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} ${LINKER_FLAGS}")
    set(TARGET_BINARY ${TARGET_NAME}.elf)
elseif (VENDOR_RPI)
    include(${ROOT_DIR}/deps/target/rpi/ProjectIncludeLib.cmake)
    # note: Pico SDK names the output <target>.elf itself
    set(TARGET_BINARY ${TARGET_NAME})
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

# Define a custom build target for flashing
if (VENDOR_STM32)
    add_custom_target(flash-${TARGET_BINARY}
        COMMAND ${Python3_EXECUTABLE} "${ROOT_DIR}/build/scripts/flash_stm32.py"
                $<TARGET_FILE:${TARGET_BINARY}>
                --jlink "C:/Program Files (x86)/SEGGER/JLink/JLink.exe"
        DEPENDS ${TARGET_BINARY}
        COMMENT "Flashing ${TARGET_BINARY} to the STM32 board..."
        WORKING_DIRECTORY "${ROOT_DIR}"
    )
endif()

# Define a custom build target for running in QEMU with semihosting
if (VENDOR_STM32)
    # Map Cortex-M target family to QEMU board and MCU defaults
    set(_qemu_default_board "")
    set(_qemu_default_mcu "")

    if (TARGET_CORTEX_M0)
        set(_qemu_default_board "STM32F0-Discovery")
        set(_qemu_default_mcu "STM32F051R8")
    elseif (TARGET_CORTEX_M3)
        set(_qemu_default_board "NUCLEO-F103RB")
        set(_qemu_default_mcu "STM32F103RB")
    elseif (TARGET_CORTEX_M4)
        set(_qemu_default_board "STM32F4-Discovery")
        set(_qemu_default_mcu "STM32F407VG")
    elseif (TARGET_CORTEX_M7)
        # qemu-system-gnuarmeclipse has no STM32F7 board model; override via -DQEMU_STM32_BOARD=... if using another QEMU binary
        set(_qemu_default_board "")
        set(_qemu_default_mcu "")
    endif()

    set(QEMU_STM32_BOARD "${_qemu_default_board}" CACHE STRING "QEMU board name for STM32 targets")
    set(QEMU_STM32_MCU "${_qemu_default_mcu}" CACHE STRING "QEMU MCU name for STM32 targets")

    find_program(QEMU_ARM_EXECUTABLE
        NAMES qemu-system-gnuarmeclipse qemu-system-gnuarmeclipse.exe qemu-system-arm qemu-system-arm.exe
        HINTS
            "$ENV{QEMU_ARM_PATH}"
            "D:/Eclipse/xpack-qemu-arm-8.2.6-1/bin"
            "/opt/xpack/qemu-arm/bin"
            "/usr/local/bin"
    )

    if (QEMU_ARM_EXECUTABLE AND QEMU_STM32_BOARD AND QEMU_STM32_MCU)
        add_custom_target(qemu-${TARGET_BINARY}
            COMMAND ${QEMU_ARM_EXECUTABLE}
                    --board ${QEMU_STM32_BOARD}
                    --mcu ${QEMU_STM32_MCU}
                    --nographic
                    --semihosting-config enable=on,target=native
                    --image $<TARGET_FILE:${TARGET_BINARY}>
            DEPENDS ${TARGET_BINARY}
            USES_TERMINAL
            COMMENT "Running ${TARGET_BINARY} in QEMU (${QEMU_STM32_BOARD} / ${QEMU_STM32_MCU})..."
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )
        message(STATUS "QEMU: added target 'qemu-${TARGET_BINARY}' using ${QEMU_ARM_EXECUTABLE} (--board ${QEMU_STM32_BOARD}, --mcu ${QEMU_STM32_MCU})")
    elseif (NOT QEMU_ARM_EXECUTABLE)
        message(STATUS "QEMU: skipping target 'qemu-${TARGET_BINARY}': QEMU executable not found (set QEMU_ARM_EXECUTABLE or QEMU_ARM_PATH)")
    else()
        message(STATUS "QEMU: skipping target 'qemu-${TARGET_BINARY}': QEMU_STM32_BOARD or QEMU_STM32_MCU is not set")
    endif()
endif()

install(TARGETS ${TARGET_BINARY} DESTINATION ${LIBRARY_OUTPUT_PATH})