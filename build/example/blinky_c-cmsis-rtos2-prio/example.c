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
 *      Purpose: Priority Inheritance demonstration via CMSIS RTOS2.
 *
 *      Scenario:
 *        Three threads compete for a shared mutex:
 *          - LowPrio  (osPriorityBelowNormal) : acquires mutex first, holds it 5 s
 *          - MidPrio  (osPriorityNormal)      : spins in a busy loop (no RTOS blocking),
 *                                               starving LowPrio when inheritance is OFF
 *          - HighPrio (osPriorityAboveNormal) : waits 1 s, then tries to acquire mutex
 *
 *      Without osMutexPrioInherit:
 *        MidPrio always preempts LowPrio, so LowPrio never finishes its 5 s delay,
 *        the mutex is never released, and HighPrio is blocked indefinitely
 *        (classic priority inversion).
 *
 *      With osMutexPrioInherit (enabled here):
 *        The moment HighPrio blocks on the mutex, LowPrio inherits HighPrio's
 *        priority, preempts MidPrio, completes the 5 s delay, and releases the
 *        mutex. HighPrio then acquires it immediately.
 *
 *      LED mapping:
 *        LED_RED   (led 0) – LowPrio  holds mutex (on) / sleeping (off)
 *        LED_GREEN (led 1) – MidPrio  running busy loop
 *        LED_BLUE  (led 2) – HighPrio holds mutex
 *
 *---------------------------------------------------------------------------*/

#include <stdio.h>

#include "cmsis_os2.h"          // ARM::CMSIS:RTOS2
#include "RTE_Components.h"     // CMSIS_device_header definition, Example driver include
#include CMSIS_device_header    // STK config

// When 1 the RED led will be periodically blinking because MidPrioThread will cooperate
// and periodically release time to LowPrioThread when MidPrioThread is sleeping.
// For STK though such cooperation fix is not required as it supports priority inheritance,
// so it will work just fine with 0 or 1.
#define FIX_COOPERATIVE_BEHAVIOR (0)

/* --------------------------------------------------------------------------
 *  Mutex
 * --------------------------------------------------------------------------
 *  osMutexRecursive is NOT used — only one acquire per thread at a time.
 *  osMutexPrioInherit enables the priority inheritance protocol.
 *  Remove osMutexPrioInherit to observe priority inversion (HighPrio blocks
 *  forever while MidPrio spins).
 * -------------------------------------------------------------------------*/
osMutexId_t mutex_id;

static const osMutexAttr_t mutex_attr = {
  "PrioInheritMutex",   /* human-readable name                    */
  osMutexPrioInherit,   /* attr_bits – enables priority inherit.  */
  NULL,                 /* memory for control block (auto)        */
  0U                    /* size  for control block (auto)         */
};

/* Thread handles */
osThreadId_t tid_high;
osThreadId_t tid_mid;
osThreadId_t tid_low;

/*----------------------------------------------------------------------------
 *      LED helpers
 *---------------------------------------------------------------------------*/
static LedId to_hw_led (unsigned char led) {
    switch (led){
    case 0: return LED_RED;
    case 1: return LED_GREEN;
    case 2: return LED_BLUE;
    default: return LED_ORANGE;
    }
}

static void led_on (unsigned char led) {
  //printf("LED On:  #%d\n", led);
  Led_Set(to_hw_led(led), true);
}

static void led_off (unsigned char led) {
  //printf("LED Off: #%d\n", led);
  Led_Set(to_hw_led(led), false);
}

/*----------------------------------------------------------------------------
 *      Thread: LowPrio  (osPriorityBelowNormal)
 *
 *      Immediately acquires the mutex and holds it for 5 s (simulates a
 *      long critical section).  LED_RED is on while the mutex is held.
 *      After releasing it sleeps another 5 s before repeating.
 *---------------------------------------------------------------------------*/
void LowPrioThread (void *argument) {
  (void)argument;

  for (;;) {
    //printf("[Low ] Acquiring mutex...\n");
    osMutexAcquire(mutex_id, osWaitForever);

    led_on(0);                        /* LED_RED on: mutex acquired            */
    //printf("[Low ] Mutex acquired – holding for 5 s\n");

    osDelay(5000U);                   /* hold mutex 5 s                        */

    //printf("[Low ] Releasing mutex\n");
    osMutexRelease(mutex_id);
    led_off(0);                       /* LED_RED off: mutex released           */

    osDelay(5000U);                   /* sleep before next cycle               */
  }
}

/*----------------------------------------------------------------------------
 *      Thread: MidPrio  (osPriorityNormal)
 *
 *      Waits 1 s (same as HighPrio) then spins in a tight busy loop without
 *      calling any blocking RTOS API.  This is the "villain" of priority
 *      inversion: without inheritance it starves LowPrio indefinitely.
 *      LED_GREEN blinks every 200 ms to show the thread is running.
 *---------------------------------------------------------------------------*/
void MidPrioThread (void *argument) {
  (void)argument;

  osDelay(1000U);                     /* align start with High/Low scenario    */

  for (;;) {
    /* Non-blocking busy work – intentionally avoids any RTOS block call      */
    led_on(1);                        /* LED_GREEN on                          */
    //printf("[Mid ] Running busy loop (non-blocking)\n");

    /* Busy payload without blocking call to the kernel (osThreadYield, osDelay)
     * will not allow stk::SwitchStrategyFP32 to switch to the LowPrioThread priority
     * thread which holds mutex blocking execution of HighPrioThread.
     */
    for (volatile int32_t i = 0; i < 1000000; ++i);
    led_off(1);
    for (volatile int32_t i = 0; i < 1000000; ++i);

    /*
     * Use a short cooperative yield so the system can still process timers,
     * but this is NOT a blocking wait on a mutex/semaphore/flag – MidPrio
     * will always re-schedule before LowPrio (absent priority inheritance).
     */
#if FIX_COOPERATIVE_BEHAVIOR
    osThreadYield();
#endif
  }
}

/*----------------------------------------------------------------------------
 *      Thread: HighPrio  (osPriorityAboveNormal)
 *
 *      Delays 1 s to let LowPrio acquire the mutex first, then blocks on
 *      the mutex.  With priority inheritance LowPrio inherits HighPrio's
 *      priority and completes promptly; without it HighPrio blocks forever.
 *      LED_BLUE is on while HighPrio holds the mutex.
 *---------------------------------------------------------------------------*/
void HighPrioThread (void *argument) {
  (void)argument;

  osDelay(1000U);                     /* let LowPrio grab mutex first          */

  for (;;) {
    //printf("[High] Trying to acquire mutex...\n");

    osMutexAcquire(mutex_id, osWaitForever);
    /* ------------------------------------------------------------------
     * If priority inheritance is working correctly we reach this point
     * ~5 s after LowPrio first acquired the mutex.  Without inheritance
     * we never get here while MidPrio is running.
     * ----------------------------------------------------------------*/
    led_on(2);                        /* LED_BLUE on: mutex acquired           */
    //printf("[High] Mutex acquired – doing high-priority work\n");

    osDelay(500U);                    /* short critical section                */

    //printf("[High] Releasing mutex\n");
    osMutexRelease(mutex_id);
    led_off(2);                       /* LED_BLUE off                          */

    osDelay(9500U);                   /* wait before next cycle (~10 s total)  */
  }
}

/*----------------------------------------------------------------------------
 *      app_main: create mutex and threads, then wait forever
 *---------------------------------------------------------------------------*/
void app_main (void *argument) {
  (void)argument;

  /* Create mutex with priority inheritance                                   */
  mutex_id = osMutexNew(&mutex_attr);

  /* Thread attributes – only the priority differs between threads            */
  const osThreadAttr_t low_attr = {
    .name     = "LowPrio",
    .priority = osPriorityBelowNormal,
  };
  const osThreadAttr_t mid_attr = {
    .name     = "MidPrio",
    .priority = osPriorityNormal,
  };
  const osThreadAttr_t high_attr = {
    .name     = "HighPrio",
    .priority = osPriorityAboveNormal,
  };

  tid_low  = osThreadNew(LowPrioThread,  NULL, &low_attr);
  tid_mid  = osThreadNew(MidPrioThread,  NULL, &mid_attr);
  tid_high = osThreadNew(HighPrioThread, NULL, &high_attr);

  osDelay(osWaitForever);
}

/*----------------------------------------------------------------------------
 *      main: system init and kernel start
 *---------------------------------------------------------------------------*/
#ifndef _STK_STANDALONE_EXAMPLE
void app_run()
#else
int main(int argc, char* argv[])
#endif
{
  /* System Initialization */
  SystemCoreClockUpdate();

  /* Init LEDs (see to_hw_led):
   *   led 0 = LED_RED   -> LowPrio holds mutex
   *   led 1 = LED_GREEN -> MidPrio busy loop
   *   led 2 = LED_BLUE  -> HighPrio holds mutex
   */
  Led_InitAll(false);

  osKernelInitialize();
  osThreadNew(app_main, NULL, NULL);
  if (osKernelGetState() == osKernelReady) {
    osKernelStart();
  }

  while (1);
}
