/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Ported to SuperTinyKernel(TM) (STK) native API.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cassert>

#include "pico/async_context_stk.h"
#include "pico/async_context_base.h"
#include "pico/sync.h" // get_core_num()

// -----------------------------------------------------------------------------
// Forward declarations
//
// Unlike the C original (which could get away with a "static const async_context_type_t
// template;" tentative declaration followed later by its definition), C++ requires a const
// namespace-scope object of a type with no user-provided default constructor to be initialized
// at its one and only definition. So instead we forward-declare every handler function used by
// the type table, define the table itself once (right here, near the top), and then provide the
// function bodies further down - taking a function's address only ever needs its prototype to
// be visible, not its definition.
//
// NOTE: field order below is assumed to match pico's async_context_type_t (as inferred from
// other pico_async_context backends' designated initializer order: type, acquire_lock_blocking,
// release_lock, lock_check, execute_sync, add_at_time_worker, remove_at_time_worker,
// add_when_pending_worker, remove_when_pending_worker, set_work_pending, poll, wait_until,
// wait_for_work_until, deinit). Verify against your local pico/async_context.h /
// pico/async_context_base.h if this ever changes upstream.
// -----------------------------------------------------------------------------
static void     async_context_stk_acquire_lock_blocking(async_context_t *self_base);
static void     async_context_stk_release_lock(async_context_t *self_base);
static void     async_context_stk_lock_check(async_context_t *self_base);
static uint32_t async_context_stk_execute_sync(async_context_t *self_base, uint32_t (*func)(void *param), void *param);
static bool     async_context_stk_add_at_time_worker(async_context_t *self_base, async_at_time_worker_t *worker);
static bool     async_context_stk_remove_at_time_worker(async_context_t *self_base, async_at_time_worker_t *worker);
static bool     async_context_stk_add_when_pending_worker(async_context_t *self_base, async_when_pending_worker_t *worker);
static bool     async_context_stk_remove_when_pending_worker(async_context_t *self_base, async_when_pending_worker_t *worker);
static void     async_context_stk_set_work_pending(async_context_t *self_base, async_when_pending_worker_t *worker);
static void     async_context_stk_wait_until(async_context_t *self_base, absolute_time_t until);
static void     async_context_stk_wait_for_work_until(async_context_t *self_base, absolute_time_t until);
static void     async_context_stk_wake_up(async_context_t *self_base);
static void     async_context_stk_deinit(async_context_t *self_base);

static const async_context_type_t s_async_context_stk_type =
{
    ASYNC_CONTEXT_STK,
    async_context_stk_acquire_lock_blocking,
    async_context_stk_release_lock,
    async_context_stk_lock_check,
    async_context_stk_execute_sync,
    async_context_stk_add_at_time_worker,
    async_context_stk_remove_at_time_worker,
    async_context_stk_add_when_pending_worker,
    async_context_stk_remove_when_pending_worker,
    async_context_stk_set_work_pending,
    0, // poll: this backend is task-based, not polling-based
    async_context_stk_wait_until,
    async_context_stk_wait_for_work_until,
    async_context_stk_deinit,
};

// -----------------------------------------------------------------------------
// Timeout conversion: converts a pico absolute_time_t deadline into an STK tick count, using
// STK's own tick resolution (stk::GetTickResolution(), microseconds/tick) rather than assuming
// any particular fixed tick period.
// -----------------------------------------------------------------------------
static stk::Timeout sensible_ticks_until(absolute_time_t until)
{
    const int64_t delay_us = absolute_time_diff_us(get_absolute_time(), until);

    if (delay_us <= 0)
    {
        return 0;
    }

    // Clamp to 60s, keeping the intermediate arithmetic (and the
    // resulting tick count) comfortably inside stk::Timeout (int32_t) range regardless of tick
    // resolution.
    static const int64_t max_delay_us = 60000000;
    const int64_t clamped_us = (delay_us > max_delay_us) ? max_delay_us : delay_us;

    const uint32_t resolution_us = stk::GetTickResolution();
    stk::Ticks ticks = 0;

    if (resolution_us != 0U)
    {
        ticks = (clamped_us + static_cast<int64_t>(resolution_us) - 1) / static_cast<int64_t>(resolution_us);
    }

    // Round up by one extra tick: rounding down to zero is wrong (may miss a needed wakeup),
    // but we also don't want to wake up only to find there is no work to do yet.
    ticks++;

    // Never collide with the WAIT_INFINITE sentinel.
    if (ticks >= static_cast<stk::Ticks>(stk::WAIT_INFINITE))
    {
        ticks = static_cast<stk::Ticks>(stk::WAIT_INFINITE) - 1;
    }

    return static_cast<stk::Timeout>(ticks);
}

// -----------------------------------------------------------------------------
// process_under_lock: drains due callbacks/workers, then re-arms (or disarms) the one-shot
// WakeTimer for whatever's due next, via TimerHost::Restart(), which atomically
// stops-and-restarts the timer with a fresh delay.
// -----------------------------------------------------------------------------
static void process_under_lock(async_context_stk_t *self)
{
#ifndef NDEBUG
    async_context_stk_lock_check(&self->core);
#endif

    bool repeat;
    do
    {
        repeat = false;
        const absolute_time_t next_time = async_context_base_execute_once(&self->core);

        if (is_at_the_end_of_time(next_time))
        {
            // No further due work: disarm the timer if it's running. Any future state change
            // goes through the lock (add_at_time_worker / set_work_pending / release_lock) and
            // will reprocess and re-arm as needed, so it's safe to fully stop here rather than
            // arming a "practically infinite" delay instead.
            if (self->timer.IsActive())
            {
                repeat = !self->timer_host->Stop(self->timer);
            }
        }
        else
        {
            const stk::Timeout ticks = sensible_ticks_until(next_time);

            if (ticks)
            {
                // One-shot re-arm with the new delay; period=0 since there's no point
                // auto-reloading for longer than the delay itself.
                repeat = !self->timer_host->Restart(self->timer, static_cast<uint32_t>(ticks), 0U);
            }
            else
            {
                repeat = true;
            }
        }
    }
    while (repeat);
}

// -----------------------------------------------------------------------------
// wake_up: wakes the worker task (and anyone blocked in wait_for_work_until()). Both the ISR
// and non-ISR paths use TrySignal() (rather than Signal()) so that a redundant wake while one is
// already pending is silently absorbed instead of asserting.
// -----------------------------------------------------------------------------
void async_context_stk_wake_up(async_context_t *self_base)
{
    async_context_stk_t *self = (async_context_stk_t *)self_base;

    if (self->task_tid == stk::TID_NONE)
    {
        return; // worker task hasn't started running yet
    }

    if (stk::hw::IsInsideISR())
    {
        self->task_wake_sem.TrySignal();
        self->work_needed_sem.TrySignal();
    }
    else if (stk::GetTid() != self->task_tid)
    {
        self->task_wake_sem.TrySignal();
        self->work_needed_sem.TrySignal();
    }
    else
    {
        // We're already running inside the worker task (e.g. release_lock() called re-entrantly
        // from within a callback): processing will happen naturally when the lock is finally
        // unlocked at nesting level 0, no need to wake ourselves.
#ifndef NDEBUG
        async_context_stk_lock_check(self_base);
#endif
    }
}

// -----------------------------------------------------------------------------
// Worker task
// -----------------------------------------------------------------------------
void async_context_stk::Worker::Bind(async_context_stk *owner, stk::Word *stack, size_t stack_size,
                                     stk::Weight weight, stk::EAccessMode mode)
{
    m_owner      = owner;
    m_stack      = stack;
    m_stack_size = stack_size;
    m_weight     = weight;
    m_mode       = mode;
}

void async_context_stk::Worker::Run()
{
    async_context_stk *self = m_owner;

    // TId is only known once the task actually starts running; record it so wake_up() can tell
    // whether it is being called from this same task (avoiding a redundant self-wake).
    self->task_tid = stk::GetTid();

    do
    {
        self->task_wake_sem.Wait(); // WAIT_INFINITE

        if (self->task_should_exit)
        {
            break;
        }

        async_context_stk_acquire_lock_blocking(&self->core);
        process_under_lock(self);
        async_context_stk_release_lock(&self->core);

        // Signal a cross-core event (SEV) to wake up any other CPU core waiting in a WFE loop
        // (e.g., waiting for spinlocks, async work, or lock release). Also sets the Event Register
        // so a core entering WFE shortly won't miss this update.
        __sev();

    }
    while (!self->task_should_exit);
}

void async_context_stk::Worker::OnExit()
{
    // Called by the kernel (KERNEL_DYNAMIC) after Run() returns, in kernel/tick context - must
    // stick to ISR-safe primitives only. Semaphore::TrySignal() qualifies.
    m_owner->task_complete_sem.TrySignal();
}

// -----------------------------------------------------------------------------
// WakeTimer
// -----------------------------------------------------------------------------
void async_context_stk::WakeTimer::OnExpired(stk::time::TimerHost * /*host*/)
{
    async_context_stk_wake_up(&m_owner->core);
}

// -----------------------------------------------------------------------------
// init / deinit
// -----------------------------------------------------------------------------
bool async_context_stk_init(async_context_stk_t *self, async_context_stk_config_t *config)
{
    if ((self == nullptr) || (config == nullptr) ||
        (config->kernel == nullptr) || (config->timer_host == nullptr) ||
        (config->task_stack == nullptr) || (config->task_stack_size == 0U))
    {
        return false;
    }

    // Do NOT memset(self, 0, sizeof(*self)): lock_mutex / *_sem / timer are live C++ objects
    // already default-constructed (with the right (0,1) binary semaphore counts, via their
    // NSDMIs) by virtue of *self existing. We only need to set the plain data fields below.
    self->core.type     = &s_async_context_stk_type;
    self->core.flags    = ASYNC_CONTEXT_FLAG_CALLBACK_FROM_NON_IRQ;
    self->core.core_num = get_core_num();

    self->kernel           = config->kernel;
    self->timer_host       = config->timer_host;
    self->nesting          = 0U;
    self->task_should_exit = false;
    self->task_tid         = stk::TID_NONE;

    self->timer.Bind(self);
    self->task.Bind(self, config->task_stack, config->task_stack_size,
                    config->task_weight, config->task_access_mode);

    self->kernel->AddTask(&self->task);

    self->initialized = true;
    return true;
}

static uint32_t end_task_func(void *param)
{
    async_context_stk_t *self = static_cast<async_context_stk_t *>(param);
    self->task_should_exit = true;
    return 0U;
}

void async_context_stk_deinit(async_context_t *self_base)
{
    async_context_stk_t *self = (async_context_stk_t *)self_base;

    if (self->initialized)
    {
        // Ask the worker task to exit via the normal execute_sync path so the request is
        // handled with the lock held, exactly like any other unit of async work; this also
        // wakes the task if it is currently blocked in task_wake_sem.Wait().
        async_context_execute_sync(self_base, end_task_func, self);

        self->task_complete_sem.Wait(); // WAIT_INFINITE, signaled by Worker::OnExit()

        if (self->timer.IsActive())
        {
            self->timer_host->Stop(self->timer);
        }

        self->initialized = false;
    }

    // No heap allocations to free: lock_mutex / *_sem / timer / task are all inline members and
    // are cleaned up normally when *self itself is destroyed by the caller.
}

// -----------------------------------------------------------------------------
// Locking
// -----------------------------------------------------------------------------
void async_context_stk_acquire_lock_blocking(async_context_t *self_base)
{
    async_context_stk_t *self = (async_context_stk_t *)self_base;
    assert(!stk::hw::IsInsideISR());

    self->lock_mutex.Lock(); // WAIT_INFINITE; stk::sync::Mutex is always recursive
    self->nesting++;
}

void async_context_stk_lock_check(async_context_t *self_base)
{
#ifndef NDEBUG
    async_context_stk_t *self = (async_context_stk_t *)self_base;
    assert(self->lock_mutex.GetOwner() == stk::GetTid());
#else
    (void)self_base;
#endif
}

struct sync_func_call_t
{
    async_when_pending_worker_t worker;
    stk::sync::Semaphore        sem{0U, 1U}; // plain object on the caller's own stack: no heap needed
    uint32_t                  (*func)(void *param);
    void                       *param;
    uint32_t                    rc;
};

static void handle_sync_func_call(async_context_t *context, async_when_pending_worker_t *worker)
{
    STK_UNUSED(context);
    sync_func_call_t *call = (sync_func_call_t *)worker;

    // execute function
    call->rc = call->func(call->param);

    call->sem.Signal();
}

uint32_t async_context_stk_execute_sync(async_context_t *self_base, uint32_t (*func)(void *param), void *param)
{
    async_context_stk_t *self = (async_context_stk_t *)self_base;
    hard_assert(self->lock_mutex.GetOwner() != stk::GetTid());

    sync_func_call_t call = {};

    call.worker.do_work = handle_sync_func_call;
    call.func           = func;
    call.param          = param;

    async_context_add_when_pending_worker(self_base, &call.worker);
    async_context_set_work_pending(self_base, &call.worker);
    call.sem.Wait(); // WAIT_INFINITE
    async_context_remove_when_pending_worker(self_base, &call.worker);

    return call.rc;
}

void async_context_stk_release_lock(async_context_t *self_base)
{
    async_context_stk_t *self = (async_context_stk_t *)self_base;
    bool do_wakeup = false;

    if (self->nesting == 1)
    {
        // Always process on outermost unlock exit, to catch cases (e.g. lwIP) where there's no
        // explicit notification when a timer is added.
        if (self->task_tid != stk::GetTid())
        {
            // Defer the wakeup until after we release the lock, otherwise it can wake the task
            // only to have it immediately block on us again.
            do_wakeup = true;
        }
        else
        {
            process_under_lock(self);
        }
    }

    --self->nesting;
    self->lock_mutex.Unlock();

    if (do_wakeup)
    {
        async_context_stk_wake_up(self_base);
    }
}

// -----------------------------------------------------------------------------
// Worker registration
// -----------------------------------------------------------------------------
bool async_context_stk_add_at_time_worker(async_context_t *self_base, async_at_time_worker_t *worker)
{
    async_context_stk_acquire_lock_blocking(self_base);
    const bool rc = async_context_base_add_at_time_worker(self_base, worker);
    async_context_stk_release_lock(self_base);
    return rc;
}

bool async_context_stk_remove_at_time_worker(async_context_t *self_base, async_at_time_worker_t *worker)
{
    async_context_stk_acquire_lock_blocking(self_base);
    const bool rc = async_context_base_remove_at_time_worker(self_base, worker);
    async_context_stk_release_lock(self_base);
    return rc;
}

bool async_context_stk_add_when_pending_worker(async_context_t *self_base, async_when_pending_worker_t *worker)
{
    async_context_stk_acquire_lock_blocking(self_base);
    const bool rc = async_context_base_add_when_pending_worker(self_base, worker);
    async_context_stk_release_lock(self_base);
    return rc;
}

bool async_context_stk_remove_when_pending_worker(async_context_t *self_base, async_when_pending_worker_t *worker)
{
    async_context_stk_acquire_lock_blocking(self_base);
    const bool rc = async_context_base_remove_when_pending_worker(self_base, worker);
    async_context_stk_release_lock(self_base);
    return rc;
}

void async_context_stk_set_work_pending(async_context_t *self_base, async_when_pending_worker_t *worker)
{
    worker->work_pending = true;
    async_context_stk_wake_up(self_base);
}

// -----------------------------------------------------------------------------
// Waiting
// -----------------------------------------------------------------------------
void async_context_stk_wait_until(async_context_t *self_base, absolute_time_t until)
{
    STK_UNUSED(self_base);
    assert(!stk::hw::IsInsideISR());
    const stk::Timeout ticks = sensible_ticks_until(until);
    stk::Sleep(ticks);
}

void async_context_stk_wait_for_work_until(async_context_t *self_base, absolute_time_t until)
{
    async_context_stk_t *self = (async_context_stk_t *)self_base;
    assert(!stk::hw::IsInsideISR());

    while (!time_reached(until))
    {
        const stk::Timeout ticks = sensible_ticks_until(until);
        if (!ticks || self->work_needed_sem.Wait(ticks))
        {
            return;
        }
    }
}
