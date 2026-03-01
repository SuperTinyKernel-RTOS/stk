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

/*! \def   STK_NEED_TASK_ID
    \brief When defined as 1, the Stack descriptor (stk::Stack) carries a \c tid field
           used by the SEGGER SystemView trace back-end to identify tasks during context switches.
    \note  Automatically set to 1 when STK_SEGGER_SYSVIEW is enabled. Not intended for manual override.
    \see   STK_SEGGER_SYSVIEW, stk::Stack
*/
#if STK_SEGGER_SYSVIEW
    #define STK_NEED_TASK_ID 1
#endif

/*! \def   STK_SYNC_DEBUG_NAMES
    \brief Enable debug names for synchronization primitives (mutexes, events, etc.) for debugging and tracing purposes.
    \note  When set to 1, synchronization objects gain a string name field (see ITraceable::SetTraceName)
           that is visible in SEGGER SystemView and other trace tools.
    \note  Automatically set to 1 when STK_SEGGER_SYSVIEW is enabled. Can be manually defined in stk_config.h to override the default.
    \note  Default: 0 (disabled) unless STK_SEGGER_SYSVIEW is active.
*/
#if !defined(STK_SYNC_DEBUG_NAMES) && STK_SEGGER_SYSVIEW
    #define STK_SYNC_DEBUG_NAMES 1
#elif !defined(STK_SYNC_DEBUG_NAMES)
    #define STK_SYNC_DEBUG_NAMES 0
#endif

/*! \def   __stk_forceinline
    \brief Forces compiler to always inline the decorated function, regardless of optimisation level.
    \note  Used on latency-critical paths (ISR handlers, scheduler hot paths) where a function call
           overhead would be unacceptable or would unpredictably affect real-time timing.
    \note  On compilers not listed below the attribute expands to nothing (inlining becomes a hint only).
*/
#ifdef __GNUC__
    #define __stk_forceinline __attribute__((always_inline)) inline
#elif defined(__ICCARM__) || defined(_MSC_VER)
    #define __stk_forceinline __forceinline
#else
    #define __stk_forceinline
#endif

/*! \def       __stk_aligned
    \brief     Specifies minimum alignment in bytes for the decorated variable or struct member (data instance prefix).
    \param[in] x: Required alignment in bytes. Must be a power of two.
    \note      On compilers not listed below the attribute expands to nothing and alignment is not enforced.
               Verify alignment-sensitive data on new toolchains.
*/
#ifdef __GNUC__
    #define __stk_aligned(x) __attribute__((aligned(x)))
#elif defined(__ICCARM__)
    #define __stk_aligned(x) __attribute__((aligned(x)))
#else
    #define __stk_aligned(x)
#endif

/*! \def   __stk_attr_naked
    \brief Suppresses compiler-generated function prologue and epilogue (function prefix).
    \note  The decorated function must consist of inline assembly only. C statements that rely
           on a stack frame (local variables, non-trivial expressions, function calls) produce
           undefined behaviour. Used for context-switch stubs and ISR entry points where the
           register save/restore sequence must be precisely hand-written.
*/
#ifdef __GNUC__
    #define __stk_attr_naked __attribute__((naked))
#elif defined(__ICCARM__)
    #define __stk_attr_naked __attribute__((naked))
#else
    #define __stk_attr_naked
#endif

/*! \def   __stk_attr_noreturn
    \brief Declares that function never returns to its caller (function prefix).
    \note  Enables compiler to omit return-path code and dead-store warnings after the call site.
    \warning Applying this attribute to a function that does return produces undefined behaviour.
*/
#ifdef __GNUC__
    #define __stk_attr_noreturn __attribute__((__noreturn__))
#elif defined(__ICCARM__)
    #define __stk_attr_noreturn __attribute__((noreturn))
#else
    #define __stk_attr_noreturn
#endif

/*! \def   __stk_attr_unused
    \brief Suppresses compiler warnings about an unused type, variable, or function (declaration prefix).
    \note  Does not prevent the linker from discarding the symbol. Use __stk_attr_used when the symbol
           must be retained regardless of whether it appears to be referenced.
*/
#ifdef __GNUC__
    #define __stk_attr_unused __attribute__((unused))
#elif defined(__ICCARM__)
    #define __stk_attr_unused __attribute__((unused))
#else
    #define __stk_attr_unused
#endif

/*! \def   __stk_attr_used
    \brief Marks a symbol as used, preventing the linker from discarding it even if no references are visible (declaration prefix).
    \note  Commonly applied to ISR vector table entries, trap stubs, and objects placed in special
           linker sections that are referenced only from assembly or linker scripts.
*/
#ifdef __GNUC__
    #define __stk_attr_used __attribute__((used))
#elif defined(__ICCARM__)
    #define __stk_attr_used __attribute__((used))
#else
    #define __stk_attr_used
#endif

/*! \def   __stk_attr_noinline
    \brief Prevents compiler from inlining the decorated function (function prefix).
    \note  Used where inlining would obscure stack-depth analysis, produce unpredictable code size,
           or make profiling and tracing results misleading.
*/
#ifdef __GNUC__
    #define __stk_attr_noinline __attribute__((noinline))
#elif defined(__ICCARM__)
    #define __stk_attr_noinline __attribute__((noinline))
#else
    #define __stk_attr_noinline
#endif

/*! \def   __stk_attr_deprecated
    \brief Marks a function, class, variable, or typedef as deprecated (declaration prefix).
    \note  The compiler will emit a warning at every call site or use of the decorated symbol,
           prompting callers to migrate to the replacement API.
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

/*! \def     __stk_unreachable
    \brief   Informs compiler that a code path is logically unreachable (in-code statement).
    \note    Enables dead-code elimination and suppresses "control reaches end of non-void function" warnings.
    \warning If execution reaches this point at runtime the behaviour is undefined with no diagnostic.
             Use only where the path is provably unreachable (e.g. after an exhaustive switch).
*/
#ifdef __GNUC__
    #define __stk_unreachable() __builtin_unreachable()
#else
    #error "__stk_unreachable() is not implemented for this compiler. Add a definition to stk_defs.h."
#endif

/*! \def   __stk_full_memfence
    \brief Emits a full (sequentially-consistent) memory barrier (in-code statement).
    \note  Prevents both the compiler and the CPU from reordering memory accesses across this point.
           Used to enforce visibility ordering between cores or between a task and an ISR without
           entering a critical section.
*/
#ifdef __GNUC__
    #define __stk_full_memfence() __sync_synchronize()
#else
    #error "__stk_full_memfence() is not implemented for this compiler. Add a definition to stk_defs.h."
#endif

/*! \def   __stk_relax_cpu
    \brief Emits a CPU pipeline-relaxation hint for use inside hot busy-wait (spin) loops (in-code statement).
    \note  Reduces power consumption and memory-bus contention while spinning by signalling to the CPU
           that the current thread is in a spin-wait. On x86 this maps to the PAUSE instruction;
           on RISC-V with Zihintpause it maps to the PAUSE hint, on other targets it falls back to
           a full memory fence which at minimum prevents compiler reordering.
    \note  Can be redefined externally (e.g. in test harnesses) to intercept control inside kernel
           waiting loops without modifying kernel source.
*/
#ifndef __stk_relax_cpu
#ifdef __GNUC__
    #if defined(__i386__) || defined(__x86_64__)
        #define __stk_relax_cpu() __builtin_ia32_pause()
    #elif defined(__riscv)
        #ifdef __riscv_zihintpause
            #define __stk_relax_cpu() __builtin_riscv_pause()
        #else
            #define __stk_relax_cpu() __stk_full_memfence() // Zihintpause not available, memory fence used as fallback
        #endif
    #else
        #define __stk_relax_cpu() __stk_full_memfence()
    #endif
#else
    #error "__stk_relax_cpu() is not implemented for this compiler. Add a definition to stk_defs.h."
#endif
#endif

/*! \def   __stk_debug_break
    \brief Triggers a hardware breakpoint, halting execution in an attached debugger (in-code statement).
    \note  Active only when \c DEBUG or \c _DEBUG is defined (i.e. in debug builds).
           In release builds, or when no known architecture is detected, expands to nothing.
    \note  Used in assertion handlers and fault paths to halt the system at the exact failure point
           rather than continuing into undefined state.
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
    \brief Runtime assertion. Halts execution if the expression \a e evaluates to false.
    \note  By default maps to the standard \c assert() macro from \c <assert.h>.
           Define \c _STK_ASSERT_REDIRECT to redirect to a custom handler \c STK_ASSERT_IMPL
           with the signature: \code void STK_ASSERT_IMPL(const char *expr, const char *file, int32_t line); \endcode
           This is useful for embedded targets where the standard assert handler is unavailable
           or where a custom fault logger or LED indicator is preferred.
*/
#ifdef _STK_ASSERT_REDIRECT
    extern void STK_ASSERT_IMPL(const char *, const char *, int32_t);
    #define STK_ASSERT(e) ((e) ? (void)0 : STK_ASSERT_IMPL(#e, __FILE__, __LINE__))
#else
    #include <assert.h>
    #define STK_ASSERT(e) assert(e)
#endif

/*! \def   STK_STATIC_ASSERT_N
    \brief Compile-time assertion with a user-defined name suffix.
    \note  The \a NAME parameter is appended to the internal typedef name, allowing multiple
           assertions in the same scope without symbol-name collisions. Use STK_STATIC_ASSERT
           for single assertions where a unique name is not required.
*/
#define STK_STATIC_ASSERT_N(NAME, X) typedef char __stk_static_assert_##NAME[(X) ? 1 : -1] __stk_attr_unused

/*! \def   STK_STATIC_ASSERT
    \brief Compile-time assertion. Produces a compilation error if \a X is false.
    \note  Uses a fixed internal name. If multiple STK_STATIC_ASSERT calls appear in the same
           scope, use STK_STATIC_ASSERT_N to provide distinct names and avoid duplicate typedef errors.
*/
#define STK_STATIC_ASSERT(X) STK_STATIC_ASSERT_N(_, X)

/*! \def   STK_STACK_MEMORY_FILLER
    \brief Sentinel value written to the entire stack region at initialization (stack watermark pattern).
    \note  Used to detect stack overflow and to measure peak stack usage at run-time: any stack word
           that still contains this value was never written by the task.
    \note  Defaults to \c 0xDEADBEEF on 32-bit targets and \c 0xDEADBEEFDEADBEEF on 64-bit targets.
           Can be overridden by defining STK_STACK_MEMORY_FILLER before including this header or in stk_config.h.
*/
#ifndef STK_STACK_MEMORY_FILLER
    #define STK_STACK_MEMORY_FILLER ((size_t)(sizeof(size_t) <= 4 ? 0xdeadbeef : 0xdeadbeefdeadbeef))
#endif

/*! \def   _STK_ARCH_CPU_COUNT
    \brief Number of physical CPU cores available to the scheduler (default: 1).
    \note  Controls the number of per-CPU kernel service instances and per-CPU data structures
           allocated by the kernel. Set to the actual core count for SMP (symmetric multi-processing)
           targets. Can be defined in the architecture header or stk_config.h.
*/
#ifndef _STK_ARCH_CPU_COUNT
    #define _STK_ARCH_CPU_COUNT 1
#endif

/*! \def   STK_STACK_SIZE_MIN
    \brief Minimum stack size in elements of \c size_t, shared by all stack allocation lower-bound checks.
    \see   TrapStackMemory
    \note  This is the smallest stack that can correctly save and restore all CPU registers during
           a context switch or service trap. The required size depends on the number of registers
           the architecture mandates saving.
    \note  Default values by architecture:
           - Non-RISC-V and standard RISC-V (RV32I / RV64): 32 elements.
           - RISC-V RV32E (embedded, __riscv_32e != 0) without FPU: 256 elements
             (smaller sizes cause memory corruption on RP2350).
           - RISC-V RV32E with FPU (__riscv_flen != 0): 512 elements (FP registers double the frame size).
    \note  Can be overridden to any larger value in stk_config.h if your application's
           interrupt nesting or stack frame size requires it.
*/
#ifndef STK_STACK_SIZE_MIN
    #if (__riscv_32e == 0) // non-RISC-V or standard RISC-V (RV32I/RV64): small register file
        #define STK_STACK_SIZE_MIN 32
    #else                  // RISC-V RV32E: embedded reduced register file
        #if (__riscv_flen == 0) // no FPU
            #define STK_STACK_SIZE_MIN 256 // smaller sizes cause memory corruption on RP2350
        #else                   // with FPU: floating-point registers require larger frame
            #define STK_STACK_SIZE_MIN 512
        #endif
    #endif
#endif

/*! \def   STK_SLEEP_TRAP_STACK_SIZE
    \brief Stack size for the sleep trap in elements of \c size_t (default: STK_STACK_SIZE_MIN).
    \see   Kernel::SleepTrapStackMemory
    \note  If IEventOverrider::OnSleep() is overridden with a non-trivial implementation (e.g.
           calling platform-specific low-power APIs that use the stack), increase this value
           to accommodate the additional stack frame depth required by that implementation.
           Can be defined in stk_config.h.
*/
#ifndef STK_SLEEP_TRAP_STACK_SIZE
    #define STK_SLEEP_TRAP_STACK_SIZE (STK_STACK_SIZE_MIN)
#endif

/*! \def   STK_ALLOCATE_COUNT
    \brief Selects a static array element count at compile time based on a mode flag.
    \note  On GCC/Clang: expands to ONTRUE if (MODE & FLAG) is non-zero, otherwise ONFALSE.
           On MSVC: always expands to max(ONTRUE, ONFALSE) because MSVC does not support
           zero-sized arrays. This means MSVC builds may over-allocate when the flag is not set,
           but avoids a compile error. Configuration mistakes that would result in ONFALSE on
           other compilers will be silently masked on MSVC.
    \param[in] MODE: Bitmask of active kernel modes (e.g. EKernelMode flags).
    \param[in] FLAG: The specific mode bit to test.
    \param[in] ONTRUE: Array count to use when FLAG is active.
    \param[in] ONFALSE: Array count to use when FLAG is inactive (may be 0 on GCC/Clang).
*/
#ifdef _MSC_VER
    #define STK_ALLOCATE_COUNT(MODE, FLAG, ONTRUE, ONFALSE) ((ONTRUE) > (ONFALSE) ? (ONTRUE) : (ONFALSE))
#else
    #define STK_ALLOCATE_COUNT(MODE, FLAG, ONTRUE, ONFALSE) ((MODE) & (FLAG) ? (ONTRUE) : (ONFALSE))
#endif

/*! \def     STK_MIN
    \brief   Returns the smaller of two values.
    \warning Arguments are evaluated twice. Do not pass expressions with side effects (e.g. function calls, increments).
*/
#define STK_MIN(A, B) ((A) < (B) ? (A) : (B))

/*! \def     STK_MAX
    \brief   Returns the larger of two values.
    \warning Arguments are evaluated twice. Do not pass expressions with side effects (e.g. function calls, increments).
*/
#define STK_MAX(A, B) ((A) > (B) ? (A) : (B))

/*! \def   STK_ENDIAN_IDX_HI
    \brief Array index of the high 32-bit word when a 64-bit value is viewed as \c uint32_t[2].
    \note  Big-endian: 0 (high word first). Little-endian: 1 (high word second).
    \see   STK_ENDIAN_IDX_LO, hw::ReadVolatile64, hw::WriteVolatile64
*/
/*! \def   STK_ENDIAN_IDX_LO
    \brief Array index of the low 32-bit word when a 64-bit value is viewed as \c uint32_t[2].
    \note  Big-endian: 1 (low word second). Little-endian: 0 (low word first).
    \see   STK_ENDIAN_IDX_HI, hw::ReadVolatile64, hw::WriteVolatile64
*/
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    #define STK_ENDIAN_IDX_HI (0) // big-endian: high word at index 0
    #define STK_ENDIAN_IDX_LO (1) // big-endian: low word at index 1
#else
    #define STK_ENDIAN_IDX_HI (1) // little-endian (default): high word at index 1
    #define STK_ENDIAN_IDX_LO (0) // little-endian (default): low word at index 0
#endif

/*! \namespace stk
    \brief     Namespace of STK package.
 */
namespace stk {

/*! \namespace stk::util
    \brief     Internal utility namespace containing data structure helpers (linked lists, etc.)
               used by the kernel implementation. Not part of the public user API.
 */
namespace util {}

/*! \brief     Reinterpret the bit pattern of \a from as type \c _To without invoking undefined
               behaviour from \c reinterpret_cast on unrelated pointer types.
    \tparam    _To: Target type. Must be trivially copyable.
    \tparam    _From: Source type. Must be trivially copyable.
    \param[in] from: Source value to reinterpret.
    \return    The same bit pattern as \a from, interpreted as \c _To.
    \warning   \c sizeof(_To) must equal \c sizeof(_From); no compile-time check is performed here.
               Mismatched sizes will read uninitialised union bytes, producing undefined behaviour.
    \warning   Not valid for pointer-to-pointer conversions between unrelated types in strict C++.
               Intended for bare-metal embedded use only where the target type layout is known.
    \note      Declared \c static to give each translation unit its own copy and avoid ODR issues
               with the anonymous union across compilation units.
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
