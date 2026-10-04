/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*
 * Blinky built on the STK pthread API (stk_c_pthread.h).
 *
 * Four LED threads pass a "baton" to each other: only the thread whose turn it is
 * lights its LED (exclusively), keeps it on for 1 s, then hands the turn to the next
 * thread. Mutex + condition variable replace the EventFlags used in example.c.
 *
 * Kernel requirements (stk_config.h):
 *   - STK_C_KERNEL_TYPE_CPU_0 must be a KERNEL_DYNAMIC kernel, with KERNEL_SYNC for
 *     the mutex/condvar, e.g.:
 *       #define STK_C_KERNEL_TYPE_CPU_0 Kernel<KERNEL_DYNAMIC | KERNEL_SYNC, STK_C_KERNEL_MAX_TASKS, SwitchStrategyRR, PlatformDefault>
 *   - STK_C_KERNEL_MAX_TASKS >= 1 (main task) + LED_MAX (threads) + 1 (internal reaper,
 *     created lazily) = 6 for 4 LEDs.
 */

#include <pthread.h>
#include "example.h"

enum { MAIN_STACK_SIZE = 256, THREAD_STACK_SIZE = 256 }; // in stk_word_t units

// Bootstrap (main) task stack and one stack per LED thread
STK_DEFINE_STACK_POOL(g_MainStack,   1,       MAIN_STACK_SIZE);
STK_DEFINE_STACK_POOL(g_ThreadStack, LED_MAX, THREAD_STACK_SIZE);

// Baton: the id of the LED thread whose turn it is (RED goes first)
static pthread_mutex_t g_Lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_Turn  = PTHREAD_COND_INITIALIZER;
static uint32_t        g_Baton = LED_RED;

// Timeline for precise (drift-free) LED switching
static stk_tick_t g_Timeline = 0;

static pthread_t g_Threads[LED_MAX];

// Evaluate a pthread call and assert it returned 0 (stays side-effect safe with NDEBUG)
#define PT_CHECK(call) do { int rc_ = (call); STK_C_ASSERT(rc_ == 0); (void)rc_; } while (0)

static void *LedThread(void *arg)
{
    const uint32_t id   = (uint32_t)(uintptr_t)arg;
    const uint32_t next = (id + 1U) % LED_MAX;
    const stk_tick_t period = stk_ticks_from_ms(250);

    while (true)
    {
        // block until it is this thread's turn
        PT_CHECK(pthread_mutex_lock(&g_Lock));
        while (g_Baton != id)
        {
            PT_CHECK(pthread_cond_wait(&g_Turn, &g_Lock));
        }
        PT_CHECK(pthread_mutex_unlock(&g_Lock));

        // change LED state
        {
            stk_critical_section_enter();
            Led_SwitchOnExclusive((LedId)id);
            stk_critical_section_exit();
        }

        // sleep 1s drift-free: only the baton holder touches g_Timeline, so no locking needed
        g_Timeline += period;
        stk_sleep_until(g_Timeline);

        // hand off to the next thread
        PT_CHECK(pthread_mutex_lock(&g_Lock));
        g_Baton = next;
        PT_CHECK(pthread_cond_broadcast(&g_Turn));
        PT_CHECK(pthread_mutex_unlock(&g_Lock));
    }

    return NULL; // never reached
}

// Bootstrap task: creates the LED threads, then waits for them (they never finish)
static void MainTask(void *arg)
{
    (void)arg;

    // initialize explicitly (instead of lazily on first use) to avoid any first-use race
    PT_CHECK(pthread_mutex_init(&g_Lock, NULL));
    PT_CHECK(pthread_cond_init(&g_Turn, NULL));

    // start of the timeline
    g_Timeline = stk_ticks();

    for (uint32_t i = 0; i < LED_MAX; i++)
    {
        pthread_attr_t attr;
        PT_CHECK(pthread_attr_init(&attr));

        // caller-owned static stack: avoids the default 1024-word pool and malloc()
        PT_CHECK(pthread_attr_setstack(&attr, STK_GET_STACK_FROM_POOL(g_ThreadStack, i),
                                       sizeof(g_ThreadStack[i])));

        // privileged thread: some MCUs (e.g. Cortex-M7/M33) do not allow GPIO access from user mode
        PT_CHECK(pthread_attr_setprivileged_np(&attr, PTHREAD_CREATE_PRIVILEGED_NP));

        PT_CHECK(pthread_create(&g_Threads[i], &attr, LedThread, (void *)(uintptr_t)i));
        PT_CHECK(pthread_attr_destroy(&attr));
    }

    // block forever
    for (uint32_t i = 0; i < LED_MAX; i++)
    {
        pthread_join(g_Threads[i], NULL);
    }
}

void RunExample()
{
    Led_InitAll(false);

    // allocate scheduling kernel (KERNEL_DYNAMIC + KERNEL_SYNC required for pthreads)
    stk_kernel_t *k = stk_kernel_create(0);
    STK_C_ASSERT(k != NULL);

    // init kernel with default periodicity - 1ms tick
    stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

    // tell the pthread shim which kernel pthread_create() adds threads to
    stk_pthread_bind_kernel(k);

    stk_task_t *main_task = stk_task_create_user(MainTask, NULL,
                                                 STK_GET_STACK_FROM_POOL(g_MainStack, 0),
                                                 MAIN_STACK_SIZE);
    STK_C_ASSERT(main_task != NULL);

    stk_kernel_add_task(k, main_task);

    // start scheduler, execution in main() will be blocked on this line
    stk_kernel_start(k);

    // shall not reach here after stk_kernel_start() was called
    STK_C_ASSERT(false);
}
