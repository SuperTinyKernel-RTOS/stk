/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk.h>
#include "stk_c_time.h"         // must be included AFTER stk.h
#include <time/stk_time.h>      // must be included AFTER stk_c_time.h, see notes in stk_c_time.h
#include <memory/stk_memory.h>  // for placement new

// Note: stk_c_time.h (C++ section) maps STK_C_TIMER_MAX to STK_TIMER_COMMAND_QUEUE_SIZE, and
// STK_C_TIMER_THREADS_COUNT / STK_C_TIMER_HANDLER_STACK_SIZE to their C++ counterparts, before
// it includes time/stk_time.h.

using namespace stk;
using namespace stk::time;

// -----------------------------------------------------------------------------
// stk_timer_t - concrete Timer subclass that bridges C++ OnExpired() -> C callback
//
// The object is constructed by stk_timer_create() in caller-supplied memory
// (stk_timer_mem_t), there is no internal pool. The struct is opaque to C callers.
// -----------------------------------------------------------------------------

struct stk_timer_t final : public Timer
{
    stk_timer_t(stk_timer_callback_t callback, void *user_data)
        : m_host_handle(nullptr), m_callback(callback), m_user_data(user_data)
    {
        STK_ASSERT(callback != nullptr);
    }

    // Update the host association without touching callback/user_data.
    // Called every time the timer is rearmed so the expiration callback
    // always receives the correct host pointer.
    void SetHostHandle(stk_timerhost_t *host_handle) { m_host_handle = host_handle; }

    void OnExpired(TimerHost *host) override
    {
        STK_UNUSED(host);

        if (m_callback != nullptr)
        {
            m_callback(m_host_handle, this, m_user_data);
        }
    }

private:
    stk_timerhost_t     *m_host_handle; //!< C-level host, forwarded to the callback
    stk_timer_callback_t m_callback;
    void                *m_user_data;
};

// Interop-private helpers
namespace stk {
namespace interop_c_helper {

extern void InitializeTimerHost(stk_kernel_t *kernel, stk::time::TimerHost *th, EAccessMode amode);
extern EAddResult InitializeTimerHostEx(stk_kernel_t *kernel, stk::time::TimerHost *th, EAccessMode amode,
    const stk::time::TimerHost::Config &cfg);

} // interop_c_helper
} // stk

// maps the C++ command result to the C API result
static __stk_forceinline stk_timer_result_t ToCTimerResult(TimerHost::EResult r)
{
    // C result enum must stay in sync with its C++ counterpart (values are cast directly)
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_CANCELLED)     == static_cast<int32_t>(TimerHost::RES_CANCELLED),     "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_INVALID_PARAM) == static_cast<int32_t>(TimerHost::RES_INVALID_PARAM), "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_QUEUE_FULL)    == static_cast<int32_t>(TimerHost::RES_QUEUE_FULL),    "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_NOT_READY)     == static_cast<int32_t>(TimerHost::RES_NOT_READY),     "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_FAIL)          == static_cast<int32_t>(TimerHost::RES_FAIL),          "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_OK)            == static_cast<int32_t>(TimerHost::RES_OK),            "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_ACTIVE)        == static_cast<int32_t>(TimerHost::RES_ACTIVE),        "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_INACTIVE)      == static_cast<int32_t>(TimerHost::RES_INACTIVE),      "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_NOT_PERIODIC)  == static_cast<int32_t>(TimerHost::RES_NOT_PERIODIC),  "stk_timer_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_TIMER_RES_QUEUED)        == static_cast<int32_t>(TimerHost::RES_QUEUED),        "stk_timer_result_t mismatch");

    return static_cast<stk_timer_result_t>(static_cast<int32_t>(r));
}

// -----------------------------------------------------------------------------
// stk_timerhost_t — wraps a TimerHost instance
//
// One instance per CPU core, held in s_TimerHosts[]. The struct is opaque to
// C callers.
// -----------------------------------------------------------------------------

struct stk_timerhost_t
{
    TimerHost handle;
};

// Static pool: one host per core, indexed by core_nr (0 ... STK_C_CPU_COUNT-1).
static stk_timerhost_t s_TimerHosts[STK_C_CPU_COUNT];

// =============================================================================
// C-interface
// =============================================================================
extern "C" {

// -----------------------------------------------------------------------------
// TimerHost
// -----------------------------------------------------------------------------

stk_timerhost_t *stk_timerhost_get(uint8_t core_nr)
{
    stk_timerhost_t *result = nullptr;

    if (core_nr < STK_C_CPU_COUNT)
    {
        result = &s_TimerHosts[core_nr];
    }

    return result;
}

void stk_timerhost_init(stk_timerhost_t *host,
                        stk_kernel_t    *kernel,
                        bool             privileged)
{
    STK_ASSERT(host   != nullptr);
    STK_ASSERT(kernel != nullptr);
    
    interop_c_helper::InitializeTimerHost(kernel, &host->handle, 
        (privileged ? ACCESS_PRIVILEGED : ACCESS_USER));
}
    
void stk_timerhost_config_init(stk_timerhost_config_t *cfg)
{
    STK_ASSERT(cfg != nullptr);

    cfg->tick_weight        = static_cast<stk_weight_t>(DEFAULT_WEIGHT);
    cfg->handler_weight     = static_cast<stk_weight_t>(DEFAULT_WEIGHT);
    cfg->tick_stack         = nullptr;
    cfg->tick_stack_size    = 0U;
    cfg->handler_stack_size = 0U;

    for (size_t i = 0U; i < STK_C_TIMER_THREADS_COUNT; ++i)
    {
        cfg->handler_stack[i] = nullptr;
    }
}

stk_add_result_t stk_timerhost_init_ex(stk_timerhost_t              *host,
                                       stk_kernel_t                 *kernel,
                                       bool                          privileged,
                                       const stk_timerhost_config_t *cfg)
{
    // C result enum must stay in sync with its C++ counterpart (values are cast directly)
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_ADD_FAIL_INVALID)   == static_cast<int32_t>(stk::ADD_FAIL_INVALID),   "stk_add_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_ADD_FAIL_DUPLICATE) == static_cast<int32_t>(stk::ADD_FAIL_DUPLICATE), "stk_add_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_ADD_FAIL_POOL_FULL) == static_cast<int32_t>(stk::ADD_FAIL_POOL_FULL), "stk_add_result_t mismatch");
    STK_STATIC_ASSERT_DESC(static_cast<int32_t>(STK_ADD_OK)             == static_cast<int32_t>(stk::ADD_OK),             "stk_add_result_t mismatch");

    STK_ASSERT(host   != nullptr);
    STK_ASSERT(kernel != nullptr);

    TimerHost::Config tcfg; // defaults

    if (cfg != nullptr)
    {
        tcfg.tick_weight        = static_cast<Weight>(cfg->tick_weight);
        tcfg.handler_weight     = static_cast<Weight>(cfg->handler_weight);
        tcfg.tick_stack         = cfg->tick_stack;
        tcfg.tick_stack_size    = cfg->tick_stack_size;
        tcfg.handler_stack_size = cfg->handler_stack_size;

        for (size_t i = 0U; i < STK_C_TIMER_THREADS_COUNT; ++i)
        {
            tcfg.handler_stack[i] = cfg->handler_stack[i];
        }
    }

    return static_cast<stk_add_result_t>(interop_c_helper::InitializeTimerHostEx(kernel, &host->handle,
        (privileged ? ACCESS_PRIVILEGED : ACCESS_USER), tcfg));
}

void stk_timerhost_init_polled(stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    host->handle.InitializePolled();
}

stk_timeout_t stk_timerhost_process(stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    return static_cast<stk_timeout_t>(host->handle.Process());
}

stk_timer_result_t stk_timerhost_shutdown(stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    return ToCTimerResult(host->handle.Shutdown());
}

bool stk_timerhost_is_empty(const stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    return host->handle.IsEmpty();
}

size_t stk_timerhost_get_size(const stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    return host->handle.GetSize();
}

int64_t stk_timerhost_get_time_now(const stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    return static_cast<int64_t>(host->handle.GetTimeNow());
}

// -----------------------------------------------------------------------------
// Timer lifecycle
// -----------------------------------------------------------------------------

stk_timer_t *stk_timer_create(stk_timer_mem_t *const membuf,
                              stk_timer_callback_t   callback,
                              void                  *user_data)
{
    STK_ASSERT(membuf != nullptr);
    STK_ASSERT(callback != nullptr);

    stk_timer_t *result = nullptr;
    if ((membuf != nullptr) && (callback != nullptr))
    {
        result = ConstructIn<stk_timer_t>(*membuf, callback, user_data);
    }

    return result;
}

void stk_timer_destroy(stk_timer_t *tmr)
{
    if (tmr != nullptr)
    {
        // destroying an active timer is a programming error
        STK_ASSERT(!tmr->IsActive());

        tmr->~stk_timer_t();
    }
}

// -----------------------------------------------------------------------------
// Timer control helpers
//
// Every control function that rearms a timer also refreshes the host handle
// stored in the timer so the C callback always receives the correct host.
// -----------------------------------------------------------------------------

stk_timer_result_t stk_timer_start(stk_timerhost_t *host,
                                   stk_timer_t     *tmr,
                                   uint32_t         delay,
                                   uint32_t         period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->SetHostHandle(host);

    return ToCTimerResult(host->handle.Start(*tmr, delay, period));
}

stk_timer_result_t stk_timer_stop(stk_timerhost_t *host, stk_timer_t *tmr)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return ToCTimerResult(host->handle.Stop(*tmr));
}

stk_timer_result_t stk_timer_reset(stk_timerhost_t *host, stk_timer_t *tmr)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return ToCTimerResult(host->handle.Reset(*tmr));
}

stk_timer_result_t stk_timer_restart(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->SetHostHandle(host);

    return ToCTimerResult(host->handle.Restart(*tmr, delay, period));
}

stk_timer_result_t stk_timer_start_or_reset(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t delay, uint32_t period_ticks)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association (harmless if timer is already active on host)
    tmr->SetHostHandle(host);

    return ToCTimerResult(host->handle.StartOrReset(*tmr, delay, period_ticks));
}

stk_timer_result_t stk_timer_set_period(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t period_ticks)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return ToCTimerResult(host->handle.SetPeriod(*tmr, period_ticks));
}

// -----------------------------------------------------------------------------
// Timer control - synchronous API
// -----------------------------------------------------------------------------

stk_timer_result_t stk_timer_start_sync(stk_timerhost_t *host,
                                        stk_timer_t     *tmr,
                                        uint32_t         delay,
                                        uint32_t         period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->SetHostHandle(host);

    return ToCTimerResult(host->handle.StartSync(*tmr, delay, period));
}

stk_timer_result_t stk_timer_activate_sync(stk_timerhost_t *host, stk_timer_t *tmr)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->SetHostHandle(host);

    return ToCTimerResult(host->handle.ActivateSync(*tmr));
}

stk_timer_result_t stk_timer_stop_sync(stk_timerhost_t *host, stk_timer_t *tmr)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return ToCTimerResult(host->handle.StopSync(*tmr));
}

stk_timer_result_t stk_timer_restart_sync(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->SetHostHandle(host);

    return ToCTimerResult(host->handle.RestartSync(*tmr, delay, period));
}

stk_timer_result_t stk_timer_reset_sync(stk_timerhost_t *host, stk_timer_t *tmr)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return ToCTimerResult(host->handle.ResetSync(*tmr));
}

stk_timer_result_t stk_timer_set_period_sync(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t period_ticks)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return ToCTimerResult(host->handle.SetPeriodSync(*tmr, period_ticks));
}

stk_timer_result_t stk_timer_change_sync(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->SetHostHandle(host);

    return ToCTimerResult(host->handle.ChangeSync(*tmr, delay, period));
}

stk_timer_result_t stk_timer_query_sync(stk_timerhost_t *host, stk_timer_t *tmr, stk_timer_info_t *info)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);
    STK_ASSERT(info != nullptr);

    TimerHost::TimerInfo ti = {};
    const TimerHost::EResult res = host->handle.QuerySync(*tmr, ti);

    if (res == TimerHost::RES_OK)
    {
        info->active    = ti.active;
        info->remaining = ti.remaining;
        info->delay     = ti.delay;
        info->period    = ti.period;
    }

    return ToCTimerResult(res);
}

// -----------------------------------------------------------------------------
// Timer query
// -----------------------------------------------------------------------------

bool stk_timer_is_active(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return tmr->IsActive();
}

uint32_t stk_timer_get_period(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return tmr->GetPeriod();
}

int64_t stk_timer_get_deadline(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return static_cast<int64_t>(tmr->GetDeadline());
}

int64_t stk_timer_get_timestamp(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return static_cast<int64_t>(tmr->GetTimestamp());
}

uint32_t stk_timer_get_remaining_ticks(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return tmr->GetRemainingTicks();
}

// -----------------------------------------------------------------------------
// PeriodicTrigger
// -----------------------------------------------------------------------------

struct stk_periodic_trigger_t
{
    stk_periodic_trigger_t(uint32_t period, bool start) : handle(period, start)
    {}

    PeriodicTrigger handle;
};

stk_periodic_trigger_t *stk_periodic_trigger_create(stk_periodic_trigger_mem_t *const membuf,
                                                    uint32_t                    period_ticks,
                                                    bool                        started)
{
    STK_ASSERT(membuf != nullptr);

    stk_periodic_trigger_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_periodic_trigger_t>(*membuf, period_ticks, started);
    }

    return result;
}

void stk_periodic_trigger_destroy(stk_periodic_trigger_t *const trig)
{
    if (trig != nullptr)
    {
        trig->~stk_periodic_trigger_t();
    }
}

bool stk_periodic_trigger_poll(stk_periodic_trigger_t *trig)
{
    STK_ASSERT(trig != nullptr);

    return trig->handle.Poll();
}

void stk_periodic_trigger_set_period(stk_periodic_trigger_t *trig, uint32_t period_ticks)
{
    STK_ASSERT(trig != nullptr);

    trig->handle.SetPeriod(period_ticks);
}

void stk_periodic_trigger_restart(stk_periodic_trigger_t *trig)
{
    STK_ASSERT(trig != nullptr);

    trig->handle.Restart();
}

uint32_t stk_periodic_trigger_get_period(const stk_periodic_trigger_t *trig)
{
    STK_ASSERT(trig != nullptr);

    return static_cast<uint32_t>(trig->handle.GetPeriod());
}

// -----------------------------------------------------------------------------
// C++ instance access
// -----------------------------------------------------------------------------

TimerHost *stk_timerhost_get_instance(stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    return &host->handle;
}

Timer *stk_timer_get_instance(stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return tmr;
}

PeriodicTrigger *stk_periodic_trigger_get_instance(stk_periodic_trigger_t *trig)
{
    STK_ASSERT(trig != nullptr);

    return &trig->handle;
}

// =============================================================================
} // extern "C"
// =============================================================================
