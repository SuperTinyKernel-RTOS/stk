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

// ---------------------------------------------------------------------------
// This example is built with STK_MPU_TASK_REGIONS=2 (in addition to STK_MPU=1
// and STK_MPU_STACK_GUARD=1): each task gets only 2 hardware MPU region slots -
// slot 0 (automatic stack guard) and slot 1 (one application-defined region).
//
// On an 8-region MPU this raises STK_CORTEX_M_MPU_TASK_REGION_IDX to 6 (=
// STK_CORTEX_M_MPU_REGIONS_MAX - STK_MPU_TASK_REGIONS), so the static/global
// region budget below in OnConfigureMpu() grows from 4 to 6 regions - the driver
// only writes the RBAR + one alias register (A1) per context switch in this
// configuration, which stays within its 4-region-aligned hardware block at
// index 6 (no override of STK_CORTEX_M_MPU_TASK_REGION_IDX needed).
//
// Since 'shared code' / 'shared data' are identical for every LED task instance,
// they are configured once as two of those global regions instead of being
// repeated per-task; the single remaining per-task slot is then reserved for
// what genuinely differs per task instance: its own object memory (the 'self'
// region).
//
// Final hardware region-index mapping on this 8-region MPU:
//   Regions 0-5: static/global table returned by OnConfigureMpu() - see the
//                detailed per-region breakdown at the top of that function.
//   Region 6:    automatic stack guard (task-relative slot 0, kernel-managed)
//   Region 7:    per-task 'self' region (task-relative slot 1, application-
//                defined - see NonSecureLedTask::GetMpuRegions() below)
// ---------------------------------------------------------------------------

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

// Per-task instance memory: on ARMv7-M the MPU requires a region's base address to be
// naturally aligned to its size (power-of-two rounded up), so each task's storage block
// is both sized and aligned to TASK_MEMORY_SIZE - this is what lets the 'self' region in
// GetMpuRegions() below cover exactly one task instance without spilling into its neighbor.
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
    explicit NonSecureLedTask(uint8_t task_id) : m_task_id(task_id), m_my_flag(FLAGS_ALL[task_id]),
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
            s_HwCmdQueue.Write({
                .id      = HwCommand::CMD_LED_ON,
                .param_0 = m_task_id}
            );

            // sleep 1s drift-free and then delegate work to the next task
            // we could use simple stk::Sleep() but due to other work around Sleep call we
            // will get a time drift, STK allows to sleep until exact timestamp making it
            // possible precise sleeping with 1 tick precision, you could also use
            // time::TimerHost for timer-related tasks (see related 'timer' example)
            stk::SleepUntil(g_Timeline += period);

            // hand off to the next task
            g_TaskFlags.Set(m_next_flag);

            // uncommenting this will cause MemManage exception due to access of Secure
            // memory region by Non-Secure task, under debugger you will see such call stack:
            //
            //   * PlatformEventHandler::OnException() at example.cpp:383
            //   * StkExceptionHandlerMain() at stk_arch_arm-cortex-m.cpp:2 683
            //   * <signal handler called>() at 0xfffffffd
            //   * NonSecureLedTask<(stk::EAccessMode)0>::Run at example.cpp:140 <--- points to offending ++g_SecureCounter
            //   * OnTaskRun() at stk_arch_arm-cortex-m.cpp:2 751
            //
            //++g_SecureCounter;
        }
    }

    const stk::MpuRegionList *GetMpuRegions() const override
    {
        using namespace stk;

        // With STK_MPU_TASK_REGIONS=2 each task only has one application-defined
        // slot available (task-relative slot 1, following the automatic stack
        // guard in slot 0). 'Shared code' and 'shared data' are identical across
        // all 4 task instances, so they are configured once as global regions in
        // PlatformEventHandler::OnConfigureMpu() instead of being repeated here;
        // this single slot is reserved for what is genuinely per-instance: access
        // to the task's own object memory.
        static MpuRegionConfig s_self_region[] =
        {
            {
              // note: region_idx is driver-owned - the kernel always overwrites it to the
              // correct task-relative slot (slot 1 -> hardware Region 7, see the file-level
              // region-index mapping at the top of this file); the value here is
              // informational only and never actually read by the driver.
              .addr        = 0U, // patched below with 'this'
              .size        = TASK_MEMORY_SIZE, // cover whole allocated region of the task instance
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .share       = hw::mpu::SHARE_NON,
              .exec        = hw::mpu::EXEC_NEVER
            }
        };

        // Unlike the static table in PlatformEventHandler::OnConfigureMpu() (identical
        // for every task/call), this table is recomputed on every call: there are 4
        // NonSecureLedTask instances sharing this same GetMpuRegions() code, and each one
        // must only be granted access to its own object memory - 'this' points at the
        // start of that instance's TASK_MEMORY_SIZE-byte block (see s_LedTaskMem[]), so the
        // base address is patched in here right before the table is handed back to the
        // kernel for this task.
        s_self_region[0].addr = hw::PtrToWord(this);

        static const stk::MpuRegionList mpu_regions(
            s_self_region, STK_STATIC_ARRAY_SIZE(s_self_region));

        return &mpu_regions;
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
    const stk::MpuConfig *OnConfigureMpu() const override
    {
        using namespace stk;
        using namespace stk::hw::mpu;

        // ---------------------------------------------------------------------------
        // MPU (Memory Protection Unit) setup for STM32F4 (Cortex-M4, ARMv7-M MPU, 8 regions)
        // ---------------------------------------------------------------------------
        // This adds coarse-grained memory sandboxing that applies globally, regardless
        // of which task is running:
        //   Region 0: Flash        - read-only, executable       (code + rodata)
        //   Region 1: SRAM         - read/write, execute-never   (data, stacks, heap)
        //   Region 2: NULL guard   - first 256 bytes, no access at all (catches null derefs)
        //   Region 3: Reserved     - currently a disabled/zero-sized placeholder (see NOTE)
        //   Region 4: Shared code  - read/execute, common code shared by every task
        //   Region 5: Shared data  - read/write, globals shared by every task
        //
        // NOTE - documentation/config mismatch found while reviewing this table: Region 3
        // below is configured with size=0 and ACCESS_NONE, which is effectively a disabled
        // placeholder entry, NOT a Peripherals region or a second NULL guard. The
        // PERIPH_BASE_ADDR/PERIPH_SIZE_BYTES/CCMRAM_BASE_ADDR constants in EMemPartition
        // below are defined but never referenced by mpu_table - meaning non-privileged
        // tasks currently have NO MPU-mapped access to the peripheral bus at all via this
        // table. If peripheral access from user-mode tasks is required, Region 3 should be
        // repurposed to map PERIPH_BASE_ADDR/PERIPH_SIZE_BYTES (Device memory type,
        // execute-never); otherwise consider removing the unused enum constants.
        //
        // Regions 4/5 are configured here (instead of per-task, as they would be with
        // STK_MPU_TASK_REGIONS=4) because this example builds with STK_MPU_TASK_REGIONS=2:
        // each task only gets 1 application-defined region slot, reserved below for the
        // task's own (per-instance) object memory. 'Shared code'/'shared data' are
        // identical for every task instance, so a single global region covers all of them
        // at once - and the region budget for this scales accordingly:
        // STK_CORTEX_M_MPU_TASK_REGION_IDX == STK_CORTEX_M_MPU_REGIONS_MAX - STK_MPU_TASK_REGIONS,
        // i.e. 8 - 2 == 6 static regions available here (vs. only 4 with 4 task regions).
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
            CCMRAM_BASE_ADDR  = 0x10000000UL,  // 64KB CCM block - currently unused below
            PERIPH_BASE_ADDR  = 0x40000000UL,  // currently unused below - see NOTE above
            PERIPH_SIZE_BYTES = 512  * 1024 * 1024 // covers the whole peripheral aperture
        };

        extern char __stk_mpu_shared_code_start[];
        extern char __stk_mpu_shared_code_end[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_data_end[];

        // Static (boot-time, non per-task) region table. hw::mpu takes raw byte
        // address/size directly - ConfigureTable() works out the ARMv7-M
        // size-field and RBAR/RASR bit layout internally, so there's no manual
        // register math or CMSIS ARM_MPU_* helper involved.
        static const stk::MpuRegionConfig mpu_table[] =
        {
            {   // REGION 0: Flash - Privileged RO / User RO, executable, cacheable
                .addr        = FLASH_BASE_ADDR,
                .size        = FLASH_SIZE_BYTES,
                .access_perm = ACCESS_PRIV_RO_USER_RO,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .share       = SHARE_NON,
                .exec        = EXEC_ALLOWED
            },
            {   // REGION 1: SRAM - full RW, execute-never (blocks code injection into RAM)
                .addr        = SRAM_BASE_ADDR,
                .size        = SRAM_SIZE_BYTES,
                .access_perm = ACCESS_PRIV_RW_USER_NO,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .share       = SHARE_NON,
                .exec        = EXEC_NEVER
            },
            {   // REGION 2: NULL guard - first 256 bytes, no access at all
                .addr        = 0x00000000UL,
                .size        = 256U,
                .access_perm = ACCESS_HW_NO_ACCESS,
                .mem_type    = TYPE_STRONGLY_ORDERED,
                .share       = SHARE_NON,
                .exec        = EXEC_NEVER
            },
            {   // REGION 3: reserved/disabled placeholder (size=0, no access) - not currently
                //           mapping anything; see the NOTE above EMemPartition for how to
                //           repurpose this slot for a Peripherals region if needed
                .addr        = 0U,
                .size        = 0U,
                .access_perm = ACCESS_NONE,
                .mem_type    = TYPE_STRONGLY_ORDERED,
                .share       = SHARE_NON,
                .exec        = EXEC_ALLOWED
            },
            {   // REGION 4: Shared code - Privileged RO / User RO, executable, common
                //           to every task (STK_MPU_SHARED_CODE_SECTION)
                .addr        = hw::PtrToWord(__stk_mpu_shared_code_start),
                .size        = hw::PtrToWord(__stk_mpu_shared_code_end) - hw::PtrToWord(__stk_mpu_shared_code_start),
                .access_perm = ACCESS_PRIV_RO_USER_RO,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .share       = SHARE_NON,
                .exec        = EXEC_ALLOWED
            },
            {   // REGION 5: Shared data - full RW, execute-never, common to every task
                //           (STK_MPU_SHARED_DATA_SECTION / STK_MPU_SHARED_BSS_SECTION)
                .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
                .size        = hw::PtrToWord(__stk_mpu_shared_data_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
                .access_perm = ACCESS_FULL,
                .mem_type    = TYPE_NORMAL_CACHEABLE,
                .share       = SHARE_NON,
                .exec        = EXEC_NEVER
            }
        };

        // set MPU regions and notify driver that we want to enable MPU
        static const stk::MpuConfig mpu_config
        (
            stk::MpuRegionList(mpu_table, STK_STATIC_ARRAY_SIZE(mpu_table)),
            (hw::mpu::MPU_CFG_PRIVILEGED_BG_MEM |
             hw::mpu::MPU_CFG_CLEAR_ON_INIT)
        );

        return &mpu_config;
    }

    // Kernel-invoked fault handler: fires on MemManage faults (e.g. the Non-Secure LED
    // tasks touching g_SecureCounter, or a stack-guard violation) and on generic hard
    // faults. Dumps CPU/MPU state for diagnostics and halts via a debug breakpoint -
    // this is intentionally non-recoverable diagnostic code, not a fault-recovery example.
    bool OnException(stk::EHwException exc_id, stk::TId tid, const struct stk::FaultContext *const ctx) override
    {
        if (exc_id == stk::HW_EXCEPT_MEMACCESS)
        {
            printf("\r\n================ MEMMANAGE FAULT DETECTED ================\r\n");
            printf("(Sandbox Violation: a task's Run() reached outside the MPU\r\n");
            printf(" regions granted to it - see region dump below for what\r\n");
            printf(" WAS active for this task at fault time)\r\n");
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
