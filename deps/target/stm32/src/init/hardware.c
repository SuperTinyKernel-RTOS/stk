/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdint.h>

#include "cmsis_device.h"
#include "diag/trace.h"

/*! \file  hardware.c
    \brief Hardware initialization hooks of the STK STM32Fx target: device system initialization, HAL and clocks.

    Called by the start-up code (startup.c) in this order:
     1. \c __initialize_hardware_early() before RAM is initialized;
     2. \c __initialize_hardware() after RAM is initialized.
    All functions here are weak, an application replaces the ones it needs.
    \note  The HAL tick is driven by SysTick. Interrupts are disabled until main() is called, so HAL timeouts (e.g. waiting
           for a missing crystal in SystemClock_Config) do not expire during this initialization. The kernel takes SysTick
           over by defining its own \c SysTick_Handler; until then the default handler (exceptions.c) advances the HAL tick.
*/

/*! \brief Start of the vector table, defined by the linker script. */
extern const uint32_t __vectors_start;

void __initialize_hardware_early(void) __attribute__ ((weak));
void __initialize_hardware(void) __attribute__ ((weak));
void SystemClock_Config(void) __attribute__ ((weak));

void __initialize_hardware_early(void)
{
    // reset the clock tree and (STM32F4/F7) enable the FPU, as provided by the CMSIS device package
    SystemInit();

#if (__CORTEX_M >= 3)
    // the vector table is where the linker script has put it (the system file assumes the start of the flash)
    SCB->VTOR = (uint32_t)&__vectors_start;
#endif

#if defined(__FPU_USED) && (__FPU_USED == 1U)
    // full access to the coprocessors CP10 and CP11 (FPU)
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
    __DSB();
    __ISB();
#endif
}

void __initialize_hardware(void)
{
    // must be the first HAL call, also starts the 1 ms tick of the HAL
    HAL_Init();

    SystemClock_Config();

    // SystemCoreClock is a RAM variable, which was cleared with the start-up of RAM: recompute it
    SystemCoreClockUpdate();
}

#if defined(STM32F4) || defined(STM32F7)

/*! \def   STK_STM32FX_SYSCLK_168MHZ
    \brief Devices which run at 168 MHz with this configuration, the others at 84 MHz.
*/
#if defined(STM32F405xx) || defined(STM32F415xx) || defined(STM32F407xx) || defined(STM32F417xx) || \
    defined(STM32F427xx) || defined(STM32F437xx) || defined(STM32F429xx) || defined(STM32F439xx) || \
    defined(STM32F446xx) || defined(STM32F7)
#define STK_STM32FX_SYSCLK_168MHZ 1
#endif

#endif // STM32F4 || STM32F7

void SystemClock_Config(void)
{
#if defined(STM32F4) || defined(STM32F7)

    // regulator scale 1 allows the maximum frequency
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    // PLL from HSE (HSE_VALUE is a multiple of 1 MHz): VCO = 2 * 336 MHz / PLLP
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM            = (HSE_VALUE / 1000000U);
    osc.PLL.PLLN            = 336;
    osc.PLL.PLLQ            = 7; // 48 MHz for USB
#if defined(STK_STM32FX_SYSCLK_168MHZ)
    osc.PLL.PLLP            = RCC_PLLP_DIV2; // 168 MHz
#else
    osc.PLL.PLLP            = RCC_PLLP_DIV4; // 84 MHz
#endif
    HAL_RCC_OscConfig(&osc);

    // PLL as the system clock, bus dividers keep APB1 <= 42 MHz and APB2 <= 84 MHz
    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
#if defined(STK_STM32FX_SYSCLK_168MHZ)
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5);
#else
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);
#endif

#endif // STM32F4 || STM32F7

    // STM32F0 and STM32F1 keep the default clock (HSI, 8 MHz), override this function to change it

    // 1 ms tick from the (possibly new) core clock
    HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000U);
    HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);
}
