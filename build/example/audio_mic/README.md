# Audio Test Microphone

A minimal USB Audio Class 2.0 (UAC2) microphone that streams a generated test signal, useful for verifying the audio device stack end to end. Targets the **STM32F4 Discovery** board (`CFG_TUSB_MCU = OPT_MCU_STM32F4`). The application logic runs on [SuperTinyKernel RTOS (STK)](https://github.com/SuperTinyKernel-RTOS), a lightweight, deterministic C++ RTOS, accessed here through its native C++ API (`stk.h`).

## What it does

- Enumerates as a UAC2 microphone with 1 input channel at 48 kHz, 16-bit.
- An audio task fills a buffer with a continuously incrementing 16-bit ramp counter and writes it to the IN endpoint every 1 ms, simulating data from an I2S source. The counter resets when the streaming interface is closed.
- Handles UAC2 control requests: feature-unit mute/volume, clock-source sample-rate and clock-validity, and input-terminal connector.
- Blinks the on-board LED to indicate USB state (not mounted / mounted / suspended).

## Task layout

Three STK tasks are defined as classes deriving from `stk::Task<>` and are added to a single static kernel object before `kernel.Start()` is called:

| Task | Class | Priority | Purpose |
|------|-------|----------|---------|
| `usbd` | `UsbDeviceTask` | 30 | Runs `tud_task()` in a forever loop, processing all TinyUSB device events/callbacks |
| `audio` | `AudioIsrTask` | 31 | Simulates an I2S receive ISR: sleeps 1 ms, fills `test_buffer_audio` with the ramp counter, calls `tud_audio_write()` |
| `blinky` | `BlinkyTask` | 1 | Toggles the on-board LED on the interval set by the USB state callbacks |

Notes on the STK task model, since it differs from FreeRTOS:

- Each task is a class deriving from `stk::Task<_StackSize, _AccessMode>`, with the task body in an overridden `Run()`, a `GetTraceName()` override for diagnostics, and a `GetWeight()` override for priority. Static task instances are only scheduled once passed to `kernel.AddTask()` — all task creation/registration must happen before `kernel.Start()`, which never returns.
- Task control blocks live inside the `stk::Kernel<>` object itself, whose max task count is fixed via the kernel's template parameter (here `Kernel<KernelMode, 3, SwitchStrategyFP32, PlatformDefault>`); callers only need to supply the stack memory, sized via the `Task<>` template's `_StackSize` parameter, not a full static TCB as with FreeRTOS's `xTaskCreateStatic()`.
- Stack sizes are always caller-supplied in words of `stk::Word` (via the `Task<>` template's `_StackSize` parameter) — STK has no `configMINIMAL_STACK_SIZE` equivalent, so a baseline (`STK_MINIMAL_STACK_SIZE`) is picked in `main.cpp` and sized up per task.
- Priorities, returned from each task's `GetWeight()` override, only matter because the kernel here is instantiated with `stk::SwitchStrategyFP32` (fixed-priority); with the default round-robin strategy (`stk::SwitchStrategyRR`) `GetWeight()` is a no-op.
- `tusb_init()` is called from inside `UsbDeviceTask::Run()` rather than before the scheduler starts, since the USB IRQ handler relies on RTOS queue APIs that require the kernel to already be running.

## USB Descriptors

| Interface | Class driver |
|-----------|--------------|
| 0–1 | UAC2 audio (control + streaming), 1-channel microphone input |

## Configuration

Notable `tusb_config.h` settings:

```c
#define CFG_TUD_AUDIO                                 1
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE             48000
#define CFG_TUD_AUDIO_ENABLE_EP_IN                    1
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX            1
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX    2   // 16-bit
#define CFG_TUD_AUDIO_EP_SZ_IN                        TUD_AUDIO_EP_SIZE(TUD_OPT_HIGH_SPEED, CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX            CFG_TUD_AUDIO_EP_SZ_IN
```

> **Note:** `TUD_OPT_HIGH_SPEED` here resolves via `BOARD_TUD_MAX_SPEED`/`CFG_TUD_MAX_SPEED`. The STM32F4 Discovery's onboard USB OTG_FS peripheral is full-speed only — confirm the board file sets the max speed accordingly, since a mismatch would undersize `CFG_TUD_AUDIO_EP_SZ_IN` for full-speed's smaller max packet size per (micro)frame.

Notable `stk_config.h` settings:

```c
#define _STK_ARCH_ARM_CORTEX_M          // target architecture
#define STK_TICKLESS_IDLE       (1U)    // 1 = low-power tickless idle, 0 = high-performance
```

Notable `tusb_config.h` settings:

```c
#define CFG_TUH_OSAL_STK_USE_CPP (1U)    // use C++ STK OSAL interface
```

## Building

CMake:

```bash
mkdir build && cd build
cmake -DBOARD=stm32f4_discovery ..
cmake --build .
```

Make:

```bash
make BOARD=stm32f4_discovery all
```

## Try it

The device appears as a single-channel USB microphone. On Linux, list it with `arecord -l` and capture with `arecord` (for example `arecord -D hw:CARD=MicNode -c 1 -f S16_LE -r 48000 test.wav`); the recorded samples form a rising ramp. The included `src/plot_audio_samples.py` records and plots the signal (requires `sounddevice` and `matplotlib`).
