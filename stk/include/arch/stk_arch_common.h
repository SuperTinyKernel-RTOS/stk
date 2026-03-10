/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
        m_stack_active(nullptr), m_tick_resolution(0)
    {}

    /*! \brief     Initialize context.
        \param[in] handler: Event handler.
        \param[in] exit_trap: Exit trap's stack.
        \param[in] resolution_us: Tick resolution in microseconds (for example 1000 equals to 1 millisecond resolution).
    */
    virtual void Initialize(IPlatform::IEventHandler *handler, IKernelService *service, Stack *exit_trap,
        int32_t resolution_us)
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
        STK_ASSERT((hw::PtrToWord(stack_top) & (STK_STACK_MEMORY_ALIGN - 1)) == 0);

        return stack_top;
    }

    IPlatform::IEventHandler *m_handler;         //!< kernel event handler
    IKernelService           *m_service;         //!< kernel service
    Stack                    *m_stack_idle;      //!< idle task stack
    Stack                    *m_stack_active;    //!< active task stack
    int32_t                   m_tick_resolution; //!< system tick resolution (microseconds)

protected:
    STK_NONCOPYABLE_CLASS(PlatformContext);
};

/*! \def   GetContext
    \brief Get platform's context.
*/
/*! \def   SetContext
    \brief Set platform's context.
*/
#ifndef _STK_UNDER_TEST
#if (STK_ARCH_CPU_COUNT == 1)
    #define GetContext() g_Context[0]
#else
    #define GetContext() g_Context[STK_ARCH_GET_CPU_ID()]
#endif
#endif

/*! \def   STK_TIME_TO_CPU_TICKS_USEC
    \brief Convert time (microseconds) to CPU ticks.
*/
#define STK_TIME_TO_CPU_TICKS_USEC(CPU_FREQ, TIME) ((int64_t)(CPU_FREQ) * (TIME) / 1000000)

} // namespace stk

#endif /* STK_ARCH_COMMON_H_ */
