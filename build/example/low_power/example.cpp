/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 *
 * --------------------------------------------------------------------------------------------
 * Low-Power Demo - STM32F407G-DISC1
 * --------------------------------------------------------------------------------------------
 *
 * Hardware used on the STM32F407G-DISC1 discovery board:
 *   * USER button  - PA0 (active-HIGH, no external pull needed)
 *   * LEDs         - PD12 (GREEN), PD13 (ORANGE), PD14 (RED), PD15 (BLUE)
 *   * Internal ADC - ADC1 channel 16 (on-chip temperature sensor, VBAT/2 mux)
 *
 * Behavior:
 *   1. Three LED tasks run round-robin, each lighting one LED for 1 s at a time.
 *   2. A periodic Timer (0.5 Hz / 2 s) samples the internal temperature sensor
 *      and publishes each reading into g_AdcPipe; no dedicated ADC task is needed.
 *   3. When the CPU is idle (all tasks sleeping), IEventOverrider::OnSleep() fires
 *      and selects one of two power-saving modes based on sleep duration:
 *        – Short idle  (sleep_ticks <  40): plain Cortex-M4 SLEEP via __WFI();
 *          SysTick wakes the CPU; PLL and ADC remain powered.
 *        – Deep idle   (sleep_ticks >= 40): Cortex-M4 STOP mode via the RTC
 *          Wakeup Timer; ADC is suspended, HSE+PLL are restored on wake-up.
 *   4. Pressing the USER button (PA0) triggers EXTI0_IRQHandler which:
 *        – Toggles the kernel between running and suspended states via
 *          IKernelService::Suspend() / Resume()
 *        – While suspended all LED tasks and the ADC timer are frozen
 *
 * Scheduler: KERNEL_STATIC | KERNEL_SYNC | KERNEL_TICKLESS (when STK_TICKLESS_IDLE),
 *            Round-Robin, 4 tasks
 *            (3 LED tasks + 1 Log task; TimerHost adds its own internal tasks).
 */

#include <stdio.h> // for printf()

#include <stk.h>
#include <sync/stk_sync.h>
#include <time/stk_time.h>
#include "example.h"

using namespace bsp;

// ============================================================================
//   ADC sample descriptor
// ============================================================================

/*! \struct AdcSample
    \brief  One ADC reading delivered through the pipe.
*/
struct AdcSample
{
    stk::Ticks timestamp; //!< kernel tick at the time of conversion
    uint16_t   raw;       //!< raw 12-bit ADC value
};

// ============================================================================
//   Shared state
// ============================================================================

// One flag bit per LED task; task 0 (RED) goes first.
static const uint32_t FLAGS_ALL[] = {
    (1U << LED_RED),
    (1U << LED_GREEN),
    (1U << LED_ORANGE)
};

// Start with the RED task's flag set so it runs first.
static stk::sync::EventFlags g_TaskFlags(FLAGS_ALL[LED_RED]);

// Timeline for a precise LED switching.
static stk::Ticks g_Timeline = 0;

// Pipe carrying AdcSample structs from the ADC timer callback to LogTask.
// Capacity of 8 absorbs short bursts without dropping samples.
static stk::sync::PipeT<AdcSample, 8> g_AdcPipe;

// Kernel-suspended flag: set/cleared in the EXTI ISR.
// Marked volatile so the ISR write is visible to polling code.
static volatile bool g_KernelSuspended = false;

// ============================================================================
//   Board-specific helpers
// ============================================================================

namespace Board
{

// ----------------------------------------------------------------------------
//   GPIO / LED
// ----------------------------------------------------------------------------

static void LedInit()
{
    Led::InitAll(false);
}

static __stk_forceinline void LedSet(bsp::Led::Id pin, bool on)
{
    Led::Set(pin, on);
}

static __stk_forceinline void LedSwitchOnExclusive(bsp::Led::Id pin)
{
    Led::SwitchOnExclusive(pin);
}

// ----------------------------------------------------------------------------
//   USER button (PA0, active-HIGH) + EXTI0
// ----------------------------------------------------------------------------

static void ButtonInit()
{
    // Enable GPIOA clock
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    __DSB();

    // PA0: input, no pull (board has an external pull-down)
    GPIOA->MODER &= ~(3U << 0); // input
    GPIOA->PUPDR &= ~(3U << 0); // no pull

    // Route PA0 to EXTI0
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    __DSB();
    SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0] & ~SYSCFG_EXTICR1_EXTI0) | SYSCFG_EXTICR1_EXTI0_PA;

    // EXTI0: rising edge (button press), unmask
    EXTI->RTSR |=  (1U << 0);
    EXTI->FTSR &= ~(1U << 0);
    EXTI->IMR  |=  (1U << 0);

    // Enable EXTI0 interrupt in NVIC, priority below SysTick so it can call
    // kernel service functions safely (kernel is ISR-safe for Suspend/Resume).
    NVIC_SetPriority(EXTI0_IRQn, 8);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

// ----------------------------------------------------------------------------
//   ADC1 - internal temperature sensor (channel 16)
// ----------------------------------------------------------------------------

static void AdcInit()
{
    // Enable ADC1 and its clock
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    __DSB();

    // ADC common: TSVREFE=1 to enable temp sensor & Vrefint
    ADC->CCR |= ADC_CCR_TSVREFE;

    // ADC1 config: 12-bit, right-aligned, single conversion
    ADC1->CR1  = 0;                  // default: 12-bit resolution
    ADC1->CR2  = 0;                  // single conversion, software trigger
    ADC1->SMPR1 = (7U << 18);        // 480 cycles sample time for ch16 (temp sensor needs ≥ 10 us)
    ADC1->SQR3  = 16U;               // sequence: channel 16 (temp sensor)
    ADC1->SQR1  = 0;                 // 1 conversion in sequence

    // Power on ADC (ADON) and wait for stabilization
    ADC1->CR2 |= ADC_CR2_ADON;
    for (volatile uint32_t i = 0; i < 10000; ++i) {}
}

// Suspend ADC to save power (called from OnSleep before WFI).
static __stk_forceinline void AdcSuspend()
{
    ADC1->CR2 &= ~ADC_CR2_ADON;
}

// Resume ADC after a wake-up event.
static __stk_forceinline void AdcResume()
{
    if ((ADC1->CR2 & ADC_CR2_ADON) != ADC_CR2_ADON)
    {
        ADC1->CR2 |= ADC_CR2_ADON;
        // Short stabilization delay (aprox 3 us at 168 MHz).
        for (volatile uint32_t i = 0; i < 500; ++i) {}
    }
}

// Start a single ADC conversion and return the raw result (blocking).
static __stk_forceinline uint16_t AdcRead()
{
    AdcResume();

    ADC1->SR  &= ~ADC_SR_EOC;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC)) {}
    return static_cast<uint16_t>(ADC1->DR);
}

static __stk_forceinline float TEMP_ValueToDeg(uint32_t sens)
{
    static const float to_V  = 3.3f / 4095;             // max resolution: V per step
    static const float slope = 2.5f / 1000;             // slope in V
    static const float v_cal = 1060.0f * (3.3f / 4095); // 26 deg == 1060
    static const float t_cal = 26;                      // calibration T

    float temp = (sens * to_V - v_cal) / slope + t_cal;

    return temp;
}

// ----------------------------------------------------------------------------
//   CPU / PLL
// ----------------------------------------------------------------------------
//
// Restore the PLL-based system clock after returning from STOP mode.
//
// On STOP exit the hardware has silently selected HSI (16 MHz) as the
// system clock source and powered down both HSE and the main PLL.
// All RCC divider/multiplier fields (PLLM, PLLN, PLLP, PLLQ, AHB/APB
// prescalers) and FLASH->ACR wait-states are preserved by hardware -
// we must not touch them.  SysTick->LOAD and SysTick->VAL are equally
// untouched and must stay that way so the in-progress tick interval
// is not truncated.
//
// Sequence (mirrors the PLL bring-up inside SystemClock_Config but
// without any HAL/SysTick side-effects):
//
//   1. Re-enable HSE and wait for it to stabilize.
//   2. Re-enable the main PLL and wait for lock.
//   3. Switch the system clock source back to PLL.
//
// Must be called with interrupts disabled (called from CpuEnterDeepSleepMode
// which is already inside a __disable_irq / __enable_irq window).
// ----------------------------------------------------------------------------

static __stk_forceinline void RestorePllClock()
{
    // -- 1. Re-enable HSE ----------------------------------------------------
    // DISC1 uses the 8 MHz on-board oscillator as the PLL input (not HSI).
    // HSEON is in the upper half of RCC->CR so a 32-bit read-modify-write
    // is safe here (no other bits in that half are volatile at this point).
    RCC->CR |= RCC_CR_HSEON;

    // Poll HSERDY.  On the DISC1 the crystal typically stabilises in
    // < 2 ms; spinning here is acceptable because interrupts are masked
    // and we are not inside any scheduler context.
    while (!(RCC->CR & RCC_CR_HSERDY)) {}

    // -- 2. Re-enable the main PLL -------------------------------------------
    // PLLON and PLLRDY live in the same RCC->CR register.
    // PLLM/PLLN/PLLP/PLLQ fields in RCC->PLLCFGR were not disturbed by
    // STOP, so we only need to set the enable bit and wait for lock.
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}

    // -- 3. Switch system clock source back to PLL ---------------------------
    // Read-modify-write on RCC->CFGR: clear the SW field (bits [1:0])
    // and write SW = 10 (PLL selected).
    // The AHB/APB prescaler fields in the same register are left intact.
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;

    // Poll SWS (bits [3:2]) until the hardware confirms the switch.
    // This typically takes 1-2 AHB cycles after the PLL lock is confirmed.
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}

    // At this point HCLK is back at 168 MHz.  SysTick->LOAD, SysTick->VAL
    // and the NVIC priority table are completely unchanged.
}

// Put CPU into a DEEP SLEEP mode.
static __stk_forceinline void CpuEnterDeepSleepMode()
{
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

    __DSB();
    __WFI();

    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;

    // Restore CPU frequency: HSE + PLL back to 168 MHz.
    RestorePllClock();
}

// Put CPU into a SLEEP mode.
static __stk_forceinline void CpuEnterSleepMode()
{
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    __DSB();
    __WFI();
}

// ----------------------------------------------------------------------------
//   RTC Wakeup Timer
// ----------------------------------------------------------------------------
//
// The STM32F407 RTC wakeup timer is clocked from LSI (~32 kHz) through
// the RTC prescaler and survives in STOP mode - making it the correct
// timed wake-up source for deep sleep on this device.
//
// Clock chain selected here:
//   LSI (~32 kHz)   ->  RTC/16 divider  ->  WUCKSEL = 000
//   Effective rate  =  32000 / 16  =  2000 counts/s  ->  0.5 ms per count
//
// Conversion:  WUTR value = sleep_ticks * 2   (for 1 ms kernel ticks)
//
// A 1-tick guard (2 counts) is subtracted so the wakeup fires marginally
// before the deadline, letting SysTick close the residual gap normally.
//
// The RTC wakeup line is internally connected to EXTI line 22.  That line
// must be unmasked and its rising-edge trigger enabled before the wakeup
// interrupt can reach the NVIC.
// ----------------------------------------------------------------------------

const uint32_t RTC_WKUP_COUNTS_PER_TICK = 2U;      // at RTC/16, LSI~32kHz
const uint32_t RTC_WKUP_GUARD_COUNTS    = 2U;      // 1-tick guard
const uint32_t RTC_WKUP_MAX_COUNT       = 0xFFFFU; // 16-bit WUTR

// Initialize the RTC wakeup timer once at startup.
// Configures LSI as the RTC clock source and prepares EXTI line 22.
// The RTC write-protection and init-mode sequences follow RM0090 27.3.
static void RtcWakeupInit()
{
    // -- 1. Start LSI --------------------------------------------------------
    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {}

    // -- 2. Select LSI as the RTC clock source (RTCSEL = 10) -----------------
    // This field can only be written once after a backup-domain reset.
    // If already set to LSI we skip it to avoid a needless domain reset.
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    __DSB();
    PWR->CR |= PWR_CR_DBP;  // unlock backup domain

    if ((RCC->BDCR & RCC_BDCR_RTCSEL) != RCC_BDCR_RTCSEL_1)
    {
        // Reset backup domain, then re-select LSI
        RCC->BDCR |=  RCC_BDCR_BDRST;
        RCC->BDCR &= ~RCC_BDCR_BDRST;
        RCC->BDCR |=  RCC_BDCR_RTCSEL_1; // 10 = LSI
    }

    // -- 3. Enable RTC peripheral clock --------------------------------------
    RCC->BDCR |= RCC_BDCR_RTCEN;
    __DSB();

    // -- 4. Disable RTC write protection (magic keys per RM0090 27.3.7) ------
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;

    // -- 5. Enter init mode to set prescalers (required by RM0090) -----------
    RTC->ISR |= RTC_ISR_INIT;
    while (!(RTC->ISR & RTC_ISR_INITF)) {}

    // Async prescaler = 127, Sync prescaler = 249
    // f_ck_apre = 32000/(127+1) = 250 Hz (calendar - not used here)
    // Wakeup timer ignores these and uses its own WUCKSEL divider.
    RTC->PRER = (127U << RTC_PRER_PREDIV_A_Pos) | (249U << RTC_PRER_PREDIV_S_Pos);

    // Exit init mode
    RTC->ISR &= ~RTC_ISR_INIT;

    // -- 6. Re-enable write protection ---------------------------------------
    RTC->WPR = 0xFF;

    // -- 7. Route RTC wakeup to NVIC via EXTI line 22 ------------------------
    // Rising edge trigger; interrupt (not event) mode.
    EXTI->IMR  |= (1U << 22);
    EXTI->RTSR |= (1U << 22);
    EXTI->FTSR &= ~(1U << 22);

    NVIC_SetPriority(RTC_WKUP_IRQn, 12); // low priority - wake-up only
    NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

// Arm the RTC wakeup timer to fire after `sleep_ticks` kernel ticks.
// Must be called with interrupts disabled (called from OnSleep critical window).
static __stk_forceinline void RtcWakeupArm(stk::Timeout sleep_ticks)
{
    // Compute WUTR value, clamp to 16-bit range, apply guard.
    uint32_t counts = static_cast<uint32_t>(sleep_ticks) * RTC_WKUP_COUNTS_PER_TICK;
    if (counts > RTC_WKUP_MAX_COUNT)
        counts = RTC_WKUP_MAX_COUNT;
    if (counts > RTC_WKUP_GUARD_COUNTS)
        counts -= RTC_WKUP_GUARD_COUNTS;
    else
        counts = 1U; // minimum meaningful reload

    // Unlock RTC registers (magic keys per RM0090 27.3.7).
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;

    // Disable wakeup timer and wait until we are allowed to write WUTR
    // (hardware clears WUTWF when the timer is running).
    RTC->CR &= ~RTC_CR_WUTE;
    while (!(RTC->ISR & RTC_ISR_WUTWF)) {}

    // Clear any stale wakeup flag
    RTC->ISR &= ~RTC_ISR_WUTF;
    EXTI->PR  =  (1U << 22); // clear EXTI pending

    // Select RTC/16 clock for the wakeup counter (WUCKSEL = 000).
    RTC->CR = (RTC->CR & ~RTC_CR_WUCKSEL) | 0x0U;

    // Load the auto-reload value (counts – 1 per RM0090: timer fires after
    // WUTR+1 ck_wut cycles, so subtract 1 to get the exact period).
    RTC->WUTR = (counts > 0U) ? (counts - 1U) : 0U;

    // Enable wakeup timer + interrupt.
    RTC->CR |= RTC_CR_WUTIE | RTC_CR_WUTE;

    // Re-enable write protection.
    RTC->WPR = 0xFF;
}

// Disarm the RTC wakeup timer after the CPU wakes.
// Safe to call even if the timer already fired.
static __stk_forceinline void RtcWakeupDisarm()
{
    // Unlock RTC registers (magic keys per RM0090 27.3.7).
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;

    RTC->CR  &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);
    RTC->ISR &= ~RTC_ISR_WUTF;

    // Re-enable write protection.
    RTC->WPR = 0xFF;

    // Clear EXTI pending line.
    EXTI->PR = (1U << 22);
}

} // namespace Board

// ============================================================================
//   RTC Wakeup ISR
// ============================================================================
//
// Fires on EXTI line 22 when the RTC wakeup counter expires.
// Its only job is to clear the RTC and EXTI flags so the interrupt does not
// re-trigger. The kernel's SysTick ISR advances the tick counter after PLL is
// restored by RestorePllClock.
// ============================================================================

extern "C" void RTC_WKUP_IRQHandler()
{
    // Clear the RTC wakeup flag (inside write-protected domain)
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WPR = 0xFF;

    // Clear EXTI line 22 pending bit
    EXTI->PR = (1U << 22);
}

// ============================================================================
//   IEventOverrider - low-power sleep hook
// ============================================================================
//
//   The kernel calls OnSleep() whenever all tasks are sleeping and the CPU
//   would otherwise spin-wait until the next tick. Returning true tells the
//   kernel that we handled the idle period ourselves.
// ============================================================================

class LowPowerOverrider final : public stk::IPlatform::IEventOverrider
{
public:
    /*! \brief Called by the kernel when it enters the idle/sleep state.

        Strategy
        ---------
        sleep_ticks <= 8   ->  CPU stays in scheduler idle; no sleep entered.

        sleep_ticks 9..39  ->  Cortex-M4 SLEEP mode (__WFI).
                               SysTick wakes the CPU at the next tick boundary.
                               Fast entry/exit; ADC stays powered; PLL untouched.

        sleep_ticks >= 40  ->  Cortex-M4 STOP mode driven by the RTC Wakeup Timer.
           1. Arm the RTC wakeup counter (LSI -> RTC/16, 2000 counts/s) to fire
              one tick early so SysTick can close the residual timing gap.
           2. Suspend the ADC to eliminate its ~1 mA quiescent draw.
           3. Enter STOP mode via CpuEnterDeepSleepMode().
              On return the helper re-enables HSE + PLL and switches the system
              clock back to 168 MHz before returning.
           4. Disarm the RTC wakeup timer (no-op if it already fired).
           5. Restore the ADC so the ADC timer can resume its next measurement.

        g_KernelSuspended (set by the USER-button EXTI ISR) forces STOP mode
        regardless of sleep_ticks to maximise power saving while paused.
        In that case the RTC timer is not armed - only the button EXTI or
        another external interrupt will wake the CPU.

        \return true - we handled the idle period; the platform driver must
                       not execute its own default idle path.
    */
    bool OnSleep(stk::Timeout sleep_ticks) override
    {
        // Note: For extended processing here we must increase the stack of
        //       the sleep task using STK_SLEEP_TRAP_STACK_SIZE in stk_config.h.
        //       Based on processing inside OnSleep you would need more or less
        //       stack. 256 looks optimal for this implementation.
        //       You can always inspect Kernel::m_sleep_trap->memory stack array
        //       and check how it is filled (not touched memory is filled with
        //       STK_STACK_MEMORY_FILLER.
        STK_STATIC_ASSERT(STK_SLEEP_TRAP_STACK_SIZE >= 256);

        enum {
            TICKS_IDLE_SLEEP = 8,
            TICKS_DEEP_SLEEP = 40
        };

        if ((sleep_ticks >= TICKS_DEEP_SLEEP) || g_KernelSuspended)
        {
            stk::IKernelService *kernel = stk::IKernelService::GetInstance();

            // 1. Arm RTC wakeup timer and suspend kernel
            //    Skip when kernel is suspended indefinitely - the USER button
            //    EXTI0 is the sole intended wake-up source in that state.
            if (!g_KernelSuspended)
            {
                // Arm RTC wakeup timer
                Board::RtcWakeupArm(sleep_ticks);

                // Suspend kernel by turning off its ticking timer.
                // We ignore returned ticks as they will be equal to sleep_ticks here.
                STK_UNUSED(kernel->Suspend());
            }

            // 2. Power down ADC to cut its quiescent current during STOP.
            Board::AdcSuspend();

            // 3. Enter STOP mode. Returns after any enabled interrupt fires.
            //    RestorePllClock() inside brings HSE + PLL back to 168 MHz.
            //    Interrupts must be enabled before WFI so any IRQ can wake us.
            Board::CpuEnterDeepSleepMode();

            // 4. Disarm RTC wakeup (clears flags; safe if already fired).
            Board::RtcWakeupDisarm();

            // 5. Restore ADC so the ADC timer finds it powered on next conversion.
            Board::AdcResume();

            // 6. Resume scheduling.
            if (!g_KernelSuspended)
            {
                // Resume with number of slept ticks to avoid time skew.
                kernel->Resume(sleep_ticks);

                // We return false in order to avoid re-entry to OnSleep() again
                // the driver will wait in a Idle sleep until the context switch.
                return false;
            }
        }
        else
        if (sleep_ticks > TICKS_IDLE_SLEEP)
        {
            // 1. Power down ADC to cut its quiescent current during STOP.
            Board::AdcSuspend();

            // 2. Short idle - plain SLEEP mode.
            //    SysTick (or EXTI0) wakes the CPU at the next tick boundary.
            //    PLL never stops; ADC stays powered; no clock restore needed.
            Board::CpuEnterSleepMode();

            // 3. Restore ADC so the ADC timer finds it powered on next conversion.
            Board::AdcResume();
        }
        else
        {
            // Short idle - plain SLEEP mode.
            // SysTick (or EXTI0) wakes the CPU at the next tick boundary.
            // PLL never stops; ADC stays powered; no clock restore needed.
            Board::CpuEnterSleepMode();
        }

        // Return true so that driver would re-enter OnSleep() again and we
        // could continue sleeping which could be broken by some ISR in the
        // system.
        return true;
    }
};

// ============================================================================
//   Task stack size
// ============================================================================

#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// ============================================================================
//   LED task
// ============================================================================

template <stk::EAccessMode _AccessMode>
class LedTask final : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t     m_task_id;
    const char *m_name;
    uint32_t    m_my_flag;
    uint32_t    m_next_flag;

public:
    LedTask(uint8_t task_id, const char *name)
        : m_task_id(task_id),
          m_name(name),
          m_my_flag(FLAGS_ALL[task_id]),
          m_next_flag(FLAGS_ALL[(task_id + 1) % 3])
    {}

    const char *GetTraceName() const override { return m_name; }

private:
    void Run() override
    {
        // get a start of the timeline (race is acceptable here)
        g_Timeline = stk::GetTicks();

        while (true)
        {
            // block until this task's flag is set; auto-cleared on return
            uint32_t result = g_TaskFlags.Wait(m_my_flag, stk::sync::EventFlags::OPT_WAIT_ANY);
            if (stk::sync::EventFlags::IsError(result))
                continue;

            // Atomically switch the active LED.
            {
                stk::hw::ScopedCriticalSection __guard;
                SwitchOnLED(static_cast<LedId>(m_task_id));
            }

            // sleep 1s drift-free and then delegate work to the next task
            // we could use simple stk::Sleep() but due to other work around Sleep call we
            // will get a time drift, STK allows to sleep until exact timestamp making it
            // possible precise sleeping with 1 tick precision, you could also use
            // time::TimerHost for timer-related tasks (see related 'timer' example)
            stk::SleepUntil(g_Timeline += stk::GetTicksFromMs(1000));

            // hand off to the next task
            g_TaskFlags.Set(m_next_flag);
        }
    }

    static void SwitchOnLED(uint8_t id)
    {
        Board::LedSet(bsp::Led::RED,    id == 0);
        Board::LedSet(bsp::Led::GREEN,  id == 1);
        Board::LedSet(bsp::Led::ORANGE, id == 2); // DISC1 has no BLUE user LED at the same position
        Board::LedSet(bsp::Led::BLUE,   false);   // keep BLUE as "system suspended" indicator (see ISR)
    }
};

// ============================================================================
//   ADC timer - fires every 2 s, samples the internal temperature sensor,
//               and publishes each reading into g_AdcPipe.
//
//   Replaces the former AdcTask: no dedicated kernel task is consumed.
//   The callback executes in the TimerHost handler task context; it must
//   complete quickly and must not block.
// ============================================================================

class AdcTimer final : public stk::time::TimerHost::Timer
{
public:
    void OnExpired(stk::time::TimerHost * /*host*/) override
    {
        // Perform a single blocking ADC conversion.
        // Board::AdcRead() is a tight spin on EOC — acceptable here because
        // the conversion takes < 5 µs (480 sample cycles @ 21 MHz ADC clock).
        AdcSample sample;
        sample.raw       = Board::AdcRead();
        sample.timestamp = stk::GetTicks();

        // Push the sample into the pipe (non-blocking).
        // TryWrite() is used so the handler task is never blocked by a slow
        // consumer; if the pipe is full (LogTask is stalled) the oldest
        // unread samples will accumulate until the pipe drains.
        // Switch to Write() (blocking) if you must not drop samples, but be
        // aware that blocking inside OnExpired() stalls ALL pending timers.
        if (!g_AdcPipe.TryWrite(sample))
        {
            // Pipe full — log task is not keeping up.
            // The sample is dropped; add an overflow counter here if needed.
        }
    }
};

// ============================================================================
//   Log task - blocks on g_AdcPipe and logs every received sample.
// ============================================================================
//
//   Design notes:
//   - Read() blocks indefinitely (WAIT_INFINITE) so the task consumes zero
//     CPU time when no ADC data is available - no busy-polling.
//   - printf() is called outside any lock; the task owns its own stack-local
//     copy of the sample, so no shared-state issues arise.
//   - A 10 s timeout on Read() is used instead of WAIT_INFINITE so the task
//     can periodically print a "heartbeat" even if the ADC timer is suspended
//     (e.g. when the kernel is paused by the USER button).
// ============================================================================

template <stk::EAccessMode _AccessMode>
class LogTask final : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
public:
    const char *GetTraceName() const override { return "log"; }

private:
    void Run() override
    {
        while (true)
        {
            AdcSample sample;

            // Block until a sample arrives or 2 s elapse.
            if (!g_AdcPipe.Read(sample, stk::GetTicksFromMs(10000)))
            {
                // Timeout: ADC timer may be frozen (misbehavior).
                // Print a warning so the console stays active.
                printf("[log] t=%d s warning: no ADC sample for 10 s\n",
                        static_cast<int>(sample.timestamp / 1000));
            }
            else
            {
                // Convert raw ADC counts to degrees Celsius.
                const float deg_c = Board::TEMP_ValueToDeg(sample.raw);

                // Log: tick timestamp, raw counts, and temperature.
                printf("[log] t=%d s  raw=%-4d  temp=%.2f C\n",
                       static_cast<int>(sample.timestamp / 1000),
                       static_cast<int>(sample.raw),
                       static_cast<float>(deg_c));
            }
        }
    }
};

// ============================================================================
//   EXTI0 ISR - USER button on PA0
// ============================================================================
//
//   Toggles the kernel between running and suspended states.
//   While suspended all task wake-ups and context switches are frozen,
//   which effectively pauses the entire application.
//
//   The BLUE LED acts as a "suspended" indicator:
//     ON  -> kernel is suspended
//     OFF -> kernel is running normally
// ============================================================================

extern "C" void EXTI0_IRQHandler()
{
    // Note: You would need a debouncing logic to handle the button press
    // reliably, it is outside the scope of this example though.

    // Clear the EXTI pending bit first to acknowledge the interrupt.
    EXTI->PR = (1U << 0);

    stk::IKernelService *kernel = stk::IKernelService::GetInstance();

    if (!g_KernelSuspended)
    {
        // Suspend() is ISR-safe. It returns the number of ticks until the
        // nearest scheduled wake-up (useful for tickless operation), but we
        // do not need that value here since we resume on the next button press.
        kernel->Suspend();

        g_KernelSuspended = true;

        // Light the BLUE LED to signal the suspended state, all other - off.
        Board::LedSwitchOnExclusive(Led::BLUE);
    }
    else
    {
        // Resume() with 0 ticks because we could sleep indefinitely long, so it
        // is similar to restore from hibernation.
        kernel->Resume(0);

        g_KernelSuspended = false;

        // Extinguish the suspended-state indicator.
        Board::LedInit();
    }
}

// ============================================================================
//   RunExample - entry point
// ============================================================================

void RunExample()
{
    using namespace stk;

    // - Board initialization -------------------------------------------------
    Board::LedInit();       // LEDs
    Board::AdcInit();       // CPU temperature reader
    Board::ButtonInit();    // Standby button
    Board::RtcWakeupInit(); // RTC for deep-sleep wake-up

    // - Kernel setup ---------------------------------------------------------
    // KERNEL_STATIC  - tasks never exit (required for the while(true) pattern).
    // KERNEL_TICKLESS - kernel suppresses SysTick firings when all tasks sleep,
    //                   amplifying the power benefit of OnSleep()+WFI.
    //
    // Task count reduced from 5 to 4: the former AdcTask is replaced by an
    // AdcTimer managed by TimerHost (which adds its own internal tasks).
    const uint8_t KernelMode = KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // 4 user tasks: 3 LED + 1 Log.
    // TimerHost::TASK_COUNT additional internal tasks are registered separately.
    static Kernel<KernelMode, 4 + stk::time::TimerHost::TASK_COUNT, SwitchStrategyRR, PlatformDefault> kernel;

    // Tasks run in privileged mode - GPIO and ADC registers require it on
    // Cortex-M4 when the MPU restricts peripheral access to privileged code.
    static LedTask<ACCESS_PRIVILEGED> task_red (0, "LED-red");
    static LedTask<ACCESS_PRIVILEGED> task_grn (1, "LED-grn");
    static LedTask<ACCESS_PRIVILEGED> task_org (2, "LED-org");
    static LogTask<ACCESS_PRIVILEGED> task_log;

    kernel.Initialize();

    // - TimerHost setup ------------------------------------------------------
    // Initialize the host and register it with the kernel.  Must be called
    // after kernel.Initialize() and before kernel.Start().
    // ACCESS_PRIVILEGED is required so the handler task can call AdcRead()
    // which accesses peripheral registers.
    static stk::time::TimerHost timer_host;
    timer_host.Initialize(&kernel, ACCESS_PRIVILEGED);

    // ADC sampling timer: periodic, fires every 1 s (~1 Hz).
    static AdcTimer adc_timer;
    const uint32_t adc_period = stk::GetTicksFromMs(2000);
    timer_host.Start(adc_timer, adc_period, adc_period);

    // - Low-power event overrider --------------------------------------------
    // Must be registered before kernel.Start() so the platform driver sees it
    // from the very first idle period.
    static LowPowerOverrider lp_overrider;
    kernel.GetPlatform()->SetEventOverrider(&lp_overrider);

    // - Start scheduling -----------------------------------------------------
    kernel.AddTask(&task_red);
    kernel.AddTask(&task_grn);
    kernel.AddTask(&task_org);
    kernel.AddTask(&task_log);

    // Start() never returns in KERNEL_STATIC mode.
    kernel.Start();

    STK_ASSERT(false);
}
