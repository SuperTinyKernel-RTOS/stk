/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

// note: If missing, this header must be customized (get it in the root of the source folder) and
//       copied to the /include folder manually.
#include "stk_config.h"

#ifdef _STK_ARCH_RISC_V

#include "stk_arch.h"
#include "arch/stk_arch_common.h"
#include "stk_helper.h"

using namespace stk;

#if STK_MPU_STACK_GUARD && !STK_MPU
    #error "Enable MPU support (STK_MPU=1) to use per-task MPU feature (STK_MPU_STACK_GUARD=1)!"
#endif

/*! \def     STK_RISCV_USER_MODE
    \brief   Enables non-privileged (U-mode) task execution, independent of the PMP based memory protection.
    \details If (1), tasks created with \a ACCESS_USER run in U-mode (mstatus.MPP = 0) while \a ACCESS_PRIVILEGED
             tasks run in M-mode. In this mode the driver provides: the privilege state tracking (s_StkRiscvNpriv),
             ecall services (critical section, privilege boost/drop), the kernel service proxy for U-mode callers
             (see IKernelService::GetInstance()) and U-mode aware CriticalSection API.
             The hart must implement the U-mode (privilege levels M+U). Memory protection (PMP regions, stack guard)
             is a separate feature on top of it, see \a STK_MPU.
    \note    Default: follows \a STK_MPU (backward compatible). Define as (1) before including this file (or in
             stk_config.h) to run \a ACCESS_USER tasks in U-mode without \a STK_MPU. Always (1) if \a STK_MPU is (1).
    \note    Without \a STK_MPU there is no isolation: the shared code region check of the privilege boost
             is not performed and U-mode is granted access to the whole address space (see \a STK_RISCV_PMP_PRESENT).
    \see     STK_MPU, STK_RISCV_PMP_PRESENT
*/
#ifndef STK_RISCV_USER_MODE
    #define STK_RISCV_USER_MODE STK_MPU
#endif
#if STK_MPU && !STK_RISCV_USER_MODE
    #error "STK_MPU requires STK_RISCV_USER_MODE=1 (PMP restricts U-mode only)!"
#endif
#if STK_RISCV_USER_MODE && (STK_ARCH_CPU_COUNT > 1U) && !defined(STK_ARCH_GET_CPU_ID)
    // the privilege state (s_StkRiscvNpriv[]) and the per-hart Context are looked up by hart id from U-mode code too,
    // the default implementation reads mhartid CSR which traps in U-mode
    #error "Multi-hart U-mode build (STK_RISCV_USER_MODE/STK_MPU) requires STK_ARCH_GET_CPU_ID() which is readable from U-mode (mhartid CSR is M-mode only)!"
#endif

/*! \def     STK_RISCV_PMP_PRESENT
    \brief   If (1), the hart implements PMP. Used only if \a STK_RISCV_USER_MODE is (1) and \a STK_MPU is (0).
    \details RISC-V rule: if PMP is implemented but no entry matches, U-mode access fails. Therefore, to let U-mode
             tasks run without \a STK_MPU the driver programs one permissive PMP entry (RWX, whole address space).
             Set to (0) if the hart has no PMP at all (all U-mode accesses are then allowed by hardware and the
             PMP CSRs must not be touched).
*/
#ifndef STK_RISCV_PMP_PRESENT
    #define STK_RISCV_PMP_PRESENT (1)
#endif

/*! \def     STK_RISCV_PMP_TASK_REGION_IDX
    \brief   Base PMP entry index reserved for per-task regions (the last STK_MPU_TASK_REGIONS entries by default).
    \note    PMP priority: the lowest entry index wins, therefore static regions (low indices) have priority
             over per-task regions.
*/
#if STK_MPU
    #ifndef STK_RISCV_PMP_TASK_REGION_IDX
        #define STK_RISCV_PMP_TASK_REGION_IDX (STK_RISCV_PMP_REGIONS_MAX - STK_MPU_TASK_REGIONS)
    #endif
    #if (STK_RISCV_PMP_REGIONS_MAX > 16U)
        #error "Only up to 16 PMP entries are supported by the driver."
    #endif
#endif

/*! \brief External symbols defining the boundary addresses of the shared code region (see linker script).
    \note  Used by the privilege boost/drop service to accept requests only from the shared code.
*/
#if STK_MPU
    extern char __stk_mpu_shared_code_start[];
    extern char __stk_mpu_shared_code_end[];
#endif

/*! \var    g_StkLastPanicId
   \brief  Panic id cache for post-mortem inspection.
   \note   Intentionally outside of the anonymous namespace: external C linkage keeps an unmangled,
           stable symbol name for debuggers and tools; __stk_attr_used prevents it from being discarded.
*/
extern "C" volatile EKernelPanicId g_StkLastPanicId;
__stk_attr_used
STK_MPU_KERNEL_BSS_SECTION
volatile EKernelPanicId g_StkLastPanicId = KERNEL_PANIC_NONE;

/*! \def   _STK_RISCV_USE_PENDSV
    \brief Enables ARM-style PendSV-equivalent context switching via CLINT MSIP.

   \details
   ARM Cortex-M provides a dedicated PendSV interrupt that fires at the lowest
   hardware priority, allowing a timer ISR to trigger a context switch without
   paying the full save/restore cost inline - the switch is deferred until no
   higher-priority ISR is active.

   RISC-V M-mode has no equivalent mechanism. When this macro is defined, STK
   emulates the pattern using the Machine Software Interrupt (MSIP):

    1. STK_SYSTICK_HANDLER fires on MTIMER expiry, saves the interrupted task's
       context to a per-hart scratch slot (s_StkRiscvSpIsrInt), switches to the
       private ISR stack, and calls TrySwitchContext.
    2. If the scheduler selects a new task, TrySwitchContext sets MSIP[hart] = 1
       via the CLINT, pending a software interrupt.
    3. STK_SYSTICK_HANDLER restores the interrupted task and returns via mret,
       which immediately re-enters the trap due to the pending MSIP.
    4. STK_MSI_HANDLER saves the task's context to the idle stack, clears MSIP,
       loads the active task's context, and returns via mret to the new task.

   \warning
   On standard M-mode RISC-V (without CLIC), interrupts are non-preemptible and
   MSIP fires immediately after mret - there is no genuine deferral window.
   The net result on a context-switch tick is two full save/restore cycles and
   two mret transitions instead of one, with no scheduling latency benefit over
   the simpler single-handler path.

   This macro should only be enabled if targeting a RISC-V core with a hardware
   preemptible interrupt controller (e.g. CLIC) where deferring to a lower
   software-interrupt priority level provides a measurable benefit. For all
   standard M-mode deployments the single-handler path (this macro undefined)
   is preferred: lower overhead, simpler control flow, identical scheduling
   behaviour.

   \see STK_SYSTICK_HANDLER, STK_MSI_HANDLER, HW_ScheduleContextSwitch
 */
//#define _STK_RISCV_USE_PENDSV

// CLINT
// Details: https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc
#ifndef STK_RISCV_CLINT_BASE_ADDR
    #define STK_RISCV_CLINT_BASE_ADDR (0x2000000U)
#endif
#ifndef STK_RISCV_CLINT_MTIMECMP_ADDR
    #define STK_RISCV_CLINT_MTIMECMP_ADDR (STK_RISCV_CLINT_BASE_ADDR + 0x4000U) // 8-byte value, 1 per hart
#endif
#ifndef STK_RISCV_CLINT_MTIME_ADDR
    #define STK_RISCV_CLINT_MTIME_ADDR (STK_RISCV_CLINT_BASE_ADDR + 0xBFF8U) // 8-byte value, global
#endif

/*! \brief  Size in bytes of the private ISR stack allocated by Context.
    \note   Must be large enough to accommodate TrySwitchContext() and the scheduler call chain.
            Increase if STK_KERNEL_PANIC or other deep call paths are added to the ISR.
*/
#define STK_RISCV_ISR_STACK_SIZE 256U

/*! \def   STK_TIMER_CLOCK_FREQUENCY
    \brief System clock frequency in Hz. Default: 1 MHz.
*/
#ifndef STK_TIMER_CLOCK_FREQUENCY
    #define STK_TIMER_CLOCK_FREQUENCY 1000000U
#endif

// Spin lock timeout.
#ifndef STK_SPINLOCK_TIMEOUT_US
    #define STK_SPINLOCK_TIMEOUT_US (5U * 1000U * 1000U) // 5 sec
#endif

// Conservative lower bound on cycles consumed per iteration of the HW_SpinLockLock retry
// loop (atomic test-and-set attempt + compare + branch + relax hint). Deliberately an
// underestimate, so the derived iteration budget always waits at least
// STK_SPINLOCK_TIMEOUT_US before giving up rather than timing out early. If
// __stk_relax_cpu() ever becomes a multi-cycle instruction (or the pause/fence it lowers
// to on a given target is costlier) on some core, raise this.
#ifndef STK_SPINLOCK_MIN_CYCLES_PER_ITER
    #define STK_SPINLOCK_MIN_CYCLES_PER_ITER (4U)
#endif

/*! \brief  Optional section attribute for placing ISR handlers into a dedicated linker section.
    \note   Define before including this file to override (e.g. __attribute__((section(".isr")))).
            Defaults to empty, handlers are placed in the default text section.
*/
#ifndef STK_RISCV_ISR_SECTION
    #define STK_RISCV_ISR_SECTION
#endif

/*! \brief  Declares a C-linkage machine-mode interrupt handler placed in STK_RISCV_ISR_SECTION.
    \note   __attribute__((interrupt("machine"))) instructs the compiler to use mret instead of
            ret and to save/restore caller-saved registers in the prologue/epilogue.
*/
#define STK_RISCV_ISR extern "C" STK_RISCV_ISR_SECTION __attribute__ ((interrupt("machine")))

//! ASM shortcuts:
#define STK_ASM_EXIT_FROM_HANDLER "mret"

/*! \brief  Controls whether MTIMECMP is programmed per hart (1) or shared across all harts (0).
    \note   Set to 1 for multi-core targets where each hart requires an independent tick deadline.
            Set to 0 only if the platform uses a single shared MTIMECMP for all harts.
            Defaults to 1.
*/
#ifndef STK_RISCV_CLINT_MTIMECMP_PER_HART
    #define STK_RISCV_CLINT_MTIMECMP_PER_HART 1
#endif

/*! \brief  Get the hardware thread id (hart) of the calling core.
    \note   Defaults to reading the mhartid CSR. Define before including this file to override
            with a platform-specific implementation if mhartid is unavailable or aliased.
    \warning With STK_RISCV_USER_MODE and STK_ARCH_CPU_COUNT > 1 this macro is also evaluated by U-mode code
             (privilege state lookup, see s_StkRiscvNpriv), therefore it must be U-mode safe (must not access
             M-mode CSRs, e.g. read a memory-mapped hart id or a value kept in a U-mode readable location)
             and must be defined by the user.
*/
#ifndef STK_ARCH_GET_CPU_ID
    #define STK_ARCH_GET_CPU_ID() read_csr(mhartid)
#endif

//! FPU presence (FPU present=1).
#if (__riscv_flen == 0)
    #define STK_RISCV_FPU 0
#else
    #define STK_RISCV_FPU __riscv_flen
#endif

#define STR(x) #x
#define XSTR(s) STR(s)

//! CPU register access portable defines.
#if (__riscv_xlen == 32)
    #define REGBYTES XSTR(4)
    #define LREG     XSTR(lw)
    #define SREG     XSTR(sw)
#elif (__riscv_xlen == 64)
    #define REGBYTES XSTR(8)
    #define LREG     XSTR(ld)
    #define SREG     XSTR(sd)
#else
    #error Unsupported RISC-V platform!
#endif

#if (STK_RISCV_FPU == 32)
    #define FREGBYTES XSTR(4)
    #define FLREG     XSTR(flw)
    #define FSREG     XSTR(fsw)
#elif (STK_RISCV_FPU == 64)
    #define FREGBYTES XSTR(8)
    #define FLREG     XSTR(fld)
    #define FSREG     XSTR(fsd)
#elif (STK_RISCV_FPU != 0)
#error Unsupported FP register count!
#endif


#if (__riscv_32e == 1)
    #define STK_RISCV_REGISTER_COUNT (15U + (STK_RISCV_FPU != 0U ? 31U : 0U))
#else
    #define STK_RISCV_REGISTER_COUNT (31U + (STK_RISCV_FPU != 0U ? 31U : 0U))
#endif

#define STK_SERVICE_SLOTS 2U // (0) mepc, (1) mstatus

#if (__riscv_32e == 1)
    #define FOFFSET XSTR(68) // FP stack offset = (17 * 4)
    #if (STK_RISCV_FPU == 0)
        #define REGSIZE XSTR(((15U + STK_SERVICE_SLOTS) * 4U)) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus
    #else
        #if (STK_RISCV_FPU == 32)
        #define REGSIZE XSTR((((15U + STK_SERVICE_SLOTS) * 4U) + (31U * 4U))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #elif (STK_RISCV_FPU == 64)
        #define REGSIZE XSTR((((15U + STK_SERVICE_SLOTS) * 4U) + (31U * 8U))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #endif
    #endif
#elif (__riscv_xlen == 32)
    #define FOFFSET XSTR(132) // FP stack offset = (33 * 4)
    #if (STK_RISCV_FPU == 0)
        #define REGSIZE XSTR(((31U + STK_SERVICE_SLOTS) * 4U)) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus
    #else
        #if (STK_RISCV_FPU == 32)
        #define REGSIZE XSTR((((31U + STK_SERVICE_SLOTS) * 4U) + (31U * 4U))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #elif (STK_RISCV_FPU == 64)
        #define REGSIZE XSTR((((31U + STK_SERVICE_SLOTS) * 4U) + (31U * 8U))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #endif
    #endif
#elif (__riscv_xlen == 64)
    #define FOFFSET XSTR(264) // FP stack offset = (33 * 8)
    #if (STK_RISCV_FPU == 0)
        #define REGSIZE XSTR(((31U + STK_SERVICE_SLOTS) * 8U)) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus
    #else
        #if (STK_RISCV_FPU == 32)
        #define REGSIZE XSTR((((31U + STK_SERVICE_SLOTS) * 8U) + (31U * 4U))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #elif (STK_RISCV_FPU == 64)
        #define REGSIZE XSTR((((31U + STK_SERVICE_SLOTS) * 8U) + (31U * 8U))) // STK_RISCV_REGISTER_COUNT + 2 for mepc, mstatus + 32 fp registers
        #endif
    #endif
#endif

#if (__riscv_xlen == 32)
    #define REGBYTES_LOG2 "2" // log2(4) - used for hart-index shift
#elif (__riscv_xlen == 64)
    #define REGBYTES_LOG2 "3" // log2(8)
#endif

/*! \brief  Name of the machine timer interrupt (MTI) handler entry point.
    \note   Fires periodically at the rate configured by STK_RISCV_CLINT_MTIMECMP_ADDR and drives
            the scheduler tick. Define before including this file to match the vector table symbol
            name of the target platform.
            Defaults to riscv_mtvec_mti (see vector_table.h / vector_table.c).
*/
#ifndef STK_SYSTICK_HANDLER
    #define STK_SYSTICK_HANDLER riscv_mtvec_mti
#endif

/*! \brief  Name of the machine exception handler entry point.
    \note   Handles ecall (scheduler startup) and other synchronous exceptions such as
            illegal instruction or misaligned access. Define before including this file
            to match the vector table symbol name of the target platform.
            Defaults to riscv_mtvec_exception (see vector_table.h / vector_table.c).
*/
#ifndef STK_SVC_HANDLER
    #define STK_SVC_HANDLER riscv_mtvec_exception
#endif

/*! \brief  Name of the machine software interrupt (MSI) handler entry point.
    \note   Used as the PendSV equivalent on RISC-V: pended by STK_SYSTICK_HANDLER after
            scheduling to perform the actual context switch. Define before including this file
            to match the vector table symbol name of the target platform.
            Defaults to riscv_mtvec_msi (see vector_table.h / vector_table.c).
    \see    _STK_RISCV_USE_PENDSV
*/
#ifndef STK_MSI_HANDLER
    #define STK_MSI_HANDLER riscv_mtvec_msi
#endif

//! mcause values handled by the MPU support (synchronous exceptions).
#define STK_RISCV_CAUSE_IACCESS_FAULT (1U)
#define STK_RISCV_CAUSE_LACCESS_FAULT (5U)
#define STK_RISCV_CAUSE_SACCESS_FAULT (7U)
#define STK_RISCV_CAUSE_ECALL_U       (8U)
#define STK_RISCV_CAUSE_ECALL_M       (11U)

//! Service ecall: register a5 carries (STK_RISCV_SVC_MAGIC | id), a0 an argument. Magic lets M-mode service calls
//! be told apart from the scheduler start ecall and from the user-forwarded ecalls (a5 is available on RV32E too).
#define STK_RISCV_SVC_MAGIC           (0x53544B00)

// ****************************************************************************
namespace { // file-local block (private)
// ****************************************************************************

/*! \struct TaskFrame
    \brief  Complete initial task register frame laid out at the bottom of a new stack
            (lowest address), matching the layout the ISR context-save/restore assembly
            expects at sp+0 after "addi sp, sp, -REGSIZE".

    Member order is low-to-high address (ascending), matching the slot offsets used
    by STK_ASM_SAVE_CONTEXT / STK_ASM_LOAD_CONTEXT:
      [0] MEPC    - machine exception PC (entry function address)
      [1] MSTATUS - machine status (privilege + interrupt enable flags)
      [2] X1/RA   - return address (x1)
      [3] X2/SP   - stack pointer slot (skipped by save/restore, zero-initialised)
      [4] X3/GP   - global pointer slot, repurposed as FSR when FPU present
      [5..N] X4-X15 (RV32E), or X4-X31 (RV32I/RV64)
      [N+1..] F0-F31 (when FPU present)

    Only MEPC, MSTATUS, X1 (RA), X3 (FSR), and X10 (first argument) are set by
    InitStack, all other slots are left STK_STACK_MEMORY_FILLER-initialised by InitStackMemory.
*/
struct TaskFrame
{
    // Service slots (indices 0, 1) - sit at sp+0 and sp+REGBYTES
    Word MEPC;    //!< Machine exception PC: entry function address, restored to mepc on first mret.
    Word MSTATUS; //!< Machine status: MPP + MPIE + optional FS/XS, restored to mstatus on first mret.

    // General-purpose register slots (indices 2..N), one per xN
    Word X1_RA;   //!< x1, return address - OnTaskExit for user tasks, FILLER for traps.
    Word X2_SP;   //!< x2, stack pointer slot - skipped by context save/restore, STK_STACK_MEMORY_FILLER-initialised.
#if (STK_RISCV_FPU != 0)
    Word X3_FSR;  //!< x3/gp slot repurposed: holds initial FCSR value (0 = round-to-nearest, no flags).
#else
    Word X3_GP;   //!< x3, global pointer slot - skipped by save/restore, STK_STACK_MEMORY_FILLER-initialised.
#endif
    Word X4;      //!< x4, tp - STK_STACK_MEMORY_FILLER-initialised.
    Word X5;      //!< x5, t0 - STK_STACK_MEMORY_FILLER-initialised.
    Word X6;      //!< x6, t1 - STK_STACK_MEMORY_FILLER-initialised.
    Word X7;      //!< x7, t2 - STK_STACK_MEMORY_FILLER-initialised.
    Word X8;      //!< x8, s0/fp - STK_STACK_MEMORY_FILLER-initialised.
    Word X9;      //!< x9, s1 - STK_STACK_MEMORY_FILLER-initialised.
    Word X10_A0;  //!< x10, a0 - first function argument (user_task pointer for STACK_USER_TASK).
    Word X11;     //!< x11, a1 - STK_STACK_MEMORY_FILLER-initialised.
    Word X12;     //!< x12, a2 - STK_STACK_MEMORY_FILLER-initialised.
    Word X13;     //!< x13, a3 - STK_STACK_MEMORY_FILLER-initialised.
    Word X14;     //!< x14, a4 - STK_STACK_MEMORY_FILLER-initialised.
    Word X15;     //!< x15, a5 - STK_STACK_MEMORY_FILLER-initialised.
#if (__riscv_32e != 1)
    Word X16;     //!< x16 - STK_STACK_MEMORY_FILLER-initialised.
    Word X17;     //!< x17 - STK_STACK_MEMORY_FILLER-initialised.
    Word X18;     //!< x18, s2 - STK_STACK_MEMORY_FILLER-initialised.
    Word X19;     //!< x19, s3 - STK_STACK_MEMORY_FILLER-initialised.
    Word X20;     //!< x20, s4 - STK_STACK_MEMORY_FILLER-initialised.
    Word X21;     //!< x21, s5 - STK_STACK_MEMORY_FILLER-initialised.
    Word X22;     //!< x22, s6 - STK_STACK_MEMORY_FILLER-initialised.
    Word X23;     //!< x23, s7 - STK_STACK_MEMORY_FILLER-initialised.
    Word X24;     //!< x24, s8 - STK_STACK_MEMORY_FILLER-initialised.
    Word X25;     //!< x25, s9 - STK_STACK_MEMORY_FILLER-initialised.
    Word X26;     //!< x26, s10 - STK_STACK_MEMORY_FILLER-initialised.
    Word X27;     //!< x27, s11 - STK_STACK_MEMORY_FILLER-initialised.
    Word X28;     //!< x28, t3 - STK_STACK_MEMORY_FILLER-initialised.
    Word X29;     //!< x29, t4 - STK_STACK_MEMORY_FILLER-initialised.
    Word X30;     //!< x30, t5 - STK_STACK_MEMORY_FILLER-initialised.
    Word X31;     //!< x31, t6 - STK_STACK_MEMORY_FILLER-initialised.
#endif
#if (STK_RISCV_FPU != 0)
    // FP register slots - at FOFFSET from frame base, immediately after integer slots.
    // FREGBYTES may differ from REGBYTES (32-bit FP on a 64-bit integer machine).
    // Declared as Word arrays for uniform struct sizing; the FP load/store
    // instructions address them by byte offset and tolerate the type mismatch.
    Word F[32];   //!< f0-f31, all STK_STACK_MEMORY_FILLER-initialised (round-to-nearest set via X3_FSR).
#endif

    //! Check if context is privileged (MSTATUS_MPP bit set).
    __stk_forceinline bool IsPrivileged() const
    {
        return ((MSTATUS & MSTATUS_MPP) == MSTATUS_MPP);
    }

    //! Make non-privileged.
    __stk_forceinline void MakeNonPrivileged()
    {
        MSTATUS &= ~static_cast<Word>(MSTATUS_MPP);
    }

    //! Make privileged.
    __stk_forceinline void MakePrivileged()
    {
        MSTATUS |= MSTATUS_MPP;
    }
};

// Local functions of the driver:
// ----------------------------------------------------------------------------
namespace drv {
// ----------------------------------------------------------------------------

void OnTaskRun(ITask *runnable);
void OnTaskExit();
void OnSchedulerSleep();
void OnSchedulerSleepOverride();
void OnSchedulerExit();

// ----------------------------------------------------------------------------
} // namespace drv
// ----------------------------------------------------------------------------

// ****************************************************************************
} // namespace {
// ****************************************************************************

/*! Pointer Cache --------------------------------------------------------------

    ISR assembly pointer cache: stack pointers used by naked ISR handlers for
    context switching. Indexed by hart id (0 for single-core builds). Declared
    volatile to prevent the compiler from caching reads.

    \note   Intentionally outside of the anonymous namespace: the ISR assembly references
            these symbols by name ("la reg, sym"), therefore they require external C linkage
            to keep an unmangled, stable symbol name; __stk_attr_used prevents LTO discarding.
*/
extern "C" {

/*! \brief  Pointer to the idle task stack, per hart. Written by scheduler,
            read by STK_MSI_HANDLER.
*/
extern Stack *volatile s_StkRiscvStackIdle[STK_ARCH_CPU_COUNT];

/*! \brief  Pointer to the active task stack, per hart. Written by scheduler,
            read by STK_MSI_HANDLER (PendSV) or STK_SYSTICK_HANDLER (non-PendSV).
*/
extern Stack *volatile s_StkRiscvStackActive[STK_ARCH_CPU_COUNT];

/*! \brief  Pointer to the private ISR stack, per hart. Written once by
            Context::OnStart, read by both ISR handlers.
*/
extern Stack *volatile s_StkRiscvStackIsr[STK_ARCH_CPU_COUNT];

#ifdef _STK_RISCV_USE_PENDSV
/*! \brief  Scratch storage for the SP of the task interrupted by STK_SYSTICK_HANDLER, per hart.
    \note   Written and read exclusively by STK_SYSTICK_HANDLER. Never touched by
            STK_MSI_HANDLER, making it safe across mutual preemption without
            requiring mscratch or a full Stack struct.
*/
extern volatile Word s_StkRiscvSpIsrInt[STK_ARCH_CPU_COUNT];
#endif

} // extern "C"

__stk_attr_used
STK_MPU_KERNEL_BSS_SECTION
Stack *volatile s_StkRiscvStackIdle[STK_ARCH_CPU_COUNT] = {};

__stk_attr_used
STK_MPU_KERNEL_BSS_SECTION
Stack *volatile s_StkRiscvStackActive[STK_ARCH_CPU_COUNT] = {};

__stk_attr_used
STK_MPU_KERNEL_BSS_SECTION
Stack *volatile s_StkRiscvStackIsr[STK_ARCH_CPU_COUNT] = {};

#ifdef _STK_RISCV_USE_PENDSV
__stk_attr_used
STK_MPU_KERNEL_BSS_SECTION
volatile Word s_StkRiscvSpIsrInt[STK_ARCH_CPU_COUNT] = {};
#endif

// ****************************************************************************
namespace { // file-local block (private)
// ****************************************************************************

// Local static variables:
STK_MPU_KERNEL_BSS_SECTION
#if STK_CORE_FREQ_UNIFIED
uint32_t s_StkSystemCoreClock[1U] = {};
#else
uint32_t s_StkSystemCoreClock[STK_ARCH_CPU_COUNT] = {};
#endif

/*! \brief    Cached spin-lock timeout, in retry-loop iterations.
    \details  Derived from STK_SPINLOCK_TIMEOUT_US and the real core clock frequency by
              HW_InitSpinlockTimeout(), which must be called once per hart during
              Context::Initialize() before any spinlock is used in anger. The initial
              value here is a conservative fallback (matches the old fixed-iteration-count
              behavior) so that any spinlock attempt occurring before Initialize() has run
              still has a bounded, non-zero timeout instead of panicking immediately.
*/
STK_MPU_SHARED_DATA_SECTION
uint32_t s_StkSpinlockTimeoutIters = 0xFFFFFFFU;

#if (STK_ARCH_CPU_COUNT > 1U)
/*! \brief  Global lock to synchronize critical sections of multiple cores.
*/
STK_MPU_KERNEL_BSS_SECTION
volatile bool s_StkRiscvCsuLock = false;
#endif

/*! \brief  Order all predecessor Read/Write with all successor Read/Write (similar to ARM's __DSB(DSB ISH)).
*/
__stk_forceinline void __DSB()
{
    __asm__ __volatile__("fence rw, rw" ::: "memory");
}

/*! \brief  Flush the instruction cache and pipeline (similar to ARM's __ISB).
*/
__stk_forceinline void __ISB()
{
#ifdef __riscv_zifencei
    __asm__ __volatile__("fence.i" ::: "memory");
#else
    __sync_synchronize();
#endif
}

/*! \brief  Put core into a low-power state (similar to ARM's __WFI).
*/
__stk_forceinline void __WFI()
{
    __asm__ __volatile__("wfi");
}

/*! \brief  Triggers scheduler startup via M-mode environment call (mcause = RISCV_EXCP_ENVIRONMENT_CALL_FROM_M_MODE).
    \note   Caught by STK_SVC_HANDLER which detects m_starting and calls Context::OnStart().
*/
__stk_forceinline void HW_StartScheduler()
{
    __asm__ __volatile__("ecall");
}

/*! \brief  Get current (caller's) Hart Id.
*/
__stk_forceinline uint8_t HW_GetHartId()
{
    return STK_ARCH_GET_CPU_ID();
}

/*! \brief  Disable CPU interrupts.
*/
__stk_forceinline void HW_DisableInterrupts()
{
    __asm__ __volatile__("csrrci zero, mstatus, %0"
    : /* output: none */
    : "i"(MSTATUS_MIE)
    : /* clobbers: none */);
}

/*! \brief  Enable CPU interrupts.
*/
__stk_forceinline void HW_EnableInterrupts()
{
    __asm__ __volatile__("csrrsi zero, mstatus, %0"
    : /* output: none */
    : "i"(MSTATUS_MIE)
    : /* clobbers: none */);
}

/*! \brief  Enter critical section.
    \return Session value which has to be supplied to HW_ExitCriticalSection().
*/
__stk_forceinline Word HW_EnterCriticalSection()
{
    Word ses;
    __asm__ __volatile__("csrrci %0, mstatus, %1"
    : "=r"(ses)
    : "i"(MSTATUS_MIE)
    :  /* clobbers: none */);

    return ses;
}

/*! \brief     Exit critical section.
    \param[in] ses: Session value obtained by HW_EnterCriticalSection().
*/
__stk_forceinline void HW_ExitCriticalSection(Word ses)
{
    __asm__ __volatile__("csrrs zero, mstatus, %0"
    : /* output: none */
    : "r"(ses)
    : /* clobbers: none */);
}

/*! \brief  Enter low-power/sleep mode.
*/
__stk_forceinline void HW_EnterSleepMode()
{
    __DSB(); // data barrier
    __WFI(); // enter standby mode until time slot expires
}

/*! \brief  Stop machine timer.
*/
__stk_forceinline void HW_StopMTimer()
{
    clear_csr(mie, MIP_MTIP);
}

/*! \brief     Switch context by scheduling PendSV interrupt.
    \param[in] hart: Hard id.
    \note      Pend Machine Software Interrupt (MSI) - equivalent of ARM's PENDSVSET.
*/
__stk_forceinline void HW_ScheduleContextSwitch(uint8_t hart)
{
#ifdef STK_RISCV_SOFTIRQ_ADDR
    // RP2350: SET bit for the hart is at bit position [hart] of SIO RISCV_SOFTIRQ (see SIO_RISCV_SOFTIRQ_OFFSET)
    *(volatile uint32_t *)(STK_RISCV_SOFTIRQ_ADDR) = (1U << hart);
#else
    volatile uint32_t *msip = (volatile uint32_t *)(STK_RISCV_CLINT_BASE_ADDR);
    msip[hart] = 1U; // set pending
#endif
    __DSB();
}

/*! \brief Clear pending switch by SV exception.
    \note  Unconditional (not gated by _STK_RISCV_USE_PENDSV): the MSI interrupt is now
           always enabled, since Context::OnForceContextSwitch() relies on it for
           asynchronous/forced switches even on the single-handler (non-PendSV) tick path.
*/
__stk_forceinline void HW_ClearPendingSwitch()
{
    clear_csr(mie, MIP_MSIP);
}

/*! \brief  Get CPU core clock frequency.
    \return Frequency in Hz.
*/
__stk_forceinline uint32_t HW_CoreClockFrequency()
{
#if STK_CORE_FREQ_UNIFIED
    return s_StkSystemCoreClock[0];
#else
    return s_StkSystemCoreClock[STK_ARCH_GET_CPU_ID()];
#endif
}

/*! \brief  Get mtime reference clock frequency.
    \note   By default mtime is driven by a fixed 1 MHz reference, independent of the CPU PLL.
    \return Frequency in Hz.
*/
__stk_forceinline uint32_t HW_MtimeClockFrequency()
{
    return STK_TIMER_CLOCK_FREQUENCY; // timer frequency, e.g. 1 MHz
}

/*! \brief  Get mtime.
    \return Ticks.
    \note   mtime is a free-running 64-bit counter driven by a fixed-frequency
            reference clock (not the CPU clock).
*/
__stk_forceinline uint64_t HW_GetMtime()
{
#if ( __riscv_xlen > 32)
    return *(hw::WordToPtr<volatile uint64_t>(STK_RISCV_CLINT_MTIME_ADDR));
#else
    const Word mtime_base = STK_RISCV_CLINT_MTIME_ADDR;
    const volatile uint32_t *const mtime_lo = hw::WordToPtr<volatile uint32_t>(mtime_base);
    const volatile uint32_t *const mtime_hi = hw::WordToPtr<volatile uint32_t>(mtime_base + sizeof(uint32_t));

    uint32_t hi, lo;
    do
    {
        hi = (*mtime_hi);
        lo = (*mtime_lo);
    }
    while (hi != (*mtime_hi)); // make sure mtime_hi did not tick when read mtime_lo

    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

/*! \brief     Set mtimecmp register.
    \param[in] time_next: Time at which to trigger next ISR.
*/
__stk_forceinline void HW_SetMtimecmp(uint64_t time_next)
{
#if STK_RISCV_CLINT_MTIMECMP_PER_HART
    const uint8_t hart = HW_GetHartId();
#else
    const uint8_t hart = 0U;
#endif

#if (__riscv_xlen == 64)
    hw::WordToPtr<volatile uint64_t>(STK_RISCV_CLINT_MTIMECMP_ADDR)[hart] = time_next;
#else
    const Word mtimecmp_base = STK_RISCV_CLINT_MTIMECMP_ADDR + (hart * sizeof(uint64_t));
    volatile uint32_t *mtimecmp_lo = hw::WordToPtr<volatile uint32_t>(mtimecmp_base);
    volatile uint32_t *mtimecmp_hi = hw::WordToPtr<volatile uint32_t>(mtimecmp_base + sizeof(uint32_t));

    // expecting 4-byte aligned memory
    STK_ASSERT(((uintptr_t)mtimecmp_lo & (4U - 1U)) == 0U);
    STK_ASSERT(((uintptr_t)mtimecmp_hi & (4U - 1U)) == 0U);

    // prevent unexpected interrupt by setting some very large value to the high part
    // details: https://riscv.org/wp-content/uploads/2017/05/riscv-privileged-v1.10.pdf, page 31
    (*mtimecmp_hi) = ~0U;

    (*mtimecmp_lo) = (uint32_t)(time_next & 0xFFFFFFFFU);
    (*mtimecmp_hi) = (uint32_t)(time_next >> 32);
#endif
}

/*! \brief     Get elapsed mtime counts since a reference point.
    \param[in] since: mtime snapshot taken at an earlier point.
    \return    Elapsed mtime counts. Unsigned subtraction handles the rare
               64-bit wrap (every ~585 years at 1 GHz) correctly.
*/
__stk_forceinline uint64_t HW_GetMtimeElapsed(uint64_t since)
{
    return HW_GetMtime() - since;
}

/*! \brief     Enable cycle counter on CPU core.
*/
__stk_forceinline void HW_EnableCycleCounter()
{
    __asm__ __volatile__("csrci mcountinhibit, 0x1");
}

/*! \brief     Get value of cycle counter (for the hart of the caller).
    \return    Counter value in cycles.
*/
__stk_forceinline Cycles HW_GetCycleCounter()
{
    uint32_t high, low, check;
    do
    {
        __asm__ __volatile__("csrr %0, mcycleh" : "=r"(high));
        __asm__ __volatile__("csrr %0, mcycle"  : "=r"(low));
        __asm__ __volatile__("csrr %0, mcycleh" : "=r"(check));
    }
    while (high != check);

    return (static_cast<Cycles>(high) << 32) | low;
}

/*! \brief Get SP of the calling process.
*/
__stk_forceinline Word HW_GetCallerSP()
{
    Word sp;
    __asm__ __volatile__("mv %0, sp"
    : "=r"(sp)
    : /* input: none */
    : /* clobbers: none */);

    return sp;
}

/*! \brief Start critical section.
*/
__stk_forceinline Word HW_CriticalSectionStart()
{
    const Word ses = HW_EnterCriticalSection();

    // ensure the disable is recognized before subsequent code
    __DSB();
    __ISB();

    return ses;
}

/*! \brief End critical section.
*/
__stk_forceinline void HW_CriticalSectionEnd(Word ses)
{
    // ensure all memory work is finished before re-enabling
    __DSB();

    HW_ExitCriticalSection(ses);

    // synchronization point: any pending interrupt can be serviced immediately at this boundary
    __ISB();
}

/*! \class  ScopedHwIrqLock
    \brief  RAII wrapper over HW_CriticalSectionStart()/HW_CriticalSectionEnd(): masks local interrupts
            for the lifetime of the object and restores the previous state on scope exit (any path).
    \note   M-mode only (accesses mstatus). It is NOT hw::CriticalSection: no nesting counter, no multi-hart
            spin-lock, no U-mode ecall path. Use it for short kernel-internal sections of the driver.
    \warning Do not use where the section must not be closed by a destructor (scheduler start/stop paths
            which leave through mret, panic handler): those restore mstatus by other means.
    \note   Force-inlined: generates the same code as the explicit Start/End pair.
*/
class ScopedHwIrqLock
{
public:
    __stk_forceinline ScopedHwIrqLock() : m_ses(HW_CriticalSectionStart())
    {}

    __stk_forceinline ~ScopedHwIrqLock()
    {
        HW_CriticalSectionEnd(m_ses);
    }

    ScopedHwIrqLock(const ScopedHwIrqLock &) = delete;
    ScopedHwIrqLock &operator=(const ScopedHwIrqLock &) = delete;

private:
    const Word m_ses;
};

/*! \brief     Attempt to acquire a spin-lock without blocking (RISC-V).
    \details   Uses a GCC built-in atomic test-and-set with acquire memory ordering.
               On RISC-V A-extension targets this lowers to an \c amoswap.b.aq
               (or equivalent LR/SC loop), providing atomicity and acquire fence
               semantics in a single instruction. If the lock byte was already set
               the operation fails immediately without modifying state.
    \param[in] lock: Spin-lock state variable. Must be \c false (released) initially.
    \retval    true  Lock was free and has been acquired by the caller.
    \retval    false Lock was already held; caller must retry or back off.
    \note      ISR-safe. May be called from both thread and handler mode.
    \note      Pairs with HW_SpinLockUnlock(). Never call HW_SpinLockUnlock() without
               a preceding successful HW_SpinLockTryLock() or HW_SpinLockLock().
    \see       HW_SpinLockLock, HW_SpinLockUnlock
*/
__stk_forceinline bool HW_SpinLockTryLock(volatile bool &lock)
{
    return !__atomic_test_and_set(&lock, __ATOMIC_ACQUIRE);
}

/*! \brief     Acquire a spin-lock, blocking until it becomes available (RISC-V).
    \details   Calls HW_SpinLockTryLock() in a tight retry loop. On each failed attempt
               \c __stk_relax_cpu() is executed to reduce bus contention before the next
               try: on targets with the Zihintpause extension this emits a \c pause hint
               instruction; on others it falls back to a full memory fence.
               The retry budget is \c s_StkSpinlockTimeoutIters iterations, derived from
               \c STK_SPINLOCK_TIMEOUT_US and the real core clock by
               HW_InitSpinlockTimeout() (see there): if the lock has not been acquired by
               the time the counter expires, the kernel invariant has been violated (the
               lock owner exited without releasing) and STK_KERNEL_PANIC() is called with
               \c KERNEL_PANIC_SPINLOCK_DEADLOCK.
    \param[in] lock: Spin-lock state variable. Must be \c false (released) initially.
    \note      ISR-safe. The timeout represents a roughly constant wall-clock duration
               (STK_SPINLOCK_TIMEOUT_US) rather than a fixed iteration count, so it no
               longer needs re-tuning when optimization level, LTO, or core clock speed
               change.
    \warning   Must not be called while already holding the same \a lock - doing so will
               spin until the timeout expires and then trigger STK_KERNEL_PANIC().
    \see       HW_SpinLockTryLock, HW_SpinLockUnlock, HW_InitSpinlockTimeout
*/
__stk_forceinline void HW_SpinLockLock(volatile bool &lock)
{
    uint32_t timeout = s_StkSpinlockTimeoutIters;
    while (!HW_SpinLockTryLock(lock))
    {
        if (--timeout == 0U)
        {
            // Invariant violated: the lock owner exited without releasing,
            // Kernel state is suspect, enter defined safe state.
            STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK);
        }
        __stk_relax_cpu();
    }
}

/*! \brief     Release a spin-lock (RISC-V).
    \details   Issues a \c fence \c rw,w predecessor barrier to ensure all data writes
               made while the lock was held (including scheduler metadata) are visible
               to all harts before the lock is cleared. The lock byte is then cleared
               with a GCC atomic clear at release ordering.
               The explicit \c fence \c rw,w is retained alongside \c __ATOMIC_RELEASE
               for toolchains that do not lower \c __ATOMIC_RELEASE to a full release
               fence on all RISC-V targets (e.g. older GCC configurations), on conforming
               toolchains the two fences are equivalent and the explicit one is redundant.
    \param[in] lock: Spin-lock state variable previously acquired by the caller.
    \warning   Caller must own the lock. Releasing an unowned lock is an unrecoverable
               error and triggers STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK).
    \note      ISR-safe.
    \see       HW_SpinLockTryLock, HW_SpinLockLock
*/
__stk_forceinline void HW_SpinLockUnlock(volatile bool &lock)
{
    if (!lock)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK); // release attempt of unowned lock
    }

    // ensure all data writes (like scheduling metadata) are flushed before the lock is released:
    // __atomic_clear with __ATOMIC_RELEASE provides the required store-release barrier,
    // the explicit fence rw,w is retained for toolchains that do not lower __ATOMIC_RELEASE
    // to a full release fence on all RISC-V targets
    __asm__ __volatile__("fence rw, w" ::: "memory");

    __atomic_clear(&lock, __ATOMIC_RELEASE);
}

/*! \brief     Recompute the spin-lock timeout from the current core clock.
    \details   Converts the STK_SPINLOCK_TIMEOUT_US real-time budget into a retry-loop
               iteration count using the conservative (underestimated)
               STK_SPINLOCK_MIN_CYCLES_PER_ITER cost-per-iteration, so the resulting
               timeout means the same real-world duration regardless of optimization
               level, LTO, or core clock configuration. Called once per hart from
               Context::Initialize(); safe to call redundantly from multiple harts since
               it is idempotent for a given clock frequency.
    \note      Clamped against a zero/unconfigured clock and against overflow, so a
               misconfigured clock tree cannot produce an immediate false-positive
               KERNEL_PANIC_SPINLOCK_DEADLOCK on the very first lock attempt.
*/
void HW_InitSpinlockTimeout()
{
    const Cycles cycles_budget = ConvertTimeUsToClockCycles(HW_CoreClockFrequency(), STK_SPINLOCK_TIMEOUT_US);
    const Cycles iters = cycles_budget / STK_SPINLOCK_MIN_CYCLES_PER_ITER;

    // floor guards against a zero/not-yet-configured clock; ceiling guards against overflow
    // when narrowing to uint32_t
    s_StkSpinlockTimeoutIters = static_cast<uint32_t>(
        stk::Min(stk::Max(iters, static_cast<Cycles>(0x1000U)), static_cast<Cycles>(0xFFFFFFFFU)));
}

// ----------------------------------------------------------------------------
namespace drv {
// ----------------------------------------------------------------------------

/*! SaveJmp/RestoreJmp ---------------------------------------------------------

    RISC-V callee-saved registers per the ABI:
      s0/fp (x8), s1 (x9), s2-s11 (x18-x27), sp (x2), ra (x1)

    Both functions are naked so the compiler emits no prologue/epilogue:
      - SaveJmp captures the *caller's* SP and RA before any frame adjustment.
      - RestoreJmp reloads everything and jumps directly to the saved RA, making
        SaveJmp's caller see a non-zero return value (val) as if SaveJmp returned
        a second time.

    If FPU is present (STK_RISCV_FPU != 0), FCSR is also saved/restored to
    preserve the caller's rounding mode and exception flags across the jump.
    The callee-saved FP data registers (fs0-fs11) are NOT saved here -
    the ABI already guarantees they survive any normal call boundary.

    a0 = &f  (first argument)
    a1 = val (second argument, RestoreJmp only)
*/

/*! \struct JmpFrame
    \brief  Callee-saved CPU register snapshot used by SaveJmp() and RestoreJmp().
    \note   Layout matches the RISC-V ABI callee-saved register set: ra, sp, s0-s11.
            If an FPU is present, FCSR is appended to preserve the caller's
            floating-point rounding mode and accrued exception flags across the jump.
            FP data registers (fs0-fs11) are intentionally excluded -
            the ABI guarantees they survive any normal call boundary.
    \see    SaveJmp, RestoreJmp
*/
struct JmpFrame
{
    Word RA;   //!< Return address of the SaveJmp call site (x1, ra).
    Word SP;   //!< Stack pointer of the SaveJmp call site (x2, sp).
    Word S0;   //!< Callee-saved register s0/fp (x8).
    Word S1;   //!< Callee-saved register s1 (x9).
    Word S2;   //!< Callee-saved register s2 (x18).
    Word S3;   //!< Callee-saved register s3 (x19).
    Word S4;   //!< Callee-saved register s4 (x20).
    Word S5;   //!< Callee-saved register s5 (x21).
    Word S6;   //!< Callee-saved register s6 (x22).
    Word S7;   //!< Callee-saved register s7 (x23).
    Word S8;   //!< Callee-saved register s8 (x24).
    Word S9;   //!< Callee-saved register s9 (x25).
    Word S10;  //!< Callee-saved register s10 (x26).
    Word S11;  //!< Callee-saved register s11 (x27).
#if (STK_RISCV_FPU != 0)
    Word FCSR; //!< Floating-point control and status register (rounding mode + exception flags).
#endif
};

/*! \brief     Save callee-saved CPU registers into a JmpFrame.
    \param[in] f: Frame to save the register snapshot into.
    \return    0 when called directly; RestoreJmp() makes this function appear
               to return \a val a second time at the original call site.
    \note      Naked function - the compiler emits no prologue or epilogue,
               ensuring the snapshot reflects the *caller's* true register state.
    \note      MISRA deviation: [STK-DEV-003] Rule 7-5-1, 7-5-2, 6-6-4
               (__attribute__((naked))). Required to capture the caller's true
               SP and return address before any compiler-generated frame
               adjustment. A non-naked wrapper would snapshot the wrapper's
               own frame, producing a broken call chain on RestoreJmp().
    \note      Pair with RestoreJmp().
    \see       RestoreJmp
*/
__attribute__((naked))
int32_t SaveJmp(JmpFrame &/*f*/)
{
    __asm__ __volatile__(
    // a0 = &f - no prologue has touched sp or s0 yet
    SREG " ra, 0*" REGBYTES "(a0)  \n" // save return address
    SREG " sp, 1*" REGBYTES "(a0)  \n" // save caller's stack pointer
    SREG " s0, 2*" REGBYTES "(a0)  \n"
    SREG " s1, 3*" REGBYTES "(a0)  \n"
    SREG " s2, 4*" REGBYTES "(a0)  \n"
    SREG " s3, 5*" REGBYTES "(a0)  \n"
    SREG " s4, 6*" REGBYTES "(a0)  \n"
    SREG " s5, 7*" REGBYTES "(a0)  \n"
    SREG " s6, 8*" REGBYTES "(a0)  \n"
    SREG " s7, 9*" REGBYTES "(a0)  \n"
    SREG " s8, 10*" REGBYTES "(a0) \n"
    SREG " s9, 11*" REGBYTES "(a0) \n"
    SREG " s10, 12*" REGBYTES "(a0) \n"
    SREG " s11, 13*" REGBYTES "(a0) \n"
#if (STK_RISCV_FPU != 0)
    "frcsr t0                       \n" // read fcsr (rounding mode + flags)
    SREG " t0, 14*" REGBYTES "(a0)  \n" // save to JmpFrame::FCSR
#endif
    "li    a0, 0                    \n" // return 0
    "ret                            \n" // explicit return (naked)
    );
}

/*! \brief     Restore callee-saved CPU registers from a JmpFrame and jump back
               to the SaveJmp() call site.
    \param[in] f: Frame previously populated by SaveJmp().
    \param[in] val: Value that SaveJmp() will appear to return at the restored
               call site. Should be non-zero to distinguish a restore from
               an original save.
    \note      Naked noreturn function - execution transfers directly to the
               saved LR/RA; this function never returns to its own caller.
    \note      MISRA deviation: [STK-DEV-003] Rule 7-5-1, 7-5-2, 6-6-4
               (__attribute__((naked))). Required to restore SP and branch to
               the saved return address without any compiler-generated epilogue
               that would corrupt the restored stack state.
    \note      Pair with SaveJmp().
    \warning   Undefined behavior if \a f was not previously initialized by
               a matching SaveJmp() call on the same stack.
    \see       SaveJmp
*/
__attribute__((naked, noreturn))
void RestoreJmp(JmpFrame &/*f*/, int32_t /*val*/)
{
    __asm__ __volatile__(
    // a0 = &f, a1 = val
    LREG " ra, 0*" REGBYTES "(a0)  \n"
    LREG " sp, 1*" REGBYTES "(a0)  \n"
    LREG " s0, 2*" REGBYTES "(a0)  \n"
    LREG " s1, 3*" REGBYTES "(a0)  \n"
    LREG " s2, 4*" REGBYTES "(a0)  \n"
    LREG " s3, 5*" REGBYTES "(a0)  \n"
    LREG " s4, 6*" REGBYTES "(a0)  \n"
    LREG " s5, 7*" REGBYTES "(a0)  \n"
    LREG " s6, 8*" REGBYTES "(a0)  \n"
    LREG " s7, 9*" REGBYTES "(a0)  \n"
    LREG " s8, 10*" REGBYTES "(a0) \n"
    LREG " s9, 11*" REGBYTES "(a0) \n"
    LREG " s10, 12*" REGBYTES "(a0) \n"
    LREG " s11, 13*" REGBYTES "(a0) \n"
#if (STK_RISCV_FPU != 0)
    LREG " t0, 14*" REGBYTES "(a0)  \n" // load saved fcsr into t0
    "fscsr t0                       \n" // restore rounding mode + flags
#endif
    "mv    a0, a1                   \n" // return val to SaveJmp's caller
    "ret                            \n" // jump to saved RA
    );
}

// ----------------------------------------------------------------------------
} // namespace drv
// ----------------------------------------------------------------------------

// ############################################################################
#if STK_RISCV_USER_MODE
// ############################################################################

//! Service command ids (ecall), see STK_RISCV_SVC_MAGIC.
enum ESvcCommandId : uint8_t
{
    SVC_ENTER_CRITICAL = 1U, //!< Enter critical section from U-mode (masks interrupts via mie).
    SVC_EXIT_CRITICAL,       //!< Exit critical section from U-mode.
    SVC_BOOST_PRIV,          //!< Elevate current task to M-mode (only from the shared code region).
    SVC_DROP_PRIV            //!< Drop elevated privileges back to U-mode (only from the shared code region).
};

/*! \var     s_StkRiscvNpriv
    \brief   Per-hart flag, non-zero if the active task of the hart runs in U-mode (STK_RISCV_USER_MODE). Lives in shared BSS
             because U-mode can not read mstatus. Indexed by HW_GetHartId() (U-mode safe on multi-hart, see STK_ARCH_GET_CPU_ID).
    \note    Written by the kernel only (context load, boost/drop). Fail-safe: a wrong "privileged" reading
             makes the CSR access trap as an illegal instruction, a wrong "unprivileged" reading just takes the ecall path.
    \warning User ISRs which run on a task's stack and call CriticalSection API must not interrupt U-mode code
             in this state: such an ISR must run with its own stack (see HW_IsHandlerMode).
*/
STK_MPU_SHARED_BSS_SECTION
volatile bool s_StkRiscvNpriv[STK_ARCH_CPU_COUNT] = {};

//! Check if caller context is Privileged (M-mode).
__stk_forceinline bool HW_IsPrivilegedContext()
{
    return !s_StkRiscvNpriv[HW_GetHartId()];
}

//! Issue service ecall.
__stk_forceinline Word HW_Ecall(ESvcCommandId id, Word arg = 0U)
{
    register Word a0 __asm("a0") = arg;
    register Word a5 __asm("a5") = (STK_RISCV_SVC_MAGIC | static_cast<Word>(id));
    __asm__ __volatile__("ecall" : "+r"(a0) : "r"(a5) : "memory");
    return a0;
}

// ############################################################################
#else // !STK_RISCV_USER_MODE
// ############################################################################

__stk_forceinline bool HW_IsPrivilegedContext() { return true; }

// ############################################################################
#endif // STK_RISCV_USER_MODE
// ############################################################################

// ############################################################################
#if STK_MPU || (STK_RISCV_USER_MODE && STK_RISCV_PMP_PRESENT)
// ############################################################################

// ----------------------------------------------------------------------------
namespace hw_int {
namespace pmp {
// ----------------------------------------------------------------------------

constexpr Word   CFG_A_NAPOT   = (3U << 3U);                   // A field = NAPOT, L = 0 (unlocked, U-mode only)

/*! \def   STK_RISCV_PMP_RX_SWAPPED
    \brief Set to 1 if the core has R and X bits of pmpcfg swapped (default 0 = standard layout).
    \note  RP2350 Hazard3, erratum RP2350-E6: pmpcfg = [L, 0, 0, A[1:0], R, W, X], i.e. bit 2 = R, bit 1 = W,
           bit 0 = X, while the RISC-V Privileged ISA has bit 0 = R, bit 1 = W, bit 2 = X. Without the swap a
           "RW" region becomes "WX" (not readable, executable), and "R-X" stays "R-X" (symmetric).
*/
#ifndef STK_RISCV_PMP_RX_SWAPPED
    #define STK_RISCV_PMP_RX_SWAPPED (0)
#endif

//! Build R/W/X bits of a pmpcfg byte from STK access (bit 0 = R, bit 1 = W) and exec (bit 2 = X) flags.
constexpr Word MakeCfgPerm(Word access_rw, Word exec)
{
#if STK_RISCV_PMP_RX_SWAPPED
    return ((access_rw & 0x1U) << 2U) | (access_rw & 0x2U) | ((exec & 0x4U) >> 2U);
#else
    return (access_rw & 0x3U) | (exec & 0x4U);
#endif
}
constexpr size_t CFG_PER_WORD  = sizeof(Word);                 // mpu.cfg entries per CSR: 4 (RV32), 8 (RV64)

// CSR numbers must be immediates, with constant indices the switch folds into straight-line code.
#define STK_PMP_CSR_W(num, v) __asm__ __volatile__("csrw %0, %1" : : "i"(num), "r"(v) : "memory")
#define STK_PMP_CSR_R(num, o) __asm__ __volatile__("csrr %0, %1" : "=r"(o) : "i"(num))
#define STK_PMP_ADDR_W(n) case n: STK_PMP_CSR_W(0x3B0 + n, v); break;
#define STK_PMP_ADDR_R(n) case n: STK_PMP_CSR_R(0x3B0 + n, r); break;

__stk_forceinline void WriteAddr(size_t i, Word v)
{
    switch (i)
    {
    STK_PMP_ADDR_W(0)  STK_PMP_ADDR_W(1)  STK_PMP_ADDR_W(2)  STK_PMP_ADDR_W(3)
    STK_PMP_ADDR_W(4)  STK_PMP_ADDR_W(5)  STK_PMP_ADDR_W(6)  STK_PMP_ADDR_W(7)
    STK_PMP_ADDR_W(8)  STK_PMP_ADDR_W(9)  STK_PMP_ADDR_W(10) STK_PMP_ADDR_W(11)
    STK_PMP_ADDR_W(12) STK_PMP_ADDR_W(13) STK_PMP_ADDR_W(14) STK_PMP_ADDR_W(15)
    default: STK_ASSERT(false); break;
    }
}

__stk_forceinline Word ReadAddr(size_t i)
{
    Word r = 0U;
    switch (i)
    {
    STK_PMP_ADDR_R(0)  STK_PMP_ADDR_R(1)  STK_PMP_ADDR_R(2)  STK_PMP_ADDR_R(3)
    STK_PMP_ADDR_R(4)  STK_PMP_ADDR_R(5)  STK_PMP_ADDR_R(6)  STK_PMP_ADDR_R(7)
    STK_PMP_ADDR_R(8)  STK_PMP_ADDR_R(9)  STK_PMP_ADDR_R(10) STK_PMP_ADDR_R(11)
    STK_PMP_ADDR_R(12) STK_PMP_ADDR_R(13) STK_PMP_ADDR_R(14) STK_PMP_ADDR_R(15)
    default: STK_ASSERT(false); break;
    }
    return r;
}

// w = index of the packed word (RV32: mpu.cfg0..3, RV64: mpu.cfg0 and mpu.cfg2 only)
__stk_forceinline Word ReadCfgWord(size_t w)
{
    Word r = 0U;
    switch (w)
    {
#if (__riscv_xlen == 32)
    case 0: STK_PMP_CSR_R(0x3A0, r); break;
    case 1: STK_PMP_CSR_R(0x3A1, r); break;
    case 2: STK_PMP_CSR_R(0x3A2, r); break;
    case 3: STK_PMP_CSR_R(0x3A3, r); break;
#else
    case 0: STK_PMP_CSR_R(0x3A0, r); break;
    case 1: STK_PMP_CSR_R(0x3A2, r); break;
#endif
    default: STK_ASSERT(false); break;
    }
    return r;
}

__stk_forceinline void WriteCfgWord(size_t w, Word v)
{
    switch (w)
    {
#if (__riscv_xlen == 32)
    case 0: STK_PMP_CSR_W(0x3A0, v); break;
    case 1: STK_PMP_CSR_W(0x3A1, v); break;
    case 2: STK_PMP_CSR_W(0x3A2, v); break;
    case 3: STK_PMP_CSR_W(0x3A3, v); break;
#else
    case 0: STK_PMP_CSR_W(0x3A0, v); break;
    case 1: STK_PMP_CSR_W(0x3A2, v); break;
#endif
    default: STK_ASSERT(false); break;
    }
}

//! Set cfg byte of the entry (read-modify-write of the packed word).
__stk_forceinline void WriteCfgByte(size_t idx, Word cfg)
{
    const size_t w  = idx / CFG_PER_WORD;
    const size_t sh = (idx % CFG_PER_WORD) * 8U;
    WriteCfgWord(w, (ReadCfgWord(w) & ~(static_cast<Word>(0xFFU) << sh)) | ((cfg & 0xFFU) << sh));
}

//! "MPU off": permissive entry 0 for U-mode (RWX, whole address space), shadows all entries below.
void Disable()
{
    WriteCfgByte(0U, 0U);
    WriteAddr(0U, ~static_cast<Word>(0U));
    WriteCfgByte(0U, 0x7U | CFG_A_NAPOT);
    __DSB();
}

#if !STK_MPU
/*! \brief U-mode without memory protection (STK_RISCV_USER_MODE=1, STK_MPU=0).
    \note  Permissive PMP entry is mandatory: with PMP implemented, U-mode accesses which match no entry fail.
*/
void ConfigureUserAccess()
{
    Disable();

    // allow U-mode to read time/cycle/instret counters
    __asm__ __volatile__("csrs mcounteren, %0" : : "r"(static_cast<Word>(0x7U)));

    __DSB();
}
#endif // !STK_MPU

// ----------------------------------------------------------------------------
} // namespace pmp
} // namespace hw_int
// ----------------------------------------------------------------------------

// ############################################################################
#endif // STK_MPU || (STK_RISCV_USER_MODE && STK_RISCV_PMP_PRESENT)
// ############################################################################

// ############################################################################
#if STK_MPU
// ############################################################################

// ----------------------------------------------------------------------------
namespace hw_int {
namespace pmp {
// ----------------------------------------------------------------------------

constexpr size_t TASK_IDX = STK_RISCV_PMP_TASK_REGION_IDX;

//! Disabled/inactive region.
constexpr MpuRegionConfig s_StkDisabledMpuRegion =
{
    .addr        = 0U,
    .size        = 0U,
    .access_perm = hw::mpu::ACCESS_NONE,
    .mem_type    = hw::mpu::TYPE_STRONGLY_ORDERED,
    .share       = hw::mpu::SHARE_NON,
    .exec        = hw::mpu::EXEC_NEVER
};

struct RegionBounds
{
    Word start;
    Word end;
    bool valid;
};

__stk_forceinline RegionBounds GetRegionBounds(const MpuRegionConfig &cfg)
{
    RegionBounds result = { 0U, 0U, false };

    if ((cfg.size != 0U) && (cfg.access_perm != hw::mpu::ACCESS_NONE))
    {
        result.start = cfg.addr & ~static_cast<Word>(cfg.size - 1U);
        result.end   = result.start + cfg.size - 1U;
        result.valid = (result.start < result.end);
    }

    return result;
}

__stk_forceinline bool IsOverlapping(const RegionBounds &a, const RegionBounds &b)
{
    return ((!a.valid || !b.valid) ? false : ((a.start <= b.end) && (b.start <= a.end)));
}

// PMP applies the lowest matching entry index, a silent overlap would change the meaning, treat it as an error.
void ValidateNoOverlaps(const MpuRegionConfig dynamic_cfg_list[], size_t dynamic_cfg_count,
    const MpuRegionConfig static_cfg_list[], size_t static_cfg_count)
{
    // 1. Validate internal overlaps within dynamic list
    for (size_t i = 0U; i < dynamic_cfg_count; ++i)
    {
        const RegionBounds bounds_i = GetRegionBounds(dynamic_cfg_list[i]);
        if (!bounds_i.valid)
        {
            continue;
        }

        for (size_t j = i + 1U; j < dynamic_cfg_count; ++j)
        {
            const RegionBounds bounds_j = GetRegionBounds(dynamic_cfg_list[j]);
            if (!bounds_j.valid)
            {
                continue;
            }

            // Assert failure means two active dynamic configurations cross boundaries
            if (IsOverlapping(bounds_i, bounds_j))
            {
                STK_KERNEL_PANIC(KERNEL_PANIC_BAD_MEMORY_REGION);
            }
        }

        // 2. Validate dynamic region 'i' against static regions from overrider
        if (static_cfg_list != nullptr)
        {
            for (size_t k = 0U; k < static_cfg_count; ++k)
            {
                const RegionBounds static_bounds = GetRegionBounds(static_cfg_list[k]);
                if (!static_bounds.valid)
                {
                    continue;
                }

                // Assert failure means a dynamic region collides with a static region
                if (IsOverlapping(bounds_i, static_bounds))
                {
                    STK_KERNEL_PANIC(KERNEL_PANIC_BAD_MEMORY_REGION);
                }
            }
        }
    }

    // validate static list against itself (dynamic list is empty)
    if ((dynamic_cfg_count == 0U) && (static_cfg_list != nullptr))
    {
        for (size_t i = 0U; i < static_cfg_count; ++i)
        {
            const RegionBounds static_bounds_i = GetRegionBounds(static_cfg_list[i]);
            if (!static_bounds_i.valid)
            {
                continue;
            }

            for (size_t j = i + 1U; j < static_cfg_count; ++j)
            {
                const RegionBounds static_bounds_j = GetRegionBounds(static_cfg_list[j]);
                if (!static_bounds_j.valid)
                {
                    continue;
                }

                if (IsOverlapping(static_bounds_i, static_bounds_j))
                {
                    STK_KERNEL_PANIC(KERNEL_PANIC_BAD_MEMORY_REGION);
                }
            }
        }
    }
}

/*! \brief      Compute mpu.addr/mpu.cfg values for a region without touching hardware.
    \param[out] reg: reg.addr = mpu.addr value, reg.attr = mpu.cfg byte.
    \note       NAPOT only: size must be a power of two >= 8 and addr aligned to size (STK_ASSERT).
*/
void ConfigureRegion(MpuRegion &reg, const MpuRegionConfig &cfg)
{
    if ((cfg.size != 0U) && (cfg.access_perm != hw::mpu::ACCESS_NONE))
    {
        STK_ASSERT(cfg.size >= 8U);
        STK_ASSERT((cfg.size & (cfg.size - 1U)) == 0U);
        STK_ASSERT((cfg.addr & (cfg.size - 1U)) == 0U);

        // NAPOT: mpu.addr = (base | (size / 2 - 1)) >> 2
        reg.addr = (cfg.addr | static_cast<Word>((cfg.size >> 1U) - 1U)) >> 2U;
        reg.attr = MakeCfgPerm(static_cast<Word>(cfg.access_perm), static_cast<Word>(cfg.exec)) |
                   CFG_A_NAPOT;
    }
    else
    {
        reg.addr = 0U;
        reg.attr = 0U; // A = OFF
    }
}

void ConfigureStatic(const MpuRegionConfig cfg_list[], size_t cfg_count, uint32_t flags)
{
    STK_ASSERT((cfg_list != nullptr) || (cfg_count == 0U));
    STK_ASSERT(cfg_count <= TASK_IDX);
    STK_ASSERT((flags & hw::mpu::MPU_CFG_IN_FAULTS) == 0U);

    ValidateNoOverlaps(nullptr, 0U, cfg_list, cfg_count);

    for (size_t i = 0U; i < TASK_IDX; ++i)
    {
        MpuRegion reg;
        ConfigureRegion(reg, (i < cfg_count ? cfg_list[i] : s_StkDisabledMpuRegion));

        WriteCfgByte(i, 0U); // disable first: mpu.addr change must not create a transient match
        WriteAddr(i, reg.addr);
        WriteCfgByte(i, reg.attr);
    }

    // allow U-mode to read time/cycle/instret counters
    __asm__ __volatile__("csrs mcounteren, %0" : : "r"(static_cast<Word>(0x7U)));

    __DSB();
}

#if STK_MPU_STACK_GUARD
void ConfigureDynamic(TaskMpu &task_mpu, const MpuRegionConfig cfg_list[], size_t cfg_count,
    IPlatform::IEventOverrider *overrider)
{
    STK_ASSERT((cfg_list != nullptr) || (cfg_count == 0U));
    STK_ASSERT(cfg_count <= TaskMpu::NUM_REGIONS);

    const size_t clamped_count = (cfg_count < TaskMpu::NUM_REGIONS ? cfg_count : TaskMpu::NUM_REGIONS);

    MpuRegionList static_regions(nullptr, 0);
    if (overrider != nullptr)
    {
        const MpuConfig *const cfg = overrider->OnConfigureMpu();
        if (cfg != nullptr)
        {
            static_regions = cfg->regions;
        }
    }

    // check dynamic region, static region is checked by ConfigureStatic
    if (clamped_count != 0)
    {
        ValidateNoOverlaps(cfg_list, clamped_count, static_regions.GetPtr(), static_regions.GetSize());
    }

    size_t idx = 0U;
    for (; idx < clamped_count; ++idx)
    {
        ConfigureRegion(task_mpu.region[idx], cfg_list[idx]);
    }
    for (; idx < TaskMpu::NUM_REGIONS; ++idx)
    {
        ConfigureRegion(task_mpu.region[idx], s_StkDisabledMpuRegion);
    }
}
#endif // STK_MPU_STACK_GUARD

/*! \brief     Make per-task PMP regions of the incoming task effective.
    \note      Called with interrupts disabled, between context save and load (see hw_int::LoadTask).
*/
__stk_forceinline void LoadRegions(const Stack *st)
{
#if !STK_MPU_STACK_GUARD
    STK_UNUSED(st);
#else
    constexpr size_t N = TaskMpu::NUM_REGIONS;
    constexpr size_t B = TASK_IDX;

    for (size_t i = 0U; i < N; ++i)
    {
        WriteAddr(B + i, st->mpu.region[i].addr);
    }

    for (size_t w = (B / CFG_PER_WORD); w <= ((B + N - 1U) / CFG_PER_WORD); ++w)
    {
        Word v = ReadCfgWord(w);

        const size_t first = ((B > (w * CFG_PER_WORD)) ? B : (w * CFG_PER_WORD));
        const size_t last  = (((B + N) < ((w + 1U) * CFG_PER_WORD)) ? (B + N) : ((w + 1U) * CFG_PER_WORD));

        for (size_t e = first; e < last; ++e)
        {
            const size_t sh = (e % CFG_PER_WORD) * 8U;
            v = (v & ~(static_cast<Word>(0xFFU) << sh)) | ((st->mpu.region[e - B].attr & 0xFFU) << sh);
        }

        WriteCfgWord(w, v);
    }
#endif
}

// ----------------------------------------------------------------------------
} // namespace pmp
} // namespace hw_int
// ----------------------------------------------------------------------------

// ############################################################################
#endif // STK_MPU
// ############################################################################

// ############################################################################
#if STK_RISCV_USER_MODE
// ############################################################################

// ----------------------------------------------------------------------------
namespace hw_int {
// ----------------------------------------------------------------------------

/*! \brief     Publish privilege state of the incoming task and make its PMP regions effective (if STK_MPU).
    \note      Called with interrupts disabled, between context save and load.
*/
__stk_forceinline void LoadActiveTask(const Stack *st)
{
    const TaskFrame *const tf = hw::WordToPtr<TaskFrame>(st->SP);

    // update privilege level of the active task
    s_StkRiscvNpriv[HW_GetHartId()] = !tf->IsPrivileged();

#if STK_MPU
    pmp::LoadRegions(st);
#endif
}

// ----------------------------------------------------------------------------
} // namespace hw_int
// ----------------------------------------------------------------------------

// ############################################################################
#endif // STK_RISCV_USER_MODE
// ############################################################################

//! Internal context.
struct Context final : public PlatformContext
{
    constexpr Context() : PlatformContext(), m_stack_main(), m_stack_isr(), m_stack_isr_mem(),
        m_exit_buf(), m_overrider(nullptr), m_specific(nullptr), m_tick_period(0), m_last_mtime(0ULL),
    #if STK_TICKLESS_IDLE
        m_sleep_ticks(0),
    #endif
        m_csu(0), m_csu_nesting(0), m_csu_npriv(false),
        m_starting(false), m_started(false), m_exiting(false)

    {}

    void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap,
        uint32_t resolution_us)
    {
        InitializeBase(handler, service, exit_trap, resolution_us);

        STK_STATIC_ASSERT_DESC_N(SP, offsetof(Stack, SP) == 0U,
            "expect Stack::mode member at offset of 0 (first member)");
    #if STK_MPU
        STK_STATIC_ASSERT_DESC_N(pmp_idx, (STK_RISCV_PMP_TASK_REGION_IDX + STK_MPU_TASK_REGIONS) <= STK_RISCV_PMP_REGIONS_MAX,
            "PMP task region block exceeds the number of implemented PMP entries");
    #endif

        // init ISR's stack
        {
            StackMemoryWrapper<STK_RISCV_ISR_STACK_SIZE> stack_isr_mem(&m_stack_isr_mem);
            m_stack_isr.SP          = InitStackMemory(&stack_isr_mem);
            m_stack_isr.access_mode = ACCESS_PRIVILEGED;
        }

        // init Main stack
        {
            m_stack_main.SP          = STK_STACK_MEMORY_FILLER;
            m_stack_main.access_mode = ACCESS_PRIVILEGED;
        }

        m_csu         = 0U;
        m_csu_nesting = 0U;
        m_tick_period = ConvertTimeUsToClockCycles(STK_TIMER_CLOCK_FREQUENCY, resolution_us);
        m_last_mtime  = 0ULL;
        m_starting    = false;
        m_started     = false;
        m_exiting     = false;

        // initialize system clock if user-side code has not yet done it
        if (s_StkSystemCoreClock[0] == 0U)
        {
            for (uint8_t i = 0U; i < STK_STATIC_ARRAY_SIZE(s_StkSystemCoreClock); ++i)
            {
                s_StkSystemCoreClock[i] = STK_SYSTEM_CORE_CLOCK_FREQUENCY;
            }
        }

        // Re-derive the spin-lock timeout from the now-configured core clock. It is used by
        // hw::SpinLock (always) and by the global critical-section lock s_StkRiscvCsuLock
        // (multi-core builds only; on a single hart that lock is compiled out). Redundant
        // across harts (each hart's Context::Initialize() calls this), but idempotent and
        // cheap - and must run before any spinlock is used for real, since
        // s_StkSpinlockTimeoutIters otherwise still holds its conservative pre-init fallback
        // value.
        HW_InitSpinlockTimeout();

        // mcycle counter must be enabled per-core
    #if STK_SUBMICORSECOND_PRECISION_TIMER
        HW_EnableCycleCounter();
    #endif
    }

    __stk_forceinline void ProcessTick()
    {
        // process tick - scheduler may update m_stack_active to point at a new task
        const ScopedHwIrqLock cs_;

    #if STK_TICKLESS_IDLE
        Timeout ticks = m_sleep_ticks;
    #endif

        if (m_handler->OnTick(m_stack_idle, m_stack_active
        #if STK_TICKLESS_IDLE
            , ticks
        #endif
        ))
        {
            // refresh ISR asm pointer cache so the naked ISR reads the correct
            // (possibly new) active stack SP immediately when jal returns
            // s_StkRiscvStackActive[hart] always points to Context::m_stack_active, the pointer
            // itself is stable, but we reassign here so multi-core hart-indexed builds
            // stay correct if the hart mapping ever changes in future,
            // for single-core builds this is a simple store to a known address at index 0
            const uint8_t hart = HW_GetHartId();
            s_StkRiscvStackActive[hart] = m_stack_active;
        #ifdef _STK_RISCV_USE_PENDSV
            s_StkRiscvStackIdle[hart] = m_stack_idle;
        #endif

        #if STK_SEGGER_SYSVIEW
            if (m_stack_idle->tid != SYS_TASK_ID_SLEEP)
            {
                // previous task went idle
                SEGGER_SYSVIEW_OnTaskStopReady(m_stack_idle->tid, TRACE_EVENT_SWITCH);
            }

            if (m_stack_active->tid != SYS_TASK_ID_SLEEP)
            {
                // current task resumed execution
                SEGGER_SYSVIEW_OnTaskStartExec(m_stack_active->tid);
            }
        #endif

        #ifdef _STK_RISCV_USE_PENDSV
            HW_ScheduleContextSwitch(hart);
        #endif
        }

    #if STK_TICKLESS_IDLE
        m_sleep_ticks = ticks;
    #endif
    }

    __stk_forceinline void EnterCriticalSection()
    {
        // disable local interrupts and save state
        const Word current_ses = HW_CriticalSectionStart();

        if (m_csu_nesting == 0U)
        {
        #if (STK_ARCH_CPU_COUNT > 1U)
            // ONLY attempt the global spinlock if we aren't already nested;
            // it provides inter-core exclusion only and is redundant on a single core
            HW_SpinLockLock(s_StkRiscvCsuLock);
        #endif

            // store the hardware interrupt state to restore later
            m_csu = current_ses;
        }

        // increase nesting count within a limit
        if (++m_csu_nesting > STK_CS_NESTINGS_MAX)
        {
            // invariant violated: exceeded max allowed number of recursions
            STK_KERNEL_PANIC(KERNEL_PANIC_CS_NESTING_OVERFLOW);
        }
    }

    __stk_forceinline void ExitCriticalSection()
    {
        if (m_csu_nesting == 0U)
        {
            // invariant violated: unbalanced Exit, uint8_t would wrap to 255 and the lock would never release
            STK_KERNEL_PANIC(KERNEL_PANIC_CS_NESTING_OVERFLOW);
        }
        --m_csu_nesting;

        if (m_csu_nesting == 0U)
        {
            // capture the state before releasing lock
            const Word ses_to_restore = m_csu;

        #if (STK_ARCH_CPU_COUNT > 1U)
            // release global lock
            HW_SpinLockUnlock(s_StkRiscvCsuLock);
        #endif

            // restore hardware interrupts
        #if STK_RISCV_USER_MODE
            if (m_csu_npriv)
            {
                // entered from U-mode: m_csu holds saved mie, interrupts resume on mret of the ecall trap
                m_csu_npriv = false;
                write_csr(mie, ses_to_restore);
                __DSB();
            }
            else
        #endif
            {
                HW_CriticalSectionEnd(ses_to_restore);
            }
        }
    }

#if STK_RISCV_USER_MODE
    /*! \brief  Enter critical section on behalf of a U-mode task (runs inside the ecall trap).
        \note   Interrupts are masked through mie CSR (mstatus.MIE does not mask M-mode interrupts in U-mode).
    */
    __stk_forceinline void EnterCriticalSectionNpriv()
    {
        const Word saved_mie = read_csr(mie);
        write_csr(mie, 0U);

        if (m_csu_nesting == 0U)
        {
        #if (STK_ARCH_CPU_COUNT > 1U)
            // mirror of EnterCriticalSection(): ExitCriticalSection() releases the global lock unconditionally
            HW_SpinLockLock(s_StkRiscvCsuLock);
        #endif

            m_csu       = saved_mie;
            m_csu_npriv = true;
        }

        if (++m_csu_nesting > STK_CS_NESTINGS_MAX)
        {
            STK_KERNEL_PANIC(KERNEL_PANIC_CS_NESTING_OVERFLOW);
        }
    }

#endif // STK_RISCV_USER_MODE

#if STK_MPU
    /*! \brief  Configure global (static) PMP regions, mirror of ARM ConfigureMpu().
    */
    void ConfigureMpu()
    {
        const MpuConfig *const cfg = (m_overrider != nullptr ? m_overrider->OnConfigureMpu() : nullptr);

        if (cfg != nullptr)
        {
            if ((cfg->mode & hw::mpu::MPU_CFG_CLEAR_ON_INIT) != 0U)
            {
                for (size_t i = 0U; i < STK_RISCV_PMP_REGIONS_MAX; ++i)
                {
                    hw_int::pmp::WriteCfgByte(i, 0U);
                    hw_int::pmp::WriteAddr(i, 0U);
                }
            }

            hw_int::pmp::ConfigureStatic(cfg->regions.GetPtr(), cfg->regions.GetSize(), cfg->mode);
        }
        else
        {
            hw_int::pmp::Disable();
        }
    }
#endif // STK_MPU

    uint64_t GetSleepTicksPrev()
    {
    #if STK_TICKLESS_IDLE
        const uint64_t ticks = (static_cast<uint64_t>(m_sleep_ticks) * static_cast<uint64_t>(m_tick_period));
    #else
        const uint64_t ticks = (1U * static_cast<uint64_t>(m_tick_period));
    #endif
        return ticks;
    }

    uint64_t GetTimeNow(uint64_t &error)
    {
        const uint64_t mtime_now = HW_GetMtime();
        error = (mtime_now - m_last_mtime) - GetSleepTicksPrev();
        return mtime_now;
    }

    void RearmTimer(const uint64_t mtime_now, const uint64_t error)
    {
    #if STK_TICKLESS_IDLE
        // guard against overflow (theoretical at normal tick periods and CPU frequencies)
        STK_ASSERT((static_cast<uint64_t>(m_sleep_ticks) * static_cast<uint64_t>(m_tick_period)) <= (UINT64_MAX - mtime_now));
        const uint64_t next_time = (static_cast<uint64_t>(m_sleep_ticks) * static_cast<uint64_t>(m_tick_period));
    #else
        const uint64_t next_time = (1U * static_cast<uint64_t>(m_tick_period));
    #endif
        HW_SetMtimecmp(mtime_now + next_time - error);
        m_last_mtime = mtime_now;
    }

    __stk_forceinline void OnSwitchContext()
    {
        // capture mtime at ISR entry as the absolute base for the next period;
        // this eliminates drift from time spent inside OnTick regardless of how
        // long the scheduler takes to run
        uint64_t error = 0U;
        const uint64_t mtime_now = GetTimeNow(error);
        __stk_compiler_barrier(); // avoid compiler reordering, we count ticks from this point

        // make sure timer is enabled by the Kernel::Start(), disable its start anywhere else
        STK_ASSERT(m_started);
        STK_ASSERT(m_handler != nullptr);

        // process tick - scheduler may update m_stack_active and m_sleep_ticks
        ProcessTick();

        // rearm timer: use the ISR-entry mtime snapshot as the absolute base so
        // any CPU cycles consumed by OnTick do not accumulate as period drift
        RearmTimer(mtime_now, error);
    }

    __stk_forceinline void OnForceContextSwitch(TId id)
    {
        if (m_handler->OnForceContextSwitch(id, m_stack_idle, m_stack_active))
        {
            // refresh ISR asm pointer cache so STK_MSI_HANDLER reads the correct
            // (possibly new) active/idle stack SP
            //
            // note: unlike ProcessTick(), the idle slot is refreshed unconditionally here,
            // not just under _STK_RISCV_USE_PENDSV; a forced switch can be requested from
            // arbitrary task/ISR context (not necessarily from inside STK_SYSTICK_HANDLER),
            // so it can never be applied inline - it always goes through STK_MSI_HANDLER
            const uint8_t hart = HW_GetHartId();
            s_StkRiscvStackActive[hart] = m_stack_active;
            s_StkRiscvStackIdle[hart]   = m_stack_idle;

            // always pend the MSI, regardless of _STK_RISCV_USE_PENDSV
            HW_ScheduleContextSwitch(hart);
        }
    }

    void StartTickTimer(Timeout elapsed_ticks)
    {
    #if STK_TICKLESS_IDLE
        // reset sleep ticks if kernel was restarted
        m_sleep_ticks = elapsed_ticks;
    #else
        STK_UNUSED(elapsed_ticks);
    #endif

        // start timer with default periodicity
        m_last_mtime = HW_GetMtime();
        HW_SetMtimecmp(m_last_mtime + m_tick_period);

        // enable timer interrupt
        set_csr(mie, MIP_MTIP);
    }

    void OnSleepOverride()
    {
    #if STK_TICKLESS_IDLE
        const Timeout sleep_ticks = m_sleep_ticks;
    #else
        const Timeout sleep_ticks = 1;
    #endif

        if (!m_overrider->OnSleep(sleep_ticks))
        {
            HW_EnterSleepMode();
        }
    }

#if STK_TLS && !STK_INLINE_TLS
    Word GetTls()
    {
        hw::CriticalSection::ScopedLock cs_;

        STK_ASSERT(m_stack_active != nullptr);

        return m_stack_active->tls;
    }

    void SetTls(Word tp)
    {
        hw::CriticalSection::ScopedLock cs_;

        STK_ASSERT(m_stack_active != nullptr);

        m_stack_active->tls = tp;
    }
#endif // STK_TLS && !STK_INLINE_TLS

    void Start();
    void OnStart();
    void OnStop();
#if STK_TICKLESS_IDLE
    Timeout Suspend();
    void Resume(Timeout elapsed_ticks);
#endif

    typedef IPlatform::IEventOverrider                               eovrd_t;
    typedef PlatformRiscV::ISpecificEventHandler                     sehndl_t;
    typedef StackMemoryWrapper<STK_RISCV_ISR_STACK_SIZE>::MemoryType isrmem_t;

    Stack         m_stack_main;    //!< main stack info
    Stack         m_stack_isr;     //!< ISR stack info
    isrmem_t      m_stack_isr_mem; //!< ISR stack memory
    drv::JmpFrame m_exit_buf;      //!< saved context of the exit point
    eovrd_t      *m_overrider;     //!< platform events overrider
    sehndl_t     *m_specific;      //!< platform-specific event handler
    uint32_t      m_tick_period;   //!< system tick periodicity (microseconds, ticks)
    uint64_t      m_last_mtime;    //!< mtime captured at previous OnTick entry
#if STK_TICKLESS_IDLE
    Timeout       m_sleep_ticks;   //!< tick count programmed for the current/last period
#endif
    Word          m_csu;           //!< user critical session
    uint8_t       m_csu_nesting;   //!< depth of user critical session nesting
    bool          m_csu_npriv;     //!< 'true' if the outermost critical section was entered from U-mode (mie masking)
    bool          m_starting;      //!< 'true' when in is being started
    bool          m_started;       //!< 'true' when in started state
    volatile bool m_exiting;       //!< 'true' when is exiting the scheduling process
};

/*! \var s_StkPlatformContext
    \brief Shared static context instance (one per hart).
*/
STK_MPU_KERNEL_DATA_SECTION
Context s_StkPlatformContext[STK_ARCH_CPU_COUNT];

// ****************************************************************************
} // namespace {
// ****************************************************************************

__stk_attr_noinline // keep out of inlining to preserve stack frame
__stk_attr_noreturn // never returns - a trap
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    g_StkLastPanicId = id;

    // disable all maskable interrupts: this prevents scheduler from running again and corrupting state further
    HW_DisableInterrupts();

    // spin forever: with a watchdog active this produces a clean reset, without a watchdog,
    // a debugger can attach and inspect 'id'
    for (;;)
    {
        __stk_relax_cpu();
    }
}

void PlatformRiscV::ProcessTick()
{
#ifdef _STK_RISCV_USE_PENDSV
    // Context::ProcessTick() masks interrupts itself
    GetContext().ProcessTick();
#else
    // unsupported scenario
    STK_ASSERT(false);
#endif
}

#define STK_ASM_SAVE_CONTEXT_BASE\
    SREG " x1, 2*" REGBYTES "(sp)    \n"\
    /*SREG " x2, 3*" REGBYTES "(sp)  \n" // skip saving sp, Stack pointer */\
    /*SREG " x3, 4*" REGBYTES "(sp)  \n" // skip saving gp, Global pointer (note: slot is used by fscsr) */\
    SREG " x4, 5*" REGBYTES "(sp)    \n"\
    SREG " x5, 6*" REGBYTES "(sp)    \n"\
    SREG " x6, 7*" REGBYTES "(sp)    \n"\
    SREG " x7, 8*" REGBYTES "(sp)    \n"\
    SREG " x8, 9*" REGBYTES "(sp)    \n"\
    SREG " x9, 10*" REGBYTES "(sp)   \n"\
    SREG " x10, 11*" REGBYTES "(sp)  \n"\
    SREG " x11, 12*" REGBYTES "(sp)  \n"\
    SREG " x12, 13*" REGBYTES "(sp)  \n"\
    SREG " x13, 14*" REGBYTES "(sp)  \n"\
    SREG " x14, 15*" REGBYTES "(sp)  \n"\
    SREG " x15, 16*" REGBYTES "(sp)  \n"

#if (__riscv_32e != 1)
#define STK_ASM_SAVE_CONTEXT_RV32I_EXT\
    SREG " x16, 17*" REGBYTES "(sp)  \n"\
    SREG " x17, 18*" REGBYTES "(sp)  \n"\
    SREG " x18, 19*" REGBYTES "(sp)  \n"\
    SREG " x19, 20*" REGBYTES "(sp)  \n"\
    SREG " x20, 21*" REGBYTES "(sp)  \n"\
    SREG " x21, 22*" REGBYTES "(sp)  \n"\
    SREG " x22, 23*" REGBYTES "(sp)  \n"\
    SREG " x23, 24*" REGBYTES "(sp)  \n"\
    SREG " x24, 25*" REGBYTES "(sp)  \n"\
    SREG " x25, 26*" REGBYTES "(sp)  \n"\
    SREG " x26, 27*" REGBYTES "(sp)  \n"\
    SREG " x27, 28*" REGBYTES "(sp)  \n"\
    SREG " x28, 29*" REGBYTES "(sp)  \n"\
    SREG " x29, 30*" REGBYTES "(sp)  \n"\
    SREG " x30, 31*" REGBYTES "(sp)  \n"\
    SREG " x31, 32*" REGBYTES "(sp)  \n"
#else
#define STK_ASM_SAVE_CONTEXT_RV32I_EXT
#endif

#if (STK_RISCV_FPU != 0)
#define STK_ASM_SAVE_CONTEXT_FP\
    FSREG " f0, " FOFFSET "+0*" FREGBYTES "(sp)  \n"\
    FSREG " f1, " FOFFSET "+1*" FREGBYTES "(sp)  \n"\
    FSREG " f2, " FOFFSET "+2*" FREGBYTES "(sp)  \n"\
    FSREG " f3, " FOFFSET "+3*" FREGBYTES "(sp)  \n"\
    FSREG " f4, " FOFFSET "+4*" FREGBYTES "(sp)  \n"\
    FSREG " f5, " FOFFSET "+5*" FREGBYTES "(sp)  \n"\
    FSREG " f6, " FOFFSET "+6*" FREGBYTES "(sp)  \n"\
    FSREG " f7, " FOFFSET "+7*" FREGBYTES "(sp)  \n"\
    FSREG " f8, " FOFFSET "+8*" FREGBYTES "(sp)  \n"\
    FSREG " f9, " FOFFSET "+9*" FREGBYTES "(sp)  \n"\
    FSREG " f10, " FOFFSET "+10*" FREGBYTES "(sp)  \n"\
    FSREG " f11, " FOFFSET "+11*" FREGBYTES "(sp)  \n"\
    FSREG " f12, " FOFFSET "+12*" FREGBYTES "(sp)  \n"\
    FSREG " f13, " FOFFSET "+13*" FREGBYTES "(sp)  \n"\
    FSREG " f14, " FOFFSET "+14*" FREGBYTES "(sp)  \n"\
    FSREG " f15, " FOFFSET "+15*" FREGBYTES "(sp)  \n"\
    FSREG " f16, " FOFFSET "+16*" FREGBYTES "(sp)  \n"\
    FSREG " f17, " FOFFSET "+17*" FREGBYTES "(sp)  \n"\
    FSREG " f18, " FOFFSET "+18*" FREGBYTES "(sp)  \n"\
    FSREG " f19, " FOFFSET "+19*" FREGBYTES "(sp)  \n"\
    FSREG " f20, " FOFFSET "+20*" FREGBYTES "(sp)  \n"\
    FSREG " f21, " FOFFSET "+21*" FREGBYTES "(sp)  \n"\
    FSREG " f22, " FOFFSET "+22*" FREGBYTES "(sp)  \n"\
    FSREG " f23, " FOFFSET "+23*" FREGBYTES "(sp)  \n"\
    FSREG " f24, " FOFFSET "+24*" FREGBYTES "(sp)  \n"\
    FSREG " f25, " FOFFSET "+25*" FREGBYTES "(sp)  \n"\
    FSREG " f26, " FOFFSET "+26*" FREGBYTES "(sp)  \n"\
    FSREG " f27, " FOFFSET "+27*" FREGBYTES "(sp)  \n"\
    FSREG " f28, " FOFFSET "+28*" FREGBYTES "(sp)  \n"\
    FSREG " f29, " FOFFSET "+29*" FREGBYTES "(sp)  \n"\
    FSREG " f30, " FOFFSET "+30*" FREGBYTES "(sp)  \n"\
    FSREG " f31, " FOFFSET "+31*" FREGBYTES "(sp)  \n"
#else
#define STK_ASM_SAVE_CONTEXT_FP
#endif

#define STK_ASM_SAVE_CONTEXT_PC_STATUS\
    "csrr t0, mepc                   \n"\
    "csrr t1, mstatus                \n"\
    SREG " t0, 0*" REGBYTES "(sp)    \n"\
    SREG " t1, 1*" REGBYTES "(sp)    \n"

#if (STK_RISCV_FPU != 0)
#define STK_ASM_SAVE_CONTEXT_FRCSR\
    "frcsr t0                        \n"\
    SREG " t0, 4*" REGBYTES "(sp)    \n"  /* use stack memory slot of gp  (see comment for x3 above) */
#else
#define STK_ASM_SAVE_CONTEXT_FRCSR
#endif

#define STK_ASM_SAVE_CONTEXT\
    "addi sp, sp, -" REGSIZE       " \n" /* allocate stack memory for registers */\
    STK_ASM_SAVE_CONTEXT_BASE\
    STK_ASM_SAVE_CONTEXT_RV32I_EXT\
    STK_ASM_SAVE_CONTEXT_FP\
    STK_ASM_SAVE_CONTEXT_PC_STATUS\
    STK_ASM_SAVE_CONTEXT_FRCSR

#define STK_ASM_LOAD_CONTEXT_BASE\
    LREG " x1, 2*" REGBYTES "(sp)    \n"\
    /*LREG " x2, 3*" REGBYTES "(sp)  \n" skip loading sp, Stack pointer */\
    /*LREG " x3, 4*" REGBYTES "(sp)  \n" skip loading gp, Global pointer (note: slot is used by fscsr) */\
    LREG " x4, 5*" REGBYTES "(sp)    \n"\
    LREG " x5, 6*" REGBYTES "(sp)    \n"\
    LREG " x6, 7*" REGBYTES "(sp)    \n"\
    LREG " x7, 8*" REGBYTES "(sp)    \n"\
    LREG " x8, 9*" REGBYTES "(sp)    \n"\
    LREG " x9, 10*" REGBYTES "(sp)   \n"\
    LREG " x10, 11*" REGBYTES "(sp)  \n"\
    LREG " x11, 12*" REGBYTES "(sp)  \n"\
    LREG " x12, 13*" REGBYTES "(sp)  \n"\
    LREG " x13, 14*" REGBYTES "(sp)  \n"\
    LREG " x14, 15*" REGBYTES "(sp)  \n"\
    LREG " x15, 16*" REGBYTES "(sp)  \n"

#if (__riscv_32e != 1)
#define STK_ASM_LOAD_CONTEXT_RV32I_EXT\
    LREG " x16, 17*" REGBYTES "(sp)  \n"\
    LREG " x17, 18*" REGBYTES "(sp)  \n"\
    LREG " x18, 19*" REGBYTES "(sp)  \n"\
    LREG " x19, 20*" REGBYTES "(sp)  \n"\
    LREG " x20, 21*" REGBYTES "(sp)  \n"\
    LREG " x21, 22*" REGBYTES "(sp)  \n"\
    LREG " x22, 23*" REGBYTES "(sp)  \n"\
    LREG " x23, 24*" REGBYTES "(sp)  \n"\
    LREG " x24, 25*" REGBYTES "(sp)  \n"\
    LREG " x25, 26*" REGBYTES "(sp)  \n"\
    LREG " x26, 27*" REGBYTES "(sp)  \n"\
    LREG " x27, 28*" REGBYTES "(sp)  \n"\
    LREG " x28, 29*" REGBYTES "(sp)  \n"\
    LREG " x29, 30*" REGBYTES "(sp)  \n"\
    LREG " x30, 31*" REGBYTES "(sp)  \n"\
    LREG " x31, 32*" REGBYTES "(sp)  \n"
#else
#define STK_ASM_LOAD_CONTEXT_RV32I_EXT
#endif

#if (STK_RISCV_FPU != 0)
#define STK_ASM_LOAD_CONTEXT_FP\
    FLREG " f0, " FOFFSET "+0*" FREGBYTES "(sp)  \n"\
    FLREG " f1, " FOFFSET "+1*" FREGBYTES "(sp)  \n"\
    FLREG " f2, " FOFFSET "+2*" FREGBYTES "(sp)  \n"\
    FLREG " f3, " FOFFSET "+3*" FREGBYTES "(sp)  \n"\
    FLREG " f4, " FOFFSET "+4*" FREGBYTES "(sp)  \n"\
    FLREG " f5, " FOFFSET "+5*" FREGBYTES "(sp)  \n"\
    FLREG " f6, " FOFFSET "+6*" FREGBYTES "(sp)  \n"\
    FLREG " f7, " FOFFSET "+7*" FREGBYTES "(sp)  \n"\
    FLREG " f8, " FOFFSET "+8*" FREGBYTES "(sp)  \n"\
    FLREG " f9, " FOFFSET "+9*" FREGBYTES "(sp)  \n"\
    FLREG " f10, " FOFFSET "+10*" FREGBYTES "(sp)  \n"\
    FLREG " f11, " FOFFSET "+11*" FREGBYTES "(sp)  \n"\
    FLREG " f12, " FOFFSET "+12*" FREGBYTES "(sp)  \n"\
    FLREG " f13, " FOFFSET "+13*" FREGBYTES "(sp)  \n"\
    FLREG " f14, " FOFFSET "+14*" FREGBYTES "(sp)  \n"\
    FLREG " f15, " FOFFSET "+15*" FREGBYTES "(sp)  \n"\
    FLREG " f16, " FOFFSET "+16*" FREGBYTES "(sp)  \n"\
    FLREG " f17, " FOFFSET "+17*" FREGBYTES "(sp)  \n"\
    FLREG " f18, " FOFFSET "+18*" FREGBYTES "(sp)  \n"\
    FLREG " f19, " FOFFSET "+19*" FREGBYTES "(sp)  \n"\
    FLREG " f20, " FOFFSET "+20*" FREGBYTES "(sp)  \n"\
    FLREG " f21, " FOFFSET "+21*" FREGBYTES "(sp)  \n"\
    FLREG " f22, " FOFFSET "+22*" FREGBYTES "(sp)  \n"\
    FLREG " f23, " FOFFSET "+23*" FREGBYTES "(sp)  \n"\
    FLREG " f24, " FOFFSET "+24*" FREGBYTES "(sp)  \n"\
    FLREG " f25, " FOFFSET "+25*" FREGBYTES "(sp)  \n"\
    FLREG " f26, " FOFFSET "+26*" FREGBYTES "(sp)  \n"\
    FLREG " f27, " FOFFSET "+27*" FREGBYTES "(sp)  \n"\
    FLREG " f28, " FOFFSET "+28*" FREGBYTES "(sp)  \n"\
    FLREG " f29, " FOFFSET "+29*" FREGBYTES "(sp)  \n"\
    FLREG " f30, " FOFFSET "+30*" FREGBYTES "(sp)  \n"\
    FLREG " f31, " FOFFSET "+31*" FREGBYTES "(sp)  \n"
#else
#define STK_ASM_LOAD_CONTEXT_FP
#endif

#define STK_ASM_LOAD_CONTEXT_PC_STATUS\
    LREG " t0, 0*" REGBYTES "(sp)    \n"\
    LREG " t1, 1*" REGBYTES "(sp)    \n"\
    "csrw mepc, t0                   \n"\
    "csrw mstatus, t1                \n"

#if (STK_RISCV_FPU != 0)
#define STK_ASM_LOAD_CONTEXT_FRCSR\
    LREG " t0, 4*" REGBYTES "(sp)    \n" /* use stack memory slot of gp (see comment for x3 below) */\
    "fscsr t0                        \n"
#else
#define STK_ASM_LOAD_CONTEXT_FRCSR
#endif

#define STK_ASM_LOAD_CONTEXT\
    STK_ASM_LOAD_CONTEXT_PC_STATUS\
    STK_ASM_LOAD_CONTEXT_FRCSR\
    STK_ASM_LOAD_CONTEXT_BASE\
    STK_ASM_LOAD_CONTEXT_RV32I_EXT\
    STK_ASM_LOAD_CONTEXT_FP\
    "addi sp, sp, " REGSIZE   " \n" /* shrink stack memory of registers */

// Make file local content (anonymous namespace): helpers which depend on Context.
// ****************************************************************************
namespace { // file-local block (private)
// ****************************************************************************

__stk_forceinline void HW_LoadContextAndExit()
{
    __asm__ __volatile__(
    LREG " t0, %0               \n" // load the first member (SP) into t0
    LREG " sp, 0(t0)            \n" // sp = t0

    STK_ASM_LOAD_CONTEXT
    STK_ASM_EXIT_FROM_HANDLER " \n"

    : /* output: none */
    : "m"(GetContext().m_stack_active)
    : "t0", "t1", "a2", "a3", "a4", "a5", "gp", "memory");
}

__stk_forceinline void HW_EnableFullFpuAccess()
{
#if (STK_RISCV_FPU != 0)
    __asm__ __volatile__(
    "csrs mstatus, %0"
    : /* output: none */
    : "r"(MSTATUS_FS | MSTATUS_XS)
    : "memory" /* ensure no FP instructions are moved before this call */);
#endif
}

__stk_forceinline void HW_ClearFpuState()
{
#if (STK_RISCV_FPU != 0)
    __asm__ __volatile__(
    "fssr x0"
    : /* output: none */
    : /* input: none */
    : "memory" /* ensure flags are cleared before next FP op */);
#endif
}

__stk_forceinline void HW_SaveMainSP()
{
    __asm__ __volatile__(
    SREG " sp, %0"
    : "=m"(GetContext().m_stack_main)
    : /* input: none */
    : "memory" /* protect against compiler reordering */ );
}

__stk_forceinline void HW_LoadMainSP()
{
    __asm__ __volatile__(
    LREG " sp, %0"
    : /* output: none */
    : "m"(GetContext().m_stack_main)
    : "memory" /* protect against compiler reordering */ );
}

/*! \brief  Get active machine exception/interrupt cause code.
    \note   Reads the \c mcause CSR directly. Valid only when called from a trap
            handler context (i.e. when \c HW_IsHandlerMode() returns \c true).
            In thread context \c mcause retains its last trap value and must not
            be used for ISR detection - use \c HW_IsHandlerMode() for that.
    \return Raw \c mcause value:
              bit[31]    = 1 for asynchronous interrupt, 0 for synchronous exception.
              bits[30:0] = cause code, unique per interrupt/exception source.
*/
__stk_forceinline Word HW_GetCurrentException()
{
    Word mcause;
    __asm__ __volatile__("csrr %0, mcause" : "=r"(mcause));
    return mcause;
}

__stk_forceinline bool HW_IsHandlerMode()
{
    const Word current_sp = HW_GetCallerSP();

    // get the bounds of the ISR stack from our Context
    // note: STK uses StackMemoryWrapper, so we check against that memory block
    const Word isr_stack_base = hw::PtrToWord(&GetContext().m_stack_isr_mem);
    const Word isr_stack_top  = isr_stack_base + (STK_RISCV_ISR_STACK_SIZE * sizeof(Word));

    return ((current_sp >= isr_stack_base) && (current_sp < isr_stack_top));
}

__stk_forceinline void OnTaskStart()
{
    HW_LoadContextAndExit();
}

// ****************************************************************************
} // namespace {
// ****************************************************************************

// __stk_attr_used for LTO
extern "C" STK_RISCV_ISR_SECTION __stk_attr_used void TrySwitchContext()
{
    GetContext().OnSwitchContext();

#if STK_RISCV_USER_MODE && !defined(_STK_RISCV_USE_PENDSV)
    // the switch is committed by the SysTick handler, load privilege (and PMP) state of the (possibly new) active task
    hw_int::LoadActiveTask(s_StkRiscvStackActive[HW_GetHartId()]);
#endif
}

// ############################################################################
#if STK_RISCV_USER_MODE
// ############################################################################

/*! \brief Make privilege (and PMP, if STK_MPU) state of the active task effective (called between context save and load).
    \note  Leaf-like C function, called from asm on the ISR stack: all caller-saved registers are free there.
*/
extern "C" STK_RISCV_ISR_SECTION __stk_attr_used void StkRiscvLoadActive()
{
    hw_int::LoadActiveTask(s_StkRiscvStackActive[HW_GetHartId()]);
}

/*! \brief   Dispatcher of synchronous traps taken from/with the saved task frame (ecall services, PMP faults if STK_MPU).
    \param   cause: mcause.
    \param   f: Saved task frame (on the task stack), modified in place and restored by the asm epilogue.
    \return  SP to resume with (frame address).
*/
extern "C" STK_RISCV_ISR_SECTION __stk_attr_used Word StkRiscvTrapDispatch(Word cause, TaskFrame *tf)
{
    Context &ctx = GetContext();
    const bool from_user = !tf->IsPrivileged();

    if ((cause == STK_RISCV_CAUSE_ECALL_U) || (cause == STK_RISCV_CAUSE_ECALL_M))
    {
    #if STK_MPU
        const Word ecall_pc = tf->MEPC;
        const bool in_shared = (ecall_pc >= hw::PtrToWord(__stk_mpu_shared_code_start)) &&
                               (ecall_pc <  hw::PtrToWord(__stk_mpu_shared_code_end));
    #else
        // no memory protection: there is no shared code region to bind the escalation to (and nothing
        // to protect, U-mode code can alter any memory anyway), accept boost/drop from any call site
        const bool in_shared = true;
    #endif

        tf->MEPC += 4U; // ecall is always a 4 byte instruction

        switch (static_cast<ESvcCommandId>(tf->X15 & 0xFFU))
        {
        case SVC_ENTER_CRITICAL:
            ctx.EnterCriticalSectionNpriv(); // masks via mie, valid whichever mode made the ecall
            break;

        case SVC_EXIT_CRITICAL:
            ctx.ExitCriticalSection();       // restores mie because m_csu_npriv is set
            break;

        case SVC_BOOST_PRIV:
            // limit privilege escalation to STK_MPU_SHARED_CODE_SECTION functions only (read-only, immutable),
            // the check is effective with STK_MPU only
            if (in_shared && from_user)
            {
                tf->MakePrivileged();
                s_StkRiscvNpriv[HW_GetHartId()] = false; // boost to privileged
            }
            else
            {
                STK_KERNEL_PANIC(KERNEL_PANIC_NS_ACCESS);
            }
            break;

        case SVC_DROP_PRIV:
            if (in_shared && !from_user)
            {
                tf->MakeNonPrivileged();
                s_StkRiscvNpriv[HW_GetHartId()] = true; // revert to non-privileged
            }
            else
            {
                STK_KERNEL_PANIC(KERNEL_PANIC_NS_ACCESS);
            }
            break;

        default:
            // any service number not in ESvcCommandId is a defect, panic unconditionally
            STK_KERNEL_PANIC(KERNEL_PANIC_UNKNOWN_SVC);
            break;
        }
    }
#if STK_MPU
    else
    {
        // access faults (PMP violations): mcause 1, 5, 7
        if (from_user)
        {
            FaultContext fc;
            fc.mcause  = cause;
            fc.mepc    = tf->MEPC;
            fc.mtval   = read_csr(mtval);
            fc.mstatus = tf->MSTATUS;
        #if STK_MPU
            for (size_t w = 0U; w < FaultContext::PMP_CFG_WORDS; ++w)
            {
                fc.mpu.cfg[w] = hw_int::pmp::ReadCfgWord(w);
            }
            for (size_t i = 0U; i < STK_RISCV_PMP_REGIONS_MAX; ++i)
            {
                fc.mpu.addr[i] = hw_int::pmp::ReadAddr(i);
            }
        #endif

            const TId tid = ctx.m_handler->OnGetTid(hw::PtrToWord(tf));

            // if handled: skip the faulting load/store instruction (2 or 4 bytes), an instruction
            // access fault can not be skipped (instruction length is unknown), treat it as fatal
            if ((ctx.m_overrider != nullptr) &&
                ctx.m_overrider->OnException(HW_EXCEPT_MEMACCESS, tid, &fc) &&
                (cause != STK_RISCV_CAUSE_IACCESS_FAULT))
            {
                const uint16_t insn = *reinterpret_cast<const uint16_t *>(tf->MEPC);
                tf->MEPC += (((insn & 0x3U) == 0x3U) ? 4U : 2U);
            }
            else
            {
                STK_KERNEL_PANIC(KERNEL_PANIC_CPU_EXCEPTION);
            }
        }
        else
        {
            // fault of M-mode code: same behaviour as for any other unexpected exception
            if (ctx.m_specific != nullptr)
            {
                ctx.m_specific->OnException(cause);
            }
            else
            {
                STK_KERNEL_PANIC(KERNEL_PANIC_CPU_EXCEPTION);
            }
        }
    }

#else
    else
    {
        // the front-end routes ecalls only (no PMP faults to handle without STK_MPU)
        STK_KERNEL_PANIC(KERNEL_PANIC_CPU_EXCEPTION);
    }
#endif // STK_MPU

    return hw::PtrToWord(tf);
}

// ############################################################################
#endif // STK_RISCV_USER_MODE
// ############################################################################

// ############################################################################
#ifdef _STK_RISCV_USE_PENDSV
// ############################################################################

extern "C" STK_RISCV_ISR_SECTION __stk_attr_naked void STK_SYSTICK_HANDLER()
{
    __asm__ __volatile__(
    // 1. save full interrupted context onto the task stack
    STK_ASM_SAVE_CONTEXT

    // 2. store task SP into s_StkRiscvSpIsrInt[hart] directly (plain Word, no struct indirection)
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvSpIsrInt         \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n" // t0 = hart * sizeof(Word)
    "add  t1, t1, t0                     \n" // t1 = &s_StkRiscvSpIsrInt[hart]
    SREG " sp, 0(t1)                     \n" // store sp directly - no pointer dereference
#else
    "la   t1, s_StkRiscvSpIsrInt         \n"
    SREG " sp, 0(t1)                     \n" // store sp directly - no pointer dereference
#endif

    // 3. switch to private ISR stack
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvStackIsr         \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n"
    "add  t1, t1, t0                     \n"
    LREG " t1, 0(t1)                     \n"
#else
    "la   t1, s_StkRiscvStackIsr         \n"
    LREG " t1, 0(t1)                     \n"
#endif
    LREG " sp, 0(t1)                     \n" // sp = Stack::SP of ISR stack

    // 4. run scheduler
    "jal  ra, TrySwitchContext           \n"

    // 5. restore the interrupted task's SP from s_StkRiscvSpIsrInt[hart]
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvSpIsrInt         \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n"
    "add  t1, t1, t0                     \n"
    LREG " sp, 0(t1)                     \n" // sp = saved task SP - direct load, no struct
#else
    "la   t1, s_StkRiscvSpIsrInt         \n"
    LREG " sp, 0(t1)                     \n" // sp = saved task SP - direct load, no struct
#endif

    // 6. restore context
    STK_ASM_LOAD_CONTEXT

    // 7. exit ISR handler
    STK_ASM_EXIT_FROM_HANDLER "          \n"

    : /* outputs:  none - naked, compiler emits nothing outside this asm */
    : /* inputs: all addresses loaded as linker symbols via "la" */
    : /* clobbers: none - the asm string owns all registers */);
}

// ############################################################################
#endif // _STK_RISCV_USE_PENDSV
// ############################################################################

/*! \brief STK_MSI_HANDLER

    Performs the actual raw stack-pointer swap for a context switch, driven by the CLINT
    Machine Software Interrupt (MSIP), triggered via HW_PendForceSwitch().

    Compiled and wired unconditionally (regardless of _STK_RISCV_USE_PENDSV):

     - PendSV builds:     this is the ONLY place a switch is ever applied - both
                          tick-decided (via HW_ScheduleContextSwitch) and forced
                          (via HW_PendForceSwitch) switches go through here.

     - Non-PendSV builds: tick-decided switches are still applied inline within
                          STK_SYSTICK_HANDLER itself (see below) for lower overhead; this
                          handler is used ONLY for forced (asynchronous) switches requested
                          via Context::OnForceContextSwitch(), which can be called from
                          arbitrary task/ISR context where no inline swap is possible.

    Reads s_StkRiscvStackIdle[hart] for the task being switched OUT and
    s_StkRiscvStackActive[hart] for the task being switched IN - both are refreshed by the
    caller (ProcessTick() or OnForceContextSwitch()) immediately before triggering this ISR.
*/
extern "C" STK_RISCV_ISR_SECTION __stk_attr_naked void STK_MSI_HANDLER()
{
    __asm__ __volatile__(
    // 1. save context
    STK_ASM_SAVE_CONTEXT

    // 2. store task SP into s_StkRiscvStackIdle[hart]->SP
    // all integer registers are now saved. t0/t1 are free to use as scratch.
    // "la" loads the address of the global array - a linker-time constant,
    // no compiler-generated runtime code, safe to use here
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvStackIdle        \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n" // t0 = hart * sizeof(Stack*)
    "add  t1, t1, t0                     \n" // t1 = &s_StkRiscvStackIdle[hart]
    LREG " t1, 0(t1)                     \n" // t1 = s_StkRiscvStackIdle[hart] (Stack*)
#else
    "la   t1, s_StkRiscvStackIdle        \n"
    LREG " t1, 0(t1)                     \n" // t1 = s_StkRiscvStackIdle[0] (Stack*)
#endif
    SREG " sp, 0(t1)                     \n" // Stack::SP = task's sp (SP is first member)

    // 3. clear exception: MSIP[hart] = 0
#ifdef STK_RISCV_SOFTIRQ_ADDR
    // RP2350: write CLR bit (8 + hart) of SIO RISCV_SOFTIRQ. All registers are already
    // saved, t0-t2 are free scratch.
    "csrr  t1, mhartid                   \n"
    "li    t2, 0x100                     \n"
    "sll   t2, t2, t1                    \n" // t2 = 0x100 << hart
    "li    t0, %[softirq_addr]           \n"
    "sw    t2, 0(t0)                     \n"
#else
  #if (STK_ARCH_CPU_COUNT > 1U)
    "csrr  t0, mhartid                   \n"
    "slli  t0, t0, 2                     \n" // t0 = hart * 4
    "li    t1, %[clint_msip_base]        \n"
    "add   t0, t0, t1                    \n" // t0 = &MSIP[hart]
  #else
    "li    t0, %[clint_msip_base]        \n" // t0 = &MSIP[0]
  #endif
    "sw    zero, 0(t0)                   \n" // MSIP[hart] = 0
#endif
    "fence rw, rw                        \n" // fence rw,rw  - ensure the write is visible before re-enable

#if STK_RISCV_USER_MODE
    // U-mode/PMP: switch to the private ISR stack and make privilege (and PMP) state of the incoming task effective
    // (all registers of the incoming task are not loaded yet, caller-saved ones are free)
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvStackIsr         \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n"
    "add  t1, t1, t0                     \n" // t1 = &s_StkRiscvStackIsr[hart]
    LREG " t1, 0(t1)                     \n" // t1 = s_StkRiscvStackIsr[hart] (Stack*)
#else
    "la   t1, s_StkRiscvStackIsr         \n"
    LREG " t1, 0(t1)                     \n" // t1 = s_StkRiscvStackIsr[0] (Stack*)
#endif
    LREG " sp, 0(t1)                     \n" // sp = Stack::SP of ISR stack
    "jal  ra, StkRiscvLoadActive         \n"
#endif

    // 4. load SP from s_StkRiscvStackActive[hart]->SP
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvStackActive      \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n"
    "add  t1, t1, t0                     \n"
    LREG " t1, 0(t1)                     \n"
#else
    "la   t1, s_StkRiscvStackActive      \n"
    LREG " t1, 0(t1)                     \n"
#endif
    LREG " sp, 0(t1)                     \n"  // sp = active task's saved SP

    // 5. load context of the active task
    STK_ASM_LOAD_CONTEXT

    // 6. exit ISR handler
    STK_ASM_EXIT_FROM_HANDLER "          \n"

    : /* outputs:  none - naked, compiler emits nothing outside this asm */
#ifdef STK_RISCV_SOFTIRQ_ADDR
    : [softirq_addr] "i" (STK_RISCV_SOFTIRQ_ADDR) /* other inputs: all addresses loaded as linker symbols via "la" */
#else
    : [clint_msip_base] "i" (STK_RISCV_CLINT_BASE_ADDR) /* other inputs: all addresses loaded as linker symbols via "la" */
#endif
    : /* clobbers: none - the asm string owns all registers */);
}

// ############################################################################
#ifndef _STK_RISCV_USE_PENDSV
// ############################################################################

/* STK_SYSTICK_HANDLER

RISC-V machine-timer ISR: Saves the interrupted task's full context, switches
to the private ISR stack, calls TrySwitchContext (which reschedules the timer
and runs the scheduler), then restores the (possibly new) task's context.

DESIGN RULES - must be obeyed to work correctly at all optimisation levels:

 1. Single asm volatile, no compiler operands.
    The function body is ONE __asm__ __volatile__("..." : : : ) with empty
    input/output/clobber lists. No "m" or "r" constraints are used because
    the compiler evaluates those as C expressions BEFORE emitting any asm
    text, i.e. before the register save - trashing uninitialized registers.

 2. All addresses are linker symbols loaded via "la" inside the asm.
    s_StkRiscvStackActive and s_StkRiscvStackIsr are plain file-scope globals
    with C linkage (declared extern "C" above). "la reg, sym" emits a PC-relative
    load that is resolved at link time, it produces no compiler-generated code
    outside the asm string.

 3. Stack pointer indexing uses sizeof(Stack*) == REGBYTES.
    For multi-hart builds the array index is hart * REGBYTES, which is a
    single left-shift by log2(REGBYTES): 2 for RV32 (4 bytes), 3 for RV64
    (8 bytes). REGBYTES_LOG2 is defined below accordingly.

 4. s_StkRiscvStackActive[hart]->SP is updated by TrySwitchContext.
    The naked asm reads it fresh after the jal returns, so it always sees
    the task the scheduler has chosen - even if it changed.

 Stack frame layout (offsets from sp after "addi sp,-REGSIZE"):
   [0*REGBYTES] mepc     (service slot 0)
   [1*REGBYTES] mstatus  (service slot 1)
   [2*REGBYTES] x1  / ra
   [3*REGBYTES] x2  / sp  - SKIPPED, managed explicitly
   [4*REGBYTES] x3  / gp  - SKIPPED, fixed register; slot reused for FCSR
   [5*REGBYTES] x4  / tp
   [6*REGBYTES] x5  / t0
   ...
   [32*REGBYTES] x31 / t6  (RV32I; absent on RV32E)
   [FOFFSET + n*FREGBYTES] fn  (FP registers, if STK_RISCV_FPU != 0)
*/
extern "C" STK_RISCV_ISR_SECTION __stk_attr_naked void STK_SYSTICK_HANDLER()
{
    __asm__ __volatile__(
    // 1. save context
    STK_ASM_SAVE_CONTEXT

    // 2. store task SP into s_StkRiscvStackActive[hart]->SP
    // all integer registers are now saved. t0/t1 are free to use as scratch.
    // "la" loads the address of the global array - a linker-time constant,
    // no compiler-generated runtime code, safe to use here
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvStackActive      \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n"  // t0 = hart * sizeof(Stack*)
    "add  t1, t1, t0                     \n"  // t1 = &s_StkRiscvStackActive[hart]
    LREG " t1, 0(t1)                     \n"  // t1 = s_StkRiscvStackActive[hart] (Stack*)
#else
    "la   t1, s_StkRiscvStackActive      \n"
    LREG " t1, 0(t1)                     \n"  // t1 = s_StkRiscvStackActive[0] (Stack*)
#endif
    SREG " sp, 0(t1)                     \n"  // Stack::SP = task's sp (SP is first member)

    // 3. switch to private ISR stack
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvStackIsr         \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n"
    "add  t1, t1, t0                     \n"
    LREG " t1, 0(t1)                     \n"  // t1 = s_StkRiscvStackIsr[hart] (Stack*)
#else
    "la   t1, s_StkRiscvStackIsr         \n"
    LREG " t1, 0(t1)                     \n"  // t1 = s_StkRiscvStackIsr[0] (Stack*)
#endif
    LREG " sp, 0(t1)                     \n"  // sp = Stack::SP of ISR stack

    // 4. call TrySwitchContext
    // runs on the ISR stack: reschedules timer, runs scheduler
    // (which may update m_stack_active to a new task), then updates
    // s_StkRiscvStackActive[hart] so step 5 below reads the correct new SP,
    // all caller-saved registers (a0-a7, t0-t6, ra) are trashed - expected
    "jal  ra, TrySwitchContext           \n"

    // 5. reload SP from s_StkRiscvStackActive[hart]->SP
    // TrySwitchContext updated s_StkRiscvStackActive[hart] before returning,
    // we re-read it fresh to pick up any task switch the scheduler made
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr t0, mhartid                    \n"
    "la   t1, s_StkRiscvStackActive      \n"
    "slli t0, t0, " REGBYTES_LOG2 "      \n"
    "add  t1, t1, t0                     \n"
    LREG " t1, 0(t1)                     \n"
#else
    "la   t1, s_StkRiscvStackActive      \n"
    LREG " t1, 0(t1)                     \n"
#endif
    LREG " sp, 0(t1)                     \n"  // sp = active task's saved SP

    // 6. load context of the active task
    STK_ASM_LOAD_CONTEXT

    // 7. exit ISR handler
    STK_ASM_EXIT_FROM_HANDLER "          \n"

    : /* outputs:  none - naked, compiler emits nothing outside this asm */
    : /* inputs:   none - all addresses loaded as linker symbols via "la" */
    : /* clobbers: none - the asm string owns all registers */
    );
}

// ############################################################################
#endif // !_STK_RISCV_USE_PENDSV
// ############################################################################

void Context::OnStart()
{
    const uint8_t hart = HW_GetHartId();

    // save SP of main stack to reuse it for scheduler exit
    HW_SaveMainSP();

    // enable FPU (if available)
    HW_EnableFullFpuAccess();

    // clear FPU usage status if FPU was used before kernel start
    HW_ClearFpuState();

    // notify kernel
    m_handler->OnStart(m_stack_active);

    // initialize ISR asm pointer cache
    s_StkRiscvStackIsr[hart]    = &m_stack_isr; // set once here, the ISR stack never moves
    s_StkRiscvStackActive[hart] = m_stack_active;
    s_StkRiscvStackIdle[hart]   = m_stack_idle;

#if STK_MPU
    // configure static PMP regions
    ConfigureMpu();
#elif STK_RISCV_USER_MODE && STK_RISCV_PMP_PRESENT
    // U-mode without memory protection: grant U-mode access to the whole address space
    hw_int::pmp::ConfigureUserAccess();
#endif

#if STK_RISCV_USER_MODE
    // publish privilege state (and PMP window, if STK_MPU) of the first task
    hw_int::LoadActiveTask(m_stack_active);
#endif

    // start with initially 1 elapsed tick (after timer expires)
    StartTickTimer(1);

    // change state before enabling interrupts
    m_started  = true;
    m_starting = false;

    // start recording task execution start
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_OnTaskStartExec(m_stack_active->tid);
#endif

    // enable SV exception (MSI)
    // note: unconditional - even the single-handler (non-PendSV) tick path needs this,
    // since Context::OnForceContextSwitch() always triggers a switch via STK_MSI_HANDLER
    set_csr(mie, MIP_MSIP);
}

#if STK_RISCV_USER_MODE
extern "C" void StkRiscvSvcHandlerC();

/* STK_SVC_HANDLER (U-mode build)

   Naked front-end of the exception vector. Traps it owns are processed by StkRiscvTrapDispatch() on the ISR stack:
     - mcause 8  : ecall from U-mode (service call, id in a5),
     - mcause 11 : ecall from M-mode carrying STK_RISCV_SVC_MAGIC in a5 (privilege drop of boosted code),
     - mcause 1/5/7 : access faults (PMP violations, STK_MPU only).
   Anything else (scheduler start ecall, user-forwarded ecalls, other exceptions) is chained to the original
   handler with all registers intact. t0 is borrowed through mscratch for the classification only.
*/
extern "C" STK_RISCV_ISR_SECTION __stk_attr_naked void STK_SVC_HANDLER()
{
    __asm__ __volatile__(
    "csrw  mscratch, t0                  \n"
    "csrr  t0, mcause                    \n"
#if STK_MPU
    "addi  t0, t0, -1                    \n" // 1: instruction access fault
    "beqz  t0, .Lstk_svc_ours            \n"
    "addi  t0, t0, -4                    \n" // 5: load access fault
    "beqz  t0, .Lstk_svc_ours            \n"
    "addi  t0, t0, -2                    \n" // 7: store/AMO access fault
    "beqz  t0, .Lstk_svc_ours            \n"
    "addi  t0, t0, -1                    \n" // 8: ecall from U-mode
    "beqz  t0, .Lstk_svc_ours            \n"
#else
    "addi  t0, t0, -8                    \n" // 8: ecall from U-mode
    "beqz  t0, .Lstk_svc_ours            \n"
#endif
    "addi  t0, t0, -3                    \n" // 11: ecall from M-mode
    "bnez  t0, .Lstk_svc_chain           \n"
    "li    t0, " XSTR(STK_RISCV_SVC_MAGIC) " \n"
    "xor   t0, t0, a5                    \n"
    "srli  t0, t0, 8                     \n" // a5[31:8] == magic -> service call
    "bnez  t0, .Lstk_svc_chain           \n"

    ".Lstk_svc_ours:                     \n"
    "csrr  t0, mscratch                  \n" // restore t0
    STK_ASM_SAVE_CONTEXT
    "mv    a1, sp                        \n" // a1 = TaskFrame*
#if (STK_ARCH_CPU_COUNT > 1U)
    "csrr  t0, mhartid                   \n"
    "slli  t0, t0, " REGBYTES_LOG2 "     \n" // t0 = hart * sizeof(Stack*)
    "la    t1, s_StkRiscvStackActive     \n"
    "add   t1, t1, t0                    \n" // t1 = &s_StkRiscvStackActive[hart]
    LREG " t1, 0(t1)                     \n"
    SREG " sp, 0(t1)                     \n" // Stack::SP = frame
    "la    t1, s_StkRiscvStackIsr        \n"
    "add   t1, t1, t0                    \n" // t1 = &s_StkRiscvStackIsr[hart]
    LREG " t1, 0(t1)                     \n"
    LREG " sp, 0(t1)                     \n" // switch to ISR stack
#else
    "la    t1, s_StkRiscvStackActive     \n"
    LREG " t1, 0(t1)                     \n"
    SREG " sp, 0(t1)                     \n" // Stack::SP = frame
    "la    t1, s_StkRiscvStackIsr        \n"
    LREG " t1, 0(t1)                     \n"
    LREG " sp, 0(t1)                     \n" // switch to ISR stack
#endif
    "csrr  a0, mcause                    \n"
    "jal   ra, StkRiscvTrapDispatch      \n" // a0 = SP to resume with
    "mv    sp, a0                        \n"
    STK_ASM_LOAD_CONTEXT
    STK_ASM_EXIT_FROM_HANDLER "          \n"

    ".Lstk_svc_chain:                    \n"
    "csrr  t0, mscratch                  \n" // restore t0
    "j     StkRiscvSvcHandlerC           \n"
    : /* outputs:  none */
    : /* inputs:   none */
    : /* clobbers: none */);
}
#define STK_SVC_HANDLER_BODY StkRiscvSvcHandlerC
#else
#define STK_SVC_HANDLER_BODY STK_SVC_HANDLER
#endif

STK_RISCV_ISR void STK_SVC_HANDLER_BODY()
{
    const Word cause = HW_GetCurrentException();
    Context &ctx = GetContext();

    /*if (cause & (1UL << (__riscv_xlen - 1)))
    {
        cause &= ~(1UL << (__riscv_xlen - 1));

        if (cause == IRQ_M_TIMER)
        {

        }
    }*/

    if (cause == IRQ_M_EXT)
    {
        // not starting scheduler, then try to forward ecall to user
        if (!ctx.m_starting)
        {
            // forward event to user
            if (ctx.m_specific != nullptr)
            {
                ctx.m_specific->OnException(cause);
            }

            // switch to the next instruction of the caller space (PC) after the return
            write_csr(mepc, read_csr(mepc) + sizeof(Word));
        }
        else
        {
            // make sure interrupts do not interfere
            HW_DisableInterrupts();

            // configure scheduling
            ctx.OnStart();

            // start first task
            OnTaskStart();
        }
    }
    else
    {
        if (ctx.m_specific != nullptr)
        {
            // forward event to user
            ctx.m_specific->OnException(cause);
        }
        else
        {
            // trap further execution
            // note: normally, if trapped here with cause 2 or 4 then check stack memory size of the
            // tasks, scheduler and ISR, they were likely overwritten if your code is 100% correct
            STK_KERNEL_PANIC(KERNEL_PANIC_CPU_EXCEPTION);
        }
    }
}

STK_MPU_SHARED_CODE_SECTION
void drv::OnTaskRun(ITask *runnable)
{
    STK_ASSERT(runnable != nullptr);
    runnable->Run();
}

STK_MPU_SHARED_CODE_SECTION
void drv::OnTaskExit()
{
#if STK_RISCV_USER_MODE
    // U-mode task: elevate for the kernel call (ecall is inside the shared code region)
    const bool boost = !HW_IsPrivilegedContext();
    if (boost)
    {
        HW_Ecall(SVC_BOOST_PRIV);
    }
#endif

    const Context &ctx = GetContext();

    {
        // scoped: the section must end before the idle loop below (which never returns)
        const ScopedHwIrqLock cs_;

        ctx.m_handler->OnTaskExit(ctx.m_stack_active);
    }

#if STK_RISCV_USER_MODE
    if (boost)
    {
        HW_Ecall(SVC_DROP_PRIV);

        // can only busy-wait when non-Privileged (WFI is not guaranteed to be available in U-mode)
        for (;;)
        {
            __stk_relax_cpu();
        }
    }
    else
#endif
    {
        for (;;)
        {
            __DSB(); // data barrier
            __WFI(); // enter standby mode until time slot expires
        }
    }
}

STK_RISCV_ISR_SECTION
void drv::OnSchedulerSleep()
{
    // if hit here, increase the size of STK_SLEEP_TRAP_STACK_SIZE
    STK_STATIC_ASSERT(STK_SLEEP_TRAP_STACK_SIZE >= STK_STACK_SIZE_MIN);

    for (;;)
    {
    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnIdle();
    #endif

        HW_EnterSleepMode();
    }
}

STK_RISCV_ISR_SECTION
void drv::OnSchedulerSleepOverride()
{
    // if hit here, increase the size of STK_SLEEP_TRAP_STACK_SIZE
    STK_STATIC_ASSERT(STK_SLEEP_TRAP_STACK_SIZE >= STK_STACK_SIZE_MIN);

    for (;;)
    {
    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnIdle();
    #endif

        GetContext().OnSleepOverride();
    }
}

void drv::OnSchedulerExit()
{
    // switch to main stack
    HW_LoadMainSP();

    // jump to the exit from the IKernel::Start()
    drv::RestoreJmp(GetContext().m_exit_buf, 0);
}

void PlatformRiscV::Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap)
{
    GetContext().Initialize(event_handler, service, exit_trap, resolution_us);
}

void Context::Start()
{
    m_exiting = false;

    // save jump location of the Exit trap
    STK_UNUSED(drv::SaveJmp(m_exit_buf));
    if (m_exiting)
    {
        // notify kernel about a full stop
        m_handler->OnStop();
    }
    else
    {
        // enable FPU (if available)
        HW_EnableFullFpuAccess();

        // start
        m_starting = true;
        HW_StartScheduler();
    }
}

#if STK_TICKLESS_IDLE
Timeout Context::Suspend()
{
    const uint32_t resolution = static_cast<uint32_t>(ConvertTimeUsToClockCycles(HW_CoreClockFrequency(), m_tick_resolution));
    if (resolution == 0U)
    {
        STK_ASSERT(false);
        return NO_WAIT;
    }

    HW_DisableInterrupts();

    // stop tick timer
    HW_StopMTimer();

    // clear pending PendSV exception
    HW_ClearPendingSwitch();

    // get already elapsed CPU cycles since SysTick ISR invocation up to SysTick timer stop (see above)
    // to account for them for a new period value
    const uint32_t elapsed = HW_GetMtime() - m_last_mtime;

    // get already elapsed ticks since the OnTick and a call to Suspend(), we shall account for this
    // period and return only the remainder
    const Timeout elapsed_ticks = static_cast<Timeout>(elapsed / resolution);
    const Timeout sleep_ticks = Max(m_sleep_ticks - elapsed_ticks, static_cast<Timeout>(0));

    // notify core
    m_handler->OnSuspend(true);

    HW_EnableInterrupts();

    return sleep_ticks;
}
#endif

#if STK_TICKLESS_IDLE
void Context::Resume(Timeout elapsed_ticks)
{
    HW_DisableInterrupts();

    // notify core
    m_handler->OnSuspend(false);

    // start with initially elapsed ticks (OnTick will fire with elapsed_ticks + 1)
    StartTickTimer(elapsed_ticks + 1);

    HW_EnableInterrupts();
}
#endif

void PlatformRiscV::Start()
{
    GetContext().Start();
}

// ############################################################################
#if STK_MPU_STACK_GUARD
// ############################################################################

// ****************************************************************************
namespace { // file-local block (private)
// ****************************************************************************

/*! \brief Build per-task PMP shadow table: application regions first, stack guard as the last slot.
    \note  Mirror of the ARM driver: a task which does not provide MPU regions gets no stack guard,
           the guard is canceled if the task's own regions overlap the stack.
    \note  Stack memory must be power-of-two sized and naturally aligned (NAPOT).
*/
void ConfigureTaskMpu(IPlatform::IEventOverrider *overrider, Stack *stack, IStackMemory *stack_memory,
    ITask *user_task)
{
    MpuRegionConfig task_mpu_cfg[TaskMpu::NUM_REGIONS] = {};
    size_t cfg_count = 0U;

    // user_task is null for the internal sleep/exit trap stacks, which never carry application-defined regions
    if (user_task != nullptr)
    {
        const MpuRegionList *const regions = user_task->GetMpuRegions();
        if (regions != nullptr)
        {
            const MpuRegionConfig stack_guard_cfg =
            {
                .addr        = hw::PtrToWord(stack_memory->GetStack()),
                .size        = stack_memory->GetStackSize() * sizeof(Word),
                .access_perm = hw::mpu::ACCESS_FULL,
                .mem_type    = hw::mpu::TYPE_NORMAL_CACHEABLE,
                .share       = hw::mpu::SHARE_NON,
                .exec        = hw::mpu::EXEC_NEVER
            };

            const hw_int::pmp::RegionBounds bounds_s = hw_int::pmp::GetRegionBounds(stack_guard_cfg);

            size_t regions_count = regions->GetSize();
            bool stack_guard_canceled = false;

            for (size_t i = 0U; i < regions_count; ++i)
            {
                const hw_int::pmp::RegionBounds bounds_i = hw_int::pmp::GetRegionBounds((*regions)[i]);

                if (hw_int::pmp::IsOverlapping(bounds_s, bounds_i))
                {
                    stack_guard_canceled = true;
                    break;
                }
            }

            const size_t max_user_regions = stack_guard_canceled ? TaskMpu::NUM_REGIONS : (TaskMpu::NUM_REGIONS - 1U);

            STK_ASSERT(regions_count <= max_user_regions);
            if (regions_count > max_user_regions)
            {
                regions_count = max_user_regions; // defensively clamp in release builds
            }

            for (size_t i = 0U; i < regions_count; ++i)
            {
                task_mpu_cfg[cfg_count++] = (*regions)[i];
            }

            if (!stack_guard_canceled)
            {
                task_mpu_cfg[cfg_count++] = stack_guard_cfg;
            }
        }
    }

    hw_int::pmp::ConfigureDynamic(stack->mpu, task_mpu_cfg, cfg_count, overrider);
}

// ****************************************************************************
} // namespace {
// ****************************************************************************

// ############################################################################
#endif // STK_MPU_STACK_GUARD
// ############################################################################

void PlatformRiscV::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    // TaskFrame must map exactly onto the slot layout consumed by STK_ASM_SAVE_CONTEXT / STK_ASM_LOAD_CONTEXT - no padding allowed
    STK_STATIC_ASSERT_DESC(sizeof(TaskFrame) == (STK_RISCV_REGISTER_COUNT + STK_SERVICE_SLOTS) * sizeof(Word),
        "TaskFrame size must match REGSIZE: (REGISTER_COUNT + SERVICE_SLOTS) * REGBYTES");

    STK_ASSERT(stack_memory->GetStackSize() > (STK_RISCV_REGISTER_COUNT + STK_SERVICE_SLOTS));

    // initialize stack memory (fills all slots with STK_STACK_MEMORY_FILLER)
    const Word stack_top = PlatformContext::InitStackMemory(stack_memory);

    // initialize Stack Pointer (SP): frame sits at the bottom of the register window
    stack->SP = stack_top - ((STK_RISCV_REGISTER_COUNT + STK_SERVICE_SLOTS) * sizeof(Word));

    // place the task frame at SP directly at the base of the register window
    TaskFrame *const task_frame = hw::WordToPtr<TaskFrame>(stack->SP);

    // initialize registers for the user task's first start
    switch (stack_type)
    {
    case STACK_USER_TASK: {
        task_frame->MEPC   = hw::PtrToWord(&drv::OnTaskRun);
        task_frame->X1_RA  = hw::PtrToWord(&drv::OnTaskExit);
        task_frame->X10_A0 = hw::PtrToWord(user_task);
        break; }

    case STACK_SLEEP_TRAP: {
        task_frame->MEPC  = hw::PtrToWord(GetContext().m_overrider != nullptr ? &drv::OnSchedulerSleepOverride : &drv::OnSchedulerSleep);
        task_frame->X1_RA = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        break; }

    case STACK_EXIT_TRAP: {
        task_frame->MEPC  = hw::PtrToWord(&drv::OnSchedulerExit);
        task_frame->X1_RA = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        break; }

    default: {
        STK_KERNEL_PANIC(KERNEL_PANIC_BAD_STACK_TYPE);
        break; }
    }

    // mstatus: return to M-mode (MPP), interrupts enabled on mret (MPIE),
    // FPU/extension state initial (FS/XS) if FPU present
#if STK_RISCV_USER_MODE
    // ACCESS_USER tasks run in U-mode (MPP=0), where PMP (if STK_MPU) is enforced; traps (sleep/exit) always run in M-mode
    const bool is_priv = (stack_type != STACK_USER_TASK) || ((stack->access_mode & ACCESS_PRIVILEGED) != 0U);
    task_frame->MSTATUS = (is_priv ? MSTATUS_MPP : 0U) | MSTATUS_MPIE | (STK_RISCV_FPU != 0U ? (MSTATUS_FS | MSTATUS_XS) : 0U);
#else
    task_frame->MSTATUS = MSTATUS_MPP | MSTATUS_MPIE | (STK_RISCV_FPU != 0U ? (MSTATUS_FS | MSTATUS_XS) : 0U);
#endif

#if (STK_RISCV_FPU != 0)
    task_frame->X3_FSR = 0U; // FCSR = 0: round-to-nearest, no accrued exception flags
#endif

#if STK_MPU_STACK_GUARD
    ConfigureTaskMpu(GetContext().m_overrider, stack, stack_memory, user_task);
#endif
}

void Context::OnStop()
{
#if STK_RISCV_USER_MODE
    s_StkRiscvNpriv[HW_GetHartId()] = 0U; // main context is M-mode
#endif

    // stop timer
    HW_StopMTimer();

    // clear pending SV exception
    HW_ClearPendingSwitch();

    m_started = false;
    m_exiting = true;

    // make sure all assignments are set and executed
    __DSB();
    __ISB();
}

void PlatformRiscV::Stop()
{
    GetContext().OnStop();

    // load context of the Exit trap
    HW_DisableInterrupts();
    OnTaskStart();
}

uint32_t PlatformRiscV::GetTickResolution() const
{
    return GetContext().m_tick_resolution;
}

Cycles PlatformRiscV::GetSysTimerCount() const
{
    return static_cast<Cycles>(HW_GetMtime());
}

uint32_t PlatformRiscV::GetSysTimerFrequency() const
{
    return HW_MtimeClockFrequency();
}

void PlatformRiscV::ForceContextSwitch(TId id)
{
    const ScopedHwIrqLock cs_;

    Context &ctx = GetContext();
    STK_ASSERT(ctx.m_started);

    ctx.OnForceContextSwitch(id);
}

TId PlatformRiscV::GetTid() const
{
    TId result;

    if (HW_IsHandlerMode())
    {
        // to avoid the collision with TID_ISR_N mask, extract and fit into available space:

        const Word exc = HW_GetCurrentException();
        const Word num = (exc & 0x7FFU);
    #if (__riscv_xlen > 32)
        const Word interrupt_bit = ((exc & (1ULL << (__riscv_xlen - 1))) ? 0x800U : 0);
    #else
        const Word interrupt_bit = ((exc & (1U << (__riscv_xlen - 1))) ? 0x800U : 0);
    #endif

        const TId isr_tid = TID_ISR_N | num | interrupt_bit;
        STK_ASSERT(IsIsrTid(isr_tid));
        result = isr_tid;
    }
    else
    {
        const Context &ctx = GetContext();

        if (ctx.m_started)
        {
            result = ctx.m_handler->OnGetTid(HW_GetCallerSP());
        }
        else
        {
            result = TID_NONE;
        }
    }

    return result;
}

Timeout PlatformRiscV::Suspend()
{
#if STK_TICKLESS_IDLE
    return GetContext().Suspend();
#else
    return 0;
#endif
}

void PlatformRiscV::Resume(Timeout elapsed_ticks)
{
#if STK_TICKLESS_IDLE
    GetContext().Resume(elapsed_ticks);
#else
    STK_UNUSED(elapsed_ticks);
#endif
}

void PlatformRiscV::ProcessHardFault()
{
    bool is_handled = false;
    const Context &ctx = GetContext();

    if (ctx.m_overrider != nullptr)
    {
        is_handled = ctx.m_overrider->OnHardFault();
    }

    if (!is_handled)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_HRT_HARD_FAULT);
    }
}

void PlatformRiscV::SetEventOverrider(IEventOverrider *overrider, bool non_secure)
{
    STK_UNUSED(non_secure);

    STK_ASSERT(!GetContext().m_started);
    GetContext().m_overrider = overrider;
}

Word PlatformRiscV::GetCallerSP() const
{
    return HW_GetCallerSP();
}

void PlatformRiscV::SetCpuFrequency(uint8_t core_id, uint32_t frequency)
{
    const hw::CriticalSection::ScopedLock cs_;

    if (core_id == 0xFFU)
    {
        for (uint8_t i = 0U; i < STK_STATIC_ARRAY_SIZE(s_StkSystemCoreClock); ++i)
        {
            s_StkSystemCoreClock[i] = frequency;
        }
    }
    else
    {
        STK_ASSERT(core_id < STK_STATIC_ARRAY_SIZE(s_StkSystemCoreClock));

        if (core_id < STK_STATIC_ARRAY_SIZE(s_StkSystemCoreClock))
        {
            s_StkSystemCoreClock[core_id] = frequency;
        }
    }
}

void PlatformRiscV::SetSpecificEventHandler(ISpecificEventHandler *handler)
{
    STK_ASSERT(!GetContext().m_started);
    GetContext().m_specific = handler;
}

// ############################################################################
#if STK_RISCV_USER_MODE
// ############################################################################

// ****************************************************************************
namespace { // file-local block (private)
// ****************************************************************************

/*! \class ScopedPrivilegeBoost
    \brief RAII utility to temporarily elevate U-mode execution to M-mode via ecall (mirror of the ARM driver).
    \note  Both ecalls must originate from STK_MPU_SHARED_CODE_SECTION code (checked by the trap dispatcher
           if STK_MPU=1), the destructor is force-inlined into such code.
*/
struct ScopedPrivilegeBoost
{
    explicit ScopedPrivilegeBoost();

    __stk_forceinline ~ScopedPrivilegeBoost()
    {
        HW_Ecall(SVC_DROP_PRIV);
    }
};

STK_MPU_SHARED_CODE_SECTION
ScopedPrivilegeBoost::ScopedPrivilegeBoost()
{
    HW_Ecall(SVC_BOOST_PRIV);
}

/*! \class KernelServiceSvcProxy
    \brief Proxy wrapper providing unprivileged tasks secure access to privileged kernel services.

    This class implements the \a stk::IKernelService interface, acting as a bridge across the
    MPU boundary. It resides within a dedicated shared data section and utilizes \a ScopedPrivilegeBoost
    internally to elevate execution rights before forwarding system requests to the underlying
    privileged kernel instance.

    \note **HARDWARE ISOLATION:**
          This proxy allows user-space tasks to invoke kernel APIs safely. The privilege elevation
          occurs only for the duration of the targeted kernel service execution.
*/
class KernelServiceSvcProxy final : public stk::IKernelService
{
public:
    /*! \brief Default constructor.
    */
    explicit KernelServiceSvcProxy() : IKernelService()
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~KernelServiceSvcProxy() = default;

    stk::TId GetTid() const override;
    stk::Ticks GetTicks() const override;
    uint32_t GetTickResolution() const override;
    stk::Cycles GetSysTimerCount() const override;
    uint32_t GetSysTimerFrequency() const override;
    void Delay(stk::Timeout ticks) override;
    bool Sleep(stk::Timeout ticks) override;
    bool SleepUntil(stk::Ticks timestamp) override;
    void SleepCancel(stk::TId task_id) override;
    void SwitchToNext() override;
    stk::EWaitResult Wait(stk::ISyncObject *sobj, stk::IMutex *mutex, stk::Timeout timeout) override;
    stk::TId Wake(stk::ISyncObject *sobj, bool all) override;
    void WakeDeleted(ISyncObject *sobj) override;
    stk::Timeout Suspend() override;
    void Resume(stk::Timeout elapsed_ticks) override;
    stk::Weight GetWeight(stk::TId tid) const override;
    stk::EWaitOrder GetDefaultWaitOrder() const override;
    stk::IWaitObject *SelectWaiter(stk::IWaitObject::ListHeadType &wlist, stk::EWaitOrder order) override;
    void InheritWeight(stk::TId tid, stk::TId donor_tid) override;
    void InheritWeight(stk::TId tid, const stk::ISyncObject *sobj) override;
    void RestoreWeight(stk::TId tid, stk::ISyncObject *sobj) override;
    stk::Weight SetWeight(stk::TId tid, stk::Weight weight) override;
    stk::Timeout SetTimeSlice(stk::TId tid, stk::Timeout ticks) override;
    void ExitTask() override;
    ELockResult LockScheduler() override;
    ELockResult UnlockScheduler() override;
    ELockResult RestoreSchedulerLock(bool locked) override;
};

/*! \var s_KernelServiceSvcProxy
    \brief Shared static proxy instance exposed to unprivileged context calls.
*/
STK_MPU_SHARED_DATA_SECTION
KernelServiceSvcProxy s_StkKernelServiceUnprivProxy;

// ****************************************************************************
} // namespace {
// ****************************************************************************

STK_MPU_SHARED_CODE_SECTION
stk::TId KernelServiceSvcProxy::GetTid() const
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->GetTid();
}

STK_MPU_SHARED_CODE_SECTION
stk::Ticks KernelServiceSvcProxy::GetTicks() const
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->GetTicks();
}

STK_MPU_SHARED_CODE_SECTION
uint32_t KernelServiceSvcProxy::GetTickResolution() const
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_tick_resolution;
}

STK_MPU_SHARED_CODE_SECTION
stk::Cycles KernelServiceSvcProxy::GetSysTimerCount() const
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->GetSysTimerCount();
}

STK_MPU_SHARED_CODE_SECTION
uint32_t KernelServiceSvcProxy::GetSysTimerFrequency() const
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->GetSysTimerFrequency();
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::Delay(stk::Timeout ticks)
{
    const ScopedPrivilegeBoost pb;
    GetContext().m_service->Delay(ticks);
}

STK_MPU_SHARED_CODE_SECTION
bool KernelServiceSvcProxy::Sleep(stk::Timeout ticks)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->Sleep(ticks);
}

STK_MPU_SHARED_CODE_SECTION
bool KernelServiceSvcProxy::SleepUntil(stk::Ticks timestamp)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->SleepUntil(timestamp);
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::SleepCancel(stk::TId task_id)
{
    const ScopedPrivilegeBoost pb;
    GetContext().m_service->SleepCancel(task_id);
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::SwitchToNext()
{
    const ScopedPrivilegeBoost pb;
    GetContext().m_service->SwitchToNext();
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::ExitTask()
{
    const ScopedPrivilegeBoost pb;
    GetContext().m_service->ExitTask();
}

STK_MPU_SHARED_CODE_SECTION
ELockResult KernelServiceSvcProxy::LockScheduler()
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->LockScheduler();
}

STK_MPU_SHARED_CODE_SECTION
ELockResult KernelServiceSvcProxy::UnlockScheduler()
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->UnlockScheduler();
}

STK_MPU_SHARED_CODE_SECTION
ELockResult KernelServiceSvcProxy::RestoreSchedulerLock(bool locked)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->RestoreSchedulerLock(locked);
}

STK_MPU_SHARED_CODE_SECTION
stk::EWaitResult KernelServiceSvcProxy::Wait(stk::ISyncObject *sobj, stk::IMutex *mutex, stk::Timeout timeout)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->Wait(sobj, mutex, timeout);
}

STK_MPU_SHARED_CODE_SECTION
stk::TId KernelServiceSvcProxy::Wake(stk::ISyncObject *sobj, bool all)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->Wake(sobj, all);
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::WakeDeleted(stk::ISyncObject *sobj)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->WakeDeleted(sobj);
}

STK_MPU_SHARED_CODE_SECTION
stk::Timeout KernelServiceSvcProxy::Suspend()
{
    return 0; // not allowed for non-privileged process
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::Resume(stk::Timeout elapsed_ticks)
{
    // not allowed for non-privileged process
    STK_UNUSED(elapsed_ticks);
}

STK_MPU_SHARED_CODE_SECTION
stk::Weight KernelServiceSvcProxy::GetWeight(stk::TId tid) const
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->GetWeight(tid);
}

STK_MPU_SHARED_CODE_SECTION
stk::EWaitOrder KernelServiceSvcProxy::GetDefaultWaitOrder() const
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->GetDefaultWaitOrder();
}

STK_MPU_SHARED_CODE_SECTION
stk::IWaitObject *KernelServiceSvcProxy::SelectWaiter(stk::IWaitObject::ListHeadType &wlist, stk::EWaitOrder order)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->SelectWaiter(wlist, order);
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::InheritWeight(stk::TId tid, stk::TId donor_tid)
{
    const ScopedPrivilegeBoost pb;
    GetContext().m_service->InheritWeight(tid, donor_tid);
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::InheritWeight(stk::TId tid, const stk::ISyncObject *sobj)
{
    const ScopedPrivilegeBoost pb;
    GetContext().m_service->InheritWeight(tid, sobj);
}

STK_MPU_SHARED_CODE_SECTION
void KernelServiceSvcProxy::RestoreWeight(stk::TId tid, stk::ISyncObject *sobj)
{
    const ScopedPrivilegeBoost pb;
    GetContext().m_service->RestoreWeight(tid, sobj);
}

STK_MPU_SHARED_CODE_SECTION
stk::Weight KernelServiceSvcProxy::SetWeight(stk::TId tid, stk::Weight weight)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->SetWeight(tid, weight);
}

STK_MPU_SHARED_CODE_SECTION
stk::Timeout KernelServiceSvcProxy::SetTimeSlice(stk::TId tid, stk::Timeout ticks)
{
    const ScopedPrivilegeBoost pb;
    return GetContext().m_service->SetTimeSlice(tid, ticks);
}

// ############################################################################
#endif // STK_RISCV_USER_MODE
// ############################################################################

IKernelService *IKernelService::GetInstance()
{
#if STK_RISCV_USER_MODE
    if (HW_IsPrivilegedContext() || HW_IsHandlerMode())
    {
        return GetContext().m_service;
    }

    return &s_StkKernelServiceUnprivProxy;
#else
    return GetContext().m_service;
#endif
}

STK_MPU_SHARED_CODE_SECTION
stk::hw::CriticalSection::Session stk::hw::CriticalSection::Enter(const CriticalSection::Session ses)
{
#if STK_RISCV_USER_MODE
    const bool is_priv_ses = ((ses & SESSION_FLAG_NPRIV) == 0U);

    if (is_priv_ses && (HW_IsPrivilegedContext() || HW_IsHandlerMode()))
    {
        GetContext().EnterCriticalSection();
        return SESSION_FLAG_NONE;
    }

    HW_Ecall(SVC_ENTER_CRITICAL);
    return SESSION_FLAG_NPRIV;
#else
    STK_UNUSED(ses);
    GetContext().EnterCriticalSection();
    return DEFAULT_SESSION;
#endif
}

STK_MPU_SHARED_CODE_SECTION
void stk::hw::CriticalSection::Exit(const CriticalSection::Session ses)
{
#if STK_RISCV_USER_MODE
    if ((ses & SESSION_FLAG_NPRIV) == 0U)
    {
        GetContext().ExitCriticalSection();
    }
    else
    {
        HW_Ecall(SVC_EXIT_CRITICAL);
    }
#else
    STK_UNUSED(ses);
    GetContext().ExitCriticalSection();
#endif
}

STK_MPU_SHARED_CODE_SECTION
void stk::hw::SpinLock::Lock()
{
    HW_SpinLockLock(m_lock);
}

STK_MPU_SHARED_CODE_SECTION
void stk::hw::SpinLock::Unlock()
{
    HW_SpinLockUnlock(m_lock);
}

STK_MPU_SHARED_CODE_SECTION
bool stk::hw::SpinLock::TryLock()
{
    return HW_SpinLockTryLock(m_lock);
}

STK_MPU_SHARED_CODE_SECTION
bool stk::hw::IsInsideISR()
{
    return HW_IsHandlerMode();
}

STK_MPU_SHARED_CODE_SECTION
bool stk::hw::IsPrivilegedContext()
{
    return HW_IsPrivilegedContext();
}

stk::hw::HiResClock::HiResClock() : HiResClockState()
{}

Cycles stk::hw::HiResClock::GetCycles()
{
#if STK_SUBMICORSECOND_PRECISION_TIMER
    return HW_GetCycleCounter();
#else
    return HW_GetMtime();
#endif
}

uint32_t stk::hw::HiResClock::GetFrequency() const
{
#if STK_SUBMICORSECOND_PRECISION_TIMER
    const uint32_t freq = HW_CoreClockFrequency();
#else
    const uint32_t freq = HW_MtimeClockFrequency();
#endif
    STK_ASSERT(freq != 0U);
    return freq;
}

#if STK_TLS && !STK_INLINE_TLS
Word stk::hw::GetTls()
{
    return GetContext().GetTls();
}

void stk::hw::SetTls(Word tp)
{
    GetContext().SetTls(tp);
}
#endif // STK_TLS && !STK_INLINE_TLS

#endif // _STK_ARCH_RISC_V
