/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_SWRROBIN_H_
#define STK_STRATEGY_SWRROBIN_H_

/*! \file  stk_strategy_swrrobin.h
    \brief Smooth Weighted Round-Robin task-switching strategy
           (stk::SwitchStrategySmoothWeightedRoundRobin / stk::SwitchStrategySWRR, and the no-sleep-yield
           variants stk::SwitchStrategySmoothWeightedRoundRobinNoSleepYield / stk::SwitchStrategySWRR_NSY).
*/

#include "stk_common.h"

namespace stk {

/*! \class SwitchStrategySmoothWeightedRoundRobinT
    \brief Smooth Weighted Round-Robin (SWRR) task-switching strategy: distributes CPU time
           proportionally to per-task weights while avoiding execution bursts by spreading
           selections evenly over time.

    This is an improved variant of standard Weighted Round-Robin.
    \see https://en.wikipedia.org/wiki/Weighted_round_robin

    \par Algorithm
    Each task carries two weight values:
     - <b>Static weight</b> (`GetWeight()`): configured by the application via ITask::GetWeight().
       Defines the task's desired CPU share relative to other tasks. Must be a positive 24-bit
       integer (1 .. 0x7FFFFF). A task with weight 3 receives three times as much CPU time as
       one with weight 1.
     - <b>Dynamic (current) weight</b> (`GetCurrentWeight()` / `SetCurrentWeight()`): modified
       each scheduling step by the algorithm below. Starts at 0 when a task is added.

    On every call to GetNext() (once per kernel tick):
    -# For every runnable task: `current_weight += static_weight`
    -# Select the task with the highest `current_weight` as the next to run.
    -# For the selected task only: `current_weight -= total_weight_sum`
       (where `total_weight_sum` is the sum of static weights of all \e runnable tasks).

    Over time this produces fair, proportional CPU distribution without consecutive bursts.

    \par No-sleep yield
    When \c NoSleepYield is true, Yield() keeps the calling task runnable (it is NOT put to sleep).
    OnTaskYield() remembers the yielding task and the next GetNext() excludes it from the selection
    (one-shot). The yielding task still accrues its static weight in that step, so the proportional
    accounting is preserved, but it cannot win the selection unless it is the only runnable task.
    If \c NoSleepYield is false the kernel performs the legacy yield: the task sleeps for YIELD_TICKS.

    \par Wake-up priority boost
    When a sleeping task is woken (OnTaskWake()), its `current_weight` is set to
    `total_weight_sum` so it is selected on the very next tick. This prevents starvation
    of tasks that had been blocking on I/O or synchronization objects.

    \tparam NoSleepYield: If true, Yield() keeps the task in the runnable set and the strategy skips
                          the yielding task in the next selection (see OnTaskYield). If false, the kernel
                          performs the legacy yield: the task sleeps for YIELD_TICKS.

    \note  Requires the Weight API (WEIGHT_API = 1): the kernel must provide `GetWeight()`,
           `GetCurrentWeight()`, and `SetCurrentWeight()` on each kernel task.
    \note  Requires the kernel Sleep API (SLEEP_EVENT_API = 1): the kernel must call
           OnTaskSleep() and OnTaskWake() to keep \c m_total_weight consistent as tasks
           enter and leave the runnable set.
    \note  GetNext() iterates over all runnable tasks — O(n) per tick. For large task counts
           the constant factor is small (integer arithmetic only), but this should be considered
           when sizing TASKS_MAX on severely constrained targets.
    \see   SwitchStrategySWRR, SwitchStrategySWRR_NSY, ITaskSwitchStrategy, ITask::GetWeight, IKernelTask::GetWeight
*/
template <bool NoSleepYield> class SwitchStrategySmoothWeightedRoundRobinT final : public ITaskSwitchStrategy
{
public:
    /*! \enum  EConfig
        \brief Compile-time capability flags reported to the kernel.
    */
    enum EConfig
    {
        WEIGHT_API               = 1, //!< This strategy uses per-task static and dynamic weights; the kernel must expose the Weight API on each IKernelTask.
        SLEEP_EVENT_API          = 1, //!< This strategy requires OnTaskSleep() / OnTaskWake() events to keep \c m_total_weight accurate as tasks move between the runnable and sleeping sets.
        DEADLINE_MISSED_API      = 0, //!< This strategy does not use OnTaskDeadlineMissed() events.
        PRIORITY_INHERITANCE_API = 0, //!< This strategy does not require Priority Inheritance and OnTaskPriorityChange() events.
        NOSLEEP_YIELD_API        = NoSleepYield //!< (1) enables OnTaskYield(): yielding task stays runnable instead of sleeping.
    };

    /*! \brief Construct an empty strategy with no tasks and a zero total weight.
    */
    explicit SwitchStrategySmoothWeightedRoundRobinT() : m_tasks(), m_sleep(), m_yielded(), m_total_weight(0)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~SwitchStrategySmoothWeightedRoundRobinT() = default;

    /*! \brief     Add task to the runnable set.
        \param[in] task: Task to add. Must not be \c nullptr.
        \note      The task's static weight (GetWeight()) is validated: must be a positive
                   24-bit integer [1, 0x7FFFFF]. Negative or zero weights would break
                   the algorithm invariant. Values above 0x7FFFFF would risk int32_t overflow
                   in the accumulated current-weight arithmetic.
        \note      The dynamic weight (current weight) is reset to 0 before the task is added
                   to the runnable set. This gives all newly-added tasks an equal starting
                   position in the selection cycle.
        \note      Delegates to AddActive() which also increments \c m_total_weight.
    */
    void AddTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT((task->GetWeight() > 0) && (task->GetWeight() <= 0x7FFFFF)); // must not be negative, max 24-bit number

        task->SetCurrentWeight(NO_WEIGHT);

        AddActive(task);
    }

    /*! \brief     Remove task from whichever list it currently occupies.
        \param[in] task: Task to remove. Must not be \c NULL and must belong to either
                   \c m_tasks or \c m_sleep (asserted).
        \note      If the task is in \c m_tasks, delegates to RemoveActive() which also
                   decrements \c m_total_weight. If the task is in \c m_sleep, simply
                   unlinks it (\c m_total_weight is not adjusted since sleeping tasks are
                   already excluded from the weight sum).
    */
    void RemoveTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT((task->GetHead() == &m_tasks) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_tasks)
        {
            RemoveActive(task);
        }
        else
        {
            m_sleep.Unlink(task);
        }
    }

    /*! \brief     Select and return the next task to run, applying one step of the SWRR algorithm.
        \return    The task with the highest current weight after the update step, or \c NULL
                   if \c m_tasks is empty (no runnable tasks - kernel will sleep).
        \note      <b>Algorithm applied per call (O(n) over runnable tasks):</b>
                   -# For every task in \c m_tasks: `current_weight += static_weight`.
                   -# Scan for the task with the maximum `current_weight`
                      (initial sentinel: `INT32_MIN` - never equals a valid weight).
                   -# Deduct the total runnable weight from the winner:
                      `selected->current_weight -= m_total_weight`.
        \note      With \c NoSleepYield the task recorded by OnTaskYield() is excluded from step 2
                   (one-shot, it still receives the step 1 increment). If it is the only runnable
                   task it is selected anyway.
        \note      The assertion `selected != NULL` guards against a logic error where
                   \c m_tasks is non-empty but no task was picked (should never occur).
    */
    IKernelTask *GetNext() override
    {
        IKernelTask *next = nullptr;

        if (!m_tasks.IsEmpty())
        {
            IKernelTask *skip = nullptr;

            if __stk_constexpr_cpp17 (NoSleepYield)
            {
                skip         = m_yielded[0]; // one-shot: excluded from this selection only
                m_yielded[0] = nullptr;
            }
            else
            {
                STK_UNUSED(skip);
            }

            IKernelTask *itr         = (*m_tasks.GetFirst());
            IKernelTask *const start = itr;
            int32_t max_weight       = INT32_MIN;

            do
            {
                const int32_t candidate_weight = itr->GetCurrentWeight() + itr->GetWeight();
                itr->SetCurrentWeight(candidate_weight); // yielding task still accrues weight

                if __stk_constexpr_cpp17 (NoSleepYield)
                {
                    if ((itr != skip) && (candidate_weight > max_weight))
                    {
                        max_weight = candidate_weight;
                        next = itr;
                    }
                }
                else
                {
                    if (candidate_weight > max_weight)
                    {
                        max_weight = candidate_weight;
                        next = itr;
                    }
                }

                itr = (*itr->GetNext());
            }
            while (itr != start);

            if __stk_constexpr_cpp17 (NoSleepYield)
            {
                // yielding task is the only runnable task: nobody to hand over to
                if (next == nullptr)
                {
                    STK_ASSERT(skip != nullptr);

                    next       = skip;
                    max_weight = skip->GetCurrentWeight();
                }
            }

            STK_ASSERT(next != nullptr);

            next->SetCurrentWeight(max_weight - m_total_weight);
        }

        return next;
    }

    /*! \brief     Get first task in the managed set (used by the kernel for initial scheduling).
        \return    The first task in \c m_tasks if any task is runnable, otherwise the first task
                   in \c m_sleep. Asserts if the combined set is empty (GetSize() == 0).
        \note      Preference is given to runnable tasks. The sleep fallback allows the kernel to
                   identify any task even when all are currently sleeping.
    */
    IKernelTask *GetFirst() override
    {
        STK_ASSERT(GetSize() != 0U);
        
        return (*(!m_tasks.IsEmpty() ? m_tasks.GetFirst() : m_sleep.GetFirst()));
    }

    /*! \brief  Get the total number of tasks managed by this strategy.
        \return Sum of tasks in \c m_tasks (runnable) and \c m_sleep (sleeping).
    */
    size_t GetSize() const override
    {
        return m_tasks.GetSize() + m_sleep.GetSize();
    }

    /*! \brief     Notification that a task has entered the sleeping state.
        \param[in] task: The task that is now sleeping. Must be in \c m_tasks (asserted).
        \note      Moves the task from \c m_tasks to \c m_sleep via RemoveActive(), which
                   also decrements \c m_total_weight by the task's static weight. Sleeping
                   tasks do not participate in weight distribution until they wake.
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
        \note      Applies a priority boost before re-inserting into \c m_tasks:
                   the task's current weight is set to \c m_total_weight (the sum of all
                   runnable tasks' static weights). On the next GetNext() call every
                   runnable task increases its current weight by its static weight, but the
                   waking task starts from \c m_total_weight, giving it the highest initial
                   value and guaranteeing it is selected first. This prevents starvation of
                   tasks that had been blocking on I/O or synchronization objects, and mimics
                   the fairness behaviour of plain Round-Robin for equal-weight tasks.
        \note      After the boost, delegates to AddActive() which increments \c m_total_weight.
    */
    void OnTaskWake(IKernelTask *task) override
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

    /*! \brief     Notification that the running task called Yield() (non-HRT kernel modes only).
        \param[in] task: Pointer to the yielding task, currently in the runnable set.
        \return    \c true if the strategy handled the yield itself: the task stays runnable (it is
                   NOT put to sleep) and the following GetNext() will not select it (unless it is
                   the only runnable task); \c false if the kernel shall perform the legacy yield,
                   i.e. sleep the task for YIELD_TICKS.
        \note      The exclusion is one-shot and is dropped if the task leaves the runnable set
                   before the next GetNext() (see RemoveActive()).
    */
    bool OnTaskYield(IKernelTask *task) override
    {
        if __stk_constexpr_cpp17 (NoSleepYield)
        {
            STK_ASSERT(task != nullptr);

            bool handled = false;

            if (task->GetHead() == &m_tasks)
            {
                m_yielded[0] = task; // excluded from the next selection only
                handled      = true;
            }

            return handled;
        }
        else
        {
            STK_UNUSED(task);
            return false;
        }
    }

private:
    STK_NONCOPYABLE_CLASS(SwitchStrategySmoothWeightedRoundRobinT);

    /*! \brief     Append task to \c m_tasks and update the total weight.
        \param[in] task: Task to make runnable.
        \note      Increments \c m_total_weight by the task's static weight so that the
                   post-selection deduction in GetNext() remains correct.
    */
    void AddActive(IKernelTask *task)
    {
        m_tasks.LinkBack(task);
        m_total_weight += task->GetWeight();
    }

    /*! \brief     Remove task from \c m_tasks and update the total weight.
        \param[in] task: Runnable task to remove.
        \note      Decrements \c m_total_weight by the task's static weight so that the
                   post-selection deduction in GetNext() and the wake-up boost in
                   OnTaskWake() remain proportionally correct for the remaining tasks.
                   With \c NoSleepYield also drops a pending yield exclusion of this task.
    */
    void RemoveActive(IKernelTask *task)
    {
        if __stk_constexpr_cpp17 (NoSleepYield)
        {
            // do not leave a dangling pointer to a task that is leaving the runnable set
            if (m_yielded[0] == task)
            {
                m_yielded[0] = nullptr;
            }
        }

        m_tasks.Unlink(task);
        m_total_weight -= task->GetWeight();
    }

    IKernelTask::ListHeadType m_tasks;        //!< Runnable tasks eligible for scheduling.
    IKernelTask::ListHeadType m_sleep;        //!< Sleeping (blocked) tasks not eligible for scheduling.
    IKernelTask              *m_yielded[STK_ALLOCATE_COUNT<NoSleepYield, 1U, 1U, 0U>::Value]; //!< Task that just yielded, skipped by the next GetNext() only. Zero-size without NoSleepYield on GCC/Clang, one entry on MSVC/IAR.
    int32_t                   m_total_weight; //!< Sum of static weights (GetWeight()) of all tasks currently in \c m_tasks. Sleeping tasks are excluded. Updated on every AddActive() / RemoveActive() call. Used as the post-selection deduction amount in GetNext() and as the wake-up boost value in OnTaskWake().
};

/*! \typedef SwitchStrategySmoothWeightedRoundRobin
    \brief   Shorthand alias for SwitchStrategySmoothWeightedRoundRobinT<false>.
    \see     SwitchStrategySmoothWeightedRoundRobinT
*/
typedef SwitchStrategySmoothWeightedRoundRobinT<false> SwitchStrategySmoothWeightedRoundRobin;

/*! \typedef SwitchStrategySWRR
    \brief   Shorthand alias for SwitchStrategySmoothWeightedRoundRobin.
    \see     SwitchStrategySmoothWeightedRoundRobinT
*/
typedef SwitchStrategySmoothWeightedRoundRobinT<false> SwitchStrategySWRR;

/*! \typedef SwitchStrategySmoothWeightedRoundRobinNoSleepYield
    \brief   Smooth Weighted Round-Robin strategy where Yield() keeps the task runnable (NOSLEEP_YIELD_API = 1).
    \see     SwitchStrategySmoothWeightedRoundRobinT
*/
typedef SwitchStrategySmoothWeightedRoundRobinT<true> SwitchStrategySmoothWeightedRoundRobinNoSleepYield;

/*! \typedef SwitchStrategySWRR_NSY
    \brief   Shorthand for SwitchStrategySmoothWeightedRoundRobinNoSleepYield.
    \see     SwitchStrategySmoothWeightedRoundRobinNoSleepYield
*/
typedef SwitchStrategySmoothWeightedRoundRobinT<true> SwitchStrategySWRR_NSY;

} // namespace stk

#endif /* STK_STRATEGY_SWRROBIN_H_ */
