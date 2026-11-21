if (NOT ROOT_DIR)
    message(FATAL_ERROR "ROOT_DIR must be defined and point to the root of STK repository!")
endif()

# Linker script: stk-mpu.ld adds the .stk_mpu_xxx regions for the MPU (PMP) support (STK_MPU), stk.ld otherwise
if (STK_MPU)
    set(LINKER_FLAGS "-T stk-mpu.ld")
else()
    set(LINKER_FLAGS "-T stk.ld")
endif()

set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} ${LINKER_FLAGS}")
set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} ${LINKER_FLAGS}")

list(APPEND TARGET_DEPS riscv)
list(APPEND TARGET_LIBS -Wl,--whole-archive riscv -Wl,--no-whole-archive)