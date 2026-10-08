/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_EDF_H_
#define STK_STRATEGY_EDF_H_

/*! \file  stk_strategy_edf.h
    \brief Earliest Deadline First (EDF) task-switching strategy (stk::SwitchStrategyEDF).
*/

#include "stk_common.h"

namespace stk {

/*! \class SwitchStrategyEDF
    \brief Earliest Deadline First (EDF) scheduling strategy: always selects the runnable
           task with the least time remaining before its deadline expires.

    \par Selection metric
    GetNext() compares each runnable task's \e relative deadline, obtained via
    \c IKernelTask::GetHrtRelativeDeadline() = \c deadline - \c duration (ticks remaining
    before the task must complete and call Yield()). The task with the \b minimum relative
    deadline, i.e. the one closest to missing its deadline is selected.

    \par HRT mode requirement
    This strategy is only meaningful when used with \c KERNEL_HRT mode. In HRT mode the
    kernel tracks \c duration (active ticks elapsed) per task, making
    \c GetHrtRelativeDeadline() produce meaningful, monotonically decreasing values.
    Outside HRT mode \c GetHrtRelativeDeadline() returns 0 for every task, so every
    comparison ties and the first runnable task in \c m_tasks always wins: the other
    tasks do not run until it sleeps.

    \par Tie-breaking
    When two or more tasks share the same relative deadline the first such task encountered
    during the linear scan of \c m_tasks wins, i.e. the one nearest the list head. List order
    therefore determines tie priority: tasks keep the order in which they were added, except
    that a task which sleeps and wakes is re-appended at the back (see OnTaskWake()). This
    behaviour is implementation-defined and not guaranteed to remain stable across kernel
    versions.

    \par Complexity
    GetNext() performs an O(n) linear scan over all runnable tasks on every call (once per
    kernel tick). This is inherent to EDF: unlike fixed-priority strategies there is no
    O(1) data structure that maintains a sorted deadline order under arbitrary insertions
    and removals without additional memory cost.

    \note  This strategy does not use per-task weights (WEIGHT_API = 0). The EDF deadline
           is tracked internally by the kernel in \c KERNEL_HRT mode, tasks do not need to
           override \c ITask::GetWeight().
    \note  NOSLEEP_YIELD_API = 0 by design: in HRT mode Yield() signals completion of the job and
           the task then sleeps until its next period, the strategy does not handle Yield() itself.
    \note  Requires the kernel Sleep API (SLEEP_EVENT_API = 1): the kernel must call
           OnTaskSleep() and OnTaskWake() to maintain the runnable/sleeping list split.
    \note  Unlike SwitchStrategyRoundRobin and SwitchStrategyFixedPriority, EDF maintains
           no per-task cursor. Task insertion and removal do not update any scheduling state
           beyond the list membership, all scheduling decisions are deferred to GetNext().
    \see   ITaskSwitchStrategy, IKernelTask::GetHrtRelativeDeadline
*/
class SwitchStrategyEDF final : public ITaskSwitchStrategy
{
public:
    /*! \enum  EConfig
        \brief Compile-time capability flags reported to the kernel.
    */
    enum EConfig
    {
        WEIGHT_API               = 0, //!< This strategy does not use per-task weights. Deadline tracking is handled by the kernel in KERNEL_HRT mode via GetHrtRelativeDeadline().
        SLEEP_EVENT_API          = 1, //!< This strategy requires OnTaskSleep() / OnTaskWake() events to move tasks between the runnable and sleeping lists.
        DEADLINE_MISSED_API      = 0, //!< This strategy does not use OnTaskDeadlineMissed() events.
        PRIORITY_INHERITANCE_API = 0, //!< This strategy does not require Priority Inheritance and OnTaskPriorityChange() events.
        NOSLEEP_YIELD_API        = 0  //!< This strategy does not handle Yield() itself (OnTaskYield() is not used); the kernel puts the yielding task to sleep for a tick.
    };

    /*! \brief Construct an empty strategy with no tasks.
    */
    explicit SwitchStrategyEDF() : m_tasks(), m_sleep()
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~SwitchStrategyEDF() = default;

    /*! \brief     Add task to the runnable set.
        \param[in] task: Task to add. Must not be \c NULL and must not already be in any list.
        \note      The task is appended to the back of \c m_tasks. Unlike RR and FP strategies,
                   no cursor or bitmap state is updated here — all scheduling decisions are
                   deferred to GetNext(), which scans \c m_tasks at selection time.
        \note      List order determines tie-breaking: when two tasks share the same relative
                   deadline, the one closer to the list head (added earlier, unless it has
                   since slept and woken, see OnTaskWake()) wins.
    */
    void AddTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == nullptr);

        m_tasks.LinkBack(task);
    }

    /*! \brief     Remove task from whichever list it currently occupies.
        \param[in] task: Task to remove. Must not be \c NULL and must belong to either
                   \c m_tasks or \c m_sleep (asserted).
        \note      Dispatch is by list membership check. No cursor or bitmap state is affected
                   because EDF maintains no per-task scheduling state outside list membership.
    */
    void RemoveTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0U);
        STK_ASSERT((task->GetHead() == &m_tasks) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_tasks)
        {
            m_tasks.Unlink(task);
        }
        else
        {
            m_sleep.Unlink(task);
        }
    }

    /*! \brief     Select and return the task with the earliest (minimum) relative deadline.
        \return    The runnable task whose \c GetHrtRelativeDeadline() is smallest, or
                   \c NULL if \c m_tasks is empty (no runnable tasks — kernel will sleep).
        \note      <b>Algorithm (O(n) linear scan over runnable tasks):</b>
                   -# If \c m_tasks is empty, return \c NULL immediately.
                   -# Initialize \c next to the first task in \c m_tasks and cache its relative
                      deadline as the initial minimum (avoids the need for a special INT32_MAX guard).
                   -# Iterate the remaining tasks; replace \c next whenever a task has a
                      strictly smaller \c GetHrtRelativeDeadline() value.
                   -# Return \c next.
        \note      Tie-breaking: if two tasks share the same relative deadline the one closer
                   to the head of \c m_tasks is returned (see the class documentation).
        \note      This method is called once per kernel tick. On an n-task system it
                   performs n−1 comparisons and n \c GetHrtRelativeDeadline() calls per tick.
    */
    IKernelTask *GetNext() override
    {
        IKernelTask *next = nullptr;

        if (!m_tasks.IsEmpty())
        {
            IKernelTask *const start = (*m_tasks.GetFirst());

            next = start; // initialize earliest found task to the first one

            Timeout earliest = next->GetHrtRelativeDeadline();
            IKernelTask *itr = (*start->GetNext());

            while (itr != start)
            {
                const Timeout deadline = itr->GetHrtRelativeDeadline();

                if (deadline < earliest)
                {
                    earliest = deadline;
                    next     = itr;
                }

                itr = (*itr->GetNext());
            }
        }

        return next;
    }

    /*! \brief     Get first task in the managed set (used by the kernel for initial scheduling).
        \return    The first task in \c m_tasks if any task is runnable; otherwise the first task
                   in \c m_sleep. Asserts if the combined set is empty (GetSize() == 0).
        \note      Returns the list-head task regardless of its deadline. This is called once
                   at kernel start to seed the initial context; EDF ordering begins with the
                   first GetNext() call.
    */
    IKernelTask *GetFirst() override
    {
        STK_ASSERT(GetSize() != 0U);
        
        return (*(!m_tasks.IsEmpty() ? m_tasks.GetFirst() : m_sleep.GetFirst()));
    }

    /*! \brief  Get total number of tasks managed by this strategy.
        \return Sum of tasks in \c m_tasks (runnable) and \c m_sleep (sleeping).
    */
    size_t GetSize() const override
    {
        return m_tasks.GetSize() + m_sleep.GetSize();
    }

    /*! \brief     Notification that a task has entered the sleeping state.
        \param[in] task: The task that is now sleeping. Must be in \c m_tasks (asserted).
        \note      Unlinks from \c m_tasks and appends to \c m_sleep. No cursor or bitmap
                   state is affected — EDF maintains no per-task cursor, so this is a
                   simpler operation than the equivalent in RR or FP strategies.
    */
    void OnTaskSleep(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_tasks);

        m_tasks.Unlink(task);
        m_sleep.LinkBack(task);
    }

    /*! \brief     Notification that a task has become runnable again.
        \param[in] task: The task that woke up. Must be in \c m_sleep (asserted).
        \note      Unlinks from \c m_sleep and appends to the back of \c m_tasks. No priority
                   boost is applied (unlike SwitchStrategySmoothWeightedRoundRobin) and no
                   bitmap is updated (unlike SwitchStrategyFixedPriority). The waking task's
                   deadline urgency is determined naturally by GetHrtRelativeDeadline() on
                   the next GetNext() call.
    */
    void OnTaskWake(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(!task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_sleep);

        m_sleep.Unlink(task);
        m_tasks.LinkBack(task);
    }

protected:
    STK_NONCOPYABLE_CLASS(SwitchStrategyEDF);

    IKernelTask::ListHeadType m_tasks; //!< Runnable tasks eligible for scheduling. Scanned in full by GetNext() each tick to find the minimum relative deadline.
    IKernelTask::ListHeadType m_sleep; //!< Sleeping (blocked) tasks not eligible for scheduling. Deadline tracking continues in the kernel while tasks are in this list.
};

} // namespace stk

#endif /* STK_STRATEGY_EDF_H_ */
