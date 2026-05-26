/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 *
 * ----------------------------------------------------------------------------
 * Button Debounce Demo - STM32F407G-DISC1
 * ----------------------------------------------------------------------------
 *
 * Hardware:
 *   * USER button - PA0 (active-HIGH, external pull-down on board)
 *   * LEDs        - PD12 (GREEN), PD13 (ORANGE), PD14 (RED), PD15 (BLUE)
 *
 * Behavior:
 *   1. Pressing USER button (PA0) triggers EXTI0_IRQHandler on every rising
 *      edge - including contact-bounce spikes.  A DebounceTimer (one-shot,
 *      DEBOUNCE_MS ms) is armed/re-armed on every edge via TimerHost::Restart().
 *      Only when the button stays high for the full DEBOUNCE_MS ms without another
 *      edge does OnExpired() fire and the confirmed press be acted upon.
 *   2. On a confirmed press next LED is lit exclusively.
 *
 * Debounce strategy - re-arming one-shot via Restart():
 *
 *     EXTI edge arrives          ->  Restart(DebounceTimer, DEBOUNCE_MS ms, one-shot)
 *                                                 |
 *          bounce edge           ->  Restart(...) resets the DEBOUNCE_MS ms window
 *                                                 |
 *     DEBOUNCE_MS ms of silence  ->  OnExpired() - confirmed press
 *
 *   TimerHost::Restart() is used (not StartOrReset) because the debounce
 *   window must always restart from the latest edge, even when a previous
 *   one-shot is already in-flight. Restart() is atomic: the timer cannot
 *   fire between the implicit stop and re-start, eliminating the race that
 *   would exist with a Stop() + Start() sequence.
 *
 *   The ISR only calls g_Timers.Restart() - it never touches LED state
 *   directly, keeping ISR work minimal and hardware-register access out of
 *   interrupt context.
 */

#include <stk.h>
#include <time/stk_time.h>
#include "example.h"

using namespace bsp;

enum { TASK_STACK_SIZE = 256 };

// Debounce window: button must be stable for this long after the last edge
// before the press is considered confirmed. 200 ms covers the vast majority
// of mechanical switches (typical bounce < 20 ms). The lower value makes
// button press more sensitive/responsive but prone to bouncing effect.
enum { DEBOUNCE_MS = 200 };

// ----------------------------------------------------------------------------
// Board helpers - GPIO / button init (STM32F407G-DISC1)
// ----------------------------------------------------------------------------

namespace Board
{
    static void ButtonInit()
    {
        // Enable GPIOA clock.
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
        __DSB();

        // PA0: input, no pull (board has external pull-down).
        GPIOA->MODER &= ~(3U << 0);
        GPIOA->PUPDR &= ~(3U << 0);

        // Route PA0 to EXTI0.
        RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
        __DSB();
        SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0] & ~SYSCFG_EXTICR1_EXTI0)
                           | SYSCFG_EXTICR1_EXTI0_PA;

        // EXTI0: rising-edge only, unmask.
        EXTI->RTSR |=  (1U << 0);
        EXTI->FTSR &= ~(1U << 0);
        EXTI->IMR  |=  (1U << 0);

        // Priority below SysTick so ISR can safely call TimerHost::Restart().
        NVIC_SetPriority(EXTI0_IRQn, 8);
        NVIC_EnableIRQ(EXTI0_IRQn);
    }
} // namespace Board

// ----------------------------------------------------------------------------
// Debounce timer callback - confirmed button press, fires DEBOUNCE_MS ms after
// the last rising edge on PA0 with no further edges in between.
// ----------------------------------------------------------------------------

struct DebounceTimer : public stk::time::TimerHost::Timer
{
    uint8_t m_led;

    DebounceTimer() : m_led(0)
    {}

    void OnExpired(stk::time::TimerHost * /*host*/) override
    {
        // Confirmed press: next LED as acknowledgement.
        Led::SwitchOnExclusive(static_cast<Led::Id>(m_led));
        m_led = (m_led + 1) % LED_MAX;
    }
};

// ----------------------------------------------------------------------------
// Static timer instances - must outlive the kernel
// ----------------------------------------------------------------------------

static stk::time::TimerHost g_Timers;
static DebounceTimer        g_DebounceTimer;

// ----------------------------------------------------------------------------
// EXTI0 ISR - USER button on PA0
//
// Re-arms the debounce one-shot on every rising edge.  Restart() is
// ISR-safe: it posts a single command to the TimerHost tick task and
// returns immediately without blocking.
// ----------------------------------------------------------------------------

extern "C" void EXTI0_IRQHandler()
{
    // Acknowledge the interrupt first to allow re-triggering.
    EXTI->PR = (1U << 0);

    // Check if button is pressed.
    bool pressed = ((GPIOA->IDR & (1U << 0)) != 0U);

    // Re-arm (or arm for the first time) the debounce window.
    // Restart() atomically cancels any in-flight one-shot and schedules a
    // fresh DEBOUNCE_MS ms one-shot, so every bounce (on-off-on...) simply
    // resets the window.
    if (pressed)
        g_Timers.Restart(g_DebounceTimer, stk::GetTicksFromMs(DEBOUNCE_MS), 0);
}

// ---------------------------------------------------------------------------
// RunExample
// ---------------------------------------------------------------------------

void RunExample()
{
    Led::InitAll(false);
    Board::ButtonInit();

    static stk::Kernel<stk::KERNEL_STATIC | stk::KERNEL_SYNC | (STK_TICKLESS_IDLE ? stk::KERNEL_TICKLESS : 0),
        stk::time::TimerHost::TASK_COUNT, stk::SwitchStrategyRR, stk::PlatformDefault> g_Kernel;

    g_Kernel.Initialize();
    g_Timers.Initialize(&g_Kernel, stk::ACCESS_PRIVILEGED);

    g_Kernel.Start();
    STK_ASSERT(false); // Kernel in KERNEL_STATIC mode never exits
}
