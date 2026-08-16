/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_DEFS_H_
#define STK_DEFS_H_

#include <cstddef>
#include <cstdint>
#include <algorithm>
#ifdef __ICCARM__
    #include <intrinsics.h>
    #if (__IAR_SYSTEMS_ICC__ < 8)
        #error "Only IAR EWARM 8.0 and higher is supported by STK."
    #endif
#endif

/*! \file  stk_defs.h
    \brief Compiler and platform low-level definitions for STK.
    \note  Includes stk_config.h first so user-level configuration macros (e.g.
           STK_TICKLESS_IDLE, STK_SEGGER_SYSVIEW) are visible to all definitions below.
           Refer to stk_config.h and project examples for configuration details.
*/
#include "stk_config.h"

/*! \def   STK_TICKLESS_IDLE
    \brief Enables tickless (dynamic-tick) low-power operation during idle periods.
    \note  Set to 1 in stk_config.h to activate. Default: 0 (disabled).
    \note  When enabled, the tick timer is suppressed while all tasks are sleeping
           and re-armed for the nearest wakeup deadline, reducing idle power consumption.
    \note  Requires the Kernel template to be instantiated with the KERNEL_TICKLESS flag.
           KERNEL_TICKLESS is incompatible with KERNEL_HRT.
    \note  The platform driver must support tickless operation (i.e. implement the
           variable-advance OnTick overload). Not all platform back-ends support this.
    \see   KERNEL_TICKLESS, STK_TICKLESS_USE_ARM_DWT, STK_TICKLESS_TICKS_MAX
*/
#ifndef STK_TICKLESS_IDLE
    #define STK_TICKLESS_IDLE (0)
#endif
      
/*! \def   STK_STRICT_COMPLIANCY
    \brief Allow the use of workarounds to make binary smaller and faster.
    \note  Applied workarounds will break safety-critical rules (MISRA, etc).
*/
#ifndef STK_STRICT_COMPLIANCY
    #define STK_STRICT_COMPLIANCY (0)
#endif

/*! \def   STK_TICKLESS_USE_ARM_DWT
    \brief Use DWT timer of ARM Cortex-M for a precise tick calculation.
    \note  DWT is available on Cortex-M3 and higher only (__CORTEX_M >= 3).
           It has no effect on Cortex-M0/M0+.
    \note  STK_TICKLESS_USE_ARM_DWT has no effect on RISC-V targets.
           RISC-V tickless uses the CLINT mtime absolute timestamp for drift-free
           timer rearm and does not require rearm-error compensation.
*/
#ifndef STK_TICKLESS_USE_ARM_DWT
    #define STK_TICKLESS_USE_ARM_DWT (1)
#endif

/*! \def   STK_TICKLESS_TICKS_MAX
    \brief Maximum number of kernel ticks the hardware timer may be suppressed in one
           tickless idle interval when STK_TICKLESS_IDLE=1. Default: 1000.
    \note  Must not exceed 100000. Values above this limit cause a compile-time error
           because the internal tick-request calculation uses a uint32_t accumulator
           that would overflow at higher values.
    \note  Increase to allow longer uninterrupted sleep intervals; decrease to improve
           wakeup responsiveness at the cost of more frequent timer interrupts.
    \see   STK_TICKLESS_IDLE, KERNEL_TICKLESS
*/
#ifndef STK_TICKLESS_TICKS_MAX
    #define STK_TICKLESS_TICKS_MAX (1000)
#endif
#if STK_TICKLESS_TICKS_MAX > 100000
    #error "STK_TICKLESS_TICKS_MAX is too large: cpu_ticks_requested may overflow uint32_t."
#endif

/*! \def   STK_TLS
    \brief Enable per-task thread-local storage (TLS).
    \note  When set to 1, the kernel saves and restores a per-task TLS pointer on
           every context switch, making a dedicated pointer available to each task
           at all times via stk::hw::GetTlsPtr/SetTlsPtr.
    \note  Storage strategy is controlled by STK_TLS_PREFER_REGISTER:
           - STK_TLS_PREFER_REGISTER=0 (default): the pointer is kept in the
             stk::Stack::tls member. No special compiler flags required.
           - STK_TLS_PREFER_REGISTER=1: the pointer is kept in a CPU register
             (r9 on Cortex-M, tp on RISC-V) for single-instruction access.
             Cortex-M additionally requires -ffixed-r9 for all task translation
             units; see STK_TLS_PREFER_REGISTER.
    \note  Default: 0 (disabled).
    \see   STK_TLS_PREFER_REGISTER, stk::hw::GetTlsPtr, stk::hw::SetTlsPtr
*/
#ifndef STK_TLS
    #define STK_TLS (0)
#endif

/*! \def   STK_TLS_PREFER_REGISTER
    \brief Store the per-task TLS pointer in a dedicated CPU register instead of
           the stk::Stack::tls member, enabling single-instruction TLS access.
    \note  **ARM Cortex-M:** uses r9 (ARM EABI "platform register", AAPCS 5.2.2).
           Every translation unit that contains task code \e must be compiled with
           \c -ffixed-r9. Without it the compiler may allocate r9 as an ordinary
           callee-saved register, silently overwriting the TLS pointer after a
           context switch. See stk_arch_arm-cortex-m.h for the full explanation.
    \note  **RISC-V:** uses the \c tp (x4) register, which is reserved for TLS by
           the RISC-V psABI. STK_TLS_PREFER_REGISTER is set to 1 on RISC-V;
           no additional compiler flags are needed. Can be overridden to 0 safely.
    \note  When set to 1 the stk::Stack gains no \c tls member, reducing
           per-task RAM by one \c Word. On targets with many tasks this saving can
           be significant.
    \note  Default: 0 (disabled). Leave at 0 if \c -ffixed-r9 cannot be applied
           to all task translation units (e.g. pre-built third-party libraries);
           the memory-based fallback incurs only one extra load/store per TLS access.
    \warning Cortex-M only: \c -ffixed-r9 must be applied to \e every translation
           unit in the binary that contains task code, including inlined functions
           and any library code those tasks call. A single TU compiled without
           the flag is sufficient to cause intermittent, hard-to-reproduce TLS
           corruption.
    \warning STK_TLS_PREFER_REGISTER=0 is not supported in non-Privileged context.
    \see   STK_TLS, stk::hw::GetTlsPtr, stk::hw::SetTlsPtr
*/
#ifndef STK_TLS_PREFER_REGISTER
    #ifdef _STK_ARCH_RISC_V
        #define STK_TLS_PREFER_REGISTER (1)
    #else
        #define STK_TLS_PREFER_REGISTER (0)
    #endif
#endif

/*! \def   STK_MPU
    \brief If 1, enable MPU support.
*/
#ifndef STK_MPU
    #define STK_MPU (0)
#endif

/*! \def   STK_MPU_STACK_GUARD
    \brief If 1, reprogram one MPU region to the active task's own stack on every switch.
*/
#ifndef STK_MPU_STACK_GUARD
    #define STK_MPU_STACK_GUARD (0)
#endif

/*! \def   STK_MPU_TASK_REGIONS
    \brief Number of hardware MPU region slots reserved per task (\c stk::TaskMpu::NUM_REGIONS).
    \details Slot 0 is always the automatic per-task stack guard; the remaining slots
           (\c STK_MPU_TASK_REGIONS - 1) are available to the application via
           \c ITask::GetMpuRegions(). Only consumed when \c STK_MPU_STACK_GUARD is enabled.
    \note  Supported values: 2 or 4. A smaller value frees up MPU regions for
           static/global use (see \c STK_CORTEX_M_MPU_TASK_REGION_IDX) and shrinks the
           per-task \c TaskMpu footprint and context-switch burst-copy, at the cost of
           fewer application-defined per-task regions.
    \note  Default: 2.
    \see   STK_MPU_STACK_GUARD, stk::TaskMpu, ITask::GetMpuRegions
*/
#ifndef STK_MPU_TASK_REGIONS
    #define STK_MPU_TASK_REGIONS (2)
#endif

#if STK_MPU_STACK_GUARD && (STK_MPU_TASK_REGIONS != 2) && (STK_MPU_TASK_REGIONS != 4)
    #error "STK_MPU_TASK_REGIONS must be defined as 2 or 4"
#endif

/*! \def   STK_STACK_NEEDS_TASK_ID
    \brief When defined as 1, the Stack descriptor (stk::Stack) carries a \c tid field
           used by the SEGGER SystemView trace back-end to identify tasks during context switches.
    \note  Set unconditionally (no \c #ifndef guard) whenever STK_SEGGER_SYSVIEW is enabled.
           Any prior manual definition will be silently overwritten. Do not define this macro
           manually; enable STK_SEGGER_SYSVIEW instead.
    \see   STK_SEGGER_SYSVIEW, stk::Stack
*/
#if STK_SEGGER_SYSVIEW || STK_MPU_STACK_GUARD
	#ifndef STK_STACK_NEEDS_TASK_ID
		#define STK_STACK_NEEDS_TASK_ID (1)
	#endif
#endif

/*! \def   STK_SYNC_DEBUG_NAMES
    \brief Enable debug names for synchronization primitives (mutexes, events, etc.) for debugging and tracing purposes.
    \note  When set to 1, synchronization objects gain a string name field (see ITraceable::SetTraceName)
           that is visible in SEGGER SystemView and other trace tools.
    \note  Automatically set to 1 when STK_SEGGER_SYSVIEW is enabled. Can be manually defined in stk_config.h to override the default.
    \note  Default: 0 (disabled) unless STK_SEGGER_SYSVIEW is active.
*/
#if !defined(STK_SYNC_DEBUG_NAMES) && STK_SEGGER_SYSVIEW
    #define STK_SYNC_DEBUG_NAMES (1)
#elif !defined(STK_SYNC_DEBUG_NAMES)
    #define STK_SYNC_DEBUG_NAMES (0)
#endif

/*! \def   STK_VIRT_DTOR
    \brief Makes destructors virtual and compliant to strict rules if STK_STRICT_COMPLIANCY=0.
*/
#if !STK_STRICT_COMPLIANCY
    #define STK_VIRT_DTOR
#else
    #define STK_VIRT_DTOR virtual
#endif

/*! \def   __stk_forceinline
    \brief Forces compiler to always inline the decorated function, regardless of optimisation level.
    \note  Used on latency-critical paths (ISR handlers, scheduler hot paths) where a function call
           overhead would be unacceptable or would unpredictably affect real-time timing.
    \note  On compilers not listed below the attribute expands to nothing (inlining becomes a hint only).
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_forceinline __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    #define __stk_forceinline __forceinline
#else
    #define __stk_forceinline inline
#endif

/*! \def       __stk_aligned
    \brief     Specifies minimum alignment in bytes for the decorated variable or struct member (data instance prefix).
    \param[in] x: Required alignment in bytes. Must be a power of two.
    \note      On compilers not listed below the attribute expands to nothing and alignment is not enforced.
               Verify alignment-sensitive data on new toolchains.
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_aligned(x) __attribute__((aligned(x)))
#else
    #define __stk_aligned(x)
#endif

/*! \def   __stk_weak
    \brief Marks a function or variable as weak, allowing it to be overridden by the user.
    \note  On compilers not listed below the attribute expands to nothing.
           If not supported, multiple definition errors may occur during linking.
*/
#if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__) || defined(__CC_ARM) || defined(__ARMCC_VERSION)
    #define __stk_weak __attribute__((weak))
#else
    #define __stk_weak
#endif

/*! \def   __stk_attr_naked
    \brief Suppresses compiler-generated function prologue and epilogue (function prefix).
    \note  The decorated function must consist of inline assembly only. C statements that rely
           on a stack frame (local variables, non-trivial expressions, function calls) produce
           undefined behaviour. Used for context-switch stubs and ISR entry points where the
           register save/restore sequence must be precisely hand-written.
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_attr_naked __attribute__((naked))
#else
    #define __stk_attr_naked
#endif

/*! \def   __stk_attr_noreturn
    \brief Declares that function never returns to its caller (function prefix).
    \note  Enables compiler to omit return-path code and dead-store warnings after the call site.
    \warning Applying this attribute to a function that does return produces undefined behaviour.
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_attr_noreturn __attribute__((__noreturn__))
#else
    #define __stk_attr_noreturn
#endif

/*! \def   __stk_attr_unused
    \brief Suppresses compiler warnings about an unused type, variable, or function (declaration prefix).
    \note  Does not prevent the linker from discarding the symbol. Use __stk_attr_used when the symbol
           must be retained regardless of whether it appears to be referenced.
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_attr_unused __attribute__((unused))
#else
    #define __stk_attr_unused
#endif

/*! \def   __stk_attr_used
    \brief Marks a symbol as used, preventing the linker from discarding it even if no references are visible (declaration prefix).
    \note  Commonly applied to ISR vector table entries, trap stubs, and objects placed in special
           linker sections that are referenced only from assembly or linker scripts.
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_attr_used __attribute__((used))
#else
    #define __stk_attr_used
#endif

/*! \def   __stk_attr_noinline
    \brief Prevents compiler from inlining the decorated function (function prefix).
    \note  Used where inlining would obscure stack-depth analysis, produce unpredictable code size,
           or make profiling and tracing results misleading.
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_attr_noinline __attribute__((noinline))
#else
    #define __stk_attr_noinline
#endif

/*! \def   __stk_attr_deprecated
    \brief Marks a function, class, variable, or typedef as deprecated (declaration prefix).
    \note  The compiler will emit a warning at every call site or use of the decorated symbol,
           prompting callers to migrate to the replacement API.
*/
#if defined(__GNUC__) || defined(__ICCARM__)
    #define __stk_attr_deprecated __attribute__((deprecated))
#elif defined(_MSC_VER)
    #define __stk_attr_deprecated __declspec(deprecated)
#else
    #define __stk_attr_deprecated
#endif

/*! \brief Emits a full (sequentially-consistent) memory barrier (in-code statement).
    \note  Prevents both the compiler and the CPU from reordering memory accesses across this point.
           Used to enforce visibility ordering between cores or between a task and an ISR without
           entering a critical section.
*/
#if defined(__GNUC__) || defined(__clang__)
    static __stk_forceinline void __stk_full_memfence() { __sync_synchronize(); }
#elif defined(__ICCARM__)
    static __stk_forceinline void __stk_full_memfence() { __DMB(); }
#elif defined(_MSC_VER)
    static __stk_forceinline void __stk_full_memfence() { __stk_dmb(); }
#else
    #error "__stk_full_memfence() is not implemented for this compiler. Add a definition to stk_defs.h."
#endif

/*! \brief Compiler-only barrier: prevents instruction reordering by the compiler,
           emits no hardware instruction and costs zero cycles.
    \note  Use to pin a memory access to its intended position in the instruction
           stream without any bus or cache side-effects.
    \note  On MSVC _ReadWriteBarrier() is the equivalent: a compiler fence with
           no hardware instruction emitted, deprecated in favour of volatile but
           still the correct lightweight barrier for this purpose.
*/
#if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__)
    static __stk_forceinline void __stk_compiler_barrier() { __asm volatile("" ::: "memory"); }
#elif defined(_MSC_VER)
    static __stk_forceinline void __stk_compiler_barrier() { _ReadWriteBarrier(); }
#else
    #error "__stk_compiler_barrier() is not implemented for this compiler. Add a definition to stk_defs.h."
#endif

/*! \brief Emits a CPU pipeline-relaxation hint for use inside hot busy-wait (spin) loops (in-code statement).
    \note  Reduces power consumption and memory-bus contention while spinning by signalling to the CPU
           that the current thread is in a spin-wait. Platform-specific expansions:
           - x86/x64 (GCC/Clang/MSVC): the \c PAUSE instruction via \c __builtin_ia32_pause() or \c _mm_pause().
           - RISC-V with Zihintpause (GCC/Clang): the \c PAUSE hint via \c __builtin_riscv_pause().
           - RISC-V without Zihintpause (GCC/Clang): falls back to \c __stk_full_memfence().
           - ARM Cortex-M (GCC/Clang/IAR): the \c YIELD instruction via inline asm; valid Thumb on all M0-M33 variants.
           - ARM Cortex-M (MSVC): the \c YIELD instruction via \c __yield().
           - Other/unknown targets: falls back to \c __stk_full_memfence().
    \note  Can be redefined externally (e.g. in test harnesses) to intercept control inside kernel
           waiting loops without modifying kernel source.
*/
#ifndef __stk_relax_cpu
#if defined(__GNUC__) || defined(__clang__)
    #if defined(__i386__) || defined(__x86_64__)
    static __stk_forceinline void __stk_relax_cpu() { __builtin_ia32_pause(); }
    #elif defined(__riscv)
        #ifdef __riscv_zihintpause
            static __stk_forceinline void __stk_relax_cpu() { __builtin_riscv_pause(); }
        #else
            static __stk_forceinline void __stk_relax_cpu() { __asm volatile("nop" ::: "memory"); }
        #endif
    #elif defined(__ARM_ARCH) || defined(_STK_ARCH_ARM_CORTEX_M)
            static __stk_forceinline void __stk_relax_cpu() { __asm volatile("yield" ::: "memory"); }
    #else
            static __stk_forceinline void __stk_relax_cpu() { __asm volatile("" ::: "memory"); }
    #endif
#elif defined(__ICCARM__)
    static __stk_forceinline void __stk_relax_cpu() { __asm volatile("YIELD"); }
#elif defined(_MSC_VER)
    #include <intrin.h>
    #if defined(_M_IX86) || defined(_M_X64)
        static __stk_forceinline void __stk_relax_cpu() { _mm_pause(); }
    #elif defined(_M_ARM) || defined(_M_ARM64)
        static __stk_forceinline void __stk_relax_cpu() { __yield(); }
    #else
        static __stk_forceinline void __stk_relax_cpu() { __stk_full_memfence(); }
    #endif
#else
    #error "__stk_relax_cpu() is not implemented for this compiler. Add a definition to stk_defs.h."
#endif
#endif

/*! \def   __stk_debug_break
    \brief Triggers a hardware breakpoint, halting execution in an attached debugger (in-code statement).
    \note  Behaviour by build and architecture:
           - Release build (neither \c DEBUG nor \c _DEBUG defined): always expands to nothing,
             regardless of architecture.
           - Debug build with a recognised architecture (\c _STK_ARCH_ARM_CORTEX_M,
             \c _STK_ARCH_RISC_V, or \c _STK_ARCH_X86_WIN32): emits the appropriate
             breakpoint instruction (\c bkpt, \c ebreak, or \c __debugbreak / \c int $3).
           - Debug build with no recognised architecture: the macro is left \b undefined.
             Any usage site will produce a compiler error. Add a definition for your
             architecture in stk_defs.h or define it to nothing in stk_config.h to suppress.
    \note  Used in assertion handlers and fault paths to halt the system at the exact failure point
           rather than continuing into undefined state.
*/
#if defined(DEBUG) || defined(_DEBUG)
    #if defined(_STK_ARCH_ARM_CORTEX_M)
        static __stk_forceinline void __stk_debug_break() { __asm volatile("bkpt 0"); }
    #elif defined(_STK_ARCH_RISC_V)
        static __stk_forceinline void __stk_debug_break() { __asm volatile("ebreak"); }
    #elif defined(_STK_ARCH_X86_WIN32)
        #ifdef _MSC_VER
            static __stk_forceinline void __stk_debug_break() { __debugbreak(); }
        #else
            static __stk_forceinline void __stk_debug_break() { __asm volatile("int $3"); }
        #endif
    #endif
#else
    static __stk_forceinline void __stk_debug_break() {}
#endif

/*! \def   __stk_constexpr_cpp17
    \brief constexpr definition for C++17 and above.
    \note  Can be used as 'if __stk_constexpr_cpp17 (x) {}' with C++11 without a warning.
*/
#if (__cplusplus >= 201703L) || (defined(_MSVC_LANG) && (_MSVC_LANG >= 201703L))
    #define __stk_constexpr_cpp17 constexpr
#else
    #define __stk_constexpr_cpp17
#endif

/*! \def   STK_ASSERT
    \brief Runtime assertion. Halts execution if the expression \a e evaluates to false.
    \note  Behaviour depends on build configuration:
           - If \c _STK_ASSERT_REDIRECT is defined: always redirects to the custom handler
             \c STK_ASSERT_HANDLER (regardless of debug/release build type). Signature:
             \code void STK_ASSERT_HANDLER(const char *expr, const char *file, int32_t line); \endcode
             Use this on embedded targets where the standard assert handler is unavailable,
             or where a custom fault logger or LED indicator is preferred.
           - Else if \c DEBUG or \c _DEBUG is defined (debug build): maps to the standard
             \c assert() macro from \c <assert.h>, which halts execution on failure.
           - Else (release build): expands to nothing. The expression \a e is completely
             elided, including any side effects. Do not rely on side effects inside STK_ASSERT.
    \warning In release builds without \c _STK_ASSERT_REDIRECT, all assertions are silently
             removed. Safety-critical applications (ISO 26262, IEC 61508) should define
             \c _STK_ASSERT_REDIRECT to retain fault detection in all build configurations.
*/
#ifdef _STK_ASSERT_REDIRECT
    extern void STK_ASSERT_HANDLER(const char *, const char *, int32_t);
    #define STK_ASSERT(e) ((e) ? (void)0 : STK_ASSERT_HANDLER(#e, __FILE__, __LINE__))
#else
    #if defined(DEBUG) || defined(_DEBUG)
        #include <cassert>
        #define STK_ASSERT(e) assert(e)
    #else
        #define STK_ASSERT(e)
    #endif
#endif

/*! \def   STK_STATIC_ASSERT_DESC_N
    \brief Compile-time assertion with a user-defined name suffix and a custom error description.
    \note  \a NAME is appended to the internal symbol name, allowing multiple assertions in the
           same scope without symbol-name collisions.
    \note  \a DESC is a string literal shown in the compiler diagnostic — use it to provide a
           human-readable explanation of the constraint. Prefer this over STK_STATIC_ASSERT_N
           when the expression alone would not be self-explanatory in a compiler error.
*/
#define STK_STATIC_ASSERT_DESC_N(NAME, X, DESC) static_assert((X), DESC)

/*! \def   STK_STATIC_ASSERT_DESC
    \brief Compile-time assertion with a custom error description. Produces a compilation error if \a X is false.
    \note  Uses a fixed internal symbol name. If more than one STK_STATIC_ASSERT_DESC appears in
           the same scope, use STK_STATIC_ASSERT_DESC_N to provide distinct name suffixes and
           avoid duplicate symbol errors.
*/
#define STK_STATIC_ASSERT_DESC(X, DESC) STK_STATIC_ASSERT_DESC_N(_, X, DESC)

/*! \def   STK_STATIC_ASSERT_N
    \brief Compile-time assertion with a user-defined name suffix.
    \note  \a NAME is appended to the internal symbol name, allowing multiple assertions in the
           same scope without symbol-name collisions.
    \note  The compiler diagnostic message is the stringified form of expression \a X.
           Use STK_STATIC_ASSERT_DESC_N instead when you need a more descriptive error message.
*/
#define STK_STATIC_ASSERT_N(NAME, X) STK_STATIC_ASSERT_DESC_N(N, (X), #X)

/*! \def   STK_STATIC_ASSERT
    \brief Compile-time assertion. Produces a compilation error if \a X is false.
    \note  Uses a fixed internal symbol name. If more than one STK_STATIC_ASSERT appears in
           the same scope, use STK_STATIC_ASSERT_N to provide distinct name suffixes and
           avoid duplicate symbol errors.
*/
#define STK_STATIC_ASSERT(X) STK_STATIC_ASSERT_DESC_N(_, (X), #X)

/*! \def   STK_STACK_MEMORY_FILLER
    \brief Sentinel value written to the entire stack region at initialization (stack watermark pattern).
    \note  Used to detect stack overflow and to measure peak stack usage at run-time: any stack word
           that still contains this value was never written by the task.
    \note  Defaults to \c 0xDEADBEEF on 32-bit targets and \c 0xDEADBEEFDEADBEEF on 64-bit targets.
           Can be overridden by defining STK_STACK_MEMORY_FILLER before including this header or in stk_config.h.
*/
#ifndef STK_STACK_MEMORY_FILLER
    #define STK_STACK_MEMORY_FILLER (static_cast<stk::Word>((sizeof(stk::Word) <= 4U) ? 0xDEADBEEFU : 0xDEADBEEFDEADBEEFULL))
#endif

/*! \def   STK_STACK_MEMORY_ALIGN
    \brief Stack memory alignment.
*/
#ifndef STK_STACK_MEMORY_ALIGN
    #if defined(__riscv)
        #define STK_STACK_MEMORY_ALIGN (16U)
    #elif defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
        #define STK_STACK_MEMORY_ALIGN (8U)
    #else // ARM, others
        #define STK_STACK_MEMORY_ALIGN (4U)
    #endif
#endif

/*! \def   STK_CS_NESTINGS_MAX
    \brief Maximum allowable recursion depth for critical section entry (default: 16).
    \note  Establishes a hard deterministic bound for nested calls to Context::EnterCriticalSection()
           and Context::UnprivEnterCriticalSection(). This limit is mandatory for safety-critical
           certification (e.g., ISO 26262, IEC 61508) to prevent unbounded stack usage
           and identify logic deadlocks.
    \note  Exceeding this value at runtime shall trigger a Kernel Panic with the code
           KERNEL_PANIC_CS_NESTING_OVERFLOW to transition the system into a Safe State.
           Can be overridden in stk_config.h based on Worst-Case Stack Usage (WCSU) analysis.
*/
#ifndef STK_CS_NESTINGS_MAX
    #define STK_CS_NESTINGS_MAX (16U)
#endif

/*! \def   STK_ARCH_CPU_COUNT
    \brief Number of physical CPU cores available to the scheduler (default: 1).
    \note  Controls the number of per-CPU kernel service instances and per-CPU data structures
           allocated by the kernel. Set to the actual core count for SMP (symmetric multi-processing)
           targets. Can be defined in the architecture header or stk_config.h.
*/
#ifndef STK_ARCH_CPU_COUNT
    #define STK_ARCH_CPU_COUNT (1U)
#endif

/*! \def   STK_STACK_SIZE_MIN
    \brief Minimum stack size in elements of \c Word, shared by all stack allocation lower-bound checks.
    \see   TrapStackMemory
    \note  This is the smallest stack that can correctly save and restore all CPU registers during
           a context switch or service trap. The required size depends on the number of registers
           the architecture mandates saving and specific platform alignment requirements.
    \note  Default values by architecture:
           - ARM Cortex-M and RISC-V RV32E (reduced 16-register file): 32 elements.
           - Standard RISC-V (RV32I / RV64I) without FPU: 256 elements
             (larger register file; smaller sizes may cause memory corruption on RP2350).
           - Standard RISC-V with FPU: 512 elements + (__riscv_flen * 2)
             (dedicated FP registers significantly expand the required frame size).
    \note  Can be overridden to any larger value in stk_config.h if your application's
           interrupt nesting or stack frame size requires it.
*/
#ifndef STK_STACK_SIZE_MIN
    #ifdef __riscv
        #if defined(__riscv_32e) && (__riscv_32e == 1)
            // RISC-V RV32E (Embedded): Small 16-register file.
            #if !defined(__riscv_flen) || (__riscv_flen == 0)
                #define STK_STACK_SIZE_MIN (32U)
            #else
                // FPU present: Requires additional space for 32 FP registers.
                #define STK_STACK_SIZE_MIN (32U + (__riscv_flen * 2))
            #endif
        #else
            // Standard RISC-V (RV32I/RV64I): Large 32-register file.
            // Higher minimum to prevent memory corruption on platforms like RP2350.
            #if !defined(__riscv_flen) || (__riscv_flen == 0)
                #define STK_STACK_SIZE_MIN (256U)
            #else
                // Standard RISC-V with FPU: Maximum frame allocation.
                #define STK_STACK_SIZE_MIN (512U + (__riscv_flen * 2))
            #endif
        #endif
    #else
        // ARM Cortex-M and other architectures
        #define STK_STACK_SIZE_MIN (32U)
    #endif
#endif

/*! \def   STK_SLEEP_TRAP_STACK_SIZE
    \brief Stack size for the sleep trap in elements of \c Word (default: STK_STACK_SIZE_MIN).
    \see   Kernel::SleepTrapStackMemory
    \note  If IEventOverrider::OnSleep() is overridden with a non-trivial implementation (e.g.
           calling platform-specific low-power APIs that use the stack), increase this value
           to accommodate the additional stack frame depth required by that implementation.
           Can be defined in stk_config.h.
*/
#ifndef STK_SLEEP_TRAP_STACK_SIZE
    #define STK_SLEEP_TRAP_STACK_SIZE (STK_STACK_SIZE_MIN)
#endif

/*! \struct STK_ALLOCATE_COUNT
    \brief  Selects a static array element count at compile time based on a mode flag.
    \note   On GCC/Clang: evaluates to ONTRUE if (MODE & FLAG) is non-zero, otherwise ONFALSE.
            On MSVC/IAR: always evaluates to the maximum of ONTRUE and ONFALSE because these
            compilers do not support zero-sized arrays.

    \tparam MODE    Bitmask of active kernel modes (e.g., EKernelMode flags).
    \tparam FLAG    The specific mode bit to test.
    \tparam ONTRUE  Array count to use when FLAG is active.
    \tparam ONFALSE Array count to use when FLAG is inactive (may be 0 on GCC/Clang).
*/
template <size_t MODE, size_t FLAG, size_t ONTRUE, size_t ONFALSE>
struct STK_ALLOCATE_COUNT
{
#if defined(_MSC_VER) || defined(__ICCARM__)
    // MSVC and IAR builds may over-allocate when the flag is not set to avoid compile errors.
    static constexpr size_t Value = ((ONTRUE > ONFALSE) ? ONTRUE : ONFALSE);
#else
    // GCC and Clang support zero-sized array extensions natively.
    static constexpr size_t Value = (((MODE & FLAG) != 0U) ? ONTRUE : ONFALSE);
#endif
};

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
    #define STK_ENDIAN_IDX_HI (0U) // big-endian: high word at index 0
    #define STK_ENDIAN_IDX_LO (1U) // big-endian: low word at index 1
#else
    #define STK_ENDIAN_IDX_HI (1U) // little-endian (default): high word at index 1
    #define STK_ENDIAN_IDX_LO (0U) // little-endian (default): low word at index 0
#endif

/*! \def       STK_NONCOPYABLE_CLASS
    \brief     Disables copy construction and assignment for a class.
    \details   This macro declares a private copy constructor and copy assignment
               operator to prevent the compiler from generating default ones. It
               ensures that instances of a class cannot be duplicated.
    \param[in] TYPE: The name of the class to be made non-copyable.
    \warning   This macro must be placed within the \c private or \c protected
               section of the class declaration to be effective.
    \note      In C++11 and later, it is generally preferred to use \c = delete, however,
               this macro provides compatibility for legacy environments.
*/
#define STK_NONCOPYABLE_CLASS(TYPE)\
    TYPE(const TYPE &) = delete;\
    TYPE &operator=(const TYPE &) = delete;

/*! \def       STK_UNUSED
    \brief     Explicitly marks a variable as unused to suppress compiler warnings.
*/
#define STK_UNUSED(X) static_cast<void>((X))

/*! \def       STK_LIKELY
    \brief     Provides a compiler hint that the given expression is highly likely to evaluate to true.
    \param     x The expression to evaluate.
*/
/*! \def       STK_UNLIKELY
    \brief     Provides a compiler hint that the given expression is highly unlikely to evaluate to true
               (typically used for error handling).
    \param     x The expression to evaluate.
*/
#if __cplusplus >= 202002L
    #define STK_LIKELY(x)   ([]() { if constexpr (!!(x)) [[likely]] return true; else return false; }())
    #define STK_UNLIKELY(x) ([]() { if constexpr (!!(x)) return true; else [[unlikely]] return false; }())
#elif defined(__GNUC__) || defined(__clang__)
    #define STK_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define STK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define STK_LIKELY(x)   (x)
    #define STK_UNLIKELY(x) (x)
#endif

/*! \def       STK_STATIC_ARRAY_SIZE
    \brief     Get size of the static array.
    \param[in] ARRAY: Pointer to the array.
    \warning   Array must be static.
*/
#define STK_STATIC_ARRAY_SIZE(ARRAY) static_cast<size_t>(sizeof(ARRAY) / sizeof(ARRAY[0]))

/*! \namespace stk
    \brief     Namespace of STK package.
 */
namespace stk {

/*! \brief Compile-time minimum of two values.
    \note  Arguments are evaluated exactly once, safe for any expression type.
*/
template <typename T>
static constexpr T Min(T a, T b) noexcept { return ((a < b) ? a : b); }

/*! \brief Compile-time maximum of two values.
    \note  Arguments are evaluated exactly once, safe for any expression type.
*/
template <typename T>
static constexpr T Max(T a, T b) noexcept { return ((a > b) ? a : b); }

/*! \brief Aligns a value towards the next larger size multiple.
    \note  Arguments are evaluated exactly once, safe for any expression type.
*/
template <typename T>
static constexpr T Align(T v, T align) noexcept
{
    return ((v + align - static_cast<T>(1U)) / align) * align;
}

/*! \brief Fast compile-time alignment for power-of-two boundaries.
    \note  Arguments are evaluated exactly once. The 'align' parameter MUST be a power of two.
*/
template <typename T>
static constexpr T AlignPow2(T v, T align) noexcept
{
    return (v + align - static_cast<T>(1U)) & ~(align - static_cast<T>(1U));
}

/*! \brief     Count leading zeros.
    \param[in] value: Value must be non 0.
    \warning   Undefined behavior if temp_val is 0, but guaranteed safe by upstream checks.
*/
static __stk_forceinline uint32_t CountLeadingZeros(const uint32_t value) noexcept
{
    uint32_t ret_val;
    uint32_t temp_val = value;

#if defined(__GNUC__) || defined(__clang__)
    ret_val = static_cast<uint32_t>(__builtin_clz(temp_val));
#elif defined(__ICCARM__)
    ret_val = static_cast<uint32_t>(__CLZ(temp_val));
#else
    uint32_t count = 0U;

    // note: binary search requires temp_val > 0 to resolve to a max of 31 leading zeros safely
    if (temp_val <= 0x0000FFFFU) { count += 16U; temp_val = temp_val << 16U; } else { }
    if (temp_val <= 0x00FFFFFFU) { count += 8U;  temp_val = temp_val << 8U;  } else { }
    if (temp_val <= 0x0FFFFFFFU) { count += 4U;  temp_val = temp_val << 4U;  } else { }
    if (temp_val <= 0x3FFFFFFFU) { count += 2U;  temp_val = temp_val << 2U;  } else { }
    if (temp_val <= 0x7FFFFFFFU) { count += 1U; } else { }

    ret_val = count;
#endif

    return ret_val;
}

/*! \namespace stk::util
    \brief     Internal utility namespace containing data structure helpers (linked lists, etc.)
               used by the kernel implementation. Not part of the public user API.
 */
namespace util {}

} // namespace stk

/*! \brief     A wrapper for a built-in memcpy, redefine to your own if required.
    \note      Can be overridden by defining _STK_CUSTOM_MEMCPY in system configuration.
*/
#ifndef _STK_CUSTOM_MEMCPY
static void STK_MEMCPY(void *const dest, const void *const src, const size_t size);
#endif

/*! \brief     A wrapper for a built-in memset, redefine to your own if required.
    \note      Can be overridden by defining _STK_CUSTOM_MEMSET in system configuration.
*/
#ifndef _STK_CUSTOM_MEMSET
static void STK_MEMSET(void *const dest, const uint8_t value, const size_t size);
#endif

#endif /* STK_DEFS_H_ */
