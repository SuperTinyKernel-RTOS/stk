/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Reinhard Panhuber
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/* plot_audio_samples.py requires following modules:
 * $ sudo apt install libportaudio
 * $ pip3 install sounddevice matplotlib
 *
 * Then run
 * $ python3 plot_audio_samples.py
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

// STK's own kernel type (STK_C_KERNEL_TYPE_CPU_0), CPU count and max-task count are project
// settings, expected to be defined in stk_config.h - see the "Kernel factory functions" section
// of stk_c.h. This example needs at least 3 task slots (blinky, usbd, audio).
#include "stk_c.h"

// STK has no built-in equivalent of FreeRTOS's configMINIMAL_STACK_SIZE (stack sizes are always
// caller-supplied, in words of stk_word_t) - pick a baseline here and size up per task below.
// Adjust for your target/toolchain.
#define STK_MINIMAL_STACK_SIZE 128U

// Increase stack size when debug log is enabled
#define USBD_STACK_SIZE ((4U * STK_MINIMAL_STACK_SIZE / 2U) * (CFG_TUSB_DEBUG ? 2U : 1U))

#define BLINKY_STACK_SIZE STK_MINIMAL_STACK_SIZE
#define AUDIO_STACK_SIZE STK_MINIMAL_STACK_SIZE

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+

/* Blink pattern
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum {
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

// task stacks - STK's C API keeps the task control block in its own internal static slot pool
// (sized via STK_C_KERNEL_MAX_TASKS), so unlike FreeRTOS's xTaskCreateStatic() only the stack
// memory itself needs to be supplied here.
static stk_word_t blinky_stack[BLINKY_STACK_SIZE];
static stk_word_t usb_device_stack[USBD_STACK_SIZE];
static stk_word_t audio_stack[AUDIO_STACK_SIZE];

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

// Audio controls
// Current states
bool mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];      // +1 for master channel 0
uint16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];// +1 for master channel 0
uint32_t sampFreq;
uint8_t clkValid;

// Range states
audio20_control_range_2_n_t(1) volumeRng[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];// Volume range state
audio20_control_range_4_n_t(1) sampleFreqRng;                                    // Sample frequency range state

// Audio test data
uint16_t test_buffer_audio[CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE / 1000 * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX / 2];
uint16_t startVal = 0;

void led_blinking_task(void *param);
void usb_device_task(void *param);
void audio_isr_task(void *param);

void board_init_after_tusb(void) {
  // nothing to do
}

/*------------- MAIN -------------*/
int main(int argc, char* argv[]) {
  (void) argc;
  (void) argv;

  board_init();

  // Init values
  sampFreq = CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE;
  clkValid = 1;

  sampleFreqRng.wNumSubRanges = 1;
  sampleFreqRng.subrange[0].bMin = CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bMax = CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bRes = 0;

  // STK kernel/task setup. Unlike FreeRTOS, tasks are created individually via
  // stk_task_create_privileged()/_user() and only take effect once added with
  // stk_kernel_add_task() - all of which (for a static kernel) must happen before
  // stk_kernel_start(). Priorities only matter if STK_C_KERNEL_TYPE_CPU_0 (stk_config.h)
  // selects SwitchStrategyFP32; with the default round-robin strategy they're a no-op.
  stk_kernel_t *k = stk_kernel_create(0);
  stk_kernel_init(k, STK_PERIODICITY_DEFAULT);

  // blinky task
  stk_task_t *blinky_task = stk_task_create_privileged(led_blinking_task, NULL, blinky_stack, BLINKY_STACK_SIZE);
  stk_task_set_name(blinky_task, "blinky");
  stk_task_set_priority(blinky_task, 1);
  stk_kernel_add_task(k, blinky_task);

  // Create a task for tinyusb device stack
  stk_task_t *usb_device_task_hdl = stk_task_create_privileged(usb_device_task, NULL, usb_device_stack, USBD_STACK_SIZE);
  stk_task_set_name(usb_device_task_hdl, "usbd");
  stk_task_set_priority(usb_device_task_hdl, 30);
  stk_kernel_add_task(k, usb_device_task_hdl);

  // Audio receive (I2S) ISR simulation
  // To simulate a ISR the priority is set to the highest
  stk_task_t *audio_task = stk_task_create_privileged(audio_isr_task, NULL, audio_stack, AUDIO_STACK_SIZE);
  stk_task_set_name(audio_task, "audio");
  stk_task_set_priority(audio_task, 31);
  stk_kernel_add_task(k, audio_task);

  // start the scheduler - never returns
  stk_kernel_start(k);

  return 0;
}

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
void usb_device_task(void *param) {
  (void) param;

  // init device stack on configured roothub port
  // This should be called after scheduler/kernel is started.
  // Otherwise it could cause kernel issue since USB IRQ handler does use RTOS queue API.
  tusb_rhport_init_t dev_init = {
      .role = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);

  board_init_after_tusb();

  // RTOS forever loop
  while (1) {
    // tinyusb device task
    tud_task();
  }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
  blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
  blink_interval_ms = BLINK_NOT_MOUNTED;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
  blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

//--------------------------------------------------------------------+
// AUDIO Task
//--------------------------------------------------------------------+

// This task simulates an audio receive ISR, one frame is received every 1ms.
// We assume that the audio data is read from an I2S buffer.
// In a real application, this would be replaced with actual I2S receive callback.
void audio_isr_task(void *param) {
  (void) param;

  while (1) {
    stk_sleep_ms(1);
    for (size_t cnt = 0; cnt < sizeof(test_buffer_audio) / 2; cnt++) {
      test_buffer_audio[cnt] = startVal++;
    }

    tud_audio_write((uint8_t *) test_buffer_audio, sizeof(test_buffer_audio));
  }
}

//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;
  (void) pBuff;

  // We do not support any set range requests here, only current value requests
  TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t ep = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) ep;

  return false;// Yet not implemented
}

// Invoked when audio class specific set request received for an interface
bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;
  (void) pBuff;

  // We do not support any set range requests here, only current value requests
  TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) itf;

  return false;// Yet not implemented
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  (void) itf;

  // We do not support any set range requests here, only current value requests
  TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);

  // If request is for our feature unit
  if (entityID == 2) {
    switch (ctrlSel) {
      case AUDIO20_FU_CTRL_MUTE:
        // Request uses format layout 1
        TU_VERIFY(p_request->wLength == sizeof(audio20_control_cur_1_t));

        mute[channelNum] = ((audio20_control_cur_1_t *) pBuff)->bCur;

        TU_LOG1("    Set Mute: %d of channel: %u\r\n", mute[channelNum], channelNum);
        return true;

      case AUDIO20_FU_CTRL_VOLUME:
        // Request uses format layout 2
        TU_VERIFY(p_request->wLength == sizeof(audio20_control_cur_2_t));

        volume[channelNum] = (uint16_t) ((audio20_control_cur_2_t *) pBuff)->bCur;
        TU_LOG1("    Set Volume: %d dB of channel: %u\r\n", volume[channelNum], channelNum);
        return true;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }
  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t ep = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) ep;

  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an interface
bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) itf;

  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  // uint8_t itf = TU_U16_LOW(p_request->wIndex); 			// Since we have only one audio function implemented, we do not need the itf value
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  // Input terminal (Microphone input)
  if (entityID == 1) {
    switch (ctrlSel) {
      case AUDIO20_TE_CTRL_CONNECTOR: {
        // The terminal connector control only has a get request with only the CUR attribute.
        audio20_desc_channel_cluster_t ret;

        // Those are dummy values for now
        ret.bNrChannels = 1;
        ret.bmChannelConfig = (audio20_channel_config_t) 0;
        ret.iChannelNames = 0;

        TU_LOG1("    Get terminal connector\r\n");

        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void *) &ret, sizeof(ret));
      } break;

        // Unknown/Unsupported control selector
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Feature unit
  if (entityID == 2) {
    switch (ctrlSel) {
      case AUDIO20_FU_CTRL_MUTE:
        // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
        // There does not exist a range parameter block for mute
        TU_LOG1("    Get Mute of channel: %u\r\n", channelNum);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[channelNum], 1);

      case AUDIO20_FU_CTRL_VOLUME:
        switch (p_request->bRequest) {
          case AUDIO20_CS_REQ_CUR:
            TU_LOG1("    Get Volume of channel: %u\r\n", channelNum);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &volume[channelNum], sizeof(volume[channelNum]));

          case AUDIO20_CS_REQ_RANGE:
            TU_LOG1("    Get Volume range of channel: %u\r\n", channelNum);

            // Copy values - only for testing - better is version below
            audio20_control_range_2_n_t(1)
                ret;

            ret.wNumSubRanges = 1;
            ret.subrange[0].bMin = -90;// -90 dB
            ret.subrange[0].bMax = 90; // +90 dB
            ret.subrange[0].bRes = 1;  // 1 dB steps

            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void *) &ret, sizeof(ret));

            // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
        break;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Clock Source unit
  if (entityID == 4) {
    switch (ctrlSel) {
      case AUDIO20_CS_CTRL_SAM_FREQ:
        // channelNum is always zero in this case
        switch (p_request->bRequest) {
          case AUDIO20_CS_REQ_CUR:
            TU_LOG1("    Get Sample Freq.\r\n");
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sampFreq, sizeof(sampFreq));

          case AUDIO20_CS_REQ_RANGE:
            TU_LOG1("    Get Sample Freq. range\r\n");
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sampleFreqRng, sizeof(sampleFreqRng));

            // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
        break;

      case AUDIO20_CS_CTRL_CLK_VALID:
        // Only cur attribute exists for this request
        TU_LOG1("    Get Sample Freq. valid\r\n");
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &clkValid, sizeof(clkValid));

      // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  TU_LOG1("  Unsupported entity: %d\r\n", entityID);
  return false;// Yet not implemented
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;
  (void) p_request;
  startVal = 0;

  return true;
}

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinking_task(void *param) {
  (void) param;
  static bool led_state = false;

  while (1) {
    // Blink every interval ms
    stk_sleep_ms((stk_timeout_t) blink_interval_ms);

    board_led_write(led_state);
    led_state = 1 - led_state;// toggle
  }
}
