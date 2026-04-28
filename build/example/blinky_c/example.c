/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_config.h>
#include <stk_c.h>
#include <stk_c_time.h>
#include <stk_c_memory.h>
#include "example.h"

#define STACK_SIZE 256

static volatile uint8_t g_TaskSwitch = 0;
static uint32_t g_Stack[STK_C_KERNEL_MAX_TASKS][STACK_SIZE] __stk_c_stack;

static void InitLeds()
{
    Led_Init(LED_RED, false);
    Led_Init(LED_GREEN, false);
    Led_Init(LED_BLUE, false);
}

// Task function switching the LED
static void SwitchOnLED(uint8_t task_id)
{
    switch (task_id)
    {
    case 0:
        Led_Set(LED_RED, true);
        Led_Set(LED_GREEN, false);
        Led_Set(LED_BLUE, false);
        break;
    case 1:
        Led_Set(LED_RED, false);
        Led_Set(LED_GREEN, true);
        Led_Set(LED_BLUE, false);
        break;
    case 2:
        Led_Set(LED_RED, false);
        Led_Set(LED_GREEN, false);
        Led_Set(LED_BLUE, true);
        break;
    }
}

void TaskFunc(void *arg)
{
    uint8_t task_id = (uint8_t)((uintptr_t)arg);

    // just fake counters to demonstrate that scheduler is saving/restoring context correctly
    // preserving values of floating-point and 64 bit variables
    volatile float count = 0;
    volatile uint64_t count_skip = 0;

    while (true)
    {
        if (g_TaskSwitch != task_id)
        {
            // to avoid hot loop and excessive CPU usage sleep 10ms while waiting for the own turn,
            // if scheduler does not have active threads then it will fall into a sleep mode which is
            // saving the consumed power
            stk_sleep_ms(10);

            ++count_skip;
            continue;
        }

        ++count;

        // change LED state
        {
            stk_critical_section_enter();

            SwitchOnLED(task_id);

            stk_critical_section_exit();
        }

        // sleep 1s and delegate work to another task switching another LED
        stk_sleep_ms(1000);
        g_TaskSwitch = (task_id + 1) % 3;
    }
}

void RunExample()
{
    InitLeds();

    // allocate scheduling kernel
    stk_kernel_t *k = stk_kernel_create(0);

    // init kernel with default periodicity - 1ms tick
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

    // using privileged tasks as some MCUs may not allow writing to GPIO from a user thread, such ARM Cortex-M7/M33/...
    stk_task_t *t1 = stk_task_create_privileged(TaskFunc, (void *)0, g_Stack[0], STACK_SIZE);
    stk_task_t *t2 = stk_task_create_privileged(TaskFunc, (void *)1, g_Stack[1], STACK_SIZE);
    stk_task_t *t3 = stk_task_create_privileged(TaskFunc, (void *)2, g_Stack[2], STACK_SIZE);

    stk_kernel_add_task(k, t1);
    stk_kernel_add_task(k, t2);
    stk_kernel_add_task(k, t3);

    // start scheduler (it will start threads added by stk_kernel_add_task), execution in main() will be blocked on this line
    stk_kernel_start(k);

    // shall not reach here after stk_kernel_start() was called
    STK_C_ASSERT(false);
}
