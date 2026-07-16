/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdio.h>

#include <stk.h>
#include <sync/stk_sync.h>
#include "example.h"

using namespace bsp;

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
enum { TASK_STACK_SIZE = 1024 };
#else
enum { TASK_STACK_SIZE = 256 };
#endif

// One flag bit per LED task; task 0 (RED) goes first
static constexpr uint32_t FLAGS_ALL[] = {
    (1U << LED_RED),
    (1U << LED_ORANGE),
    (1U << LED_GREEN),
    (1U << LED_BLUE)
};

// Start with the RED task's flag set so it runs first
STK_MPU_SHARED_DATA_SECTION static stk::sync::EventFlags g_TaskFlags(FLAGS_ALL[LED_RED]);

// Timeline for a precise LED switching
STK_MPU_SHARED_DATA_SECTION static stk::Ticks g_Timeline = 0;

// Hw commands.
struct HwCommand
{
    enum EId
    {
        CMD_NONE   = 0,
        CMD_LED_ON = 1
    };

    EId       id;
    stk::Word param_0;
};
STK_MPU_SHARED_DATA_SECTION static stk::sync::PipeT<HwCommand, 4> s_HwCmdQueue;

// Task's core (thread)
template <stk::EAccessMode _AccessMode>
class NonSecureLedTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
    uint8_t  m_task_id;
    uint32_t m_my_flag;
    uint32_t m_next_flag;

public:
    NonSecureLedTask(uint8_t task_id) : m_task_id(task_id), m_my_flag(FLAGS_ALL[task_id]),
          m_next_flag(FLAGS_ALL[(task_id + 1) % LED_MAX])
    {}

private:
    void Run() override
    {
        // we switch LEDs with 250ms period
        const stk::Ticks period = stk::GetTicksFromMs(250);

        // get a start of the timeline
        g_Timeline = stk::GetTicks();

        while (true)
        {
            // block until this task's flag is set; auto-cleared on return
            uint32_t result = g_TaskFlags.Wait(m_my_flag, stk::sync::EventFlags::OPT_WAIT_ANY);
            if (stk::sync::EventFlags::IsError(result))
                continue;

            // change active LED
            const HwCommand cmd = {
                .id      = HwCommand::CMD_LED_ON,
                .param_0 = m_task_id
            };
            s_HwCmdQueue.Write(cmd);

            // sleep 1s drift-free and then delegate work to the next task
            // we could use simple stk::Sleep() but due to other work around Sleep call we
            // will get a time drift, STK allows to sleep until exact timestamp making it
            // possible precise sleeping with 1 tick precision, you could also use
            // time::TimerHost for timer-related tasks (see related 'timer' example)
            stk::SleepUntil(g_Timeline += period);

            // hand off to the next task
            g_TaskFlags.Set(m_next_flag);
        }
    }

    const stk::MpuRegionConfig *GetMpuRegions(uint8_t &out_count) override
    {
        using namespace stk;

        extern char __stk_mpu_shared_code_start[];
        extern char __stk_mpu_shared_code_end[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_data_end[];

        static const MpuRegionConfig s_mpu_shared_regions[] =
        {
            {
              .region_idx  = 1,
              .addr        = hw::PtrToWord(__stk_mpu_shared_code_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_code_end) - hw::PtrToWord(__stk_mpu_shared_code_start),
              .access_perm = hw::mpu::ACCESS_PRIV_RW_USER_RO,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .exec        = hw::mpu::EXEC_ALLOWED
            },
            {
              .region_idx  = 2,
              .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_data_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .exec        = hw::mpu::EXEC_NEVER
            },
        };

        out_count = STK_STATIC_ARRAY_SIZE(s_mpu_shared_regions);
        return s_mpu_shared_regions;
    }
};

// Secure task's core.
template <stk::EAccessMode _AccessMode>
class SecureHwCommandQueueTask : public stk::Task<TASK_STACK_SIZE, _AccessMode>
{
private:
    void Run() override
    {
        while (true)
        {
            HwCommand cmd;
            if (s_HwCmdQueue.Read(cmd))
            {
                switch (cmd.id)
                {
                case HwCommand::CMD_LED_ON:
                    Led::SwitchOnExclusive(static_cast<bsp::Led::Id>(cmd.param_0));
                    break;
                default:
                    STK_ASSERT(false);
                    break;
                }
            }
        }
    }
};

class PlatformEventHandler final : public stk::IPlatform::IEventOverrider
{
    const stk::MpuRegionConfig *OnConfigureMpu(uint8_t &out_count)
    {
        using namespace stk::hw::mpu;

        // ---------------------------------------------------------------------------
        // MPU (Memory Protection Unit) setup for STM32F4 (Cortex-M4, ARMv7-M MPU, 8 regions)
        // ---------------------------------------------------------------------------
        // This adds coarse-grained memory sandboxing that applies globally, regardless
        // of which task is running:
        //   Region 0: Flash        - read-only, executable       (code + rodata)
        //   Region 1: SRAM         - read/write, execute-never   (data, stacks, heap)
        //   Region 2: Peripherals  - read/write, execute-never, Device memory type
        //   Region 3: NULL guard   - no access at all            (catches null derefs)
        //
        // Adjust FLASH_SIZE_BYTES / SRAM_SIZE_BYTES below to match your exact STM32F4
        // part (e.g. STM32F407: 1 MB flash / 192 KB SRAM; STM32F429: 2 MB / 256 KB).
        // MPU region sizes must be a power of two, so pick the next power-of-two that
        // covers your actual flash/RAM size.
        enum EMemPartition : uint32_t
        {
            FLASH_BASE_ADDR   = 0x08000000UL,
            FLASH_SIZE_BYTES  = 1024 * 1024,   // 1 MB -> ARM_MPU_REGION_SIZE_1MB
            SRAM_BASE_ADDR    = 0x20000000UL,
            SRAM_SIZE_BYTES   = 128  * 1024,   // 128 KB -> ARM_MPU_REGION_SIZE_128KB
            CCMRAM_BASE_ADDR  = 0x10000000UL,  // 64KB CCM block
            PERIPH_BASE_ADDR  = 0x40000000UL,
            PERIPH_SIZE_BYTES = 512  * 1024 * 1024 // covers the whole peripheral aperture
        };

        // Static (boot-time, non per-task) region table. hw::mpu takes raw byte
        // address/size directly - ConfigureTable() works out the ARMv7-M
        // size-field and RBAR/RASR bit layout internally, so there's no manual
        // register math or CMSIS ARM_MPU_* helper involved.
        static const stk::MpuRegionConfig mpu_table[] =
        {
            {   // REGION 0: Flash - Privileged RW / User RO, executable, cacheable
                .region_idx  = 0U,
                .addr        = FLASH_BASE_ADDR,
                .size        = FLASH_SIZE_BYTES,
                .access_perm = ACCESS_PRIV_RW_USER_RO,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .exec        = EXEC_ALLOWED
            },
            {   // REGION 1: SRAM - full RW, execute-never (blocks code injection into RAM)
                .region_idx  = 1U,
                .addr        = SRAM_BASE_ADDR,
                .size        = SRAM_SIZE_BYTES,
                .access_perm = ACCESS_PRIV_RW_USER_NO,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .exec        = EXEC_NEVER
            },
            {   // REGION 2: Peripherals - Privileged-only RW, execute-never, Device memory
                .region_idx  = 2U,
                .addr        = PERIPH_BASE_ADDR,
                .size        = PERIPH_SIZE_BYTES,
                .access_perm = ACCESS_PRIV_RW_USER_NO,
                .mem_type    = TYPE_DEVICE,
                .exec        = EXEC_NEVER
            },
            {   // REGION 3: NULL guard - first 256 bytes, no access at all
                .region_idx  = 3U,
                .addr        = 0x00000000UL,
                .size        = 256U,
                .access_perm = ACCESS_NONE,
                .mem_type    = TYPE_STRONGLY_ORDERED,
                .exec        = EXEC_NEVER
            }
        };

        out_count = STK_STATIC_ARRAY_SIZE(mpu_table);
        return mpu_table;
    }

    bool OnExceptionMemManage(stk::TId tid)
    {
        volatile uint32_t MMFAR = SCB->MMFAR;
        volatile uint32_t MMFSR = SCB->CFSR & 0x000000FFU;
        volatile uint32_t ctrl  = __get_CONTROL();

        printf("MemManage exception:\n");
        printf("    MMFAR   = 0x%08x\n", (unsigned int)MMFAR);
        printf("    MMFSR   = 0x%08x\n", (unsigned int)MMFSR);
        printf("    CONTROL = 0x%08x (nPRIV=%u)\n", (unsigned int)ctrl, (unsigned int)(ctrl & 1U));
        printf("    ITask * = 0x%08x:\n", (unsigned int)tid);

        for (uint32_t region = 0; region < STK_CORTEX_M_MPU_REGIONS_MAX; ++region)
        {
            MPU->RNR = region;
            printf("    Region %u: RBAR=0x%08x RASR=0x%08x\n",
               (unsigned int)region, (unsigned int)MPU->RBAR, (unsigned int)MPU->RASR);
        }

        return false;
    }
};

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // operating in Static + Sync mode (EventFlags requires KERNEL_SYNC) and optionally tickless
    const uint8_t KernelMode = KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // allocate scheduling kernel for 3 threads (tasks) with Round-Robin scheduling strategy
    static Kernel<KernelMode, 5, SwitchStrategyRR, PlatformDefault> kernel;

    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

    // note: using ACCESS_PRIVILEGED as Cortex-M3+ may not allow writing to GPIO from a less secure user thread
    STK_MPU_SHARED_DATA_SECTION static NonSecureLedTask<ACCESS_USER> led_task1(LED_RED);
    STK_MPU_SHARED_DATA_SECTION static NonSecureLedTask<ACCESS_USER> led_task2(LED_ORANGE);
    STK_MPU_SHARED_DATA_SECTION static NonSecureLedTask<ACCESS_USER> led_task3(LED_GREEN);
    STK_MPU_SHARED_DATA_SECTION static NonSecureLedTask<ACCESS_USER> led_task4(LED_BLUE);

    static SecureHwCommandQueueTask<ACCESS_PRIVILEGED> hw_cmd_proc;

    // init scheduling kernel
    kernel.Initialize();

    // register non-secure threads (LED tasks)
    kernel.AddTask(&led_task1);
    kernel.AddTask(&led_task2);
    kernel.AddTask(&led_task3);
    kernel.AddTask(&led_task4);

    // register secure thread which will interact with hardware
    kernel.AddTask(&hw_cmd_proc);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
