/* --------------------------------------------------------------------------
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
 *      Purpose: SuperTinyKernel RTOS via FreeRTOS example program
 *               (converted from CMSIS RTOS2 wrapper)
 *
 *---------------------------------------------------------------------------*/

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "RTE_Components.h"             // CMSIS_device_header definition, Example driver include
#include CMSIS_device_header            // STK config

/* Task handles
 * CMSIS: osThreadId_t  ->  FreeRTOS: TaskHandle_t                           */
TaskHandle_t tid_phaseA;                /* Task handle of task: phase_a      */
TaskHandle_t tid_phaseB;                /* Task handle of task: phase_b      */
TaskHandle_t tid_phaseC;                /* Task handle of task: phase_c      */
TaskHandle_t tid_phaseD;                /* Task handle of task: phase_d      */
TaskHandle_t tid_clock;                 /* Task handle of task: clock        */

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
 *      Function 'signal_func' called from multiple threads
 *
 *      CMSIS -> FreeRTOS mapping:
 *        osThreadFlagsSet(tid, mask) -> xTaskNotify(tid, mask, eSetBits)
 *        osDelay(ms)                 -> vTaskDelay(pdMS_TO_TICKS(ms))
 *---------------------------------------------------------------------------*/
void signal_func (TaskHandle_t tid)  {
  xTaskNotify(tid_clock, 0x0100, eSetBits);   /* set notification to clock task  */
  vTaskDelay(pdMS_TO_TICKS(500));             /* delay 500ms                     */
  xTaskNotify(tid_clock, 0x0100, eSetBits);   /* set notification to clock task  */
  vTaskDelay(pdMS_TO_TICKS(500));             /* delay 500ms                     */
  xTaskNotify(tid, 0x0001, eSetBits);         /* set notification to next task   */
  vTaskDelay(pdMS_TO_TICKS(500));             /* delay 500ms                     */
}

/*----------------------------------------------------------------------------
 *      Task 1 'phaseA': Phase A output
 *
 *      CMSIS -> FreeRTOS mapping:
 *        osThreadFlagsWait(mask, osFlagsWaitAny, osWaitForever)
 *        -> xTaskNotifyWait(0, mask, &flags, portMAX_DELAY)
 *---------------------------------------------------------------------------*/
void phaseA (void *argument) {
  uint32_t flags;
  for (;;) {
    xTaskNotifyWait(0x00000000, 0x0001, &flags, portMAX_DELAY); /* wait for notification bit 0x0001 */
    Switch_On(0);
    g_phases.phaseA = 1;
    signal_func(tid_phaseB);                                    /* call common signal function      */
    g_phases.phaseA = 0;
    Switch_Off(0);
  }
}

/*----------------------------------------------------------------------------
 *      Task 2 'phaseB': Phase B output
 *---------------------------------------------------------------------------*/
void phaseB (void *argument) {
  uint32_t flags;
  for (;;) {
    xTaskNotifyWait(0x00000000, 0x0001, &flags, portMAX_DELAY); /* wait for notification bit 0x0001 */
    Switch_On(1);
    g_phases.phaseB = 1;
    signal_func(tid_phaseC);                /* call common signal function   */
    g_phases.phaseB = 0;
    Switch_Off(1);
  }
}

/*----------------------------------------------------------------------------
 *      Task 3 'phaseC': Phase C output
 *---------------------------------------------------------------------------*/
void phaseC (void *argument) {
  uint32_t flags;
  for (;;) {
    xTaskNotifyWait(0x00000000, 0x0001, &flags, portMAX_DELAY); /* wait for notification bit 0x0001 */
    Switch_On(2);
    g_phases.phaseC = 1;
    signal_func(tid_phaseD);               /* call common signal function   */
    g_phases.phaseC = 0;
    Switch_Off(2);
  }
}

/*----------------------------------------------------------------------------
 *      Task 4 'phaseD': Phase D output
 *---------------------------------------------------------------------------*/
void phaseD (void *argument) {
  uint32_t flags;
  for (;;) {
    xTaskNotifyWait(0x00000000, 0x0001, &flags, portMAX_DELAY); /* wait for notification bit 0x0001 */
    Switch_On(3);
    g_phases.phaseD = 1;
    signal_func(tid_phaseA);               /* call common signal function   */
    g_phases.phaseD = 0;
    Switch_Off(3);
  }
}

/*----------------------------------------------------------------------------
 *      Task 5 'clock': Signal Clock
 *---------------------------------------------------------------------------*/
void clock_task (void *argument) {
  uint32_t flags;
  for (;;) {
    xTaskNotifyWait(0x00000000, 0x0100, &flags, portMAX_DELAY); /* wait for notification bit 0x0100 */
    vTaskDelay(pdMS_TO_TICKS(80));          /* delay  80ms                   */
  }
}

/*----------------------------------------------------------------------------
 *      app_main: Create tasks and kick off the phase chain
 *
 *      CMSIS -> FreeRTOS mapping:
 *        osThreadNew(fn, arg, attr) -> xTaskCreate(fn, name, stack, arg, pri, handle)
 *        osThreadFlagsSet(tid, mask) -> xTaskNotify(tid, mask, eSetBits)
 *        osDelay(osWaitForever)     -> vTaskSuspend(NULL)   (suspend self permanently)
 *---------------------------------------------------------------------------*/
void app_main (void *argument) {

  /* Create phase tasks
   * Stack size (256 words) and priority (2) are typical starting values;
   * tune them to suit your application's requirements.                      */
  xTaskCreate(phaseA,    "phaseA", 256, NULL, 2, &tid_phaseA);
  xTaskCreate(phaseB,    "phaseB", 256, NULL, 2, &tid_phaseB);
  xTaskCreate(phaseC,    "phaseC", 256, NULL, 2, &tid_phaseC);
  xTaskCreate(phaseD,    "phaseD", 256, NULL, 2, &tid_phaseD);
  xTaskCreate(clock_task,"clock",  256, NULL, 2, &tid_clock );

  xTaskNotify(tid_phaseA, 0x0001, eSetBits);  /* set notification to phaseA task */

  vTaskSuspend(NULL);                         /* suspend app_main permanently    */
}

/*----------------------------------------------------------------------------
 *      Main: Initialize and start FreeRTOS Kernel
 *
 *      CMSIS -> FreeRTOS mapping:
 *        osKernelInitialize()  -> (implicit — FreeRTOS needs no explicit init)
 *        osThreadNew(...)      -> xTaskCreate(...)
 *        osKernelGetState()    -> (not needed — vTaskStartScheduler asserts internally)
 *        osKernelStart()       -> vTaskStartScheduler()
 *---------------------------------------------------------------------------*/
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

  /* Create the application main task
   * Stack size (256 words) is intentionally larger here because app_main
   * itself spawns all other tasks before suspending.                        */
  xTaskCreate(app_main, "app_main", 256, NULL, 2, NULL);

  /* Start the FreeRTOS scheduler — does not return on success               */
  vTaskStartScheduler();

  /* Should never reach here; loop defensively                               */
  while(1);
}
