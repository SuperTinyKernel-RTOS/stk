# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# lwIP sys_arch port (NO_SYS=0): mutexes, semaphores, mailboxes and threads on top of STK.
# Provide lwIP itself and lwipopts.h through the consuming application. The "arch/sys_arch.h" of this port is
# found through the include directory of this library (lwIP expects it on the include path next to cc.h).

add_library(stk_lwip_port INTERFACE)
target_sources(stk_lwip_port INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/sys_arch.cpp)
target_include_directories(stk_lwip_port INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(stk_lwip_port INTERFACE stk_headers)
