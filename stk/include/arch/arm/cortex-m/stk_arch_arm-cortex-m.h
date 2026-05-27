/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_ARM_CORTEX_M_H_
#define STK_ARCH_ARM_CORTEX_M_H_

#include "stk_common.h"

/*! \file  stk_arch_arm-cortex-m.h
    \brief Platform port for ARM Cortex-M.
*/

namespace stk {

/*! \class PlatformArmCortexM
    \brief Concrete implementation of IPlatform driver for the Arm Cortex-M0, M3, M4, M7 processors.
*/
class PlatformArmCortexM final : public IPlatform
{
public:
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~PlatformArmCortexM() = default;

    void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap) override;
    void Start() override;
    void Stop() override;
    bool InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) override;
    uint32_t GetTickResolution() const override;
    Cycles GetSysTimerCount() const override;
    uint32_t GetSysTimerFrequency() const override;
    void SwitchToNext() override;
    void Sleep(Timeout ticks) override;
    bool SleepUntil(Ticks timestamp) override;
    IWaitObject *Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout) override;
    void ProcessTick() override;
    void ProcessHardFault() override;
    void SetEventOverrider(IEventOverrider *overrider) override;
    Word GetCallerSP() const override;
    TId GetTid() const override;
    Timeout Suspend() override;
    void Resume(Timeout elapsed_ticks) override;
};

/*! \typedef PlatformDefault
    \brief   Default platform implementation.
*/
typedef PlatformArmCortexM PlatformDefault;

// Inline TLS via r9. Active when both STK_TLS and STK_TLS_PREFER_REGISTER are
// enabled. Requires -ffixed-r9 on all translation units containing task code
// (see GetTls/SetTls warnings below). If -ffixed-r9 is unavailable or
// undesirable, leave STK_TLS_PREFER_REGISTER disabled; the kernel will fall back
// to a memory-based TLS slot with a small additional load/store per access.
#if STK_TLS && STK_TLS_PREFER_REGISTER

/*! \brief   Get thread-local storage (TLS).
    \return  TLS value.
    \note    Uses r9 as the TLS base pointer per the ARM EABI "platform register"
             convention (AAPCS 5.2.2).  The value is saved and restored by PendSV
             on every context switch, so each task sees its own TLS pointer on entry.
    \warning **Requires \c -ffixed-r9 for every translation unit that contains task
             code** - including any library code a task calls that touches r9.
             Without it, the compiler treats r9 as an ordinary callee-saved register:
             it may spill r9 to the stack in a function prologue, overwrite it with an
             intermediate value (e.g. during 64-bit arithmetic), and restore it in the
             epilogue.
    \see     SetTls, stk::hw::GetTlsPtr, stk::hw::SetTlsPtr
*/
__stk_forceinline Word GetTls()
{
    Word tp;
    __asm volatile("MOV %0, r9" : "=r"(tp) : /* input: none */ : /* clobbers: none */);
    return tp;
}

/*! \brief     Set thread-local storage (TLS).
    \param[in] tp: TLS value to store in r9.
    \note      Uses the r9 register as the TLS base pointer, following the ARM EABI
               "platform register" convention (AAPCS §5.2.2).
    \warning   **Requires \c -ffixed-r9 for every translation unit that contains task
               code.** See \c GetTls() for the full explanation.
    \see       GetTls, stk::hw::GetTlsPtr, stk::hw::SetTlsPtr
*/
__stk_forceinline void SetTls(Word tp)
{
    __asm volatile("MOV r9, %0" : /* output: none */ : "r"(tp) : /* clobbers: none */);
}

// Notify stk_arch.h that we defined inline versions of GetTls/SetTls.
#define STK_INLINE_TLS 1

#endif // STK_TLS_PREFER_REGISTER

} // namespace stk

/*! \def   __stk_dmb
    \brief Hardware memory barrier: ensures visibility across cores and bus masters.
*/
#define __stk_dmb() __asm volatile("dmb sy" ::: "memory")

/*! \def   __stk_tz_nsc_entry
    \brief TrustZone: attribute for Non-Secure callable gateway functions.
    \note  Places the function in the .nsc_entry section mapped to the NSC region.
*/
#define __stk_tz_nsc_entry __attribute__((cmse_nonsecure_entry))

#endif /* STK_ARCH_ARM_CORTEX_M_H_ */
