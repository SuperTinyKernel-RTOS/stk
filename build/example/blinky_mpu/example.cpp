/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdio.h>
#include <new>

#include <stk.h>
#include <sync/stk_sync.h>
#include "example.h"

using namespace bsp;

// Size of the task's stack (number of stk::Word)
static constexpr size_t TASK_STACK_SIZE = 256U;

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

// Variable residing in Secure memory region accessible by only Secure tasks.
static uint32_t g_SecureCounter = 0;

// Non-Secure task memory (on ARMv7-M must be aligned to its size).
static constexpr uint32_t TASK_MEMORY_SIZE = 4096;
static uint8_t s_LedTaskMem[4][TASK_MEMORY_SIZE] __stk_aligned(TASK_MEMORY_SIZE);

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

            // uncommenting this will cause MemManage exception due to access of Secure
            // memory region by Non-Secure task
            //++g_SecureCounter;
        }
    }

    const stk::MpuRegionConfig *GetMpuRegions(uint8_t &out_count) override
    {
        using namespace stk;

        extern char __stk_mpu_shared_code_start[];
        extern char __stk_mpu_shared_code_end[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_data_end[];

        static MpuRegionConfig s_mpu_shared_regions[] =
        {
            // region_idx = 0 is reserved by kernel for a stack memory

            {
              .region_idx  = 1,
              .addr        = hw::PtrToWord(__stk_mpu_shared_code_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_code_end) - hw::PtrToWord(__stk_mpu_shared_code_start),
              .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .share       = hw::mpu::SHARE_NON,
              .exec        = hw::mpu::EXEC_ALLOWED
            },
            {
              .region_idx  = 2,
              .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
              .size        = hw::PtrToWord(__stk_mpu_shared_data_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .share       = hw::mpu::SHARE_NON,
              .exec        = hw::mpu::EXEC_ALLOWED
            },
            {
              .region_idx  = 3,
              .addr        = 0,
              .size        = TASK_MEMORY_SIZE, // cover whole allocated region of the task instance
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .share       = hw::mpu::SHARE_NON,
              .exec        = hw::mpu::EXEC_NEVER
            }
        };

        // note: dynamic entry, we have 4 instances and need to allow every instance an access to self
        // where 'this' is pointing to the start of the task's memory region of TASK_MEMORY_SIZE size
        s_mpu_shared_regions[2].addr = hw::PtrToWord(this);

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
                case HwCommand::CMD_LED_ON: {
                    Led::SwitchOnExclusive(static_cast<bsp::Led::Id>(cmd.param_0));

                    // counter is accessed by a Safe task, no MemManage exception in this case
                    ++g_SecureCounter;
                    break; }
                default: {
                    STK_ASSERT(false);
                    break; }
                }
            }
        }
    }
};

class PlatformEventHandler final : public stk::IPlatform::IEventOverrider
{
    const stk::MpuRegionConfig *OnConfigureMpu(uint8_t &out_count) override
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
            {   // REGION 0: Flash - Privileged RO / User RO, executable, cacheable
                .region_idx  = 0U,
                .addr        = FLASH_BASE_ADDR,
                .size        = FLASH_SIZE_BYTES,
                .access_perm = ACCESS_PRIV_RO_USER_RO,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .share       = SHARE_NON,
                .exec        = EXEC_ALLOWED
            },
            {   // REGION 1: SRAM - full RW, execute-never (blocks code injection into RAM)
                .region_idx  = 1U,
                .addr        = SRAM_BASE_ADDR,
                .size        = SRAM_SIZE_BYTES,
                .access_perm = ACCESS_PRIV_RW_USER_NO,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .share       = SHARE_NON,
                .exec        = EXEC_NEVER
            },
            {   // REGION 2: NULL guard - first 256 bytes, no access at all
                .region_idx  = 2U,
                .addr        = 0x00000000UL,
                .size        = 256U,
                .access_perm = ACCESS_HW_NO_ACCESS,
                .mem_type    = TYPE_STRONGLY_ORDERED,
                .share       = SHARE_NON,
                .exec        = EXEC_NEVER
            },
            {   // REGION 3: NULL guard - first 256 bytes, no access at all
                .region_idx  = 3U,
                .addr        = 0U,
                .size        = 0U,
                .access_perm = ACCESS_NONE,
                .mem_type    = TYPE_STRONGLY_ORDERED,
                .share       = SHARE_NON,
                .exec        = EXEC_ALLOWED
            }
        };

        out_count = STK_STATIC_ARRAY_SIZE(mpu_table);
        return mpu_table;
    }

    bool OnException(stk::EHwException exc_id, stk::TId tid, const struct stk::FaultContext *const ctx) override
    {
        if (exc_id == stk::HW_EXCEPT_MEMACCESS)
        {
            printf("\r\n================ MEMMANAGE FAULT DETECTED ================\r\n");
        }
        else
        {
            printf("\r\n================== HARD FAULT DETECTED ===================\r\n");
        }

        printf("ITask *: 0x%08X:\r\n", (unsigned int)tid);

        printf("EXC_RETURN: 0x%08X\r\n\r\n", (unsigned int)ctx->EXC_RETURN);

        printf("--- Stacked CPU Registers ---\r\n");
        printf("R0:   0x%08X    R1:   0x%08X    R2:   0x%08X\r\n", (unsigned int)ctx->frame.R0, (unsigned int)ctx->frame.R1, (unsigned int)ctx->frame.R2);
        printf("R3:   0x%08X    R12:  0x%08X    LR:   0x%08X\r\n", (unsigned int)ctx->frame.R3, (unsigned int)ctx->frame.R12, (unsigned int)ctx->frame.LR);
        printf("PC:   0x%08X    xPSR: 0x%08X\r\n\r\n", (unsigned int)ctx->frame.PC, (unsigned int)ctx->frame.xPSR);

        printf("--- System Control Registers ---\r\n");
        printf("CFSR: 0x%08X    HFSR: 0x%08X    AFSR: 0x%08X\r\n", (unsigned int)ctx->CFSR, (unsigned int)ctx->HFSR, (unsigned int)ctx->AFSR);
        printf("MMFAR: 0x%08X (%s)\r\n", (unsigned int)ctx->MMFAR, ctx->mmfar_valid ? "VALID" : "INVALID");
        printf("BFAR:  0x%08X (%s)\r\n\r\n", (unsigned int)ctx->BFAR, ctx->bfar_valid ? "VALID" : "INVALID");
        printf("CONTROL: 0x%08X (nPRIV=%u)\r\n", (unsigned int)ctx->CONTROL, (unsigned int)(ctx->CONTROL & 1U));

        printf("--- MPU Status & Config ---\r\n");
        printf("CTRL:  0x%08X\r\n", (unsigned int)ctx->mpu.CTRL);
    #if STK_ARCH_ARMV8_M
        printf("MAIR0: 0x%08X    MAIR1: 0x%08X\r\n", (unsigned int)ctx->mpu.MAIR0, (unsigned int)ctx->mpu.MAIR1);
    #endif

        printf("--- MPU Regions Configuration ---\r\n");
        for (size_t i = 0U; i < 8U; i++)
        {
            printf("  Region %u -> RBAR: 0x%08X    RLAR: 0x%08X\r\n",
                   (unsigned int)i,
                   (unsigned int)ctx->mpu.regions[i].RBAR,
                   (unsigned int)ctx->mpu.regions[i].ATTR);
        }
        printf("=====================================================\r\n");

        __stk_debug_break();
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

    // for MPU configuration and MemFault/HardFault exceptions processing
    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

    // make sure memory is enough
    STK_STATIC_ASSERT(sizeof(NonSecureLedTask<ACCESS_USER>) <= TASK_MEMORY_SIZE);

    // Non-Secure tasks
    NonSecureLedTask<ACCESS_USER> *led_task1 = new (s_LedTaskMem[0]) NonSecureLedTask<ACCESS_USER>(LED_RED);
    NonSecureLedTask<ACCESS_USER> *led_task2 = new (s_LedTaskMem[1]) NonSecureLedTask<ACCESS_USER>(LED_ORANGE);
    NonSecureLedTask<ACCESS_USER> *led_task3 = new (s_LedTaskMem[2]) NonSecureLedTask<ACCESS_USER>(LED_GREEN);
    NonSecureLedTask<ACCESS_USER> *led_task4 = new (s_LedTaskMem[3]) NonSecureLedTask<ACCESS_USER>(LED_BLUE);

    static SecureHwCommandQueueTask<ACCESS_PRIVILEGED> hw_cmd_proc;

    // init scheduling kernel
    kernel.Initialize();

    // register non-secure threads (LED tasks)
    kernel.AddTask(led_task1);
    kernel.AddTask(led_task2);
    kernel.AddTask(led_task3);
    kernel.AddTask(led_task4);

    // register secure thread which will interact with hardware
    kernel.AddTask(&hw_cmd_proc);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
