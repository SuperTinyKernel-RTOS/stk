/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

// note: If missing, this header must be customized (get it in the root of the source folder) and
//       copied to the /include folder manually.
#include "stk_config.h"

#ifdef _STK_ARCH_ARM_CORTEX_M

#ifdef _STK_CORTEX_M_TRUSTZONE
#include <arm_cmse.h>
#endif

#include "stk.h"
#include "stk_arch.h"
#include "arch/stk_arch_common.h"
         
using namespace stk;

//! Do sanity check for a compiler define, __CORTEX_M must be defined.
#ifndef __CORTEX_M
#error Expecting __CORTEX_M with value corresponding to Cortex-M model (0, 3, 4, ...)!
#endif

//! Driver expects at least SysTick presence.
#if !defined(SysTick)
    #error "SysTick peripheral definition is missing"
#endif

//! Driver expects DWT when tickless mode is enabled and STK_TICKLESS_USE_ARM_DWT=1.
#if (__CORTEX_M > 1U) && STK_TICKLESS_USE_ARM_DWT && !defined(DWT)
    #error "DWT peripheral definition is missing"
#endif

//! If (1) then code assumes FPU presence on CPU which is used by the compiler.
#define STK_CORTEX_M_FPU ((__FPU_PRESENT == 1U) && (__FPU_USED == 1U))

//! If (1), manage Link Register (LR) per task.
#define STK_CORTEX_M_MANAGE_LR (__CORTEX_M >= 3U)

//! Exception return token.
#define STK_CORTEX_M_EXC_RETURN_THREAD_PSP (0xFFFFFFFDU) // Thread mode, PSP, basic

// ISR priorities:
#define STK_CORTEX_M_ISR_PRIORITY_HIGHEST  (0U)
#define STK_CORTEX_M_ISR_PRIORITY_LOWEST   (0xFFU)

//! Number of registers kept in stack.
#if STK_CORTEX_M_MANAGE_LR
    #define STK_CORTEX_M_REGISTER_COUNT (17U)
#else
    #define STK_CORTEX_M_REGISTER_COUNT (16U)
#endif

//! Additional words pushed onto the stack when TrustZone is active.
//! Layout (low -> high address, pushed last -> first):
//!   [0] PSPLIM    – Secure Process Stack Pointer Limit
//!   [1] PSPLIM_NS – Non-Secure Process Stack Pointer Limit
//!   [2] PSP_NS    – Non-Secure Process Stack Pointer (mid-execution value)
//!   [3] CONTROL_NS – Non-Secure CONTROL register (nPRIV, SPSEL)
#ifdef _STK_CORTEX_M_TRUSTZONE
    #define STK_CORTEX_M_TZ_REGISTER_COUNT (4U)
#else
    #define STK_CORTEX_M_TZ_REGISTER_COUNT (0U)
#endif

//! Total words reserved at the top of each task stack.
#define STK_CORTEX_M_TOTAL_REGISTER_COUNT \
    (STK_CORTEX_M_REGISTER_COUNT + STK_CORTEX_M_TZ_REGISTER_COUNT)

//! SysTick_Handler
#ifndef STK_SYSTICK_HANDLER
    #define STK_SYSTICK_HANDLER SysTick_Handler
#endif

//! PendSV_Handler
#ifndef STK_PENDSV_HANDLER
    #define STK_PENDSV_HANDLER PendSV_Handler
#endif

//! SVC_Handler
#ifndef STK_SVC_HANDLER
    #define STK_SVC_HANDLER SVC_Handler
#endif
      
// Inline ASM helpers:
#ifdef __ICCARM__
    #define STK_ASM_SYNTAX_UNIFIED      /* IAR: not needed, unified is default */
    #define STK_ASM_POOL                /* IAR: not needed, handled automatically */
    #define STK_ASM_ALIGN_2             /* IAR: not needed */
#else
    #define STK_ASM_SYNTAX_UNIFIED      ".syntax unified             \n"
    #define STK_ASM_POOL                ".pool                       \n"
    #define STK_ASM_ALIGN_2             ".align 2                    \n"
#endif

//! SVC commands.
enum ESvcCommandId : uint8_t
{
    SVC_START_SCHEDULING = 0U,
    SVC_ENTER_CRITICAL,
    SVC_EXIT_CRITICAL,
    //SVC_FORCE_SWITCH
};

//! ARM Cortex-M Program Counter (PC) register helpers.
namespace stk::hw::reg::PC {

//! Mask to isolate the Thumb state bit (Bit 0) / Halfword alignment mask.
enum class ERegMask : Word
{
    MASK_THUMB_BIT = (1U << 0U) //!< Bit 0: Must be 1 for execution, 0 for word alignment checks
};

//! Clear the LSB to ensure strict halfword instruction alignment.
static inline Word ClearThumbBit(Word REG_PC) noexcept
{
    return (REG_PC & ~static_cast<Word>(ERegMask::MASK_THUMB_BIT));
}

} // namespace stk::hw::reg::PC

//! ARM Cortex-M Program Status Register (xPSR / EPSR) definitions.
namespace stk::hw::reg::XPSR {

//! Default initial value.
constexpr Word DEFAULT_INIT = 0U;

//! xPSR/EPSR register bits.
enum class ERegMask : Word
{
    MASK_T_BIT = (1U << 24U) //!< Bit 24: Thumb state execution bit (Must be 1)
};

//! Enable Thumb state execution by setting the T bit.
static inline Word SetThumbExecution(Word REG_XPSR) noexcept
{
    return (REG_XPSR | static_cast<Word>(ERegMask::MASK_T_BIT));
}

} // namespace stk::hw::reg::XPSR

//! ARM Cortex-M CONTROL/CONTROL_NS register.
namespace stk::hw::reg::CONTROL {

//! Default initial value.
constexpr Word DEFAULT_INIT = 0U;

//! CONTROL register bits.
enum class ERegMask : Word
{
    MASK_nPRIV = (1U << 0), //!< Bit 0: 0 = Privileged, 1 = Unprivileged
    MASK_SPSEL = (1U << 1), //!< Bit 1: 0 = MSP, 1 = PSP
    MASK_FPCA  = (1U << 2), //!< Bit 2: Floating-point context active
    MASK_SFPA  = (1U << 3)  //!< Bit 3: Secure Floating-point active
};

//! Make unprivileged.
static inline Word SetUnprivileged(Word REG_CONTROL) noexcept
{
    return (REG_CONTROL | static_cast<Word>(ERegMask::MASK_nPRIV));
}

//! Make privileged.
static inline Word SetPrivileged(Word REG_CONTROL) noexcept
{
    return (REG_CONTROL & ~static_cast<Word>(ERegMask::MASK_nPRIV));
}

//! Set Stack Pointer selection to PSP.
static inline Word SetSPSelectionToPSP(Word REG_CONTROL) noexcept
{
    return (REG_CONTROL | static_cast<Word>(ERegMask::MASK_SPSEL));
}

} // namespace stk::hw::reg::CONTROL

#if defined(_STK_CORTEX_M_TRUSTZONE) && (STK_CORTEX_M_TZ_REGISTER_COUNT != 0U)
/*! \\struct TrustZoneFrame
    \\brief  Per-task TrustZone register snapshot saved on every context switch.

    All four words are pushed onto the Secure PSP stack BELOW the callee-saved
    register block (r4-r11, LR) by PendSV and restored before popping them.
    This makes PSP_S the single spine for the entire context frame, so the
    save/restore path never needs to branch on EXC_RETURN S-bit.

    In-memory layout (low address -> high address, i.e. lowest SP offset -> highest):
      [SP+0]  PSP_NS     – pushed last (second STMDB), popped first (first LDMIA)
      [SP+4]  CONTROL_NS
      [SP+8]  PSPLIM     – pushed first (first STMDB), popped last (second LDMIA)
      [SP+12] PSPLIM_NS

    Push sequence in PendSV (STMDB decrements then stores, lowest reg at lower addr):
      STMDB r0!, {PSPLIM, PSPLIM_NS}   -> SP+8, SP+12
      STMDB r0!, {PSP_NS, CONTROL_NS}  -> SP+0, SP+4  ← new stack top

    Member declaration order matches struct byte offsets so that InitStack can
    use hw::WordToPtr<TrustZoneFrame>(stack->SP)->field directly.
*/
struct TrustZoneFrame
{
    Word PSP_NS;     //!< Non-Secure PSP (saved mid-execution value).      SP+0
    Word CONTROL_NS; //!< Non-Secure CONTROL register (nPRIV, SPSEL).      SP+4
    Word PSPLIM;     //!< Secure PSPLIM register value.                    SP+8
    Word PSPLIM_NS;  //!< Non-Secure PSPLIM register value.                SP+12
};
#define STK_CORTEX_M_TRUSTZONE_FRAME (1)
#else
#define STK_CORTEX_M_TRUSTZONE_FRAME (0)
#endif

/*! \struct ExceptionFrame
    \brief  ARMv7-M hardware exception frame (8 words, highest address on the stack).
*/
struct ExceptionFrame
{
    Word R0;
    Word R1;
    Word R2;
    Word R3;
    Word R12;
    Word LR;
    Word PC;
    Word PSR;
};

/*! \struct TaskFrame
    \brief  Full initial task frame laid out at the top of a new stack.

    On Cortex-M3+ the hardware expects an EXC_RETURN value in LR immediately
    below the exception frame in the callee-saved register block.
    Grouping it here eliminates all raw-index arithmetic from InitStack.

    Member order is low-to-high address, matching downward stack growth:
    EXC_RETURN (lower) sits one word below exc (higher), exactly where
    OnTaskStart's LDMIA expects it.
*/
struct TaskFrame
{
#if STK_CORTEX_M_MANAGE_LR
    Word           EXC_RETURN; //!< Exception return value (LR), loaded by LDMIA in OnTaskStart.
#endif
    ExceptionFrame exc;        //!< ARMv7-M hardware exception frame.
};

// Shortcuts:
#define STK_ASM_EXIT_FROM_HANDLER  "BX LR"  // use in naked exception/ISR handlers
#define STK_ASM_EXIT_FROM_FUNCTION "BX LR"  // use in naked C-callable wrapper functions
#define STK_ASM_DISABLE_INTERRUPTS "CPSID i"
#define STK_ASM_ENABLE_INTERRUPTS  "CPSIE i"

// Local static functions:
static void OnTaskRun(ITask *runnable);
static void OnTaskExit();
static void OnSchedulerSleep();
static void OnSchedulerSleepOverride();
static void OnSchedulerExit();

/*! \brief     Start scheduler.
    \note      Triggered via SVC. This will transition CPU to SVC Handler.
*/
static __stk_forceinline void HW_StartScheduler()
{
    __asm volatile("SVC %0"
    : /* output: none */
    : "I"(SVC_START_SCHEDULING)
    : "memory" /* protect against compiler reordering */ );
}

/*! \brief     Force an immediate context switch.
*/
#ifdef SVC_FORCE_SWITCH
static __stk_forceinline void HW_ForceContextSwitch()
{
    __asm volatile("SVC %0"
    : /* output: none */
    : "I"(SVC_FORCE_SWITCH)
    : "memory" /* protect against compiler reordering */ );
}
#endif

/*! \brief     Enter critical section (unprivileged callers only).
    \details   Issues SVC_ENTER_CRITICAL, transferring control to SVC_Handler_Main.
               The handler reads the current BASEPRI, raises it to mask all
               configurable-priority interrupts, then writes the saved value back
               into the stacked R0 slot of the exception frame. When the SVC returns,
               the hardware restores that R0 to the register, making the saved BASEPRI
               appear as this function's return value per AAPCS.
    \note      \b Naked function. The compiler emits no prologue or epilogue.
               A prologue would push registers onto the PSP before the SVC, shifting
               the exception frame the handler expects, causing it to write the saved
               BASEPRI to the wrong stack slot and corrupting the return value.
               \c STK_ASM_EXIT_FROM_FUNCTION replaces the compiler-generated return.
    \note      Unprivileged thread mode only. In handler mode or privileged thread
               mode use Context::EnterCriticalSection() directly to avoid the SVC overhead.
    \return    Previous value of BASEPRI, restored from the stacked R0 by the hardware
               on SVC return.
    \see       HW_SVCExitCritical, SVC_Handler_Main
*/
__stk_attr_naked Word HW_SVCEnterCritical()
{
    __asm volatile(
    "SVC %0                      \n"
    STK_ASM_EXIT_FROM_FUNCTION " \n"
    : /* output: none */
    : "I"(SVC_ENTER_CRITICAL)
    : "memory" /* protect against compiler reordering */ );
    
#ifdef __ICCARM__
#pragma diag_suppress=Pe940
    // return value is passed in r0 by the SVC handler per AAPCS;
    // IAR cannot see this through the naked asm, suppress the warning.
    return 0U;
#pragma diag_default=Pe940
#endif
}

/*! \brief     Exit critical section (unprivileged callers only).
    \details   Passes \a prev in R0 per AAPCS, then issues SVC_EXIT_CRITICAL.
               SVC_Handler_Main reads R0 directly from the stacked exception frame
               and restores BASEPRI to that value, re-enabling interrupts at the
               priority level saved by the matching HW_SVCEnterCritical() call.
    \note      \b Naked function. The compiler emits no prologue or epilogue.
               A prologue would push registers and adjust SP before the SVC fires,
               placing R0 (carrying \a prev) at the wrong offset in the exception
               frame. The handler would then restore BASEPRI from garbage, leaving
               interrupts permanently masked or unmasked incorrectly.
               \c STK_ASM_EXIT_FROM_FUNCTION replaces the compiler-generated return.
    \note      Unprivileged thread mode only. Must be paired with a preceding
               HW_SVCEnterCritical() call on the same thread. Passing a \a prev value
               not obtained from HW_SVCEnterCritical() produces undefined behaviour.
    \param[in] prev: Previous BASEPRI value returned by the matching HW_SVCEnterCritical().
    \see       HW_SVCEnterCritical, SVC_Handler_Main
*/
__stk_attr_naked void HW_SVCExitCritical(Word /*prev*/)
{
    __asm volatile(
    "SVC %0                      \n"
    STK_ASM_EXIT_FROM_FUNCTION " \n"
    : /* output: none */
    : "I"(SVC_EXIT_CRITICAL)
    : "memory" /* protect against compiler reordering */);
}

/*! \brief     Disable CPU interrupts.
*/
static __stk_forceinline void HW_DisableInterrupts()
{
#if (defined(__clang__) && defined(__ARMCOMPILER_VERSION)) || defined(__ICCARM__)
    __asm volatile(STK_ASM_DISABLE_INTERRUPTS ::: "memory");
#else
    __disable_irq();
#endif
}

/*! \brief     Enable CPU interrupts.
*/
static __stk_forceinline void HW_EnableInterrupts()
{
#if (defined(__clang__) && defined(__ARMCOMPILER_VERSION)) || defined(__ICCARM__)
    __asm volatile(STK_ASM_ENABLE_INTERRUPTS ::: "memory");
#else
    __enable_irq();
#endif
}

/*! \brief     Check if interrupts are disabled.
    \return    true if interrupts are disabled (PRIMASK bit 0 is set).
*/
__stk_attr_unused
static __stk_forceinline bool HW_InterruptsDisabled()
{
#if (defined(__clang__) && defined(__ARMCOMPILER_VERSION)) || defined(__ICCARM__)
    Word primask;
    __asm volatile("MRS %0, primask" : "=r"(primask));
    return ((primask & 1U) != 0U);
#else
    return ((__get_PRIMASK() & 1U) != 0U);
#endif
}

/*! \brief     Enter critical section.
    \return    Session value which has to be supplied to HW_ExitCriticalSection().
*/
static __stk_forceinline void HW_CriticalSectionStart(uint32_t &SES)
{
    SES = __get_PRIMASK();
    HW_DisableInterrupts();

    // ensure the disable is recognized before subsequent code
    __DSB();
    __ISB();
}

/*! \brief     Exit critical section.
    \param[in] ses: Session value obtained by HW_EnterCriticalSection().
*/
static __stk_forceinline void HW_CriticalSectionEnd(uint32_t SES)
{
    // ensure all memory work is finished before re-enabling
    __DSB();

    __set_PRIMASK(SES);

    // synchronization point: any pending interrupt can be serviced immediately at this boundary
    __ISB();
}

/*! \brief     Enter low-power/sleep mode.
*/
static __stk_forceinline void HW_EnterSleepMode()
{
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk; // disable deep-sleep, go into a WAIT mode (sleep)
    __DSB();                            // ensure store takes effect (see ARM info)
    __WFI();
}

// IAR wrappers for: __atomic_test_and_set, __atomic_clear.
#ifdef __ICCARM__
    #include <xatomic.h>

    #define __ATOMIC_RELAXED __MEMORY_ORDER_RELAXED__
    #define __ATOMIC_CONSUME __MEMORY_ORDER_CONSUME__
    #define __ATOMIC_ACQUIRE __MEMORY_ORDER_ACQUIRE__
    #define __ATOMIC_RELEASE __MEMORY_ORDER_RELEASE__
    #define __ATOMIC_ACQ_REL __MEMORY_ORDER_ACQ_REL__
    #define __ATOMIC_SEQ_CST __MEMORY_ORDER_SEQ_CST__

    static __stk_forceinline bool __atomic_test_and_set(volatile bool *ptr, int memorder)
    {
    #ifdef __ICCARM__
      STK_STATIC_ASSERT(sizeof(std::__iar_atomic_flag) == sizeof(bool));
    #endif
      
        return std::__iar_atomic_flag_test_and_set(ptr, memorder);
    }

    static __stk_forceinline void __atomic_clear(volatile bool *ptr, int memorder)
    {
    #ifdef __ICCARM__
      STK_STATIC_ASSERT(sizeof(std::__iar_atomic_flag) == sizeof(bool));
    #endif
      
        std::__iar_atomic_flag_clear(ptr, memorder);
    }
#endif

#ifdef CONTROL_nPRIV_Msk
/*! \brief     Attempt to acquire a spin-lock without blocking (M3/M4/M7).
    \details   Uses a GCC built-in atomic test-and-set with acquire memory ordering,
               mapping to an LDREX/STREX sequence on ARMv7-M. If the lock byte was
               already set the operation fails immediately without modifying state.
    \param[in] lock: Spin-lock state variable. Must be \c false (released) initially.
    \retval    true  Lock was free and has been acquired by the caller.
    \retval    false Lock was already held, caller must retry or back off.
    \note      ISR-safe. May be called from both thread and handler mode.
    \note      Pairs with HW_SpinLockUnlock(). Never call HW_SpinLockUnlock() without
               a preceding successful HW_SpinLockTryLock() or HW_SpinLockLock().
    \see       HW_SpinLockLock, HW_SpinLockUnlock
*/
static __stk_forceinline bool HW_SpinLockTryLock(volatile bool &lock)
{     
    return !__atomic_test_and_set(&lock, __ATOMIC_ACQUIRE);
}

/*! \brief     Release a spin-lock (M3/M4/M7).
    \details   Issues a \c dmb ishst barrier to flush all store-buffer writes (including
               scheduler metadata modified under the lock) to the point-of-coherency
               before clearing the lock byte with a GCC atomic clear at release ordering.
               This ensures that any core or bus master that subsequently acquires the
               lock observes all writes made while the lock was held.
    \param[in] lock: Spin-lock state variable previously acquired by the caller.
    \warning   Caller must own the lock. Releasing an unowned lock is an unrecoverable
               error and triggers STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK).
    \note      ISR-safe.
    \see       HW_SpinLockTryLock, HW_SpinLockLock
*/
static __stk_forceinline void HW_SpinLockUnlock(volatile bool &lock)
{ 
    if (lock == false)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK); // release attempt of unowned lock
    }

    // ensure all data writes (like scheduling metadata) are flushed before the lock is released:
    // __atomic_clear with __ATOMIC_RELEASE provides the required store-release barrier,
    // the explicit dmb ishst is retained for toolchains that do not lower __ATOMIC_RELEASE
    // to a full DMB on ARMv7-M (e.g. older GCC versions with -mcpu=cortex-m4)
#ifndef __ICCARM__
    __asm volatile("dmb ishst" ::: "memory");
#endif

    __atomic_clear(&lock, __ATOMIC_RELEASE);
}
#elif defined(RP2040_H)
// Raspberry RP2040 dual-core M0+ implementation, using Hardware Spinlock 0 (SIO base 0xd0000000 + offset)
#define STK_SIO_SPINLOCK SIO->SPINLOCK31

/*! \brief     Attempt to acquire a spin-lock without blocking (RP2040 dual-core M0+).
    \details   Reads SIO hardware STK_SIO_SPINLOCK. On RP2040 the SIO spinlock register
               returns a non-zero value exactly once per acquisition attempt, a zero
               return means another core holds it. On success the software \a lock flag
               is set to \c true and a full DMB is issued to order all subsequent
               memory accesses behind the acquire point.
    \param[in] lock: Software spin-lock state variable. Reflects lock ownership for
               the local core, the hardware SIO spinlock arbitrates between cores.
    \retval    true  Hardware spinlock was free and has been acquired by the caller.
    \retval    false Hardware spinlock was already held by the other core.
    \note      ISR-safe.
    \note      Uses SIO STK_SIO_SPINLOCK. Do not use STK_SIO_SPINLOCK anywhere else
               in the application.
    \see       HW_SpinLockLock, HW_SpinLockUnlock
*/
static __stk_forceinline bool HW_SpinLockTryLock(volatile bool &lock)
{
    bool success = (STK_SIO_SPINLOCK == 0 ? false : ((lock) = true, true));
    __DMB();

    return success;
}

/*! \brief     Release a spin-lock (RP2040 dual-core M0+).
    \details   Issues a DMB to drain pending stores, clears the software \a lock flag,
               then writes any value to SIO STK_SIO_SPINLOCK to release the hardware spinlock.
               The hardware register must be written last: once released, the other core
               may immediately acquire and begin modifying shared state, so all local
               writes must be complete and visible before that point.
    \param[in] lock: Software spin-lock state variable previously acquired by the caller.
    \warning   Caller must own the lock. Releasing an unowned lock is an unrecoverable
               error and triggers STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK).
               Only the software \a lock flag is verified at runtime, the hardware SIO
               spinlock state is not independently checked. Both layers must be consistent:
               a bug that sets \a lock without acquiring STK_SIO_SPINLOCK, or acquires
               STK_SIO_SPINLOCK without setting \a lock, will bypass this guard silently.
    \note      ISR-safe.
    \see       HW_SpinLockTryLock, HW_SpinLockLock
*/
static __stk_forceinline void HW_SpinLockUnlock(volatile bool &lock)
{
    if (!lock)
        STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK); // release attempt of unowned lock

    __DMB();
    (lock) = false;
    STK_SIO_SPINLOCK = 1; // writing any value releases the hardware lock
}

#undef STK_SIO_SPINLOCK
#else // !RP2040_H
// Standard single-core Cortex-M0 implementation:

/*! \brief     Attempt to acquire a spin-lock without blocking (Cortex-M0, single-core).
    \details   On Cortex-M0/M0+ there is no LDREX/STREX, so atomicity is achieved by
               raising a critical section (PRIMASK) around the test-and-set. If the lock
               is already held the critical section is released immediately and the
               function returns false with no side-effects. On success a DMB is issued
               before releasing the critical section to ensure the lock acquisition is
               visible to any bus master before the caller proceeds.
    \param[in] lock: Spin-lock state variable. Must be \c false (released) initially.
    \retval    true  Lock was free and has been acquired by the caller.
    \retval    false Lock was already held, caller must retry or back off.
    \note      ISR-safe, but temporarily disables interrupts during the test-and-set.
    \see       HW_SpinLockLock, HW_SpinLockUnlock
*/
static __stk_forceinline bool HW_SpinLockTryLock(volatile bool &lock)
{
    uint32_t ses;
    HW_CriticalSectionStart(ses);

    if (lock)
    {
        HW_CriticalSectionEnd(ses);
        return false;
    }

    lock = true;
    __DMB();

    HW_CriticalSectionEnd(ses);
    return true;
}

/*! \brief     Release a spin-lock (Cortex-M0, single-core).
    \details   Issues a DMB to ensure all writes made while the lock was held are
               visible to the bus before clearing the lock flag. No critical section is
               needed for the store itself since a single-byte write on Cortex-M0 is
               inherently atomic with respect to the local core.
    \param[in] lock: Spin-lock state variable previously acquired by the caller.
    \warning   Caller must own the lock. Releasing an unowned lock is an unrecoverable
               error and triggers STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK).
    \note      ISR-safe.
    \see       HW_SpinLockTryLock, HW_SpinLockLock
*/
static __stk_forceinline void HW_SpinLockUnlock(volatile bool &lock)
{
    if (!lock)
        STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK); // release attempt of unowned lock

    __DMB();
    lock = false;
}
#endif // CONTROL_nPRIV_Msk

/*! \brief     Acquire a spin-lock, blocking until it becomes available.
    \details   Calls HW_SpinLockTryLock() in a tight retry loop. On each failed attempt
               \c __stk_relax_cpu() (typically a \c NOP or \c YIELD hint) is executed to
               reduce bus contention before the next try. A countdown of 0xFFFFFF
               iterations is enforced: if the lock has not been acquired by the time the
               counter expires, the kernel invariant has been violated (the lock owner
               exited without releasing) and STK_KERNEL_PANIC() is called with
               \c KERNEL_PANIC_SPINLOCK_DEADLOCK.
    \param[in] lock: Spin-lock state variable. Must be \c false (released) initially.
    \note      ISR-safe. The timeout is a fixed iteration count, not a wall-clock duration,
               effective timeout window varies with CPU frequency and bus load.
    \warning   Must not be called while already holding the same \a lock - the M0 fallback
               implementation uses a critical section internally and will deadlock.
    \see       HW_SpinLockTryLock, HW_SpinLockUnlock
*/
static __stk_forceinline void HW_SpinLockLock(volatile bool &lock)
{
    uint32_t timeout = 0xFFFFFFU;
    while (!HW_SpinLockTryLock(lock))
    {
        if (--timeout == 0U)
        {
            // invariant violated: the lock owner exited without releasing
            STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK);
        }
        __stk_relax_cpu();
    }
}

/*! SaveJmp/RestoreJmp ----------------------------------------------------------

    ARM Cortex-M callee-saved registers per the AAPCS ABI:
      r4-r11, sp (r13), lr (r14)

    Both functions are naked so the compiler emits no prologue/epilogue:
      - SaveJmp captures the *caller's* SP and LR before any frame adjustment.
      - RestoreJmp reloads everything and jumps directly to the saved LR, making
        SaveJmp's caller see a non-zero return value (val) as if SaveJmp returned
        a second time.

    Cortex-M0/M0+/M1 (Thumb-1 only):
      STR/LDR with r8-r11/sp/lr cannot be encoded in 16-bit Thumb-1.
      High registers are moved into r1/r2 first, then stored via r0 base.
      SP and LR are also moved into low registers before store/load.

    If FPU is present (STK_CORTEX_M_FPU != 0), FPSCR is also saved/restored
    to preserve the caller's rounding mode and exception flags across the jump.
    The callee-saved VFP data registers (d8-d15 / s16-s31) are NOT saved here -
    the ABI already guarantees they survive any normal call boundary.

    r0 = &f  (first argument)
    r1 = val (second argument, RestoreJmp only)
*/

/*! \struct JmpFrame
    \brief  Callee-saved CPU register snapshot used by SaveJmp() and RestoreJmp().
    \note   Layout matches the AAPCS callee-saved register set: r4-r11, sp, lr.
            If an FPU is present, FPSCR is appended to preserve the caller's
            floating-point rounding mode and exception flags across the jump.
            VFP data registers (d8-d15 / s16-s31) are intentionally excluded -
            the ABI guarantees they survive any normal call boundary.
    \see    SaveJmp, RestoreJmp
*/
struct JmpFrame
{
    Word R4, R5, R6, R7, R8, R9, R10, R11; //!< Callee-saved general-purpose registers.
    Word SP;    //!< Stack pointer of the SaveJmp call site (r13).
    Word LR;    //!< Return address of the SaveJmp call site (r14).
#if STK_CORTEX_M_FPU
    Word FPSCR; //!< Floating-point status and control register (rounding mode + exception flags).
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
__stk_attr_naked
int32_t SaveJmp(JmpFrame &/*f*/)
{
    __asm volatile(
    STK_ASM_SYNTAX_UNIFIED
      
#if (__CORTEX_M >= 3U)
    // Cortex-M3/M4/M7: STMIA stores r4-r11 at r0+0 .. r0+28
    "STMIA r0,  {r4-r11}            \n" // store r4-r11 at offsets 0-28, no writeback
    "STR   sp,  [r0, #32]           \n" // SP at offset 32
    "STR   lr,  [r0, #36]           \n" // LR at offset 36
#else
    // Cortex-M0/M0+/M1: Thumb-1 only
    "STR  r4,  [r0, #0]             \n"
    "STR  r5,  [r0, #4]             \n"
    "STR  r6,  [r0, #8]             \n"
    "STR  r7,  [r0, #12]            \n"
    "MOV  r1,  r8                   \n"
    "STR  r1,  [r0, #16]            \n"
    "MOV  r1,  r9                   \n"
    "STR  r1,  [r0, #20]            \n"
    "MOV  r1,  r10                  \n"
    "STR  r1,  [r0, #24]            \n"
    "MOV  r1,  r11                  \n"
    "STR  r1,  [r0, #28]            \n"
    "MOV  r1,  sp                   \n"
    "STR  r1,  [r0, #32]            \n"
    "MOV  r1,  lr                   \n"
    "STR  r1,  [r0, #36]            \n"
#endif

#if STK_CORTEX_M_FPU
    "VMRS r1,  FPSCR                \n"
    "STR  r1,  [r0, #40]            \n"
#endif
    "MOVS r0,  #0                   \n"
    "BX   lr                        \n");
    
#ifdef __ICCARM__
#pragma diag_suppress=Pe940
    // return value is passed in r0 by the SVC handler per AAPCS;
    // IAR cannot see this through the naked asm, suppress the warning.
    return 0;
#pragma diag_default=Pe940
#endif
}

/*! \brief     Restore callee-saved CPU registers from a JmpFrame and jump back
               to the SaveJmp() call site.
    \param[in] f: Frame previously populated by SaveJmp().
    \param[in] val: Value that SaveJmp() will appear to return at the restored
               call site. Should be non-zero to distinguish a restore from
               an original save.
    \note      Naked noreturn function - execution transfers directly to the
               saved LR; this function never returns to its own caller.
    \note      MISRA deviation: [STK-DEV-003] Rule 7-5-1, 7-5-2, 6-6-4
               (__attribute__((naked))). Required to restore SP and branch to
               the saved return address without any compiler-generated epilogue
               that would corrupt the restored stack state.
    \note      Pair with SaveJmp().
    \warning   Undefined behavior if \a f was not previously initialized by
               a matching SaveJmp() call on the same stack.
    \see       SaveJmp
*/
__stk_attr_naked
void RestoreJmp(JmpFrame &/*f*/, int32_t /*val*/)
{
    __asm volatile(
    STK_ASM_SYNTAX_UNIFIED
      
#if (__CORTEX_M >= 3U)
    // Cortex-M3/M4/M7: LDMIA loads r4-r11 from offsets 0-28
    "LDR   sp,  [r0, #32]           \n" // restore SP
#if STK_CORTEX_M_FPU
    "LDR   r2,  [r0, #40]           \n" // load saved FPSCR
    "VMSR  FPSCR, r2                \n" // restore rounding mode + flags
#endif
    "LDR   r2,  [r0, #36]           \n" // load saved LR into r2
    "LDMIA r0,  {r4-r11}            \n" // restore r4-r11, no writeback
    "MOV   r0,  r1                  \n" // return val
    "BX    r2                       \n"
#else
    // Cortex-M0/M0+/M1: Thumb-1 only
    "LDR  r2,  [r0, #36]            \n"
    "MOV  lr,  r2                   \n"
    "LDR  r2,  [r0, #32]            \n"
    "MOV  sp,  r2                   \n"
    "LDR  r2,  [r0, #28]            \n"
    "MOV  r11, r2                   \n"
    "LDR  r2,  [r0, #24]            \n"
    "MOV  r10, r2                   \n"
    "LDR  r2,  [r0, #20]            \n"
    "MOV  r9,  r2                   \n"
    "LDR  r2,  [r0, #16]            \n"
    "MOV  r8,  r2                   \n"
    "LDR  r4,  [r0, #0]             \n"
    "LDR  r5,  [r0, #4]             \n"
    "LDR  r6,  [r0, #8]             \n"
    "LDR  r7,  [r0, #12]            \n"
    "MOV  r0,  r1                   \n"  // return val
    "BX   lr                        \n"
#endif
    );
}

// -----------------------------------------------------------------------------

/*! \brief  Get current exception (not 0 if inside ISR).
*/
static __stk_forceinline Word HW_GetCurrentException()
{
    return __get_IPSR();
}

/*! \brief  Check if caller is in Handler Mode (IPSR != 0), i.e. inside ISR.
*/
static __stk_forceinline bool HW_IsHandlerMode()
{
    return (HW_GetCurrentException() != 0U);
}

/*! \brief  Check if caller is in Privileged Thread Mode (nPRIV == 0).
    \note   ARM Cortex-M0 is always Privileged Thread Mode (M0 does not support privileges).
*/
static __stk_forceinline bool HW_IsPrivilegedThreadMode()
{
#ifdef CONTROL_nPRIV_Msk
    return ((__get_CONTROL() & CONTROL_nPRIV_Msk) == 0U);
#else
    return true;
#endif
}

/*! \brief  Get SP of the calling process.
    \return SP register value.
*/
static __stk_forceinline Word HW_GetCallerSP()
{
    // use SP (R13) directly: __get_PSP() returns 0 in unprivileged thread mode
    Word sp;
    __asm volatile("MOV %0, sp" : "=r" (sp));
    return sp;
}

/*! \brief Schedule context switch via the PendSV interrupt.
*/
static __stk_forceinline void HW_ScheduleContextSwitch()
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

/*! \brief Clear FPU state.
*/
static __stk_forceinline void HW_ClearFpuState()
{
#if STK_CORTEX_M_FPU
    __set_CONTROL(__get_CONTROL() & ~CONTROL_FPCA_Msk);
#endif
}

/*! \brief Enable FPU.
*/
static __stk_forceinline void HW_EnableFullFpuAccess()
{
#if STK_CORTEX_M_FPU
    // Enable FPU CP10/CP11 Secure register access.
    SCB->CPACR |= (static_cast<uint32_t>(0b11) << 20U) | (static_cast<uint32_t>(0b11) << 22U);

#if defined(_STK_CORTEX_M_TRUSTZONE)
    // Grant Non-Secure state access to the FPU CP10/CP11.
    SCB->NSACR |= (static_cast<uint32_t>(0b11) << 10U);

    // Enable FPU CP10/CP11 Non-Secure register access.
    SCB_NS->CPACR |= (static_cast<uint32_t>(0b11) << 20U) | (static_cast<uint32_t>(0b11) << 22U);
    __DSB();
    __ISB();

    // Lazy stacking for Secure and Non-Secure states.
    FPU->FPCCR |= FPU_FPCCR_TS_Msk | FPU_FPCCR_LSPEN_Msk | FPU_FPCCR_LSPENS_Msk;
    __DSB();
    __ISB();
#endif

    // Allow Unprivileged (User mode) access to the FPU registers/FPSCR.
    FPU->FPCCR |= (1UL << FPU_FPCCR_USER_Pos);
    __DSB();
    __ISB();
#endif
}

/*! \brief  Get core clock frequency.
    \return Frequency in Hz.
*/
static __stk_forceinline uint32_t HW_CoreClockFrequency()
{
    return SystemCoreClock;
}

/*! \brief Clear pending switch by PendSV exception.
*/
static __stk_forceinline void HW_ClearPendingSwitch()
{
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk;
}

/*! \brief Start SysTick timer peripheral.
*/
static __stk_forceinline void HW_SysTickStart(uint32_t period_ticks)
{
    const uint32_t result = SysTick_Config(static_cast<uint32_t>(ConvertTimeUsToClockCycles(HW_CoreClockFrequency(), period_ticks)));
    STK_ASSERT(result == 0U);
    STK_UNUSED(result);

    // QEMU workaround (Launchpad Bug #1872237):
    // SysTick_Config() writes VAL=0 before setting ENABLE=1. On QEMU,
    // systick_reload() silently discards VAL writes while ENABLE=0, leaving
    // the internal tick accumulator stale from the previous period.
    // Writing VAL=0 here, after ENABLE=1 is already set, forces a correct reload.
    // This is a no-op on real Cortex-M hardware (spec: ARMv7-M ARM B3.3.1).
    SysTick->VAL = 0U;
}

/*! \brief Stop SysTick timer peripheral.
*/
static __stk_forceinline void HW_SysTickStop()
{
    SysTick->CTRL = 0U;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
}

/*! \brief  Pause SysTick timer peripheral.
*/
__stk_attr_unused /* can be unused due to configuration */
static __stk_forceinline void HW_SysTickDisable()
{
    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
}

/*! \brief  Get SysTick current value.
    \return Value of SysTick->VAL register.
*/
static __stk_forceinline uint32_t HW_SysTickValue()
{
    return SysTick->VAL;
}

/*! \brief  Get SysTick value after it was disabled.
    \return Value of SysTick->VAL register.
*/
__stk_attr_unused /* can be unused due to configuration */
static __stk_forceinline uint32_t HW_SysTickValueAfterDisable()
{
    // is required for QEMU which then resets SysTick->VAL and thus we can't calculate elapsed time correctly
    __DSB();

    // check for a QEMU case and discard elapsed result
    uint32_t val = HW_SysTickValue();
    if (val == 0U)
    {
        val = SysTick->LOAD;
    }

    return val;
}

/*! \brief     Get number of elapsed ticks of the current period of SysTick timer peripheral.
    \param[in] val: a value of SysTick->VAL register.
*/
__stk_attr_unused /* can be unused due to configuration */
static __stk_forceinline uint32_t HW_SysTickElapsed(uint32_t val)
{
    return SysTick->LOAD - val;
}

/*! \brief Rearm SysTick timer peripheral with new period.
    \note  SysTick peripheral becomes enabled.
*/
__stk_attr_unused /* can be unused due to configuration */
static __stk_forceinline void HW_SysTickRearm(uint32_t ticks)
{
    SysTick->LOAD  = ticks - 1U;
    SysTick->VAL   = 0U;
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
}

/*! \brief Enable DWT Cycle Counter (CYCCNT).
*/
static __stk_forceinline void HW_DWTEnableCounter()
{
    // Enable Trace and Debug blocks (DWT, ITM, ETM, TPIU)
#if defined(CoreDebug)
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#elif defined(DCB)
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#endif
    __DSB();

    // LAR (Lock Access Register) is mandatory for Cortex-M7 to allow register writes,
    // it is typically not implemented or deprecated on M0, M3, M4, and M33
#if (__CORTEX_M == 7U)
    DWT->LAR = 0xC5ACCE55; // unlock DWT unit using standard CoreSight magic key
    __DSB();
#endif

#if defined(DWT)
    // Do not interfere with already enabled and running counter
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != DWT_CTRL_CYCCNTENA_Msk)
    {
        DWT->CYCCNT = 0U;                     // Reset the counter value to zero
        DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk; // Start the counter
    }
#endif
}

/*! \brief   Get current DWT Counter (CYCCNT) value.
    \return  Counter value.
    \warning Counter value us prone to periodic wrapping.
*/
static __stk_forceinline uint32_t HW_DWTGetCounter()
{
#if defined(DWT)
    return DWT->CYCCNT;
#else
    return 0U;
#endif
}

// Implement these in your ARM TrustZone helper:
extern "C" __stk_weak
Word NS_stk_util_GetOnTaskExitAddr()
{
    return hw::PtrToWord(&OnTaskExit);
}

//! Global lock to synchronize critical sections of multiple cores.
static volatile bool s_StkCortexmCsuLock = false;

//! Internal context.
static struct Context final : public PlatformContext
{
    explicit Context() : PlatformContext(), m_exit_buf(), m_overrider(nullptr),
    #if STK_TICKLESS_IDLE
        m_sleep_ticks(0), m_sleep_error(0U),
    #endif
        m_csu(0U), m_csu_nesting(0U), m_started(false), m_exiting(false)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~Context() = default;

    void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap,
        uint32_t resolution_us) override
    {
        PlatformContext::Initialize(handler, service, exit_trap, resolution_us);

        STK_STATIC_ASSERT_DESC_N(SP, offsetof(Stack, SP) == 0U,
            "expect Stack::mode member at offset of 0 (first member)");
        STK_STATIC_ASSERT_DESC_N(mode, offsetof(Stack, access_mode) == 4U,
            "expect Stack::mode member at offset of 4 (second member)");        

        m_csu         = 0U;
        m_csu_nesting = 0U;
        m_started     = false;
        m_exiting     = false;
    #if STK_TICKLESS_IDLE
        m_sleep_ticks = 0;
        m_sleep_error = 0U;
    #endif

    #if (__CORTEX_M > 1) && STK_TICKLESS_USE_ARM_DWT
        HW_DWTEnableCounter();
    #endif

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_Init(
                HW_CoreClockFrequency(),
                HW_CoreClockFrequency(),
                nullptr,
                SendSysDesc);
    #endif
    }

#if STK_TICKLESS_IDLE
    __stk_forceinline void OnTick(Timeout &ticks)
#else
    __stk_forceinline void OnTick()
#endif
    {
        if (m_handler->OnTick(m_stack_idle, m_stack_active
        #if STK_TICKLESS_IDLE
            , ticks
        #endif
        ))
        {
        #if STK_SEGGER_SYSVIEW
            SEGGER_SYSVIEW_OnTaskStopExec();
            if (GetContext().m_stack_active->tid != SYS_TASK_ID_SLEEP)
                SEGGER_SYSVIEW_OnTaskStartExec(GetContext().m_stack_active->tid);
        #endif

            HW_ScheduleContextSwitch();
        }
    }

    __stk_forceinline void ProcessTick()
    {
        HW_DisableInterrupts();

    #if STK_TICKLESS_IDLE
        Timeout ticks = m_sleep_ticks;

        OnTick(ticks);

        // rearm SysTick only if tick period changed
        if (ticks != m_sleep_ticks)
        {
            m_sleep_ticks = ReloadTickPeriod(ticks);
        }
    #else
        OnTick();
    #endif

        HW_EnableInterrupts();
    }

    __stk_forceinline void EnterCriticalSection()
    {
        // disable local interrupts first to prevent self-deadlock
        uint32_t current_ses;
        HW_CriticalSectionStart(current_ses);

        if (m_csu_nesting == 0U)
        {
            // ONLY attempt the global spinlock if we aren't already nested
            HW_SpinLockLock(s_StkCortexmCsuLock);

            // store the hardware interrupt state to restore later
            m_csu = current_ses;
        }

        // increase nesting count within a limit
        if (++m_csu_nesting > STK_CRITICAL_SECTION_NESTINGS_MAX)
        {
            // invariant violated: exceeded max allowed number of recursions
            STK_KERNEL_PANIC(KERNEL_PANIC_CS_NESTING_OVERFLOW);
        }
    }

    __stk_forceinline void ExitCriticalSection()
    {
        STK_ASSERT(m_csu_nesting != 0U);
        --m_csu_nesting;

        if (m_csu_nesting == 0U)
        {
            // capture the state before releasing lock
            const uint32_t ses_to_restore = m_csu;

            // release global lock
            HW_SpinLockUnlock(s_StkCortexmCsuLock);

            // restore hardware interrupts
            HW_CriticalSectionEnd(ses_to_restore);
        }
    }

    // Unprivileged
    __stk_forceinline void UnprivEnterCriticalSection()
    {
        // elevate to privileged/disabled state via SVC
        const Word current_ses = HW_SVCEnterCritical();

        if (m_csu_nesting == 0U)
        {
            // ONLY attempt the global spinlock if we aren't already nested
            HW_SpinLockLock(s_StkCortexmCsuLock);

            // store the hardware interrupt state to restore later
            m_csu = current_ses;
        }

        // increase nesting count within a limit
        if (++m_csu_nesting > STK_CRITICAL_SECTION_NESTINGS_MAX)
        {
            // invariant violated: exceeded max allowed number of recursions
            STK_KERNEL_PANIC(KERNEL_PANIC_CS_NESTING_OVERFLOW);
        }
    }

    // Unprivileged
    __stk_forceinline void UnprivExitCriticalSection()
    {
        STK_ASSERT(m_csu_nesting != 0U);
        --m_csu_nesting;

        if (m_csu_nesting == 0U)
        {
            // capture the state before releasing lock
            const Word ses_to_restore = m_csu;

            // release global lock
            HW_SpinLockUnlock(s_StkCortexmCsuLock);

            // restore hardware interrupts via SVC
            HW_SVCExitCritical(ses_to_restore);
        }
    }

    void StartTickTimer(Timeout elapsed_ticks)
    {
    #if STK_TICKLESS_IDLE
        // Reset sleep ticks if kernel was restarted.
        m_sleep_ticks = elapsed_ticks;
    #else
        STK_UNUSED(elapsed_ticks);
    #endif

        // Start SysTick timer (it is yet can't fire an interrupt due to HW_DisableInterrupts).
        HW_SysTickStart(m_tick_resolution);

        // Note: Always after SysTick_Config because it may change SysTick priority.
        NVIC_SetPriority(SysTick_IRQn, STK_CORTEX_M_ISR_PRIORITY_LOWEST);
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
    
    uint32_t GetTickResolutionInClockCycles()
    {
        return static_cast<uint32_t>(ConvertTimeUsToClockCycles(HW_CoreClockFrequency(), static_cast<Ticks>(m_tick_resolution)));
    }

    void Start();
    void OnStart();
    void OnStop();
#if STK_TICKLESS_IDLE
    Timeout ReloadTickPeriod(Timeout ticks_requested);
    Timeout Suspend();
    void Resume(Timeout elapsed_ticks);
#endif

    typedef IPlatform::IEventOverrider eovrd_t;

    JmpFrame      m_exit_buf;    //!< saved context of the exit point
    eovrd_t      *m_overrider;   //!< platform events overrider
#if STK_TICKLESS_IDLE
    Timeout       m_sleep_ticks; //!< sleep ticks of the current session
    uint32_t      m_sleep_error; //!< sleep error which is accounted in the next sleep
#endif
    Word          m_csu;         //!< user critical session
    uint8_t       m_csu_nesting; //!< depth of user critical session nesting
    volatile bool m_started;     //!< 'true' when in started state
    bool          m_exiting;     //!< 'true' when is exiting the scheduling process
}
s_StkPlatformContext[STK_ARCH_CPU_COUNT];

//! High resolution clock for Cortex-M3+ using DWT peripheral.
class HiResClockDWT
{
    Cycles   m_acc;
    uint32_t m_prev;

public:
    HiResClockDWT() : m_acc(0U), m_prev(0U)
    {
        HW_DWTEnableCounter();

        m_prev = HW_DWTGetCounter();
        m_acc  = 0U;
    }

    static HiResClockDWT *GetInstance()
    {
        // keep declaration function-local to allow compiler stripping it from the binary if
        // it is unused by the user code
        static HiResClockDWT clock;
        return &clock;
    }

    void Update()
    {
        const uint32_t current = HW_DWTGetCounter();

        // unsigned subtraction handles the wrap-around perfectly
        const uint32_t delta = current - m_prev;
        m_acc += delta;

        m_prev = current;
    }

    Cycles GetCycles()
    {
        Update();
        return m_acc;
    }

    uint32_t GetFrequency()
    {
        return HW_CoreClockFrequency();
    }
};

//! High resolution clock implementation for Cortex-M0.
class HiResClockM0
{
public:
    static HiResClockM0 *GetInstance()
    {
        // keep declaration function-local to allow compiler stripping it from the binary if
        // it is unused by the user code
        static HiResClockM0 clock;
        return &clock;
    }

    Cycles GetCycles()
    {
        // On M0, combine the coarse OS ticks with the fine-grained SysTick counter
        const Cycles cycles = ConvertTimeUsToClockCycles(HW_CoreClockFrequency(), static_cast<Ticks>(stk::GetTicks() * GetContext().m_tick_resolution));

        const uint32_t val  = HW_SysTickValue(); // down-counter (cycles remaining in current tick)
        const uint32_t load = SysTick->LOAD;     // current reload value

        // total elapsed cycles
        return cycles + static_cast<Cycles>(load - val);
    }

    uint32_t GetFrequency()
    {
        return HW_CoreClockFrequency();
    }
};

#if (__CORTEX_M >= 3U)
    typedef HiResClockDWT HiResClockImpl;
#else
    typedef HiResClockM0 HiResClockImpl;
#endif

//! Panic id cache for post-mortem inspection.
static volatile EKernelPanicId g_LastPanicId = KERNEL_PANIC_NONE;

__stk_attr_noinline  // keep out of inlining to preserve stack frame
__stk_attr_noreturn  // never returns - a trap
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    g_LastPanicId = id;

    // disable all maskable interrupts: this prevents scheduler from running again and corrupting state further
    HW_DisableInterrupts();

    // spin forever: with a watchdog active this produces a clean reset, without a watchdog,
    // a debugger can attach and inspect 'id'
    for (;;)
    {
        __stk_relax_cpu();
    }
}

void PlatformArmCortexM::ProcessTick()
{
    GetContext().ProcessTick();
}

#if STK_TICKLESS_IDLE
Timeout Context::ReloadTickPeriod(Timeout ticks_requested)
{
    const uint32_t SYSTICK_MAX_LOAD = 0x00FFFFFFU; // SysTick LOAD register is 24-bit
    const uint32_t tick_resolution = GetTickResolutionInClockCycles();
    if (tick_resolution == 0U)
    {
        STK_ASSERT(false);
        return NO_WAIT;
    }
        
    // guard against uint32_t overflow in the reload calculation
    STK_ASSERT(static_cast<uint64_t>(ticks_requested) * tick_resolution <= UINT32_MAX);

    // clamp ticks_requested so that cpu_ticks_requested fits into 24-bit SysTick LOAD register
    // without clamping large sleep tick counts silently truncate LOAD, causing the timer to fire far too early
    // breaking the timing
    const Timeout max_ticks = static_cast<Timeout>(SYSTICK_MAX_LOAD / tick_resolution);
    if (ticks_requested > max_ticks)
    {
        ticks_requested = max_ticks;
    }

    // start counting how many CPU cycles further instructions take until SysTick timer is enabled again;
    // without DWT we will have tick error of around 80 cycles depending on CPU model and compiler optimization
#if (__CORTEX_M > 1) && STK_TICKLESS_USE_ARM_DWT
    const uint32_t error = HW_DWTGetCounter();
    __stk_compiler_barrier(); // prevent reordering, we measure all cycles of instructions below this point
#endif

    // pause SysTick
    HW_SysTickDisable();

    // get already elapsed CPU cycles since SysTick ISR invocation up to SysTick timer stop (see above)
    // to account for them for a new period value
    const uint32_t elapsed_till_stop = HW_SysTickElapsed(HW_SysTickValueAfterDisable());

    // OnTick() should not consume more than next period
    STK_ASSERT(static_cast<Timeout>(elapsed_till_stop / tick_resolution) <= static_cast<Timeout>(ticks_requested));

    const uint32_t cpu_ticks_requested = static_cast<uint32_t>(ticks_requested) * tick_resolution;

    // substract number of cycles elapsed till SysTick stop + error from previous round
    uint32_t new_load = cpu_ticks_requested - elapsed_till_stop - m_sleep_error;

    // clamp: rearm overhead must never push new_load into underflow
    if (new_load > cpu_ticks_requested)
    {
        new_load = cpu_ticks_requested;
    }

    // reload with elapsed ticks accounted
    HW_SysTickRearm(new_load);

#if (__CORTEX_M > 1) && STK_TICKLESS_USE_ARM_DWT
    // calculate error: subtract cycles consumed by the rearm sequence itself in the next round
    m_sleep_error = HW_DWTGetCounter() - error;
#endif

    // return actual clamped ticks armed
    return ticks_requested;
}
#endif

extern "C" void STK_SYSTICK_HANDLER()
{
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_RecordEnterISR();
#endif

    Context &ctx = GetContext();

#ifdef HAL_MODULE_ENABLED // STM32 HAL
    // make sure STM32 HAL gets timing information as it depends on SysTick in delaying procedures
#if STK_TICKLESS_IDLE
    uwTick += static_cast<uint32_t>(ctx.m_sleep_ticks * ctx.m_tick_resolution);
#else
    HAL_IncTick();
#endif

    // STM32 HAL starts SysTick on its initialization that will cause a crash on NULL,
    // therefore use additional check if HAL_MODULE_ENABLED is defined
    if (ctx.m_started)
    {
#else
    {
        // make sure SysTick is enabled by the Kernel::Start(), disable its start anywhere else
        STK_ASSERT(ctx.m_started);
        STK_ASSERT(ctx.m_handler != nullptr);
#endif
        ctx.ProcessTick();
    }

#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_RecordExitISR();
    SEGGER_SYSVIEW_IsStarted();
#endif
}

/*! \def   STK_ASM_BLOCK_PRIVILEGE_MODE
    \brief Set privileged mode, an equivalent of the following C code:

    if (GetContext().m_stack_active->access_mode & ACCESS_PRIVILEGED)
        __set_CONTROL(__get_CONTROL() & ~CONTROL_nPRIV_Msk);
    else
        __set_CONTROL(__get_CONTROL() | CONTROL_nPRIV_Msk)
*/

#ifdef __ICCARM__
    #define STK_ASM_BLOCK_PRIVILEGE_MODE_LOAD_ACTIVE_STACK\
    "MOV        r1, %[st_active]\n" /* r1 = Stack* (already in register) */
#else
    #define STK_ASM_BLOCK_PRIVILEGE_MODE_LOAD_ACTIVE_STACK\
    "LDR        r1, %[st_active]\n" /* r1 = Stack* (m_stack_active) */      
#endif

#define STK_ASM_BLOCK_PRIVILEGE_MODE\
    STK_ASM_BLOCK_PRIVILEGE_MODE_LOAD_ACTIVE_STACK\
    "LDR        r1, [r1, #4]    \n" /* r1 = m_stack_active->access_mode (offset 4), reuse r1 */ \
    "TST        r1, %[priv_val] \n" /* test ACCESS_PRIVILEGED flag */ \
    "BNE        1f              \n" /* if bit 0 is 1, branch to Privileged (1:) */\
    /* unprivileged mode: set CONTROL.nPRIV = 1 */\
    "MRS        r0, CONTROL     \n"\
    "ORR        r0, r0, #1      \n"\
    "MSR        CONTROL, r0     \n"\
    "B          2f              \n"\
    "1:                         \n"\
    /* privileged mode: set CONTROL.nPRIV = 0 */\
    "MRS        r0, CONTROL     \n"\
    "BIC        r0, r0, #1      \n"\
    "MSR        CONTROL, r0     \n"\
    "2:                         \n"

extern "C" __stk_attr_naked void STK_PENDSV_HANDLER()
{
    __asm volatile(
    STK_ASM_SYNTAX_UNIFIED

    STK_ASM_DISABLE_INTERRUPTS " \n"

    // Save the Secure PSP unconditionally. It is the spine of the entire context
    // frame regardless of whether the task was interrupted inside Secure or
    // Non-Secure code. PSP_NS, CONTROL_NS, and both PSPLIMs are embedded inside
    // the frame (in TrustZoneFrame) so the restore path never needs to branch on
    // EXC_RETURN. The CPU's own EXC_RETURN S-bit in the restored LR selects the
    // correct return world on "BX LR".
    "MRS        r0, PSP          \n"

#if STK_CORTEX_M_FPU
    // Save FP registers.
    "TST        LR, #16          \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */

    "IT         EQ               \n" /* if result is positive */
    "VMOVEQ     s0, s0           \n" /* force hardware lazy state preservation */

    "IT         EQ               \n" /* if result is positive */
    "VSTMDBEQ   r0!, {s16-s31}   \n" /* store 16 SP registers */
#endif

    // Save registers of inactive task's CPU context:

#if STK_CORTEX_M_MANAGE_LR
    // Save r4-r11 and LR.
    // Note: for Cortex-M3 and higher save LR to keep correct Thread state of the task
    // when it is restored.
    "STMDB      r0!, {r4-r11, LR}\n"
#else
    // note: STMIA is limited to r0-r7 range, therefore save via stack memory
    "SUBS       r0, r0, #16      \n" /* decrement for r4-r7 */
    "STMIA      r0!, {r4-r7}     \n"
    "MOV        r4, r8           \n"
    "MOV        r5, r9           \n"
    "MOV        r6, r10          \n"
    "MOV        r7, r11          \n"
    "SUBS       r0, r0, #32      \n" /* decrement for r8-r11 and pointer reset */
    "STMIA      r0!, {r4-r7}     \n"
    "SUBS       r0, r0, #16      \n" /* final pointer adjustment */
#endif

    // ARMv8-M TrustZone: save PSPLIM (Secure), PSPLIM_NS, PSP_NS, and CONTROL_NS
    // into the Secure stack frame below the callee-saved registers, matching
    // TrustZoneFrame layout. Using PSP/PSP_S as the single context-frame spine means
    // the restore path is unconditional and never needs to branch on EXC_RETURN.
    // PSP_NS captures the task's Non-Secure stack mid-execution (may be inside an
    // NS call that was interrupted); CONTROL_NS preserves NS privilege / stack-select.
#if STK_CORTEX_M_TRUSTZONE_FRAME
    "MRS        r2, PSPLIM       \n" /* Secure PSPLIM */
    "MRS        r3, PSPLIM_NS    \n" /* Non-Secure PSPLIM */
    "STMDB      r0!, {r2, r3}    \n" /* push PSPLIM, PSPLIM_NS */

    "MRS        r2, PSP_NS       \n" /* Non-Secure PSP (mid-execution value) */
    "MRS        r3, CONTROL_NS   \n" /* Non-Secure CONTROL (nPRIV, SPSEL) */
    "STMDB      r0!, {r2, r3}    \n" /* push PSP_NS, CONTROL_NS */
#endif

    // Store in GetContext().m_stack_idle.
#ifdef __ICCARM__
    "STR        r0, [%[st_idle]] \n" /* store the first member (Stack::SP) from r0 */
#else
    "LDR        r1, %[st_idle]   \n"
    "STR        r0, [r1]         \n" /* store the first member (Stack::SP) from r0 */
#endif

    // Set privileged/unprivileged mode for the active stack.
#ifdef CONTROL_nPRIV_Msk
    STK_ASM_BLOCK_PRIVILEGE_MODE
#endif

    // Load stack of the active task from GetContext().m_stack_active (note: keep in sync with OnTaskStart).
#ifdef __ICCARM__
    "LDR        r0, [%[st_active]]\n" /* load the first member of Stack (Stack::SP) into r0 */
#else
    "LDR        r1, %[st_active] \n"
    "LDR        r0, [r1]         \n"  /* load the first member of Stack (Stack::SP) into r0 */
#endif

    // ARMv8-M TrustZone: restore TrustZoneFrame fields in reverse push order.
    // Pop order: PSP_NS + CONTROL_NS first (pushed last), then PSPLIM + PSPLIM_NS.
    // After this, r0 points to the callee-saved register region.
    // PSP_S is restored unconditionally after general-register pop.
    // CONTROL_NS restores NS privilege/stack-select; PSP_NS restores the task's
    // Non-Secure stack to exactly where it was when the task was interrupted.
    // EXC_RETURN in LR (restored by LDMIA below) carries the S-bit that tells
    // the CPU which world to return to on "BX LR" - no explicit branch needed.
#if STK_CORTEX_M_TRUSTZONE_FRAME
    "LDMIA      r0!, {r2, r3}    \n" /* pop PSP_NS, CONTROL_NS */
    "MSR        PSP_NS, r2       \n" /* restore Non-Secure PSP */
    "MSR        CONTROL_NS, r3   \n" /* restore Non-Secure CONTROL */

    "LDMIA      r0!, {r2, r3}    \n" /* pop PSPLIM, PSPLIM_NS */
    "MSR        PSPLIM, r2       \n" /* restore Secure PSPLIM */
    "MSR        PSPLIM_NS, r3    \n" /* restore Non-Secure PSPLIM */
#endif

    // Restore registers of active task's CPU context:

#if STK_CORTEX_M_MANAGE_LR
    // Restore r4-r11 and LR.
    "LDMIA      r0!, {r4-r11, LR}\n"
#else
    // note: LDMIA is limited to r0-r7 range, therefore load via stack memory
    "LDMIA      r0!, {r4-r7}     \n"
    "MOV        r8, r4           \n"
    "MOV        r9, r5           \n"
    "MOV        r10, r6          \n"
    "MOV        r11, r7          \n"
    "LDMIA      r0!, {r4-r7}     \n"
#endif

#if STK_CORTEX_M_FPU
    // Restore FP registers.
    "TST        LR, #16         \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ              \n" /* if result is positive */
    "VLDMIAEQ   r0!, {s16-s31}  \n" /* restore FP registers */
#endif

    // Restore PSP.
    "MSR        PSP, r0         \n"

    STK_ASM_ENABLE_INTERRUPTS " \n"

    STK_ASM_EXIT_FROM_HANDLER " \n"

    : /* output: none */
#ifdef __ICCARM__
    : [st_idle]   "r" (GetContext().m_stack_idle),
      [st_active] "r" (GetContext().m_stack_active),
#else
    : [st_idle]   "m" (GetContext().m_stack_idle),
      [st_active] "m" (GetContext().m_stack_active),
#endif
      [priv_val]  "i" (ACCESS_PRIVILEGED)
    : "r0", "r1" /* used as a scratchpad */
#if STK_CORTEX_M_TRUSTZONE_FRAME
     , "r2", "r3"
#endif
#if defined(__ICCARM__)
     , "memory"
#endif
     );
}

__stk_attr_naked void OnTaskStart()
{
    // Note: HW_DisableInterrupts() must be called prior calling this function.
  
    __asm volatile(
    STK_ASM_SYNTAX_UNIFIED

    // Set privileged/unprivileged mode for the active stack.
#ifdef CONTROL_nPRIV_Msk
    STK_ASM_BLOCK_PRIVILEGE_MODE
#endif

    // Load stack of the active task from GetContext().m_stack_active (Note: keep
    // in sync with OnTaskStart).
#ifdef __ICCARM__
    "LDR        r0, [%[st_active]]\n" /* IAR: load the first member of Stack (Stack::SP) into r0, %[st_active] is a pointer value register */
#else
    "LDR        r1, %[st_active] \n"  /* GCC: %[st_active] is a memory address pointing to Stack* */
    "LDR        r0, [r1]         \n"  /* GCC: load the first member of Stack (Stack::SP) into r0 */
#endif

    // ARMv8-M TrustZone: restore TrustZoneFrame fields in reverse push order (see STK_PENDSV_HANDLER).
    // Pop order: PSP_NS + CONTROL_NS first, then PSPLIM + PSPLIM_NS.
#if STK_CORTEX_M_TRUSTZONE_FRAME
    "LDMIA      r0!, {r2, r3}    \n" /* pop PSP_NS, CONTROL_NS */
    "MSR        PSP_NS, r2       \n" /* restore Non-Secure PSP */
    "MSR        CONTROL_NS, r3   \n" /* restore Non-Secure CONTROL */

    "LDMIA      r0!, {r2, r3}    \n" /* pop PSPLIM, PSPLIM_NS */
    "MSR        PSPLIM, r2       \n" /* restore Secure PSPLIM */
    "MSR        PSPLIM_NS, r3    \n" /* restore Non-Secure PSPLIM */
#endif

    // Restore registers of active task's CPU context:

#if STK_CORTEX_M_MANAGE_LR
    // Restore r4-r11 and LR.
    "LDMIA      r0!, {r4-r11, LR}\n"
#else
    // Note: LDMIA is limited to r0-r7 range, therefore load via stack memory.
    "LDMIA      r0!, {r4-r7}     \n"
    "MOV        r8, r4           \n"
    "MOV        r9, r5           \n"
    "MOV        r10, r6          \n"
    "MOV        r11, r7          \n"
    "LDMIA      r0!, {r4-r7}     \n"
#endif

#if STK_CORTEX_M_FPU
    // Restore FP registers.
    "TST        LR, #16         \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ              \n" /* if result is positive */
    "VLDMIAEQ   r0!, {s16-s31}  \n" /* restore FP registers */
#endif

    // Restore PSP.
    "MSR        PSP, r0         \n"

#if !STK_CORTEX_M_MANAGE_LR
    // M0: set LR to Thread mode, use PSP state and stack
    "LDR        r0, =%[exc_ret] \n"
    "MOV        LR, r0          \n"
#endif

    STK_ASM_ENABLE_INTERRUPTS " \n"

    STK_ASM_EXIT_FROM_HANDLER " \n"

    : /* output: none */
#ifdef __ICCARM__
    : [st_active] "r" (GetContext().m_stack_active),
#else
    : [st_active] "m" (GetContext().m_stack_active),
#endif
      [priv_val]  "i" (ACCESS_PRIVILEGED)
#if !STK_CORTEX_M_MANAGE_LR
    , [exc_ret]   "i" (STK_CORTEX_M_EXC_RETURN_THREAD_PSP)
#endif
    : "r0", "r1" /* used as a scratchpad */
#if STK_CORTEX_M_TRUSTZONE_FRAME
    , "r2", "r3"
#endif
#if defined(__ICCARM__)
     , "memory"
#endif
     );
}

void Context::Start()
{
    m_exiting = false;

    // Enable FPU before SaveJmp as it references FPU with VMRS.
    HW_EnableFullFpuAccess();

    // Save jump location of the Exit trap.
    STK_UNUSED(SaveJmp(m_exit_buf));
    if (m_exiting)
    {
        // Notify kernel about a full stop.
        m_handler->OnStop();
    }
    else
    {
        HW_StartScheduler();
    }
}

void Context::OnStart()
{
    // Interrupts must be disabled at this point.
    STK_ASSERT(HW_InterruptsDisabled());

    // FPU
    HW_EnableFullFpuAccess();

    // Clear FPU usage status if FPU was used before kernel start.
    HW_ClearFpuState();

    // Get the first active stack from the kernel.
    m_handler->OnStart(m_stack_active);

    // Start with initially 1 elapsed tick (after timer expires).
    StartTickTimer(1);

    // Set lowest priority for PendSV (SysTick priority is set in StartTickTimer).
    NVIC_SetPriority(PendSV_IRQn, STK_CORTEX_M_ISR_PRIORITY_LOWEST);
    // Set highest priority for SVC interrupts to support critical section for unprivileged tasks.
#ifdef CONTROL_nPRIV_Msk
    NVIC_SetPriority(SVCall_IRQn, STK_CORTEX_M_ISR_PRIORITY_HIGHEST);
#endif

    m_started = true;
}

#if STK_TICKLESS_IDLE
Timeout Context::Suspend()
{
    const uint32_t tick_resolution = GetTickResolutionInClockCycles();
    if (tick_resolution == 0U)
    {
        STK_ASSERT(false);
        return NO_WAIT;
    }

    HW_DisableInterrupts();

    // pause SysTick in order to read elapsed value
    HW_SysTickDisable();

    // get already elapsed CPU cycles since SysTick ISR invocation up to SysTick timer stop (see above)
    // to account for them for a new period value
    const uint32_t elapsed = HW_SysTickElapsed(HW_SysTickValueAfterDisable());

    // stop SysTick timer
    HW_SysTickStop();

    // clear pending PendSV exception
    HW_ClearPendingSwitch();

    // notify core about suspension (it will also yield currently active task forcibly)
    m_handler->OnSuspend(true);

    // update tasks and out currently active task (if any) into a sleep, it will cause a switch
    // to a sleep trap after HW_EnableInterrupts, otherwise not
    Timeout no_sleep = 0;
    OnTick(no_sleep);

    // get already elapsed ticks since the OnTick and a call to Suspend(), we shall account for this
    // period and return only the remainder
    const Timeout elapsed_ticks = static_cast<Timeout>(elapsed / tick_resolution);
    const Timeout sleep_ticks = Max(m_sleep_ticks - elapsed_ticks, static_cast<Timeout>(0));

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

// __stk_attr_used required for Link-Time Optimization (-flto)
extern "C" __stk_attr_used void SVC_Handler_Main(Word *svc_args)
{
    // Word is typedef uintptr_t (stk_common.h) - the only integer type the Standard
    // blesses for lossless pointer round-trips (MISRA C++ 5-2-8, CERT INT36-C)
    STK_STATIC_ASSERT_DESC_N(PTR, sizeof(Word) == sizeof(void *),
        "Word must be uintptr_t width for safe pointer round-trip via frame->PC");

    // priority 0 (NMI, HardFault) unaffected: SVC (priority 0 per OnStart()) remains
    // reachable so SVC_EXIT_CRITICAL can always unwind
    STK_STATIC_ASSERT_DESC_N(NVIC, __NVIC_PRIO_BITS < 32U,
        "NVIC priority bit width exceeds safe shift range");

    // 'volatile': R0 is written back to stacked memory, compiler must not eliminate the store
    volatile ExceptionFrame *const frame = reinterpret_cast<volatile ExceptionFrame *>(svc_args);

    // details: https://developer.arm.com/documentation/ka004005/latest
    // Thumb SVC encoding: [15:8] = 0xDF, [7:0] = imm8
    // opcode lives two bytes (one Thumb halfword) before the stacked PC:
    const uint8_t       *const insn_ptr = hw::WordToPtr<const uint8_t>(frame->PC - 2U);
    const ESvcCommandId  command        = static_cast<ESvcCommandId>(*insn_ptr);

    switch (command)
    {
    case SVC_START_SCHEDULING: {
        // disallow any duplicate attempt
        STK_ASSERT(!GetContext().m_started);
        if (!GetContext().m_started)
        {
            // make sure interrupts do not interfere, OnStart expects interrupts disabled
            HW_DisableInterrupts();

            GetContext().OnStart();

            // start first task
            OnTaskStart();
        }
        break; }

    /*case SVC_FORCE_SWITCH: {
        HW_ScheduleContextSwitch();
        break; }*/

#ifdef CONTROL_nPRIV_Msk
    case SVC_ENTER_CRITICAL: {
        const Word saved_basepri = __get_BASEPRI();
        __set_BASEPRI(static_cast<uint32_t>(1U) << __NVIC_PRIO_BITS); // mask all configurable-priority interrupts
        __DSB();                   // BASEPRI write visible to bus before SVC return
        __ISB();                   // pipeline flush: mask in effect at first caller instruction
        frame->R0 = saved_basepri; // return saved value via stacked R0
        break; }

    case SVC_EXIT_CRITICAL: {
        __DSB();                   // drain pending stores before widening interrupt window
        __set_BASEPRI(frame->R0);  // restore saved BASEPRI (passed in R0)
        __ISB();                   // pending interrupts at restored priority may fire now
        break; }
#endif // CONTROL_nPRIV_Msk

    default: {
        // any SVC number not in ESvcCommandId is a defect, panic unconditionally
        STK_KERNEL_PANIC(KERNEL_PANIC_UNKNOWN_SVC);
        break; }
    }
}

// details: "How to Write an SVC Function", https://developer.arm.com/documentation/ka004005/latest
extern "C" __stk_attr_naked void STK_SVC_HANDLER()
{
    __asm volatile(
    STK_ASM_SYNTAX_UNIFIED
#ifndef __ICCARM__
    ".global SVC_Handler_Main   \n"
#endif
    STK_ASM_ALIGN_2 // ensure the entry point is aligned

#ifdef _STK_CORTEX_M_TRUSTZONE
    // ARMv8-M TrustZone (Cortex-M33 / Mainline)
    // EXC_RETURN bit layout (ARMv8-M ARM B1.5.8):
    //   bit 6 (0x40): 1 = Secure stack, 0 = Non-Secure stack.  <-- S-bit
    //   bit 2 (0x04): 1 = PSP,          0 = MSP.
    // We test bit 6 first to select the correct world's stack pointer,
    // then bit 2 to choose MSP vs PSP within that world.
    #if (__CORTEX_M >= 33U)
    // -----------------------------------------------------------------
    // Cortex-M33 and later (ARMv8-M Mainline) -- IT/ITE available.
    // -----------------------------------------------------------------
    "TST    LR, #4              \n" // bit 2: 0 = MSP, 1 = PSP
    "BNE    .use_psp            \n"
    // --- MSP branch ---
    "TST    LR, #64             \n" // bit 6 (S-bit): 1 = Secure, 0 = Non-Secure
    "ITE    NE                  \n"
    "MRSNE  r0, MSP             \n" // r0 = Secure MSP
    "MRSEQ  r0, MSP_NS          \n" // r0 = Non-Secure MSP
    "B      .call_main          \n"

    ".use_psp:                  \n"
    "TST    LR, #64             \n" // bit 6 (S-bit): 1 = Secure, 0 = Non-Secure
    "ITE    NE                  \n"
    "MRSNE  r0, PSP             \n" // r0 = Secure PSP
    "MRSEQ  r0, PSP_NS          \n" // r0 = Non-Secure PSP
    #else
    // -----------------------------------------------------------------
    // Cortex-M23 (ARMv8-M Baseline) -- no IT instructions, limited ISA.
    // Shift bits into the sign position and use BMI (Branch if Minus).
    //   bit 2 -> sign: LSLS r1, #29  (32 - 3 = 29)
    //   bit 6 -> sign: LSLS r1, #25  (32 - 7 = 25)
    // -----------------------------------------------------------------
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #29         \n" // shift bit 2 into sign
    "BMI    .use_psp_v8m_b      \n" // bit 2 set  -> PSP branch
    // --- MSP branch ---
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #25         \n" // shift bit 6 (S-bit) into sign
    "BMI    .use_msp_s          \n" // S=1 -> Secure MSP
    "MRS    r0, MSP_NS          \n" // S=0 -> Non-Secure MSP
    "B      .call_main          \n"

    ".use_msp_s:                \n"
    "MRS    r0, MSP             \n" // r0 = Secure MSP
    "B      .call_main          \n"

    ".use_psp_v8m_b:            \n"
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #25         \n" // shift bit 6 (S-bit) into sign
    "BMI    .use_psp_s          \n" // S=1 -> Secure PSP
    "MRS    r0, PSP_NS          \n" // S=0 -> Non-Secure PSP
    "B      .call_main          \n"

    ".use_psp_s:                \n"
    "MRS    r0, PSP             \n" // r0 = Secure PSP
    #endif

    ".call_main:                \n"
#elif (__CORTEX_M >= 3U)
    // Cortex-M3/M4/M7
    "TST    LR, #4              \n" // check EXC_RETURN bit 2
    "ITE    EQ                  \n"
    "MRSEQ  r0, MSP             \n" // r0 = MSP
    "MRSNE  r0, PSP             \n" // else r0 = PSP
#else
    // Cortex-M0/M0+ (limited ISA)
    "MOV    r0, LR              \n" // r0 = LR
    "LSLS   r0, r0, #29         \n" // if (r0 & 4)
    "BMI    .use_psp_m0         \n" // else
    "MRS    r0, MSP             \n" // r0 = MSP
    "B      .call_main_m0       \n"

    ".use_psp_m0:               \n"
    "MRS    r0, PSP             \n" // else r0 = PSP

    ".call_main_m0:             \n"
#endif

    // even on Cortex-M3+, a long jump is safer when using LTO, we load address into register to allow far jump (>2KB)
    "LDR    r1, =SVC_Handler_Main \n"
    "BX     r1                  \n"

    STK_ASM_ALIGN_2  // ensure literal pool is aligned
    STK_ASM_POOL     // ensure literal pool is reachable
    );
}

void OnTaskRun(ITask *runnable)
{
    runnable->Run();
}

void OnTaskExit()
{
    uint32_t cs;
    HW_CriticalSectionStart(cs);

    GetContext().m_handler->OnTaskExit(GetContext().m_stack_active);

    HW_CriticalSectionEnd(cs);

    for (;;)
    {
        // enter standby mode until time slot expires
        HW_EnterSleepMode();
    }
}

void OnSchedulerSleep()
{
    // if hit here, increase the size of STK_SLEEP_TRAP_STACK_SIZE
    STK_STATIC_ASSERT(STK_SLEEP_TRAP_STACK_SIZE >= STK_STACK_SIZE_MIN);

#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_OnIdle();
#endif

    for (;;)
    {
        HW_EnterSleepMode();
    }
}

void OnSchedulerSleepOverride()
{
    // if hit here, increase the size of STK_SLEEP_TRAP_STACK_SIZE
    STK_STATIC_ASSERT(STK_SLEEP_TRAP_STACK_SIZE >= STK_STACK_SIZE_MIN);

#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_OnIdle();
#endif

    for (;;)
    {
        GetContext().OnSleepOverride();
    }
}

void OnSchedulerExit()
{
    __set_CONTROL(0U); // switch to MSP
    __set_PSP(0U);     // clear PSP (for a clean register state)

    // jump back to SaveJmp's return site with m_exiting already set to true
    RestoreJmp(GetContext().m_exit_buf, 0);
}

#if STK_SEGGER_SYSVIEW
static void SendSysDesc()
{
    SEGGER_SYSVIEW_SendSysDesc("SuperTinyKernel (STK)");
}
#endif

void PlatformArmCortexM::Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us,
    Stack *exit_trap)
{
    GetContext().Initialize(event_handler, service, exit_trap, resolution_us);
}

void PlatformArmCortexM::Start()
{
    GetContext().Start();
}

void PlatformArmCortexM::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    STK_STATIC_ASSERT_DESC_N(ExceptionFrame, (sizeof(ExceptionFrame) == (8 * sizeof(Word))),
        "ExceptionFrame layout must match the ARMv7-M hardware exception frame exactly");
    STK_ASSERT(stack_memory->GetStackSize() > STK_CORTEX_M_TOTAL_REGISTER_COUNT);

#ifdef _STK_CORTEX_M_TRUSTZONE
    bool is_non_secure_task = false;

    // Replace stack_memory of Non-Secure task with a Secure memory which is required for launching the task
    // and Secure operations via NSC veneer.
    if ((user_task != nullptr) && (user_task->GetSecureStackMemory() != nullptr))
    {
        stack_memory       = user_task->GetSecureStackMemory();
        is_non_secure_task = true;

        STK_ASSERT(stack_memory->GetStackSize() > STK_CORTEX_M_TOTAL_REGISTER_COUNT);
    }
#endif

    // Initialize stack memory.
    const Word stack_top = Context::InitStackMemory(stack_memory);

    // Initialize Stack Pointer (SP).
    stack->SP = stack_top - (STK_CORTEX_M_TOTAL_REGISTER_COUNT * sizeof(Word));

    // Place the initial task frame flush against the top of the stack:
    // TaskFrame::exc (ExceptionFrame) occupies the top 8 words, TaskFrame::EXC_RETURN
    // (when present) sits immediately below it, and TrustZoneFrame (when present) sits
    // below that.
    TaskFrame *const task_frame = hw::WordToPtr<TaskFrame>(stack_top - sizeof(TaskFrame));

    // Initialize registers for the user task's first start.
    switch (stack_type)
    {
    case STACK_USER_TASK: {
    #ifdef _STK_CORTEX_M_TRUSTZONE
        if (is_non_secure_task)
        {
            task_frame->exc.LR = NS_stk_util_GetOnTaskExitAddr();
        }
        else
    #endif
        {
            task_frame->exc.LR = hw::PtrToWord(&OnTaskExit);
        }
        task_frame->exc.PC = hw::PtrToWord(&OnTaskRun);
        task_frame->exc.R0 = hw::PtrToWord(user_task);
        break; }

    case STACK_SLEEP_TRAP: {
        task_frame->exc.PC = hw::PtrToWord(GetContext().m_overrider != nullptr ? &OnSchedulerSleepOverride : &OnSchedulerSleep);
        task_frame->exc.LR = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        task_frame->exc.R0 = 0U;
        break; }

    case STACK_EXIT_TRAP: {
        task_frame->exc.PC = hw::PtrToWord(&OnSchedulerExit);
        task_frame->exc.LR = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        task_frame->exc.R0 = 0U;
        break; }

    default: {
        STK_KERNEL_PANIC(KERNEL_PANIC_BAD_STACK_TYPE);
        break; }
    }

    // Ensure the Program Counter is properly aligned to halfword boundaries
    // by clearing the Thumb state tracking bit from the physical target address.
    task_frame->exc.PC = hw::reg::PC::ClearThumbBit(task_frame->exc.PC);

    // Initialize the Execution Program Status Register (EPSR) with the T-bit enabled,
    // which is required for all ARM Cortex-M processors to execute instructions.
    task_frame->exc.PSR = hw::reg::XPSR::SetThumbExecution(stk::hw::reg::XPSR::DEFAULT_INIT);

#if STK_CORTEX_M_MANAGE_LR
    // Set the EXC_RETURN value to target Thread Mode using the Process Stack Pointer (PSP).
    // Note for TrustZone configurations: Execution will return to the Non-Secure state
    // via a Secure-side trampoline, which invokes the task's entry/ITask::Run function
    // using a dedicated Non-Secure function call (__BXNS / Non-Secure callable boundary).
    task_frame->EXC_RETURN = STK_CORTEX_M_EXC_RETURN_THREAD_PSP;
#endif // STK_CORTEX_M_MANAGE_LR

#if STK_CORTEX_M_TRUSTZONE_FRAME
    // Write TrustZoneFrame immediately above stack->SP (below the r4-r11 region).
    // Frame layout in memory (low -> high address, matching STMDB push order in PendSV):
    //   [SP+0]  PSP_NS     (pushed last by 2nd STMDB, popped first by 1st LDMIA)
    //   [SP+4]  CONTROL_NS
    //   [SP+8]  PSPLIM     (pushed first by 1st STMDB, popped last by 2nd LDMIA)
    //   [SP+12] PSPLIM_NS
    TrustZoneFrame *const tz_frame = hw::WordToPtr<TrustZoneFrame>(stack->SP);
    tz_frame->PSP_NS     = 0U;
    tz_frame->CONTROL_NS = hw::reg::CONTROL::DEFAULT_INIT;
    tz_frame->PSPLIM     = 0U; // unlimited for Secure task
    tz_frame->PSPLIM_NS  = 0U; // unlimited for Secure task

    if (is_non_secure_task)
    {
        IStackMemory *ns_stack_mem = user_task;

        // Initialize stack memory.
        const Word ns_stack_top = Context::InitStackMemory(ns_stack_mem);

        // NS thread privileged by default.
        tz_frame->CONTROL_NS = hw::reg::CONTROL::SetPrivileged(tz_frame->CONTROL_NS);

        // NS thread is using PSP_NS.
        tz_frame->CONTROL_NS = hw::reg::CONTROL::SetSPSelectionToPSP(tz_frame->CONTROL_NS);

        // For a fresh NS task, PSP_NS starts at the top of its NS stack
        // (the hardware exception frame was placed there by the caller).
        tz_frame->PSP_NS = ns_stack_top;

        // Bottom of NS stack.
        tz_frame->PSPLIM_NS = hw::PtrToWord(ns_stack_mem->GetStack());

        // Bottom of S stack.
        tz_frame->PSPLIM = hw::PtrToWord(stack_memory->GetStack());
    }
    else
    {
        stack->access_mode |= ACCESS_SECURE;

        // Bottom of S stack.
        tz_frame->PSPLIM = hw::PtrToWord(stack_memory->GetStack());
    }
#endif // STK_CORTEX_M_TRUSTZONE_FRAME
}

// ---------------------------------------------------------------------------
// ARMv8-M TrustZone Non-Secure callable (NSC) gateway veneers.
// ---------------------------------------------------------------------------
#ifdef _STK_CORTEX_M_TRUSTZONE

/*! \brief  NSC gateway: OnTaskExit.
*/
STK_TZ_NSC_GATEWAY
void NSC_stk_Kernel_OnTaskExit()
{
    OnTaskExit();
}

/*! \brief  NSC gateway: hw::CriticalSection::Enter.
*/
STK_TZ_NSC_GATEWAY
void NSC_stk_hw_CriticalSection_Enter()
{
    hw::CriticalSection::Enter();
}

/*! \brief  NSC gateway: hw::CriticalSection::Exit.
*/
STK_TZ_NSC_GATEWAY
void NSC_stk_hw_CriticalSection_Exit()
{
    hw::CriticalSection::Exit();
}

/*! \brief  NSC gateway: hw::SpinLock::Lock.
*/
STK_TZ_NSC_GATEWAY
void NSC_stk_hw_SpinLock_Lock(hw::SpinLock *sl)
{
    if ((sl != nullptr) && (cmse_check_pointed_object(sl,  CMSE_NONSECURE) != nullptr))
    {
        sl->Lock();
    }
}

/*! \brief  NSC gateway: hw::SpinLock::Unlock.
*/
STK_TZ_NSC_GATEWAY
void NSC_stk_hw_SpinLock_Unlock(hw::SpinLock *sl)
{
    if ((sl != nullptr) && (cmse_check_pointed_object(sl,  CMSE_NONSECURE) != nullptr))
    {
        sl->Unlock();
    }
}

/*! \brief  NSC gateway: hw::SpinLock::TryLock.
*/
STK_TZ_NSC_GATEWAY
bool NSC_stk_hw_SpinLock_TryLock(hw::SpinLock *sl)
{
    bool locked;

    if ((sl != nullptr) && (cmse_check_pointed_object(sl,  CMSE_NONSECURE) != nullptr))
    {
        locked = sl->TryLock();
    }
    else
    {
        locked = false;
    }

    return locked;
}

/*! \brief  NSC gateway: hw::HiResClock::GetFrequency.
*/
STK_TZ_NSC_GATEWAY
uint32_t NSC_stk_hw_HiResClock_GetFrequency()
{
    return hw::HiResClock::GetFrequency();
}

/*! \brief  NSC gateway: hw::HiResClock::GetCycles.
*/
STK_TZ_NSC_GATEWAY
stk::Cycles NSC_stk_hw_HiResClock_GetCycles()
{
    return hw::HiResClock::GetCycles();
}

/*! \brief  NSC gateway: hw::IsInsideISR.
*/
STK_TZ_NSC_GATEWAY
bool NSC_stk_hw_IsInsideISR()
{
    return hw::IsInsideISR();
}

#endif // _STK_CORTEX_M_TRUSTZONE
// ---------------------------------------------------------------------------

void Context::OnStop()
{
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_Stop();
#endif

    // stop SysTick timer
    HW_SysTickStop();

    // clear pending PendSV exception
    HW_ClearPendingSwitch();

    m_started = false;
    m_exiting = true;

    // make sure all assignments are set and executed
    __DSB();
    __ISB();
}

void PlatformArmCortexM::Stop()
{
    GetContext().OnStop();

    // load context of the Exit trap
    HW_DisableInterrupts();
    OnTaskStart();
}

uint32_t PlatformArmCortexM::GetTickResolution() const
{
    return GetContext().m_tick_resolution;
}

Cycles PlatformArmCortexM::GetSysTimerCount() const
{
    return static_cast<Cycles>(HW_SysTickValue());
}

uint32_t PlatformArmCortexM::GetSysTimerFrequency() const
{
    return HW_CoreClockFrequency();
}

void PlatformArmCortexM::SwitchToNext()
{
    GetContext().m_handler->OnTaskSwitch(HW_GetCallerSP());
}

void PlatformArmCortexM::Sleep(Timeout ticks)
{
    GetContext().m_handler->OnTaskSleep(HW_GetCallerSP(), ticks);
}

bool PlatformArmCortexM::SleepUntil(Ticks timestamp)
{
    return GetContext().m_handler->OnTaskSleepUntil(HW_GetCallerSP(), timestamp);
}

EWaitResult PlatformArmCortexM::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return GetContext().m_handler->OnTaskWait(HW_GetCallerSP(), sync_obj, mutex, timeout);
}

TId PlatformArmCortexM::GetTid() const
{
    TId result;
    const Word isr = HW_GetCurrentException();

    // return special TId which denotes ISR
    if (isr != 0U)
    {
        const TId isr_tid = TID_ISR_N | isr;
        STK_ASSERT(IsIsrTid(isr_tid));
        result = isr_tid;
    }
    else
    {
        result = GetContext().m_handler->OnGetTid(HW_GetCallerSP());
    }
    
    return result;
}

void PlatformArmCortexM::ProcessHardFault()
{
    bool is_handled = false;

    if (GetContext().m_overrider != nullptr)
    {
        is_handled = GetContext().m_overrider->OnHardFault();
    }

    if (!is_handled)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_HRT_HARD_FAULT);
    }
}

void PlatformArmCortexM::SetEventOverrider(IEventOverrider *overrider)
{
    STK_ASSERT(!GetContext().m_started);
    GetContext().m_overrider = overrider;
}

Word PlatformArmCortexM::GetCallerSP() const
{
    return HW_GetCallerSP();
}

Timeout PlatformArmCortexM::Suspend()
{
#if STK_TICKLESS_IDLE
    return GetContext().Suspend();
#else
    return 0;
#endif
}

void PlatformArmCortexM::Resume(Timeout elapsed_ticks)
{
#if STK_TICKLESS_IDLE
    GetContext().Resume(elapsed_ticks);
#else
    STK_UNUSED(elapsed_ticks);
#endif
}

IKernelService *IKernelService::GetInstance()
{
    return GetContext().m_service;
}

void stk::hw::CriticalSection::Enter()
{
    bool is_privileged = false;
  
    // if we are in Handler or Privileged Thread Mode, we can skip the SVC and take the fast path
    if (HW_IsPrivilegedThreadMode())
    {
        is_privileged = true;
    }
    else if (HW_IsHandlerMode())
    {
        is_privileged = true;
    }
    else
    {
        // not privileged path
    }

    if (is_privileged)
    {
        GetContext().EnterCriticalSection();
    }
    else
    {
        GetContext().UnprivEnterCriticalSection();
    }
}

void stk::hw::CriticalSection::Exit()
{
    bool is_privileged = false;
  
    // if we are in Handler or Privileged Thread Mode, we can skip the SVC and take the fast path
    if (HW_IsPrivilegedThreadMode())
    {
        is_privileged = true;
    }
    else if (HW_IsHandlerMode())
    {
        is_privileged = true;
    }
    else
    {
        // not privileged path
    }

    if (is_privileged)
    {
        GetContext().ExitCriticalSection();
    }
    else
    {
        GetContext().UnprivExitCriticalSection();
    }
}

void stk::hw::SpinLock::Lock()
{
    HW_SpinLockLock(m_lock);
}

void stk::hw::SpinLock::Unlock()
{
    HW_SpinLockUnlock(m_lock);
}

bool stk::hw::SpinLock::TryLock()
{
    return HW_SpinLockTryLock(m_lock);
}

bool stk::hw::IsInsideISR()
{
    return HW_IsHandlerMode();
}

bool stk::hw::IsContextPrivileged()
{
    return HW_IsPrivilegedThreadMode();
}

Cycles stk::hw::HiResClock::GetCycles()
{
    return HiResClockImpl::GetInstance()->GetCycles();
}

uint32_t stk::hw::HiResClock::GetFrequency()
{
    const uint32_t freq = HiResClockImpl::GetInstance()->GetFrequency();
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

#endif // _STK_ARCH_ARM_CORTEX_M
