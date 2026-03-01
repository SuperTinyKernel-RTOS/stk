/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_SWRROBIN_H_
#define STK_STRATEGY_SWRROBIN_H_

#include "stk_common.h"

namespace stk {

/*! \class SwitchStrategySmoothWeightedRoundRobin
    \brief Tasks switching strategy - Smooth Weighted Round-Robin (SWRR).

    Smooth WRR distributes CPU time proportionally to weights, but avoids bursts
    by smoothing the task execution over the time. It is an improved variant of
    standard WRR: https://en.wikipedia.org/wiki/Weighted_round_robin

    Algorithm:

    Smooth Weighted Round-Robin (SWRR) uses a dynamic value called "current weight"
    to determine which task should be selected next. Each scheduling step increases
    the current weight of every task by its static weight, and the task with the
    highest current weight gets CPU time. After selection, the total weight of all
    tasks is subtracted from its current weight.

    Weight provided by GetWeight() defines how much CPU time the task should receive
    relative to other tasks participating in scheduling. A higher weight means a
    larger expected share of CPU time. Static weight is configured when a task is
    added to the scheduler.

    The dynamic counter provided by GetCurrentWeight() is modified by the scheduling
    strategy at runtime. During Smooth Weighted Round-Robin scheduling:
       - Every tick: current_weight += weight
       - When selected to run: current_weight -= total_weight_sum

    The task with the highest current weight is selected for execution.
    Over time, this produces fair proportional CPU distribution while
    avoiding long bursts of consecutive execution.
*/
class SwitchStrategySmoothWeightedRoundRobin : public ITaskSwitchStrategy
{
public:
    enum EConfig
    {
        WEIGHT_API      = 1, // strategy is using Weight API of the kernel task
        SLEEP_EVENT_API = 1  // strategy needs OnTaskSleep/OnTaskWake events
    };

    SwitchStrategySmoothWeightedRoundRobin() : m_tasks(), m_sleep(), m_total_weight(0)
    {}

    void AddTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);

        STK_ASSERT((task->GetWeight() > 0) && (task->GetWeight() <= 0x7FFFFF)); // must not be negative, max 24-bit number

        task->SetCurrentWeight(0);

        AddActive(task);
    }

    void RemoveTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == &m_tasks || task->GetHead() == &m_sleep);

        if (task->GetHead() == &m_tasks)
            RemoveActive(task);
        else
            m_sleep.Unlink(task);
    }

    IKernelTask *GetNext()
    {
        if (m_tasks.IsEmpty())
            return nullptr; // idle

        IKernelTask *selected = nullptr;
        int32_t max_weight = INT32_MIN;
        IKernelTask *itr = (*m_tasks.GetFirst()), * const start = itr;

        do
        {
            itr->SetCurrentWeight(itr->GetCurrentWeight() + itr->GetWeight());

            int32_t candidate_weight = itr->GetCurrentWeight();
            if (candidate_weight > max_weight)
            {
                max_weight = candidate_weight;
                selected = itr;
            }
        }
        while ((itr = (*itr->GetNext())) != start);

        STK_ASSERT(selected != nullptr);

        selected->SetCurrentWeight(max_weight - m_total_weight);

        return selected;
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

        // boost priority of the previously sleeping task, this resembles to a RR pattern
        // with tasks having equal weights
        task->SetCurrentWeight(m_total_weight);

        AddActive(task);
    }

private:
    void AddActive(IKernelTask *task)
    {
        m_tasks.LinkBack(task);
        m_total_weight += task->GetWeight();
    }

    void RemoveActive(IKernelTask *task)
    {
        m_tasks.Unlink(task);
        m_total_weight -= task->GetWeight();
    }

    IKernelTask::ListHeadType m_tasks;        //!< runnable tasks
    IKernelTask::ListHeadType m_sleep;        //!< sleeping tasks
    int32_t                   m_total_weight; //!< total weight
};

/*! \typedef SwitchStrategySWRR
    \brief   Shortcut for SwitchStrategySmoothWeightedRoundRobin.
*/
typedef SwitchStrategySmoothWeightedRoundRobin SwitchStrategySWRR;

} // namespace stk

#endif /* STK_STRATEGY_SWRROBIN_H_ */
