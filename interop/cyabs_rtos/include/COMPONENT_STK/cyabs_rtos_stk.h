/***********************************************************************************************//**
 * \file cyabs_rtos_stk.h
 *
 * \brief
 * Kernel control of the SuperTinyKernel (STK) port of the Infineon RTOS abstraction layer.
 *
 * The abstraction layer has no "start the scheduler" call: every RTOS is started by the application
 * with its own API (vTaskStartScheduler(), tx_kernel_enter(), osKernelStart()). For STK that is
 * this header. The port owns the kernel instance, its configuration is in cyabs_rtos_stk_config.h.
 *
 * Typical use:
 * \code
 * static void app_init(void)               // plays the role of tx_application_define()
 * {
 *     cy_rtos_thread_create(&g_main_thread, main_thread, "main", NULL, 4096,
 *                           CY_RTOS_PRIORITY_NORMAL, 0);
 * }
 *
 * int main(void)
 * {
 *     board_init();
 *     cy_rtos_stk_kernel_enter(app_init);  // does not return while threads are alive
 *     for (;;) { }
 * }
 * \endcode
 * or, FreeRTOS style, create everything first and start afterwards:
 * \code
 *     cy_rtos_stk_kernel_init();
 *     cy_rtos_thread_create(...);
 *     cy_rtos_stk_kernel_start();
 * \endcode
 *
 ***************************************************************************************************
 * \copyright
 * Licensed under the Apache License, Version 2.0 (same license as the abstraction-rtos library).
 * SPDX-License-Identifier: Apache-2.0
 **************************************************************************************************/

#ifndef CYABS_RTOS_STK_H
#define CYABS_RTOS_STK_H

#include "cy_result.h"
#include "cyabs_rtos_stk_config.h"
#include "cyabs_rtos_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Values of cy_rtos_last_error() reported by this port together with CY_RTOS_GENERAL_ERROR. */
#define CY_RTOS_STK_ERR_NONE            (0U)    /**< no error */
#define CY_RTOS_STK_ERR_CONTEXT         (1U)    /**< not allowed in this context (ISR, scheduler not
                                                     running or locked, not a thread created by
                                                     cy_rtos_thread_create()) */
#define CY_RTOS_STK_ERR_WAIT_CANCELED   (2U)    /**< the wait was cancelled by the kernel */
#define CY_RTOS_STK_ERR_WAIT_FAILED     (3U)    /**< the kernel rejected the wait / sleep */
#define CY_RTOS_STK_ERR_LIMIT           (4U)    /**< semaphore is already at its maximum count */
#define CY_RTOS_STK_ERR_NOT_OWNER       (5U)    /**< mutex is not owned by the calling thread */
#define CY_RTOS_STK_ERR_KERNEL_STATE    (6U)    /**< kernel is not initialized (see
                                                     cy_rtos_stk_kernel_init()) */
#define CY_RTOS_STK_ERR_DUPLICATE       (7U)    /**< thread or its stack is already known to the
                                                     kernel */
#define CY_RTOS_STK_ERR_TIMER_HOST      (8U)    /**< the timer host rejected the command */
#define CY_RTOS_STK_ERR_NOT_STARTED     (9U)    /**< the kernel never ran the target thread */

/** Initializes the kernel (and the software timer host, see CY_RTOS_STK_USE_TIMERS). Must be called
 *  once before the first cy_rtos_thread_create() / cy_rtos_timer_init(). Calling it again is a no-op.
 *
 * @return CY_RSLT_SUCCESS, or CY_RTOS_NO_MEMORY if the kernel has no task slots for the timer host.
 */
cy_rslt_t cy_rtos_stk_kernel_init(void);

/** Starts the scheduler. Returns only when all threads have exited and no timer host is running
 *  (with CY_RTOS_STK_USE_TIMERS = 1 it effectively does not return).
 *  cy_rtos_stk_kernel_init() must have been called before.
 */
void cy_rtos_stk_kernel_start(void);

/** Initializes the kernel, calls \p app_init to create the first threads, and starts the scheduler.
 *
 * @param[in] app_init Called after the kernel is initialized and before it is started; may be NULL.
 *
 * @return CY_RTOS_NO_MEMORY / error of cy_rtos_stk_kernel_init(); CY_RSLT_SUCCESS after the
 *         scheduler has returned.
 */
cy_rslt_t cy_rtos_stk_kernel_enter(void (* app_init)(void));

#ifdef __cplusplus
} // extern "C"

namespace stk
{
class IKernel;
}

/** Gives C++ code access to the STK kernel owned by the port, to use native STK services (for
 *  example stk::IKernel::GetTaskState()) next to the abstraction layer. */
stk::IKernel* cy_rtos_stk_get_kernel(void);
#endif

#endif /* CYABS_RTOS_STK_H */
