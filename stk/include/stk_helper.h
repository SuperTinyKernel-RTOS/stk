/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
    public:
        stk::RunFuncType GetFunc() { return &Run; }
        void *GetFuncUserData() { return this; }

    private:
        static void Run(void *user_data)
        {
            ((MyTask *)user_data)->RunInner();
        }

        void RunInner()
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

    Word *GetStack() const { return const_cast<Word *>(m_stack); }
    size_t GetStackSize() const { return _StackSize; }
    size_t GetStackSizeBytes() const { return _StackSize * sizeof(Word); }
    EAccessMode GetAccessMode() const { return _AccessMode; }

    /*! \brief Default no-op handler. Override in subclass to log or handle missed deadlines.
        \note  HRT deadline misses are only possible when the kernel is started with KERNEL_HRT.
    */
    virtual void OnDeadlineMissed(uint32_t duration) { (void)duration; }

    /*! \brief Default weight of 1. Override in subclass if custom scheduling weight is needed.
        \note  Only relevant when using SwitchStrategySmoothWeightedRoundRobin. Prefer TaskW for
               compile-time weight assignment.
    */
    virtual int32_t GetWeight() const { return 1; }

    /*! \brief Get object's own address as its Id. Unique per task instance, requires no manual assignment.
    */
    virtual TId GetId() const { return hw::PtrToWord(this); }

    /*! \brief Override in subclass to supply a name for SEGGER SystemView tracing. Returns NULL by default.
    */
    virtual const char *GetTraceName() const { return nullptr; }

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
    ~Task()
    {}

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
template <int32_t _Weight, size_t _StackSize, EAccessMode _AccessMode>
class TaskW : public ITask
{
public:
    enum { STACK_SIZE = _StackSize }; //!< Stack size in elements of Word, mirrors the _StackSize template parameter.

    Word *GetStack() const { return const_cast<Word *>(m_stack); }
    size_t GetStackSize() const { return _StackSize; }
    size_t GetStackSizeBytes() const { return _StackSize * sizeof(Word); }
    EAccessMode GetAccessMode() const { return _AccessMode; }

    /*! \brief Hard Real-Time mode is unsupported for weighted tasks. Triggers an assertion if called.
        \warning Do not use TaskW with KERNEL_HRT. Use Task instead.
    */
    virtual void OnDeadlineMissed(uint32_t duration) { STK_ASSERT(false); (void)duration; }

    /*! \brief Returns the compile-time weight _Weight.
    */
    virtual int32_t GetWeight() const { return _Weight; }

    /*! \brief Get object's own address as its Id. Unique per task instance, requires no manual assignment.
    */
    virtual TId GetId() const { return hw::PtrToWord(this); }

    /*! \brief Override in subclass to supply a name for SEGGER SystemView tracing. Returns NULL by default.
    */
    virtual const char *GetTraceName() const { return nullptr; }

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
    ~TaskW()
    {}

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
    ~StackMemoryWrapper()
    {}

    /*! \brief Get pointer to the first element of the wrapped stack array.
    */
    Word *GetStack() const { return (*m_stack); }

    /*! \brief Get number of elements in the wrapped stack array.
    */
    size_t GetStackSize() const { return _StackSize; }

    /*! \brief Get size of the wrapped stack array in bytes.
    */
    size_t GetStackSizeBytes() const { return _StackSize * sizeof(Word); }

private:
    MemoryType *m_stack; //!< Pointer to the externally-owned stack memory array.
};

/*! \brief     Get task/thread Id of the calling task.
    \return    Id of the calling task/thread.
    \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
*/
__stk_forceinline TId GetTid()
{
    STK_ASSERT(!hw::IsInsideISR());

    return IKernelService::GetInstance()->GetTid();
}

/*! \brief     Convert ticks to milliseconds.
    \param[in] ticks: Tick count to convert.
    \param[in] resolution: Microseconds per tick, as returned by IKernelService::GetTickResolution().
    \return    Equivalent time in milliseconds.
    \note      ISR-safe (performs only arithmetic, no kernel calls).
*/
__stk_forceinline int64_t GetMsecFromTicks(int64_t ticks, int32_t resolution)
{
    return (ticks * resolution) / 1000;
}

/*! \brief     Convert milliseconds to ticks.
    \param[in] msec: Time in milliseconds to convert.
    \param[in] resolution: Microseconds per tick, as returned by IKernelService::GetTickResolution().
    \return    Equivalent tick count.
    \note      ISR-safe (performs only arithmetic, no kernel calls).
*/
__stk_forceinline Ticks GetTicksFromMsec(int64_t msec, int32_t resolution)
{
    return msec * 1000 / resolution;
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
__stk_forceinline int32_t GetTickResolution()
{
    return IKernelService::GetInstance()->GetTickResolution();
}

/*! \brief     Convert milliseconds to ticks using the current kernel tick resolution.
    \param[in] msec: Time in milliseconds to convert.
    \return    Equivalent tick count.
    \note      Convenience overload that queries GetTickResolution() automatically.
               Use the two-argument form GetTicksFromMsec(msec, resolution) in ISR context.
    \warning   ISR-unsafe (internally calls GetTickResolution() which accesses the kernel service).
*/
__stk_forceinline Ticks GetTicksFromMsec(int64_t msec)
{
    return GetTicksFromMsec(msec, GetTickResolution());
}

/*! \brief     Get current time in milliseconds since kernel start.
    \return    Milliseconds elapsed since IKernel::Start() was called.
    \note      ISR-safe.
    \note      When the tick resolution is exactly 1000 µs (1 ms, the default PERIODICITY_DEFAULT),
               the tick count is returned directly without multiplication, avoiding a 64-bit multiply.
*/
__stk_forceinline int64_t GetTimeNowMsec()
{
    IKernelService *service = IKernelService::GetInstance();
    int32_t resolution = service->GetTickResolution();

    if (resolution == 1000) // fast path: tick == 1 ms, no conversion needed
        return service->GetTicks();
    else
        return (service->GetTicks() * resolution) / 1000;
}

/*! \brief     Delay calling process by busy-waiting until the deadline expires.
    \note      Unlike Sleep this function delays code execution by spinning in a loop until deadline expiry.
    \note      Use with care in HRT mode to avoid missed deadline (see stk::KERNEL_HRT, ITask::OnDeadlineMissed).
    \param[in] msec: Delay time (milliseconds).
    \warning   ISR-unsafe. Calling from an ISR would spin indefinitely, deadlocking the system.
*/
__stk_forceinline void Delay(uint32_t msec)
{
    STK_ASSERT(!hw::IsInsideISR());

    IKernelService::GetInstance()->Delay(msec);
}

/*! \brief     Put calling process into a sleep state.
    \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
    \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
    \param[in] msec: Sleep time (milliseconds).
    \warning   ISR-unsafe. Calling from an ISR would block the scheduler indefinitely, deadlocking the system.
*/
__stk_forceinline void Sleep(uint32_t msec)
{
    STK_ASSERT(!hw::IsInsideISR());

    IKernelService::GetInstance()->Sleep(msec);
}

/*! \brief     Notify scheduler to switch to the next runnable task.
    \note      A cooperative scheduling mechanism. In HRT mode acts as a cooperation point (see stk::KERNEL_HRT).
    \warning   ISR-unsafe. Calling from an ISR would block the scheduler indefinitely, deadlocking the system.
*/
__stk_forceinline void Yield()
{
    STK_ASSERT(!hw::IsInsideISR());

    IKernelService::GetInstance()->SwitchToNext();
}

} // namespace stk

#endif /* STK_HELPER_H_ */
