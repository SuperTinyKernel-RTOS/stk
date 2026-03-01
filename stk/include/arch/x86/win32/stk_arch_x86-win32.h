/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_X86_WIN32_H_
#define STK_ARCH_X86_WIN32_H_

#include "stk_common.h"

namespace stk {

/*! \class PlatformX86Win32
    \brief Concrete implementation of IPlatform driver for the x86 Win32 platform.
    \note  Implemented for simulation purpose on Windows platform.
*/
class PlatformX86Win32 : public IPlatform
{
public:
    void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap);
    void Start();
    void Stop();
    bool InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task);
    int32_t GetTickResolution() const;
    void SwitchToNext();
    void Sleep(Timeout ticks);
    IWaitObject *Wait(ISyncObject *sync_obj, IMutex *mutex, Timeout timeout);
    void ProcessTick();
    void ProcessHardFault();
    void SetEventOverrider(IEventOverrider *overrider);
    size_t GetCallerSP() const;
    TId GetTid() const;
};

/*! \typedef PlatformDefault
    \brief   Default platform implementation.
*/
typedef PlatformX86Win32 PlatformDefault;

} // namespace stk

/*! \def   __stk_dmb
    \brief Data memory barrier.
*/
#if defined(_MSC_VER)
    #include <intrin.h>
    #define __stk_dmb() MemoryBarrier()
#elif defined(__GNUC__)
    #define __stk_dmb() __asm volatile("mfence" ::: "memory")
#endif

#endif /* STK_ARCH_X86_WIN32_H_ */
