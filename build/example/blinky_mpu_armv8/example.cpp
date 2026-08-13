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

// ---------------------------------------------------------------------------
// MPU (Memory Protection Unit) setup for RP2350 (Cortex-M33, ARMv8-M MPU, 8 regions)
// ---------------------------------------------------------------------------
// This example is built with STK_MPU_TASK_REGIONS=2 (in addition to STK_MPU=1
// and STK_MPU_STACK_GUARD=1): each task gets only 2 hardware MPU region slots -
// slot 0 (automatic stack guard) and slot 1 (one application-defined region).
//
// On this 8-region MPU that raises STK_CORTEX_M_MPU_TASK_REGION_IDX to 6 (=
// STK_CORTEX_M_MPU_REGIONS_MAX - STK_MPU_TASK_REGIONS), so the static/global
// region budget in PlatformEventHandler::OnConfigureMpu() grows from 4 to 6
// regions - the driver only writes RBAR/RLAR plus one alias register (A1) per
// context switch in this configuration, which stays within its 4-region-aligned
// hardware block at index 6 (no manual override needed).
//
// The Pico SDK's RAM-resident division routines (__aeabi_ldivmod etc.) are
// needed identically by every task, so - like 'shared code'/'shared data' -
// they are now configured once as a global region instead of being repeated
// per task; the single remaining per-task slot is reserved for what genuinely
// differs per task instance: its own object memory (the 'self' region).
//
// Static/global regions configured in PlatformEventHandler::OnConfigureMpu():
//   Region 0: Lower flash  - read-only, executable   (code before the shared-code window)
//   Region 1: Upper flash  - read-only, executable   (code after the shared-code window)
//   Region 2: Shared code  - read/execute, common code shared by every task
//   Region 3: RAM data/BSS - read/write, execute-never, globals shared by every task
//   Region 4: RAM text     - read-only, executable, Pico SDK RAM-resident divide routines
//   Region 5: reserved, intentionally left unconfigured for now (see note in
//             OnConfigureMpu() - the BOOTROM_BASE_ADDR/BOOTROM_SIZE constants below
//             are set aside for mapping this window in a future revision, but are
//             not wired into the region table yet)
//
// Per-task regions (application-defined, see NonSecureLedTask::GetMpuRegions()):
//   Slot 0 (Region 6): automatic stack guard, configured internally by the kernel
//   Slot 1 (Region 7): task's own instance data window (the 'self' region)
// ---------------------------------------------------------------------------

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

// Init mpu_shared_data: the Pico SDK's default linker script/runtime does not know
// about STK's custom .stk_mpu_shared_data/.stk_mpu_shared_bss sections, so their
// FLASH->RAM copy and zero-fill has to be done manually, and it must happen before
// any C++ static constructor runs (globals living in those sections - like
// g_TaskFlags and g_Timeline below - would otherwise be constructed on top of
// uninitialized/stale memory).
//
// Two ways to guarantee that ordering are shown here; this example uses the second:
//   1. __attribute__((constructor(101))) - 101 is the highest priority available to
//      user code, forcing the function to the very start of the __init_array loop
//      (kept below, commented out, for reference).
//   2. PICO_RUNTIME_INIT_FUNC(..., "00100") - registers the function in the Pico
//      SDK's preinit_array section, which the SDK's runtime guarantees to run
//      before init_array (and therefore before any static constructor).
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

public:
    explicit NonSecureLedTask(uint8_t task_id, stk::Word *stack, size_t stack_size, stk::EAccessMode mode)
        : m_task_id(task_id),
          m_my_flag(FLAGS_ALL[task_id]),
          m_next_flag(FLAGS_ALL[(task_id + 1) % LED_MAX]),
          m_stack(stack),
          m_stack_size(stack_size),
          m_mode(mode)
    {}

    // ITask
    stk::EAccessMode GetAccessMode() const override { return m_mode; }

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
            //   * PlatformEventHandler::OnException() at example.cpp:459
            //   * StkExceptionHandlerMain() at stk_arch_arm-cortex-m.cpp:2 683
            //   * <signal handler called>() at 0xfffffffd
            //   * NonSecureLedTask<(stk::EAccessMode)0>::Run at example.cpp:211 <--- points to offending ++g_SecureCounter
            //   * OnTaskRun() at stk_arch_arm-cortex-m.cpp:2 751
            //
            //++g_SecureCounter;
        }
    }

#if STK_MPU
    const stk::MpuRegionList *GetMpuRegions() const override
    {
        using namespace stk;

        // With STK_MPU_TASK_REGIONS=2 each task only has one application-defined
        // slot available (task-relative slot 1, following the automatic stack
        // guard in slot 0). The Pico SDK RAM-resident division routines are
        // identical for every task instance, so they are now configured once as
        // a global region in PlatformEventHandler::OnConfigureMpu() instead of
        // being repeated here; this single slot is reserved for what is
        // genuinely per-instance: access to the task's own object memory.
        static MpuRegionConfig s_self_region[] =
        {
            // REGION 6 is reserved by the kernel for the automatic stack guard

            { // REGION 7: TASK INSTANCE DATA WINDOW - R/W for this task and privileged code
              .addr        = 0U,
              .size        = TASK_MEMORY_SIZE * sizeof(stk::Word), // cover whole allocated region of the task instance
              .access_perm = hw::mpu::ACCESS_FULL,
              .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
              .share       = hw::mpu::SHARE_NON,
              .exec        = hw::mpu::EXEC_NEVER
            }
        };

        // Unlike the static tables in OnConfigureMpu() (identical for every task/call),
        // this table is recomputed on every call: there are 4 NonSecureLedTask instances
        // sharing this same GetMpuRegions() code, and each one must only be granted access
        // to its own object memory - 'this' points at the start of that instance's
        // TASK_MEMORY_SIZE-word block (see s_LedTaskMem[]), so the base address is patched
        // in here right before the table is handed back to the kernel for this task.
        s_self_region[0].addr = hw::PtrToWord(this);

        static const stk::MpuRegionList mpu_regions(
            s_self_region, STK_STATIC_ARRAY_SIZE(s_self_region));

        return &mpu_regions;
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
    const stk::MpuConfig *OnConfigureMpu() const override
    {
        using namespace stk;

        // RP2350 memory map constants. Adjust FLASH_SIZE_BYTES / SRAM_SIZE_BYTES to
        // match your exact board/flash chip if it differs from the Pico 2 default.
        // MPU region sizes must be a power of two, so pick the next power-of-two
        // that covers your actual flash/RAM size when changing these.
        enum EMemPartition : uint32_t
        {
            // Reserved for a future revision that maps the BOOTROM window as a global
            // region (see Region 5 note above) - not yet referenced by mpu_table below.
            BOOTROM_BASE_ADDR = 0x00000000UL,
            BOOTROM_SIZE      = 32 * 1024,    // 32 KB internal boot ROM

            FLASH_BASE_ADDR   = 0x10000000UL, // start of external QSPI flash (XIP window)
            FLASH_SIZE_BYTES  = 4 * 1024 * 1024, // 4 MB -> matches Pico 2 / RP2350 default flash

            SRAM_BASE_ADDR    = 0x20000000UL,
            SRAM_SIZE_BYTES   = 512 * 1024,   // 512 KB on-chip SRAM (banks 0-9 + scratch X/Y)
        };

        extern char __stk_mpu_shared_code_start[];
        extern char __stk_mpu_shared_code_end[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_data_end[];
        extern char __stk_mpu_shared_bss_start[];
        extern char __stk_mpu_shared_bss_end[];

        // Pico SDK places __aeabi_ldivmod, __aeabi_uldivmod, __aeabi_idiv0 into RAM for a
        // faster execution. We must allow this memory region for access by non-priviliged
        // tasks to support basic math operations.
        extern char __stk_mpu_ram_text_start[];
        extern char __stk_mpu_ram_text_end[];

        // Convert pointers once to avoid long calculations in the table
        const Word shared_code_start = hw::PtrToWord(__stk_mpu_shared_code_start);
        const Word shared_code_end   = hw::PtrToWord(__stk_mpu_shared_code_end);
        const Word flash_end         = FLASH_BASE_ADDR + FLASH_SIZE_BYTES;

        const Word ram_text_start  = hw::PtrToWord(__stk_mpu_ram_text_start);
        const Word ram_text_end    = hw::PtrToWord(__stk_mpu_ram_text_end);
        const size_t ram_text_size = stk::Align<size_t>(static_cast<size_t>(ram_text_end - ram_text_start), 32U);

        // Static region table mapped to RP2350 hardware layout without overlapping windows.
        // Table position == hardware region index (entry 0 -> region 0, etc).
        static const stk::MpuRegionConfig mpu_table[] =
        {
            {   // REGION 0: LOWER FLASH - Everything from start up to the shared code window
                .addr        = FLASH_BASE_ADDR,
                .size        = shared_code_start - FLASH_BASE_ADDR,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 1: UPPER FLASH - Everything after the shared code window to the end of Flash
                .addr        = shared_code_end,
                .size        = flash_end - shared_code_end,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 2: SHARED CODE WINDOW - Explicitly mapped with user execution permissions
                .addr        = shared_code_start,
                .size        = shared_code_end - shared_code_start,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 3: RAM DATA / BSS WINDOW
                .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
                .size        = hw::PtrToWord(__stk_mpu_shared_bss_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
                .access_perm = hw::mpu::ACCESS_FULL,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            },
            {   // REGION 4: RAM TEXT WINDOW - Pico SDK division routines, RO+exec for every task
                .addr        = ram_text_start,
                .size        = ram_text_size,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            }

            // Region 5 is left unconfigured here on purpose: ConfigureTable() disables
            // any static-region slot beyond out_count automatically, so it's already
            // free for future use without a placeholder entry.
        };

        STK_UNUSED(__stk_mpu_shared_data_end);
        STK_UNUSED(__stk_mpu_shared_bss_start);

        // set MPU regions and notify driver that we want to enable MPU
        static const stk::MpuConfig mpu_config
        (
            stk::MpuRegionList(mpu_table, STK_STATIC_ARRAY_SIZE(mpu_table)),
            (hw::mpu::MPU_CFG_PRIVILEGED_BG_MEM |
             hw::mpu::MPU_CFG_CLEAR_ON_INIT)
        );

        return &mpu_config;
    }
#endif

#if STK_MPU
    static void PrintMpuConfig(const char *label, const stk::FaultContext::Mpu &mpu)
    {
        printf("--- %s MPU Status & Config ---\r\n", label);
        printf("CTRL:  0x%08X\r\n", (unsigned int)mpu.CTRL);
    #if STK_ARCH_ARMV8_M
        printf("MAIR0: 0x%08X    MAIR1: 0x%08X\r\n", (unsigned int)mpu.MAIR0, (unsigned int)mpu.MAIR1);
    #endif

        printf("--- %s MPU Regions Configuration ---\r\n", label);
        for (size_t i = 0U; i < STK_STATIC_ARRAY_SIZE(mpu.regions); i++)
        {
            printf("  Region %u -> RBAR: 0x%08X    R%s: 0x%08X\r\n",
                   (unsigned int)i,
                   (unsigned int)mpu.regions[i].RBAR,
                   STK_ARCH_ARMV8_M ? "LAR" : "ASR",
                   (unsigned int)mpu.regions[i].ATTR);
        }
    }
#endif

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

#if STK_MPU
        PrintMpuConfig(STK_ARCH_ARMV8_M ? "Secure" : "Primary", ctx->mpu);

    #if STK_ARCH_ARMV8_M
        printf("\r\n");
        PrintMpuConfig("Non-Secure", ctx->mpu_ns);
    #endif
#endif

        printf("=====================================================\r\n");

        __stk_debug_break();
        return false;
    }
};

void RunExample()
{
    using namespace stk;

    // For semihosting.
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

    // Secure tasks
    static SecureHwCommandQueueTask<ACCESS_PRIVILEGED> hw_cmd_proc;

    // init scheduling kernel
    kernel.Initialize();

    // register non-secure tasks (LED state ordering tasks)
    kernel.AddTask(led_task1);
    kernel.AddTask(led_task2);
    kernel.AddTask(led_task3);
    kernel.AddTask(led_task4);

    // register secure task which will interact with hardware
    kernel.AddTask(&hw_cmd_proc);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
