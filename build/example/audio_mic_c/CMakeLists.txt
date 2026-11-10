# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Example.cmake)

stk_add_example(audio_mic_c
    SOURCES        main.c usb_descriptors.c
    INTEROP        c
    C_KERNEL_TASKS 3
    C_KERNEL_TYPE  "Kernel<KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), STK_C_KERNEL_MAX_TASKS, sched::SwitchStrategyFP32, PlatformDefault>"
    REQUIRES_TARGETS tinyusb_device
    LINK_LIBS      tinyusb_device tinyusb_board
    OWN_MAIN
    NO_DRIVER)
