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

    To implement final concrete version of the user task inherit your implementation from this class.

    Usage example:
    \code
    template <stk::EAccessMode _AccessMode>
    class MyTask : public stk::Task<256, _AccessMode>
    {
    public:
        stk::RunFuncT GetFunc() { return &Run; }
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
                // do dome work here ...
            }
        }
    };

    MyTask<ACCESS_PRIVILEGED> my_task;
    \endcode
*/
template <uint32_t _StackSize, EAccessMode _AccessMode>
class Task : public ITask
{
public:
    enum { STACK_SIZE = _StackSize };
    size_t *GetStack() const { return const_cast<size_t *>(m_stack); }
    uint32_t GetStackSize() const { return _StackSize; }
    uint32_t GetStackSizeBytes() const { return _StackSize * sizeof(size_t); }
    EAccessMode GetAccessMode() const { return _AccessMode; }
    virtual void OnDeadlineMissed(uint32_t duration) { (void)duration; }
    virtual int32_t GetWeight() const { return 1; }
    virtual size_t GetId() const  { return reinterpret_cast<size_t>(this); }
    virtual const char *GetTraceName() const  { return NULL; }

private:
    typename StackMemoryDef<_StackSize>::Type m_stack; //!< memory region
};

/*! \class TaskW
    \brief Partial implementation of the user task. Use for SwitchStrategySmoothWeightedRoundRobin.

    See implementation details and example in Task.
*/
template <int32_t _Weight, uint32_t _StackSize, EAccessMode _AccessMode>
class TaskW : public ITask
{
public:
    enum { STACK_SIZE = _StackSize };
    size_t *GetStack() const { return const_cast<size_t *>(m_stack); }
    uint32_t GetStackSize() const { return _StackSize; }
    uint32_t GetStackSizeBytes() const { return _StackSize * sizeof(size_t); }
    EAccessMode GetAccessMode() const { return _AccessMode; }
    virtual void OnDeadlineMissed(uint32_t duration) { STK_ASSERT(false); /* HRT is unsupported */ (void)duration; }
    virtual int32_t GetWeight() const { return _Weight; }
    virtual size_t GetId() const  { return reinterpret_cast<size_t>(this); }
    virtual const char *GetTraceName() const  { return NULL; }

private:
    typename StackMemoryDef<_StackSize>::Type m_stack; //!< memory region
};

/*! \class StackMemoryWrapper
    \brief Stack memory wrapper for IStackMemory interface.
    \note  Wrapper design pattern.
*/
template <uint32_t _StackSize>
class StackMemoryWrapper : public IStackMemory
{
public:
    /*! \typedef MemoryType
        \brief   Memory type which can be wrapped.
    */
    typedef typename StackMemoryDef<_StackSize>::Type MemoryType;

    /*! \brief Constructor .
    */
    explicit StackMemoryWrapper(MemoryType *stack) : m_stack(stack)
    {
        // note: stack size must be STACK_SIZE_MIN or bigger
        STK_STATIC_ASSERT(_StackSize >= STACK_SIZE_MIN);
    }

    size_t *GetStack() const { return (*m_stack); }
    uint32_t GetStackSize() const { return _StackSize; }
    uint32_t GetStackSizeBytes() const { return _StackSize * sizeof(size_t); }

private:
    MemoryType *m_stack; //!< pointer to the wrapped memory region
};

/*! \brief     Get task/thread Id.
    \warning   ISR-unsafe.
    \return    Id of the calling task/thread.
*/
__stk_forceinline TId GetTid()
{
    // Kernel will not locate a task inside ISR
    STK_ASSERT(!hw::IsInsideISR());

    return IKernelService::GetInstance()->GetTid();
}

/*! \brief     Get milliseconds from ticks.
    \param[in] ticks: Ticks to convert.
    \param[in] resolution: Resolution (see IKernelService::GetTickResolution).
    \return    Milliseconds.
*/
__stk_forceinline int64_t GetMsecFromTicks(int64_t ticks, int32_t resolution)
{
    return (ticks * resolution) / 1000;
}

/*! \brief     Get ticks from milliseconds.
    \param[in] msec: Milliseconds to convert.
    \param[in] resolution: Resolution (see IKernelService::GetTickResolution).
    \return    Ticks.
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

/*! \brief     Get ticks from milliseconds.
    \param[in] msec: Milliseconds to convert.
    \return    Ticks.
*/
__stk_forceinline Ticks GetTicksFromMsec(int64_t msec)
{
    return GetTicksFromMsec(msec, GetTickResolution());
}

/*! \brief     Get current time in milliseconds.
    \note      ISR-safe.
    \return    Milliseconds.
*/
__stk_forceinline int64_t GetTimeNowMsec()
{
    IKernelService *service = IKernelService::GetInstance();
    int32_t resolution = service->GetTickResolution();

    if (1000 == resolution)
        return service->GetTicks();
    else
        return (service->GetTicks() * resolution) / 1000;
}

/*! \brief     Delay calling process.
    \note      Unlike Sleep this function delays code execution by spinning in a loop until deadline expiry.
    \note      Use with care in HRT mode to avoid missed deadline (see stk::KERNEL_HRT, ITask::OnDeadlineMissed).
    \param[in] msec: Delay time (milliseconds).
    \warning   ISR-unsafe.
*/
__stk_forceinline void Delay(uint32_t msec)
{
    // ISR blocks scheduler and will wait indefinitely (deadlock)
    STK_ASSERT(!hw::IsInsideISR());

    IKernelService::GetInstance()->Delay(msec);
}

/*! \brief     Put calling process into a sleep state.
    \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
    \note      Unsupported in HRT mode (see stk::KERNEL_HRT), instead task will sleep automatically according its periodicity and workload.
    \param[in] msec: Sleep time (milliseconds).
    \warning   ISR-unsafe.
*/
__stk_forceinline void Sleep(uint32_t msec)
{
    // ISR blocks scheduler and will wait indefinitely (deadlock)
    STK_ASSERT(!hw::IsInsideISR());

    IKernelService::GetInstance()->Sleep(msec);
}

/*! \brief     Notify scheduler that it can switch to a next task.
    \warning   ISR-unsafe.
*/
__stk_forceinline void Yield()
{
    // ISR blocks scheduler and will wait indefinitely (deadlock)
    STK_ASSERT(!hw::IsInsideISR());

    IKernelService::GetInstance()->SwitchToNext();
}

/*! \class PeriodicTimer
    \brief Lightweight periodic time accumulator with callback notification.

    \tparam _PeriodMsec Period value in milliseconds which defines the timer interval.

    \note  This timer accumulates elapsed time between consecutive Update() calls
           using stk::GetTimeNowMsec(). When the accumulated time reaches or exceeds
           the configured period, a user-provided callback is invoked.

    Usage example:
    \code
    PeriodicTimer<1000> timer; // 1 second period

    timer.Update([](int64_t now, uint32_t elapsed) {
        // called every ~1000 ms
    });
    \endcode
*/
template <uint32_t PeriodMsec>
struct PeriodicTimer
{
    int64_t  prev;    //!< timestamp of the previous Update() call in milliseconds
    uint32_t elapsed; //!< accumulated elapsed time in milliseconds

    /*! \brief Construct a periodic timer and initialize the reference timestamp.
    */
    PeriodicTimer() : prev(GetTimeNowMsec()), elapsed(0)
    {}

    /*! \brief     Reset timer state.
        \note      This method resets the accumulated elapsed time to zero and
                   reinitializes the reference timestamp using the current value
                   returned by stk::GetTimeNowMsec().
        \note      After calling Reset(), the next callback invocation will occur
                   only after a full period has elapsed.
    */
    void Reset()
    {
        prev = GetTimeNowMsec();
        elapsed = 0;
    }

    /*! \brief     Update the timer and invoke callback when the period is reached.
        \tparam    _Callback: Callable type accepting (int64_t now, uint32_t elapsed).
        \param[in] cb: User callback invoked when accumulated time reaches the period.
        \note      The callback is called with:
                   - \c now: current timestamp returned by stk::GetTimeNowMsec().
                   - \c elapsed: accumulated elapsed time in milliseconds.
        \note      If accumulated time exceeds the period, the remainder is preserved
                   to maintain timing accuracy across updates.
        \warning   This method assumes monotonic behavior of stk::GetTimeNowMsec().
    */
    template <typename _Callback>
    void Update(_Callback&& cb)
    {
        int64_t now = GetTimeNowMsec();
        elapsed += (uint32_t)(now - prev);
        prev = now;

        if (elapsed >= PeriodMsec)
        {
            cb(now, elapsed);
            elapsed -= PeriodMsec;
        }
    }
};

} // namespace stk

#endif /* STK_HELPER_H_ */
