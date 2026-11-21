/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  blinky_mpu_riscv/example.cpp
    \brief Blinky with per-task memory protection (RISC-V U-mode + PMP).

    \example blinky_mpu_riscv/example.cpp
*/

#include <stdio.h>
#include <new>
#ifdef _PICO_H
#include <pico/runtime.h>
#include <pico/stdio.h>
#endif

#include <stk.h>
#include <sync/stk_sync.h>
#include "example.h"

using namespace bsp;

// ---------------------------------------------------------------------------
// Memory protection setup (RISC-V, U-mode tasks + PMP)
// ---------------------------------------------------------------------------
// Tasks created with ACCESS_USER run in U-mode, tasks created with ACCESS_PRIVILEGED run in
// M-mode. Memory protection is implemented with PMP (Physical Memory Protection) entries, which
// apply to U-mode only: M-mode code (kernel and privileged tasks) is never restricted by them, so
// no entries are needed for kernel-only memory - whatever is not covered by an entry below is
// inaccessible to U-mode tasks.
//
// This example is built with STK_MPU=1, STK_MPU_STACK_GUARD=1 and STK_MPU_TASK_REGIONS=2: each task
// owns 2 PMP entries - slot 0 (automatic stack guard, configured by the kernel) and slot 1
// (one application-defined region). The per-task slots occupy the LAST entries of the PMP table
// (STK_RISCV_PMP_TASK_REGION_IDX = STK_RISCV_PMP_REGIONS_MAX - STK_MPU_TASK_REGIONS), the rest
// of the table is the budget of the static/global regions of PlatformEventHandler::OnConfigureMpu().
// PMP priority is "lowest entry index wins", so static regions take priority over per-task ones.
//
// All regions are NAPOT (naturally aligned power-of-two): size is a power of two and the base is
// aligned to the size. The linker script provides the user ROM and shared RAM windows aligned
// this way, OnConfigureMpu() verifies it with STK_ASSERT.
//
// Static/global regions configured in PlatformEventHandler::OnConfigureMpu():
//   Region 0: User ROM block - read-only, executable: text, rodata and the shared code window
//             (STK_MPU_SHARED_CODE_SECTION functions, the only place where U-mode code may
//             request a temporary privilege boost)
//   Region 1: Shared RAM     - read/write, execute-never: data + bss of STK_MPU_SHARED_DATA_SECTION
//             variables, accessible by every task
//
// Per-task regions (application-defined, see NonSecureLedTask::GetMpuRegions()):
//   Slot 0: automatic stack guard, configured internally by the kernel
//   Slot 1: task's own instance data window (the 'self' region), see TaskMemory
//
// Memory which belongs to neither window (e.g. g_SecureCounter) can be accessed by M-mode
// (privileged) tasks only. A U-mode task touching it takes a PMP access fault.
// ---------------------------------------------------------------------------

// SDK APIs (e.g. the Pico SDK) are stack-heavy and require a larger task stack
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

// Variable outside of the shared RAM window: accessible by privileged (M-mode) tasks only.
static uint32_t g_SecureCounter = 0;

// U-mode task's stack memory (NAPOT aligned, required by the PMP stack guard).
typedef stk::StackMemoryDef<TASK_STACK_SIZE> TaskStackMemory;
static TaskStackMemory::Type s_LedTaskStackMem[4];

// U-mode task instance memory (NAPOT aligned, covered by the per-task 'self' PMP region).
typedef stk::StackMemoryDef<16> TaskMemory;
static TaskMemory::Type s_LedTaskMem[4];

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
#ifdef _PICO_H
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
        stk::STK_MEMCPY(__stk_mpu_shared_data_start, __stk_mpu_shared_data_source__, data_size);
    }

    // zero-fill the BSS region
    const size_t bss_size = __stk_mpu_shared_bss_end - __stk_mpu_shared_bss_start;
    if (bss_size > 0)
    {
        stk::STK_MEMSET(__stk_mpu_shared_bss_start, 0, bss_size);
    }
}
PICO_RUNTIME_INIT_FUNC(InitMpuSharedData, "00100");
#endif

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

// U-mode task's core (thread)
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

            // uncommenting this will cause a PMP access fault (mcause 7, store access fault) due to
            // access of M-mode only memory by a U-mode task, under debugger you will see such call stack:
            //
            //   * PlatformEventHandler::OnException() at example.cpp
            //   * StkRiscvTrapDispatch() at stk_arch_risc-v.cpp
            //   * riscv_mtvec_exception() at stk_arch_risc-v.cpp
            //   * <trap>
            //   * NonSecureLedTask::Run() at example.cpp <--- points to offending ++g_SecureCounter
            //
            // note: OnException() returns false (no recovery), the driver then panics with
            //       KERNEL_PANIC_CPU_EXCEPTION
            //++g_SecureCounter;
        }
    }

#if STK_MPU
    const stk::MpuRegionList *GetMpuRegions() const override
    {
        using namespace stk;

        // With STK_MPU_TASK_REGIONS=2 each task has one application-defined slot available
        // (task-relative slot 1, following the automatic stack guard in slot 0). Everything shared
        // by all tasks (code, shared data) is configured once as a global region in
        // PlatformEventHandler::OnConfigureMpu(); this single slot is reserved for what is
        // genuinely per-instance: access to the task's own object memory.
        static MpuRegionConfig s_self_region[] =
        {
            // slot 0 is reserved by the kernel for the automatic stack guard

            { // slot 1: TASK INSTANCE DATA WINDOW - R/W for this task (NAPOT: size and alignment come from TaskMemory)
              .addr        = 0U,
              .size        = TaskMemory::SIZE_BYTES, // cover whole allocated region of the task instance
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
        // TaskMemory block (see s_LedTaskMem[]), so the base address is patched
        // in here right before the table is handed back to the kernel for this task.
        s_self_region[0].addr = hw::PtrToWord(this);

        static const stk::MpuRegionList mpu_regions(
            s_self_region, STK_STATIC_ARRAY_SIZE(s_self_region));

        return &mpu_regions;
    }
#endif
};

// Privileged (M-mode) task's core: the only one which touches hardware and M-mode only memory.
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

                    // counter is accessed by a privileged task, no PMP fault in this case
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

        extern char __stk_mpu_user_rom_start[];
        extern char __stk_mpu_user_rom_end[];
        extern char __stk_mpu_kernel_code_start[];
        extern char __stk_mpu_shared_data_start[];
        extern char __stk_mpu_shared_bss_end[];

        const Word rom_start = hw::PtrToWord(__stk_mpu_user_rom_start);
        const Word rom_end   = hw::PtrToWord(__stk_mpu_user_rom_end);
        const Word ram_start = hw::PtrToWord(__stk_mpu_shared_data_start);
        const Word ram_end   = hw::PtrToWord(__stk_mpu_shared_bss_end);

        // NAPOT: power-of-two size, base aligned to size (guaranteed by the linker script,
        // the checks catch a mismatch between the script and this table)
        auto is_napot = [](Word base, Word size) -> bool
        {
            return (size != 0U) && ((size & (size - 1U)) == 0U) && ((base & (size - 1U)) == 0U);
        };
        STK_ASSERT(is_napot(rom_start, rom_end - rom_start));
        STK_ASSERT(is_napot(ram_start, ram_end - ram_start));

        // the user ROM block must end before the kernel-only code window starts
        STK_ASSERT(rom_end <= hw::PtrToWord(__stk_mpu_kernel_code_start));

        // Static regions must not overlap (the driver panics on overlap). PMP entries are
        // U-mode only here, so privileged code is unaffected and the kernel-only code window
        // needs no entry: it lies outside the user ROM block, hence user code is denied.
        // Number of static regions must fit into STK_RISCV_PMP_TASK_REGION_IDX entries.
        static const stk::MpuRegionConfig mpu_table[] =
        {
            {   // REGION 0: User-visible ROM block (text, rodata, shared code window), R+X
                .addr        = rom_start,
                .size        = rom_end - rom_start,
                .access_perm = hw::mpu::ACCESS_PRIV_RO_USER_RO,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_ALLOWED
            },
            {   // REGION 1: Shared RAM data + bss window, R+W, no execute
                .addr        = ram_start,
                .size        = ram_end - ram_start,
                .access_perm = hw::mpu::ACCESS_FULL,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            }
        };

        // set PMP regions and notify driver that we want to enable memory protection
        static const stk::MpuConfig mpu_config
        (
            stk::MpuRegionList(mpu_table, STK_STATIC_ARRAY_SIZE(mpu_table)),
            (hw::mpu::MPU_CFG_PRIVILEGED_BG_MEM |
             hw::mpu::MPU_CFG_CLEAR_ON_INIT)
        );

        return &mpu_config;
    }
#endif

#if STK_MPU && defined(DEBUG)
#if defined(_STK_ARCH_ARM_CORTEX_M)
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
#elif defined(_STK_ARCH_RISC_V)
    //! Decode mcause of an access fault.
    static const char *GetCauseName(stk::Word mcause)
    {
        switch (mcause)
        {
        case 1:  return "Instruction access fault";
        case 5:  return "Load access fault";
        case 7:  return "Store/AMO access fault";
        default: return "Other";
        }
    }

    //! Dump PMP entries active at the time of fault (pmpcfg bytes and pmpaddr registers).
    static void PrintMpuConfig(const char *label, const stk::FaultContext &fc)
    {
        // pmpcfg CSR packs sizeof(Word) 8-bit entries (4 on RV32, 8 on RV64)
        static const unsigned ENTRIES_PER_WORD = (unsigned)sizeof(stk::Word);

        printf("--- %s PMP Regions Configuration ---\r\n", label);
        for (size_t i = 0U; i < STK_STATIC_ARRAY_SIZE(fc.mpu.addr); i++)
        {
            const unsigned cfg  = (unsigned)((fc.mpu.cfg[i / ENTRIES_PER_WORD] >> ((i % ENTRIES_PER_WORD) * 8U)) & 0xFFU);
            const unsigned mode = (cfg >> 3) & 0x3U; // A field: 0=OFF, 1=TOR, 2=NA4, 3=NAPOT
            static const char *const MODES[] = { "OFF  ", "TOR  ", "NA4  ", "NAPOT" };

            printf("  Entry %2u -> pmpcfg: 0x%02X (%s %c%c%c%s)  pmpaddr: 0x%08X",
                   (unsigned int)i, cfg, MODES[mode],
                   (cfg & 0x1U) ? 'R' : '-', (cfg & 0x2U) ? 'W' : '-', (cfg & 0x4U) ? 'X' : '-',
                   (cfg & 0x80U) ? " L" : "",
                   (unsigned int)fc.mpu.addr[i]);

            if (mode == 3U) // NAPOT: trailing ones of pmpaddr encode the size
            {
                stk::Word a = fc.mpu.addr[i];
                unsigned t = 0U;
                while ((a & 1U) != 0U) { a >>= 1U; ++t; }
                printf("  -> base 0x%08X size 0x%X", (unsigned int)((a >> 1U) << (t + 1U + 2U)), (unsigned int)(8UL << t));
            }
            printf("\r\n");
        }
    }
#endif
#endif // STK_MPU && DEBUG

    // Kernel-invoked fault handler. On ARM it fires on MemManage faults (e.g. a U-mode LED task
    // touching g_SecureCounter, or a stack-guard violation) and on generic hard faults. On RISC-V it
    // is called for PMP access faults (mcause 1, 5, 7) of U-mode tasks only (traps of M-mode code
    // do not reach this handler). Dumps CPU/MPU state for diagnostics and halts via a debug
    // breakpoint - this is intentionally non-recoverable diagnostic code, not a fault-recovery example.
#if defined(DEBUG) && defined(_STK_ARCH_ARM_CORTEX_M)
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
#elif defined(DEBUG) && defined(_STK_ARCH_RISC_V)
    bool OnException(stk::EHwException exc_id, stk::TId tid, const struct stk::FaultContext *const ctx) override
    {
        // on RISC-V the driver reports PMP access faults of U-mode code only
        printf("\r\n================ PMP ACCESS FAULT DETECTED ================\r\n");
        printf("(Sandbox Violation: a U-mode task reached outside the PMP\r\n");
        printf(" regions granted to it - see entry dump below for what\r\n");
        printf(" WAS active for this task at fault time)\r\n");
        printf("Exception: %s\r\n", (exc_id == stk::HW_EXCEPT_MEMACCESS) ? "HW_EXCEPT_MEMACCESS" : "HW_EXCEPT_FATAL");
        printf("ITask *: 0x%08X\r\n\r\n", (unsigned int)tid);

        printf("--- Trap CSRs ---\r\n");
        printf("mcause:  0x%08X (%s)\r\n", (unsigned int)ctx->mcause, GetCauseName(ctx->mcause));
        printf("mepc:    0x%08X (faulting instruction)\r\n", (unsigned int)ctx->mepc);
        printf("mtval:   0x%08X (faulting address)\r\n", (unsigned int)ctx->mtval);
        printf("mstatus: 0x%08X (MPP=%u, 0=U-mode)\r\n\r\n", (unsigned int)ctx->mstatus, (unsigned int)((ctx->mstatus >> 11) & 0x3U));

    #if STK_MPU
        PrintMpuConfig("U-mode", *ctx);
    #endif

        printf("=====================================================\r\n");

        __stk_debug_break();
        return false; // not handled: driver panics (an access fault is not recoverable here)
    }
#endif // DEBUG
};

void RunExample()
{
    using namespace stk;

    // For semihosting and logging to console.
#if defined(DEBUG) && defined(_PICO_H)
    stdio_init_all();
#endif

    Led::InitAll(false);

    // operating in Static + Sync mode (EventFlags requires KERNEL_SYNC) and optionally tickless
    const uint8_t KernelMode = KERNEL_STATIC | KERNEL_SYNC | (STK_TICKLESS_IDLE ? KERNEL_TICKLESS : 0);

    // allocate scheduling kernel for 5 threads (tasks) with Round-Robin scheduling strategy
    static Kernel<KernelMode, 5, sched::SwitchStrategyRR, PlatformDefault> kernel;

    // for PMP configuration and access fault exception processing
    static PlatformEventHandler event_overrider;
    kernel.GetPlatform()->SetEventOverrider(&event_overrider);

    // make sure memory is enough
    STK_STATIC_ASSERT(sizeof(NonSecureLedTask) <= TaskMemory::SIZE_BYTES);

    // U-mode tasks (restricted by PMP)
    NonSecureLedTask *led_task1 = new (s_LedTaskMem[0]) NonSecureLedTask(LED_RED, s_LedTaskStackMem[0], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[0]), ACCESS_USER);
    NonSecureLedTask *led_task2 = new (s_LedTaskMem[1]) NonSecureLedTask(LED_ORANGE, s_LedTaskStackMem[1], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[1]), ACCESS_USER);
    NonSecureLedTask *led_task3 = new (s_LedTaskMem[2]) NonSecureLedTask(LED_GREEN, s_LedTaskStackMem[2], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[2]), ACCESS_USER);
    NonSecureLedTask *led_task4 = new (s_LedTaskMem[3]) NonSecureLedTask(LED_BLUE, s_LedTaskStackMem[3], STK_STATIC_ARRAY_SIZE(s_LedTaskStackMem[3]), ACCESS_USER);

    // M-mode (privileged) tasks
    static SecureHwCommandQueueTask<ACCESS_PRIVILEGED> hw_cmd_proc;

    // init scheduling kernel
    kernel.Initialize();

    // register U-mode tasks (LED state ordering tasks)
    kernel.AddTask(led_task1);
    kernel.AddTask(led_task2);
    kernel.AddTask(led_task3);
    kernel.AddTask(led_task4);

    // register privileged task which will interact with hardware
    kernel.AddTask(&hw_cmd_proc);

    // start scheduler (it will start threads added by AddTask), execution in main() will be blocked on this line
    kernel.Start();

    // shall not reach here after Start() was called
    STK_ASSERT(false);
}
