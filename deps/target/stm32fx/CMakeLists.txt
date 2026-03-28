# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

set(TARGET_NAME stm32fx)
project(${TARGET_NAME})
message(STATUS "- " ${TARGET_NAME})

# Definitions, includes
set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
include(ProjectInclude.cmake)

# Sources
file(GLOB STM32FX_SRS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
file(GLOB STM32FX_CORTEXM ${CMAKE_CURRENT_SOURCE_DIR}/src/cortexm/*.c)
file(GLOB STM32FX_DIAG ${CMAKE_CURRENT_SOURCE_DIR}/src/diag/*.c)
file(GLOB STM32FX_NEWLIB ${CMAKE_CURRENT_SOURCE_DIR}/src/newlib/*.c)
if (TARGET_CORTEX_M0)
    file(GLOB STM32FX_CMSIS ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f0-cmsis/*.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f0-hal/stm32f0xx_hal_cortex.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f0-hal/stm32f0xx_hal_rcc.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f0-hal/stm32f0xx_hal_rcc_ex.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f0-hal/stm32f0xx_hal.c)
elseif (TARGET_CORTEX_M3)   
    file(GLOB STM32FX_CMSIS ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f1-cmsis/*.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f1-hal/stm32f1xx_hal_cortex.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f1-hal/stm32f1xx_hal_rcc.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f1-hal/stm32f1xx_hal_rcc_ex.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f1-hal/stm32f1xx_hal.c)
elseif (TARGET_CORTEX_M4)
    file(GLOB STM32FX_CMSIS ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f4-cmsis/*.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f4-hal/stm32f4xx_hal_cortex.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f4-hal/stm32f4xx_hal_rcc.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f4-hal/stm32f4xx_hal_rcc_ex.c)
    list(APPEND STM32FX_HAL ${CMAKE_CURRENT_SOURCE_DIR}/src/stm32f4-hal/stm32f4xx_hal.c)
endif()

# Target
add_library(${TARGET_NAME} STATIC ${STM32FX_SRS} ${STM32FX_CMSIS} ${STM32FX_CORTEXM} ${STM32FX_DIAG} ${STM32FX_NEWLIB} ${STM32FX_HAL})

install(TARGETS ${TARGET_NAME} DESTINATION ${LIBRARY_OUTPUT_PATH})