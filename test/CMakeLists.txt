# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

project(test)

option(TEST_GENERIC    "build test: Generic"                                      ON)
option(TEST_SWITCH     "build test: Switch (platform-dependent)"                  OFF)
option(TEST_CHAIN      "build test: Chain (platform-dependent)"                   OFF)
option(TEST_SLEEP      "build test: Sleep (platform-dependent)"                   OFF)
option(TEST_HRT        "build test: HRT (platform-dependent)"                     OFF)
option(TEST_MUTEX      "build test: sync::Mutex (platform-dependent)"             OFF)
option(TEST_SEMAPHORE  "build test: sync::Semaphore (platform-dependent)"         OFF)
option(TEST_EVENT      "build test: sync::Event (platform-dependent)"             OFF)
option(TEST_EVENTFLAGS "build test: sync::EventFlags (platform-dependent)"        OFF)
option(TEST_SPINLOCK   "build test: sync::SpinLock (platform-dependent)"          OFF)
option(TEST_CONDVAR    "build test: sync::ConditionVariable (platform-dependent)" OFF)
option(TEST_PIPE       "build test: sync::Pipe (platform-dependent)"              OFF)
option(TEST_RWMUTEX    "build test: sync::RWMutex (platform-dependent)"           OFF)
option(TEST_MSGQUEUE   "build test: sync::MessageQueue (platform-dependent)"      OFF)
option(TEST_TIMERHOST  "build test: time::TimerHost (platform-dependent)"         OFF)
option(TEST_BLOCKPOOL  "build test: memory::BlockPool (platform-dependent)"       OFF)

if (TEST_GENERIC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/generic)
endif()

if (TEST_SWITCH)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/switch)
endif()

if (TEST_CHAIN)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/chain)
endif()

if (TEST_SLEEP)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/sleep)
endif()

if (TEST_HRT)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/hrt)
endif()

if (TEST_MUTEX)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/mutex)
endif()

if (TEST_SEMAPHORE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/semaphore)
endif()

if (TEST_EVENT)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/event)
endif()

if (TEST_EVENTFLAGS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/eventflags)
endif()

if (TEST_SPINLOCK)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/spinlock)
endif()

if (TEST_CONDVAR)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/condvar)
endif()

if (TEST_PIPE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/pipe)
endif()

if (TEST_RWMUTEX)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/rwmutex)
endif()

if (TEST_MSGQUEUE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/msgqueue)
endif()

if (TEST_TIMERHOST)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/timerhost)
endif()

if (TEST_BLOCKPOOL)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/blockpool)
endif()