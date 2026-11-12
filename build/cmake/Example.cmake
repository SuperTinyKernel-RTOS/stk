#
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
#
#  Source: https://github.com/SuperTinyKernel-RTOS
#
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#

# Helper for build/example/<name>/CMakeLists.txt
#
#   stk_add_example(<target>
#       SOURCES        <files relative to the example folder>
#       [INTEROP       c | posix | newlib | cmsis | freertos | threadx | cyabs_rtos ...]
#       [INCLUDES      <extra include dirs>]
#       [DEFINES       <extra compile definitions, simple NAME or NAME=VALUE>]
#       [C_KERNEL_TASKS <N>]            # STK_C_KERNEL_MAX_TASKS for the C interface (see stk_c.h)
#       [C_KERNEL_TYPE  <expression>]   # STK_C_KERNEL_TYPE_CPU_0 for the C interface (see stk_c.h)
#       [REQUIRES      <ENABLE_xxx ...>]# global options of stk_config.h that must be ON
#       [REQUIRES_TARGETS <targets ...>]# external CMake targets that must exist (SDKs provided by the user)
#       [LINK_LIBS     <targets ...>]   # external targets to link with (only those which exist are linked)
#       [COMPILE_OPTIONS <flags ...>]   # extra compiler flags
#       [REQUIRES_HEADER <path>]        # header (relative to stk/include) which must exist in this source tree
#       [STM32_HAL     <modules ...>]   # extra STM32 HAL modules (adc, pwr, rtc, ...) to compile in, GPIO is always added
#       [OWN_MAIN]                      # example has its own main(), do not add common main.cpp
#       [NO_DRIVER]                     # example does not use LED driver from ../driver
#       [REQUIRES_BSP  <bsp>]           # example is written for one specific BSP (see EXAMPLE_BSP)
#       [MULTICORE]                     # example needs bsp::Cpu (dual-core Raspberry Pi RP2350)
#       [STM32_ONLY]                    # example accesses STM32F4 peripherals (RCC, GPIO, ADC) directly
#       [EMBEDDED_ONLY]                 # example can not be built for a host (simulation) target
#       [DESCRIPTION   <text>])
#
# It is a macro (not a function) on purpose: toolchain/linker flags assigned by MakeExecutable.cmake
# must stay in the scope of the example directory, otherwise CMake would ignore them.
#
# Examples which can not be built with the current configuration are skipped with a WARNING (and a hint
# how to enable them) rather than failing the whole configuration, so BUILD_EXAMPLES with many EXAMPLE_xxx
# options ON keeps working.

include_guard(GLOBAL)

# BSP (LED driver) selection, "auto" derives it from the target
set(EXAMPLE_BSP "auto" CACHE STRING "BSP (LED driver) used by the examples: auto, portable, stm32f0xx, stm32f1xx, stm32f4xx, nxpk66, nxpimxrt1050, rp2350w")
set_property(CACHE EXAMPLE_BSP PROPERTY STRINGS auto portable stm32f0xx stm32f1xx stm32f4xx nxpk66 nxpimxrt1050 rp2350w)

# Resolve BSP once
if (EXAMPLE_BSP STREQUAL "auto")
    if (VENDOR_STM32)
        if (TARGET_CORTEX_M0)
            set(_STK_EXAMPLE_BSP stm32f0xx)
        elseif (TARGET_CORTEX_M3)
            set(_STK_EXAMPLE_BSP stm32f1xx)
        else()
            set(_STK_EXAMPLE_BSP stm32f4xx)
        endif()
    else()
        # host simulation (printf) or a target without dedicated LED driver (e.g. RISC-V with semihosting)
        set(_STK_EXAMPLE_BSP portable)
    endif()
    if (VENDOR_RPI)
        set(_STK_EXAMPLE_BSP rp2350w)
    endif()
else()
    set(_STK_EXAMPLE_BSP ${EXAMPLE_BSP})
endif()

# Linker scripts of the MPU build: STM32F4 sections.ld which reserves MPU-aligned regions and exports __stk_mpu_xxx symbols,
# required when the library is built with ENABLE_MPU=ON (searched before the generic scripts of deps/target/stm32fx)
set(EXAMPLE_MPU_LD_DIR "${CMAKE_CURRENT_LIST_DIR}/../example/project/eclipse/stm/blinky-mpu-stm32f407g-disc1/ld" CACHE PATH "Folder with linker scripts (mem.ld, libs.ld, sections.ld) for the MPU examples")

macro(stk_add_example _EX_NAME)
    cmake_parse_arguments(_EX
        "OWN_MAIN;NO_DRIVER;MULTICORE;EMBEDDED_ONLY;STM32_ONLY"
        "C_KERNEL_TASKS;C_KERNEL_TYPE;DESCRIPTION;REQUIRES_BSP"
        "SOURCES;INTEROP;INCLUDES;DEFINES;REQUIRES;REQUIRES_TARGETS;LINK_LIBS;COMPILE_OPTIONS;STM32_HAL;REQUIRES_HEADER"
        ${ARGN})

    set(_EX_SKIP_REASON "")

    # --- platform checks -----------------------------------------------------------------------------------
    if (NOT VENDOR_STM32 AND NOT VENDOR_RPI AND NOT RISCV AND NOT WIN32)
        set(_EX_SKIP_REASON "STK has no scheduling driver for this host (supported: Windows x86 simulation, Arm Cortex-M (-DVENDOR_STM32=ON with arm-gcc toolchain or -DVENDOR_RPI=ON for Raspberry Pi RP2xxx), RISC-V (riscv-none-elf-gcc toolchain))")
    elseif (_EX_EMBEDDED_ONLY AND NOT CMAKE_CROSSCOMPILING)
        set(_EX_SKIP_REASON "needs an embedded target, configure with a cross toolchain, e.g. -DCMAKE_TOOLCHAIN_FILE=build/cmake/toolchain/arm-none-eabi-gcc.cmake -DVENDOR_STM32=ON")
    elseif (_EX_STM32_ONLY AND NOT VENDOR_STM32)
        set(_EX_SKIP_REASON "uses STM32F4 peripherals directly, build with -DVENDOR_STM32=ON")
    elseif (_EX_REQUIRES_BSP AND NOT _STK_EXAMPLE_BSP STREQUAL _EX_REQUIRES_BSP)
        set(_EX_SKIP_REASON "written for BSP '${_EX_REQUIRES_BSP}', use -DEXAMPLE_BSP=${_EX_REQUIRES_BSP} with its SDK")
    elseif (_EX_MULTICORE AND NOT _STK_EXAMPLE_BSP STREQUAL "rp2350w")
        set(_EX_SKIP_REASON "needs a dual-core target, use -DEXAMPLE_BSP=rp2350w with the Pico SDK")
    endif()

    # --- external SDKs ---------------------------------------------------------------------------------------
    if (NOT _EX_SKIP_REASON)
        foreach (_EX_TGT ${_EX_REQUIRES_TARGETS})
            if (NOT TARGET ${_EX_TGT})
                set(_EX_SKIP_REASON "needs CMake target '${_EX_TGT}' from an external SDK which is not available in this build (see README.md of the example)")
            endif()
        endforeach()
    endif()

    # --- headers which may be missing in a source tree ---------------------------------------------------------
    if (NOT _EX_SKIP_REASON AND _EX_REQUIRES_HEADER)
        if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../stk/include/${_EX_REQUIRES_HEADER})
            set(_EX_SKIP_REASON "needs stk/include/${_EX_REQUIRES_HEADER} which is not part of this source tree")
        endif()
    endif()

    # --- required stk_config.h features --------------------------------------------------------------------
    if (NOT _EX_SKIP_REASON)
        foreach (_EX_OPT ${_EX_REQUIRES})
            if (NOT ${_EX_OPT})
                set(_EX_SKIP_REASON "needs -D${_EX_OPT}=<value> (ON for a switch, see the example)")
            endif()
        endforeach()
    endif()

    # --- library must be built to link against -------------------------------------------------------------
    if (NOT _EX_SKIP_REASON AND NOT TARGET stk)
        set(_EX_SKIP_REASON "needs the STK library, configure with -DBUILD_LIB=ON")
    endif()

    if (_EX_SKIP_REASON)
        message(WARNING "example '${_EX_NAME}' skipped: ${_EX_SKIP_REASON}")
    else()
        set(TARGET_NAME ${_EX_NAME})
        message(STATUS "- " ${TARGET_NAME})

        # Project root dir
        set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
        set(EXAMPLE_DIR ${ROOT_DIR}/build/example)

        # Includes (note: examples include driver headers relative to their own folder, e.g. "../driver/led.h")
        include_directories(
            ${ROOT_DIR}/stk/include
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${_EX_INCLUDES})

        # Per-example compile definitions
        if (_EX_DEFINES)
            add_compile_definitions(${_EX_DEFINES})
        endif()

        # Per-example C interface configuration (forced include, so that interop sources compiled into this
        # example and the example itself see exactly the same STK_C_xxx values)
        set(_EX_CFG_H ${CMAKE_CURRENT_BINARY_DIR}/stk_example_config.h)
        set(_EX_CFG "#ifndef STK_EXAMPLE_CONFIG_H_\n#define STK_EXAMPLE_CONFIG_H_\n")
        if (_EX_C_KERNEL_TASKS)
            string(APPEND _EX_CFG "#define STK_C_KERNEL_MAX_TASKS (${_EX_C_KERNEL_TASKS})\n")
        endif()
        if (NOT _EX_C_KERNEL_TYPE)
            # default kernel of the C interface (see stk_c.h for the list of supported variants)
            set(_EX_C_KERNEL_TYPE "Kernel<KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), STK_C_KERNEL_MAX_TASKS, sched::SwitchStrategyRR, PlatformDefault>")
        endif()
        string(APPEND _EX_CFG "#ifndef STK_C_KERNEL_TYPE_CPU_0\n#define STK_C_KERNEL_TYPE_CPU_0 ${_EX_C_KERNEL_TYPE}\n#endif\n")
        string(APPEND _EX_CFG "#endif\n")
        file(GENERATE OUTPUT ${_EX_CFG_H} CONTENT "${_EX_CFG}")
        if (MSVC)
            add_compile_options("/FI${_EX_CFG_H}")
        else()
            add_compile_options("SHELL:-include ${_EX_CFG_H}")
        endif()

        if (_EX_COMPILE_OPTIONS)
            add_compile_options(${_EX_COMPILE_OPTIONS})
        endif()

        # Sources: example
        set(TEST_SRC)
        foreach (_EX_SRC ${_EX_SOURCES})
            list(APPEND TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/${_EX_SRC})
        endforeach()

        # Sources: main()
        if (NOT _EX_OWN_MAIN)
            list(APPEND TEST_SRC ${EXAMPLE_DIR}/main.cpp)
        endif()

        # Sources: LED driver
        if (NOT _EX_NO_DRIVER)
            list(APPEND TEST_SRC ${EXAMPLE_DIR}/driver/c_wrapper.cpp ${EXAMPLE_DIR}/driver/${_STK_EXAMPLE_BSP}/led.cpp)
            if (_STK_EXAMPLE_BSP STREQUAL "rp2350w")
                list(APPEND TEST_SRC ${EXAMPLE_DIR}/driver/rp2350w/cpu.cpp)
            endif()
        endif()

        # Sources: STM32 HAL modules used by the LED driver/examples which are not part of the stm32fx library
        if (VENDOR_STM32 AND NOT _EX_NO_DRIVER OR _EX_STM32_HAL)
            if (TARGET_CORTEX_M0)
                set(_EX_HAL_DIR ${ROOT_DIR}/deps/target/stm32fx/src/stm32f0-hal)
                set(_EX_HAL_PFX stm32f0xx_hal_)
            elseif (TARGET_CORTEX_M3)
                set(_EX_HAL_DIR ${ROOT_DIR}/deps/target/stm32fx/src/stm32f1-hal)
                set(_EX_HAL_PFX stm32f1xx_hal_)
            else()
                set(_EX_HAL_DIR ${ROOT_DIR}/deps/target/stm32fx/src/stm32f4-hal)
                set(_EX_HAL_PFX stm32f4xx_hal_)
            endif()
            foreach (_EX_HAL gpio ${_EX_STM32_HAL})
                list(APPEND TEST_SRC ${_EX_HAL_DIR}/${_EX_HAL_PFX}${_EX_HAL}.c)
            endforeach()
        endif()

        # MPU build needs the linker script which exports the MPU regions (must precede link directories of stm32fx)
        if (VENDOR_STM32 AND ENABLE_MPU AND TARGET_CORTEX_M4)
            link_directories(${EXAMPLE_MPU_LD_DIR})
        endif()

        # Sources: interoperability layers (compiled into the example so that every example gets its own
        # configuration of the C interface)
        foreach (_EX_IOP ${_EX_INTEROP})
            if (_EX_IOP STREQUAL "c")
                include_directories(${ROOT_DIR}/interop/c/include)
                file(GLOB _EX_IOP_SRC ${ROOT_DIR}/interop/c/src/*.cpp)
            elseif (_EX_IOP STREQUAL "posix")
                include_directories(${ROOT_DIR}/interop/posix/include)
                file(GLOB _EX_IOP_SRC ${ROOT_DIR}/interop/posix/src/*.cpp)
            elseif (_EX_IOP STREQUAL "newlib")
                include_directories(${ROOT_DIR}/interop/newlib/src)
                file(GLOB _EX_IOP_SRC ${ROOT_DIR}/interop/newlib/src/*.cpp)
            elseif (_EX_IOP STREQUAL "cmsis")
                include_directories(${ROOT_DIR}/interop/cmsis/rtos2/include)
                file(GLOB _EX_IOP_SRC ${ROOT_DIR}/interop/cmsis/rtos2/src/*.cpp)
            elseif (_EX_IOP STREQUAL "freertos")
                include_directories(${ROOT_DIR}/interop/freertos/include)
                file(GLOB _EX_IOP_SRC ${ROOT_DIR}/interop/freertos/src/*.cpp)
            elseif (_EX_IOP STREQUAL "threadx")
                include_directories(${ROOT_DIR}/interop/threadx/include)
                file(GLOB _EX_IOP_SRC ${ROOT_DIR}/interop/threadx/src/*.cpp)
            elseif (_EX_IOP STREQUAL "cyabs_rtos")
                include_directories(
                    ${ROOT_DIR}/interop/cyabs_rtos/include
                    ${ROOT_DIR}/interop/cyabs_rtos/include/COMPONENT_STK                
                    ${ROOT_DIR}/deps/infineon/core-lib/include      
                )
                file(GLOB _EX_IOP_SRC
                    ${ROOT_DIR}/interop/cyabs_rtos/source/COMPONENT_STK/*.cpp
                    ${ROOT_DIR}/interop/cyabs_rtos/source/*.c
                )
                # Fetch Infineon core-lib
                execute_process(
                    COMMAND ${Python3_EXECUTABLE} "${ROOT_DIR}/deps/scripts/fetch_infineon_core_lib.py"
                    WORKING_DIRECTORY "${ROOT_DIR}"
                    RESULT_VARIABLE _fetch_result
                )
                if (NOT _fetch_result EQUAL 0)
                    message(FATAL_ERROR "stk_add_example: Failed to fetch Infineon core-lib (exit code: ${_fetch_result})")
                endif()
            else()
                message(FATAL_ERROR "stk_add_example: unknown INTEROP '${_EX_IOP}'")
            endif()
            list(APPEND TEST_SRC ${_EX_IOP_SRC})
        endforeach()

        # Finalize (add_executable, link with stk and target dependencies, post-build steps)
        include(${ROOT_DIR}/build/cmake/MakeExecutable.cmake)
        
        # External SDKs (Pico SDK, TinyUSB, ...)
        foreach (_EX_LIB ${_EX_LINK_LIBS})
            if (TARGET ${_EX_LIB})
                target_link_libraries(${TARGET_BINARY} ${_EX_LIB})
            endif()
        endforeach()
        # Raspberry Pi: LED driver of Pico 2 W (CYW43 GPIO) and the dual-core helper (bsp::Cpu)
        if (VENDOR_RPI AND NOT _EX_NO_DRIVER)
            # CYW43 architecture: "none" (LED only) unless the example links its own variant (e.g. pico_cyw43_arch_lwip_sys_stk)
            set(_EX_CYW43_NONE pico_cyw43_arch_none)
            foreach (_EX_LIB ${_EX_LINK_LIBS})
                if (_EX_LIB MATCHES "^pico_cyw43_arch")
                    set(_EX_CYW43_NONE)
                endif()
            endforeach()
            foreach (_EX_LIB ${_EX_CYW43_NONE} pico_multicore)
                if (TARGET ${_EX_LIB})
                    target_link_libraries(${TARGET_BINARY} ${_EX_LIB})
                endif()
            endforeach()
        endif()
        
        # Define a custom build target for flashing
        if (VENDOR_STM32)
            add_custom_target(flash_${_EX_NAME}
                COMMAND ${Python3_EXECUTABLE} "${ROOT_DIR}/build/scripts/flash_stm32.py"
                        $<TARGET_FILE:${TARGET_BINARY}>
                        --jlink "C:/Program Files (x86)/SEGGER/JLink/JLink.exe"
                DEPENDS ${TARGET_BINARY}
                COMMENT "Flashing ${TARGET_BINARY} to the STM32 board..."
                WORKING_DIRECTORY "${ROOT_DIR}"
            )
        endif()
        
    endif()
endmacro()
