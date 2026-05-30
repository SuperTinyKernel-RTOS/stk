/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_RISC_V_H_
#define STK_ARCH_RISC_V_H_

#include "stk_common.h"

/*! \file  stk_arch_risc-v.h
    \brief Platform port for RISC-V.
*/

namespace stk {

/*! \class PlatformRiscV
    \brief Concrete implementation of IPlatform driver for the Risc-V processors.
*/
class PlatformRiscV : public IPlatform
{
public:
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~PlatformRiscV()
    {}

    /*! \class IEventHandler
        \brief RISC-V specific event handler.
    */
    class ISpecificEventHandler
    {
    public:
        /*! \brief Called by ISR handler on IRQ_XXX (see encoding.h).
            \note if scheduler is not started and ecall is invoked then
        */
        virtual bool OnException(Word cause) = 0;
    };

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

    void SetSpecificEventHandler(ISpecificEventHandler *handler);
};

/*! \typedef PlatformDefault
    \brief   Default platform implementation.
*/
typedef PlatformRiscV PlatformDefault;

// Enforce inline TLS because RISC-V always provides x5/tp register for riscv-none-elf-gcc or similar.
#undef STK_TLS_PREFER_REGISTER
#define STK_TLS_PREFER_REGISTER 1

/*! \brief  Get thread-local storage (TLS).
    \return TLS value.
    \note   tp register is an alias for x4
*/
__stk_forceinline Word GetTls()
{
    Word tp;
    __asm volatile("mv %0, tp" : "=r"(tp) : /* input: none */ : /* clobbers: none */);
    return tp;
}

/*! \brief     Set thread-local storage (TLS).
    \param[in] tp: TLS value.
    \note      tp register is an alias for x4
*/
__stk_forceinline void SetTls(Word tp)
{
    __asm volatile("mv tp, %0" : /* output: none */ : "r"(tp) : /* clobbers: none */);
}

// Notify stk_arch.h that we defined inline versions of GetTls/SetTls.
#define STK_INLINE_TLS 1

} // namespace stk

/*! \def   __stk_dmb
    \brief Data memory barrier.
*/
static __stk_forceinline void __stk_dmb() { __asm volatile("fence rw, rw" ::: "memory"); }

/*! \def   STK_SUBMICORSECOND_PRECISION_TIMER
    \brief Enables sub-microsecond precision timer, see \a hw::HiResClock.
    \note  By default timer precision is 1 microsecond.
*/
#ifndef STK_SUBMICORSECOND_PRECISION_TIMER
    #define STK_SUBMICORSECOND_PRECISION_TIMER 0
#endif

/*! \def   STK_SYSTEM_CORE_CLOCK_VAR
    \brief Definition of the system core clock variable holding frequency of the CPU in Hz.
*/
#ifndef STK_SYSTEM_CORE_CLOCK_VAR
    #define STK_SYSTEM_CORE_CLOCK_VAR SystemCoreClock
#endif

/*! \def   STK_SYSTEM_CORE_CLOCK_FREQUENCY
    \brief System clock frequency in Hz. Default: 150 MHz.
*/
#ifndef STK_SYSTEM_CORE_CLOCK_FREQUENCY
    #define STK_SYSTEM_CORE_CLOCK_FREQUENCY 150000000U
#endif

/*! \var   SystemCoreClock
    \brief System clock frequency in Hz.
*/
extern "C" volatile uint32_t STK_SYSTEM_CORE_CLOCK_VAR;

#endif /* STK_ARCH_RISC_V_H_ */
