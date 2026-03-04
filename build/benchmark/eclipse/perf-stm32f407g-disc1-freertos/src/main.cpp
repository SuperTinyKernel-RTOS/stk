/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "cmsis_device.h"
#include "perf.h"
#include "FreeRTOS.h"
#include "task.h"

#define SLEEP_GRANULARITY (_STK_BENCH_WINDOW / portTICK_PERIOD_MS + 2)

static StackType_t g_TaskStack[_STK_BENCH_TASK_MAX + 1][_STK_BENCH_STACK_SIZE] = {};
static StaticTask_t g_Task[_STK_BENCH_TASK_MAX + 1] = {};
static volatile TaskHandle_t g_TaskHandle[_STK_BENCH_TASK_MAX + 1] = {};
static volatile uint32_t g_Ticks = 0;
static volatile bool g_Enable = false;

extern "C" void SysTick_Handler()
{
    //HAL_IncTick();

    if (g_Enable)
        ++g_Ticks;

    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        xPortSysTickHandler();
}

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];

    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

extern "C" void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize)
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

// Use the same sleep function as in STK to make CPU consumption by a sleep routine comparable
extern "C" void vApplicationIdleHook(void)
{
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk; // disable deep-sleep, go into a WAIT mode (sleep)
    __DSB();                            // ensure store takes effect (see ARM info)
    __WFI();                            // enter sleep mode
}

static void BenchTask(void *pvParameter)
{
    uint32_t index = (uint32_t)pvParameter;

    // do processing
    g_Enable = true;
    while (g_Ticks < _STK_BENCH_WINDOW)
    {
        g_Bench[index].Process();
    }

    TaskHandle_t handle = g_TaskHandle[index];
    g_TaskHandle[index] = NULL;

    vTaskDelete(handle);
    for (;;);
}

static void BenchEnd(void *pvParameter)
{
    (void)pvParameter;

    while (g_Ticks < _STK_BENCH_WINDOW + 2)
    {
        vTaskDelay(SLEEP_GRANULARITY);
    }

wait:
    for (int32_t i = 0; i < _STK_BENCH_TASK_MAX; ++i)
    {
        if (g_TaskHandle[i] != NULL)
            goto wait;
    }

    Crc32Bench::ShowResults();
    for (;;);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for (int32_t i = 0; i < _STK_BENCH_TASK_MAX; ++i)
    {
        g_Bench[i].Initialize();
        g_TaskHandle[i] = xTaskCreateStatic(&BenchTask, "", _STK_BENCH_STACK_SIZE, (void *)i, 0, g_TaskStack[i], &g_Task[i]);
    }

    g_TaskHandle[_STK_BENCH_TASK_MAX] = xTaskCreateStatic(&BenchEnd, "", _STK_BENCH_STACK_SIZE, (void *)-1, 0, g_TaskStack[_STK_BENCH_TASK_MAX], &g_Task[_STK_BENCH_TASK_MAX]);

    vTaskStartScheduler();
    return 0;
}

