/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

//#define _STK_ASSERT_REDIRECT

#ifdef _STK_ASSERT_REDIRECT
#include <stdint.h>
extern void STK_ASSERT_IMPL(const char *err, const char *source, int32_t line);
#endif

#include <stk_c.h>
#include "example.h"

#define STK_EXAMPLE_DUALCORE (STK_C_CPU_COUNT > 1)
#define STK_EXAMPLE_USE_PIPE 1

#ifdef _STK_ASSERT_REDIRECT
void STK_ASSERT_IMPL(const char *err, const char *source, int32_t line)
{
    __stk_debug_break();
    while (true) {}
}
#endif

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

typedef enum LedState {
    LED_OFF = 0,
    LED_ON
} LedState;

// Pointers to synchronization primitives
#if STK_EXAMPLE_USE_PIPE
static stk_pipe_t     *g_CtrlSignalPipe;
#else
static stk_event_t    *g_EventReady, *g_EventSwitchOn, *g_EventSwitchOff;
#endif
static stk_mutex_t    *g_HwMutex;

// -----------------------------------------------------------------------------
// Task Entry Points (C functions)
// -----------------------------------------------------------------------------

// LED Task: Handles hardware interaction
static void LedTask_Run(void *arg)
{
    LedState task_id = (LedState)((size_t)arg);

    while (true)
    {
    #if STK_EXAMPLE_USE_PIPE
        if (!stk_pipe_read(g_CtrlSignalPipe, &task_id, STK_WAIT_INFINITE))
            continue;
    #else
        switch (task_id)
        {
        case LED_ON:
            if (!stk_event_wait(g_EventSwitchOn, STK_WAIT_INFINITE))
                continue;
            break;
        case LED_OFF:
            if (!stk_event_wait(g_EventSwitchOff, STK_WAIT_INFINITE))
                continue;
            break;
        }
    #endif

        // protect hardware access
        stk_mutex_lock(g_HwMutex);
        Led_Set(LED_GREEN, (task_id == LED_OFF ? false : true));
        stk_mutex_unlock(g_HwMutex);

    #if !STK_EXAMPLE_USE_PIPE
        stk_event_set(g_EventReady);
    #endif
    }
}

// Control Task: Logic and timing
static void CtrlTask_Run(void *arg)
{
    (void)arg;

#if !STK_EXAMPLE_USE_PIPE
    stk_event_set(g_EventReady);
#endif

    bool led_sw = false;

    // we employ drift-free timing which is not affected by any work done before and after
    // calling stk_sleep_until
    stk_tick_t timeline = stk_ticks();
    const stk_timeout_t period = stk_ticks_from_ms(250);

    while (true)
    {
    #if !STK_EXAMPLE_USE_PIPE
        if (!stk_event_wait(g_EventReady, STK_WAIT_INFINITE))
            continue;
    #endif

        // sleep in exact periods drift-free
        timeline += period;
        stk_sleep_until(timeline);

        led_sw = !led_sw;

    #if STK_EXAMPLE_USE_PIPE
        LedState new_state = led_sw ? LED_ON : LED_OFF;
        stk_pipe_write(g_CtrlSignalPipe, &new_state, STK_WAIT_INFINITE);
    #else
        if (led_sw)
            stk_event_set(g_EventSwitchOn);
        else
            stk_event_set(g_EventSwitchOff);
    #endif
    }
}

// -----------------------------------------------------------------------------
// Initialization and Startup
// -----------------------------------------------------------------------------

static void InitLeds()
{
    Led_Init(LED_GREEN, false);
}

static void InitSync()
{
    // Memory containers for sync primitives (static allocation)
    static stk_mutex_mem_t g_HwMtxMem;
#if STK_EXAMPLE_USE_PIPE
    static stk_pipe_mem_t  g_PipeMem;
    static LedState        s_PipeBuf[16] __stk_c_aligned;
#else
    static stk_event_mem_t g_EvRdyMem, g_EvOnMem, g_EvOffMem;
#endif

    g_HwMutex = stk_mutex_create(&g_HwMtxMem, sizeof(g_HwMtxMem));

#if STK_EXAMPLE_USE_PIPE
    g_CtrlSignalPipe = stk_pipe_create(&g_PipeMem, sizeof(g_PipeMem),
                                        s_PipeBuf, sizeof(s_PipeBuf),
                                        16, sizeof(LedState));
#else
    g_EventReady     = stk_event_create(&g_EvRdyMem, sizeof(g_EvRdyMem), false);
    g_EventSwitchOn  = stk_event_create(&g_EvOnMem,  sizeof(g_EvOnMem),  false);
    g_EventSwitchOff = stk_event_create(&g_EvOffMem, sizeof(g_EvOffMem), false);
#endif
}

#if STK_EXAMPLE_DUALCORE

static size_t g_StackCore0_T1[TASK_STACK_SIZE] __stk_c_stack;
#if !STK_EXAMPLE_USE_PIPE
static size_t g_StackCore0_T2[TASK_STACK_SIZE] __stk_c_stack;
#endif
static size_t g_StackCore1_T1[TASK_STACK_SIZE] __stk_c_stack;

void StartCore0()
{
    stk_kernel_t *k = stk_kernel_create(0);
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

    stk_task_t *t1 = stk_task_create_privileged(LedTask_Run, (void *)LED_OFF, g_StackCore0_T1, TASK_STACK_SIZE);
    stk_kernel_add_task(k, t1);

#if !STK_EXAMPLE_USE_PIPE
    stk_task_t *t2 = stk_task_create_privileged(LedTask_Run, (void *)LED_ON, g_StackCore0_T2, TASK_STACK_SIZE);
    stk_kernel_add_task(k, t2);
#endif

    stk_kernel_start(k);

    // shall not reach here after stk_kernel_start() was called
    STK_C_ASSERT(false);
}

void StartCore1()
{
    stk_kernel_t *k = stk_kernel_create(1);
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

    stk_task_t *t1 = stk_task_create_user(CtrlTask_Run, NULL, g_StackCore1_T1, TASK_STACK_SIZE);
    stk_kernel_add_task(k, t1);

    stk_kernel_start(k);

    // shall not reach here after stk_kernel_start() was called
    STK_C_ASSERT(false);
}

void RunExample()
{
    InitLeds();
    InitSync();

    // Start cores (MCU specific CPU start implementation)
    Cpu_Start(1, StartCore1);
    Cpu_Start(0, StartCore0);
}

#else

static size_t g_StackT1[TASK_STACK_SIZE] __stk_c_stack_attr;
#if !STK_EXAMPLE_USE_PIPE
static size_t g_StackT2[TASK_STACK_SIZE] __stk_c_stack_attr;
#endif
static size_t g_StackT3[TASK_STACK_SIZE] __stk_c_stack_attr;

void RunExample()
{
    InitLeds();
    InitSync();

    stk_kernel_t *k = stk_kernel_create(0);
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

    // add LED tasks
    stk_kernel_add_task(k, stk_task_create_privileged(LedTask_Run, (void *)LED_OFF, g_StackT1, TASK_STACK_SIZE));
#if !STK_EXAMPLE_USE_PIPE
    stk_kernel_add_task(k, stk_task_create_privileged(LedTask_Run, (void *)LED_ON, g_StackT2, TASK_STACK_SIZE));
#endif

    // add Control task
    stk_kernel_add_task(k, stk_task_create_user(CtrlTask_Run, NULL, g_StackT3, TASK_STACK_SIZE));

    stk_kernel_start(k);

    // shall not reach here after stk_kernel_start() was called
    STK_C_ASSERT(false);
}

#endif
