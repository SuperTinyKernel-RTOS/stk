/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_HELPER_H_
#define STK_HELPER_H_

#include "stk_common.h"
#include "stk_arch.h"

/*! \file  stk_helper.h
    \brief Contains helper implementations which simplify user-side code.
*/

namespace stk {

/*! \class Task
    \brief Partial implementation of the user task.

    Provides stack storage and default implementations of all optional ITask methods.
    Inherit from this class and implement GetFunc() and GetFuncUserData() to make a
    schedulable task. Use ACCESS_USER for unprivileged tasks and ACCESS_PRIVILEGED
    for tasks requiring full hardware access.

    Usage example:
    \code
    template <stk::EAccessMode _AccessMode>
    class MyTask : public stk::Task<256, _AccessMode>
    {
    private:
        void Run()
        {
            while (true)
            {
                // do some work here ...
            }
        }
    };

    MyTask<ACCESS_PRIVILEGED> my_task;
    \endcode
*/
template <size_t _StackSize, EAccessMode _AccessMode>
class Task : public ITask
{
public:
    enum { STACK_SIZE = _StackSize }; //!< Stack size in elements of Word, mirrors the _StackSize template parameter.

    const Word *GetStack()      const override { return const_cast<Word *>(m_stack); }
    size_t GetStackSize()       const override { return _StackSize; }
    size_t GetStackSizeBytes()  const override { return _StackSize * sizeof(Word); }
    EAccessMode GetAccessMode() const override { return _AccessMode; }

protected:
    STK_NONCOPYABLE_CLASS(Task);

    /*! \brief Initializes task instance and zero-initializes its internal stack memory.
        
        The constructor is protected to ensure that the Task class can only be 
        instantiated through a derived subclass. It handles the allocation (if applicable) 
        and zero-initialization of the \ref m_stack member based on the \a _StackSize 
        template parameter.
    */
    Task() : m_stack()
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~Task() = default;

private:
    typename StackMemoryDef<_StackSize>::Type m_stack; //!< Stack memory region, 16-byte aligned.
};

/*! \class TaskW
    \brief Partial implementation of the user task with a compile-time scheduling weight.
           Use when the kernel is configured with SwitchStrategySmoothWeightedRoundRobin.

    \tparam _Weight:     Static scheduling weight (positive, non-zero 24-bit integer).
                         Higher values cause this task to receive proportionally more CPU time.
    \tparam _StackSize:  Stack size in elements of Word.
    \tparam _AccessMode: Hardware access mode (ACCESS_USER or ACCESS_PRIVILEGED).

    \note  Hard Real-Time mode (KERNEL_HRT) is not supported for weighted tasks.
           OnDeadlineMissed() will trigger an assertion if HRT scheduling is attempted.

    See Task for full usage example and implementation guidance.
*/
template <Weight _Weight, size_t _StackSize, EAccessMode _AccessMode>
class TaskW : public ITask
{
public:
    enum { STACK_SIZE = _StackSize }; //!< Stack size in elements of Word, mirrors the _StackSize template parameter.

    const Word *GetStack()      const override { return const_cast<Word *>(m_stack); }
    size_t GetStackSize()       const override { return _StackSize; }
    size_t GetStackSizeBytes()  const override { return _StackSize * sizeof(Word); }
    EAccessMode GetAccessMode() const override { return _AccessMode; }
    Weight GetWeight()          const override { return _Weight; }

protected:
    STK_NONCOPYABLE_CLASS(TaskW);

    /*! \brief Initializes task instance and zero-initializes its internal stack memory.
        
        The constructor is protected to ensure that the Task class can only be 
        instantiated through a derived subclass. It handles the allocation (if applicable) 
        and zero-initialization of the \ref m_stack member based on the \a _StackSize 
        template parameter.
    */
    TaskW() : m_stack() {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~TaskW() = default;

private:
    typename StackMemoryDef<_StackSize>::Type m_stack; //!< Stack memory region, 16-byte aligned.
};

/*! \class StackMemoryWrapper
    \brief Adapts an externally-owned stack memory array to the IStackMemory interface.
    \note  Wrapper (Adapter) design pattern. Use when the stack memory is declared separately
           from the task object (e.g. in a linker section or shared buffer) and needs to be
           passed to the kernel via the IStackMemory interface.
    \tparam _StackSize Stack size in elements of Word. Must be >= STACK_SIZE_MIN.
*/
template <size_t _StackSize>
class StackMemoryWrapper : public IStackMemory
{
public:
    /*! \typedef MemoryType
        \brief   The concrete array type that this wrapper accepts, equivalent to StackMemoryDef<_StackSize>::Type.
    */
    typedef typename StackMemoryDef<_StackSize>::Type MemoryType;

    /*! \brief     Construct a wrapper around an existing stack memory array.
        \param[in] stack: Pointer to the externally-owned memory array. Must remain valid for the
                   lifetime of this wrapper and of any kernel task using it.
        \note      _StackSize must be >= STACK_SIZE_MIN; enforced by a compile-time assertion.
    */
    explicit StackMemoryWrapper(MemoryType *stack) : m_stack(stack)
    {
        STK_STATIC_ASSERT(_StackSize >= STACK_SIZE_MIN);
    }

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~StackMemoryWrapper() = default;

    /*! \brief Get pointer to the first element of the wrapped stack array.
    */
    const Word *GetStack() const override { return (*m_stack); }

    /*! \brief Get number of elements in the wrapped stack array.
    */
    size_t GetStackSize() const override { return _StackSize; }

    /*! \brief Get size of the wrapped stack array in bytes.
    */
    size_t GetStackSizeBytes() const override { return (_StackSize * sizeof(Word)); }

private:
    MemoryType *m_stack; //!< Pointer to the externally-owned stack memory array.
};

// Helper function for Kernel::UpdateTaskState.
template <bool TicklessMode> inline Timeout GetInitialSleepTicks();
template <> inline Timeout GetInitialSleepTicks<true>()  { return STK_TICKLESS_TICKS_MAX; }
template <> inline Timeout GetInitialSleepTicks<false>() { return 1; }

//! Implementation of ISyncObject::Tick, see \a ISyncObject. Placed here as it depends on hw namespace.
inline bool ISyncObject::Tick(Timeout elapsed_ticks)
{
    // note: ScopedCriticalSection usage
    //
    // Single-core: no critical section needed - Tick() runs inside the
    // SysTick ISR which already executes with interrupts disabled, making
    // re-entrancy impossible on the local core.
    //
    // Multi-core: critical section is required because the tick handler on
    // each core may call Tick() concurrently for the same Semaphore instance,
    // and ISyncObject::Tick() is not re-entrant.
#if (STK_ARCH_CPU_COUNT > 1)
    hw::CriticalSection::ScopedLock cs_;
#endif

    IWaitObject *itr = static_cast<IWaitObject *>(m_wait_list.GetFirst());

    while (itr != nullptr)
    {
        IWaitObject *const next = static_cast<IWaitObject *>(itr->GetNext());

        if (!itr->Tick(elapsed_ticks))
        {
            itr->Wake(true);
        }

        itr = next;
    }

    return !m_wait_list.IsEmpty();
}

//! Implementation of ISyncObject::Tick, see \a ISyncObject. Placed here as it depends on \a GetUserTaskFromTid.
inline Weight ISyncObject::FindWeightHigherThan(Weight comp) const
{
    Weight max_weight = NO_WEIGHT;
    const IWaitObject *itr = static_cast<const IWaitObject *>(m_wait_list.GetFirst());     

    while (itr != nullptr)
    {
        const Weight w = GetUserTaskFromTid(itr->GetTid())->GetWeight();
        if (w > max_weight)
        {
            max_weight = w;
        }
            
        itr = static_cast<const IWaitObject *>(itr->GetNext());
    }

    return ((max_weight > comp) ? max_weight : NO_WEIGHT);
}

//! Implementation of ITask::GetId, see \a ITask. Placed here as it depends on \a GetTidFromUserTask.
inline TId ITask::GetId() const
{
    return GetTidFromUserTask(this);
}

/*! \brief     Get task/thread Id of the calling task.
    \return    Id of the calling task/thread.
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
*/
__stk_forceinline TId GetTid()
{
    return IKernelService::GetInstance()->GetTid();
}

/*! \brief     Convert ticks to milliseconds.
    \param[in] tick_count: Tick count to convert.
    \param[in] resolution: Microseconds per tick, as returned by IKernelService::GetTickResolution().
    \return    Equivalent time in milliseconds.
    \note      ISR-safe (performs only arithmetic, no kernel calls).
*/
__stk_forceinline Time GetMsFromTicks(Ticks tick_count, uint32_t resolution)
{
    return static_cast<Time>((tick_count * static_cast<Time>(resolution)) / 1000LL);
}

/*! \brief     Convert milliseconds to ticks.
    \param[in] ms: Time in milliseconds to convert.
    \param[in] resolution: Microseconds per tick, as returned by IKernelService::GetTickResolution().
    \return    Equivalent tick count.
    \note      ISR-safe (performs only arithmetic, no kernel calls).
*/
__stk_forceinline Ticks GetTicksFromMs(Time ms, uint32_t resolution)
{
    return static_cast<Ticks>(((resolution != 0U) ? (ms * 1000LL / static_cast<Time>(resolution)) : 0LL));
}

/*! \brief     Get number of ticks elapsed since kernel start.
    \note      ISR-safe.
    \return    Ticks.
*/
__stk_forceinline Ticks GetTicks()
{
    return IKernelService::GetInstance()->GetTicks();
}

/*! \brief     Get number of microseconds in one tick.
    \note      Tick is a periodicity of the system timer expressed in microseconds.
    \note      ISR-safe.
    \return    Microseconds in one tick.
*/
__stk_forceinline uint32_t GetTickResolution()
{
    return IKernelService::GetInstance()->GetTickResolution();
}

/*! \brief     Convert milliseconds to ticks using the current kernel tick resolution.
    \param[in] ms: Time in milliseconds to convert.
    \return    Equivalent tick count.
    \note      Convenience overload that queries GetTickResolution() automatically.
               Use the two-argument form GetTicksFromMsec(ms, resolution) in ISR context.
    \warning   ISR-unsafe (internally calls GetTickResolution() which accesses the kernel service).
*/
__stk_forceinline Ticks GetTicksFromMs(Time ms)
{
    return GetTicksFromMs(ms, GetTickResolution());
}

/*! \brief     Get current time in milliseconds since kernel start.
    \return    Milliseconds elapsed since IKernel::Start() was called.
    \note      ISR-safe.
    \note      When the tick resolution is exactly 1000 µs (1 ms, the default PERIODICITY_DEFAULT),
               the tick count is returned directly without multiplication, avoiding a 64-bit multiply.
*/
static inline Time GetTimeNowMs()
{
    const IKernelService *const service = IKernelService::GetInstance();
    const uint32_t resolution = service->GetTickResolution();
    const Ticks tick_count = service->GetTicks();

    return ((resolution == 1000U) ? tick_count : 
        ((tick_count * static_cast<Ticks>(resolution)) / static_cast<Ticks>(1000)));
}

/*! \brief     Get system timer count value.
    \note      ISR-safe.
    \return    64-bit count value.
*/
__stk_forceinline Cycles GetSysTimerCount()
{
    return IKernelService::GetInstance()->GetSysTimerCount();
}

/*! \brief     Get system timer frequency.
    \note      ISR-safe.
    \return    Frequency (Hz).
*/
__stk_forceinline uint32_t GetSysTimerFrequency()
{
    return IKernelService::GetInstance()->GetSysTimerFrequency();
}

/*! \brief     Put calling process into a sleep state.
    \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
    \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
    \param[in] tick_count: Sleep time (in ticks). 0 does not cause yield, use Yield instead. Negative will cause an assertion.
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
*/
__stk_forceinline void Sleep(Timeout tick_count)
{
    IKernelService::GetInstance()->Sleep(tick_count);
}

/*! \brief     Put calling process into a sleep state.
    \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
    \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
    \note      Converts ms to ticks and calls IKernelService::SleepTicks() which schedules the calling
               task to sleep and spins until the kernel switches it back in.
    \param[in] ms: Sleep time (milliseconds). 0 does not cause yield, use Yield instead. Negative will cause an assertion.
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
*/
static inline void SleepMs(Timeout ms)
{
    const Ticks tick_count = GetTicksFromMs(static_cast<Time>(ms));
    Sleep(static_cast<Timeout>(tick_count < WAIT_INFINITE ? tick_count : WAIT_INFINITE));
}

/*! \brief     Put calling process into a sleep state until the specified timestamp.
    \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
    \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
    \param[in] timestamp: Absolute timestamp (ticks). 0 does not cause yield, use Yield instead. Negative will cause an assertion.
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
    \return    True if sleep succeeded, false otherwise.
*/
__stk_forceinline bool SleepUntil(Ticks timestamp)
{
    return IKernelService::GetInstance()->SleepUntil(timestamp);
}

/*! \brief     Cancel sleep of the task.
    \param[in] task_id: Id of the task.
    \note      No-op if task was not in a sleeping state.
    \note      ISR-safe.
*/
__stk_forceinline void SleepCancel(TId task_id)
{
    IKernelService::GetInstance()->SleepCancel(task_id);
}

/*! \brief     Notify scheduler to switch to the next runnable task.
    \note      A cooperative scheduling mechanism. In HRT mode acts as a cooperation point (see stk::KERNEL_HRT).
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
*/
__stk_forceinline void Yield()
{
    IKernelService::GetInstance()->SwitchToNext();
}

/*! \brief     Delay calling process by busy-waiting until the deadline expires.
    \note      Unlike Sleep this function delays code execution by spinning in a loop until deadline expiry.
    \note      Use with care in HRT mode to avoid missed deadline (see stk::KERNEL_HRT, ITask::OnDeadlineMissed).
    \param[in] tick_count: Delay time (in ticks). Negative will cause an assertion.
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
*/
__stk_forceinline void Delay(Timeout tick_count)
{
    IKernelService::GetInstance()->Delay(tick_count);
}

/*! \brief     Delay calling process by busy-waiting until the deadline expires.
    \note      Unlike Sleep this function delays code execution by spinning in a loop until deadline expiry.
    \note      Use with care in HRT mode to avoid missed deadline (see stk::KERNEL_HRT, ITask::OnDeadlineMissed).
    \param[in] ms: Delay time (milliseconds). Negative will cause an assertion.
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
*/
static inline void DelayMs(Timeout ms)
{
    const Ticks tick_count = GetTicksFromMs(static_cast<Time>(ms));
    Delay(static_cast<Timeout>(tick_count < WAIT_INFINITE ? tick_count : WAIT_INFINITE));
}

} // namespace stk

#endif /* STK_HELPER_H_ */
