/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 *
 * -----------------------------------------------------------------------------------------------
 * TrustZone + per-task MPU sandboxing example (Secure side).
 *
 * Builds on the plain TrustZone example: on top of the SAU split (Secure/Non-Secure/NSC), this
 * adds a second, independent layer of defense using the Secure world's own MPU:
 *
 *   1. IPlatform::IEventOverrider::OnConfigureMpu() installs system-wide "background" regions
 *      (Flash: RO+exec, general RAM: RW+no-exec, and a Privileged-only "secrets" partition) that
 *      apply underneath every task's own regions -- so even a task with no GetMpuRegions() of its
 *      own gets sane W^X defaults, and nothing (Secure or Non-Secure, any privilege) can reach the
 *      secrets partition except Secure Privileged code.
 *
 *   2. SecureHwCommandQueueTask sandboxes *itself* to just its own HW command queue via
 *      ITask::GetMpuRegions() -- defense-in-depth: even though this Secure task is Privileged and
 *      could otherwise touch all of Secure RAM, a bug in its own Run() can't stray outside the
 *      queue it actually needs.
 *
 *   3. The existing PlatformEventHandler::OnException() (unchanged) is what reports any MemManage
 *      fault raised by either of the above -- e.g. from the Non-Secure side's deliberately-disabled
 *      TryBreakoutDemo() in example_ns_mpu.cpp -- including the MPU region table active at fault
 *      time, which now tells you exactly which region did (or didn't) permit the access.
 *
 * Required stk_config.h settings for this example:
 *   #define STK_MPU             1
 *   #define STK_MPU_STACK_GUARD 1
 *   #define STK_MPU_TASK_REGIONS 2
 * -----------------------------------------------------------------------------------------------
 */

#include <setjmp.h>
#include <stdio.h>
#include <pico/runtime.h>
#include <pico/stdio.h>

#include <stk.h>
#include <arch/arm/cortex-m/stk_arch_arm-tz.h>
#include <time/stk_time.h>
#include <sync/stk_sync.h>
#include "example.h"

#if (__ARM_FEATURE_CMSE & 1) == 0
#error "Need ARMv8-M security extensions"
#elif (__ARM_FEATURE_CMSE & 2) == 0
#error "Compile with --mcmse"
#endif

#if !STK_MPU || !STK_MPU_STACK_GUARD
#error "This example requires STK_MPU=1 and STK_MPU_STACK_GUARD=1 in stk_config.h"
#endif

// Size of the task's stack (number of stk::Word)
// R2350 requires larger stack due to stack-memory heavy SDK API
#ifdef _PICO_H
static constexpr size_t TASK_STACK_SIZE = 1024U;
#else
static constexpr size_t TASK_STACK_SIZE = 256U;
#endif

// Tasks count.
#define TASK_NS_COUNT (4U)
#define TASK_S_COUNT  (1U)
#define TASK_COUNT    (TASK_NS_COUNT + TASK_S_COUNT)

// Kernel type.
#define KERNEL_TYPE   KERNEL_STATIC

using namespace bsp;
using namespace stk;

// Allocate a global jump buffer in Secure memory.
static jmp_buf s_NsExitEnv;

// Counter located in a secure_data section.
__attribute__((section(".secure_data"))) uint32_t s_Counter = 0U;

// Hw commands.
struct HwCommand
{
    enum EId
    {
        CMD_NONE   = 0,
        CMD_LED_ON = 1
    };

    EId  id;
    Word param_0;
};
static sync::PipeT<HwCommand, 4> s_HwCmdQueue;

static const MpuRegionConfig s_HwCmdQueueMpuLimit =
{
    .addr        = hw::PtrToWord(&s_HwCmdQueue),
    .size        = sizeof(s_HwCmdQueue),
    .access_perm = hw::mpu::ACCESS_FULL,
    .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
    .share       = hw::mpu::SHARE_NON,
    .exec        = hw::mpu::EXEC_NEVER
};

// This is your NSC exit function.
__stk_tz_nsc_entry void NSC_OnExitNs(void)
{
    // Jump back to the point right before BLXNS happened,
    // passing '1' as the return value for setjmp.
    longjmp(s_NsExitEnv, 1);
}

// A demo function for exposing some sensible data to Non-Secure state.
__stk_tz_nsc_entry uint32_t NSC_GetKey(uint8_t key[], uint32_t size)
{
    uint32_t result = 0;

    // Verify the entire buffer lies in Non-Secure memory and is writable
    if (cmse_check_address_range(key, size, CMSE_NONSECURE | CMSE_MPU_READWRITE) != nullptr)
    {
        if (size == 4)
        {
            key[0] = 1;
            key[1] = 2;
            key[2] = 3;
            key[3] = 4;

            result = 4;
        }
    }

    return result;
}

// A demo function for exposing some sensible data to Non-Secure state.
__stk_tz_nsc_entry void NSC_bsp_Led_SwitchOnExclusive(bsp::Led::Id led)
{
    // You can call hardware directly now, because execution is happening in a
    // Secure world and Non-Secure task is Privileged.
    if (hw::IsPrivilegedContext())
    {
        Led::SwitchOnExclusive(led);
    }
    else
    {
        // If Non-Secure task is not Privileged - direct hw calls are not possible, let Secure
        // Privileged task handle all hw calls for such calling contexts.
        s_HwCmdQueue.Write({ .id = HwCommand::CMD_LED_ON, led });
    }
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

static void Configure_SAU(void)
{
    extern char __nsc_start[];        // = ORIGIN(FLASH_NSC)
    extern char __nsc_end[];          // = ORIGIN(FLASH_NSC) + LENGTH(FLASH_NSC)
    extern char __ns_ram_start[];     // = ORIGIN(RAM_NS)
    //extern char __ns_ram_end[];       // = ORIGIN(RAM_NS) + LENGTH(RAM_NS)
    extern char __ns_flash_start[];   // = __nsc_end
    extern char __ns_flash_end[];     // = ORIGIN(FLASH_NS) + LENGTH(FLASH_NS)
    extern char __ns_scratch_start[]; // = ORIGIN = 0x2007E000
    extern char __ns_scratch_end[];   // = ORIGIN = 0x2007F000

    // Disable SAU while programming regions.
    SAU->CTRL = 0U;

    // Region 0: NSC Gateway.
    SAU->RNR  = 0U;
    SAU->RBAR = (uintptr_t)__nsc_start;
    SAU->RLAR = (((uintptr_t)__nsc_end - 1U) & ~0x1FU) | 2U | 1U; // NSC + ENABLE

    // Region 1: Non-Secure Flash (code + vector table).
    SAU->RNR  = 1U;
    SAU->RBAR = (uintptr_t)__ns_flash_start;
    SAU->RLAR = (((uintptr_t)__ns_flash_end - 1U) & ~0x1FU) | 0U | 1U; // NS + ENABLE

    // Region 2: Non-Secure RAM (TASK STACKS ONLY).
    SAU->RNR  = 2U;
    SAU->RBAR = (uintptr_t)__ns_ram_start;   // 0x20040000
    SAU->RLAR = (((uintptr_t)__ns_scratch_start - 1U) & ~0x1FU) | 0U | 1U; // NS + ENABLE

    // Region 3: SCRATCH RAM (core stacks / special buffers).
    SAU->RNR  = 3U;
    SAU->RBAR = (uintptr_t)__ns_scratch_start;
    SAU->RLAR = (((uintptr_t)__ns_scratch_end - 1U) & ~0x1FU) | 0U | 1U; // NS + ENABLE

    // Clear remaining SAU regions.
    for (uint32_t i = 4U; i < 8U; ++i)
    {
        SAU->RNR  = i;
        SAU->RBAR = 0U;
        SAU->RLAR = 0U;
    }

    // Enable SAU (default = Secure).
    SAU->CTRL = 1U;

    __DSB();
    __ISB();
}

static void ConfigureSecureState()
{
    // Enable secure fault reporting.
    SCB->SHCSR |= SCB_SHCSR_SECUREFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk |
                  SCB_SHCSR_MEMFAULTENA_Msk;

    // 1. Configure SAU boundaries before any peripheral writes.
    Configure_SAU();

    // 2. Do other initializations...
}

static void InvokeNonSecureState()
{
    typedef __stk_tz_ns_call void (* NSFuncT)(void);

    extern char __ns_flash_start[];   // = __nsc_end

    // 1. Init PSP to 0.
    __TZ_set_PSP_NS(0);
    __set_PSP(0);

    // 2. Read the initial Stack Pointer dynamically from Word 0 of the NS vector table
    // (Instead of hardcoding to the start of RAM, this ensures it uses the stack top your NS app expects)
    uint32_t *ns_vector_table = reinterpret_cast<uint32_t *>(__ns_flash_start);
    uint32_t initial_msp_ns = ns_vector_table[0];
    __TZ_set_MSP_NS(initial_msp_ns);

    // 3. Point the NON-SECURE VTOR to the Non-Secure vector table
    SCB_NS->VTOR = reinterpret_cast<uintptr_t>(__ns_flash_start);
    __DSB();
    __ISB();

    // 4. Extract the Reset Handler address (Word 1 of the NS vector table)
    uint32_t reset_handler_address = ns_vector_table[1];

    // 5. Clear Bit 0 (LSB): required by BLXNS — signals Secure-to-NonSecure transition.
    //    The vector table stores Thumb addresses (LSB=1), but BLXNS needs LSB=0.
    NSFuncT ResetHandler_ns = reinterpret_cast<NSFuncT>(reset_handler_address & ~1UL);

    // Save the current Secure CPU state.
    // The first time setjmp executes, it returns 0.
    if (setjmp(s_NsExitEnv) == 0)
    {
        // 6. Jump into the Non-Secure World.
        ResetHandler_ns();
    }
    else
    {
        // --- WE ARE BACK IN SECURE MODE PERMANENTLY ---

        // The Non-Secure binary called NSC_Secure_HandleNSExit(), which triggered longjmp(),
        // bypassing the standard C function return mechanism.

        // Continue your Secure-only main loop or application logic here.

        // Reset PSP_NS.
        __TZ_set_PSP_NS(0);
    }
}

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
                    //++s_Counter;
                    break; }
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

static void CreateKernel()
{
    Led::InitAll(false);

    // Operating in Static + Sync mode (EventFlags requires KERNEL_SYNC) and optionally tickless.
    const uint8_t KernelMode = KERNEL_TYPE | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // Allocate scheduling kernel for 3 threads (tasks) with Round-Robin scheduling strategy.
    static Kernel<KernelMode, TASK_COUNT, SwitchStrategyRR, PlatformDefault> kernel;

    // for MPU configuration and MemFault/HardFault exceptions processing (also supplies the
    // system-wide background MPU regions via OnConfigureMpu(), see above).
    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

    // Init scheduling kernel.
    kernel.Initialize();

    // Register kernel instance with TrustZone interface.
    tz::sec::SetKernel(0, &kernel);

    // Add Secure tasks:
    static SecureHwCommandQueueTask<ACCESS_PRIVILEGED> hw_cmd_task;
    kernel.AddTask(&hw_cmd_task);
}

void RunExample()
{
    // For semihosting.
    stdio_init_all();

    // Init BSP.
    Led::InitAll(false);

    // Configure Secure state by partitioning FLASH and RAM via SAU. STK does not wrap this functionality
    // to give more freedom and be less noisy in its API.
    ConfigureSecureState();

    // Create scheduler and add Secure tasks.
    CreateKernel();

    // Invoke Non-Secure state which will drive logic further.
    InvokeNonSecureState();

    // CPU is now executing your Non-Secure application, execution will never return here.
    while (true)
    {
        // KERNEL_STATIC does not support exit from scheduling, while it is possible with KERNEL_DYNAMIC
        // when all tasks exit on both sides Non-secure and Secure.
    #if (KERNEL_TYPE == KERNEL_STATIC)
        STK_ASSERT(false);
    #endif
    }
}
