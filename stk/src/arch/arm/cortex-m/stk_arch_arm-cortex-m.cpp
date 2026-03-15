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

//#define STK_CORTEX_M_TRUSTZONE

#ifdef STK_CORTEX_M_TRUSTZONE
#include <arm_cmse.h>
#endif

#include "stk_arch.h"
#include "arch/stk_arch_common.h"

using namespace stk;

//! Do sanity check for a compiler define, __CORTEX_M must be defined.
#ifndef __CORTEX_M
#error Expecting __CORTEX_M with value corresponding to Cortex-M model (0, 3, 4, ...)!
#endif

#define STK_CORTEX_M_EXC_RETURN_HANDLR_MSP 0xFFFFFFF1 // Handler mode, MSP stack (non-secure)
#define STK_CORTEX_M_EXC_RETURN_THREAD_MSP 0xFFFFFFF9 // Thread mode, MSP stack (non-secure)
#define STK_CORTEX_M_EXC_RETURN_THREAD_PSP 0xFFFFFFFD // Thread mode, PSP stack (non-secure)

#define STK_CORTEX_M_ISR_PRIORITY_HIGHEST  0
#define STK_CORTEX_M_ISR_PRIORITY_LOWEST   0xFF

enum ESvc : uint8_t
{
    SVC_START_SCHEDULING = 0,
    SVC_ENTER_CRITICAL,
    SVC_EXIT_CRITICAL,
    //SVC_FORCE_SWITCH
};

//! If (1) then code assumes FPU presence on CPU which is used by the compiler.
#define STK_CORTEX_M_FPU ((__FPU_PRESENT == 1) && (__FPU_USED == 1))

//! If (1), manage Link Register (LR) per task.
#define STK_CORTEX_M_MANAGE_LR (__CORTEX_M >= 3)

//! Number of registers kept in stack.
#if STK_CORTEX_M_MANAGE_LR
    #define STK_CORTEX_M_REGISTER_COUNT 17
#else
    #define STK_CORTEX_M_REGISTER_COUNT 16
#endif

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
static __stk_forceinline bool HW_InterruptsDisabled()
{
#if (defined(__clang__) && defined(__ARMCOMPILER_VERSION)) || defined(__ICCARM__)
    Word primask;
    __asm volatile("MRS %0, primask" : "=r"(primask));
    return (primask & 1);
#else
    return (__get_PRIMASK() & 1);
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
    if (!lock)
        STK_KERNEL_PANIC(KERNEL_PANIC_SPINLOCK_DEADLOCK); // release attempt of unowned lock

    // ensure all data writes (like scheduling metadata) are flushed before the lock is released:
    // __atomic_clear with __ATOMIC_RELEASE provides the required store-release barrier,
    // the explicit dmb ishst is retained for toolchains that do not lower __ATOMIC_RELEASE
    // to a full DMB on ARMv7-M (e.g. older GCC versions with -mcpu=cortex-m4)
    __asm volatile("dmb ishst" ::: "memory");

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
    uint32_t timeout = 0xFFFFFF;
    while (!HW_SpinLockTryLock(lock))
    {
        if (--timeout == 0)
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
    ".syntax unified                \n"
#if (__CORTEX_M >= 3)
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
__stk_attr_noreturn
void RestoreJmp(JmpFrame &/*f*/, int32_t /*val*/)
{
    __asm volatile(
    ".syntax unified                \n"
#if (__CORTEX_M >= 3)
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

/*! \brief  Check if caller is in Handler Mode (IPSR != 0), i.e. inside ISR.
*/
static __stk_forceinline bool HW_IsHandlerMode()
{
    return (__get_IPSR() != 0);
}

/*! \brief  Check if caller is in Privileged Thread Mode (nPRIV == 0).
    \note   ARM Cortex-M0 is always Privileged Thread Mode (M0 does not support privileges).
*/
static __stk_forceinline bool HW_IsPrivilegedThreadMode()
{
#ifdef CONTROL_nPRIV_Msk
    return ((__get_CONTROL() & CONTROL_nPRIV_Msk) == 0);
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
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
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
    // enable FPU CP10/CP11 Secure and Non-secure register access
    SCB->CPACR |= (0b11 << 20) | (0b11 << 22);
#endif
}

/*! \brief Start SysTick timer peripheral.
*/
static __stk_forceinline void HW_StartSysTick(int32_t tick_resolution)
{
    uint32_t result = SysTick_Config((uint32_t)STK_TIME_TO_CPU_TICKS_USEC(SystemCoreClock, tick_resolution));
    STK_ASSERT(result == 0);
    (void)result;
}

/*! \brief Stop SysTick timer peripheral.
*/
static __stk_forceinline void HW_StopSysTick()
{
    SysTick->CTRL = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;
}

//! Global lock to synchronize critical sections of multiple cores.
static volatile bool s_StkCortexmCsuLock = false;

//! Internal context.
static struct Context : public PlatformContext
{
    Context() : PlatformContext(), m_exit_buf(), m_overrider(nullptr), m_csu(0), m_csu_nesting(0),
        m_started(false), m_exiting(false)
    {}

    void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap, int32_t resolution_us)
    {
        PlatformContext::Initialize(handler, service, exit_trap, resolution_us);

        STK_STATIC_ASSERT_DESC_N(SP, offsetof(Stack, SP) == 0,
            "expect Stack::mode member at offset of 0 (first member)");
        STK_STATIC_ASSERT_DESC_N(mode, offsetof(Stack, mode) == 4,
            "expect Stack::mode member at offset of 4 (second member)");

        m_csu         = 0;
        m_csu_nesting = 0;
        m_overrider   = nullptr;
        m_started     = false;
        m_exiting     = false;

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_Init(SystemCoreClock, SystemCoreClock, nullptr, SendSysDesc);
    #endif
    }

    __stk_forceinline void OnTick()
    {
        HW_DisableInterrupts();

        if (m_handler->OnTick(&m_stack_idle, &m_stack_active))
        {
        #if STK_SEGGER_SYSVIEW
            SEGGER_SYSVIEW_OnTaskStopExec();
            if (GetContext().m_stack_active->tid != SYS_TASK_ID_SLEEP)
                SEGGER_SYSVIEW_OnTaskStartExec(GetContext().m_stack_active->tid);
        #endif

            HW_ScheduleContextSwitch();
        }

        HW_EnableInterrupts();
    }

    __stk_forceinline void EnterCriticalSection()
    {
        // disable local interrupts first to prevent self-deadlock
        uint32_t current_ses;
        HW_CriticalSectionStart(current_ses);

        if (m_csu_nesting == 0)
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
        STK_ASSERT(m_csu_nesting != 0);
        --m_csu_nesting;

        if (m_csu_nesting == 0)
        {
            // capture the state before releasing lock
            uint32_t ses_to_restore = m_csu;

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
        Word current_ses = HW_SVCEnterCritical();

        if (m_csu_nesting == 0)
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
        STK_ASSERT(m_csu_nesting != 0);
        --m_csu_nesting;

        if (m_csu_nesting == 0)
        {
            // capture the state before releasing lock
            Word ses_to_restore = m_csu;

            // release global lock
            HW_SpinLockUnlock(s_StkCortexmCsuLock);

            // restore hardware interrupts via SVC
            HW_SVCExitCritical(ses_to_restore);
        }
    }

    void Start();
    void OnStart();
    void OnStop();

    typedef IPlatform::IEventOverrider eovrd_t;

    JmpFrame      m_exit_buf;    //!< saved context of the exit point
    eovrd_t      *m_overrider;   //!< platform events overrider
    Word          m_csu;         //!< user critical session
    uint8_t       m_csu_nesting; //!< depth of user critical session nesting
    volatile bool m_started;     //!< 'true' when in started state
    bool          m_exiting;     //!< 'true' when is exiting the scheduling process
}
s_StkPlatformContext[STK_ARCH_CPU_COUNT];

void PlatformArmCortexM::ProcessTick()
{
    GetContext().OnTick();
}

__stk_attr_noinline  // keep out of inlining to preserve stack frame
__stk_attr_noreturn  // never returns - a trap
void STK_PANIC_HANDLER_DEFAULT(EKernelPanicId id)
{
    (void)id;

    // disable all maskable interrupts: this prevents scheduler from running again and corrupting state further
    HW_DisableInterrupts();

    // spin forever: with a watchdog active this produces a clean reset, without a watchdog,
    // a debugger can attach and inspect 'id'
    for (;;)
    {
        __stk_relax_cpu();
    }
}

extern "C" void STK_SYSTICK_HANDLER()
{
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_RecordEnterISR();
#endif

    Context &ctx = GetContext();

#ifdef HAL_MODULE_ENABLED // STM32 HAL
    // make sure STM32 HAL get timing information as it depends on SysTick in delaying procedures
    HAL_IncTick();

    // STM32 HAL is starting SysTick on its initialization that will cause a crash on NULL,
    // therefore use additional check if HAL_MODULE_ENABLED is defined
    if (ctx.m_started)
    {
#else
    {
        // make sure SysTick is enabled by the Kernel::Start(), disable its start anywhere else
        STK_ASSERT(ctx.m_started);
        STK_ASSERT(ctx.m_handler != nullptr);
#endif
        ctx.OnTick();
    }

#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_RecordExitISR();
    SEGGER_SYSVIEW_IsStarted();
#endif
}

/*! \def   STK_ASM_BLOCK_PRIVILEGE_MODE
    \brief Set privileged mode, an equivalent of the following C code:

    if (GetContext().m_stack_active->mode == ACCESS_PRIVILEGED)
        __set_CONTROL(__get_CONTROL() & ~CONTROL_nPRIV_Msk);
    else
        __set_CONTROL(__get_CONTROL() | CONTROL_nPRIV_Msk)
*/
#define STK_ASM_BLOCK_PRIVILEGE_MODE\
    "LDR        r1, %[st_active]\n" /* r1 = Stack* (m_stack_active) */ \
    "LDR        r1, [r1, #4]    \n" /* r1 = m_stack_active->mode (offset 4), reuse r1 */ \
    "CMP        r1, %[priv_val] \n" /* compare with ACCESS_PRIVILEGED */ \
    "BEQ        1f              \n" /* if equal, privileged mode */\
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
    ".syntax unified             \n"

    STK_ASM_DISABLE_INTERRUPTS " \n"

    // save stack to inactive (idle) task
    "MRS        r0, psp          \n"

#if STK_CORTEX_M_FPU
    // save FP registers
    "TST        LR, #16          \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ               \n"
    "VSTMDBEQ   r0!, {s16-s31}   \n" /* store 16 SP registers */
#endif

    // save general registers
#if STK_CORTEX_M_MANAGE_LR
    // save r4-r11 and LR
    // note: for Cortex-M3 and higher save LR to keep correct Thread state of the task when it is restored
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

    // store in GetContext().m_stack_idle
    "LDR        r1, %[st_idle]   \n"
    "STR        r0, [r1]         \n" /* store the first member (SP) from r0 */

    // set privileged/unprivileged mode for the active stack
#ifdef CONTROL_nPRIV_Msk
    STK_ASM_BLOCK_PRIVILEGE_MODE
#endif

    // load stack of the active task from GetContext().m_stack_active (note: keep in sync with OnTaskStart)
    "LDR        r1, %[st_active] \n"
    "LDR        r0, [r1]         \n" /* load the first member of Stack (Stack::SP) into r0 */

    // restore general registers
#if STK_CORTEX_M_MANAGE_LR
    // restore r4-r11 and LR
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
    // restore FP registers
    "TST        LR, #16         \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ              \n" /* if result is positive */
    "VLDMIAEQ   r0!, {s16-s31}  \n" /* restore FP registers */
#endif

    "MSR        psp, r0         \n" /* restore psp */

    STK_ASM_ENABLE_INTERRUPTS " \n"

    STK_ASM_EXIT_FROM_HANDLER " \n"

    : /* output: none */
    : [st_idle]   "m" (GetContext().m_stack_idle),
      [st_active] "m" (GetContext().m_stack_active),
      [priv_val]  "i" (ACCESS_PRIVILEGED)
    : "r0", "r1" /* only r0, r1 are used as a scratchpad */ );
}

__stk_attr_naked void OnTaskStart()
{
    // note: HW_DisableInterrupts() must be called prior calling this function

    __asm volatile(
    ".syntax unified             \n"

    // set privileged/unprivileged mode for the active stack
#ifdef CONTROL_nPRIV_Msk
    STK_ASM_BLOCK_PRIVILEGE_MODE
#endif

    // load stack of the active task from GetContext().m_stack_active (note: keep in sync with OnTaskStart)
    "LDR        r1, %[st_active] \n"
    "LDR        r0, [r1]         \n" /* load the first member of Stack (Stack::SP) into r0 */

    // restore general registers
#if STK_CORTEX_M_MANAGE_LR
    // restore r4-r11 and LR
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
    // restore FP registers
    "TST        LR, #16         \n" /* test LR for 0xffffffe_, e.g. Thread mode with FP data */
    "IT         EQ              \n" /* if result is positive */
    "VLDMIAEQ   r0!, {s16-s31}  \n" /* restore FP registers */
#endif

    // restore psp
    "MSR        psp, r0         \n"

#if !STK_CORTEX_M_MANAGE_LR
    // M0: set LR to Thread mode, use PSP state and stack
    "LDR        r0, =%[exc_ret] \n"
    "MOV        LR, r0          \n"
#endif

    STK_ASM_ENABLE_INTERRUPTS " \n"

    STK_ASM_EXIT_FROM_HANDLER " \n"

    : /* output: none */
    : [st_active] "m" (GetContext().m_stack_active),
      [priv_val]  "i" (ACCESS_PRIVILEGED),
      [exc_ret]   "i" (STK_CORTEX_M_EXC_RETURN_THREAD_PSP)
    : "r0", "r1" /* only r0, r1 are used as a scratchpad */ );
}

void Context::Start()
{
    m_exiting = false;

    // save jump location of the Exit trap
    SaveJmp(m_exit_buf);
    if (m_exiting)
        return;

    HW_StartScheduler();
}

void Context::OnStart()
{
    // interrupts must be disabled at this point
    STK_ASSERT(HW_InterruptsDisabled());

    // FPU
    HW_EnableFullFpuAccess();

    // clear FPU usage status if FPU was used before kernel start
    HW_ClearFpuState();

    // notify kernel
    m_handler->OnStart(&m_stack_active);

    // start SysTick timer (it is yet can't fire an interrupt due to HW_DisableInterrupts)
    HW_StartSysTick(m_tick_resolution);

    // set priority
    // note: after SysTick_Config because it may change SysTick priority, PendSV and SysTick peripherals
    // have equal priority to avoid race
    NVIC_SetPriority(PendSV_IRQn, STK_CORTEX_M_ISR_PRIORITY_LOWEST);
    NVIC_SetPriority(SysTick_IRQn, STK_CORTEX_M_ISR_PRIORITY_LOWEST);
    // set highest priority for SVC interrupts to support critical section for unprivileged tasks
#ifdef CONTROL_nPRIV_Msk
    NVIC_SetPriority(SVCall_IRQn, STK_CORTEX_M_ISR_PRIORITY_HIGHEST);
#endif

    m_started = true;
}

// __stk_attr_used required for Link-Time Optimization (-flto)
extern "C" __stk_attr_used void SVC_Handler_Main(Word *svc_args)
{
    // Word is typedef uintptr_t (stk_common.h) - the only integer type the Standard
    // blesses for lossless pointer round-trips (MISRA C++ 5-2-8, CERT INT36-C)
    STK_STATIC_ASSERT_DESC_N(PTR, sizeof(Word) == sizeof(void *),
        "Word must be uintptr_t width for safe pointer round-trip via frame->PC");

    // priority 0 (NMI, HardFault) unaffected: SVC (priority 0 per OnStart()) remains
    // reachable so SVC_EXIT_CRITICAL can always unwind
    STK_STATIC_ASSERT_DESC_N(NVIC, __NVIC_PRIO_BITS < 32u,
        "NVIC priority bit width exceeds safe shift range");

    // 'volatile': R0 is written back to stacked memory, compiler must not eliminate the store
    volatile ExceptionFrame * const frame = reinterpret_cast<volatile ExceptionFrame *>(svc_args);

    // details: https://developer.arm.com/documentation/ka004005/latest
    // Thumb SVC encoding: [15:8] = 0xDF, [7:0] = imm8
    // ---
    // opcode lives two bytes (one Thumb halfword) before the stacked PC:
    // do explicit subtraction instead of negative indexing (MISRA C++ 5-0-16),
    // uint8_t extracted before ESvc cast to avoid impl-defined enum conversion (MISRA 5-2-6)
    const uint8_t *insn_ptr = hw::WordToPtr<const uint8_t>(frame->PC) - 2;
    const ESvc     command  = static_cast<ESvc>(*insn_ptr);

    switch (command)
    {
    case SVC_START_SCHEDULING: {
        // disallow any duplicate attempt
        STK_ASSERT(!GetContext().m_started);
        if (GetContext().m_started)
            return;

        // make sure interrupts do not interfere, OnStart expects interrupts disabled
        HW_DisableInterrupts();

        GetContext().OnStart();

        // start first task
        OnTaskStart();
        break; }

    /*case SVC_FORCE_SWITCH: {
        HW_ScheduleContextSwitch();
        break; }*/

#ifdef CONTROL_nPRIV_Msk
    case SVC_ENTER_CRITICAL: {
        const Word saved_basepri = __get_BASEPRI();
        __set_BASEPRI(static_cast<uint32_t>(1u) << __NVIC_PRIO_BITS); // mask all configurable-priority interrupts
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
        // any SVC number not in ESvc is a defect, panic unconditionally
        STK_KERNEL_PANIC(KERNEL_PANIC_UNKNOWN_SVC);
        break; }
    }
}

// details: "How to Write an SVC Function", https://developer.arm.com/documentation/ka004005/latest
extern "C" __stk_attr_naked void STK_SVC_HANDLER()
{
    __asm volatile(
    ".syntax unified            \n"
    ".global SVC_Handler_Main   \n"
    ".align 2                   \n" // ensure the entry point is aligned

#ifdef STK_CORTEX_M_TRUSTZONE
    // ARMv8-M TrustZone (Cortex-M33)
    #if (__CORTEX_M >= 33)
    "TST    LR, #4              \n" // bit 2: 0 = Main Stack (MSP), 1 = Process Stack (PSP)
    "BNE    .use_psp            \n"
    "TST    LR, #1              \n" // check CONTROL.SFPA (secure state), bit 0: 0 = Secure, 1 = Non-Secure
    "ITE    EQ                  \n"
    "MRSEQ  r0, MSP             \n" // r0 = secure MSP
    "MRSNE  r0, MSP_NS          \n" // r0 = non-secure MSP
    "B      .call_main          \n"

    ".use_psp:                  \n"
    "TST    LR, #1              \n"
    "ITE    EQ                  \n"
    "MRSEQ  r0, PSP             \n" // r0 = secure PSP
    "MRSNE  r0, PSP_NS          \n" // r0 = non-secure MSP
    #else
    // ARMv8-M Baseline (Cortex-M23) - no IT instructions
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #29         \n" // bit 2: 0 = Main Stack (MSP), 1 = Process Stack (PSP)
    "BMI    .use_psp_v8m_b      \n"
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #31         \n" // check CONTROL.SFPA (secure state), bit 0: 0 = Secure, 1 = Non-Secure
    "BMI    .use_msp_ns         \n"
    "MRS    r0, MSP             \n" // r0 = secure MSP
    "B      .call_main          \n"

    ".use_msp_ns:               \n"
    "MRS    r0, MSP_NS          \n" // r0 = non-secure MSP
    "B      .call_main          \n"

    ".use_psp_v8m_b:            \n"
    "MOV    r1, LR              \n"
    "LSLS   r1, r1, #31         \n"
    "BMI    .use_psp_ns         \n"
    "MRS    r0, PSP             \n" // r0 = secure PSP
    "B      .call_main          \n"

    ".use_psp_ns:               \n"
    "MRS    r0, PSP_NS          \n" // r0 = non-secure MSP
    #endif

    ".call_main:                \n"
#elif (__CORTEX_M >= 3)
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

    ".align 2                   \n"   // ensure literal pool is aligned
    ".pool                      \n"); // ensure literal pool is reachable
}

static void OnTaskRun(ITask *task)
{
    task->Run();
}

static void OnTaskExit()
{
    uint32_t cs;
    HW_CriticalSectionStart(cs);

    GetContext().m_handler->OnTaskExit(GetContext().m_stack_active);

    HW_CriticalSectionEnd(cs);

    for (;;)
    {
        __DSB();
        __WFI(); // enter standby mode until time slot expires
    }
}

static void OnSchedulerSleep()
{
    for (;;)
    {
    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnIdle();
    #endif

        SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk; // disable deep-sleep, go into a WAIT mode (sleep)
        __DSB();                            // ensure store takes effect (see ARM info)
        __WFI();                            // enter sleep mode
    }
}

static void OnSchedulerSleepOverride()
{
    if (!GetContext().m_overrider->OnSleep())
        OnSchedulerSleep();
}

static void OnSchedulerExit()
{
    __set_CONTROL(0); // switch to MSP
    __set_PSP(0);     // clear PSP (for a clean register state)

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

bool PlatformArmCortexM::InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task)
{
    STK_STATIC_ASSERT_DESC_N(ExceptionFrame, (sizeof(ExceptionFrame) == (8 * sizeof(Word))),
        "ExceptionFrame layout must match the ARMv7-M hardware exception frame exactly");
    STK_STATIC_ASSERT_DESC_N(TaskFrame, sizeof(TaskFrame) == (8 + STK_CORTEX_M_MANAGE_LR) * sizeof(Word),
        "TaskFrame size must equal ExceptionFrame plus optional EXC_RETURN word");

    STK_ASSERT(stack_memory->GetStackSize() > STK_CORTEX_M_REGISTER_COUNT);

    // initialize stack memory
    Word *stack_top = Context::InitStackMemory(stack_memory);

    // initialize Stack Pointer (SP)
    stack->SP = hw::PtrToWord(stack_top - STK_CORTEX_M_REGISTER_COUNT);

    // place the initial task frame flush against the top of the stack:
    // TaskFrame::exc (ExceptionFrame) occupies the top 8 words, TaskFrame::EXC_RETURN
    // (when present) sits immediately below it
    TaskFrame * const task_frame = reinterpret_cast<TaskFrame *>(stack_top) - 1;

    // initialize registers for the user task's first start
    switch (stack_type)
    {
    case STACK_USER_TASK: {
        task_frame->exc.PC = hw::PtrToWord(&OnTaskRun) & ~0x1UL;
        task_frame->exc.LR = hw::PtrToWord(&OnTaskExit);
        task_frame->exc.R0 = hw::PtrToWord(user_task);
        break; }

    case STACK_SLEEP_TRAP: {
        task_frame->exc.PC = hw::PtrToWord(GetContext().m_overrider != nullptr ? &OnSchedulerSleepOverride : &OnSchedulerSleep);
        task_frame->exc.LR = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        task_frame->exc.R0 = 0;
        break; }

    case STACK_EXIT_TRAP: {
        task_frame->exc.PC = hw::PtrToWord(&OnSchedulerExit);
        task_frame->exc.LR = STK_STACK_MEMORY_FILLER; // should not attempt to exit
        task_frame->exc.R0 = 0;
        break; }

    default:
        return false;
    }

    // details: "Program counter: Bit [0] is always 0, so instructions are always aligned to halfword boundaries",
    // https://developer.arm.com/documentation/ddi0413/c/programmer-s-model/registers/general-purpose-registers
    task_frame->exc.PC &= ~0x1UL;

    // set T bit of EPSR sub-register to enable execution of instructions
    // details: "Special-purpose program status registers (xPSR)": Execution PSR, https://developer.arm.com/documentation/ddi0413/c/programmer-s-model/registers/special-purpose-program-status-registers--xpsr-
    task_frame->exc.PSR = static_cast<Word>(1u) << 24;

#if STK_CORTEX_M_MANAGE_LR
    task_frame->EXC_RETURN = STK_CORTEX_M_EXC_RETURN_THREAD_PSP;
#endif

    return true;
}

void Context::OnStop()
{
#if STK_SEGGER_SYSVIEW
    SEGGER_SYSVIEW_Stop();
#endif

    // stop SysTick timer
    HW_StopSysTick();

    // clear pending PendSV exception
    SCB->ICSR |= SCB_ICSR_PENDSVCLR_Msk;

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

int32_t PlatformArmCortexM::GetTickResolution() const
{
    return GetContext().m_tick_resolution;
}

void PlatformArmCortexM::SwitchToNext()
{
    GetContext().m_handler->OnTaskSwitch(HW_GetCallerSP());
}

void PlatformArmCortexM::Sleep(Timeout ticks)
{
    GetContext().m_handler->OnTaskSleep(HW_GetCallerSP(), ticks);
}

IWaitObject *PlatformArmCortexM::Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
{
    return GetContext().m_handler->OnTaskWait(HW_GetCallerSP(), sync_obj, mutex, timeout);
}

TId PlatformArmCortexM::GetTid() const
{
    return GetContext().m_handler->OnGetTid(HW_GetCallerSP());
}

void PlatformArmCortexM::ProcessHardFault()
{
    if ((GetContext().m_overrider == nullptr) || !GetContext().m_overrider->OnHardFault())
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

IKernelService *IKernelService::GetInstance()
{
    return GetContext().m_service;
}

void stk::hw::CriticalSection::Enter()
{
    // if we are in Handler or Privileged Thread Mode, we can skip the SVC and take the fast path
    if (HW_IsHandlerMode() || HW_IsPrivilegedThreadMode())
        GetContext().EnterCriticalSection();
    else
        GetContext().UnprivEnterCriticalSection();
}

void stk::hw::CriticalSection::Exit()
{
    // if we are in Handler or Privileged Thread Mode, we can skip the SVC and take the fast path
    if (HW_IsHandlerMode() || HW_IsPrivilegedThreadMode())
        GetContext().ExitCriticalSection();
    else
        GetContext().UnprivExitCriticalSection();
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

#endif // _STK_ARCH_ARM_CORTEX_M
