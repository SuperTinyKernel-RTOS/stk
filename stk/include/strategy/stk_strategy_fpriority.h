/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_FPRIORITY_H_
#define STK_STRATEGY_FPRIORITY_H_

/*! \file  stk_strategy_fpriority.h
    \brief Fixed-priority preemptive task-switching strategy with round-robin within each
           priority level (stk::SwitchStrategyFixedPriority / stk::SwitchStrategyFP32).
*/

#include "stk_common.h"

namespace stk {

/*! \class SwitchStrategyFixedPriority
    \brief Fixed-priority preemptive scheduling strategy with round-robin arbitration
           within each priority level.

    \tparam MAX_PRIORITIES  Number of distinct priority levels. Must be in the range [1, 32]
                            (enforced by a compile-time assertion). Bit \c i of the 32-bit
                            \c m_ready_bitmap represents priority level \c i.
                            The concrete alias SwitchStrategyFP32 uses MAX_PRIORITIES = 32.

    \par Priority assignment
    Each task's priority is read from \c ITask::GetWeight(), cast to \c uint8_t, and must
    be in the range [0, MAX_PRIORITIES - 1]. Tasks do \b not use the dynamic current-weight
    mechanism, the static weight value \e is the priority. Configure a task's priority by
    overriding \c ITask::GetWeight() to return the desired level.

    \par Scheduling rules
    - The highest runnable priority level is always selected. A task at priority \c N will
      never run while any task at priority > \c N is runnable.
    - Tasks at the same priority level are scheduled in round-robin order (one tick slice
      each), using a per-level cursor (\c m_prev[prio]).
    - Higher numeric value = higher priority. 0 is the lowest, MAX_PRIORITIES - 1 is highest.

    \par Priority detection — O(1) bitmap
    \c m_ready_bitmap is a 32-bit bitmask where bit \c i is set whenever priority level \c i
    has at least one runnable task. GetNext() and GetFirst() locate the highest set bit in
    O(1) via \c __builtin_clz (GCC/Clang) or a portable 32->0 scan fallback. The bitmap is
    kept consistent by AddActive() (bit set) and RemoveActive() (bit cleared on last removal).

    \note  Uses the Weight API (WEIGHT_API = 1): \c GetWeight() is interpreted as the task's
           fixed priority level. The dynamic weight functions (SetCurrentWeight /
           GetCurrentWeight) are \b not used by this strategy.
    \note  Requires the kernel Sleep API (SLEEP_EVENT_API = 1): OnTaskSleep() and OnTaskWake()
           maintain the per-priority runnable lists and keep \c m_ready_bitmap accurate.
    \see   SwitchStrategyFP32, ITaskSwitchStrategy, ITask::GetWeight
*/
template <uint8_t MAX_PRIORITIES>
class SwitchStrategyFixedPriority : public ITaskSwitchStrategy
{
public:
    /*! \enum  EConfig
        \brief Compile-time capability flags reported to the kernel.
    */
    enum EConfig
    {
        WEIGHT_API          = 1, //!< This strategy interprets GetWeight() as the task's fixed priority level (0 .. MAX_PRIORITIES - 1). Dynamic weight functions are not used.
        SLEEP_EVENT_API     = 1, //!< This strategy requires OnTaskSleep() / OnTaskWake() events to maintain per-priority runnable lists and keep \c m_ready_bitmap accurate.
        DEADLINE_MISSED_API = 0  //!< This strategy does not use OnTaskDeadlineMissed() events.
    };

    /*! \enum  EPriority
        \brief Symbolic priority level constants for common use cases.
        \note  Concrete values depend on MAX_PRIORITIES. For SwitchStrategyFP32 (MAX_PRIORITIES = 32):
               PRIORITY_HIGHEST = 31, PRIORITY_NORMAL = 16, PRIORITY_LOWEST = 0.
    */
    enum EPriority
    {
        PRIORITY_HIGHEST = MAX_PRIORITIES - 1, //!< Highest available priority level (MAX_PRIORITIES - 1).
        PRIORITY_NORMAL  = MAX_PRIORITIES / 2, //!< Mid-range priority level (MAX_PRIORITIES / 2).
        PRIORITY_LOWEST  = 0                   //!< Lowest priority level. Tasks at this level only run when no higher-priority task is runnable.
    };

    /*! \brief Construct an empty strategy with all priority levels empty, a clear bitmap, and null cursors.
        \note  A compile-time assertion enforces MAX_PRIORITIES <= 32, matching the width of the
               32-bit \c m_ready_bitmap. Instantiating with MAX_PRIORITIES > 32 is a compile error.
    */
    explicit SwitchStrategyFixedPriority() : m_tasks(), m_sleep(), m_ready_bitmap(0U), m_prev()
    {
        STK_STATIC_ASSERT_DESC(MAX_PRIORITIES <= 32U,
            "MAX_PRIORITIES exceeds 32-bit bitmap width");
    }

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~SwitchStrategyFixedPriority()
    {}

    /*! \brief     Add task to the runnable set at its fixed priority level.
        \param[in] task: Task to add. Must not be \c NULL and must not already be in any list.
        \note      Priority is extracted by casting \c GetWeight() to \c uint8_t. The result must
                   be less than MAX_PRIORITIES (asserted); passing a task whose GetWeight() is
                   out of range is a programming error.
        \note      Delegates to AddActive() which appends the task to \c m_tasks[prio], sets
                   \c m_ready_bitmap bit \c prio if this is the first task at that level, and
                   initialises \c m_prev[prio].
        \note      Tail-cursor invariant (same as RR): if \c m_prev[prio] was already pointing at
                   the tail before insertion, it is advanced to the new tail so GetNext() will
                   include the new task in the very next rotation at this priority level.
    */
    void AddTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == nullptr);
        STK_ASSERT(GetTaskPriority(task) < MAX_PRIORITIES);

        const Priority prio = GetTaskPriority(task);

        bool is_tail = (m_prev[prio] == m_tasks[prio].GetLast());

        AddActive(task);

        // if pointer was pointing to the tail, become a tail
        if (is_tail)
            m_prev[prio] = task;
    }

    /*! \brief     Remove task from whichever list it currently occupies.
        \param[in] task: Task to remove. Must not be \c NULL and must belong to either
                   \c m_tasks[priority] or \c m_sleep (asserted).
        \note      Dispatch checks \c m_sleep first (reversed from the RR/SWRR pattern).
                   If sleeping, simply unlinks from \c m_sleep. If runnable, delegates to
                   RemoveActive() which also updates the cursor and clears the bitmap bit
                   if the priority level becomes empty.
    */
    void RemoveTask(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0U);
        STK_ASSERT((task->GetHead() == &m_tasks[GetTaskPriority(task)]) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_sleep)
            m_sleep.Unlink(task);
        else
            RemoveActive(task);
    }

    /*! \brief     Select and return the next task to run.
        \return    The next task in the highest runnable priority level's round-robin rotation,
                   or \c nullptr if no runnable tasks exist (\c m_ready_bitmap == 0, kernel sleeps).
        \note      Priority selection is O(1) via GetHighestReadyPriority() on \c m_ready_bitmap.
                   Within the selected level the cursor (\c m_prev[prio]) is advanced by one
                   position in the closed-loop list, implementing round-robin within the level.
        \note      Tasks at a lower priority are never returned while any higher-priority task
                   has its bitmap bit set — preemption is enforced entirely by the bitmap lookup.
    */
    IKernelTask *GetNext()
    {
        if (m_ready_bitmap == 0U)
            return nullptr; // idle

        const Priority prio = GetHighestReadyPriority(m_ready_bitmap);

        IKernelTask *ret = (*m_prev[prio]->GetNext());
        m_prev[prio] = ret;

        return ret;
    }

    /*! \brief     Get the first task in the managed set (used by the kernel for initial scheduling).
        \return    The first task in the highest runnable priority level if any task is runnable
                   (bitmap non-zero); otherwise the first task in \c m_sleep.
                   Asserts if the combined set is empty (GetSize() == 0).
        \note      Uses \c m_ready_bitmap to determine whether any runnable task exists, consistent
                   with GetNext(). The sleep fallback allows the kernel to identify any task even
                   when all are currently sleeping.
    */
    IKernelTask *GetFirst() const
    {
        STK_ASSERT(GetSize() != 0U);

        if (m_ready_bitmap == 0U)
            return (*m_sleep.GetFirst());

        const Priority prio = GetHighestReadyPriority(m_ready_bitmap);
        return (*m_tasks[prio].GetFirst());
    }

    /*! \brief  Get the total number of tasks managed by this strategy.
        \return Sum of tasks across all priority levels in \c m_tasks plus tasks in \c m_sleep.
        \note   O(MAX_PRIORITIES): iterates all priority levels to accumulate the count.
                Unlike the RR and SWRR strategies this is not O(1).
    */
    size_t GetSize() const
    {
        size_t total = m_sleep.GetSize();
        for (Priority i = 0U; i < MAX_PRIORITIES; i += 1U)
            total += m_tasks[i].GetSize();

        return total;
    }

    /*! \brief     Notification that a task has entered the sleeping state.
        \param[in] task: The task that is now sleeping. Must be in \c m_tasks[priority] (asserted).
        \note      Delegates to RemoveActive() which unlinks the task from its priority level list,
                   updates the per-level cursor, and clears bit \c prio in \c m_ready_bitmap if this
                   was the last runnable task at that level. Then appends the task to \c m_sleep.
    */
    void OnTaskSleep(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_tasks[GetTaskPriority(task)]);

        RemoveActive(task);
        m_sleep.LinkBack(task);
    }

    /*! \brief     Notification that a task has become runnable again.
        \param[in] task: The task that woke up. Must be in \c m_sleep (asserted).
        \note      Unlinks the task from \c m_sleep and delegates to AddActive() which appends
                   it to \c m_tasks[priority] and sets the bitmap bit if the level was empty.
        \note      Unlike SwitchStrategySmoothWeightedRoundRobin, no priority boost is applied.
                   The waking task re-enters its natural priority level and waits its turn in
                   the round-robin rotation at that level. Its preemption guarantee is provided
                   solely by its fixed priority relative to other runnable levels.
    */
    void OnTaskWake(IKernelTask *task)
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(!task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_sleep);

        m_sleep.Unlink(task);
        AddActive(task);
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
    //! Priority type.
    typedef uint8_t Priority;

    /*! \brief     Append a task to its priority level's runnable list and update the bitmap.
        \param[in] task: Task to make runnable.
        \note      Appends to the back of \c m_tasks[prio].
        \note      Bitmap and cursor are only updated when the level was previously empty
                   (GetSize() == 1 after LinkBack): \c m_ready_bitmap bit \c prio is set and
                   \c m_prev[prio] is initialised to \c task. Subsequent additions at the same
                   level leave the cursor untouched (the tail-adjustment in AddTask() handles
                   the cursor for non-first additions).
    */
    void AddActive(IKernelTask *task)
    {
        const Priority prio = GetTaskPriority(task);

        m_tasks[prio].LinkBack(task);

        // init pointer
        if (m_tasks[prio].GetSize() == 1U)
        {
            m_prev[prio] = task;

            m_ready_bitmap |= (1U << prio);
        }
    }

    /*! \brief     Remove a task from its priority level's runnable list and update the bitmap/cursor.
        \param[in] task: Runnable task to remove.
        \note      Cursor update algorithm (same as SwitchStrategyRoundRobin::RemoveActive, applied
                   per priority level):
                   - Capture \c next = task->GetNext() \e before unlinking.
                   - If \c next != \c task (other tasks remain at this level): set
                     \c m_prev[prio] = next->GetPrev() so GetNext() returns \c next
                     without skipping it.
                   - If \c next == \c task (last task at this level): set \c m_prev[prio] = NULL
                     and clear bit \c prio in \c m_ready_bitmap. GetNext() will then select the
                     next highest set bit, falling through to a lower priority level.
    */
    void RemoveActive(IKernelTask *task)
    {
        const Priority prio = GetTaskPriority(task);
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
            m_ready_bitmap &= ~(1U << prio);
        }
    }

    /*! \brief     Get priority from the task..
        \param[in] task: Pointer to the task. Priority is read from \c GetWeight().
        \return    Priority level.
    */
    static __stk_forceinline Priority GetTaskPriority(IKernelTask *task)
    {
        return static_cast<Priority>(task->GetWeight());
    }

    /*! \brief     Find the index of the highest set bit in \a bitmap.
        \param[in] bitmap: Bitmask of ready priority levels. Must not be 0 (behaviour is
                   undefined for \c __builtin_clz(0); callers must check before calling).
        \return    Index of the most-significant set bit (0-based), which is the highest
                   runnable priority level.
        \note      On GCC/Clang: uses \c __builtin_clz for an O(1) hardware instruction
                   (BSR on x86, CLZ on ARM). On other compilers: falls back to a portable
                   O(32) linear scan from bit 31 down to 0.
    */
    static __stk_forceinline Priority GetHighestReadyPriority(uint32_t bitmap)
    {
    #if defined(__GNUC__)
        return static_cast<Priority>(31U - __builtin_clz(bitmap));
    #else
        for (int8_t i = 31; i >= 0; --i)
        {
            if (bitmap & (1U << i))
                return static_cast<Priority>(i);
        }
        return 0;
    #endif
    }

private:
    STK_NONCOPYABLE_CLASS(SwitchStrategyFixedPriority);

    IKernelTask::ListHeadType m_tasks[MAX_PRIORITIES]; //!< Per-priority runnable task lists. m_tasks[i] holds all runnable tasks at priority level i.
    IKernelTask::ListHeadType m_sleep;                 //!< Sleeping (blocked) tasks. Priority is retained in GetWeight() and restored on OnTaskWake().
    uint32_t                  m_ready_bitmap;          //!< Bitmask of non-empty priority levels: bit i is set when m_tasks[i] has at least one runnable task. Updated by AddActive() (bit set) and RemoveActive() (bit cleared on last removal).
    IKernelTask              *m_prev[MAX_PRIORITIES];  //!< Per-priority round-robin cursor. m_prev[i] is the most recently scheduled task at level i, or \c nullptr when level i is empty. GetNext() advances from this position.
};

/*! \typedef SwitchStrategyFP32
    \brief   Shorthand alias for SwitchStrategyFixedPriority<32>: 32 priority levels (0..31),
             using a single 32-bit \c m_ready_bitmap for O(1) highest-priority lookup.
    \see     SwitchStrategyFixedPriority
*/
typedef SwitchStrategyFixedPriority<32> SwitchStrategyFP32;

} // namespace stk

#endif /* STK_STRATEGY_FPRIORITY_H_ */
