/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com> (STK port)
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_LWIP_STK_H
#define _PICO_LWIP_STK_H

#include "pico.h"
#include "pico/async_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file pico/lwip_stk.h
* \defgroup pico_lwip_stk pico_lwip_stk
* \ingroup pico_lwip
* \brief Glue library for integrating lwIP in \c NO_SYS=0 mode with the SDK, backed by
*        SuperTinyKernel(TM) RTOS (STK)
*
* Simple \c init and \c deinit are all that is required to hook up lwIP (with full blocking
* API support) via an \ref async_context instance, on top of the STK \c sys_arch port
* (\c sys_arch.h / \c sys_arch.cpp).
*
* \note This library requires the STK \c sys_arch port to be in use, compiled with
* \c LWIP_STK_CUSTOM_CORE_LOCKING=1 so that lwIP's tcpip core lock is delegated to
* \ref pico_lwip_custom_lock_tcpip_core / \ref pico_lwip_custom_unlock_tcpip_core (defined by
* this library) instead of sys_arch.cpp's own independent \c stk::sync::Mutex - making the
* tcpip core lock *be* the async_context's own lock, so code already running inside an
* async_context callback doesn't deadlock taking it again.
*
* \note An \c stk::IKernel instance must already have been registered via
* \c sys_arch_set_kernel() before \ref lwip_stk_init is called: \c tcpip_init() spawns lwIP's
* tcpip thread via \c sys_thread_new(), which needs a kernel to add that task to.
*/

/*! \brief Initializes lwIP (NO_SYS=0 mode) support for STK using the provided async_context
 *  \ingroup pico_lwip_stk
 *
 * If the initialization succeeds, \ref lwip_stk_deinit() can be called to shutdown lwIP support
 *
 * \param context the async_context instance that provides the abstraction for handling
 * asynchronous work.
 *
 * \return true if the initialization succeeded
*/
bool lwip_stk_init(async_context_t *context);

/*! \brief De-initialize lwIP (NO_SYS=0 mode) support for STK
 *  \ingroup pico_lwip_stk
 *
 * Note that since lwIP may only be initialized once, and doesn't itself provide a shutdown mechanism, lwIP
 * itself may still consume resources.
 *
 * It is however safe to call \ref lwip_stk_init again later.
 *
 * \param context the async_context the lwip_stk support was added to via \ref lwip_stk_init
*/
void lwip_stk_deinit(async_context_t *context);

#ifdef __cplusplus
}
#endif
#endif
