/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_FPRIORITY_H_
#define STK_STRATEGY_FPRIORITY_H_

#include "stk_common.h"

namespace stk {

/*! \class SwitchStrategyFixedPriority
    \brief Fixed-priority preemptive scheduling with round-robin within same priority.

    * Higher priority tasks always preempt lower ones.
    * Tasks of equal priority are scheduled in round-robin fashion.
    * Higher numeric value means higher priority.
    * 0 is the lowest priority.
    * MAX_PRIORITIES - 1 is the highest priority.
*/
template <uint8_t MAX_PRIORITIES>
class SwitchStrategyFixedPriority : public ITaskSwitchStrategy
{
public:
    enum EConfig
    {
        WEIGHT_API      = 1, // uses GetWeight() as priority
        SLEEP_EVENT_API = 1  // uses OnTaskSleep/OnTaskWake
    };

    /*! \enum  EPriority
        \brief Task priority.
    */
    enum EPriority
    {
        PRIORITY_HIGHEST = MAX_PRIORITIES - 1,
        PRIORITY_NORMAL  = MAX_PRIORITIES / 2,
        PRIORITY_LOWEST  = 0
    };

    SwitchStrategyFixedPriority() : m_tasks(), m_sleep(), m_ready_bitmap(0), m_prev()
    {
        STK_STATIC_ASSERT(MAX_PRIORITIES <= 32 && "MAX_PRIORITIES exceeds 32-bit bitmap width");
    }

    void AddTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == nullptr);
        STK_ASSERT((uint8_t)task->GetWeight() < MAX_PRIORITIES);

        const uint8_t prio = (uint8_t)task->GetWeight();

        bool is_tail = (m_prev[prio] == m_tasks[prio].GetLast());

        AddActive(task);

        // if pointer was pointing to the tail, become a tail
        if (is_tail)
            m_prev[prio] = task;
    }

    void RemoveTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0);
        STK_ASSERT((task->GetHead() == &m_tasks[(uint8_t)task->GetWeight()]) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_sleep)
            m_sleep.Unlink(task);
        else
            RemoveActive(task);
    }

    IKernelTask *GetNext()
    {
        if (m_ready_bitmap == 0)
            return nullptr; // idle

        const uint8_t prio = GetHighestReadyPriority(m_ready_bitmap);

        IKernelTask *ret = (*m_prev[prio]->GetNext());
        m_prev[prio] = ret;

        return ret;
    }

    IKernelTask *GetFirst() const
    {
        STK_ASSERT(GetSize() != 0);

        if (m_ready_bitmap == 0)
            return (*m_sleep.GetFirst());

        const uint8_t prio = GetHighestReadyPriority(m_ready_bitmap);
        return (*m_tasks[prio].GetFirst());
    }

    size_t GetSize() const
    {
        size_t total = m_sleep.GetSize();
        for (uint8_t i = 0; i < MAX_PRIORITIES; ++i)
            total += m_tasks[i].GetSize();

        return total;
    }

    void OnTaskSleep(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_tasks[(uint8_t)task->GetWeight()]);

        RemoveActive(task);
        m_sleep.LinkBack(task);
    }

    void OnTaskWake(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(!task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_sleep);

        m_sleep.Unlink(task);
        AddActive(task);
    }

protected:
    void AddActive(IKernelTask *task)
    {
        const uint8_t prio = (uint8_t)task->GetWeight();

        m_tasks[prio].LinkBack(task);

        // init pointer
        if (m_tasks[prio].GetSize() == 1)
        {
            m_prev[prio] = task;

            m_ready_bitmap |= (1u << prio);
        }
    }

    void RemoveActive(IKernelTask *task)
    {
        const uint8_t prio = (uint8_t)task->GetWeight();
        IKernelTask *next = (*task->GetNext());

        m_tasks[prio].Unlink(task);

        // update pointer
        if (next != task)
        {
            m_prev[prio] = (*next->GetPrev());
        }
        else
        {
            m_prev[prio] = nullptr;

            // this will cause a switch to a lower priority task list
            m_ready_bitmap &= ~(1u << prio);
        }
    }

    static __stk_forceinline uint8_t GetHighestReadyPriority(uint32_t bitmap)
    {
    #if defined(__GNUC__)
        return (uint8_t)(31u - __builtin_clz(bitmap));
    #else
        for (int32_t i = 31; i >= 0; --i)
        {
            if (bitmap & (1u << i))
                return (uint8_t)i;
        }
        return 0;
    #endif
    }

private:
    IKernelTask::ListHeadType m_tasks[MAX_PRIORITIES]; //!< runnable tasks per priority
    IKernelTask::ListHeadType m_sleep;                 //!< sleeping tasks (priority irrelevant)
    uint32_t                  m_ready_bitmap;          //!< bit = priority has runnable tasks
    IKernelTask              *m_prev[MAX_PRIORITIES];  //!< round-robin cursor per priority
};

/*! \typedef SwitchStrategyFP32
    \brief   Shortcut for SwitchStrategyFixedPriority<32>.
*/
typedef SwitchStrategyFixedPriority<32> SwitchStrategyFP32;

} // namespace stk

#endif /* STK_STRATEGY_FPRIORITY_H_ */
