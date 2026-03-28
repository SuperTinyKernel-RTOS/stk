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
    ~PlatformContext()
    {}

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
        \return    Pointer to initialized stack memory.
    */
    static inline Word *InitStackMemory(IStackMemory *memory)
    {
        size_t stack_size = memory->GetStackSize();
        Word *itr = memory->GetStack();
        Word *stack_top = itr + stack_size;

        STK_ASSERT(stack_size >= STACK_SIZE_MIN);

        // initialization of the stack memory satisfies stack integrity check in Kernel::StateSwitch
        while (itr < stack_top)
            *itr++ = STK_STACK_MEMORY_FILLER;

        // expecting STK_STACK_MEMORY_ALIGN-byte aligned memory for a stack
        STK_ASSERT((hw::PtrToWord(stack_top) & (STK_STACK_MEMORY_ALIGN - 1)) == 0U);

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
static __stk_forceinline Cycles ConvertTimeUsToClockCycles(Cycles clock_freq, Ticks time_us)
{
    return ((clock_freq * static_cast<Cycles>(time_us)) / 1000000ULL);
}

} // namespace stk

#endif /* STK_ARCH_COMMON_H_ */
