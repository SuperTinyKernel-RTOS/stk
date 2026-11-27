if (NOT ROOT_DIR)
    message(FATAL_ERROR "ROOT_DIR must be defined and point to the root of STK repository!")
endif()

# (the scripts are found in the link directories, see ProjectInclude.cmake)
# Memory map of the family, then the sections: sections-mpu.ld adds the .stk_mpu_xxx regions for the MPU support
# (STK_MPU), sections.ld otherwise
if (STK_MPU)
    set(LINKER_FLAGS "-T mem.ld -T sections-mpu.ld")
else()
    set(LINKER_FLAGS "-T mem.ld -T sections.ld")
endif()

set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} ${LINKER_FLAGS}")
set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} ${LINKER_FLAGS}")

list(APPEND TARGET_DEPS stm32)
list(APPEND TARGET_LIBS stm32)
