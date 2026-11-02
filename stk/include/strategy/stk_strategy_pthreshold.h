/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_STRATEGY_PTHRESHOLD_H_
#define STK_STRATEGY_PTHRESHOLD_H_

/*! \file  stk_strategy_pthreshold.h
    \brief Fixed-priority preemptive strategy with ThreadX-style Preemption-Threshold and optional
           per-task time-slicing (stk::sched::SwitchStrategyPreemptionThreshold / stk::sched::SwitchStrategyPT32 /
           stk::sched::SwitchStrategyPT32_TS / stk::sched::SwitchStrategyPT64 / stk::sched::SwitchStrategyPT64_TS).
*/

#include "stk_common.h"
#include "stk_priority_bitmap.h"

namespace stk {
namespace sched {

/*! \class SwitchStrategyPreemptionThreshold
    \brief Fixed-priority scheduling with Preemption-Threshold (PTS), modelled on Azure RTOS /
           ThreadX. Each task has a priority \c P and a threshold \c T with \c T >= \c P.
           While a task is running it can be preempted only by tasks whose priority is
           strictly greater than \c T. With \c T == \c P the task behaves like a normal
           fixed-priority task.

    \tparam MAX_PRIORITIES  Number of priority levels, [1, PriorityBitmapTraits<ReadyBitmap>::CAPACITY]
                           (enforced by a compile-time assertion), at most 32768 (priority and threshold
                           are stored as \c uint16_t, but the packed weight is a signed \c int32_t so the
                           priority is limited to [0, 32767]). The aliases SwitchStrategyPT32 and
                           SwitchStrategyPT64 use 32 and 64 levels respectively.
    \tparam TIMESLICE       \c true to enable time-slicing with per-task quanta for use with
                           stk::KERNEL_TIMESLICE: GetNext() becomes sticky (the selected task of a
                           level keeps running) and the level is rotated only by OnTaskTimeSlice(),
                           OnTaskYield() or when the running task leaves the runnable set.
                           \c false (default): fixed 1-tick round-robin on every GetNext().
    \tparam ReadyBitmap    Type of the priority bitmaps (ready, held and advance). Defaults to \c uint32_t
                           for MAX_PRIORITIES <= 32, to \c uint64_t for <= 64 and to PriorityBitmap<> (multi-word,
                           up to 1024 bits) above that. Can be any unsigned integer type, PriorityBitmap<BITS>,
                           or a user-defined class for which a PriorityBitmapTraits specialization is
                           provided (see stk_priority_bitmap.h).

    \par Priority convention
    STK convention is used: \b higher number = higher priority. ThreadX uses the opposite
    (0 = highest). Use FromThreadX() to convert ThreadX (priority, threshold) pairs.

    \par Encoding in ITask::GetWeight()
    Both values are packed into the task weight: <tt>(priority << 16) | threshold</tt>
    (16 bits each). Weight is \c int32_t, so the priority must stay below 32768
    to keep the weight non-negative and correctly ordered.
    Use MakeWeight() / FromThreadX() inside ITask::GetWeight(). Priority occupies the high
    bits so integer comparison of weights (as used by kernel priority inheritance) orders
    by priority first.

    \par Scheduling rules
    - A task is \e held when it is selected while \c T > \c P. The hold lasts until the task
      sleeps, is removed, yields (see OnTaskYield()), or its threshold is lowered to its
      priority; if its priority changes (priority inheritance) the hold moves to the new level
      when that level's hold slot is free. A held task that is preempted by a
      task above its threshold remains held and is resumed as soon as no ready task is
      above its threshold (this mirrors ThreadX's behaviour where a preempted
      threshold-thread is resumed before ready tasks inside its protected band).
    - A held task is never time-sliced (ThreadX disables time-slicing for threads that use
      preemption-threshold): tasks of its own priority wait until it sleeps or yields.
    - Otherwise the highest ready priority level is served in round-robin: one tick each without
      TIMESLICE, or, with TIMESLICE, the running task keeps the CPU until its quantum
      (ITask::GetTimeSlice()) expires, it yields or it leaves the runnable set. Time-slicing only
      rotates tasks of the same priority, a lower priority level is never served instead.

    \note  Uses WEIGHT_API (weight = encoded priority/threshold), SLEEP_EVENT_API,
           PRIORITY_INHERITANCE_API and NOSLEEP_YIELD_API, plus TIMESLICE_API if \a TIMESLICE
           is \c true.
    \note  Requires kernel support: IKernelTask::GetBaseWeight() and IKernelService::SetWeight()
           (run-time threshold change, see ChangeThreshold()) and the OnTaskYield() hook. With
           \a TIMESLICE the kernel must be built with stk::KERNEL_TIMESLICE, it charges the
           running task and calls OnTaskTimeSlice() when the quantum expires.
    \note  PRIORITY_INHERITANCE_API = 1: an inherited weight contributes only its priority
           (the waiter's threshold is ignored). The effective threshold of the owner is
           max(configured threshold, current priority), the configured threshold being read
           via IKernelTask::GetBaseWeight().
    \note  Yield() is handled by OnTaskYield() (tx_thread_relinquish semantics) in non-HRT modes.
    \note  Differences from ThreadX: STK priority numbering (higher = more important), at most
           32768 priority levels (bounded by the ReadyBitmap capacity); without TIMESLICE the slice is fixed at 1 tick, with TIMESLICE
           (stk::KERNEL_TIMESLICE) the per-task quantum comes from ITask::GetTimeSlice().
    \note  With \a TIMESLICE, the quantum of a task can be changed at run-time with
           IKernelService::SetTimeSlice(). A task whose threshold is above its priority is held
           while it runs and is not time-sliced, use \c MakeWeight(p, p) for tasks which must be.
    \see   SwitchStrategyPT32, SwitchStrategyPT32_TS, SwitchStrategyPT64, SwitchStrategyPT64_TS,
           SwitchStrategyFixedPriority, PriorityBitmapTraits
*/
template <uint16_t MAX_PRIORITIES, bool TIMESLICE = false,
          class ReadyBitmap = typename ReadyBitmapSelect<MAX_PRIORITIES>::Type>
class SwitchStrategyPreemptionThreshold final : public ITaskSwitchStrategy
{
public:
    /*! \enum  EConfig
        \brief Compile-time capability flags reported to the kernel.
    */
    enum EConfig
    {
        WEIGHT_API               = 1, //!< GetWeight() carries the encoded priority and preemption-threshold.
        SLEEP_EVENT_API          = 1, //!< Requires OnTaskSleep() / OnTaskWake() to maintain per-priority lists.
        DEADLINE_MISSED_API      = 0, //!< Not used.
        PRIORITY_INHERITANCE_API = 1, //!< OnTaskWeightChange() events are handled.
        NOSLEEP_YIELD_API        = 1, //!< OnTaskYield() events are handled.
        TIMESLICE_API            = (TIMESLICE ? 1 : 0) //!< Sticky GetNext() and OnTaskTimeSlice() (see TIMESLICE template parameter).
    };

    /*! \brief     Build a task weight from an STK priority and threshold.
        \param[in] priority:  Task priority, [0, MAX_PRIORITIES - 1], higher = more important.
        \param[in] threshold: Preemption-threshold, [priority, MAX_PRIORITIES - 1].
    */
    static constexpr Weight MakeWeight(uint16_t priority, uint16_t threshold)
    {
        return static_cast<Weight>((static_cast<uint32_t>(priority) << 16) | static_cast<uint32_t>(threshold));
    }

    /*! \brief     Build a task weight from ThreadX numbers (0 = highest priority).
        \param[in] tx_priority:  ThreadX priority, [0, MAX_PRIORITIES - 1].
        \param[in] tx_threshold: ThreadX preemption-threshold, [0, tx_priority].
    */
    static constexpr Weight FromThreadX(uint16_t tx_priority, uint16_t tx_threshold)
    {
        return MakeWeight(static_cast<uint16_t>((MAX_PRIORITIES - 1U) - tx_priority),
                          static_cast<uint16_t>((MAX_PRIORITIES - 1U) - tx_threshold));
    }

    /*! \brief Construct an empty strategy.
        \note  A compile-time assertion enforces 1 <= MAX_PRIORITIES <= BitmapTraits::CAPACITY, i.e. the number
               of bits the \c ReadyBitmap type can hold.
        \note  The bitmaps are value-initialized, so a user-defined \c ReadyBitmap must be default-constructible
               into the empty state.
    */
    explicit SwitchStrategyPreemptionThreshold() : m_tasks(), m_sleep(), m_ready_bitmap(), m_held_bitmap(), m_advance_bitmap(), m_running(nullptr), m_prev(), m_held()
    {
        STK_STATIC_ASSERT_DESC_N(MinPrio, MAX_PRIORITIES >= 1U, "MAX_PRIORITIES must be at least 1");
        STK_STATIC_ASSERT_DESC_N(MaxPrio, MAX_PRIORITIES <= static_cast<uint32_t>(BitmapTraits::CAPACITY), "MAX_PRIORITIES exceeds ReadyBitmap width");
        STK_STATIC_ASSERT_DESC_N(MaxIdx, MAX_PRIORITIES <= 32768U, "MAX_PRIORITIES exceeds 32768 (priority is encoded in the upper 15 bits of the int32_t Weight)");
    }

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~SwitchStrategyPreemptionThreshold() = default;

    /*! \brief     Add task to the runnable set at its priority level.
        \param[in] task: Task to add. Its weight must be valid (priority < MAX_PRIORITIES,
                   priority <= threshold < MAX_PRIORITIES).
    */
    void AddTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->GetHead() == nullptr);
        STK_ASSERT(GetTaskPriority(task) < MAX_PRIORITIES);
        STK_ASSERT(GetRawThreshold(task) < MAX_PRIORITIES);
        STK_ASSERT(GetRawThreshold(task) >= GetTaskPriority(task));

        const Priority prio = GetTaskPriority(task);
        const bool is_tail = (m_prev[prio] == m_tasks[prio].GetLast());

        AddActive(task);

        // non-sticky mode: if the cursor is on the tail, move it to the new task so that the next
        // GetNext() returns the head instead of the new task (the new task waits for its turn),
        // sticky mode (TIMESLICE): the cursor stays on the running task and the new task is
        // appended behind it, the CPU is not taken over until the rotation is requested
        if (is_tail && !TIMESLICE)
        {
            m_prev[prio] = task;
        }
    }

    /*! \brief     Remove task from whichever list it occupies (drops its hold if any).
    */
    void RemoveTask(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(GetSize() != 0U);
        STK_ASSERT((task->GetHead() == &m_tasks[GetTaskPriority(task)]) || (task->GetHead() == &m_sleep));

        if (task->GetHead() == &m_sleep)
        {
            m_sleep.Unlink(task);
        }
        else
        {
            RemoveActive(task, GetTaskPriority(task));
        }
    }

    /*! \brief     Select the next task to run.
        \return    Task to run, or \c nullptr if nothing is runnable.
        \note      1) Find the highest ready priority \c hi.
                   2) If a task is held (the highest-priority holder \c H) and
                      \c hi <= threshold(H), H keeps/resumes the CPU.
                   3) Otherwise round-robin inside level \c hi; if the chosen task has
                      threshold > priority it becomes held.
        \note      In round-robin mode (TIMESLICE = \c false) the cursor of the level advances on
                   every call. In sticky mode (TIMESLICE = \c true) the cursor advances only if a
                   rotation was requested for the level (OnTaskYield(), OnTaskTimeSlice() or the
                   removal of the task the cursor was on), otherwise the same task is returned.
    */
    IKernelTask *GetNext() override
    {
        IKernelTask *next = nullptr;

        if (!BitmapTraits::IsEmpty(m_ready_bitmap))
        {
            const Priority hi = GetHighestReadyPriority(m_ready_bitmap);

            if (!BitmapTraits::IsEmpty(m_held_bitmap))
            {
                IKernelTask *const holder = m_held[GetHighestReadyPriority(m_held_bitmap)];
                STK_ASSERT(holder != nullptr);

                if (hi <= GetTaskThreshold(holder))
                {
                    next = holder;
                }
            }

            if (next == nullptr)
            {
                bool advance = true;

                if __stk_constexpr_cpp17 (TIMESLICE)
                {
                    // sticky: the cursor moves only when a rotation was requested for this level
                    advance = BitmapTraits::IsSet(m_advance_bitmap, hi);
                    BitmapTraits::Clear(m_advance_bitmap, hi);
                }

                if (advance)
                {
                    next = (*m_prev[hi]->GetNext());
                    m_prev[hi] = next;
                }
                else
                {
                    next = m_prev[hi];
                }

                if (GetTaskThreshold(next) > hi)
                {
                    m_held[hi] = next;
                    BitmapTraits::Set(m_held_bitmap, hi);
                }
            }
        }

        m_running = next;

        return next;
    }

    /*! \brief     First task in the managed set (highest ready level, else first sleeping).
    */
    IKernelTask *GetFirst() override
    {
        STK_ASSERT(GetSize() != 0U);

        IKernelTask *first_task = nullptr;

        if (BitmapTraits::IsEmpty(m_ready_bitmap))
        {
            first_task = (*m_sleep.GetFirst());
        }
        else
        {
            first_task = (*m_tasks[GetHighestReadyPriority(m_ready_bitmap)].GetFirst());
        }

        return first_task;
    }

    /*! \brief  Total number of tasks (runnable + sleeping).
    */
    size_t GetSize() const override
    {
        size_t total = m_sleep.GetSize();
        for (uint16_t i = 0U; i < MAX_PRIORITIES; ++i)
        {
            total += m_tasks[i].GetSize();
        }

        return total;
    }

    /*! \brief     Task went to sleep: leaves the runnable set and releases its hold.
        \note      In sticky mode (TIMESLICE) if the task was the one the level cursor was on, the
                   next peer is selected by the following GetNext().
    */
    void OnTaskSleep(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_tasks[GetTaskPriority(task)]);

        RemoveActive(task, GetTaskPriority(task));
        m_sleep.LinkBack(task);
    }

    /*! \brief     Task became runnable again; re-enters its priority level (not held).
        \note      It is appended behind the other tasks of its level, in sticky mode (TIMESLICE)
                   it does not take the CPU from the running peer before that task's rotation.
    */
    void OnTaskWake(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);
        STK_ASSERT(!task->IsSleeping());
        STK_ASSERT(task->GetHead() == &m_sleep);

        m_sleep.Unlink(task);
        AddActive(task);
    }

    /*! \brief     Task weight changed (priority inheritance / restore): move to the new level.
        \note      A hold is migrated to the new level if the effective threshold is still above
                   the new priority and the level's hold slot is free; otherwise it is released.
    */
    void OnTaskWeightChange(IKernelTask *task, Weight old_weight) override
    {
        const Priority old_prio = DecodePriority(old_weight);
        const Priority new_prio = GetTaskPriority(task);

        STK_ASSERT(new_prio < MAX_PRIORITIES);
        STK_ASSERT(old_prio < MAX_PRIORITIES);

        if (task->GetHead() != &m_sleep)
        {
            STK_ASSERT(task->GetHead() == &m_tasks[old_prio]);

            if (old_prio == new_prio)
            {
                // threshold-only change (IKernelService::SetWeight): no relinking needed
                UpdateHold(task, new_prio);
            }
            else
            {
                const bool was_held = (m_held[old_prio] == task);

                RemoveActive(task, old_prio);
                AddActive(task);

                // migrate the hold to the new level if the task still has a protected band
                // and the level's (single) hold slot is free
                if (was_held && (GetTaskThreshold(task) > new_prio) && (m_held[new_prio] == nullptr))
                {
                    m_held[new_prio] = task;
                    BitmapTraits::Set(m_held_bitmap, new_prio);
                }
            }
        }
    }

    /*! \brief     Yield: hand the CPU to the next task of the same priority.
        \param[in] task: The yielding (running) task.
        \return    \c true: the task stays runnable and is not put to sleep by the kernel;
                   \c false if the task is not in a runnable list (kernel falls back to a sleep-yield).
        \note      The cursor of the task's level is set to the task so that GetNext() returns its
                   next peer (or the task itself if it has none: it simply continues). The
                   highest ready level does not change, therefore no lower-priority task runs.
        \note      The yielder's preemption-threshold hold is released so peers are not blocked by it.
                   Ready tasks above its priority but within its former threshold are no longer
                   blocked either and may run.
        \note      In sticky mode (TIMESLICE) a rotation of the level is requested explicitly, the
                   kernel also restarts the time slice of the yielding task (tx_thread_relinquish
                   semantics).
    */
    bool OnTaskYield(IKernelTask *task) override
    {
        STK_ASSERT(task != nullptr);

        bool handled = false;
        const Priority prio = GetTaskPriority(task);

        if ((prio < MAX_PRIORITIES) && (task->GetHead() == &m_tasks[prio]))
        {
            if (m_held[prio] == task)
            {
                m_held[prio] = nullptr;
                BitmapTraits::Clear(m_held_bitmap, prio);
            }

            m_prev[prio] = task;
            handled      = true;

            if __stk_constexpr_cpp17 (TIMESLICE)
            {
                BitmapTraits::Set(m_advance_bitmap, prio);
            }
        }

        return handled;
    }

    /*! \brief     Time slice of the running task expired: rotate to the next task of the same priority.
        \param[in] task: The running task.
        \return    \c true if the level was rotated; \c false if the task has no same-priority peer,
                   is held by its preemption-threshold (held tasks are never time-sliced, as in
                   ThreadX), is not in a runnable list, or the strategy is not a TIMESLICE one.
        \note      A held task is never rotated, so unlike OnTaskYield() its hold is never released
                   here. When \c false is returned the task simply keeps running, the kernel has
                   already reloaded its slice.
    */
    bool OnTaskTimeSlice(IKernelTask *task) override
    {
        bool rotated = false;

        if __stk_constexpr_cpp17 (TIMESLICE)
        {
            STK_ASSERT(task != nullptr);

            const Priority prio = GetTaskPriority(task);

            // rotation requires a same-priority peer: a lone task simply continues (with a fresh slice
            // reloaded by the kernel), no rotation request is left pending for the level
            if ((prio < MAX_PRIORITIES) && (task->GetHead() == &m_tasks[prio]) && (m_held[prio] != task) &&
                (m_tasks[prio].GetSize() > 1U))
            {
                m_prev[prio] = task;
                rotated      = true;

                BitmapTraits::Set(m_advance_bitmap, prio);
            }
        }

        return rotated;
    }

    /*! \brief     Change the preemption-threshold of a task at run-time (tx_thread_preemption_change).
        \param[in] service: Kernel service (IKernelService::GetInstance()).
        \param[in] tid: Task id (IKernelService::GetTid() for the calling task).
        \param[in] priority: The task's configured STK priority (unchanged by this call).
        \param[in] threshold: New threshold, [priority, MAX_PRIORITIES - 1].
        \return    Previous threshold.
        \note      If the task is the one currently running, a raised threshold protects it
                   immediately; a threshold equal to the priority releases the protection.
                   For a task that is not running the new threshold applies from its next
                   selection. The task's time slice is not affected, but with TIMESLICE a task
                   which is held (threshold above its priority) is not time-sliced while it holds.
    */
    static uint16_t ChangeThreshold(IKernelService &service, TId tid, uint16_t priority, uint16_t threshold)
    {
        STK_ASSERT(priority < MAX_PRIORITIES);
        STK_ASSERT((threshold >= priority) && (threshold < MAX_PRIORITIES));

        return DecodeThreshold(service.SetWeight(tid, MakeWeight(priority, threshold)));
    }

protected:
    //! Priority type.
    typedef uint16_t Priority;

    //! Bitmap operations (set/clear/test/empty/highest-set) for the \c ReadyBitmap type.
    typedef PriorityBitmapTraits<ReadyBitmap> BitmapTraits;

    void AddActive(IKernelTask *task)
    {
        const Priority prio = GetTaskPriority(task);

        m_tasks[prio].LinkBack(task);

        if (m_tasks[prio].GetSize() == 1U)
        {
            m_prev[prio] = task;
            BitmapTraits::Set(m_ready_bitmap, prio);
        }
    }

    void RemoveActive(IKernelTask *task, const Priority prio)
    {
        IKernelTask *const next = (*task->GetNext());

        m_tasks[prio].Unlink(task);

        if (m_running == task)
        {
            m_running = nullptr;
        }

        // release preemption-threshold hold
        if (m_held[prio] == task)
        {
            m_held[prio] = nullptr;
            BitmapTraits::Clear(m_held_bitmap, prio);
        }

        if (next == task)
        {
            m_prev[prio] = nullptr;
            BitmapTraits::Clear(m_ready_bitmap, prio);

            if __stk_constexpr_cpp17 (TIMESLICE)
            {
                BitmapTraits::Clear(m_advance_bitmap, prio);
            }
        }
        else if (m_prev[prio] == task)
        {
            // cursor was on the removed task: step back so GetNext() returns its successor;
            // otherwise leave the rotation untouched
            m_prev[prio] = (*next->GetPrev());

            if __stk_constexpr_cpp17 (TIMESLICE)
            {
                // sticky mode: the successor must run next
                BitmapTraits::Set(m_advance_bitmap, prio);
            }
        }
    }

    /*! \brief Apply a threshold change to a runnable task that stays on the same level.
        \note  Raised threshold on the running task: it becomes held now. Threshold not above the
               priority: any hold is released. Other tasks pick the hold up when next selected.
    */
    void UpdateHold(IKernelTask *task, const Priority prio)
    {
        if (GetTaskThreshold(task) > prio)
        {
            if ((task == m_running) && (m_held[prio] == nullptr))
            {
                m_held[prio] = task;
                BitmapTraits::Set(m_held_bitmap, prio);
            }
        }
        else if (m_held[prio] == task)
        {
            m_held[prio] = nullptr;
            BitmapTraits::Clear(m_held_bitmap, prio);
        }
        else
        {
            // nothing to update
        }
    }

    static __stk_forceinline Priority DecodePriority(Weight weight)
    {
        return static_cast<Priority>((static_cast<uint32_t>(weight) >> 16) & 0xFFFFU);
    }

    static __stk_forceinline Priority GetTaskPriority(IKernelTask *task)
    {
        return DecodePriority(task->GetWeight());
    }

    static __stk_forceinline Priority DecodeThreshold(Weight weight)
    {
        return static_cast<Priority>(static_cast<uint32_t>(weight) & 0xFFFFU);
    }

    //! Configured threshold from the task's base weight (unaffected by priority inheritance).
    static __stk_forceinline Priority GetRawThreshold(IKernelTask *task)
    {
        return DecodeThreshold(task->GetBaseWeight());
    }

    /*! Effective threshold = max(configured threshold, current priority). While the task
        inherits a higher priority its protected band is raised to at least that priority
        but never loses the configured threshold. */
    static __stk_forceinline Priority GetTaskThreshold(IKernelTask *task)
    {
        const Priority prio = GetTaskPriority(task);
        const Priority thr  = GetRawThreshold(task);

        return (thr > prio ? thr : prio);
    }

    //! Index of the highest set bit of \a bitmap (must not be empty), i.e. the highest ready priority.
    static __stk_forceinline Priority GetHighestReadyPriority(const ReadyBitmap &bitmap)
    {
        return static_cast<Priority>(BitmapTraits::HighestSet(bitmap));
    }

private:
    STK_NONCOPYABLE_CLASS(SwitchStrategyPreemptionThreshold);

    IKernelTask::ListHeadType m_tasks[MAX_PRIORITIES]; //!< Runnable tasks per priority level.
    IKernelTask::ListHeadType m_sleep;                 //!< Sleeping tasks.
    ReadyBitmap               m_ready_bitmap;          //!< Bit i set when m_tasks[i] is non-empty.
    ReadyBitmap               m_held_bitmap;           //!< Bit i set when m_held[i] is a task holding its preemption-threshold.
    ReadyBitmap               m_advance_bitmap;        //!< Bit i set: next GetNext() at level i advances the cursor (TIMESLICE only).
    IKernelTask              *m_running;               //!< Task returned by the last GetNext() (nullptr if none or it left the runnable set).
    IKernelTask              *m_prev[MAX_PRIORITIES];  //!< Per-level round-robin cursor: last task returned by GetNext() (sticky mode: the selected task, kept until a rotation is requested).
    IKernelTask              *m_held[MAX_PRIORITIES];  //!< Per-level task currently holding its threshold (at most one per level, as in ThreadX).
};

/*! \typedef SwitchStrategyPT32
    \brief   SwitchStrategyPreemptionThreshold with 32 priority levels.
*/
typedef SwitchStrategyPreemptionThreshold<32> SwitchStrategyPT32;

/*! \typedef SwitchStrategyPT32_TS
    \brief   SwitchStrategyPreemptionThreshold with 32 priority levels and per-task time-slicing,
             for use with stk::KERNEL_TIMESLICE.
*/
typedef SwitchStrategyPreemptionThreshold<32, true> SwitchStrategyPT32_TS;

/*! \typedef SwitchStrategyPT64
    \brief   SwitchStrategyPreemptionThreshold with 64 priority levels (64-bit bitmaps).
    \note    On 32-bit targets (e.g. Cortex-M) 64-bit bitmaps cost two registers and a two-step scan;
             prefer SwitchStrategyPT32 unless more than 32 levels are really needed.
*/
typedef SwitchStrategyPreemptionThreshold<64> SwitchStrategyPT64;

/*! \typedef SwitchStrategyPT64_TS
    \brief   SwitchStrategyPreemptionThreshold with 64 priority levels (64-bit bitmaps) and per-task
             time-slicing, for use with stk::KERNEL_TIMESLICE.
*/
typedef SwitchStrategyPreemptionThreshold<64, true> SwitchStrategyPT64_TS;

} // namespace sched

// ----------------------------------------------------------------------------
// Backward compatibility: the strategies live in stk::sched, the names below keep existing
// code that refers to them as stk::<Name> compiling. Migrate to stk::sched::<Name>.
// ----------------------------------------------------------------------------

//! \deprecated Use stk::sched::SwitchStrategyPreemptionThreshold.
template <uint16_t MAX_PRIORITIES, bool TIMESLICE = false, class ReadyBitmap = typename sched::ReadyBitmapSelect<MAX_PRIORITIES>::Type>
using SwitchStrategyPreemptionThreshold __stk_attr_deprecated_type("Use stk::sched::SwitchStrategyPreemptionThreshold") = sched::SwitchStrategyPreemptionThreshold<MAX_PRIORITIES, TIMESLICE, ReadyBitmap>;

//! \deprecated Use stk::sched::SwitchStrategyPT32.
typedef sched::SwitchStrategyPT32 SwitchStrategyPT32 __stk_attr_deprecated_type("Use stk::sched::SwitchStrategyPT32");
//! \deprecated Use stk::sched::SwitchStrategyPT32_TS.
typedef sched::SwitchStrategyPT32_TS SwitchStrategyPT32_TS __stk_attr_deprecated_type("Use stk::sched::SwitchStrategyPT32_TS");
//! \deprecated Use stk::sched::SwitchStrategyPT64.
typedef sched::SwitchStrategyPT64 SwitchStrategyPT64 __stk_attr_deprecated_type("Use stk::sched::SwitchStrategyPT64");
//! \deprecated Use stk::sched::SwitchStrategyPT64_TS.
typedef sched::SwitchStrategyPT64_TS SwitchStrategyPT64_TS __stk_attr_deprecated_type("Use stk::sched::SwitchStrategyPT64_TS");

} // namespace stk

#endif /* STK_STRATEGY_PTHRESHOLD_H_ */
