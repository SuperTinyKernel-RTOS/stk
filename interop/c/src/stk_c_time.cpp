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

    void Initialize(stk_timer_callback_t callback, void *user_data)
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
    void *GetUserData() { return m_user_data; }
    stk_timerhost_t *GetHostHandle() { return m_host_handle; }

    void OnExpired(TimerHost */*host*/) override
    {
        if (m_callback != nullptr)
            m_callback(m_host_handle, reinterpret_cast<stk_timer_t *>(this), m_user_data);
    }

private:
    stk_timerhost_t     *m_host_handle; //!< C-level host, forwarded to the callback
    stk_timer_callback_t m_callback;
    void                *m_user_data;
};

// -----------------------------------------------------------------------------
// Timer slot pool
// -----------------------------------------------------------------------------
struct stk_timer_t
{
    CTimerWrapper handle;
};

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
    if (core_nr >= STK_C_CPU_COUNT)
        return nullptr;

    return &s_TimerHosts[core_nr];
}

void stk_timerhost_init(stk_timerhost_t *host,
                        stk_kernel_t    *kernel,
                        bool             privileged)
{
    STK_ASSERT(host   != nullptr);
    STK_ASSERT(kernel != nullptr);

    host->handle.Initialize(reinterpret_cast<IKernel *>(kernel),
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

    return (int64_t)host->handle.GetTimeNow();
}

// -----------------------------------------------------------------------------
// Timer lifecycle
// -----------------------------------------------------------------------------

stk_timer_t *stk_timer_create(stk_timer_callback_t callback, void *user_data)
{
    STK_ASSERT(callback != nullptr);

    sync::ScopedCriticalSection __cs;

    for (uint32_t i = 0; i < STK_C_TIMERS_TOTAL; ++i)
    {
        if (!s_Timers[i].busy)
        {
            s_Timers[i].busy = true;
            s_Timers[i].timer.handle.Initialize(callback, user_data);

            return &s_Timers[i].timer;
        }
    }

    // pool exhausted, you must increase STK_C_TIMER_MAX
    STK_ASSERT(false);
    return nullptr;
}

void stk_timer_destroy(stk_timer_t *timer)
{
    STK_ASSERT(timer != nullptr);

    // destroying an active timer is a programming error
    STK_ASSERT(!timer->handle.IsActive());

    sync::ScopedCriticalSection __cs;

    for (uint32_t i = 0; i < STK_C_TIMERS_TOTAL; ++i)
    {
        if (s_Timers[i].busy && (&s_Timers[i].timer == timer))
        {
            timer->handle.Reset();
            s_Timers[i].busy = false;
            return;
        }
    }

    // timer not found in the pool: indicates a double-free or corruption
    STK_ASSERT(false);
}

// -----------------------------------------------------------------------------
// Timer control helpers
//
// Every control function that rearms a timer also refreshes the host handle
// stored in the wrapper so the C callback always receives the correct host.
// -----------------------------------------------------------------------------

bool stk_timer_start(stk_timerhost_t *host,
                     stk_timer_t     *timer,
                     uint32_t         delay,
                     uint32_t         period)
{
    STK_ASSERT(host  != nullptr);
    STK_ASSERT(timer != nullptr);

    // refresh host association before timer can fire
    timer->handle.SetHostHandle(host);

    return host->handle.Start(timer->handle, delay, period);
}

bool stk_timer_stop(stk_timerhost_t *host, stk_timer_t *timer)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(timer != nullptr);

    return host->handle.Stop(timer->handle);
}

bool stk_timer_reset(stk_timerhost_t *host, stk_timer_t *timer)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(timer != nullptr);

    return host->handle.Reset(timer->handle);
}

bool stk_timer_restart(stk_timerhost_t *host, stk_timer_t *timer, uint32_t delay, uint32_t period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(timer != nullptr);

    // refresh host association before timer can fire
    timer->handle.SetHostHandle(host);

    return host->handle.Restart(timer->handle, delay, period);
}

bool stk_timer_start_or_reset(stk_timerhost_t *host, stk_timer_t *timer, uint32_t delay, uint32_t period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(timer != nullptr);

    // refresh host association (harmless if timer is already active on host)
    timer->handle.SetHostHandle(host);

    return host->handle.StartOrReset(timer->handle, delay, period);
}

bool stk_timer_set_period(stk_timerhost_t *host, stk_timer_t *timer, uint32_t period)
{
    STK_ASSERT(host != nullptr);
    STK_ASSERT(timer != nullptr);

    return host->handle.SetPeriod(timer->handle, period);
}

// -----------------------------------------------------------------------------
// Timer query
// -----------------------------------------------------------------------------

bool stk_timer_is_active(const stk_timer_t *timer)
{
    STK_ASSERT(timer != nullptr);

    return timer->handle.IsActive();
}

uint32_t stk_timer_get_period(const stk_timer_t *timer)
{
    STK_ASSERT(timer != nullptr);

    return timer->handle.GetPeriod();
}

int64_t stk_timer_get_deadline(const stk_timer_t *timer)
{
    STK_ASSERT(timer != nullptr);

    return (int64_t)timer->handle.GetDeadline();
}

int64_t stk_timer_get_timestamp(const stk_timer_t *timer)
{
    STK_ASSERT(timer != nullptr);

    return (int64_t)timer->handle.GetTimestamp();
}

uint32_t stk_timer_get_remaining_ticks(const stk_timer_t *timer)
{
    STK_ASSERT(timer != nullptr);

    return timer->handle.GetRemainingTicks();
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

stk_periodic_trigger_t *stk_periodic_trigger_create(stk_periodic_trigger_mem_t *memory,
                                                    uint32_t                    memory_size,
                                                    uint32_t                    period,
                                                    bool                        started)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_periodic_trigger_t));
    if (memory == nullptr || memory_size < sizeof(stk_periodic_trigger_t))
        return nullptr;

    return new (memory->data) stk_periodic_trigger_t(static_cast<Ticks>(period), started);
}

void stk_periodic_trigger_destroy(stk_periodic_trigger_t *trig)
{
    if (trig != nullptr)
        trig->~stk_periodic_trigger_t();
}

bool stk_periodic_trigger_poll(stk_periodic_trigger_t *trig)
{
    STK_ASSERT(trig != nullptr);

    return trig->handle.Poll();
}

void stk_periodic_trigger_set_period(stk_periodic_trigger_t *trig, uint32_t period)
{
    STK_ASSERT(trig != nullptr);

    trig->handle.SetPeriod(static_cast<Ticks>(period));
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
