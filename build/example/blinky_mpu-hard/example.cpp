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
// MPU (Memory Protection Unit) setup for EVKB-IMXRT1050 (Cortex-M7, ARMv7-M MPU
// / PMSAv7, 16 regions)
// ---------------------------------------------------------------------------
// This example is built with STK_MPU_TASK_REGIONS=2 (in addition to STK_MPU=1
// and STK_MPU_STACK_GUARD=1): each task gets only 2 hardware MPU region slots -
// slot 0 (automatic stack guard) and slot 1 (one application-defined region).
//
// On this 16-region MPU that raises STK_CORTEX_M_MPU_TASK_REGION_IDX to 14 (=
// STK_CORTEX_M_MPU_REGIONS_MAX - STK_MPU_TASK_REGIONS), so the static/global
// region budget in PlatformEventHandler::OnConfigureMpu() is 14 regions (0-13) -
// 8 of them are used below (see table). Region 7 (Peripherals) is left as a
// commented-out entry rather than enabled by default, since SecureHwCommandQueueTask
// already grants itself a dedicated Peripherals region (see its constructor);
// uncomment it there instead if other privileged code also needs raw AIPS access.
// That leaves Region 7 plus slots 9 through 13 free for future application
// additions (e.g. SEMC / SDRAM expansions or additional buffers).
//
// IMPORTANT - PMSAv7 (Cortex-M7) region rules differ from PMSAv8 (Cortex-M33):
//   * Each region's base address must be NATURALLY ALIGNED to its own size, and
//     its size must be an exact power of two (RASR.SIZE encodes log2(size) - 1).
//     PMSAv8's RBAR/RLAR pair has no such restriction (arbitrary 32-byte-granular
//     base/limit), so sizes computed purely from linker symbol deltas are not
//     automatically safe here. Linker sections like BOARD_FLASH (64MB) and
//     NCACHE_REGION (2MB) are power-of-two aligned directly by the MCUXpresso
//     script, whereas BOARD_SDRAM (30MB) is clamped to 16MB in code to strictly
//     satisfy PMSAv7 power-of-two alignment constraints.
//   * Unlike PMSAv8, PMSAv7 permits overlapping regions: on an address match,
//     the HIGHEST-numbered active region wins (no fault). Table position still
//     equals hardware region index below (entry 0 -> region 0, etc.) for the
//     8 active entries, and this example keeps the regions disjoint anyway,
//     but overlap is not the hard error it is on ARMv8-M.
//
// MPU_CFG_PRIVILEGED_BG_MEM (MPU_CTRL.PRIVDEFENA) is deliberately NOT set:
// privileged code - including the scheduler itself - gets no implicit
// "everything else" background mapping, so every address it touches (kernel
// internals, the MSP/exception stack, kernel-only globals, hardware peripherals,
// and system control spaces) must have its own explicit region defined below.
//
// Static/global regions configured in PlatformEventHandler::OnConfigureMpu():
//   Region 0: BOARD_FLASH     - read-only, executable, priv+user (FlexSPI QSPI Flash, 64 MB,
//                               __base_BOARD_FLASH to __top_BOARD_FLASH)
//   Region 1: Shared RAM      - read/write, execute-never, globals shared by every task
//                               (__stk_mpu_shared_data_start to __stk_mpu_shared_bss_end)
//   Region 2: Kernel data/BSS - read/write, execute-never, PRIVILEGED ONLY (scheduler
//                               internals + g_SecureCounter)
//   Region 3: MSP stack       - read/write, execute-never, PRIVILEGED ONLY (top of
//                               SRAM_DTC / DTCM - see memory.ld)
//   Region 4: Kernel code     - read/execute, PRIVILEGED ONLY
//   Region 5: BOARD_SDRAM     - read/write, execute-never, full access (external SDRAM,
//                               clamped to 16 MB power-of-two starting at __base_BOARD_SDRAM)
//   Region 6: NCACHE_REGION   - read/write, execute-never, non-cacheable pool for DMA descriptors
//                               (__base_NCACHE_REGION to __top_NCACHE_REGION)
//   Region 7: Peripherals     - NOT enabled by default (left commented-out below); would be
//                               read/write, execute-never, PRIVILEGED ONLY (AIPS 1-4 peripheral
//                               bridge covering GPIO, LPUART, CCM, IOMUXC at 0x40000000).
//                               SecureHwCommandQueueTask already grants itself an equivalent
//                               region, so this is only needed if other privileged code also
//                               requires raw peripheral access.
//   Region 8: System Control  - read/write, execute-never, PRIVILEGED ONLY (SCS / PPB covering
//                               NVIC, SysTick, SCB, and MPU at 0xE0000000)
//
// Slots 9 through 13 remain unallocated for static expandability.
//
// Per-task regions (application-defined, see NonSecureLedTask::GetMpuRegions()):
//   Slot 0 (Region 14): automatic stack guard, configured internally by the kernel
//   Slot 1 (Region 15): task's own instance data window (the 'self' region)
// ---------------------------------------------------------------------------

// i.MX RT1050 Hardware Peripheral & System Space Bounds
enum EHardwareMap : uint32_t
{
    PERIPHERAL_AIPS_BASE = 0x40000000UL,
    PERIPHERAL_AIPS_SIZE = 32 * 1024 * 1024, // 32 MB (AIPS-1 through AIPS-4)
    SCS_SYSTEM_BASE      = 0xE0000000UL,
    SCS_SYSTEM_SIZE      = 1 * 1024 * 1024   // 1 MB (NVIC, SysTick, MPU, SCB)
};

// Stack size (in words)
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

// Variable residing in Secure memory region accessible by only Secure/privileged code.
// Was implicitly reachable via MPU_CTRL_PRIVDEFENA_Msk background mapping; now explicitly
// isolated so it stays privileged-only even with that flag off (see PlatformEventHandler::OnConfigureMpu()).
STK_MPU_KERNEL_BSS_SECTION static uint32_t g_SecureCounter = 0;

// Non-Secure task's stack memory (on ARMv8-M must be aligned to 32 bytes at least).
static constexpr size_t TASK_STACK_MEMORY_SIZE = stk::Align<size_t>(TASK_STACK_SIZE, TASK_STACK_SIZE);
static stk::Word s_TaskStackMem[5][TASK_STACK_MEMORY_SIZE] __stk_aligned(TASK_STACK_MEMORY_SIZE * sizeof(stk::Word));

// Non-Privileged task memory (on ARMv8-M must be aligned to 32 bytes at least, next power of 2 on ARMv7-M).
static constexpr size_t TASK_MEMORY_SIZE = stk::AlignPow2<size_t>(10U, 16U);
static stk::Word s_NPrivTaskClassInstanceMem[4][TASK_MEMORY_SIZE] __stk_aligned(TASK_MEMORY_SIZE * sizeof(stk::Word));

// Privileged task memory (on ARMv8-M must be aligned to 32 bytes at least, next power of 2 on ARMv7-M).
static constexpr size_t PRIV_TASK_MEMORY_SIZE = stk::AlignPow2<size_t>(18U, 32U);
static stk::Word s_PrivTaskClassInstanceMem[1][PRIV_TASK_MEMORY_SIZE] __stk_aligned(PRIV_TASK_MEMORY_SIZE * sizeof(stk::Word));

// Custom assert handler.
#ifdef _STK_ASSERT_REDIRECT
void STK_ASSERT_HANDLER(const char *, const char *, int32_t)
{
    __stk_debug_break();
    while (true);
}
#endif

// Init mpu_shared_data.
__attribute__((constructor(101)))
static void InitMpuSharedData(void)
{
    extern char __stk_mpu_shared_data_start[];
    extern char __stk_mpu_shared_data_end[];
    extern char __stk_mpu_shared_data_source__[];
    extern char __stk_mpu_shared_bss_start[];
    extern char __stk_mpu_shared_bss_end[];

    // Copy initialized shared variables from FLASH to RAM
    const size_t data_size = (size_t)(__stk_mpu_shared_data_end - __stk_mpu_shared_data_start);
    if (data_size > 0)
    {
        STK_MEMCPY(__stk_mpu_shared_data_start, __stk_mpu_shared_data_source__, data_size);
    }

    // Zero-fill the shared BSS region
    const size_t bss_size = (size_t)(__stk_mpu_shared_bss_end - __stk_mpu_shared_bss_start);
    if (bss_size > 0)
    {
        STK_MEMSET(__stk_mpu_shared_bss_start, 0, bss_size);
    }
}

// Init mpu_kernel_data.
__attribute__((constructor(101)))
static void InitMpuKernelData(void)
{
    extern char __stk_mpu_kernel_data_start[];
    extern char __stk_mpu_kernel_data_end[];
    extern char __stk_mpu_kernel_data_source__[];
    extern char __stk_mpu_kernel_bss_start[];
    extern char __stk_mpu_kernel_bss_end[];

    // Copy initialized privileged-only kernel data from FLASH to RAM
    const size_t kernel_data_size = (size_t)(__stk_mpu_kernel_data_end - __stk_mpu_kernel_data_start);
    if (kernel_data_size > 0)
    {
        STK_MEMCPY(__stk_mpu_kernel_data_start, __stk_mpu_kernel_data_source__, kernel_data_size);
    }

    // Zero-fill the kernel BSS region
    const size_t kernel_bss_size = (size_t)(__stk_mpu_kernel_bss_end - __stk_mpu_kernel_bss_start);
    if (kernel_bss_size > 0)
    {
        STK_MEMSET(__stk_mpu_kernel_bss_start, 0, kernel_bss_size);
    }
}

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

public:
    explicit NonSecureLedTask(uint8_t task_id, stk::Word *stack, size_t stack_size)
        : m_task_id(task_id),
          m_my_flag(FLAGS_ALL[task_id]),
          m_next_flag(FLAGS_ALL[(task_id + 1) % LED_MAX]),
          m_stack(stack),
          m_stack_size(stack_size)
    {}

    // ITask
    stk::EAccessMode GetAccessMode() const override { return stk::ACCESS_USER; }

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
            // REGION 14 is reserved by the kernel for the automatic stack guard

            { // REGION 15: TASK INSTANCE DATA WINDOW - R/W for this task and privileged code
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
class SecureHwCommandQueueTask : public stk::ITask
{
    // MPU region exposing memory occupied by this instance.
    const stk::MpuRegionConfig m_mpu_regions[2];
    const stk::MpuRegionList   m_mpu_regions_list;

    // Stack memory.
    stk::Word       *m_stack;      //!< pointer to stack buffer
    size_t           m_stack_size; //!< stack size in words

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

    // ITask
    stk::EAccessMode GetAccessMode()          const override { return stk::ACCESS_PRIVILEGED; }
    const stk::MpuRegionList *GetMpuRegions() const override { return &m_mpu_regions_list; }

    // IStackMemory
    const stk::Word *GetStack()      const override { return m_stack; }
    size_t GetStackSize()            const override { return m_stack_size; }

public:
    SecureHwCommandQueueTask(stk::Word *stack, size_t stack_size)
        : m_mpu_regions
        {
            {   // Memory region of this class instance
                .addr        = stk::hw::PtrToWord(this),
                .size        = PRIV_TASK_MEMORY_SIZE * sizeof(stk::Word),
                .access_perm = stk::hw::mpu::ACCESS_FULL,
                .mem_type    = stk::hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = stk::hw::mpu::SHARE_NON,
                .exec        = stk::hw::mpu::EXEC_NEVER
            },
            {   // Peripherals (AIPS 1-4: GPIO, LPUART, CCM, IOMUXC)
                .addr        = PERIPHERAL_AIPS_BASE,
                .size        = PERIPHERAL_AIPS_SIZE,
                .access_perm = stk::hw::mpu::ACCESS_PRIV_RW_USER_NO,
                .mem_type    = stk::hw::mpu::TYPE_DEVICE,
                .share       = stk::hw::mpu::SHARE_INNER,
                .exec        = stk::hw::mpu::EXEC_NEVER
            },
        },
        m_mpu_regions_list(m_mpu_regions, STK_STATIC_ARRAY_SIZE(m_mpu_regions)),
        m_stack(stack),
        m_stack_size(stack_size)
    {}
};

class PlatformEventHandler final : public stk::IPlatform::IEventOverrider
{
#if STK_MPU
    const stk::MpuConfig *OnConfigureMpu() const override
    {
        using namespace stk;

        // ---------------------------------------------------------------------------
        // MCUXpresso Linker Memory Region Symbols
        // ---------------------------------------------------------------------------
        extern char __base_BOARD_FLASH[];
        extern char __top_BOARD_FLASH[];

        extern char __base_SRAM_DTC[];
        extern char __top_SRAM_DTC[];

        extern char __base_SRAM_ITC[];
        extern char __top_SRAM_ITC[];

        extern char __base_SRAM_OC[];
        extern char __top_SRAM_OC[];

        extern char __base_BOARD_SDRAM[];
        extern char __top_BOARD_SDRAM[];

        extern char __base_NCACHE_REGION[];
        extern char __top_NCACHE_REGION[];

        // ---------------------------------------------------------------------------
        // STK Kernel / Shared Symbols (from Linker Script)
        // ---------------------------------------------------------------------------
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_bss_end[];

        extern char __stk_mpu_kernel_data_start[];
        extern char __stk_mpu_kernel_bss_end[];

        extern char __stk_mpu_msp_stack_start[];
        extern char __stk_mpu_msp_stack_end[];

        extern char __stk_mpu_kernel_code_start[];
        extern char __stk_mpu_kernel_code_end[];

        // Static PMSAv7 MPU Region Allocation
        static const stk::MpuRegionConfig mpu_table[] =
        {
            {   // REGION 0: BOARD_FLASH (FlexSPI Serial Flash, 64 MB, Executable, Cacheable)
                .addr        = hw::PtrToWord(__base_BOARD_FLASH),
                .size        = hw::PtrToWord(__top_BOARD_FLASH) - hw::PtrToWord(__base_BOARD_FLASH),
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 1: Shared Task Data/BSS Pool (SRAM_DTC / SRAM_OC)
                .addr        = hw::PtrToWord(__stk_mpu_shared_data_start),
                .size        = hw::PtrToWord(__stk_mpu_shared_bss_end) - hw::PtrToWord(__stk_mpu_shared_data_start),
                .access_perm = hw::mpu::ACCESS_FULL,
                .mem_type    = hw::mpu::TYPE_NORMAL_NON_CACHE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            },
            {   // REGION 2: STK Kernel State & Data (Privileged-Only Access)
                .addr        = hw::PtrToWord(__stk_mpu_kernel_data_start),
                .size        = hw::PtrToWord(__stk_mpu_kernel_bss_end) - hw::PtrToWord(__stk_mpu_kernel_data_start),
                .access_perm = hw::mpu::ACCESS_PRIV_RW_USER_NO,
                .mem_type    = hw::mpu::TYPE_NORMAL_NON_CACHE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            },
            {   // REGION 3: Exception Stack (MSP Stack Guard & Data Window)
                .addr        = hw::PtrToWord(__stk_mpu_msp_stack_start),
                .size        = hw::PtrToWord(__stk_mpu_msp_stack_end) - hw::PtrToWord(__stk_mpu_msp_stack_start),
                .access_perm = hw::mpu::ACCESS_PRIV_RW_USER_NO,
                .mem_type    = hw::mpu::TYPE_NORMAL_NON_CACHE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            },
            {   // REGION 4: STK Kernel Code Space (Privileged Executable)
                .addr        = hw::PtrToWord(__stk_mpu_kernel_code_start),
                .size        = hw::PtrToWord(__stk_mpu_kernel_code_end) - hw::PtrToWord(__stk_mpu_kernel_code_start),
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_NO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 5: BOARD_SDRAM Primary Pool (Clamped to 16 MB Power-of-Two for PMSAv7)
                .addr        = hw::PtrToWord(__base_BOARD_SDRAM),
                .size        = 16 * 1024 * 1024, // 16 MB valid power of 2 (30MB top requires 2 sub-regions or clamping)
                .access_perm = hw::mpu::ACCESS_FULL,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            },
            {   // REGION 6: NCACHE_REGION (MCUXpresso Non-Cacheable Memory for DMA Descriptors)
                .addr        = hw::PtrToWord(__base_NCACHE_REGION),
                .size        = hw::PtrToWord(__top_NCACHE_REGION) - hw::PtrToWord(__base_NCACHE_REGION),
                .access_perm = hw::mpu::ACCESS_FULL,
                .mem_type    = hw::mpu::TYPE_NORMAL_NON_CACHE,
                .share       = hw::mpu::SHARE_INNER,
                .exec        = hw::mpu::EXEC_NEVER
            },
            {   // REGION 8: System Control Space (SCS: NVIC, SysTick, SCB, MPU)
                .addr        = SCS_SYSTEM_BASE,
                .size        = SCS_SYSTEM_SIZE,
                .access_perm = hw::mpu::ACCESS_PRIV_RW_USER_NO,
                .mem_type    = hw::mpu::TYPE_DEVICE,
                .share       = hw::mpu::SHARE_INNER,
                .exec        = hw::mpu::EXEC_NEVER
            },/* you can enable this region globally, or enable per Privileged task, for example SecureHwCommandQueueTask
                 can have access to GPIO, another task to another peripheral and etc
            {   // REGION 7: Peripherals (AIPS 1-4: GPIO, LPUART, CCM, IOMUXC)
                .addr        = PERIPHERAL_AIPS_BASE,
                .size        = PERIPHERAL_AIPS_SIZE,
                .access_perm = hw::mpu::ACCESS_PRIV_RW_USER_NO,
                .mem_type    = hw::mpu::TYPE_DEVICE,
                .share       = hw::mpu::SHARE_INNER,
                .exec        = hw::mpu::EXEC_NEVER
            }*/
        };

        static const stk::MpuConfig mpu_config
        (
            stk::MpuRegionList(mpu_table, STK_STATIC_ARRAY_SIZE(mpu_table)),
            static_cast<hw::mpu::EMpuConfigFlags>(
                hw::mpu::MPU_CFG_CLEAR_ON_INIT/*,
                hw::mpu::MPU_CFG_PRIVILEGED_BG_MEM */ // <<< access to background memory for Privileged tasks is prohibited in this task for full isolation
            )
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
#ifdef DEBUG
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
        PrintMpuConfig((STK_ARCH_ARMV8_M && STK_TZ_SECURE) ? "Secure" : "Primary", ctx->mpu);

    #if STK_ARCH_ARMV8_M && STK_TZ_SECURE
        printf("\r\n");
        PrintMpuConfig("Non-Secure", ctx->mpu_ns);
    #endif
#endif

        printf("=====================================================\r\n");

        __stk_debug_break();
        return false; // allow default handling by the platform driver
    }
#endif // DEBUG
};

void RunExample()
{
    using namespace stk;

    Led::InitAll(false);

    // operating in Static + Sync mode (EventFlags requires KERNEL_SYNC) and optionally tickless
    const uint8_t KernelMode = KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // allocate scheduling kernel for 3 threads (tasks) with Round-Robin scheduling strategy
    STK_MPU_KERNEL_DATA_SECTION static Kernel<KernelMode, 5, SwitchStrategyRR, PlatformDefault> kernel;

    // for MPU configuration and MemFault/HardFault exceptions processing
    STK_MPU_KERNEL_DATA_SECTION static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

    // make sure memory is enough
    STK_STATIC_ASSERT(sizeof(NonSecureLedTask) / sizeof(stk::Word) <= TASK_MEMORY_SIZE);

    // Non-Secure tasks
    NonSecureLedTask *led_task1 = new (s_NPrivTaskClassInstanceMem[0]) NonSecureLedTask(LED_RED, s_TaskStackMem[0], STK_STATIC_ARRAY_SIZE(s_TaskStackMem[0]));
    NonSecureLedTask *led_task2 = new (s_NPrivTaskClassInstanceMem[1]) NonSecureLedTask(LED_ORANGE, s_TaskStackMem[1], STK_STATIC_ARRAY_SIZE(s_TaskStackMem[1]));
    NonSecureLedTask *led_task3 = new (s_NPrivTaskClassInstanceMem[2]) NonSecureLedTask(LED_GREEN, s_TaskStackMem[2], STK_STATIC_ARRAY_SIZE(s_TaskStackMem[2]));
    NonSecureLedTask *led_task4 = new (s_NPrivTaskClassInstanceMem[3]) NonSecureLedTask(LED_BLUE, s_TaskStackMem[3], STK_STATIC_ARRAY_SIZE(s_TaskStackMem[3]));

    // Secure tasks
    SecureHwCommandQueueTask *hw_cmd_proc = new (s_PrivTaskClassInstanceMem[0]) SecureHwCommandQueueTask(s_TaskStackMem[4], STK_STATIC_ARRAY_SIZE(s_TaskStackMem[4]));

    // init scheduling kernel
    kernel.Initialize();

    // register non-Privileged tasks (LED state ordering tasks)
    kernel.AddTask(led_task1);
    kernel.AddTask(led_task2);
    kernel.AddTask(led_task3);
    kernel.AddTask(led_task4);

    // register Privileged task which will interact with hardware
    kernel.AddTask(hw_cmd_proc);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
