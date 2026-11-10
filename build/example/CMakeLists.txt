# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

project(example)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Example.cmake)

# Build every example which is supported by the current configuration (the rest is skipped with a warning)
option(EXAMPLE_ALL                       "build all examples"                       OFF)

option(EXAMPLE_AUDIO_MIC                 "build example: Audio Mic"                 OFF)
option(EXAMPLE_AUDIO_MIC_C               "build example: Audio Mic C"               OFF)
option(EXAMPLE_BLINKY                    "build example: Blinky"                    OFF)
option(EXAMPLE_BLINKY_TRUSTZONE_NS       "build example: Blinky Trustzone NS"       OFF)
option(EXAMPLE_BLINKY_TRUSTZONE_SC       "build example: Blinky Trustzone SC"       OFF)
option(EXAMPLE_BLINKY_C                  "build example: Blinky C"                  OFF)
option(EXAMPLE_BLINKY_C_CMSIS_RTOS2      "build example: Blinky C CMSIS RTOS2"      OFF)
option(EXAMPLE_BLINKY_C_CMSIS_RTOS2_PRIO "build example: Blinky C CMSIS RTOS2 Prio" OFF)
option(EXAMPLE_BLINKY_C_FREERTOS         "build example: Blinky C FreeRTOS"         OFF)
option(EXAMPLE_BLINKY_C_PTHREAD          "build example: Blinky C Pthread"          OFF)
option(EXAMPLE_BLINKY_C_THREADX          "build example: Blinky C ThreadX"          OFF)
option(EXAMPLE_BLINKY_C_CYABS_RTOS       "build example: Blinky C Abstract-RTOS"    OFF)
option(EXAMPLE_BLINKY_THREADX            "build example: Blinky ThreadX"            OFF)
option(EXAMPLE_BLINKY_FP                 "build example: Blinky FP"                 OFF)
option(EXAMPLE_BLINKY_MC                 "build example: Blinky MC"                 OFF)
option(EXAMPLE_BLINKY_MPU                "build example: Blinky MPU"                OFF)
option(EXAMPLE_BLINKY_MPU_HARD           "build example: Blinky MPU Hard"           OFF)
option(EXAMPLE_BLINKY_MPU_TRUSTZONE_NS   "build example: Blinky MPU Trustzone NS"   OFF)
option(EXAMPLE_BLINKY_MPU_TRUSTZONE_SC   "build example: Blinky MPU Trustzone SC"   OFF)
option(EXAMPLE_BLINKY_MPU_ARMV8          "build example: Blinky MPU ARMv8"          OFF)
option(EXAMPLE_BLINKY_SWRR               "build example: Blinky SWRR"               OFF)
option(EXAMPLE_BLINKY_TIME_SLICE         "build example: Blinky Time Slice"         OFF)
option(EXAMPLE_CRITICAL_SECTION          "build example: Critical Section"          OFF)
option(EXAMPLE_DEBOUNCE                  "build example: Debounce"                  OFF)
option(EXAMPLE_DRIVER                    "build example: Driver"                    OFF)
option(EXAMPLE_HRT                       "build example: HRT"                       OFF)
option(EXAMPLE_HTTPD                     "build example: Httpd"                     OFF)
option(EXAMPLE_HTTPD_TLS                 "build example: Httpd TLS"                 OFF)
option(EXAMPLE_LOW_POWER                 "build example: Low Power"                 OFF)
option(EXAMPLE_LOW_POWER_C               "build example: Low Power C"               OFF)
option(EXAMPLE_ONCE                      "build example: Once"                      OFF)
option(EXAMPLE_SECURE_TASK               "build example: Secure Task"               OFF)
option(EXAMPLE_SLEEP                     "build example: Sleep"                     OFF)
option(EXAMPLE_SYNC                      "build example: Sync"                      OFF)
option(EXAMPLE_SYNC_C                    "build example: Sync C"                    OFF)
option(EXAMPLE_TIMER                     "build example: Timer"                     OFF)
option(EXAMPLE_TIMER_C                   "build example: Timer C"                   OFF)
option(EXAMPLE_TLS                       "build example: TLS"                       OFF)

if (EXAMPLE_ALL OR EXAMPLE_AUDIO_MIC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/audio_mic)
endif()

if (EXAMPLE_ALL OR EXAMPLE_AUDIO_MIC_C)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/audio_mic_c)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_TRUSTZONE_NS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky-trustzone-ns)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_TRUSTZONE_SC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky-trustzone-sc)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_C)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_c)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_C_CMSIS_RTOS2)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_c-cmsis-rtos2)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_C_CMSIS_RTOS2_PRIO)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_c-cmsis-rtos2-prio)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_C_FREERTOS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_c-freertos)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_C_PTHREAD)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_c-pthread)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_C_THREADX)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_c-threadx)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_THREADX)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky-threadx)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_C_CYABS_RTOS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_c-cyabs_rtos)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_FP)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_fp)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_MC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_mc)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_MPU)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_mpu)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_MPU_HARD)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_mpu-hard)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_MPU_TRUSTZONE_NS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_mpu-trustzone-ns)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_MPU_TRUSTZONE_SC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_mpu-trustzone-sc)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_MPU_ARMV8)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_mpu_armv8)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_SWRR)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_swrr)
endif()

if (EXAMPLE_ALL OR EXAMPLE_BLINKY_TIME_SLICE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blinky_time-slice)
endif()

if (EXAMPLE_ALL OR EXAMPLE_CRITICAL_SECTION)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/critical_section)
endif()

if (EXAMPLE_ALL OR EXAMPLE_DEBOUNCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/debounce)
endif()

if (EXAMPLE_ALL OR EXAMPLE_DRIVER)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/driver)
endif()

if (EXAMPLE_ALL OR EXAMPLE_HRT)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/hrt)
endif()

if (EXAMPLE_ALL OR EXAMPLE_HTTPD)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/httpd)
endif()

if (EXAMPLE_ALL OR EXAMPLE_HTTPD_TLS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/httpd_tls)
endif()

if (EXAMPLE_ALL OR EXAMPLE_LOW_POWER)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/low_power)
endif()

if (EXAMPLE_ALL OR EXAMPLE_LOW_POWER_C)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/low_power_c)
endif()

if (EXAMPLE_ALL OR EXAMPLE_ONCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/once)
endif()

if (EXAMPLE_ALL OR EXAMPLE_SECURE_TASK)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/secure_task)
endif()

if (EXAMPLE_ALL OR EXAMPLE_SLEEP)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/sleep)
endif()

if (EXAMPLE_ALL OR EXAMPLE_SYNC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/sync)
endif()

if (EXAMPLE_ALL OR EXAMPLE_SYNC_C)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/sync_c)
endif()

if (EXAMPLE_ALL OR EXAMPLE_TIMER)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/timer)
endif()

if (EXAMPLE_ALL OR EXAMPLE_TIMER_C)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/timer_c)
endif()

if (EXAMPLE_ALL OR EXAMPLE_TLS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tls)
endif()