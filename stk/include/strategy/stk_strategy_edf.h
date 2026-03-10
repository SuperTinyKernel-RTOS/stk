/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_EDF_H_
#define STK_STRATEGY_EDF_H_

/*! \file  stk_strategy_edf.h
    \brief Earliest Deadline First (EDF) task-switching strategy (SwitchStrategyEDF).
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
    Outside HRT mode \c GetHrtRelativeDeadline() returns 0 for every task, making
    selection order arbitrary.

    \par Tie-breaking
    When two or more tasks share the same relative deadline the first such task encountered
    during the linear scan of \c m_tasks wins. Insertion order therefore determines tie
    priority. This behaviour is implementation-defined and not guaranteed to remain stable
    across kernel versions.

    \par Complexity
    GetNext() performs an O(n) linear scan over all runnable tasks on every call (once per
    kernel tick). This is inherent to EDF: unlike fixed-priority strategies there is no
    O(1) data structure that maintains a sorted deadline order under arbitrary insertions
    and removals without additional memory cost.

    \note  This strategy does not use per-task weights (WEIGHT_API = 0). The EDF deadline
           is tracked internally by the kernel in \c KERNEL_HRT mode, tasks do not need to
           override \c ITask::GetWeight().
    \note  Requires the kernel Sleep API (SLEEP_EVENT_API = 1): the kernel must call
           OnTaskSleep() and OnTaskWake() to maintain the runnable/sleeping list split.
    \note  Unlike SwitchStrategyRoundRobin and SwitchStrategyFixedPriority, EDF maintains
           no per-task cursor. Task insertion and removal do not update any scheduling state
           beyond the list membership, all scheduling decisions are deferred to GetNext().
    \see   ITaskSwitchStrategy, IKernelTask::GetHrtRelativeDeadline
*/
class SwitchStrategyEDF : public ITaskSwitchStrategy
{
public:
    /*! \enum  EConfig
        \brief Compile-time capability flags reported to the kernel.
    */
    enum EConfig
    {
        WEIGHT_API          = 0, //!< This strategy does not use per-task weights. Deadline tracking is handled by the kernel in KERNEL_HRT mode via GetHrtRelativeDeadline().
        SLEEP_EVENT_API     = 1, //!< This strategy requires OnTaskSleep() / OnTaskWake() events to move tasks between the runnable and sleeping lists.
        DEADLINE_MISSED_API = 0  //!< This strategy does not use OnTaskDeadlineMissed() events.
    };

    /*! \brief Construct an empty strategy with no tasks.
    */
    explicit SwitchStrategyEDF() : m_tasks(), m_sleep()
    {}

    /*! \brief     Add task to the runnable set.
        \param[in] task: Task to add. Must not be \c NULL and must not already be in any list.
        \note      The task is appended to the back of \c m_tasks. Unlike RR and FP strategies,
                   no cursor or bitmap state is updated here — all scheduling decisions are
                   deferred to GetNext(), which scans \c m_tasks at selection time.
        \note      Insertion order determines tie-breaking: when two tasks share the same relative
                   deadline, the one added earlier (closer to the list head) wins.
    */
    void AddTask(IKernelTask *task)
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
    void RemoveTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0);
        STK_ASSERT((task->GetHead() == &m_tasks) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_tasks)
            m_tasks.Unlink(task);
        else
            m_sleep.Unlink(task);
    }

    /*! \brief     Select and return the task with the earliest (minimum) relative deadline.
        \return    The runnable task whose \c GetHrtRelativeDeadline() is smallest, or
                   \c NULL if \c m_tasks is empty (no runnable tasks — kernel will sleep).
        \note      <b>Algorithm (O(n) linear scan over runnable tasks):</b>
                   -# If \c m_tasks is empty, return \c NULL immediately.
                   -# Initialise \c earliest to the first task in \c m_tasks (acts as the
                      initial minimum sentinel — avoids the need for a special INT32_MAX guard).
                   -# Iterate the remaining tasks; replace \c earliest whenever a task has a
                      strictly smaller \c GetHrtRelativeDeadline() value.
                   -# Return \c earliest.
        \note      Tie-breaking: if two tasks share the same relative deadline the one closer
                   to the head of \c m_tasks (i.e. added earlier) is returned. This is
                   consistent with AddTask() insertion order.
        \note      This method is called once per kernel tick. On an n-task system it
                   performs n−1 comparisons and n \c GetHrtRelativeDeadline() calls per tick.
    */
    IKernelTask *GetNext()
    {
        if (m_tasks.IsEmpty())
            return nullptr; // idle

        IKernelTask *itr = (*m_tasks.GetFirst()), * const start = itr;
        IKernelTask *earliest = itr;

        do
        {
            if (itr->GetHrtRelativeDeadline() < earliest->GetHrtRelativeDeadline())
                earliest = itr;
        }
        while ((itr = (*itr->GetNext())) != start);

        return earliest;
    }

    /*! \brief     Get first task in the managed set (used by the kernel for initial scheduling).
        \return    The first task in \c m_tasks if any task is runnable; otherwise the first task
                   in \c m_sleep. Asserts if the combined set is empty (GetSize() == 0).
        \note      Returns the list-head task regardless of its deadline. This is called once
                   at kernel start to seed the initial context; EDF ordering begins with the
                   first GetNext() call.
    */
    IKernelTask *GetFirst() const
    {
        STK_ASSERT(GetSize() != 0);

        if (!m_tasks.IsEmpty())
            return (*m_tasks.GetFirst());
        else
            return (*m_sleep.GetFirst());
    }

    /*! \brief  Get total number of tasks managed by this strategy.
        \return Sum of tasks in \c m_tasks (runnable) and \c m_sleep (sleeping).
    */
    size_t GetSize() const
    {
        return m_tasks.GetSize() + m_sleep.GetSize();
    }

    /*! \brief     Notification that a task has entered the sleeping state.
        \param[in] task: The task that is now sleeping. Must be in \c m_tasks (asserted).
        \note      Unlinks from \c m_tasks and appends to \c m_sleep. No cursor or bitmap
                   state is affected — EDF maintains no per-task cursor, so this is a
                   simpler operation than the equivalent in RR or FP strategies.
    */
    void OnTaskSleep(IKernelTask *task)
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
    void OnTaskWake(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(!task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_sleep);

        m_sleep.Unlink(task);
        m_tasks.LinkBack(task);
    }

    /*! \brief     Not supported, asserts unconditionally.
        \note      This strategy uses DEADLINE_MISSED_API = 0. See OnTaskDeadlineMissed() for rationale.
    */
    bool OnTaskDeadlineMissed(IKernelTask */*task*/)
    {
        // Budget Overrun API unsupported
        STK_ASSERT(false);
        return false;
    }

protected:
    STK_NONCOPYABLE_CLASS(SwitchStrategyEDF);

    IKernelTask::ListHeadType m_tasks; //!< Runnable tasks eligible for scheduling. Scanned in full by GetNext() each tick to find the minimum relative deadline.
    IKernelTask::ListHeadType m_sleep; //!< Sleeping (blocked) tasks not eligible for scheduling. Deadline tracking continues in the kernel while tasks are in this list.
};

} // namespace stk

#endif /* STK_STRATEGY_EDF_H_ */
