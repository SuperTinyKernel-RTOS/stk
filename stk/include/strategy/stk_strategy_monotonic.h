/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_RM_H_
#define STK_STRATEGY_RM_H_

/*! \file  stk_strategy_monotonic.h
    \brief Rate-Monotonic (RM) and Deadline-Monotonic (DM) task-switching strategies
           (SwitchStrategyMonotonic / SwitchStrategyRM / SwitchStrategyDM), and the
           SchedulabilityCheck utility for Worst-Case Response Time (WCRT) analysis.
*/

#include "stk_common.h"
#include "stk_strategy_rrobin.h"

namespace stk {

/*! \enum  EMonotonicSwitchStrategyType
    \brief Policy selector for SwitchStrategyMonotonic: determines the timing attribute
           used to assign fixed priorities to tasks at AddTask() time.
*/
enum EMonotonicSwitchStrategyType
{
    MSS_TYPE_RATE,    //!< Rate-Monotonic (RM): shorter scheduling period -> higher priority. Priority is derived from GetHrtPeriodicity() and is fixed at task registration.
    MSS_TYPE_DEADLINE //!< Deadline-Monotonic (DM): shorter execution deadline -> higher priority. Priority is derived from GetHrtDeadline() and is fixed at task registration.
};

/*! \class SwitchStrategyMonotonic
    \brief Monotonic scheduling strategy: Rate-Monotonic (RM) or Deadline-Monotonic (DM),
           selected at compile time by the \a _Type template parameter.

    \tparam _Type: Policy selector (EMonotonicSwitchStrategyType):
                   - \c MSS_TYPE_RATE     - Rate-Monotonic: tasks with a \e shorter scheduling
                     period receive higher priority. The most frequently activating task always
                     runs first.
                   - \c MSS_TYPE_DEADLINE - Deadline-Monotonic: tasks with a \e shorter execution
                     deadline receive higher priority.

    \par Sorted-list design
    \c m_tasks is maintained as a priority-sorted intrusive list. AddTask() inserts each new task
    at the correct sorted position (O(n) insertion sort) so that GetNext() need only scan from the
    front: the first non-sleeping task it encounters is always the highest-priority runnable task.
    No re-sorting occurs at runtime after a task is added.

    \par Sleep handling - no separate sleep list
    Unlike RR, SWRR, EDF, and FP strategies, this strategy does \b not maintain a separate sleep
    list (SLEEP_EVENT_API = 0). Sleeping tasks remain in \c m_tasks in their sorted position and
    are skipped by GetNext() via IKernelTask::IsSleeping(). OnTaskSleep() and OnTaskWake() assert
    unconditionally, the kernel must not be configured to deliver these events to this strategy.

    \par HRT mode requirement
    This strategy reads GetHrtPeriodicity() and GetHrtDeadline() from each task during AddTask()
    to determine its priority. These values are only populated in \c KERNEL_HRT mode; using this
    strategy without \c KERNEL_HRT produces undefined priority ordering.

    \note  Does not use per-task weights (WEIGHT_API = 0). Priority is derived entirely from HRT
           timing parameters set via \c IKernel::AddTask(periodicity_tc, deadline_tc, ...).
    \note  For schedulability analysis of the configured task set use
           SchedulabilityCheck::IsSchedulableWCRT().
    \see   SwitchStrategyRM, SwitchStrategyDM, SchedulabilityCheck, ITaskSwitchStrategy
*/
template <EMonotonicSwitchStrategyType _Type>
class SwitchStrategyMonotonic : public ITaskSwitchStrategy
{
public:
    /*! \enum  EConfig
        \brief Compile-time capability flags reported to the kernel.
    */
    enum EConfig
    {
        WEIGHT_API          = 0, //!< This strategy does not use per-task weights. Priority is derived from HRT timing parameters (GetHrtPeriodicity() for RM, GetHrtDeadline() for DM) at AddTask() time.
        SLEEP_EVENT_API     = 0, //!< This strategy does not use OnTaskSleep() / OnTaskWake() events. Sleeping tasks remain in \c m_tasks and are skipped by GetNext() via IKernelTask::IsSleeping(). Delivering these events will trigger an assertion.
        DEADLINE_MISSED_API = 0  //!< This strategy does not use OnTaskDeadlineMissed() events.
    };

    /*! \brief     Add a task to the priority-sorted runnable list.
        \param[in] task: Task to add. Must not be \c NULL and must not already be in any list.
        \note      Performs an O(n) insertion sort into \c m_tasks so that higher-priority tasks
                   appear closer to the head. The sort key depends on \a _Type:
                   - \c MSS_TYPE_RATE:     key = GetHrtPeriodicity() (smaller -> nearer to head).
                   - \c MSS_TYPE_DEADLINE: key = GetHrtDeadline()    (smaller -> nearer to head).
        \note      Three insertion cases handled:
                   -# Empty list: insert at front (LinkFront).
                   -# New task has a strictly smaller key than the current head: insert at front
                      (LinkFront), becoming the new highest-priority task.
                   -# Otherwise: scan forward until a task with a larger-or-equal key is found,
                      then insert immediately before it (Link), or append at back (LinkBack) if
                      the scan reaches the end of the list.
        \note      Tasks with equal keys are ordered by registration time: later-added tasks go
                   behind earlier-added tasks with the same key.
    */
    void AddTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == nullptr);

        if (m_tasks.IsEmpty())
        {
            m_tasks.LinkFront(task);
        }
        else
        {
            IKernelTask *itr = (*m_tasks.GetFirst()), * const start = itr;

            for (;;)
            {
                bool higher_priority;
                switch (_Type)
                {
                case MSS_TYPE_RATE:
                    higher_priority = (task->GetHrtPeriodicity() < itr->GetHrtPeriodicity());
                    break;
                case MSS_TYPE_DEADLINE:
                    higher_priority = (task->GetHrtDeadline() < itr->GetHrtDeadline());
                    break;
                default:
                    STK_ASSERT(false);
                    break;
                }

                if (higher_priority)
                {
                    if (itr == start)
                    {
                        m_tasks.LinkFront(task);
                        break;
                    }

                    m_tasks.Link(task, itr, itr->GetPrev());
                    break;
                }

                // end of the list
                if ((itr = (*itr->GetNext())) == start)
                {
                    m_tasks.LinkBack(task);
                    break;
                }
            }
        }
    }

    /*! \brief     Remove a task from the list.
        \param[in] task: Task to remove. Must not be \c NULL. Must be in \c m_tasks (asserted).
        \note      Unlike RR, SWRR, EDF, and FP strategies there is no separate sleep list:
                   all tasks, runnable and sleeping, reside in \c m_tasks, so this method
                   simply unlinks the task unconditionally without a list-membership check.
    */
    void RemoveTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0);
        STK_ASSERT(task->GetHead() == &m_tasks);

        m_tasks.Unlink(task);
    }

    /*! \brief     Select and return the highest-priority non-sleeping task.
        \return    The first non-sleeping task in \c m_tasks, or \c NULL if every task in
                   the list is currently sleeping (kernel will sleep until the next activation).
        \note      Because \c m_tasks is sorted in descending priority order by AddTask(), a
                   linear scan from the head is sufficient: the first task for which
                   IsSleeping() returns \c false is always the highest-priority runnable task.
                   Complexity is O(k) where k is the number of sleeping tasks at the front of
                   the list (i.e. higher-priority tasks currently between activations).
        \note      Asserts if \c m_tasks is empty; at least one task must be registered before
                   GetNext() is called.
    */
    IKernelTask *GetNext()
    {
        STK_ASSERT(!m_tasks.IsEmpty());

        IKernelTask *itr = (*m_tasks.GetFirst()), * const start = itr;
        IKernelTask *next = nullptr;

        // Scan from head (highest priority). Skip tasks that are sleeping between
        // periodic activations; return the first task ready to run.
        do
        {
            if (!itr->IsSleeping())
            {
                next = itr;
                break; // list is sorted by priority; no need to continue
            }
        }
        while ((itr = (*itr->GetNext())) != start);

        // nullptr means all tasks are sleeping, return idle signal to kernel
        return next;
    }

    /*! \brief     Get the first task in the managed set (used by the kernel for initial scheduling).
        \return    The first task in \c m_tasks (highest priority due to sort order).
                   Asserts if \c m_tasks is empty.
        \note      Because all tasks (runnable and sleeping) reside in \c m_tasks there is no
                   sleep-list fallback path, unlike RR, SWRR, EDF, and FP strategies. The
                   returned task may be sleeping; the kernel uses it only to seed the initial
                   context before the first GetNext() call.
    */
    IKernelTask *GetFirst() const
    {
        STK_ASSERT(m_tasks.GetSize() != 0);

        return (*m_tasks.GetFirst());
    }

    /*! \brief     Get the total number of tasks managed by this strategy.
        \return    Number of tasks in \c m_tasks. Includes both runnable and sleeping tasks
                   since this strategy does not maintain a separate sleep list.
    */
    size_t GetSize() const
    {
        return m_tasks.GetSize();
    }

    /*! \brief     Not supported, asserts unconditionally.
        \note      This strategy uses SLEEP_EVENT_API = 0. Sleeping tasks remain in \c m_tasks
                   in their sorted position and are detected by IsSleeping() in GetNext().
                   Calling this method indicates a kernel/strategy configuration mismatch.
    */
    void OnTaskSleep(IKernelTask */*task*/)
    {
        // Sleep API unsupported: RM/DM keeps a single sorted non-volatile list.
        STK_ASSERT(false);
    }

    /*! \brief     Not supported, asserts unconditionally.
        \note      This strategy uses SLEEP_EVENT_API = 0. See OnTaskSleep() for rationale.
    */
    void OnTaskWake(IKernelTask */*task*/)
    {
        // Sleep API unsupported: RM/DM keeps a single sorted non-volatile list.
        STK_ASSERT(false);
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

private:
    IKernelTask::ListHeadType m_tasks; //!< All tasks (runnable and sleeping) in priority order. GetNext() skips sleeping tasks via IsSleeping(). AddTask() maintains sort order.
};

/*! \class SchedulabilityCheck
    \brief Utility class providing static methods for Worst-Case Response Time (WCRT)
           schedulability analysis of a monotonic HRT task set.

    Determines whether a set of periodic tasks can meet all their deadlines under
    Rate-Monotonic or Deadline-Monotonic scheduling, assuming fully preemptive execution
    and no resource-sharing blocking.

    \note  All methods are \c static. This class is not instantiated, call its methods directly
           as \c SchedulabilityCheck::IsSchedulableWCRT<N>(strategy).
    \see   SwitchStrategyMonotonic, IsSchedulableWCRT, CalculateWCRT
*/
class SchedulabilityCheck
{
public:
    /*! \class TaskTiming
        \brief Execution deadline and scheduling period for a single periodic HRT task,
               used as input to CalculateWCRT() and GetTaskCpuLoad().

        Populated by IsSchedulableWCRT() from each kernel task via GetHrtDeadline() and
        GetHrtPeriodicity() respectively. Entries must be in descending priority order
        (index 0 = highest-priority task).

        \note  In standard WCRT notation: C is the Worst-Case Execution Time, T is the
               period, and D is the deadline. \c duration maps to C (= GetHrtDeadline(),
               the maximum allowed execution time per activation) and \c period maps to T
               (= GetHrtPeriodicity(), the activation interval).
    */
    struct TaskTiming
    {
        uint32_t duration; //!< Worst-Case Execution Time C of the task (ticks), from GetHrtDeadline(). Used as the base cost and interference multiplier Cj in CalculateWCRT().
        uint32_t period;   //!< Scheduling period T of the task (ticks), from GetHrtPeriodicity(). Used as the interference divisor Tj in CalculateWCRT() and the CPU load denominator in GetTaskCpuLoad().
    };

    /*! \class TaskCpuLoad
        \brief CPU utilisation values for a single task, in whole percent.
    */
    struct TaskCpuLoad
    {
        uint16_t task;  //!< CPU load contributed by this task alone, in percent: floor(C / T * 100) = floor(duration * 100 / period).
        uint16_t total; //!< Cumulative CPU load of this task plus all higher-priority tasks, in percent (running sum from index 0).
    };

    /*! \class TaskInfo
        \brief Analysis results for a single task: CPU load and computed WCRT.
    */
    struct TaskInfo
    {
        TaskCpuLoad cpu_load; //!< Per-task and cumulative CPU utilisation (see TaskCpuLoad).
        uint32_t    wcrt;     //!< Worst-Case Response Time in ticks. Task is schedulable if wcrt <= period (T).
    };

    /*! \class SchedulabilityCheckResult
        \brief Result of a WCRT schedulability test: overall verdict plus per-task details.

        The number of tasks is fixed at compile time through the \a _TaskCount template
        parameter, which must equal the number of tasks registered with the kernel strategy.
        Array indices match the priority-sorted task order: index 0 = highest-priority task.

        Usage example:
        \code
        // Analyse 3 HRT tasks registered with a Rate-Monotonic kernel.
        auto *rm = static_cast<stk::SwitchStrategyRM *>(kernel.GetSwitchStrategy());
        auto result = stk::SchedulabilityCheck::IsSchedulableWCRT<3>(rm);
        STK_ASSERT(result); // assert all tasks meet their deadlines
        \endcode
    */
    template <uint32_t _TaskCount>
    struct SchedulabilityCheckResult
    {
        bool     schedulable;      //!< \c true if every task's WCRT <= its period (T); \c false if any task misses its deadline.
        TaskInfo info[_TaskCount]; //!< Per-task analysis results, ordered highest priority first (index 0 = highest priority task).

        /*! \brief  Check whether the full task set is schedulable.
            \return \c true if all tasks meet their deadlines; \c false otherwise.
         */
        operator bool() const { return schedulable; }
    };

    /*! \brief               Perform WCRT schedulability analysis on the task set registered with \a strategy.
        \tparam _TaskCount:  Number of tasks to analyse. Must equal the number of tasks currently
                             registered with \a strategy (asserted at runtime: idx == _TaskCount).
        \param[in] strategy: Pointer to the monotonic scheduling strategy whose task list is analysed.
                             Must not be \c nullptr and must have at least one task registered.
        \return              A SchedulabilityCheckResult<_TaskCount> containing the schedulability
                             verdict and per-task CPU load and WCRT values.
        \note                Tasks are read from the strategy's sorted \c m_tasks list in priority
                             order (index 0 = highest priority). For each task, \c period is
                             populated from GetHrtPeriodicity() and \c duration from GetHrtDeadline()
                             before invoking GetTaskCpuLoad() and CalculateWCRT().
     */
    template <uint32_t _TaskCount>
    static inline SchedulabilityCheckResult<_TaskCount> IsSchedulableWCRT(const ITaskSwitchStrategy *strategy)
    {
        STK_ASSERT(strategy != nullptr);
        STK_ASSERT(strategy->GetFirst() != nullptr);

        const IKernelTask::ListHeadType *ktasks = strategy->GetFirst()->GetHead();

        STK_ASSERT(ktasks != nullptr);
        STK_ASSERT(ktasks->GetSize() <= _TaskCount);

        SchedulabilityCheckResult<_TaskCount> ret;
        TaskTiming tasks[_TaskCount];

        // fill tasks timing
        IKernelTask *itr = (*ktasks->GetFirst()), * const start = itr;
        uint32_t idx = 0;
        do
        {
            STK_ASSERT(idx < _TaskCount);
            tasks[idx].period   = (uint32_t)itr->GetHrtPeriodicity();
            tasks[idx].duration = (uint32_t)itr->GetHrtDeadline();
            ++idx;
        }
        while ((itr = (*itr->GetNext())) != start);
        STK_ASSERT(idx == _TaskCount);

        // calculate CPU load
        GetTaskCpuLoad(tasks, _TaskCount, ret.info);

        // run the WCRT schedulability analysis
        ret.schedulable = CalculateWCRT(tasks, _TaskCount, ret.info);

        return ret;
    }

    /*! \brief     Compute the Worst-Case Response Time (WCRT) for each task in a fixed-priority
                   periodic task set and determine schedulability.

       Evaluates schedulability using standard iterative WCRT recurrence. Assumptions:
         - Fixed priorities, tasks ordered by descending priority (index 0 = highest priority =
           shortest period for RM, or shortest deadline for DM).
         - Fully preemptive execution with no resource-sharing blocking.

       Within this function, local variable \c Cx holds \c tasks[t].duration (WCET C) and
       \c Tx holds \c tasks[t].period (period T), matching standard WCRT notation directly.

       For each task \e t the recurrence is initialised as W(0) = Cx and iterated:

       \code
           W(n+1) = Cx + sum( ceil(W(n) / Tj) * Cj,  for all j < t )
       \endcode

       where the sum runs over all higher-priority tasks j (index < t). Cj =
       tasks[j].duration and Tj = tasks[j].period. Iteration continues until
       convergence (W(n+1) == W(n)) or W(n+1) > Tx (deadline miss confirmed). A \c goto
       is used for the iteration step to avoid re-initialising loop variables inside the
       outer \c for block.

       The highest-priority task (index 0) has no higher-priority interference; its WCRT
       is set directly to its own WCET (\c tasks[0].duration) without iteration.

       \param[in]  tasks: Array of TaskTiming in descending priority order (index 0 = highest).
       \param[in]  count: Number of tasks in \a tasks.
       \param[out] info:  Array of TaskInfo of size \a count. \c info[i].wcrt receives the
                         computed WCRT for task \e i on return.
       \return     \c true if every task's WCRT <= its period (Tx); \c false if any task misses.
     */
    static inline bool CalculateWCRT(const TaskTiming *tasks, const uint32_t count, TaskInfo *info)
    {
        bool schedulable = true;
        info[0].wcrt = tasks[0].duration;

        for (uint32_t t = 1; t < count; )
        {
            uint32_t w,
            Cx = tasks[t].duration,
            Tx = tasks[t].period,
            w0 = Cx;

        next_itr:

            w = Cx;
            for (uint32_t i = 0; i < t; ++i)
                w += idiv_ceil(w0, tasks[i].period) * tasks[i].duration;

            if ((w != w0) && (w <= Tx))
            {
                w0 = w;
                goto next_itr;
            }
            else
            {
                schedulable &= (w <= Tx);
                info[t++].wcrt = w;
            }
        }

        return schedulable;
    }

    /*! \brief      Compute per-task and cumulative CPU utilization, in whole percent.
        \param[in]  tasks Array of TaskTiming in descending priority order (index 0 = highest).
        \param[in]  count Number of tasks in \a tasks.
        \param[out] info  Array of TaskInfo of size \a count. \c info[i].cpu_load is populated on return.
        \note       Per-task load = floor(C / T * 100) = floor(duration * 100 / period),
                    computed with integer arithmetic (truncating division). Cumulative load
                    is the running sum from index 0 to \a count - 1.
    */
    static inline void GetTaskCpuLoad(const TaskTiming *tasks, const uint32_t count, TaskInfo *info)
    {
        uint16_t total = 0;

        for (uint32_t t = 0; t < count; ++t)
        {
            uint16_t task_load = (uint16_t)(tasks[t].duration * 100 / tasks[t].period);
            total += task_load;

            info[t].cpu_load.task  = task_load;
            info[t].cpu_load.total = total;
        }
    }

private:
    //! Integer ceiling division: returns ceil(x / y) = x/y + (x%y > 0 ? 1 : 0).
    //! Equivalent to (int32_t)ceil((float)x / y) but exact and branch-free for integers.
    //! Reference: http://stackoverflow.com/questions/2745074/fast-ceiling-of-an-integer-division-in-c-c
    static __stk_forceinline int32_t idiv_ceil(uint32_t x, uint32_t y)
    {
        return x / y + (x % y > 0);
    }
};

/*! \typedef SwitchStrategyRM
    \brief   Shorthand alias for SwitchStrategyMonotonic<MSS_TYPE_RATE>: Rate-Monotonic scheduling
             (shorter scheduling period -> higher priority).
    \see     SwitchStrategyMonotonic, SchedulabilityCheck
*/
typedef SwitchStrategyMonotonic<MSS_TYPE_RATE> SwitchStrategyRM;

/*! \typedef SwitchStrategyDM
    \brief   Shorthand alias for SwitchStrategyMonotonic<MSS_TYPE_DEADLINE>: Deadline-Monotonic
             scheduling (shorter execution deadline -> higher priority).
    \see     SwitchStrategyMonotonic, SchedulabilityCheck
*/
typedef SwitchStrategyMonotonic<MSS_TYPE_DEADLINE> SwitchStrategyDM;

} // namespace stk

#endif /* STK_STRATEGY_RM_H_ */
