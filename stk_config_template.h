/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_CONFIG_H_
#define STK_CONFIG_H_

// note: Replace below includes with includes provided by the vendor of your board.
// note: Rename this file to stk_config.h and place to your project folder which references includes (normally where main.cpp is, see /example).

// Example for NXP K66 MCU:
//#include "system_MK66F18.h"
//#include "MK66F18.h"
//#include "core_cm4.h"

// =============================================================================
// ARCHITECTURE & DEVICE SELECTION
// =============================================================================

//#define _STK_ARCH_ARM_CORTEX_M       /* Target ARM Cortex-M MCU family */
//#define _STK_ARCH_RISC_V             /* Target RISC-V architecture */
//#define _STK_ARCH_X86_WIN32          /* Target x86/Windows simulation & testing */

#ifdef _STK_ARCH_ARM_CORTEX_M
    // Redefine if SysTick handler name is different from SysTick_Handler
    //#define STK_SYSTICK_HANDLER SysTick_Handler

    // Redefine if PendSV handler name is different from PendSV_Handler
    //#define STK_PENDSV_HANDLER PendSV_Handler

    // Redefine if SVC handler name is different from SVC_Handler
    //#define STK_SVC_HANDLER SVC_Handler
#endif


// =============================================================================
// TICKLESS / DYNAMIC-TICK MODE
// =============================================================================

#define STK_TICKLESS_IDLE            0U     /* Enable tickless low-power idle mode */
#define STK_TICKLESS_USE_ARM_DWT     1U     /* Use ARM DWT timer for precise tick calculation */
#define STK_TICKLESS_TICKS_MAX       1000U  /* Maximum suppressed ticks in one interval */


// =============================================================================
// THREAD-LOCAL STORAGE (TLS)
// =============================================================================

#define STK_TLS                      0U     /* Enable per-task thread-local storage */
#define STK_TLS_PREFER_REGISTER      0U     /* Store TLS pointer in dedicated CPU register (r9/tp) */


// =============================================================================
// MEMORY PROTECTION UNIT (MPU) & ARM TRUSTZONE
// =============================================================================

#define STK_MPU                      0U     /* Enable hardware MPU support */
#define STK_MPU_STACK_GUARD          0U     /* Reprogram MPU stack guard region on every context switch */
#define STK_MPU_TASK_REGIONS         2U     /* Number of MPU region slots reserved per task */
#define STK_MPU_TASK_REGIONS_NS      2U     /* Number of MPU region slots reserved per task (Non-Secure) */
#define STK_TZ_SECURE                0U     /* ARM TrustZone Secure binary build flag */
#define STK_TZ_NON_SECURE            0U     /* ARM TrustZone Non-Secure binary build flag */


// =============================================================================
// SYMMETRIC MULTI-PROCESSING (SMP) & CORE SETTINGS
// =============================================================================

#define STK_ARCH_CPU_COUNT           1U     /* Number of physical CPU cores available */
#define STK_CORE_FREQ_UNIFIED        1U     /* Set to 1 if all CPU cores have the same frequency */


// =============================================================================
// STACK, SAFETY & RUNTIME GUARDS
// =============================================================================

#define STK_STRICT_COMPLIANCY        0U     /* Allow performance workarounds that break safety standards */
#define STK_STACK_GUARD              1U     /* Check stack health on every context switch */
#define STK_STACK_NEEDS_TASK_ID      0U     /* Include task ID in Stack descriptor for tracing/MPU */
#define STK_STACK_SIZE_MIN           32U    /* Minimum stack size in elements of Word */
#define STK_SLEEP_TRAP_STACK_SIZE    32U    /* Stack size for the sleep trap in elements of Word */
#define STK_CS_NESTINGS_MAX          16U    /* Maximum allowable recursion depth for critical sections */


// =============================================================================
// SYNCHRONIZATION & DEBUG TRACING
// =============================================================================

#define STK_SYNC_DEBUG_NAMES         0U     /* Enable debug string names for synchronization primitives */
#define STK_SEGGER_SYSVIEW           0U     /* Enable SEGGER SystemView tracing integration */


// =============================================================================
// SOFTWARE TIMERS & MEMORY ALLOCATOR
// =============================================================================

#define STK_TIMER_THREADS_COUNT      1U     /* Number of threads handling timers in TimerHost */
#define STK_TIMER_HANDLER_STACK_SIZE 256U   /* Stack size of timer handler tasks */
#define STK_TIMER_COMMAND_QUEUE_SIZE 32U    /* Capacity of TimerHost command queue */
#define STK_MEMORY_PLACEMENT_NEW     1U     /* Enable C++ placement new operator support in memory allocator */


// =============================================================================
// C INTERFACE (stk_c.h, stk_c_time.h, stk_c_memory.h)
// =============================================================================
// All values below are optional, the defaults are shown. Uncomment to override.

//#define STK_C_KERNEL_MAX_TASKS         (4)    /* Maximum number of tasks per kernel instance */
//#define STK_C_CPU_COUNT                (1)    /* Number of kernel instances / CPU cores */

// Kernel type per core. If STK_C_KERNEL_TYPE_CPU_0 is not defined, the default is used:
//#define STK_C_KERNEL_TYPE_CPU_0 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, sched::SwitchStrategyRR, PlatformDefault>
//#define STK_C_KERNEL_TYPE_CPU_1 Kernel<KERNEL_STATIC, STK_C_KERNEL_MAX_TASKS, sched::SwitchStrategyRR, PlatformDefault>

//#define STK_C_TIMER_COMMAND_QUEUE_MAX  (32U)  /* Capacity of the TimerHost command queue */
//#define STK_C_TIMER_HANDLER_STACK_SIZE (256U) /* Stack size of the TimerHost handler tasks */
//#define STK_C_TIMER_THREADS_COUNT      (1U)   /* Number of TimerHost handler tasks */
//#define STK_C_BLOCKPOOL_MAX            (8U)   /* Maximum number of concurrent block pools */
//#define STK_C_BYTEPOOL_MAX             (8U)   /* Maximum number of concurrent byte pools */
//#define STK_C_MEMORY_DEFAULT_ALLOCATOR (0)    /* 0 = malloc/free, 1 = deterministic ISR-safe allocator */
//#define STK_C_SEM_VALUE_TYPE           uint16_t /* Semaphore counter type */
//#define STK_C_EF_VALUE_TYPE            uint32_t /* Event flags word type */

#endif /* STK_CONFIG_H_ */