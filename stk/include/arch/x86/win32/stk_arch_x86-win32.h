/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_X86_WIN32_H_
#define STK_ARCH_X86_WIN32_H_

#include "stk_common.h"

/*! \file  stk_arch_x86-win32.h
    \brief Platform port for Windows Win32 (STK emulator).
*/

namespace stk {

/*! \class PlatformX86Win32
    \brief Concrete implementation of IPlatform driver for the x86 Win32 platform.
    \note  Implemented for simulation purpose on Windows platform.
*/
class PlatformX86Win32 : public IPlatform
{
public:
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~PlatformX86Win32()
    {}

    void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap) override;
    void Start() override;
    void Stop() override;
    void InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) override;
    uint32_t GetTickResolution() const override;
    Cycles GetSysTimerCount() const override;
    uint32_t GetSysTimerFrequency() const override;
    void SwitchToNext() override;
    void ForceContextSwitch(TId id) override;
    void Sleep(Timeout ticks) override;
    bool SleepUntil(Ticks timestamp) override;
    EWaitResult Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout) override;
    void ProcessTick() override;
    void ProcessHardFault() override;
    void SetEventOverrider(IEventOverrider *overrider, bool non_secure) override;
    Word GetCallerSP() const override;
    TId GetTid() const override;
    Timeout Suspend() override;
    void Resume(Timeout elapsed_ticks) override;
    void SetCpuFrequency(uint8_t core_id, uint32_t frequency) override;
};

/*! \typedef PlatformDefault
    \brief   Default platform implementation.
*/
typedef PlatformX86Win32 PlatformDefault;

} // namespace stk

/*! \brief Data memory barrier.
*/
#if defined(_MSC_VER)
    #include <intrin.h>
    #if defined(_M_IX86) || defined(_M_X64)
        // x86/x64: full hardware serializing fence
        static __stk_forceinline void __stk_dmb() { _mm_mfence(); }
    #elif defined(_M_ARM) || defined(_M_ARM64)
        // ARM/ARM64: Data Memory Barrier (Inner Shareable)
        static __stk_forceinline void __stk_dmb() { __dmb(_ARM_BARRIER_ISH); }
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    static __stk_forceinline void __stk_dmb() { __sync_synchronize(); }
#else
    #error "__stk_dmb() is not implemented for this compiler."
#endif

#endif /* STK_ARCH_X86_WIN32_H_ */
