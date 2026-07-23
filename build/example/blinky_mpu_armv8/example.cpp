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
#include <pico/runtime.h>
#include <pico/stdio.h>

#include <stk.h>
#include <sync/stk_sync.h>
#include "example.h"

using namespace bsp;

// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
static constexpr size_t TASK_STACK_SIZE = 1024U;
#else
static constexpr size_t TASK_STACK_SIZE = 256U;
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

// Variable residing in Secure memory region accessible by only Secure tasks.
static uint32_t g_SecureCounter = 0;

// Non-Secure task's stack memory (on ARMv8-M must be aligned to 32 bytes at least).
static constexpr size_t TASK_STACK_MEMORY_SIZE = stk::Align<size_t>(TASK_STACK_SIZE, 32U);
static stk::Word s_LedTaskStackMem[4][TASK_STACK_MEMORY_SIZE] __stk_aligned(32U);

// Non-Secure task memory (on ARMv8-M must be aligned to 32 bytes at least).
static constexpr size_t TASK_MEMORY_SIZE = stk::Align<size_t>(10U, 32U);
static stk::Word s_LedTaskMem[4][TASK_MEMORY_SIZE] __stk_aligned(32U);

// Init mpu_shared_data: Pico SDK does not support custom data and bss regions, so
// we have to do this step before static constructors are called by the init routine.
// 101 is the highest priority available to user code, this forces it to run at the absolute
// start of the __init_array loop, or we can use Pico SDK API to place function pointer
// to the preinit_array section via PICO_RUNTIME_INIT_FUNC which is executed before
// init_array initialization.
//__attribute__((constructor(101)))
static void InitMpuSharedData()
{
    extern char __stk_mpu_shared_data_start[];
    extern char __stk_mpu_shared_data_end[];
    extern char __stk_mpu_shared_data_source__[];
    extern char __stk_mpu_shared_bss_start[];
    extern char __stk_mpu_shared_bss_end[];

    // copy initialized variables from FLASH to RAM
    const size_t data_size = __stk_mpu_shared_data_end - __stk_mpu_shared_data_start;
    if (data_size > 0)
    {
        STK_MEMCPY(__stk_mpu_shared_data_start, __stk_mpu_shared_data_source__, data_size);
    }

    // zero-fill the BSS region
    const size_t bss_size = __stk_mpu_shared_bss_end - __stk_mpu_shared_bss_start;
    if (bss_size > 0)
    {
        STK_MEMSET(__stk_mpu_shared_bss_start, 0, bss_size);
    }
}
PICO_RUNTIME_INIT_FUNC(InitMpuSharedData, "00100");

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
class NonSecureLedTask : public stk::ITask
{
    uint8_t            m_task_id;
    uint32_t           m_my_flag;
    uint32_t           m_next_flag;

    stk::Word         *m_stack;      //!< pointer to stack buffer
    size_t             m_stack_size; //!< stack size in words
    stk::EAccessMode   m_mode;       //!< kernel access mode
    int32_t            m_weight;     //!< scheduling weight

public:
    NonSecureLedTask(uint8_t task_id, stk::Word *stack, size_t stack_size, stk::EAccessMode mode)
        : m_task_id(task_id),
          m_my_flag(FLAGS_ALL[task_id]),
          m_next_flag(FLAGS_ALL[(task_id + 1) % LED_MAX]),
          m_stack(stack),
          m_stack_size(stack_size),
          m_mode(mode),
          m_weight(stk::DEFAULT_WEIGHT)
    {}

    // ITask
    stk::EAccessMode GetAccessMode() const override { return m_mode; }
    void OnDeadlineMissed(uint32_t)        override {}
    void OnExit()                          override {}
    int32_t GetWeight()              const override { return m_weight; }
    const char *GetTraceName()       const override { return nullptr; }

    // IStackMemory
    const stk::Word *GetStack()      const override { return m_stack; }
    size_t GetStackSize()            const override { return m_stack_size; }

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

#if STK_MPU
    const stk::MpuRegionConfig *GetMpuRegions(uint8_t &out_count) override
    {
        using namespace stk;

        // Pico SDK places __aeabi_ldivmod, __aeabi_uldivmod, __aeabi_idiv0 into RAM for a
        // faster execution. We must allow this memory region for access by non-priviliged tasks
        // to support basic math operations.
        // Check .map file for __aeabi_ldivmod location and limit required region more precisely.
        const stk::Word sdk_addr = (0x20000848UL / 32) * 32;
        const size_t sdk_len = stk::Align<size_t>(0x20000C78UL - sdk_addr, 32);

        static MpuRegionConfig s_mpu_shared_regions[] =
        {
            // REGION 4 is reserved by kernel for a stack memory

            { // REGION 5: TASK INSTANCE DATA WINDOW - R/W for this task and privileged code
              .region_idx  = 1U, // STK_CORTEX_M_MPU_TASK_REGION_IDX + region_idx (1) = REGION 5
              .addr        = 0U,
              .size        = TASK_MEMORY_SIZE * sizeof(stk::Word), // cover whole allocated region of the task instance
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .share       = hw::mpu::SHARE_NON,
              .exec        = hw::mpu::EXEC_NEVER
            },
            { // REGION 6: TASK INSTANCE DATA WINDOW - RO for all
                .region_idx  = 2U, // STK_CORTEX_M_MPU_TASK_REGION_IDX + region_idx (2) = REGION 6
                .addr        = sdk_addr,
                .size        = sdk_len,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            }

            // We still have 2 more regions for anything else for MPU with 8 regions.
        };

        // Dynamic entry, we have 4 taks instances and need to allow every instance an access to self
        // where 'this' is pointing to the start of the task's memory region of TASK_MEMORY_SIZE size.
        s_mpu_shared_regions[0].addr = hw::PtrToWord(this);

        out_count = STK_STATIC_ARRAY_SIZE(s_mpu_shared_regions);
        return s_mpu_shared_regions;
    }
#endif
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
#if STK_MPU
    const stk::MpuRegionConfig *OnConfigureMpu(uint8_t &out_count) override
    {
        using namespace stk;

        enum EMemPartition : uint32_t
        {
            BOOTROM_BASE_ADDR = 0x00000000UL,
            BOOTROM_SIZE      = 32 * 1024,

            FLASH_BASE_ADDR   = 0x10000000UL,
            FLASH_SIZE_BYTES  = 4 * 1024 * 1024, // 4 MB

            SRAM_BASE_ADDR    = 0x20000000UL,
            SRAM_SIZE_BYTES   = 512 * 1024,
        };

        extern char __stk_mpu_shared_code_start[];
        extern char __stk_mpu_shared_code_end[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_data_end[];
        extern char __stk_mpu_shared_bss_start[];
        extern char __stk_mpu_shared_bss_end[];

        // Convert pointers once to avoid long calculations in the table
        const uint32_t shared_code_start = hw::PtrToWord(__stk_mpu_shared_code_start);
        const uint32_t shared_code_end   = hw::PtrToWord(__stk_mpu_shared_code_end);
        const uint32_t flash_end         = FLASH_BASE_ADDR + FLASH_SIZE_BYTES;

        // Static region table mapped to RP2350 hardware layout without overlapping windows.
        static const stk::MpuRegionConfig mpu_table[] =
        {
            {   // REGION 0: LOWER FLASH - Everything from start up to the shared code window
                .region_idx  = 0U,
                .addr        = FLASH_BASE_ADDR,
                .size        = shared_code_start - FLASH_BASE_ADDR,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 1: UPPER FLASH - Everything after the shared code window to the end of Flash
                .region_idx  = 1U,
                .addr        = shared_code_end,
                .size        = flash_end - shared_code_end,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 2: SHARED CODE WINDOW - Explicitly mapped with user execution permissions
                .region_idx  = 2U,
                .addr        = shared_code_start,
                .size        = shared_code_end - shared_code_start,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 3: RAM DATA / BSS WINDOW
                .region_idx  = 3U,
                .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
                .size        = hw::PtrToWord(__stk_mpu_shared_bss_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
                .access_perm = hw::mpu::ACCESS_FULL,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            }
        };

        STK_UNUSED(__stk_mpu_shared_data_end);
        STK_UNUSED(__stk_mpu_shared_bss_start);

        out_count = STK_STATIC_ARRAY_SIZE(mpu_table);
        return mpu_table;
    }
#endif

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

    stdio_init_all();

    Led::InitAll(false);

    // operating in Static + Sync mode (EventFlags requires KERNEL_SYNC) and optionally tickless
    const uint8_t KernelMode = KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // allocate scheduling kernel for 3 threads (tasks) with Round-Robin scheduling strategy
    static Kernel<KernelMode, 5, SwitchStrategyRR, PlatformDefault> kernel;

    // for MPU configuration and MemFault/HardFault exceptions processing
    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

    // make sure memory is enough
    STK_STATIC_ASSERT(sizeof(NonSecureLedTask) / sizeof(stk::Word) <= TASK_MEMORY_SIZE);

    // Non-Secure tasks
    NonSecureLedTask *led_task1 = new (s_LedTaskMem[0]) NonSecureLedTask(LED_RED, s_LedTaskStackMem[0], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[0]), ACCESS_USER);
    NonSecureLedTask *led_task2 = new (s_LedTaskMem[1]) NonSecureLedTask(LED_ORANGE, s_LedTaskStackMem[1], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[1]), ACCESS_USER);
    NonSecureLedTask *led_task3 = new (s_LedTaskMem[2]) NonSecureLedTask(LED_GREEN, s_LedTaskStackMem[2], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[2]), ACCESS_USER);
    NonSecureLedTask *led_task4 = new (s_LedTaskMem[3]) NonSecureLedTask(LED_BLUE, s_LedTaskStackMem[3], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[3]), ACCESS_USER);

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
