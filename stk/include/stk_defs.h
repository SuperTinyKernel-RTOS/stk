/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_DEFS_H_
#define STK_DEFS_H_

#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

/*! \file  stk_defs.h
    \brief Contains compiler low-level definitions.
*/

#if STK_SEGGER_SYSVIEW
    #define STK_NEED_TASK_ID 1
#endif

/*! \def   STK_SYNC_DEBUG_NAMES
    \brief Enable names for synchronization primitives for debugging/tracing purpose.
*/
#if !defined(STK_SYNC_DEBUG_NAMES) && STK_SEGGER_SYSVIEW
    #define STK_SYNC_DEBUG_NAMES 1
#elif !defined(STK_SYNC_DEBUG_NAMES)
    #define STK_SYNC_DEBUG_NAMES 0
#endif

/*! \def   __stk_forceinline
    \brief Inline function (function prefix).
*/
#ifdef __GNUC__
    #define __stk_forceinline __attribute__((always_inline)) inline
#elif defined(__ICCARM__) || defined(_MSC_VER)
    #define __stk_forceinline __forceinline
#else
    #define __stk_forceinline
#endif

/*! \def       __stk_aligned
    \brief     Align data structure to the x bytes (data instance prefix).
    \param[in] x: Alignment value in bytes.
*/
#ifdef __GNUC__
    #define __stk_aligned(x) __attribute__((aligned(x)))
#elif defined(__ICCARM__)
    #define __stk_aligned(x) __attribute__((aligned(x)))
#else
    #define __stk_aligned(x)
#endif

/*! \def   __stk_attr_naked
    \brief Instruct compiler that function does not have prologue and epilogue (function prefix).
*/
#ifdef __GNUC__
    #define __stk_attr_naked __attribute__((naked))
#elif defined(__ICCARM__)
    #define __stk_attr_naked __attribute__((naked))
#else
    #define __stk_attr_naked
#endif

/*! \def   __stk_attr_noreturn
    \brief Instruct compiler that function never returns (function prefix).
*/
#ifdef __GNUC__
    #define __stk_attr_noreturn __attribute__((__noreturn__))
#elif defined(__ICCARM__)
    #define __stk_attr_noreturn __attribute__((noreturn))
#else
    #define __stk_attr_noreturn
#endif

/*! \def   __stk_attr_unused
    \brief Instruct compiler that marked type or object may be unused.
*/
#ifdef __GNUC__
    #define __stk_attr_unused __attribute__((unused))
#elif defined(__ICCARM__)
    #define __stk_attr_unused __attribute__((unused))
#else
    #define __stk_attr_unused
#endif

/*! \def   __stk_attr_used
    \brief Instruct compiler that marked type or object is used.
*/
#ifdef __GNUC__
    #define __stk_attr_used __attribute__((used))
#elif defined(__ICCARM__)
    #define __stk_attr_used __attribute__((used))
#else
    #define __stk_attr_used
#endif

/*! \def   __stk_attr_noinline
    \brief Instruct compiler to not inline the function.
*/
#ifdef __GNUC__
    #define __stk_attr_noinline __attribute__((noinline))
#elif defined(__ICCARM__)
    #define __stk_attr_noinline __attribute__((noinline))
#else
    #define __stk_attr_noinline
#endif

/*! \def   __stk_attr_deprecated
    \brief Instruct compiler to mark function or class as deprecated.
*/
#ifdef __GNUC__
    #define __stk_attr_deprecated __attribute__((deprecated))
#elif defined(__ICCARM__)
    #define __stk_attr_deprecated __attribute__((deprecated))
#elif defined(_MSC_VER)
    #define __stk_attr_deprecated __declspec(deprecated)
#else
    #define __stk_attr_deprecated
#endif

/*! \def   __stk_unreachable
    \brief Instruct compiler that code below it is unreachable (in-code statement).
*/
#ifdef __GNUC__
    #define __stk_unreachable() __builtin_unreachable()
#else
    #error __stk_unreachable()
#endif

/*! \def   __stk_full_memfence
    \brief Full memory barrier.
*/
#ifdef __GNUC__
    #define __stk_full_memfence() __sync_synchronize()
#else
    #error __stk_full_memfence()
#endif

/*! \def   __stk_relax_cpu
    \note  Can be redefined by STK tests to intercept control inside the waiting loops in the Kernel.
    \brief Emits CPU relaxing instruction for usage inside a hot-spinning loop.
*/
#ifndef __stk_relax_cpu
#ifdef __GNUC__
    #if defined(__i386__) || defined(__x86_64__)
        #define __stk_relax_cpu() __builtin_ia32_pause()
    #elif defined(__riscv)
        #ifdef __riscv_zihintpause
            #define __stk_relax_cpu() __builtin_riscv_pause()
        #else
            #define __stk_relax_cpu() __stk_full_memfence()
        #endif
    #else
        #define __stk_relax_cpu() __stk_full_memfence()
    #endif
#else
    #error __stk_relax_cpu()
#endif
#endif

/*! \def   __stk_debug_break
    \brief Breakpoint.
*/
#if defined(DEBUG) || defined(_DEBUG)
    #if defined(_STK_ARCH_ARM_CORTEX_M)
        #define __stk_debug_break() __asm volatile("bkpt 0")
    #elif defined(_STK_ARCH_RISC_V)
        #define __stk_debug_break() __asm volatile("ebreak")
    #elif defined(_STK_ARCH_X86_WIN32)
        #ifdef _MSC_VER
            #define __stk_debug_break() __debugbreak()
        #else
            #define __stk_debug_break() __asm volatile("int $3")
        #endif
    #endif
#else
    #define __stk_debug_break()
#endif

/*! \def   STK_ASSERT
    \brief A shortcut to the assert() function. Can be overridden by the alternative _STK_ASSERT_FUNC
           if _STK_ASSERT_REDIRECT is defined.
*/
#ifdef _STK_ASSERT_REDIRECT
    extern void STK_ASSERT_IMPL(const char *, const char *, int32_t);
    #define STK_ASSERT(e) ((e) ? (void)0 : STK_ASSERT_IMPL(#e, __FILE__, __LINE__))
#else
    #include <assert.h>
    #define STK_ASSERT(e) assert(e)
#endif

/*! \def   STK_STATIC_ASSERT_N
    \brief Complie-time assert with user-defined name.
*/
#define STK_STATIC_ASSERT_N(NAME, X) typedef char __stk_static_assert_##NAME[(X) ? 1 : -1] __stk_attr_unused

/*! \def   STK_STATIC_ASSERT
    \brief Complie-time assert.
*/
#define STK_STATIC_ASSERT(X) STK_STATIC_ASSERT_N(_, X)

/*! \def   STK_STACK_MEMORY_FILLER
    \brief Stack memory filler (stack memory is filled with this value when initialized).
*/
#ifndef STK_STACK_MEMORY_FILLER
    #define STK_STACK_MEMORY_FILLER ((size_t)(sizeof(size_t) <= 4 ? 0xdeadbeef : 0xdeadbeefdeadbeef))
#endif

/*! \def   _STK_ARCH_CPU_COUNT
    \brief Physical CPU count (default: 1).
*/
#ifndef _STK_ARCH_CPU_COUNT
    #define _STK_ARCH_CPU_COUNT 1
#endif

/*! \def   STK_STACK_SIZE_MIN
    \brief Minimal stack size (number of size_t).
    \see   TrapStackStackMemory
    \note  This size forms a minimal possible stack for the service traps of the scheduler. Depending
           on the CPU architecture and the number of supported CPU registers default minimal size may
           not be enough, for example: RISC-V, RV32I requires it to be at least 64, while RV32I + RVF
           doubles this to 128. STK_STACK_SIZE_MIN can be redefined to the desired value in
           stk_config.h file.
*/
#ifndef STK_STACK_SIZE_MIN
    #if (__riscv_32e == 0)
        #define STK_STACK_SIZE_MIN 32
    #else
        #if (__riscv_flen == 0)
            #define STK_STACK_SIZE_MIN 256 // note: smaller size causes memory corruption on RP2350
        #else
            #define STK_STACK_SIZE_MIN 512
        #endif
    #endif
#endif

/*! \def   STK_SLEEP_TRAP_STACK_SIZE
    \brief Stack size for a sleep trap (number of size_t).
    \see   Kernel::SleepTrapStackMemory
    \note  Increase size to the required if sleep trap is overridden with IEventOverrider::OnSleep().
*/
#ifndef STK_SLEEP_TRAP_STACK_SIZE
    #define STK_SLEEP_TRAP_STACK_SIZE (STK_STACK_SIZE_MIN)
#endif

/*! \def   STK_ALLOCATE_COUNT
    \brief Get count of objects to be allocated statically within the array.
    \note  Microsoft compiler does not support zero sized arrays unlike GCC or Clang, thus always allocate.
*/
#ifdef _MSC_VER
    #define STK_ALLOCATE_COUNT(MODE, FLAG, ONTRUE, ONFALSE) ((ONTRUE) > (ONFALSE) ? (ONTRUE) : (ONFALSE))
#else
    #define STK_ALLOCATE_COUNT(MODE, FLAG, ONTRUE, ONFALSE) ((MODE) & (FLAG) ? (ONTRUE) : (ONFALSE))
#endif

/*! \def   STK_MIN
    \brief Get min value.
*/
#define STK_MIN(A, B) ((A) < (B) ? (A) : (B))

/*! \def   STK_MAX
    \brief Get max value.
*/
#define STK_MAX(A, B) ((A) > (B) ? (A) : (B))

/*! \def   STK_ENDIAN_IDX_HI
    \brief Endian dependent high index value.
*/
/*! \def   STK_ENDIAN_IDX_LO
    \brief Endian dependent low index value.
*/
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    #define STK_ENDIAN_IDX_HI (0)
    #define STK_ENDIAN_IDX_LO (1)
#else
    #define STK_ENDIAN_IDX_HI (1)
    #define STK_ENDIAN_IDX_LO (0)
#endif

/*! \namespace stk
    \brief     Namespace of STK package.
 */
namespace stk {

/*! \namespace stk::util
    \brief     Namespace of the helper inventory.
 */
namespace util {}

/*! \fn        forced_cast
    \brief     Perform low-level type punning to reinterpret raw bits as another type.
               Bypasses compiler restrictions/warnings where static_cast or reinterpret_cast might fail.
    \warning   Use with care.
    \tparam    _To: Target type to convert to.
    \tparam    _From: Source type being converted.
    \param[in] from: Reference to the source value.
    \return    Value reinterpreted as type _To.
*/
template <class _To, class _From>
static __stk_forceinline _To forced_cast(const _From &from)
{
    union { _From from; _To to; } cast;
    cast.from = from;
    return cast.to;
}

} // namespace stk

#endif /* STK_DEFS_H_ */
