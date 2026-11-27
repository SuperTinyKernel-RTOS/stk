if (NOT ROOT_DIR)
    message(FATAL_ERROR "ROOT_DIR must be defined and point to the root of STK repository!")
endif()

# Family of the device, defaults are the devices of the boards used by STK
if (TARGET_CORTEX_M0)
    set(STM32FX_FAMILY f0)
    set(STM32FX_FAMILY_DEFINE STM32F0)
    set(STM32FX_DEFAULT_DEVICE STM32F051x8)  # STM32F0DISCOVERY
elseif (TARGET_CORTEX_M3)
    set(STM32FX_FAMILY f1)
    set(STM32FX_FAMILY_DEFINE STM32F1)
    set(STM32FX_DEFAULT_DEVICE STM32F103xB)  # NUCLEO-F103RB
elseif (TARGET_CORTEX_M4)
    set(STM32FX_FAMILY f4)
    set(STM32FX_FAMILY_DEFINE STM32F4)
    set(STM32FX_DEFAULT_DEVICE STM32F407xx)  # STM32F407DISC1
elseif (TARGET_CORTEX_M7)
    set(STM32FX_FAMILY f7)
    set(STM32FX_FAMILY_DEFINE STM32F7)
    set(STM32FX_DEFAULT_DEVICE STM32F746xx)  # NUCLEO-F746ZG, STM32F746G-DISCO
else()
    message(FATAL_ERROR "stm32: TARGET_CORTEX_M0, TARGET_CORTEX_M3, TARGET_CORTEX_M4 or TARGET_CORTEX_M7 must be set!")
endif()

if (NOT TARGET_CPU_FAMILY)
    set(TARGET_CPU_FAMILY ${STM32FX_DEFAULT_DEVICE} CACHE STRING "TARGET_CPU_FAMILY" FORCE)
endif()

# CMSIS-Core, CMSIS device package and HAL driver are fetched from STMicroelectronics, see deps/scripts/deps.py
set(STM32FX_ST_DIR            ${ROOT_DIR}/deps/stm32)
set(STM32FX_CMSIS_CORE_DIR    ${STM32FX_ST_DIR}/cmsis-core)
set(STM32FX_CMSIS_DEVICE_DIR  ${STM32FX_ST_DIR}/${STM32FX_FAMILY}/cmsis-device)
set(STM32FX_HAL_DIR           ${STM32FX_ST_DIR}/${STM32FX_FAMILY}/hal)

if (NOT EXISTS ${STM32FX_CMSIS_CORE_DIR}/Include OR NOT EXISTS ${STM32FX_CMSIS_DEVICE_DIR}/Include OR NOT EXISTS ${STM32FX_HAL_DIR}/Inc)
    message(WARNING "STM32 dependencies are missing, will fetch them with: python3 deps/scripts/fetch_deps.py stm32-${STM32FX_FAMILY}")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${ROOT_DIR}/deps/scripts/fetch_deps.py stm32-${STM32FX_FAMILY}
        RESULT_VARIABLE fetch_result
    )
    if (NOT fetch_result EQUAL 0)
        message(FATAL_ERROR "stk: fetch_deps.py failed")
    endif()
endif()

# Base includes: the target comes first, its HAL configuration shim is picked by the HAL headers
include_directories(
    ${ROOT_DIR}/deps/target/stm32/include
    ${STM32FX_CMSIS_CORE_DIR}/Include
    ${STM32FX_CMSIS_DEVICE_DIR}/Include
    ${STM32FX_HAL_DIR}/Inc
)

# Link directories: memory map of the family, sections
link_directories(
    ${ROOT_DIR}/deps/target/stm32/src/ld/stm32${STM32FX_FAMILY}
    ${ROOT_DIR}/deps/target/stm32/src/ld
)

# Trace channel: semihosting-debug (default), itm or none, see include/diag/trace.h
set(STK_STM32FX_TRACE "semihosting-debug" CACHE STRING "Trace channel of the STM32Fx target: semihosting, semihosting-debug, itm or none")
if (STK_STM32FX_TRACE STREQUAL "semihosting")
    set(STM32FX_TRACE_DEFINES -DTRACE -DSTK_TRACE_SEMIHOSTING)
elseif (STK_STM32FX_TRACE STREQUAL "semihosting-debug")
    set(STM32FX_TRACE_DEFINES -DTRACE -DSTK_TRACE_SEMIHOSTING_DEBUG)
elseif (STK_STM32FX_TRACE STREQUAL "itm")
    set(STM32FX_TRACE_DEFINES -DTRACE -DSTK_TRACE_ITM)
elseif (STK_STM32FX_TRACE STREQUAL "none")
    set(STM32FX_TRACE_DEFINES "")
else()
    message(FATAL_ERROR "STK_STM32FX_TRACE must be semihosting, semihosting-debug, itm or none")
endif()

# Base definitions
add_definitions(
    -D${STM32FX_FAMILY_DEFINE}
    -D${TARGET_CPU_FAMILY}
    -DUSE_HAL_DRIVER
    -DUSE_FULL_ASSERT
    -DHSE_VALUE=8000000
    ${STM32FX_TRACE_DEFINES}
)

# Device specific include file
set(_STK_DEVICE_INC "cmsis_device.h" CACHE STRING "_STK_DEVICE_INC" FORCE)
