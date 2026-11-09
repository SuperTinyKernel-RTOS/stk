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
 *      Purpose: SuperTinyKernel RTOS via Infineon abstraction-rtos API example program
 *               (converted from the ThreadX API wrapper example)
 *
 *---------------------------------------------------------------------------*/

/*! \file  blinky_c-cyabs_rtos/example.c
    \brief SuperTinyKernel RTOS via Infineon abstraction-rtos API example program.
    
    \example blinky_c-cyabs_rtos/example.c
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "cyabs_rtos.h"                 // Infineon abstraction-rtos API
#include "cyabs_rtos_stk.h"             // kernel start of the STK port

#include "RTE_Components.h"             // CMSIS_device_header definition, Example driver include
#include CMSIS_device_header            // STK config

#define STACK_SIZE          1024U       /* bytes, per thread (>= CY_RTOS_MIN_STACK_SIZE) */
#define APP_PRIORITY        CY_RTOS_PRIORITY_NORMAL /* all threads share one priority    */

#define FLAG_PHASE_A        0x0001UL    /* wake-up bit of thread phaseA      */
#define FLAG_PHASE_B        0x0002UL    /* wake-up bit of thread phaseB      */
#define FLAG_PHASE_C        0x0004UL    /* wake-up bit of thread phaseC      */
#define FLAG_PHASE_D        0x0008UL    /* wake-up bit of thread phaseD      */
#define FLAG_CLOCK          0x0100UL    /* wake-up bit of thread clock       */

/* Halt on an API failure (replace with the application error handler) */
#define APP_CHECK(call)     do { if ((call) != CY_RSLT_SUCCESS) { for (;;) {} } } while (0)

/* Thread handles (the "thread id" of the other examples) */
cy_thread_t          tid_app_main;      /* Thread: app_main                  */
cy_thread_t          tid_phaseA;        /* Thread: phase_a                   */
cy_thread_t          tid_phaseB;        /* Thread: phase_b                   */
cy_thread_t          tid_phaseC;        /* Thread: phase_c                   */
cy_thread_t          tid_phaseD;        /* Thread: phase_d                   */
cy_thread_t          tid_clock;         /* Thread: clock                     */

cy_event_t           g_flags;           /* replaces osThreadFlags / notifications */

/* Thread stacks (static, must be aligned: 8 bytes, 16 on RISC-V) */
#define STACK_DECL(name)    static uint8_t name[STACK_SIZE] __attribute__((aligned(16)))
STACK_DECL(stack_app_main);
STACK_DECL(stack_phaseA);
STACK_DECL(stack_phaseB);
STACK_DECL(stack_phaseC);
STACK_DECL(stack_phaseD);
STACK_DECL(stack_clock);

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
static void wait_flag (uint32_t flag) {
  uint32_t bits = flag;                              /* in: bits to wait for, out: flags */
  (void)cy_rtos_event_waitbits(&g_flags, &bits, true /* clear */, false /* any */,
                               CY_RTOS_NEVER_TIMEOUT);
}

/*----------------------------------------------------------------------------
 *      Function 'signal_func' called from multiple threads
 *---------------------------------------------------------------------------*/
void signal_func (uint32_t next_flag)  {
  cy_rtos_event_setbits(&g_flags, FLAG_CLOCK);      /* set signal to clock thread    */
  cy_rtos_delay_milliseconds(500);                  /* delay 500ms                   */
  cy_rtos_event_setbits(&g_flags, FLAG_CLOCK);      /* set signal to clock thread    */
  cy_rtos_delay_milliseconds(500);                  /* delay 500ms                   */
  cy_rtos_event_setbits(&g_flags, next_flag);       /* set signal to next thread     */
  cy_rtos_delay_milliseconds(500);                  /* delay 500ms                   */
}

/*----------------------------------------------------------------------------
 *      Thread 1 'phaseA': Phase A output
 *---------------------------------------------------------------------------*/
void phaseA (cy_thread_arg_t argument) {
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
void phaseB (cy_thread_arg_t argument) {
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
void phaseC (cy_thread_arg_t argument) {
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
void phaseD (cy_thread_arg_t argument) {
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
void clock_thread (cy_thread_arg_t argument) {
  (void)argument;
  for (;;) {
    wait_flag(FLAG_CLOCK);                          /* wait for an event flag        */
    cy_rtos_delay_milliseconds(80);                 /* delay  80ms                   */
  }
}

/*----------------------------------------------------------------------------
 *      Main application thread: create the threads and kick off the phase chain
 *---------------------------------------------------------------------------*/
void app_main (cy_thread_arg_t argument) {
  (void)argument;

  APP_CHECK(cy_rtos_thread_create(&tid_phaseA, phaseA, "phaseA",
                                  stack_phaseA, STACK_SIZE, APP_PRIORITY, 0));
  APP_CHECK(cy_rtos_thread_create(&tid_phaseB, phaseB, "phaseB",
                                  stack_phaseB, STACK_SIZE, APP_PRIORITY, 0));
  APP_CHECK(cy_rtos_thread_create(&tid_phaseC, phaseC, "phaseC",
                                  stack_phaseC, STACK_SIZE, APP_PRIORITY, 0));
  APP_CHECK(cy_rtos_thread_create(&tid_phaseD, phaseD, "phaseD",
                                  stack_phaseD, STACK_SIZE, APP_PRIORITY, 0));
  APP_CHECK(cy_rtos_thread_create(&tid_clock,  clock_thread, "clock",
                                  stack_clock,  STACK_SIZE, APP_PRIORITY, 0));

  cy_rtos_event_setbits(&g_flags, FLAG_PHASE_A);    /* set signal to phaseA thread   */

  /* sleep forever (osWaitForever): nobody sends a notification to this thread */
  (void)cy_rtos_thread_wait_notification(CY_RTOS_NEVER_TIMEOUT);
}

/*----------------------------------------------------------------------------
 *      Application init (called by cy_rtos_stk_kernel_enter before the scheduler starts)
 *
 *      Replaces tx_application_define(): the kernel objects are created here,
 *      then the kernel starts scheduling.
 *---------------------------------------------------------------------------*/
static void app_init (void) {
  APP_CHECK(cy_rtos_event_init(&g_flags));

  /* Create application main thread */
  APP_CHECK(cy_rtos_thread_create(&tid_app_main, app_main, "app_main",
                                  stack_app_main, STACK_SIZE, APP_PRIORITY, 0));
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

  (void)cy_rtos_stk_kernel_enter(app_init);   // Start STK kernel (does not return)

  while(1);
}
