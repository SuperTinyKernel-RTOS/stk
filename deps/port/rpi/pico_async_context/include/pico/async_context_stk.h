/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Ported to SuperTinyKernel(TM) (STK) native API.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_ASYNC_CONTEXT_STK_H
#define _PICO_ASYNC_CONTEXT_STK_H

/** \file pico/async_context_stk.h
 *  \defgroup async_context_stk async_context_stk
 *  \ingroup pico_async_context
 *
 * \brief async_context_stk provides an implementation of \ref async_context that handles
 * asynchronous work in a separate task, scheduled directly by SuperTinyKernel (STK)'s native
 * scheduler.
 *
 * This backend is built entirely on STK's own C++ primitives:
 *   - stk::sync::Mutex             (already recursive)  for the context's lock
 *   - stk::sync::Semaphore         (binary, (0,1))       for worker wakeups and execute_sync()'s rendezvous
 *   - stk::time::TimerHost::Timer                        for the "wake at next due time" timer
 *   - stk::ITask                                         for the worker task itself
 *
 * Because STK is a C++-only API, this header (and its implementation file) are C++, unlike
 * some of pico_async_context's other, plain-C backends. Public entry points still use the same
 * snake_case / bool-returning style as the rest of pico_async_context for consistency.
 *
 * \note STK's synchronization primitives (Mutex, Semaphore, Timer) are plain C++ objects with
 *       real constructors/destructors - do NOT memset() an async_context_stk_t. It is safe (and
 *       required) to simply default-construct it, e.g.:
 *       \code
 *       static async_context_stk_t ctx; // or on the stack / heap, all fine
 *       \endcode
 *
 * \note This port takes the application's own stk::IKernel and stk::time::TimerHost by pointer,
 *       rather than lazily creating private ones under the hood. Both must already be
 *       constructed and Initialize()'d (Start() need not have been called yet) before calling
 *       async_context_stk_init(). This avoids any hidden global state and lets the application
 *       share one TimerHost across many timers, as STK intends.
 */

#include "pico/async_context.h"

// STK includes
#include "stk.h"
#include "sync/stk_sync.h"
#include "time/stk_time.h"

// Version of STK interface.
#define ASYNC_CONTEXT_STK_VERSION (0x20260801)

#ifndef ASYNC_CONTEXT_DEFAULT_STK_TASK_WEIGHT
// Meaning depends on the application's chosen ITaskSwitchStrategy:
//  - SwitchStrategyFixedPriority (FP32-style): treated as a fixed priority level (0..31).
//  - SwitchStrategySmoothWeightedRoundRobin:   treated as a proportional CPU share.
// The default (idle weight + 4) gives the worker task a modest priority boost above idle
// without approaching time-critical priorities, for the fixed-priority case.
#define ASYNC_CONTEXT_DEFAULT_STK_TASK_WEIGHT (stk::DEFAULT_WEIGHT + 4U)
#endif

#ifndef ASYNC_CONTEXT_DEFAULT_STK_TASK_STACK_SIZE
#define ASYNC_CONTEXT_DEFAULT_STK_TASK_STACK_SIZE (1024U)
#endif

// The pico async_context_type_t enum (in pico/async_context.h) needs an entry identifying this
// backend. Add ASYNC_CONTEXT_STK to that enum (alongside the other backends already defined
// there) in your tree; a fallback definition is provided here so this header is self-contained
// if you haven't.
#ifndef ASYNC_CONTEXT_STK
#define ASYNC_CONTEXT_STK (4U)
#endif

// Number of tasks used by async_context.
#define ASYNC_CONTEXT_STK_TASKS (1U)

typedef struct async_context_stk async_context_stk_t;

/**
 * \brief Configuration object for async_context_stk instances.
 */
typedef struct async_context_stk_config
{
    /**
     * \brief Kernel the worker task is scheduled on.
     * \note  Must be non-null, already Initialize()'d. Must not have been Start()'d in a way
     *        that forbids further AddTask() calls (KERNEL_DYNAMIC allows AddTask() any time).
     */
    stk::IKernel *kernel;

    /**
     * \brief TimerHost used to schedule the context's "next due" wakeup.
     * \note  Must be non-null, already Initialize()'d against the same \a kernel. May (and
     *        typically should) be shared with other timers in the application - TimerHost is
     *        designed for that; async_context_stk only ever has at most one active timer.
     */
    stk::time::TimerHost *timer_host;

    /**
     * \brief Scheduling weight for the async_context task (see ASYNC_CONTEXT_DEFAULT_STK_TASK_WEIGHT).
     */
    stk::Weight task_weight;

    /**
     * \brief Hardware access mode for the async_context task.
     */
    stk::EAccessMode task_access_mode;

    /**
     * \brief Pointer to stack memory for the async_context task (STK has no dynamic task
     *        creation path; the caller always owns the stack storage).
     */
    stk::Word *task_stack;

    /**
     * \brief Stack size for the async_context task, in stk::Word elements.
     */
    size_t task_stack_size;
} async_context_stk_config_t;

struct async_context_stk
{
    async_context_t core;

    /**
     * \brief Worker task that drains due callbacks/workers under the context's lock.
     * \note  Public only so its (fixed) storage lives inline in async_context_stk_t with no
     *        heap allocation; treat as an implementation detail. Bound and added to the kernel
     *        by async_context_stk_init().
     */
    class Worker : public stk::ITask
    {
    public:
        void Bind(async_context_stk *owner, stk::Word *stack, size_t stack_size,
                  stk::Weight weight, stk::EAccessMode mode);

        // ---- stk::ITask / stk::IStackMemory ----
        const stk::Word *GetStack()      const override { return m_stack; }
        size_t            GetStackSize() const override { return m_stack_size; }
        stk::EAccessMode  GetAccessMode()const override { return m_mode; }
        stk::Weight       GetWeight()    const override { return m_weight; }
        const char       *GetTraceName() const override { return "async_ctx_stk"; }
        void OnExit() override;

    private:
        void Run() override; // defined in async_context_stk.cpp (needs complete async_context_stk)

        async_context_stk *m_owner      = nullptr;
        stk::Word         *m_stack      = nullptr;
        size_t             m_stack_size = 0U;
        stk::Weight        m_weight     = stk::DEFAULT_WEIGHT;
        stk::EAccessMode   m_mode       = stk::ACCESS_PRIVILEGED;
    };

    /**
     * \brief One-shot timer used to wake the worker task at the next due time reported by
     *        async_context_base_execute_once(). Re-armed via TimerHost::Restart(), which
     *        atomically stops-and-restarts the timer with a fresh delay, every time the worker
     *        task processes work.
     */
    class WakeTimer : public stk::time::TimerHost::Timer
    {
    public:
        void Bind(async_context_stk *owner) { m_owner = owner; }

    private:
        void OnExpired(stk::time::TimerHost *host) override;
        async_context_stk *m_owner = nullptr;
    };

    Worker                task;
    WakeTimer             timer;

    stk::sync::Mutex      lock_mutex;                // recursive by construction
    stk::sync::Semaphore  task_wake_sem{0U, 1U};     // wakes the worker task's processing loop
    stk::sync::Semaphore  work_needed_sem{0U, 1U};   // wakes async_context_wait_for_work_until()
    stk::sync::Semaphore  task_complete_sem{0U, 1U}; // signaled once the worker task has exited

    stk::time::TimerHost *timer_host       = nullptr;
    stk::IKernel         *kernel           = nullptr;

    volatile stk::TId     task_tid         = stk::TID_NONE; // set by Worker::Run() once it starts
    uint8_t               nesting          = 0U;
    volatile bool         task_should_exit = false;
    bool                  initialized      = false;
};

/*!
 * \brief Initialize an async_context_stk instance using the specified configuration
 * \ingroup async_context_stk
 *
 * If this method succeeds (returns true), then the async_context is available for use
 * and can be de-initialized by calling async_context_deinit().
 *
 * \param self a pointer to an async_context_stk structure to initialize (default-constructed,
 *             NOT memset - see the class-level note above)
 * \param config the configuration object specifying characteristics for the async_context
 * \return true if initialization is successful, false otherwise
 */
bool async_context_stk_init(async_context_stk_t *self, async_context_stk_config_t *config);

/*!
 * \brief Return a copy of the default configuration object used by \ref async_context_stk_init_with_defaults()
 * \ingroup async_context_stk
 *
 * The caller can then modify just the settings it cares about (in particular \a kernel,
 * \a timer_host, \a task_stack and \a task_stack_size, which have no sensible default), and
 * call \ref async_context_stk_init().
 * \return the default configuration object
 */
static inline async_context_stk_config_t async_context_stk_default_config(void)
{
    async_context_stk_config_t config = {};
    config.kernel           = nullptr;
    config.timer_host       = nullptr;
    config.task_weight      = ASYNC_CONTEXT_DEFAULT_STK_TASK_WEIGHT;
    config.task_access_mode = stk::ACCESS_PRIVILEGED;
    config.task_stack       = nullptr;
    config.task_stack_size  = ASYNC_CONTEXT_DEFAULT_STK_TASK_STACK_SIZE;
    return config;
}

/*!
 * \brief Initialize an async_context_stk instance with default values plus the mandatory
 *        kernel / timer_host / stack resources.
 * \ingroup async_context_stk
 *
 * \param self a pointer to an async_context_stk structure to initialize
 * \param kernel already-Initialize()'d kernel to schedule the worker task on
 * \param timer_host already-Initialize()'d TimerHost (bound to the same kernel) used to
 *        schedule the context's wakeups
 * \param task_stack stack memory for the worker task, at least ASYNC_CONTEXT_DEFAULT_STK_TASK_STACK_SIZE words
 * \param task_stack_size size of \a task_stack, in stk::Word elements
 * \return true if initialization is successful, false otherwise
 */
static inline bool async_context_stk_init_with_defaults(async_context_stk_t *self,
        stk::IKernel *kernel,
        stk::time::TimerHost *timer_host,
        stk::Word *task_stack,
        size_t task_stack_size)
{
    async_context_stk_config_t config = async_context_stk_default_config();
    config.kernel          = kernel;
    config.timer_host      = timer_host;
    config.task_stack       = task_stack;
    config.task_stack_size = task_stack_size;
    return async_context_stk_init(self, &config);
}

#endif
