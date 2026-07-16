/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 *
 * --------------------------------------------------------------------------------------------
 * Low-Power Demo - STM32F407G-DISC1  (pure-C translation)
 * --------------------------------------------------------------------------------------------
 *
 * Hardware used on the STM32F407G-DISC1 discovery board:
 *   * USER button  - PA0 (active-HIGH, no external pull needed)
 *   * LEDs         - PD12 (GREEN), PD13 (ORANGE), PD14 (RED), PD15 (BLUE)
 *   * Internal ADC - ADC1 channel 16 (on-chip temperature sensor, VBAT/2 mux)
 *
 * Behavior: identical to example.cpp; see that file for a full description.
 *
 * Translation notes
 * -----------------
 *   C++ class / feature          C equivalent
 *   ─────────────────────────────────────────────────────────────────────────
 *   stk::sync::EventFlags        stk_ef_t  (stk_ef_create / stk_ef_wait …)
 *   stk::sync::PipeT<T,N>        stk_pipe_t (stk_pipe_create / stk_pipe_*)
 *   stk::Task<N,mode>            stk_task_create_privileged + static stack
 *   stk::time::TimerHost         stk_timerhost_t (stk_timerhost_get/init)
 *   stk::time::TimerHost::Timer  stk_timer_t (stk_timer_create + callback)
 *   IPlatform::IEventOverrider   stk_event_overrider_t (stk_kernel_set_event_overrider)
 *   IKernelService               stk_critical_section_enter/exit +
 *                                stk_kernel_suspend / stk_kernel_resume
 *   kernel.GetPlatform()->
 *     SetEventOverrider()        stk_kernel_set_event_overrider()
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <stk_c.h>
#include <stk_c_time.h>
#include "example.h"   /* bsp LED/button ids - same header as the C++ version */

/* ============================================================================
 *   stk_config.h must define:
 *     #define STK_C_KERNEL_TYPE_CPU_0  \
 *         stk::Kernel<KERNEL_STATIC | KERNEL_SYNC | KERNEL_TICKLESS,
 *                     4 + stk::time::TimerHost::TASK_COUNT,
 *                     stk::SwitchStrategyRR,
 *                     stk::PlatformDefault>
 *   (Replace KERNEL_TICKLESS with 0 when STK_TICKLESS_IDLE is not enabled.)
 * ============================================================================ */

/* ============================================================================
 *   ADC sample descriptor
 * ============================================================================ */

typedef struct
{
    int64_t  timestamp; /* stk_tick_t: kernel tick at the time of conversion */
    uint16_t raw;       /* raw 12-bit ADC value                               */
} AdcSample;

/* ============================================================================
 *   Shared state
 * ============================================================================ */

/* One flag bit per LED task; task 0 (RED) runs first. */
#define FLAG_RED    (1U << LED_RED)
#define FLAG_GREEN  (1U << LED_GREEN)
#define FLAG_ORANGE (1U << LED_ORANGE)

static const uint32_t FLAGS_ALL[3] = { FLAG_RED, FLAG_GREEN, FLAG_ORANGE };

/* EventFlags - replaces stk::sync::EventFlags g_TaskFlags */
static stk_ef_mem_t  s_TaskFlagsMem;
static stk_ef_t     *g_TaskFlags;

/* CriticalSection - guards exclusive LED access from the LED tasks */
static stk_cs_mem_t  s_CritSecMem;
static stk_cs_t      *g_CritSec;

/* Precise LED timeline - replaces stk::Ticks g_Timeline */
static stk_tick_t g_Timeline = 0;

/* Pipe carrying AdcSample structs - replaces stk::sync::PipeT<AdcSample, 8> */
static stk_pipe_mem_t  s_AdcPipeMem;
static uint8_t         s_AdcPipeBuf[8 * sizeof(AdcSample)];
static stk_pipe_t     *g_AdcPipe;

/* Kernel-suspended flag: set/cleared in the EXTI ISR. */
static volatile bool g_KernelSuspended = false;

/* Kernel handle - needed by the EXTI ISR to call Suspend/Resume. */
static stk_kernel_t *g_Kernel;

/* ============================================================================
 *   Board-specific helpers  (identical logic to example.cpp)
 * ============================================================================ */

/* --- GPIO / LED ------------------------------------------------------------ */

static void LedInit(void)
{
    Led_InitAll(false);
}

static inline void LedSet(int pin, bool on)
{
    Led_Set(pin, on);
}

static inline void LedSwitchOnExclusive(int pin)
{
    Led_SwitchOnExclusive(pin);
}

/* --- ADC1 ------------------------------------------------------------------ */

static void AdcInit(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    __DSB();

    ADC->CCR     |= ADC_CCR_TSVREFE;
    ADC1->CR1     = 0;
    ADC1->CR2     = 0;
    ADC1->SMPR1   = (7U << 18);
    ADC1->SQR3    = 16U;
    ADC1->SQR1    = 0;

    ADC1->CR2    |= ADC_CR2_ADON;
    for (volatile uint32_t i = 0; i < 10000; ++i) {}
}

static inline void AdcSuspend(void)
{
    ADC1->CR2 &= ~ADC_CR2_ADON;
}

static inline void AdcResume(void)
{
    if ((ADC1->CR2 & ADC_CR2_ADON) != ADC_CR2_ADON)
    {
        ADC1->CR2 |= ADC_CR2_ADON;
        for (volatile uint32_t i = 0; i < 500; ++i) {}
    }
}

static inline uint16_t AdcRead(void)
{
    AdcResume();
    ADC1->SR  &= ~ADC_SR_EOC;
    ADC1->CR2 |=  ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC)) {}
    return (uint16_t)ADC1->DR;
}

static inline float TEMP_ValueToDeg(uint32_t sens)
{
    static const float to_V  = 3.3f / 4095.0f;
    static const float slope = 2.5f / 1000.0f;
    static const float v_cal = 1060.0f * (3.3f / 4095.0f);
    static const float t_cal = 26.0f;
    return (sens * to_V - v_cal) / slope + t_cal;
}

/* --- CPU / PLL ------------------------------------------------------------- */

static inline void RestorePllClock(void)
{
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {}

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}
}

static inline void CpuEnterDeepSleepMode(void)
{
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    __DSB();
    __WFI();
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    RestorePllClock();
}

static inline void CpuEnterSleepMode(void)
{
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    __DSB();
    __WFI();
}

/* --- USER button (PA0, active-HIGH) + EXTI0 -------------------------------- */

static void ButtonInit(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    __DSB();

    GPIOA->MODER &= ~(3U << 0);
    GPIOA->PUPDR &= ~(3U << 0);

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    __DSB();
    SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0] & ~SYSCFG_EXTICR1_EXTI0)
                       | SYSCFG_EXTICR1_EXTI0_PA;

    EXTI->RTSR |=  (1U << 0);
    EXTI->FTSR &= ~(1U << 0);
    EXTI->IMR  |=  (1U << 0);

    NVIC_SetPriority(EXTI0_IRQn, 8);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

/* --- RTC Wakeup Timer ------------------------------------------------------ */

#define RTC_WKUP_COUNTS_PER_TICK 2U
#define RTC_WKUP_GUARD_COUNTS    2U
#define RTC_WKUP_MAX_COUNT       0xFFFFU

static void RtcWakeupInit(void)
{
    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {}

    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    __DSB();
    PWR->CR |= PWR_CR_DBP;

    if ((RCC->BDCR & RCC_BDCR_RTCSEL) != RCC_BDCR_RTCSEL_1)
    {
        RCC->BDCR |=  RCC_BDCR_BDRST;
        RCC->BDCR &= ~RCC_BDCR_BDRST;
        RCC->BDCR |=  RCC_BDCR_RTCSEL_1;
    }

    RCC->BDCR |= RCC_BDCR_RTCEN;
    __DSB();

    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;

    RTC->ISR |= RTC_ISR_INIT;
    while (!(RTC->ISR & RTC_ISR_INITF)) {}

    RTC->PRER = (127U << RTC_PRER_PREDIV_A_Pos) | (249U << RTC_PRER_PREDIV_S_Pos);
    RTC->ISR  &= ~RTC_ISR_INIT;

    RTC->WPR = 0xFF;

    EXTI->IMR  |= (1U << 22);
    EXTI->RTSR |= (1U << 22);
    EXTI->FTSR &= ~(1U << 22);

    NVIC_SetPriority(RTC_WKUP_IRQn, 12);
    NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

static inline void RtcWakeupArm(int32_t sleep_ticks)
{
    uint32_t counts = (uint32_t)sleep_ticks * RTC_WKUP_COUNTS_PER_TICK;
    if (counts > RTC_WKUP_MAX_COUNT)
        counts = RTC_WKUP_MAX_COUNT;
    counts = (counts > RTC_WKUP_GUARD_COUNTS) ? (counts - RTC_WKUP_GUARD_COUNTS) : 1U;

    RTC->WPR  = 0xCA;
    RTC->WPR  = 0x53;
    RTC->CR  &= ~RTC_CR_WUTE;
    while (!(RTC->ISR & RTC_ISR_WUTWF)) {}
    RTC->ISR &= ~RTC_ISR_WUTF;
    EXTI->PR  =  (1U << 22);
    RTC->CR   = (RTC->CR & ~RTC_CR_WUCKSEL) | 0x0U;
    RTC->WUTR = (counts > 0U) ? (counts - 1U) : 0U;
    RTC->CR  |= RTC_CR_WUTIE | RTC_CR_WUTE;
    RTC->WPR  = 0xFF;
}

static inline void RtcWakeupDisarm(void)
{
    RTC->WPR  = 0xCA;
    RTC->WPR  = 0x53;
    RTC->CR  &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WPR  = 0xFF;
    EXTI->PR  =  (1U << 22);
}

/* ============================================================================
 *   RTC Wakeup ISR
 * ============================================================================ */

void RTC_WKUP_IRQHandler(void)
{
    RTC->WPR  = 0xCA;
    RTC->WPR  = 0x53;
    RTC->ISR &= ~RTC_ISR_WUTF;
    RTC->WPR  = 0xFF;
    EXTI->PR  =  (1U << 22);
}

/* ============================================================================
 *   Low-power event overrider
 *
 *   Replaces LowPowerOverrider::OnSleep().
 *   The stk_event_overrider_t is installed via stk_kernel_set_event_overrider().
 * ============================================================================ */

#define TICKS_IDLE_SLEEP 8
#define TICKS_DEEP_SLEEP 40

static bool OnSleep(int32_t sleep_ticks, void *user_data)
{
    (void)user_data;

    // Note: For extended processing here we must increase the stack of
    //       the sleep task using STK_SLEEP_TRAP_STACK_SIZE in stk_config.h.
    //       Based on processing inside OnSleep you would need more or less
    //       stack. 256 looks optimal for this implementation.
    //       You can always inspect Kernel::m_sleep_trap->memory stack array
    //       and check how it is filled (not touched memory is filled with
    //       STK_STACK_MEMORY_FILLER.
    assert(STK_SLEEP_TRAP_STACK_SIZE >= 256);

    if ((sleep_ticks >= TICKS_DEEP_SLEEP) || g_KernelSuspended)
    {
        if (!g_KernelSuspended)
        {
            /* Arm RTC wakeup timer. */
            RtcWakeupArm(sleep_ticks);

            /* Suspend kernel ticking. */
            stk_kernel_suspend(g_Kernel);
        }

        AdcSuspend();
        CpuEnterDeepSleepMode();
        RtcWakeupDisarm();
        AdcResume();

        if (!g_KernelSuspended)
        {
            stk_kernel_resume(g_Kernel, sleep_ticks);

            /* Return false: let driver wait in idle sleep until context switch. */
            return false;
        }
    }
    else
    if (sleep_ticks > TICKS_IDLE_SLEEP)
    {
        AdcSuspend();
        CpuEnterSleepMode();
        AdcResume();
    }
    else
    {
        CpuEnterSleepMode();
    }

    /* Return true: driver re-enters OnSleep() until an ISR breaks the sleep. */
    return true;
}

/* ============================================================================
 *   ADC timer callback
 *
 *   Replaces AdcTimer::OnExpired().
 *   Fired every 2 s by the TimerHost; samples the temperature sensor and
 *   pushes the result into g_AdcPipe (non-blocking).
 * ============================================================================ */

static void AdcTimerCallback(stk_timerhost_t *host,
                             stk_timer_t     *timer,
                             void            *user_data)
{
    (void)host;
    (void)timer;
    (void)user_data;

    AdcSample sample;
    sample.raw       = AdcRead();
    sample.timestamp = stk_ticks();

    stk_pipe_trywrite(g_AdcPipe, &sample);
}

/* ============================================================================
 *   LED task
 *
 *   Replaces LedTask<ACCESS_PRIVILEGED>.
 *   Three instances share the same entry function, differentiated by a
 *   per-instance context struct passed through the void* arg.
 * ============================================================================ */

typedef struct
{
    uint8_t  task_id;
    uint32_t my_flag;
    uint32_t next_flag;
} LedTaskCtx;

static LedTaskCtx s_LedCtx[3];

static void LedTaskEntry(void *arg)
{
    LedTaskCtx *ctx = (LedTaskCtx *)arg;

    g_Timeline = stk_ticks();

    while (true)
    {
        uint32_t result = stk_ef_wait(g_TaskFlags,
                                      ctx->my_flag,
                                      STK_EF_OPT_WAIT_ANY,
                                      STK_WAIT_INFINITE);
        if (stk_ef_is_error(result))
            continue;

        /* Atomically switch the active LED. */
        stk_cs_enter(g_CritSec);
        LedSet(LED_RED,    ctx->task_id == 0);
        LedSet(LED_GREEN,  ctx->task_id == 1);
        LedSet(LED_ORANGE, ctx->task_id == 2);
        stk_cs_exit(g_CritSec);

        /* Drift-free 1 s sleep. */
        stk_sleep_until(g_Timeline += stk_ticks_from_ms(1000));

        /* Hand off to the next LED task. */
        stk_ef_set(g_TaskFlags, ctx->next_flag);
    }
}

/* ============================================================================
 *   Log task
 *
 *   Replaces LogTask<ACCESS_PRIVILEGED>.
 *   Blocks on g_AdcPipe; logs every received AdcSample.
 * ============================================================================ */

static void LogTaskEntry(void *arg)
{
    (void)arg;

    while (true)
    {
        AdcSample sample;

        /* Block up to 10 s for a sample (matches the C++ 10000 ms timeout). */
        bool ok = stk_pipe_read(g_AdcPipe, &sample, stk_ticks_from_ms(10000));

        if (!ok)
        {
            printf("[log] t=%d s warning: no ADC sample for 10 s\n",
                   (int)(sample.timestamp / 1000));
        }
        else
        {
            float deg_c = TEMP_ValueToDeg(sample.raw);
            printf("[log] t=%d s  raw=%-4d  temp=%.2f C\n",
                   (int)(sample.timestamp / 1000),
                   (int)sample.raw,
                   (double)deg_c);
        }
    }
}

/* ============================================================================
 *   EXTI0 ISR - USER button (PA0)
 *
 *   Replaces the C++ EXTI0_IRQHandler.
 *   Toggles kernel between running and suspended via stk_kernel_suspend /
 *   stk_kernel_resume.  BLUE LED signals the suspended state.
 * ============================================================================ */

void EXTI0_IRQHandler(void)
{
    EXTI->PR = (1U << 0); /* acknowledge */

    if (!g_KernelSuspended)
    {
        stk_kernel_suspend(g_Kernel);

        g_KernelSuspended = true;
        LedSwitchOnExclusive(LED_BLUE);
    }
    else
    {
        stk_kernel_resume(g_Kernel, 0);

        g_KernelSuspended = false;
        LedInit();
    }
}

/* ============================================================================
 *   Task stacks
 * ============================================================================ */

#define TASK_STACK_SIZE 512

static __stk_c_stack stk_word_t s_StackRed[TASK_STACK_SIZE];
static __stk_c_stack stk_word_t s_StackGrn[TASK_STACK_SIZE];
static __stk_c_stack stk_word_t s_StackOrg[TASK_STACK_SIZE];
static __stk_c_stack stk_word_t s_StackLog[TASK_STACK_SIZE];

/* ============================================================================
 *   RunExample - entry point
 * ============================================================================ */

void RunExample(void)
{
    /* --- Board initialization ----------------------------------------------- */
    LedInit();
    AdcInit();
    ButtonInit();
    RtcWakeupInit();

    /* --- Kernel ------------------------------------------------------------- */
    /* STK_C_KERNEL_TYPE_CPU_0 must be configured in stk_config.h with:
     *   KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0)
     *   task count = 4 + TimerHost::TASK_COUNT
     *   strategy   = SwitchStrategyRR
     *   platform   = PlatformDefault                                            */
    g_Kernel = stk_kernel_create(0);
    stk_kernel_init(g_Kernel, STK_PERIODICITY_DEFAULT);

    /* --- Sync objects ------------------------------------------------------- */
    /* EventFlags: start with RED task's flag set so it runs first. */
    g_TaskFlags = stk_ef_create(&s_TaskFlagsMem, sizeof(s_TaskFlagsMem), FLAG_RED);

    /* CriticalSection: guards exclusive LED access from the LED tasks. */
    g_CritSec = stk_cs_create(&s_CritSecMem, sizeof(s_CritSecMem));

    /* Pipe for AdcSample structs, capacity 8. */
    g_AdcPipe = stk_pipe_create(&s_AdcPipeMem, sizeof(s_AdcPipeMem),
                                s_AdcPipeBuf,  sizeof(s_AdcPipeBuf),
                                8, sizeof(AdcSample));

    /* --- LED task contexts -------------------------------------------------- */
    for (int i = 0; i < 3; ++i)
    {
        s_LedCtx[i].task_id   = (uint8_t)i;
        s_LedCtx[i].my_flag   = FLAGS_ALL[i];
        s_LedCtx[i].next_flag = FLAGS_ALL[(i + 1) % 3];
    }

    /* --- Tasks -------------------------------------------------------------- */
    stk_task_t *task_red = stk_task_create_privileged(LedTaskEntry, &s_LedCtx[0],
                                                      s_StackRed, TASK_STACK_SIZE);
    stk_task_set_name(task_red, "LED-red");

    stk_task_t *task_grn = stk_task_create_privileged(LedTaskEntry, &s_LedCtx[1],
                                                      s_StackGrn, TASK_STACK_SIZE);
    stk_task_set_name(task_grn, "LED-grn");

    stk_task_t *task_org = stk_task_create_privileged(LedTaskEntry, &s_LedCtx[2],
                                                      s_StackOrg, TASK_STACK_SIZE);
    stk_task_set_name(task_org, "LED-org");

    stk_task_t *task_log = stk_task_create_privileged(LogTaskEntry, NULL,
                                                      s_StackLog, TASK_STACK_SIZE);
    stk_task_set_name(task_log, "log");

    /* --- TimerHost ---------------------------------------------------------- */
    stk_timerhost_t *timer_host = stk_timerhost_get(0);
    stk_timerhost_init(timer_host, g_Kernel, /*privileged=*/true);

    stk_timer_t *adc_timer = stk_timer_create(AdcTimerCallback, NULL);
    uint32_t adc_period    = (uint32_t)stk_ticks_from_ms(2000);
    stk_timer_start(timer_host, adc_timer, adc_period, adc_period);

    /* --- Low-power event overrider ------------------------------------------ */
    /* Replaces kernel.GetPlatform()->SetEventOverrider(&lp_overrider).
     * Must be called after stk_kernel_init() and before stk_kernel_start(). */
    static stk_event_overrider_t lp_overrider = {
        .on_sleep      = OnSleep,
        .on_hard_fault = NULL,
        .user_data     = NULL
    };
    stk_kernel_set_event_overrider(g_Kernel, &lp_overrider);

    /* --- Add tasks and start ------------------------------------------------ */
    stk_kernel_add_task(g_Kernel, task_red);
    stk_kernel_add_task(g_Kernel, task_grn);
    stk_kernel_add_task(g_Kernel, task_org);
    stk_kernel_add_task(g_Kernel, task_log);

    /* Never returns in KERNEL_STATIC mode. */
    stk_kernel_start(g_Kernel);
}
