/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_ARCH_COMMON_H_
#define STK_ARCH_COMMON_H_

/*! \file  stk_arch_common.h
    \brief Contains common inventory for platform implementation.
*/

#include "stk_common.h"

namespace stk {

/*! \class PlatformContext
    \brief Base platform context for all platform implementations.
*/
class PlatformContext
{
public:
    explicit PlatformContext() : m_handler(nullptr), m_service(nullptr), m_stack_idle(nullptr),
        m_stack_active(nullptr), m_tick_resolution(0U)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~PlatformContext() = default;

    /*! \brief     Initialize context.
        \param[in] handler: Event handler.
        \param[in] exit_trap: Exit trap's stack.
        \param[in] resolution_us: Tick resolution in microseconds (for example 1000 equals to 1 millisecond resolution).
    */
    virtual void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap,
        uint32_t resolution_us)
    {
        m_handler         = handler;
        m_service         = service;
        m_stack_idle      = exit_trap;
        m_stack_active    = nullptr;
        m_tick_resolution = resolution_us;
    }

    /*! \brief     Initialize stack memory by filling it with STK_STACK_MEMORY_FILLER.
        \note      Returned pointer is for a stack growing from top to down.
        \param[in] memory: Stack memory to initialize.
        \return    Top of the stack memory (valid memory region is (stack_top - sizeof(Word))).
    */
    static inline Word InitStackMemory(IStackMemory *const memory)
    {
        STK_ASSERT(memory != nullptr);
              
        ArrayView<Word> stack(const_cast<Word *>(memory->GetStack()), memory->GetStackSize());
        STK_ASSERT(stack.GetSize() >= STACK_SIZE_MIN);
        
        // initialize stack memory to satisfy stack integrity check in Kernel::StateSwitch
        for (size_t i = 0U; i < stack.GetSize(); ++i)
        {
            stack[i] = STK_STACK_MEMORY_FILLER;
        }
        
        // get address of the last valid item and step forward by 1 Word size
        const Word stack_top = hw::PtrToWord(memory->GetStack()) + (stack.GetSize() * sizeof(Word));

        // expecting STK_STACK_MEMORY_ALIGN-byte aligned memory for a stack
        STK_ASSERT((stack_top & (STK_STACK_MEMORY_ALIGN - 1U)) == 0U);

        return stack_top;
    }

    IPlatform::IEventHandler *m_handler;         //!< kernel event handler
    IKernelService           *m_service;         //!< kernel service
    Stack                    *m_stack_idle;      //!< idle task stack
    Stack                    *m_stack_active;    //!< active task stack
    uint32_t                  m_tick_resolution; //!< system tick resolution (microseconds)

protected:
    STK_NONCOPYABLE_CLASS(PlatformContext);
};

/*! \def   STK_ARCH_GET_CPU_ID
    \brief Get CPU core id of the caller, e.g. if called while running on core 0 then returned value must be 0.
*/
#ifndef STK_ARCH_GET_CPU_ID
    #define STK_ARCH_GET_CPU_ID() (0)
#endif

/*! \def   GetContext
    \brief Get platform's context.
*/
#ifndef _STK_UNDER_TEST
    #define GetContext() s_StkPlatformContext[STK_ARCH_GET_CPU_ID()]
#endif

/*! \brief     Convert time (microseconds) to core clock cycles.
    \param[in] clock_freq: Clock frequency.
    \param[in] time_us: Time (microseconds).
    \return    Clock cycles.
*/
static __stk_forceinline Cycles ConvertTimeUsToClockCycles(uint32_t clock_freq, Ticks time_us)
{
    return ((static_cast<Cycles>(time_us) * clock_freq) / 1000000ULL);
}

} // namespace stk

#endif /* STK_ARCH_COMMON_H_ */
