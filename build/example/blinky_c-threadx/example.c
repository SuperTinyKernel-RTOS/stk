/* --------------------------------------------------------------------------
 * Copyright (c) 2013-2019 ARM Limited. All rights reserved.
 * Copyright (c) 2026 Neutron Code Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *      Purpose: SuperTinyKernel RTOS via ThreadX API wrapper example program
 *               (converted from the CMSIS RTOS2 / FreeRTOS examples)
 *
 *---------------------------------------------------------------------------*/

/*! \file  blinky_c-threadx/example.c
    \brief SuperTinyKernel RTOS via ThreadX API wrapper example program.
    
    \example blinky_c-threadx/example.c
*/

#include <stdio.h>

#include "tx_api.h"                     // ThreadX API on SuperTinyKernel

#include "RTE_Components.h"             // CMSIS_device_header definition, Example driver include
#include CMSIS_device_header            // STK config

/*----------------------------------------------------------------------------
 *      CMSIS-RTOS2 / FreeRTOS -> ThreadX mapping used by this example
 *
 *        osThreadNew / xTaskCreate     -> tx_thread_create
 *        osThreadFlagsSet / xTaskNotify-> tx_event_flags_set(TX_OR)
 *        osThreadFlagsWait /
 *          xTaskNotifyWait             -> tx_event_flags_get(TX_OR_CLEAR)
 *        osDelay / vTaskDelay          -> tx_thread_sleep
 *        osDelay(osWaitForever) /
 *          vTaskSuspend(NULL)          -> tx_thread_suspend(tx_thread_identify())
 *        osKernelInitialize + Start /
 *          vTaskStartScheduler         -> tx_kernel_enter
 *
 *      ThreadX has no per-thread flags, so a single event flags group is used
 *      and every thread waits on its own bit of that group.
 *---------------------------------------------------------------------------*/

#define STACK_SIZE          1024U       /* bytes, per thread                 */
#define APP_PRIORITY        2U          /* all threads share one priority    */

#define FLAG_PHASE_A        0x0001UL    /* wake-up bit of thread phaseA      */
#define FLAG_PHASE_B        0x0002UL    /* wake-up bit of thread phaseB      */
#define FLAG_PHASE_C        0x0004UL    /* wake-up bit of thread phaseC      */
#define FLAG_PHASE_D        0x0008UL    /* wake-up bit of thread phaseD      */
#define FLAG_CLOCK          0x0100UL    /* wake-up bit of thread clock       */

/* Milliseconds to ticks (rounded up), tick rate is TX_TIMER_TICKS_PER_SECOND */
#define MS_TO_TICKS(ms)     ((ULONG)((((ULONG)(ms) * TX_TIMER_TICKS_PER_SECOND) + 999UL) / 1000UL))

/* Control blocks (the "thread id" of the other examples) */
TX_THREAD            tid_app_main;      /* Thread: app_main                  */
TX_THREAD            tid_phaseA;        /* Thread: phase_a                   */
TX_THREAD            tid_phaseB;        /* Thread: phase_b                   */
TX_THREAD            tid_phaseC;        /* Thread: phase_c                   */
TX_THREAD            tid_phaseD;        /* Thread: phase_d                   */
TX_THREAD            tid_clock;         /* Thread: clock                     */

TX_EVENT_FLAGS_GROUP g_flags;           /* replaces osThreadFlags / notifications */

/* Thread stacks */
static ULONG         stack_app_main[STACK_SIZE / sizeof(ULONG)];
static ULONG         stack_phaseA[STACK_SIZE / sizeof(ULONG)];
static ULONG         stack_phaseB[STACK_SIZE / sizeof(ULONG)];
static ULONG         stack_phaseC[STACK_SIZE / sizeof(ULONG)];
static ULONG         stack_phaseD[STACK_SIZE / sizeof(ULONG)];
static ULONG         stack_clock [STACK_SIZE / sizeof(ULONG)];

struct phases_t {
  int_fast8_t phaseA;
  int_fast8_t phaseB;
  int_fast8_t phaseC;
  int_fast8_t phaseD;
} g_phases;


/*----------------------------------------------------------------------------
 *      Switch LED on
 *---------------------------------------------------------------------------*/
void Switch_On (unsigned char led) {
  //printf("LED On:  #%d\n", led);
  Led_Set((LedId)led, true);
}

/*----------------------------------------------------------------------------
 *      Switch LED off
 *---------------------------------------------------------------------------*/
void Switch_Off (unsigned char led) {
  //printf("LED Off: #%d\n", led);
  Led_Set((LedId)led, false);
}


/*----------------------------------------------------------------------------
 *      Wait for a wake-up bit (blocks forever, clears the bit on return)
 *---------------------------------------------------------------------------*/
static void wait_flag (ULONG flag) {
  ULONG actual = 0;
  (void)tx_event_flags_get(&g_flags, flag, TX_OR_CLEAR, &actual, TX_WAIT_FOREVER);
}

/*----------------------------------------------------------------------------
 *      Function 'signal_func' called from multiple threads
 *---------------------------------------------------------------------------*/
void signal_func (ULONG next_flag)  {
  tx_event_flags_set(&g_flags, FLAG_CLOCK, TX_OR);  /* set signal to clock thread    */
  tx_thread_sleep(MS_TO_TICKS(500));                /* delay 500ms                   */
  tx_event_flags_set(&g_flags, FLAG_CLOCK, TX_OR);  /* set signal to clock thread    */
  tx_thread_sleep(MS_TO_TICKS(500));                /* delay 500ms                   */
  tx_event_flags_set(&g_flags, next_flag, TX_OR);   /* set signal to next thread     */
  tx_thread_sleep(MS_TO_TICKS(500));                /* delay 500ms                   */
}

/*----------------------------------------------------------------------------
 *      Thread 1 'phaseA': Phase A output
 *---------------------------------------------------------------------------*/
void phaseA (ULONG argument) {
  (void)argument;
  for (;;) {
    wait_flag(FLAG_PHASE_A);                        /* wait for an event flag        */
    Switch_On(0);
    g_phases.phaseA = 1;
    signal_func(FLAG_PHASE_B);                      /* call common signal function   */
    g_phases.phaseA = 0;
    Switch_Off(0);
  }
}

/*----------------------------------------------------------------------------
 *      Thread 2 'phaseB': Phase B output
 *---------------------------------------------------------------------------*/
void phaseB (ULONG argument) {
  (void)argument;
  for (;;) {
    wait_flag(FLAG_PHASE_B);                        /* wait for an event flag        */
    Switch_On(1);
    g_phases.phaseB = 1;
    signal_func(FLAG_PHASE_C);                      /* call common signal function   */
    g_phases.phaseB = 0;
    Switch_Off(1);
  }
}

/*----------------------------------------------------------------------------
 *      Thread 3 'phaseC': Phase C output
 *---------------------------------------------------------------------------*/
void phaseC (ULONG argument) {
  (void)argument;
  for (;;) {
    wait_flag(FLAG_PHASE_C);                        /* wait for an event flag        */
    Switch_On(2);
    g_phases.phaseC = 1;
    signal_func(FLAG_PHASE_D);                      /* call common signal function   */
    g_phases.phaseC = 0;
    Switch_Off(2);
  }
}

/*----------------------------------------------------------------------------
 *      Thread 4 'phaseD': Phase D output
 *---------------------------------------------------------------------------*/
void phaseD (ULONG argument) {
  (void)argument;
  for (;;) {
    wait_flag(FLAG_PHASE_D);                        /* wait for an event flag        */
    Switch_On(3);
    g_phases.phaseD = 1;
    signal_func(FLAG_PHASE_A);                      /* call common signal function   */
    g_phases.phaseD = 0;
    Switch_Off(3);
  }
}

/*----------------------------------------------------------------------------
 *      Thread 5 'clock': Signal Clock
 *---------------------------------------------------------------------------*/
void clock_thread (ULONG argument) {
  (void)argument;
  for (;;) {
    wait_flag(FLAG_CLOCK);                          /* wait for an event flag        */
    tx_thread_sleep(MS_TO_TICKS(80));               /* delay  80ms                   */
  }
}

/*----------------------------------------------------------------------------
 *      Main application thread: create the threads and kick off the phase chain
 *---------------------------------------------------------------------------*/
void app_main (ULONG argument) {
  (void)argument;

  tx_thread_create(&tid_phaseA, "phaseA", phaseA, 0,
                   stack_phaseA, STACK_SIZE, APP_PRIORITY, APP_PRIORITY,
                   TX_NO_TIME_SLICE, TX_AUTO_START);
  tx_thread_create(&tid_phaseB, "phaseB", phaseB, 0,
                   stack_phaseB, STACK_SIZE, APP_PRIORITY, APP_PRIORITY,
                   TX_NO_TIME_SLICE, TX_AUTO_START);
  tx_thread_create(&tid_phaseC, "phaseC", phaseC, 0,
                   stack_phaseC, STACK_SIZE, APP_PRIORITY, APP_PRIORITY,
                   TX_NO_TIME_SLICE, TX_AUTO_START);
  tx_thread_create(&tid_phaseD, "phaseD", phaseD, 0,
                   stack_phaseD, STACK_SIZE, APP_PRIORITY, APP_PRIORITY,
                   TX_NO_TIME_SLICE, TX_AUTO_START);
  tx_thread_create(&tid_clock,  "clock",  clock_thread, 0,
                   stack_clock,  STACK_SIZE, APP_PRIORITY, APP_PRIORITY,
                   TX_NO_TIME_SLICE, TX_AUTO_START);

  tx_event_flags_set(&g_flags, FLAG_PHASE_A, TX_OR);    /* set signal to phaseA thread   */

  tx_thread_suspend(tx_thread_identify());              /* sleep forever (osWaitForever) */
}

/*----------------------------------------------------------------------------
 *      ThreadX application entry (called by tx_kernel_enter)
 *
 *      Replaces osKernelInitialize() / osThreadNew(app_main) / osKernelStart():
 *      the kernel objects are created here, then the kernel starts scheduling.
 *---------------------------------------------------------------------------*/
void tx_application_define (void *first_unused_memory) {
  (void)first_unused_memory;

  tx_event_flags_create(&g_flags, "flags");

  /* Create application main thread */
  tx_thread_create(&tid_app_main, "app_main", app_main, 0,
                   stack_app_main, STACK_SIZE, APP_PRIORITY, APP_PRIORITY,
                   TX_NO_TIME_SLICE, TX_AUTO_START);
}

#ifndef _STK_STANDALONE_EXAMPLE
void app_run()
#else
int main(int argc, char* argv[])
#endif
{
  // System Initialization
  SystemCoreClockUpdate();

  // Init LEDs
  Led_InitAll(false);

  // ...

  tx_kernel_enter();                    // Start ThreadX kernel (does not return)

  while(1);
}
