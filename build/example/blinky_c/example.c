/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk_c.h>
#include <stk_c_time.h>
#include <stk_c_memory.h>
#include "example.h"

enum { STACK_SIZE = 256 };

// One flag bit per LED task; task 0 (RED) goes first
static const uint32_t FLAGS_ALL[] = {
    (1U << LED_RED),
    (1U << LED_ORANGE),
    (1U << LED_GREEN),
    (1U << LED_BLUE)
};

// EventFlags object and its backing memory
static stk_ef_mem_t g_TaskFlagsMem;
static stk_ef_t    *g_TaskFlags;

// Stack of the tasks
STK_DEFINE_STACK_POOL(g_Stack, STK_C_KERNEL_MAX_TASKS, STACK_SIZE);

// Per-task argument passed through the void* arg
typedef struct {
    uint8_t  task_id;
    uint32_t my_flag;
    uint32_t next_flag;
} TaskArg;

static TaskArg g_TaskArgs[LED_MAX];

void TaskFunc(void *arg)
{
    const TaskArg *a = (const TaskArg *)arg;

    while (true)
    {
        // block until this task's flag is set; auto-cleared on return
        uint32_t result = stk_ef_wait(g_TaskFlags, a->my_flag, STK_EF_OPT_WAIT_ANY, STK_WAIT_INFINITE);
        if (stk_ef_is_error(result))
            continue;

        // change LED state
        {
            stk_critical_section_enter();
            Led_SwitchOnExclusive((LedId)a->task_id);
            stk_critical_section_exit();
        }

        // sleep 1s and delegate work to the next task
        stk_sleep_ms(1000);

        // hand off to the next task
        stk_ef_set(g_TaskFlags, a->next_flag);
    }
}

void RunExample()
{
    Led_InitAll(false);

    // initialize per-task argument structs
    for (uint8_t i = 0; i < LED_MAX; i++)
    {
        g_TaskArgs[i].task_id   = i;
        g_TaskArgs[i].my_flag   = FLAGS_ALL[i];
        g_TaskArgs[i].next_flag = FLAGS_ALL[(i + 1) % LED_MAX];
    }

    // create EventFlags with the RED task's flag pre-set so it runs first
    g_TaskFlags = stk_ef_create(&g_TaskFlagsMem, sizeof(g_TaskFlagsMem), FLAGS_ALL[LED_RED]);
    STK_C_ASSERT(g_TaskFlags != NULL);

    // allocate scheduling kernel (KERNEL_SYNC required for EventFlags)
    stk_kernel_t *k = stk_kernel_create(0);
    STK_C_ASSERT(k != NULL);

    // init kernel with default periodicity - 1ms tick
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

    // using privileged tasks as some MCUs may not allow writing to GPIO from a user thread, such ARM Cortex-M7/M33/...
    stk_task_t *t1 = stk_task_create_privileged(TaskFunc, &g_TaskArgs[LED_RED],    STK_GET_STACK_FROM_POOL(g_Stack, 0), STACK_SIZE);
    stk_task_t *t2 = stk_task_create_privileged(TaskFunc, &g_TaskArgs[LED_ORANGE], STK_GET_STACK_FROM_POOL(g_Stack, 1), STACK_SIZE);
    stk_task_t *t3 = stk_task_create_privileged(TaskFunc, &g_TaskArgs[LED_GREEN],  STK_GET_STACK_FROM_POOL(g_Stack, 2), STACK_SIZE);
    stk_task_t *t4 = stk_task_create_privileged(TaskFunc, &g_TaskArgs[LED_BLUE],   STK_GET_STACK_FROM_POOL(g_Stack, 3), STACK_SIZE);

    stk_kernel_add_task(k, t1);
    stk_kernel_add_task(k, t2);
    stk_kernel_add_task(k, t3);
    stk_kernel_add_task(k, t4);

    // start scheduler (it will start threads added by stk_kernel_add_task), execution in main() will be blocked on this line
    stk_kernel_start(k);

    // shall not reach here after stk_kernel_start() was called
    STK_C_ASSERT(false);
}
