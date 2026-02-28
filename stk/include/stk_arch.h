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
    \brief Contains platform implementations (IPlatform).
*/

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

/*! \namespace stk::hw
    \brief     Hardware Abstraction Layer (HAL) for architecture-specific operations.

    This namespace contains low-level functions that interface directly with the
    CPU registers and hardware state. These functions are typically implemented
    in assembly or using compiler intrinsics for maximum performance and
    minimum jitter.

    \see GetTls, SetTls, CriticalSection, SpinLock
*/
namespace hw {

/*! \brief     Check if CPU is currently executing in an interrupt context.
    \return    \c true if executing inside an ISR; \c false if in Task context.
*/
bool IsInsideISR();

#if !_STK_INLINE_TLS_DEFINED

/*! \brief     Get thread-local storage (TLS).
    \return    TLS value.
*/
uintptr_t GetTls();

/*! \brief     Set thread-local storage (TLS).
    \param[in] tp: TLS value.
*/
void SetTls(uintptr_t tp);

#endif // _STK_INLINE_TLS_DEFINED

/*! \brief     Get thread-local storage (TLS) pointer.
    \return    TLS pointer of _TyTls type.
*/
template <class _TyTls>
__stk_forceinline _TyTls *GetTlsPtr()
{
    return reinterpret_cast<_TyTls *>(GetTls());
}

/*! \brief     Set thread-local storage (TLS) pointer.
    \param[in] tp: TLS pointer of _TyTls type.
*/
template <class _TyTls>
__stk_forceinline void SetTlsPtr(const _TyTls *tp)
{
    SetTls(reinterpret_cast<uintptr_t>(tp));
}

/*! \class SpinLock
    \brief Minimal SpinLock implementation for short critical sections.
    \note  Use only for very short, low-latency critical sections.
           Spinning blocks the CPU and may affect real-time behavior.
*/
struct CriticalSection
{
    /*! \class ScopedLock
        \brief Enters critical section and exits from it automatically within the scope of execution.

        Usage example:
        \code
        {
            CriticalSection::ScopedLock __my_cs;

            // code below is free from race condition
        }
        \endcode
    */
    struct ScopedLock
    {
        explicit ScopedLock() { CriticalSection::Enter(); }
        ~ScopedLock() { CriticalSection::Exit(); }
    };

    /*! \brief Enter a critical section.
        \note  Disables preemption to protect shared resources.
               Supports nesting: multiple calls from the same context are safe.
               Use sparingly, as long critical sections can affect real-time behavior.
    */
    static void Enter();

    /*! \brief Exit a critical section.
        \note  Must be called after CriticalSection::Enter().
               Each call to CriticalSection::Enter() must be matched with one CriticalSection::Exit().
    */
    static void Exit();
};

/*! \class     SpinLock
    \brief     Minimal SpinLock implementation for short critical sections.
    \note      Use only for very short, low-latency critical sections.
               Spinning blocks the CPU and may affect real-time behavior.
*/
class SpinLock
{
public:
    enum EState
    {
        UNLOCKED = 0,
        LOCKED
    };

    /*! \brief Construct a SpinLock (unlocked by default).
    */
    SpinLock() : m_lock(UNLOCKED)
    {}

    /*! \brief Acquire the SpinLock.
        \note  Blocks (busy-waits) until the lock is acquired.
               Non-recursive: calling Lock() twice on the same lock from the same thread/core will deadlock.
    */
    void Lock();

    /*! \brief Release the SpinLock.
        \note  Must be called after a successful Lock().
               Releases the lock immediately, allowing other threads/cores to acquire it.
    */
    void Unlock();

    /*! \brief  Attempt to acquire the SpinLock without blocking.
        \return True if the lock was successfully acquired; false otherwise.
        \note   Non-blocking. Does not guarantee acquisition. Use only for short critical sections or retry loops.
    */
    bool TryLock();

    /*! \brief  Check if locked.
        \return True if locked.
        \note   The value may change at any time.
    */
    bool IsLocked() const { return (m_lock == LOCKED); }

protected:
#ifdef _STK_ARCH_X86_WIN32
    volatile long m_lock; //! lock state (see EState)
#else
    volatile bool m_lock __stk_aligned(8); //! lock state (see EState)
#endif
};

/*! \brief     Read 64-bit volatile value atomically.
    \param[in] addr: Pointer to the volatile memory location.
    \return    Value.
    \see       WriteAtomic64
*/
template <typename T>
__stk_forceinline T ReadAtomic64(volatile const T *addr)
{
    // only for 64-bit value
    STK_STATIC_ASSERT(sizeof(T) == 8);

    if (sizeof(void *) == 8) // atomic on 64-bit arch
    {
        return (*addr);
    }
    else
    {
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
        while (hi != (*phi)); // make sure phi did not tick when read plo

        return ((uint64_t)hi << 32) | lo;
    }
}

/*! \brief     Writes 64-bit value atomically.
    \param[in] addr: Pointer to the volatile memory location.
    \param[in] value: Value to be written.
    \warning   It is safe only for cases where a single writer sets an arbitrary value and no other
               writer touches the same address concurrently, not for a read-modify-write increment.
    \see       ReadAtomic64
*/
template <typename T>
__stk_forceinline void WriteAtomic64(volatile T *addr, T value)
{
    // only for 64-bit value
    STK_STATIC_ASSERT(sizeof(T) == 8);

    if (sizeof(void *) == 8) // atomic on 64-bit arch
    {
        (*addr) = value;
    }
    else
    {
        volatile uint32_t *plo = &((volatile uint32_t *)addr)[STK_ENDIAN_IDX_LO];
        volatile uint32_t *phi = &((volatile uint32_t *)addr)[STK_ENDIAN_IDX_HI];

        // write hi first (reader reads hi first, see ReadAtomic64)
        (*phi) = (uint32_t)(value >> 32);
        __stk_full_memfence();

        (*plo) = (uint32_t)value;
    }
}

} // namespace hw
} // namespace stk

#endif /* STK_ARCH_H_ */
