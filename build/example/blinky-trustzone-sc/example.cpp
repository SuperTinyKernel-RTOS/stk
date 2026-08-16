/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <setjmp.h>
#include <stdio.h>
#include <pico/runtime.h>
#include <pico/stdio.h>
#include <pico/unique_id.h>

#include <stk.h>
#include <time/stk_time.h>
#include <sync/stk_sync.h>
#include <arch/arm/cortex-m/stk_arch_arm-tz.h> // for Secure/Non-Secure-specific TrustZone API

#include "example.h"

#if (__ARM_FEATURE_CMSE & 1) == 0
#error "Need ARMv8-M security extensions"
#elif (__ARM_FEATURE_CMSE & 2) == 0
#error "Compile with --mcmse"
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

// This is your NSC exit function.
__stk_tz_nsc_entry void NSC_OnExitNs(void)
{
    // Jump back to the point right before BLXNS happened,
    // passing '1' as the return value for setjmp.
    longjmp(s_NsExitEnv, 1);
}

// A demo function for exposing SDK functions to Non-Secure side of the binary.
__stk_tz_nsc_entry void NSC_GetBoardUID(pico_unique_board_id_t *id_out)
{
    pico_get_unique_board_id(id_out);
}

// A demo function for exposing some sensible data to Non-Secure side of the binary.
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
    // Kernel-invoked fault handler: fires on MemManage faults (e.g. the Non-Secure LED
    // tasks touching g_SecureCounter, or a stack-guard violation) and on generic hard
    // faults. Dumps CPU/MPU state for diagnostics and halts via a debug breakpoint -
    // this is intentionally non-recoverable diagnostic code, not a fault-recovery example.
#ifdef DEBUG
    bool OnException(stk::EHwException exc_id, stk::TId tid, const struct stk::FaultContext *const ctx) override
    {
    #ifdef DEBUG
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
    #endif // DEBUG

        return false;
    }
#endif
};

static void CreateKernel()
{
    Led::InitAll(false);

    // Operating in Static + Sync mode (EventFlags requires KERNEL_SYNC) and optionally tickless.
    const uint8_t KernelMode = KERNEL_TYPE | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // Allocate scheduling kernel for 3 threads (tasks) with Round-Robin scheduling strategy.
    static Kernel<KernelMode, TASK_COUNT, SwitchStrategyRR, PlatformDefault> kernel;

    // for MPU configuration and MemFault/HardFault exceptions processing
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
    // For semihosting and logging to console.
#ifdef DEBUG
    stdio_init_all();
#endif

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
