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
class PlatformArmCortexM : public IPlatform
{
public:
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~PlatformArmCortexM()
    {}

    void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap);
    void Start();
    void Stop();
    bool InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task);
    uint32_t GetTickResolution() const;
    Cycles GetSysTimerCount() const;
    uint32_t GetSysTimerFrequency() const;
    void SwitchToNext();
    void Sleep(Timeout ticks);
    bool SleepUntil(Ticks timestamp);
    IWaitObject *Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout);
    void ProcessTick();
    void ProcessHardFault();
    void SetEventOverrider(IEventOverrider *overrider);
    Word GetCallerSP() const;
    TId GetTid() const;
    Timeout Suspend();
    void Resume(Timeout elapsed_ticks);
};

/*! \typedef PlatformDefault
    \brief   Default platform implementation.
*/
typedef PlatformArmCortexM PlatformDefault;

/*! \brief  Get thread-local storage (TLS).
    \return TLS value.
    \note   Using r9 register.
*/
__stk_forceinline Word GetTls()
{
    Word tp;
    __asm volatile("MOV %0, r9" : "=r"(tp) : /* input: none */ : /* clobbers: none */);
    return tp;
}

/*! \brief     Set thread-local storage (TLS).
    \param[in] tp: TLS value.
    \note      Using r9 register.
*/
__stk_forceinline void SetTls(Word tp)
{
    __asm volatile("MOV r9, %0" : /* output: none */ : "r"(tp) : /* clobbers: none */);
}

// Notify stk_arch.h that we defined inline versions of GetTls/SetTls.
#define _STK_INLINE_TLS_DEFINED 1

} // namespace stk

/*! \def   __stk_dmb
    \brief Hardware memory barrier: ensures visibility across cores and bus masters.
*/
#define __stk_dmb() __asm volatile("dmb sy" ::: "memory")

#endif /* STK_ARCH_ARM_CORTEX_M_H_ */
