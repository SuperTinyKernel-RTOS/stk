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

/*! \file  stk_strategy_rrobin.h
    \brief Round-Robin task-switching strategy (stk::SwitchStrategyRoundRobin / stk::SwitchStrategyRR).
*/

#include "stk_common.h"

namespace stk {

/*! \class SwitchStrategyRoundRobin
    \brief Round-Robin task-switching strategy: each runnable task receives one time slice
           (one tick interval) in turn before the kernel moves to the next task.

    Internally maintains two intrusive lists:
     - \c m_tasks — tasks currently eligible for scheduling (runnable).
     - \c m_sleep — tasks that called Sleep() or are otherwise blocked.

    The iterator cursor (\c m_prev) points to the most recently scheduled task. On each
    call to GetNext() the cursor advances by one position in \c m_tasks, wrapping around
    at the end (closed-loop list). When \c m_tasks is empty, GetNext() returns \c nullptr
    and the kernel transitions to the sleep trap.

    \note  All runnable tasks receive equal CPU time regardless of creation order or any
           other factor. There is no priority or weight mechanism in this strategy
           (WEIGHT_API = 0).
    \note  Requires the kernel Sleep API (SLEEP_EVENT_API = 1): the kernel must call
           OnTaskSleep() and OnTaskWake() when a task's sleep state changes.
    \see   SwitchStrategyRR, ITaskSwitchStrategy
*/
class SwitchStrategyRoundRobin final : public ITaskSwitchStrategy
{
public:
    /*! \enum  EConfig
        \brief Compile-time capability flags reported to the kernel.
    */
    enum EConfig
    {
        WEIGHT_API               = 0, //!< This strategy does not use per-task weights; all tasks are treated equally.
        SLEEP_EVENT_API          = 1, //!< This strategy requires OnTaskSleep() / OnTaskWake() events to maintain the active/sleep list split.
        DEADLINE_MISSED_API      = 0, //!< This strategy does not use OnTaskDeadlineMissed() events.
        PRIORITY_INHERITANCE_API = 0  //!< This strategy does not require Priority Inheritance and OnTaskPriorityChange() events.
    };

    /*! \brief Construct an empty strategy with no tasks and a null cursor.
    */
    SwitchStrategyRoundRobin() : m_tasks(), m_sleep(), m_prev(nullptr)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~SwitchStrategyRoundRobin()
    {}

    /*! \brief     Add task to the runnable set.
        \param[in] task: Task to add. Must not be \c nullptr and must not already be in any list.
        \note      The task is appended to the back of \c m_tasks.
        \note      Cursor invariant: if \c m_prev was already pointing at the tail before insertion,
                   it is advanced to the new tail so that GetNext() will return the new task on its
                   next iteration rather than skipping it.
    */
    void AddTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == nullptr);

        bool tail = (m_prev == m_tasks.GetLast());

        m_tasks.LinkBack(task);

        // if pointer was pointing to the tail, become a tail
        if (tail)
            m_prev = task;
    }

    /*! \brief     Remove task from whichever list it currently occupies.
        \param[in] task: Task to remove. Must not be \c nullptr and must belong to either
                   \c m_tasks or \c m_sleep (asserted).
        \note      If the task is in \c m_tasks, delegates to RemoveActive() which also
                   updates the cursor. If the task is in \c m_sleep, simply unlinks it.
    */
    void RemoveTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0);
        STK_ASSERT((task->GetHead() == &m_tasks) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_tasks)
            RemoveActive(task);
        else
            m_sleep.Unlink(task);
    }

    /*! \brief     Advance cursor and return the next runnable task.
        \return    Pointer to the next task in \c m_tasks after the cursor position, or
                   \c nullptr if \c m_tasks is empty (no runnable tasks — kernel will sleep).
        \note      The cursor (\c m_prev) is updated to the returned task on each call.
                   Because \c m_tasks is a closed-loop list the cursor wraps automatically
                   from the last task back to the first, producing continuous round-robin rotation.
        \note      If the cursor itself is \c nullptr (all tasks were sleeping and none have woken),
                   the method returns \c nullptr immediately without touching \c m_prev.
    */
    IKernelTask *GetNext() override
    {
        IKernelTask *ret = m_prev;

        if (ret != nullptr)
        {
            ret    = (*ret->GetNext());
            m_prev = ret;
        }

        return ret;
    }

    /*! \brief     Get first task in the managed set (used by the kernel for initial scheduling).
        \return    The first task in \c m_tasks if any task is runnable; otherwise the first task
                   in \c m_sleep. Asserts if the combined set is empty (GetSize() == 0).
        \note      Preference is given to runnable tasks. The sleep fallback allows the kernel to
                   identify any task even when all are currently sleeping.
    */
    IKernelTask *GetFirst() const override
    {
        STK_ASSERT(GetSize() != 0U);

        if (!m_tasks.IsEmpty())
            return (*m_tasks.GetFirst());
        else
            return (*m_sleep.GetFirst());
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
        \note      Moves the task from \c m_tasks to \c m_sleep via RemoveActive(), which
                   also updates the cursor so GetNext() continues correctly.
    */
    void OnTaskSleep(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_tasks);

        RemoveActive(task);
        m_sleep.LinkBack(task);
    }

    /*! \brief     Notification that a task has become runnable again.
        \param[in] task: The task that woke up. Must be in \c m_sleep (asserted).
        \note      Moves the task from \c m_sleep to \c m_tasks via AddActive(), which
                   also restores the cursor if it was null (i.e. this is the first
                   runnable task after a period where all tasks were sleeping).
    */
    void OnTaskWake(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(!task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_sleep);

        m_sleep.Unlink(task);
        AddActive(task);
    }

protected:
    STK_NONCOPYABLE_CLASS(SwitchStrategyRoundRobin);

    /*! \brief     Append a task to \c m_tasks and restore the cursor if necessary.
        \param[in] task: Task to make runnable.
        \note      Cursor invariant: if \c m_prev is \c nullptr (all tasks were previously sleeping),
                   it is set to the newly added task so GetNext() immediately returns a valid task
                   on the next call rather than returning \c nullptr and causing a spurious sleep cycle.
    */
    void AddActive(IKernelTask *task)
    {
        m_tasks.LinkBack(task);

        // update pointer: if all tasks were sleeping, this task will change state
        // of the kernel to active
        if (m_prev == nullptr)
            m_prev = task;
    }

    /*! \brief     Remove a task from \c m_tasks and update the cursor.
        \param[in] task: Runnable task to remove.
        \note      Cursor update algorithm: the cursor must be repositioned so that the \e next
                   call to GetNext() returns the task that would have followed the removed one.
                   - Capture \c next = task->GetNext() (the successor in the closed-loop list)
                     \e before unlinking, while the list links are still valid.
                   - After unlinking, if \c next != \c task (i.e. other tasks remain), set
                     \c m_prev = next->GetPrev(). GetNext() will then advance from \c m_prev
                     to \c next, preserving the round-robin sequence without skipping a task.
                   - If \c next == \c task the removed task was the only element; set \c m_prev
                     to \c nullptr so GetNext() returns \c nullptr and the kernel sleeps.
    */
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

    IKernelTask::ListHeadType m_tasks; //!< Runnable tasks eligible for scheduling.
    IKernelTask::ListHeadType m_sleep; //!< Sleeping (blocked) tasks not eligible for scheduling.
    IKernelTask              *m_prev;  //!< Iterator cursor: the most recently scheduled task, or \c nullptr when no runnable tasks exist. GetNext() advances from this position.
};

/*! \typedef SwitchStrategyRR
    \brief   Shorthand alias for SwitchStrategyRoundRobin.
    \see     SwitchStrategyRoundRobin
*/
typedef SwitchStrategyRoundRobin SwitchStrategyRR;

} // namespace stk

#endif /* STK_STRATEGY_RROBIN_H_ */
