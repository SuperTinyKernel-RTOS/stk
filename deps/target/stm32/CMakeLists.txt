# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

set(TARGET_NAME stm32)
project(${TARGET_NAME})
message(STATUS "- " ${TARGET_NAME})

# Definitions, includes
set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
include(ProjectInclude.cmake)

# Sources of the target
file(GLOB STM32FX_INIT ${CMAKE_CURRENT_SOURCE_DIR}/src/init/startup.c
                       ${CMAKE_CURRENT_SOURCE_DIR}/src/init/hardware.c
                       ${CMAKE_CURRENT_SOURCE_DIR}/src/init/exceptions.c)
file(GLOB STM32FX_LIBC ${CMAKE_CURRENT_SOURCE_DIR}/src/libc/*.c)
file(GLOB STM32FX_DIAG ${CMAKE_CURRENT_SOURCE_DIR}/src/diag/*.c)

# Vector table of the device (generate one for another device with tools/gen_vectors.py)
string(TOLOWER "${TARGET_CPU_FAMILY}" STM32FX_DEVICE)
set(STM32FX_VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/src/init/vectors_${STM32FX_DEVICE}.c)
if (NOT EXISTS ${STM32FX_VECTORS})
    message(FATAL_ERROR "No vector table for ${TARGET_CPU_FAMILY}: generate src/init/vectors_${STM32FX_DEVICE}.c "
                        "with tools/gen_vectors.py from startup_${STM32FX_DEVICE}.s of the CMSIS device package.")
endif()

# Sources fetched from STMicroelectronics (deps/scripts/fetch_deps.py): system file of the device and the HAL core
set(STM32FX_ST_SRC ${STM32FX_CMSIS_DEVICE_DIR}/Source/Templates/system_stm32${STM32FX_FAMILY}xx.c)
foreach (_module hal hal_cortex hal_rcc hal_rcc_ex)
    list(APPEND STM32FX_ST_SRC ${STM32FX_HAL_DIR}/Src/stm32${STM32FX_FAMILY}xx_${_module}.c)
endforeach()

# Target
add_library(${TARGET_NAME} STATIC ${STM32FX_INIT} ${STM32FX_VECTORS} ${STM32FX_LIBC} ${STM32FX_DIAG} ${STM32FX_ST_SRC})

# MPU support: start-up code initializes the .stk_mpu_xxx regions of sections-mpu.ld
if (STK_MPU)
    target_compile_definitions(${TARGET_NAME} PRIVATE STK_MPU=1)
endif()

install(TARGETS ${TARGET_NAME} DESTINATION ${LIBRARY_OUTPUT_PATH})
