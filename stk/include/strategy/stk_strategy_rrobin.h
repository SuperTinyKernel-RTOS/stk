/*
 * SuperTinyKernel™ (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_RROBIN_H_
#define STK_STRATEGY_RROBIN_H_

#include "stk_common.h"

namespace stk {

/*! \class SwitchStrategyRoundRobin
    \brief Tasks switching strategy concrete implementation - Round-Robin.

    Round-Robin: all tasks are given an equal amount of processing time.
*/
class SwitchStrategyRoundRobin : public ITaskSwitchStrategy
{
public:
    enum EConfig
    {
        WEIGHT_API      = 0, // strategy does not need Weight API of the kernel task
        SLEEP_EVENT_API = 1  // strategy needs OnTaskSleep/OnTaskWake events
    };

    SwitchStrategyRoundRobin() : m_tasks(), m_sleep(), m_prev(nullptr)
    {}

    void AddTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == nullptr);

        bool tail = (m_prev == m_tasks.GetLast());

        m_tasks.LinkBack(task);

        // if pointer was pointing to the tail, become a tail
        if (tail)
            m_prev = task;
    }

    void RemoveTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0);
        STK_ASSERT((task->GetHead() == &m_tasks) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_tasks)
            RemoveActive(task);
        else
            m_sleep.Unlink(task);
    }

    IKernelTask *GetNext()
    {
        IKernelTask *ret = m_prev;

        if (ret != nullptr)
        {
            ret    = (*ret->GetNext());
            m_prev = ret;
        }

        return ret;
    }

    IKernelTask *GetFirst() const
    {
        STK_ASSERT(GetSize() != 0);

        if (!m_tasks.IsEmpty())
            return (*m_tasks.GetFirst());
        else
            return (*m_sleep.GetFirst());
    }

    size_t GetSize() const
    {
        return m_tasks.GetSize() + m_sleep.GetSize();
    }

    void OnTaskSleep(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_tasks);

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
        m_tasks.LinkBack(task);

        // update pointer: if all tasks were sleeping, this task will change state
        // of the kernel to active
        if (m_prev == nullptr)
            m_prev = task;
    }

    void RemoveActive(IKernelTask *task)
    {
        IKernelTask *next = (*task->GetNext());

        m_tasks.Unlink(task);

        // update pointer: set to previous task so that GetNext() could return next,
        // if there are no tasks left GetNext() will return nullptr causing a sleep
        // state for the kernel
        if (next != task)
            m_prev = (*next->GetPrev());
        else
            m_prev = nullptr;
    }

    IKernelTask::ListHeadType m_tasks; //!< runnable tasks
    IKernelTask::ListHeadType m_sleep; //!< sleeping tasks
    IKernelTask              *m_prev;  //!< pointer to the previous task
};

/*! \typedef SwitchStrategyRR
    \brief   Shortcut for SwitchStrategyRoundRobin.
*/
typedef SwitchStrategyRoundRobin SwitchStrategyRR;

} // namespace stk

#endif /* STK_STRATEGY_RROBIN_H_ */
