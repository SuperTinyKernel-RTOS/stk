/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Ported to SuperTinyKernel(TM) (STK) native API.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_CYW43_ARCH_STK_H
#define _PICO_CYW43_ARCH_STK_H

/** \file pico/cyw43_arch_stk.h
 *  \defgroup cyw43_arch_stk cyw43_arch_stk
 *  \ingroup pico_cyw43_arch
 *
 * \brief STK-specific entry point for pico_cyw43_arch's default async_context.
 *
 * async_context_stk requires an already-Initialize()'d stk::IKernel and a stk::time::TimerHost
 * Initialize()'d against that same kernel (see pico/async_context_stk.h), rather than lazily
 * creating a private timer/task under the hood - and STK has no global registry for either.
 * cyw43_arch_init_default_async_context() therefore can't discover them on its own; the
 * application must supply them once, up front, via cyw43_arch_stk_preinit().
 */

#include "pico/async_context_stk.h"

/*!
 * \brief Supply the kernel/timer_host that cyw43_arch_init_default_async_context() should bind
 *        the cyw43 worker task and its wake timer to.
 * \ingroup cyw43_arch_stk
 *
 * Must be called before cyw43_arch_init() (or cyw43_arch_init_default_async_context(), if called
 * directly) - but only if you intend to rely on the *default* async_context. If you construct and
 * async_context_stk_init() your own async_context_stk_t and hand it to cyw43_arch via
 * cyw43_arch_set_async_context() before calling cyw43_arch_init(), this call is unnecessary.
 *
 * \param kernel a kernel already Initialize()'d (Start() need not have been called yet)
 * \param timer_host a TimerHost already Initialize()'d against \a kernel
 * \note  Neither pointer is copied or validated here; both must remain valid for the lifetime of
 *        the default async_context (i.e. until cyw43_arch_deinit() completes).
 */
void cyw43_arch_stk_preinit(stk::IKernel *kernel, stk::time::TimerHost *timer_host);

#endif
