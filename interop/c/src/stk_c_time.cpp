/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <cstddef> // for std::size_t

#include "stk.h"
#include "memory/stk_memory.h"

#include "stk_c.h"
#include "stk_c_time.h"

#define STK_C_TIMERS_TOTAL (STK_C_TIMER_MAX * STK_C_CPU_COUNT)

// Override STK_TIMER_COUNT_MAX with STK_C_TIMER_MAX.
#undef STK_TIMER_COUNT_MAX
#define STK_TIMER_COUNT_MAX (STK_C_TIMER_MAX)
// Override STK_TIMER_HANDLER_STACK_SIZE with STK_C_TIMER_HANDLER_STACK_SIZE.
#undef STK_TIMER_HANDLER_STACK_SIZE
#define STK_TIMER_HANDLER_STACK_SIZE (STK_C_TIMER_HANDLER_STACK_SIZE)
#include "time/stk_time.h"

using namespace stk;
using namespace stk::time;

// -----------------------------------------------------------------------------
// Internal concrete Timer subclass that bridges C++ OnExpired() -> C callback
// -----------------------------------------------------------------------------

class CTimerWrapper final : public TimerHost::Timer
{
public:
    CTimerWrapper() : m_host_handle(nullptr), m_callback(nullptr), m_user_data(nullptr)
    {}

    void Initialize(stk_timer_callback_t const callback, void *user_data)
    {
        STK_ASSERT(callback != nullptr);

        m_callback  = callback;
        m_user_data = user_data;
    }

    // Update the host association without touching callback/user_data.
    // Called every time the timer is rearmed so the expiration callback
    // always receives the correct host pointer.
    void SetHostHandle(stk_timerhost_t *host_handle) { m_host_handle = host_handle; }

    // Clear all fields so the slot can be reused after stk_timer_destroy().
    void Reset()
    {
        m_callback    = nullptr;
        m_user_data   = nullptr;
        m_host_handle = nullptr;
    }

    stk_timer_callback_t GetCallback() { return m_callback; }
    void *GetUserData()                { return m_user_data; }
    stk_timerhost_t *GetHostHandle()   { return m_host_handle; }

    void OnExpired(TimerHost *host) override;

private:
    stk_timerhost_t     *m_host_handle; //!< C-level host, forwarded to the callback
    stk_timer_callback_t m_callback;
    void                *m_user_data;
};

struct stk_timer_t
{
    CTimerWrapper handle;
};

// Cast from CTimerWrapper to stk_timer_t without a warning.
static __stk_forceinline stk_timer_t *CastCppTimerWrapperToC(CTimerWrapper *const t)
{
    return reinterpret_cast<stk_timer_t *>(reinterpret_cast<void *>(t));
}

// Interop-private helpers
namespace stk {
namespace interop_c_helper {

extern void InitializeTimerHost(stk_kernel_t *kernel, stk::time::TimerHost *th, EAccessMode amode);

} // interop_c_helper
} // stk

void CTimerWrapper::OnExpired(TimerHost *host)
{
    STK_UNUSED(host);

    if (m_callback != nullptr)
    {
        m_callback(m_host_handle, CastCppTimerWrapperToC(this), m_user_data);
    }
}

// -----------------------------------------------------------------------------
// Timer slot pool
// -----------------------------------------------------------------------------
static struct TimerSlot
{
    TimerSlot() : timer(), busy(false)
    {}

    stk_timer_t timer;
    bool        busy;
}
s_Timers[STK_C_TIMERS_TOTAL];

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
    
bool stk_timerhost_shutdown(stk_timerhost_t *host)
{
    STK_ASSERT(host != nullptr);

    return host->handle.Shutdown();
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

stk_timer_t *stk_timer_create(stk_timer_callback_t callback, void *user_data)
{
    STK_ASSERT(callback != nullptr);

    const sync::ScopedCriticalSection __cs;

    stk_timer_t *result = nullptr;

    for (uint32_t i = 0U; i < STK_C_TIMERS_TOTAL; ++i)
    {
        if (!s_Timers[i].busy)
        {
            s_Timers[i].busy = true;
            s_Timers[i].timer.handle.Initialize(callback, user_data);
            result = &s_Timers[i].timer;
            break;
        }
    }

    // pool exhausted, you must increase STK_C_TIMER_MAX
    STK_ASSERT(result != nullptr);

    return result;
}

void stk_timer_destroy(stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    // destroying an active timer is a programming error
    STK_ASSERT(!tmr->handle.IsActive());

    const sync::ScopedCriticalSection __cs;

    bool found = false;

    for (uint32_t i = 0U; ((i < STK_C_TIMERS_TOTAL) && !found); ++i)
    {
        if (s_Timers[i].busy && (&s_Timers[i].timer == tmr))
        {
            tmr->handle.Reset();
            s_Timers[i].busy = false;
            found = true;
        }
    }

    // timer not found in the pool: indicates a double-free or corruption
    STK_ASSERT(found);
}

// -----------------------------------------------------------------------------
// Timer control helpers
//
// Every control function that rearms a timer also refreshes the host handle
// stored in the wrapper so the C callback always receives the correct host.
// -----------------------------------------------------------------------------

bool stk_timer_start(stk_timerhost_t *host,
                     stk_timer_t     *tmr,
                     uint32_t         delay,
                     uint32_t         period)
{
    STK_ASSERT(host  != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->handle.SetHostHandle(host);

    return host->handle.Start(tmr->handle, delay, period);
}

bool stk_timer_stop(stk_timerhost_t *host, stk_timer_t *tmr)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return host->handle.Stop(tmr->handle);
}

bool stk_timer_reset(stk_timerhost_t *host, stk_timer_t *tmr)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return host->handle.Reset(tmr->handle);
}

bool stk_timer_restart(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t delay, uint32_t period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association before timer can fire
    tmr->handle.SetHostHandle(host);

    return host->handle.Restart(tmr->handle, delay, period);
}

bool stk_timer_start_or_reset(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t delay, uint32_t period_ticks)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    // refresh host association (harmless if timer is already active on host)
    tmr->handle.SetHostHandle(host);

    return host->handle.StartOrReset(tmr->handle, delay, period_ticks);
}

bool stk_timer_set_period(stk_timerhost_t *host, stk_timer_t *tmr, uint32_t period_ticks)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(tmr != nullptr);

    return host->handle.SetPeriod(tmr->handle, period_ticks);
}

// -----------------------------------------------------------------------------
// Timer query
// -----------------------------------------------------------------------------

bool stk_timer_is_active(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return tmr->handle.IsActive();
}

uint32_t stk_timer_get_period(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return tmr->handle.GetPeriod();
}

int64_t stk_timer_get_deadline(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return static_cast<int64_t>(tmr->handle.GetDeadline());
}

int64_t stk_timer_get_timestamp(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return static_cast<int64_t>(tmr->handle.GetTimestamp());
}

uint32_t stk_timer_get_remaining_ticks(const stk_timer_t *tmr)
{
    STK_ASSERT(tmr != nullptr);

    return tmr->handle.GetRemainingTicks();
}

// -----------------------------------------------------------------------------
// PeriodicTrigger
// -----------------------------------------------------------------------------

struct stk_periodic_trigger_t
{
    stk_periodic_trigger_t(uint32_t period, bool start) : handle(period, start)
    {}

    time::PeriodicTrigger handle;
};

stk_periodic_trigger_t *stk_periodic_trigger_create(stk_periodic_trigger_mem_t *const membuf,
                                                    uint32_t                    membuf_size,
                                                    uint32_t                    period_ticks,
                                                    bool                        started)
{
    STK_ASSERT(membuf != nullptr);
    STK_ASSERT(membuf_size >= sizeof(stk_periodic_trigger_t));

    stk_periodic_trigger_t *result = nullptr;
    if (membuf_size >= sizeof(stk_periodic_trigger_t))
    {      
        result = new (membuf->data) stk_periodic_trigger_t(period_ticks, started);
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

// =============================================================================
} // extern "C"
// =============================================================================
