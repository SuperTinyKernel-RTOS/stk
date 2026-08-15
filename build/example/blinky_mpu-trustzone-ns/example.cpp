/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 *
 * -----------------------------------------------------------------------------------------------
 * TrustZone + per-task MPU sandboxing example (Non-Secure side).
 *
 * Builds on the plain TrustZone LED example: each of the 4 LED tasks now also owns a private
 * data buffer that is sandboxed via the Cortex-M MPU using ITask::GetMpuRegions(). At
 * ACCESS_USER, a task can touch:
 *   - its own stack (region 0, the automatic stack guard installed by the driver), and
 *   - its own TaskPrivateData buffer (region 1, supplied below),
 * and nothing else. Any attempt to read/write another task's private buffer -- or anything
 * outside those two regions -- takes a MemManage fault (stk::HW_EXCEPT_MEMACCESS) instead of
 * silently corrupting a neighboring task's state.
 *
 * Required stk_config.h settings for this example:
 *   #define STK_MPU              1  // enable MPU support
 *   #define STK_MPU_STACK_GUARD  1  // enable per-task MPU regions (adds region 0 automatically)
 *   #define STK_MPU_TASK_REGIONS 2  // slot 0 = stack guard, slot 1 = our private buffer below
 *
 * All linker symbols used below (__flash_binary_start/__flash_binary_end) are already exported
 * by memmap_tz_ns.ld -- no linker script changes are needed to build this example.
 * -----------------------------------------------------------------------------------------------
 */

#include <new>
#include <pico/runtime.h>

#include <stk.h>
#include <sync/stk_sync_eventflags.h>
#include <arch/arm/cortex-m/stk_arch_arm-tz.h>
#include <arch/arm/cortex-m/stk_arch_arm-cortex-m.h> // stk::MpuRegionConfig, stk::hw::mpu::*
#include <time/stk_time.h>
#include "example.h"

#if (__ARM_FEATURE_CMSE & 1) == 0
#error "Need ARMv8-M security extensions"
#elif (__ARM_FEATURE_CMSE & 2) != 0
#error "Compile without --mcmse"
#endif

#if !STK_MPU
#error "This example requires STK_MPU=1 in stk_config.h"
#endif

#include "pico/unique_id.h"

using namespace bsp;
using namespace stk::tz::nsec::std;

void NSC_OnExitNs(void);
uint32_t NSC_GetKey(uint8_t key[], uint32_t size);
void NSC_GetBoardUID(pico_unique_board_id_t *id_out);
void NSC_bsp_Led_SwitchOnExclusive(bsp::Led::Id led);

extern "C" void runtime_init_clocks(void)
{
    // clocks are initialized on Secure side of the binary
}

void pico_get_unique_board_id(pico_unique_board_id_t *id_out)
{
    // you can get Id via NSC call, if needed for a Non-Secure side of the binary
    //*id_out = pico_unique_board_id_t{};

    NSC_GetBoardUID(id_out);
}

// Size of the task's stack (number of stk::Word)
// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
static constexpr size_t TASK_STACK_SIZE = 1024U;
#else
static constexpr size_t TASK_STACK_SIZE = 256U;
#endif

// One flag bit per LED task; task 0 (RED) goes first
static const uint32_t FLAGS_ALL[] = {
    (1U << LED_RED),
    (1U << LED_ORANGE),
    (1U << LED_GREEN),
    (1U << LED_BLUE)
};

// Non-Secure task's stack memory (on ARMv8-M must be aligned to 32 bytes at least).
const size_t TASK_STACK_MEMORY_SIZE = stk::Align<size_t>(TASK_STACK_SIZE, 32U);
static stk::Word s_LedTaskStackMem[4][TASK_STACK_MEMORY_SIZE] __stk_aligned(32U);

// Non-Secure task memory (on ARMv8-M must be aligned to 32 bytes at least).
static constexpr size_t TASK_MEMORY_SIZE = stk::Align<size_t>(10U, 32U);
static stk::Word s_LedTaskMem[4][TASK_MEMORY_SIZE] __stk_aligned(32U);

// Start with the RED task's flag set so it runs first
STK_MPU_SHARED_DATA_SECTION static stk::sync::EventFlags g_TaskFlags(FLAGS_ALL[LED_RED]);

// Timeline for a precise LED switching
STK_MPU_SHARED_DATA_SECTION static stk::Ticks g_Timeline = 0;

// Task breakout example.
STK_MPU_SHARED_DATA_SECTION static stk::ITask *g_BreakoutTask = nullptr;
STK_MPU_SHARED_CODE_SECTION void SetBreakoutTask(stk::ITask *tsk)
{
    g_BreakoutTask = tsk;
}

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
             hw::mpu::MPU_CFG_CLEAR_ON_INIT |
             hw::mpu::MPU_CFG_NONSECURE_MPU)
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
        PrintMpuConfig("Non-Secure", ctx->mpu);
    #endif

        printf("=====================================================\r\n");

        __stk_debug_break();
        return false;
    }
};

// Task's core (thread)
class MyTask : public stk::ITask
{
    // Private per-task scratch memory. Each task instance gets its own, and (thanks to the MPU
    // region installed in MyTask's constructor) no other ACCESS_USER task can read or write it,
    // even though all four task instances live in the same Non-Secure .bss / are the same C++ type.
    struct TaskPrivateData
    {
        volatile uint32_t handoff_count; //!< Incremented every time this task hands off to the next LED.
    };

    uint8_t  m_task_id;
    uint32_t m_my_flag;
    uint32_t m_next_flag;

    // Some other task's private data.
    TaskPrivateData m_private;

    // MPU region exposing memory occupied by this instance.
    const stk::MpuRegionConfig m_mpu_regions[1];
    const stk::MpuRegionList   m_mpu_regions_list;

    stk::Word         *m_stack;      //!< pointer to stack buffer
    size_t             m_stack_size; //!< stack size in words
    stk::EAccessMode   m_mode;       //!< kernel access mode

public:
    MyTask(uint8_t task_id, stk::Word *stack, size_t stack_size, stk::EAccessMode mode) :
        m_task_id(task_id),
        m_my_flag(FLAGS_ALL[task_id]),
        m_next_flag(FLAGS_ALL[(task_id + 1) % LED_MAX]),
        m_private{ .handoff_count = 0 },
        m_mpu_regions{ {
            .addr        = stk::hw::PtrToWord(this),
            .size        = TASK_MEMORY_SIZE * sizeof(stk::Word),
            .access_perm = stk::hw::mpu::ACCESS_FULL,           // this task: full R/W of its own buffer
            .mem_type    = stk::hw::mpu::TYPE_NORMAL_CACHEABLE, // ordinary SRAM
            .share       = stk::hw::mpu::SHARE_NON,             // single-core, no need to share
            .exec        = stk::hw::mpu::EXEC_NEVER             // data only, never executable
        } },
        m_mpu_regions_list(m_mpu_regions, STK_STATIC_ARRAY_SIZE(m_mpu_regions)),
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
    // Supplies region 1 (the slot right after the automatic stack guard in region 0).
    const stk::MpuRegionList *GetMpuRegions() const override
    {
        return &m_mpu_regions_list;
    }

    // -------------------------------------------------------------------------------------
    // Disabled on purpose. Uncommenting this and calling it from Run() deliberately reaches
    // outside this task's sandbox (into another MyTask instance's private buffer, which this
    // task's MPU regions do NOT grant access to) and will fault with stk::HW_EXCEPT_MEMACCESS.
    // See example_s_mpu.cpp's PlatformEventHandler::OnException for how the Secure side reports
    // it (full register + MPU region dump), and OnConfigureMpu() for the background regions
    // that remain in force underneath every task's own sandbox.
    // -------------------------------------------------------------------------------------
    void TryBreakoutDemo(MyTask *other_task)
    {
        // Not part of this task's MPU regions -> MemManage fault on the very first access.
    #if 0
        other_task->m_private.handoff_count = 0xBADBADBAU;
    #endif
    }

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
            {
                const stk::hw::CriticalSection::ScopedLock __guard;
                NSC_bsp_Led_SwitchOnExclusive(static_cast<LedId>(m_task_id));
            }

            // touch only our own sandboxed buffer - allowed by our MPU region
            ++m_private.handoff_count;

            // access violation example
            TryBreakoutDemo(static_cast<MyTask *>(g_BreakoutTask));

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
};

void RunExample()
{
    using namespace stk;

    uint8_t key[4] = {0};
    NSC_GetKey(key, 4);

    // make sure memory is enough
    STK_STATIC_ASSERT(sizeof(MyTask) / sizeof(stk::Word) <= TASK_MEMORY_SIZE);

    // Non-Secure tasks
    MyTask *led_task1 = new (s_LedTaskMem[0]) MyTask(LED_RED,    s_LedTaskStackMem[0], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[0]), ACCESS_USER);
    MyTask *led_task2 = new (s_LedTaskMem[1]) MyTask(LED_ORANGE, s_LedTaskStackMem[1], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[1]), ACCESS_USER);
    MyTask *led_task3 = new (s_LedTaskMem[2]) MyTask(LED_GREEN,  s_LedTaskStackMem[2], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[2]), ACCESS_USER); // made ACCESS_PRIVILEGED as an example, see NSC_bsp_Led_SwitchOnExclusive on secure side
    MyTask *led_task4 = new (s_LedTaskMem[3]) MyTask(LED_BLUE,   s_LedTaskStackMem[3], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[3]), ACCESS_USER);

    SetBreakoutTask(led_task4);

    static tz::nsec::Kernel kernel;

    // Background Flash region (see PlatformEventHandler::OnConfigureMpu above); no general-RAM
    // fallback, so each task's isolation comes entirely from its own GetMpuRegions().
    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

    kernel.Initialize(0);

    // Register threads (tasks). Each task's MPU regions (stack guard + its own
    // TaskPrivateData, via GetMpuRegions() above) are latched in here.
    kernel.AddTask(led_task1);
    kernel.AddTask(led_task2);
    kernel.AddTask(led_task3);
    kernel.AddTask(led_task4);

    // Start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line.
    kernel.Start();

    // Return back to Secure binary. Note: kernel.Start() will exit only if Secure side initialized Kernel instance
    // with KERNEL_DYNAMIC mode and when all tasks exited on both sides.
    NSC_OnExitNs();
}
