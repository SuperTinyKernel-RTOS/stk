/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_H_
#define STK_ARCH_H_

/*! \file  stk_arch.h
    \brief Hardware Abstraction Layer (HAL) declarations for the \c stk::hw namespace.

    Selects and includes the correct architecture back-end header based on the active
    architecture macro (_STK_ARCH_ARM_CORTEX_M, _STK_ARCH_RISC_V, _STK_ARCH_X86_WIN32),
    then declares the portable \c stk::hw interface that the rest of the kernel uses:
     - Thread-local storage (TLS) read/write: GetTls, SetTls, GetTlsPtr, SetTlsPtr.
     - ISR context detection: IsInsideISR.
     - Preemption control: CriticalSection (nestable, single-core) and SpinLock (SMP).
     - Lock-free 64-bit volatile I/O: ReadVolatile64, WriteVolatile64.
*/

// Architecture back-end selection.
// Exactly one of the following macros must be defined by the build system (e.g. via -D compiler flag):
//   _STK_ARCH_ARM_CORTEX_M  — ARM Cortex-M (M0/M0+/M3/M4/M7/M33/M55).
//   _STK_ARCH_RISC_V        — RISC-V (RV32I/RV32E/RV64, with optional FPU).
//   _STK_ARCH_X86_WIN32     — x86/x64 on Windows (simulation/test use only).
//
// Defining more than one is not supported and will result in multiple conflicting definitions.
// _STK_ARCH_DEFINED is set by whichever back-end is included; it can be tested by downstream
// headers or build checks to verify that a valid architecture was selected.
#ifdef _STK_ARCH_ARM_CORTEX_M
#include "arch/arm/cortex-m/stk_arch_arm-cortex-m.h"
#define _STK_ARCH_DEFINED
#endif
#ifdef _STK_ARCH_RISC_V
#include "arch/risc-v/stk_arch_risc-v.h"
#define _STK_ARCH_DEFINED
#endif
#ifdef _STK_ARCH_X86_WIN32
#include "arch/x86/win32/stk_arch_x86-win32.h"
#define _STK_ARCH_DEFINED
#endif

namespace stk {

/*! \def   STK_KERNEL_PANIC
    \brief Called when the kernel detects an unrecoverable internal fault.
    \note  Unlike STK_ASSERT (which checks preconditions) this macro is reached only
           when a runtime invariant has been irreversibly violated — the kernel
           cannot continue operating correctly from this point.
    \note  Default behaviour:
             - In debug builds:   triggers a hardware breakpoint so a debugger can
                                  inspect state, then falls through to the safe-state handler.
             - In all builds:     calls STK_PANIC_HANDLER(id) which must not return.
    \note  Override STK_PANIC_HANDLER by defining it before including this header
           or in stk_config.h. The handler receives a numeric id (EKernelPanicId)
           and must never return. A minimal safe default is provided below.
    \param[in] id: EKernelPanicId value identifying the fault.
*/
#define STK_KERNEL_PANIC(id)                         \
    do {                                             \
        __stk_debug_break();   /* debug aid */       \
        STK_PANIC_HANDLER(id); /* must not return */ \
    } while (0)

/*! \namespace stk::hw
    \brief     Hardware Abstraction Layer (HAL) for architecture-specific operations.

    This namespace contains low-level functions that interface directly with the
    CPU registers and hardware state. Implementations live in the architecture
    back-end headers included above and are typically written in assembly or using
    compiler intrinsics for maximum performance and minimum latency.

    \see IsInsideISR, GetTlsPtr, SetTlsPtr, CriticalSection, SpinLock,
         ReadVolatile64, WriteVolatile64
*/
namespace hw {

/*! \brief     Cast a pointer to a CPU register-width integer.
    \tparam    T: The type of the object pointed to.
    \param[in] ptr: The pointer to be converted.
    \return    The numeric value of the pointer as a Word.
    \note      This operation is used to store pointers within task context structures
               or stack frames where raw register values are required.
    \note      MISRA deviation: [STK-DEV-001] Rule 5-2-7 (reinterpret_cast). This is a
               mechanical necessity for low-level kernel operations where the hardware
               requires integral values for address registers.
    \see       WordToPtr
*/
template <typename T>
__stk_forceinline Word PtrToWord(T *ptr) noexcept
{
    STK_STATIC_ASSERT(sizeof(Word) == sizeof(T *));
    return reinterpret_cast<Word>(ptr);
}

/*! \brief     Cast a CPU register-width integer back to a pointer.
    \tparam    T: The type of the object the resulting pointer will address.
    \param[in] value: The register-width integer (Word) to be converted.
    \return    A pointer of type T* addressing the memory location specified by the value.
    \note      This is the inverse of PtrToWord and is primarily used when restoring
               a task's context from a saved stack frame.
    \note      MISRA deviation: [STK-DEV-001] Rule 5-2-7 (reinterpret_cast).
               Required for restoring pointer types from numeric CPU context structures.
    \see       PtrToWord
*/
template <typename T>
__stk_forceinline T *WordToPtr(Word value) noexcept
{
    STK_STATIC_ASSERT(sizeof(Word) == sizeof(T *));
    return reinterpret_cast<T *>(value);
}

/*! \brief     Check whether the CPU is currently executing inside a hardware interrupt service routine (ISR).
    \return    \c true if called from an ISR context; \c false if called from a normal task/thread context.
    \note      Used as a guard in all ISR-unsafe kernel functions (Sleep, Delay, Yield, GetTid, etc.)
               to catch accidental calls from interrupt context that would deadlock the scheduler.
    \note      ISR-safe itself: may be called from any context.
*/
bool IsInsideISR();

// Some architectures (e.g. RISC-V with the 'tp' register) can implement TLS access as a
// single inline instruction. When the back-end header defines _STK_INLINE_TLS_DEFINED,
// GetTls and SetTls are provided as inline functions there and the declarations below
// are suppressed to avoid duplicate definitions.
#if !_STK_INLINE_TLS_DEFINED

/*! \brief     Read raw thread-pointer (TP) register used as per-task TLS storage.
    \return    Current TP register value as a \c Word.
    \note      Architecture-specific. On ARM Cortex-M the kernel stores the TLS pointer
               in a dedicated register or memory location; on RISC-V it is the \c tp register.
    \note      Use GetTlsPtr<T>() for a type-safe wrapper that returns a typed pointer.
    \warning   ISR-unsafe in the sense that the TP register holds the \e current task's context;
               reading it from an ISR will return the interrupted task's TLS, not an ISR-specific one.
*/
Word GetTls();

/*! \brief     Write raw thread-pointer (TP) register used as per-task TLS storage.
    \param[in] tp: New TP value to store.
    \note      Called by the scheduler during every context switch to update the register
               to the incoming task's TLS pointer. Not intended for direct use in application code;
               use SetTlsPtr<T>() instead.
*/
void SetTls(Word tp);

#endif // _STK_INLINE_TLS_DEFINED

/*! \brief     Type-safe wrapper around GetTls() that casts the raw TP value to a typed pointer.
    \tparam    _TyTls: The type pointed to by the TLS register (typically the per-task kernel context struct).
    \return    Pointer to the current task's TLS object, or \c nullptr if TLS has not been set.
    \note      Equivalent to \c reinterpret_cast<_TyTls*>(GetTls()). Prefer this over calling
               GetTls() directly to avoid scattered reinterpret_casts throughout the codebase.
*/
template <class _TyTls>
__stk_forceinline _TyTls *GetTlsPtr()
{
    return hw::WordToPtr<_TyTls>(GetTls());
}

/*! \brief     Type-safe wrapper around SetTls() that stores a typed pointer as the raw TP value.
    \tparam    _TyTls: The type pointed to by the TLS register (typically the per-task kernel context struct).
    \param[in] tp: Pointer to the new task's TLS object.
    \note      Equivalent to \c SetTls(reinterpret_cast<Word>(tp)). Called by the scheduler
               during context switches to install the incoming task's TLS pointer.
*/
template <class _TyTls>
__stk_forceinline void SetTlsPtr(const _TyTls *tp)
{
    SetTls(hw::PtrToWord(tp));
}

/*! \class CriticalSection
    \brief Nestable, SMP-safe critical section that combines local interrupt masking with a
           global cross-core spinlock.

    \note  \b Mechanism (two-layer protocol):
           -# Local interrupts are masked on the calling core (via PRIMASK / CPSID on Cortex-M,
              or equivalent on other architectures) to prevent re-entrant ISR access on this core.
           -# A global spinlock (\c g_CsuLock) is then acquired to block any other core from
              entering the same critical section concurrently.
           On Enter() the spinlock is only acquired at nesting depth 0 so that nested Enter()
           calls from the same core do not deadlock. On Exit(), the spinlock is released and
           interrupts are restored only when the outermost Exit() brings the nesting counter
           back to zero.
    \note  \b SMP support: safe across multiple cores. The global spinlock ensures that only one
           core at a time can hold the section, regardless of interrupt state on other cores.
           On RP2040, the global lock is a hardware SIO peripheral spinlock rather than a
           software atomic, providing the cross-core guarantee with no software polling overhead
           until contention occurs.
    \note  \b Unprivileged mode: on Cortex-M targets with TrustZone / privilege separation,
           Enter() and Exit() escalate via SVC when called from an unprivileged thread, so the
           mechanism works correctly in both privileged and unprivileged task contexts.
    \note  Keep critical sections as short as possible. Every cycle spent holding the section
           blocks all other cores and increases interrupt latency, which can cause missed
           deadlines in HRT mode.
    \see   SpinLock
*/
class CriticalSection
{
public:
    /*! \class ScopedLock
        \brief RAII guard that enters the critical section on construction and exits it on destruction.
        \note  Guarantees Exit() is always called even if an early return or exception unwinds the scope.

        Usage example:
        \code
        {
            CriticalSection::ScopedLock lock;

            // shared resource access protected here
        } // CriticalSection::Exit() called automatically
        \endcode
    */
    class ScopedLock
    {
    public:
        /*! \brief Enter the critical section.
        */
        explicit ScopedLock() { CriticalSection::Enter(); }

        /*! \brief Exit the critical section.
        */
        ~ScopedLock() { CriticalSection::Exit(); }

    private:
        STK_NONCOPYABLE_CLASS(ScopedLock);
    };

    /*! \brief   Enter a critical section.
        \note    Masks local interrupts on this core and, at nesting depth 0, acquires the global
                 cross-core spinlock. Subsequent nested Enter() calls on the same core increment
                 the nesting counter without re-acquiring the spinlock, so nesting is safe.
        \warning Every Enter() must be paired with exactly one Exit(). A missing Exit() leaves
                 local interrupts masked and the global spinlock held permanently, stalling all
                 other cores and the scheduler. Prefer ScopedLock to avoid mismatched pairs.
    */
    static void Enter();

    /*! \brief   Exit a critical section.
        \note    Decrements the nesting counter. When it reaches zero (outermost Exit()), releases
                 the global cross-core spinlock first and then restores local interrupt masking to
                 the state captured at the matching Enter().
        \warning Must only be called after a matching Enter(). Calling Exit() without a prior Enter()
                 produces undefined behaviour (nesting counter underflow, caught by assertion in
                 debug builds).
    */
    static void Exit();

private:
    explicit CriticalSection() {}
    STK_NONCOPYABLE_CLASS(CriticalSection);
};

/*! \class     SpinLock
    \brief     Atomic busy-wait lock used as the global cross-core synchronisation primitive
               inside CriticalSection.
    \note      Implemented using an atomic test-and-set (or hardware spinlock peripheral on RP2040)
               so that it is safe across multiple CPU cores. CriticalSection::Enter() acquires
               this lock (via \c g_CsuLock) after masking local interrupts, giving the combined
               interrupt-mask + cross-core guarantee described in CriticalSection.
    \note      SpinLock is exposed as a public API for use cases that need a bare cross-core lock
               without interrupt masking, for example protecting data shared only between two
               tasks on different cores where ISR access is not a concern.
    \note      Use only for very short, low-latency critical sections. Spinning wastes CPU cycles
               and can increase interrupt latency and power consumption.
    \note      Non-recursive: calling Lock() twice from the same thread/core without an intervening
               Unlock() will deadlock. The ARM implementation includes a debug-break timeout
               (0xFFFFFF iterations) to catch lock-not-released bugs in debug builds.
    \see       CriticalSection
*/
class SpinLock
{
public:
    /*! \enum  EState
        \brief Internal lock state values.
    */
    enum EState
    {
        UNLOCKED = 0, //!< Lock is free and available for acquisition.
        LOCKED        //!< Lock is held by a thread or core.
    };

    /*! \brief Construct a SpinLock (unlocked by default).
    */
    explicit SpinLock() : m_lock(UNLOCKED)
    {}

    /*! \brief   Acquire SpinLock, blocking until it is available.
        \note    Busy-waits (spins) using __stk_relax_cpu() until the lock transitions to UNLOCKED
                 and this call wins the atomic acquisition.
        \warning Non-recursive. Calling Lock() a second time from the same thread/core while
                 already holding the lock will spin forever (deadlock).
        \warning Calling Lock() from an ISR while the interrupted task holds the same lock will
                 also deadlock. Prefer CriticalSection for ISR-to-task synchronisation.
    */
    void Lock();

    /*! \brief   Release SpinLock, allowing another thread or core to acquire it.
        \note    The lock transitions immediately to UNLOCKED. If another core is spinning in Lock(),
                 it will acquire the lock on its next successful atomic attempt.
        \warning Must only be called by the thread or core that currently holds the lock (via Lock()
                 or a successful TryLock()). Calling Unlock() without a prior acquisition produces
                 undefined behaviour.
    */
    void Unlock();

    /*! \brief  Attempt to acquire SpinLock in a single non-blocking attempt.
        \return \c true if the lock was acquired; \c false if it was already held by another thread/core.
        \note   Returns immediately regardless of lock state. On success the caller holds the lock
                and must call Unlock() when done. On failure the lock state is unchanged.
        \note   Useful in try-acquire / back-off patterns or when a fallback action is available
                if the resource is busy.
    */
    bool TryLock();

    /*! \brief  Sample current lock state.
        \return \c true if the lock is currently held; \c false if it is free.
        \note   The result is a snapshot only. On SMP systems another core may acquire or release
                the lock between this read and any subsequent action, so IsLocked() must not be
                used as a synchronisation check. Use TryLock() or Lock() for safe acquisition.
    */
    bool IsLocked() const { return (m_lock == LOCKED); }

protected:
    STK_NONCOPYABLE_CLASS(SpinLock);

#ifdef _STK_ARCH_X86_WIN32
    volatile long m_lock; //!< Lock state (see EState). \c long required by Win32 Interlocked API (InterlockedCompareExchange).
#else
    volatile bool m_lock __stk_aligned(8); //!< Lock state (see EState). 8-byte aligned to occupy its own cache line word and avoid false sharing on SMP targets.
#endif
};

/*! \brief     Atomically read a 64-bit volatile value.
    \tparam    T: Must be exactly 8 bytes wide and at least 4-byte aligned (enforced by static assertions).
    \param[in] addr: Pointer to the volatile 64-bit memory location.
    \return    Consistent 64-bit snapshot of the value at \a addr.
    \note      On 64-bit architectures a single aligned load is inherently atomic, the value is
               read directly with no additional protocol.
    \note      On 32-bit architectures uses a lock-free hi-lo retry protocol compatible with
               WriteVolatile64: reads the high half, then the low half, then re-reads the high
               half. If the high half changed between the first and second read, a tick occurred
               between the two 32-bit loads and the pair is retried until a consistent snapshot
               is obtained. Requires that WriteVolatile64 writes hi before lo (which it does).
    \note      Requires a \b single writer that uses WriteVolatile64. Safe with multiple concurrent
               readers. Not C++ memory-model compliant, intended for bare-metal embedded use only.
    \note      MISRA deviation: [STK-DEV-002] Rule 5-2-7, 5-0-15. Required for lock-free 64-bit I/O
               on 32-bit architectures using a Hi-Lo retry protocol.
    \warning   Not safe for read-modify-write operations. Use only for polling a value written
               atomically by a single producer.
    \see       WriteVolatile64
*/
template <typename T>
__stk_forceinline T ReadVolatile64(volatile const T *addr)
{
    STK_STATIC_ASSERT_N(sz, sizeof(T) == 8);  // only 64-bit types permitted
    STK_STATIC_ASSERT_N(al, alignof(T) >= 4); // type must be at least 4-byte aligned

    if (sizeof(void *) == 8) // 64-bit arch: aligned 64-bit load is inherently atomic
    {
        return (*addr);
    }
    else
    {
        // 32-bit arch: split the 64-bit address into two 32-bit halves.
        // Writer always updates hi before lo (see WriteVolatile64), so if hi is
        // the same before and after reading lo, no write straddled the two reads.
        volatile const uint32_t *plo = &((volatile const uint32_t *)addr)[STK_ENDIAN_IDX_LO];
        volatile const uint32_t *phi = &((volatile const uint32_t *)addr)[STK_ENDIAN_IDX_HI];

        uint32_t hi, lo;
        do
        {
            hi = (*phi);
            __stk_full_memfence();

            lo = (*plo);
            __stk_full_memfence();
        }
        while (hi != (*phi)); // hi changed: a write occurred during the read; retry

        return ((uint64_t)hi << 32) | lo;
    }
}

/*! \brief     Atomically write a 64-bit volatile value.
    \tparam    T: Must be exactly 8 bytes wide and at least 4-byte aligned (enforced by static assertions).
    \param[in] addr:  Pointer to the volatile 64-bit memory location.
    \param[in] value: Value to write.
    \note      On 64-bit architectures a single aligned store is inherently atomic.
    \note      On 32-bit architectures the value is split into two 32-bit half-writes.
               The high half is always written \b before the low half. This ordering is the
               contractual invariant that ReadVolatile64 depends on to detect a mid-write tear:
               if a reader observes a changed high half, it knows a write occurred and retries.
               Breaking this order (writing lo before hi) will corrupt concurrent reads.
    \note      A full memory fence is emitted between the two half-writes to prevent the CPU
               from reordering the stores.
    \note      ISR-safe: does not use a critical section.
    \note      MISRA deviation: [STK-DEV-002] Rule 5-2-7, 5-0-15. Required for lock-free 64-bit I/O
               on 32-bit architectures using a Hi-Lo retry protocol.
    \warning   Supports only a \b single writer at a time. Concurrent writers on the same address
               will corrupt the value. Not safe for read-modify-write operations (e.g. increment);
               the caller must ensure exclusive write access by other means.
    \warning   Not C++ memory-model compliant; intended for bare-metal embedded use only.
    \see       ReadVolatile64
*/
template <typename T>
__stk_forceinline void WriteVolatile64(volatile T *addr, T value)
{
    STK_STATIC_ASSERT_N(sz, sizeof(T) == 8);  // only 64-bit types permitted
    STK_STATIC_ASSERT_N(al, alignof(T) >= 4); // type must be at least 4-byte aligned

    if (sizeof(void *) == 8) // 64-bit arch: aligned 64-bit store is inherently atomic
    {
        (*addr) = value;
    }
    else
    {
        volatile uint32_t *plo = &((volatile uint32_t *)addr)[STK_ENDIAN_IDX_LO];
        volatile uint32_t *phi = &((volatile uint32_t *)addr)[STK_ENDIAN_IDX_HI];

        // Write hi first: ReadVolatile64 reads hi twice and retries if it changed,
        // so writing hi before lo ensures readers can detect a torn write.
        (*phi) = (uint32_t)(value >> 32);
        __stk_full_memfence();

        (*plo) = (uint32_t)value;
    }
}

} // namespace hw
} // namespace stk

#ifndef STK_PANIC_HANDLER
    /*! \brief Default panic handler: disable interrupts, record the id,
               and spin in a tight loop — a defined, detectable safe state.
        \note  On a system with a watchdog enabled this will trigger a watchdog
               reset after the watchdog period, which is the desired behaviour.
        \note  Replace with a platform-specific handler (e.g. one that writes a
               fault log to non-volatile memory and calls NVIC_SystemReset()) by
               defining STK_PANIC_HANDLER in stk_config.h.
    */
    extern void STK_PANIC_HANDLER_DEFAULT(stk::EKernelPanicId id);
    #define STK_PANIC_HANDLER(id) STK_PANIC_HANDLER_DEFAULT(id)
#endif

#endif /* STK_ARCH_H_ */
