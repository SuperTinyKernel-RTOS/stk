/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk.h>
#include "example.h"

// Sync primitives used and why:
//
//   PipeT<LedCommand, 2>  - typed command queue from CtrlTask -> LedTask.
//                           Consolidates two LedTasks into one; the command carries both
//                           the target state and the LED id, so LedTask needs no shared globals.
//
//   Semaphore             - CtrlTask's 1-second periodic tick source. Signal() is called from
//                           a dedicated TimerTask, replacing the manual drift-correction math
//                           (GetTimeNowMs arithmetic) from the original. The semaphore's stateful
//                           "remembered signal" property means a late wakeup never loses a tick.
//
//   Mutex                 - guards CtrlTask's internal state (led counter + last command) so it
//                           is safe to extend CtrlTask later with a second writer (e.g. a UART
//                           command handler) without introducing races.  Demonstrates that
//                           ACCESS_USER tasks coordinate through kernel objects, not raw globals.
//
//   Event (manual-reset)  - LedTask fires it after each hardware change; MonitorTask waits on it
//                           to log/observe the transition. Demonstrates the privileged->unprivileged
//                           notification path: hardware task signals, observer reacts.

#include <sync/stk_sync_pipe.h>
#include <sync/stk_sync_semaphore.h>
#include <sync/stk_sync_mutex.h>
#include <sync/stk_sync_event.h>

using namespace bsp;

// ---------------------------------------------------------------------------
// Command sent through the pipe: what state to apply and which LED to drive.
// ---------------------------------------------------------------------------
struct LedCommand
{
    LedId  led;
    bool   on;
};

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// ---------------------------------------------------------------------------
// Shared synchronization objects
// ---------------------------------------------------------------------------

// Command queue: CtrlTask writes LedCommands; LedTask blocks on Read().
// Depth 2: allows CtrlTask to queue an on and an off command without stalling
// if LedTask has not yet drained the previous one.
static stk::sync::PipeT<LedCommand, 2> g_LedPipe;

// Tick semaphore: TimerTask signals it every 1 s; CtrlTask waits on it.
// Initial count 0: CtrlTask blocks until the first tick arrives.
static stk::sync::Semaphore g_TickSem(0);

// Mutex protecting CtrlTask's mutable state.
static stk::sync::Mutex g_CtrlMtx;

// Manual-reset event: LedTask fires it after each hardware transition;
// MonitorTask waits on it to observe changes without polling.
// Manual-reset: stays signaled until MonitorTask explicitly resets it,
// so a slow monitor never misses a transition that happened while it was busy.
static stk::sync::Event g_LedChangedEvt(/*manual_reset=*/true);

// ---------------------------------------------------------------------------
// LedTask - ACCESS_PRIVILEGED
// Directly drives hardware. Receives commands from CtrlTask via g_LedPipe.
// Notifies MonitorTask via g_LedChangedEvt after each hardware change.
// ---------------------------------------------------------------------------
template <stk::EAccessMode _AccessMode>
class LedTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
private:
    void Run() override
    {
        while (true)
        {
            LedCommand cmd;

            // Block (zero CPU) until CtrlTask sends a command.
            if (!g_LedPipe.Read(cmd))
                continue;

            // switch LED on/off
            {
                // we do not want preemption during IO with hardware
                stk::hw::ScopedCriticalSection __cs;

                Led::Set(cmd.led, cmd.on);
            }

            // Notify MonitorTask that a hardware transition just occurred.
            // Set() is ISR-safe and a no-op if the event is already signaled,
            // so a fast CtrlTask firing multiple commands before MonitorTask
            // wakes up will not block LedTask.
            g_LedChangedEvt.Set();
        }
    }
};

// ---------------------------------------------------------------------------
// TimerTask - ACCESS_USER
// Signals g_TickSem every 1 s. Separating the timer from CtrlTask keeps
// CtrlTask's logic clean and makes the tick source easily replaceable
// (e.g. by a hardware timer ISR calling Signal() directly).
// Note, consider using time::TimerHost for a timer-related functionality.
// ---------------------------------------------------------------------------
template <stk::EAccessMode _AccessMode>
class TimerTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
private:
    void Run() override
    {
        const stk::Timeout period = stk::GetTicksFromMs(1000);
        stk::Ticks timeline = stk::GetTicks(); // timeline is not shared with other tasks

        while (true)
        {
            // sleep 1s drift-free and delegate work to another task switching another LED, hw thread could have
            // some latency, thus account for it
            stk::SleepUntil(timeline += period);

            // Semaphore::Signal() is ISR-safe: can be moved to a hardware timer ISR
            // later without changing CtrlTask at all.
            g_TickSem.Signal();
        }
    }
};

// ---------------------------------------------------------------------------
// CtrlTask - ACCESS_USER
// Owns the LED sequencing logic. Waits for the 1-second tick semaphore, then
// sends the next on/off command to LedTask via the pipe.
// Uses g_CtrlMtx to protect its state - ready for a second writer.
// ---------------------------------------------------------------------------
template <stk::EAccessMode _AccessMode>
class CtrlTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
private:
    // Mutable state guarded by g_CtrlMtx.
    bool m_led_on = false;

    void Run() override
    {
        while (true)
        {
            // Wait for the next 1-second tick from TimerTask.
            // Semaphore remembers signals: if TimerTask fires while CtrlTask is
            // busy below, the count increments and Wait() returns immediately on
            // the next iteration - no tick is ever silently dropped.
            g_TickSem.Wait();

            LedCommand cmd;

            // guard state under mutex - safe to add a second writer later
            {
                stk::sync::Mutex::ScopedLock lock(g_CtrlMtx);

                m_led_on = !m_led_on;
                cmd = { Led::GREEN, m_led_on };
            }

            // if MCU supports (for example Cortex-M7/M33), ACCESS_USER does not allow an access to a hardware directly,
            // therefore you can process in this thread/task an insecure context or data
            g_LedPipe.Write(cmd);
        }
    }
};

// ---------------------------------------------------------------------------
// MonitorTask - ACCESS_USER
// Observes LED changes without polling. Blocks on the manual-reset event
// set by LedTask, logs/reacts, then resets it for the next transition.
// Demonstrates the privileged -> unprivileged notification path.
// ---------------------------------------------------------------------------
template <stk::EAccessMode _AccessMode>
class MonitorTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
private:
    void Run() override
    {
        while (true)
        {
            // Block until LedTask signals a hardware transition.
            // WAIT_INFINITE: MonitorTask has nothing to do between transitions.
            g_LedChangedEvt.Wait();

            // --- react to the change here (e.g. increment a counter, log via UART) ---

            // Re-arm the manual-reset event for the next transition.
            // Reset() after processing ensures we don't busy-return on the
            // same signal if we loop faster than LedTask fires the next one.
            g_LedChangedEvt.Reset();
        }
    }
};

// ---------------------------------------------------------------------------
// RunExample
// ---------------------------------------------------------------------------
void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // allocate scheduling kernel for 4 threads (tasks) with Round-robin scheduling strategy
    static Kernel<KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0), 4,
            SwitchStrategyRR, PlatformDefault> kernel;

    // these are secure/trusted tasks which are allowed to access hardware safely
    static LedTask<ACCESS_PRIVILEGED> secure_hw_task;

    // if MCU supports (for example Cortex-M7/M33), ACCESS_USER does not allow an access to a hardware directly,
    // therefore you can process in this thread/task an insecure context or data
    static TimerTask<ACCESS_USER>   timer_task;
    static CtrlTask<ACCESS_USER>    ctrl_task;
    static MonitorTask<ACCESS_USER> monitor_task;

    // init scheduling kernel
    kernel.Initialize();

    // register threads (tasks)
    kernel.AddTask(&secure_hw_task);
    kernel.AddTask(&timer_task);
    kernel.AddTask(&ctrl_task);
    kernel.AddTask(&monitor_task);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
