/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_COMMON_H_
#define STK_COMMON_H_

#include "stk_defs.h"
#include "stk_linked_list.h"

/*! \file  stk_common.h
    \brief Contains interface definitions of the library.
*/

namespace stk {

// Forward declarations:
class IKernelService;
class IKernelTask;
class ITask;

/*! \enum    EAccessMode
    \brief   Hardware access mode by the user task.
    \warning Type is explicitly 32-bit to be compatible with platform implementations.
*/
enum EAccessMode : int32_t
{
    ACCESS_USER = 0,  //!< Unprivileged access mode (access to some hardware is restricted, see CPU manual for details).
    ACCESS_PRIVILEGED //!< Privileged access mode (access to hardware is fully unrestricted).
};

/*! \enum  EKernelMode
    \brief Kernel operating mode.
*/
enum EKernelMode : uint8_t
{
    KERNEL_STATIC   = (1 << 0), //!< All tasks are static and can not exit.
    KERNEL_DYNAMIC  = (1 << 1), //!< Tasks can be added or removed and therefore exit when done.
    KERNEL_HRT      = (1 << 2), //!< Hard Real-Time (HRT) behavior (tasks are scheduled periodically and have an execution deadline, whole system is failed when task's deadline is failed).
    KERNEL_SYNC     = (1 << 3), //!< Synchronization support (see \a Event).
    KERNEL_TICKLESS = (1 << 4), //!< Tickless mode. To use this mode STK_TICKLESS_IDLE must be defined to 1 in stk_config.h.
};

/*! \enum  EKernelPanicId
    \brief Identifies the source of a kernel panic.
*/
enum EKernelPanicId : uint32_t
{
    KERNEL_PANIC_NONE                = 0, //!< Panic is absent (no fault).
    KERNEL_PANIC_SPINLOCK_DEADLOCK   = 1, //!< Spin-lock timeout expired: lock owner never released.
    KERNEL_PANIC_STACK_CORRUPT       = 2, //!< Stack integrity check failed.
    KERNEL_PANIC_ASSERT              = 3, //!< Internal assertion failed (maps from STK_ASSERT).
    KERNEL_PANIC_HRT_HARD_FAULT      = 4, //!< Kernel running in KERNEL_HRT mode reported deadline failure of the task.
    KERNEL_PANIC_CPU_EXCEPTION       = 5, //!< CPU reported an exception and halted execution.
    KERNEL_PANIC_CS_NESTING_OVERFLOW = 6, //!< Critical section nesting limit exceeded: violation of STK_CRITICAL_SECTION_NESTINGS_MAX.
    KERNEL_PANIC_UNKNOWN_SVC         = 7, //!< Unknown service command received by SVC handler.
    KERNEL_PANIC_BAD_STATE           = 8, //!< Kernel entered unexpected (bad) state.
    KERNEL_PANIC_BAD_MODE            = 9  //!< Kernel is in bad/unsupported mode for the current operation.
};

/*! \enum  EStackType
    \brief Stack type.
    \see   IPlatform::InitStack
*/
enum EStackType
{
    STACK_USER_TASK = 0, //!< Stack of the user task.
    STACK_SLEEP_TRAP,    //!< Stack of the Sleep trap.
    STACK_EXIT_TRAP      //!< Stack of the Exit trap.
};

/*! \enum  EConsts
    \brief Constants.
*/
enum EConsts
{
    PERIODICITY_MAX      = 99000,             //!< Maximum periodicity (microseconds), 99 milliseconds (note: this value is the highest working on a real hardware and QEMU).
    PERIODICITY_DEFAULT  = 1000,              //!< Default periodicity (microseconds), 1 millisecond.
    STACK_SIZE_MIN       = STK_STACK_SIZE_MIN //!< Minimum stack size in elements of Word. Used as a lower bound for all stack allocations (user task, sleep trap, exit trap). See: StackMemoryDef, StackMemoryWrapper.
};

/*! \enum  ESystemTaskId
    \brief System task id.
*/
enum ESystemTaskId
{
    SYS_TASK_ID_SLEEP = 0xFFFFFFFF, //!< Sleep trap.
    SYS_TASK_ID_EXIT  = 0xFFFFFFFE  //!< Exit trap.
};

/*! \enum  ETraceEventId
    \brief Trace event identifiers for tracing task suspension and resume with debugging tools (e.g. SEGGER SystemView).
    \note  Values are offset by 1000 to avoid collisions with the host tool's built-in system event range (0–999).
*/
enum ETraceEventId
{
    TRACE_EVENT_UNKNOWN = 0,        //!< Unknown / uninitialized trace event.
    TRACE_EVENT_SWITCH  = 1000 + 1, //!< Task context switch event (task became active).
    TRACE_EVENT_SLEEP   = 1000 + 2  //!< Task entered sleep / blocked state.
};

/*! \typedef Word
    \brief   Native processor word type.
    \details Represents natural data width of the CPU (matching uintptr_t).
             Used for stack allocation, register storage, pointer value storage,
             and memory alignment to ensure atomic access, optimal performance,
             and hardware compatibility.
*/
typedef uintptr_t Word;

/*! \typedef ThreadId
    \brief   Task/thread id.
*/
typedef Word TId;

/*! \typedef Timeout
    \brief   Timeout time (ticks).
*/
typedef int32_t Timeout;

/*! \typedef Ticks
    \brief   Ticks value.
*/
typedef int64_t Ticks;

/*! \typedef Time
    \brief   Time value.
*/
typedef int64_t Time;

/*! \typedef Cycles
    \brief   Cycles value.
*/
typedef uint64_t Cycles;

/*! \typedef Weight
    \brief   Weight value (aka priority).
*/
typedef int32_t Weight;

/*! \var     TID_ISR_N
    \brief   Bitmask sentinel for ISR-context task identifiers.

    The upper 20 bits of the TId space are reserved for ISR contexts.
    When GetTid() is called from an interrupt service routine, it returns
    \c TID_ISR_N | exception_number, where exception_number is the raw
    value:

    \code
    //  bits[31:12] = 0xFFFFF (this sentinel mask)
    //  bits[11:0]  = exception number
    \endcode

    This encoding guarantees uniqueness per exception, so two ISRs at
    different priority levels or with different exception numbers are never
    treated as the same owner by synchronization primitives.

    Task TIds are word-aligned pointers. On all supported Cortex-M and RISC-V targets
    they fall in the range \c 0x00000000..0xEFFFFFFF, so no overlap with
    the \c 0xFFFFF000..0xFFFFFFFF sentinel range is possible.

    \note  Use \c IsIsrTid() to test for this sentinel rather than comparing
           against this constant directly.
    \see   IsIsrTid()
*/
static constexpr TId TID_ISR_N = static_cast<TId>(0xFFFFF000U);

/*! \var     TID_NONE
    \brief   Reserved task/thread id representing zero/none thread id.
*/
static constexpr TId TID_NONE = static_cast<TId>(0U);

/*! \var     WAIT_INFINITE
    \brief   Timeout value: block indefinitely until the synchronization object is signaled.
    \note    Pass as the \a timeout argument to IKernelService::Wait().
*/
static constexpr Timeout WAIT_INFINITE = INT32_MAX;

/*! \var     NO_WAIT
    \brief   Timeout value: return immediately if the synchronization object is not yet signaled (non-blocking poll).
    \note    Pass as the \a timeout argument to IKernelService::Wait().
*/
static constexpr Timeout NO_WAIT = 0;

/*! \var     NO_WEIGHT
    \brief   Weight value: weight is not set.
*/
static constexpr Weight NO_WEIGHT = static_cast<Weight>(-1);

/*! \var     DEFAULT_WEIGHT
    \brief   Weight value: default weight of value (1) (see \c SwitchStrategySmoothWeightedRoundRobin).
*/
static constexpr Weight DEFAULT_WEIGHT = static_cast<Weight>(1);

/*! \brief     Test whether a task identifier represents an ISR context.

    Returns \c true if \a tid was produced by GetTid() called from an
    interrupt service routine, i.e. its upper 20 bits match \c TID_ISR_N.
    Use this predicate instead of comparing against \c TID_ISR_N directly.

    \param[in] tid: Task identifier to test.
    \return    \c true if \a tid encodes an ISR context, \c false otherwise.
    \note      ISR-safe (bitmask arithmetic only, no kernel calls).
    \see       TID_ISR_N
*/
static inline bool IsIsrTid(TId tid) { return ((tid & TID_ISR_N) == TID_ISR_N); }


/*! \class StackMemoryDef
    \brief Stack memory type definition.
    \note  This descriptor provides an encapsulated type only on basis of which you can declare
           your memory array variable.

    Usage example:
    \code
    StackMemoryDef<128>::Type my_memory_array;
    \endcode
*/
template <size_t _StackSize> struct StackMemoryDef
{
    enum { SIZE = _StackSize };

    /*! \typedef Type
        \brief   Stack memory type.
    */
    typedef __stk_aligned(STK_STACK_MEMORY_ALIGN) Word Type[_StackSize];
};

/*! \class Stack
    \brief Stack descriptor.
*/
struct Stack
{
    Word        SP;   //!< Stack Pointer (SP) register (note: must be the first entry in this struct).
    EAccessMode mode; //!< Hardware access mode of the owning task (see \a EAccessMode).
#if STK_STACK_NEEDS_TASK_ID
    TId         tid;  //!< Task id (see \a STK_SEGGER_SYSVIEW).
#endif
#ifdef _STK_ARCH_ARM_CORTEX_M
#if STK_TLS && !STK_TLS_PREFER_REGISTER
    Word        tls;  //!< Thread-local storage if not using ARM Cortex-M R9 register for a fast inline access to TLS.
#endif
#endif
};

/*! \class IStackMemory
    \brief Interface for a stack memory region.
*/
class IStackMemory
{
public:
    /*! \brief   Get pointer to the stack memory.
    */
    virtual Word *GetStack() const = 0;

    /*! \brief   Get number of elements of the stack memory array.
    */
    virtual size_t GetStackSize() const = 0;

    /*! \brief   Get size of the memory in bytes.
    */
    virtual size_t GetStackSizeBytes() const = 0;

    /*! \brief   Get available stack space.
        \return  Number of elements of the stack memory array remaining on the stack (computed via the
                 watermark pattern).
                 Returns 0 if the stack has been fully used or the watermark \a STK_STACK_MEMORY_FILLER
                 was overwritten.
        \warning Stack type: Bottom to Top (index[0]).
    */
    virtual size_t GetStackSpace() const
    {
        const Word *stack = GetStack();
        const size_t stack_size = GetStackSize();

        // count leading Words equal to STK_STACK_MEMORY_FILLER (watermark)
        size_t space = 0U;
        for ( ; (space < stack_size) && (stack[space] == STK_STACK_MEMORY_FILLER); ++space)
        {}

        return space;
    }
};

/*! \class IWaitObject
    \brief Wait object.
*/
class IWaitObject : public util::DListEntry<IWaitObject, false>
{
public:
    /*! \typedef   ListHeadType
        \brief     List head type for IWaitObject elements.
    */
    typedef DLHeadType ListHeadType;

    /*! \typedef   ListEntryType
        \brief     List entry type of IWaitObject elements.
    */
    typedef DLEntryType ListEntryType;

    /*! \brief     Get thread Id of this task.
        \return    Thread Id.
    */
    virtual TId GetTid() const = 0;

    /*! \brief     Wake task.
        \param[in] timeout: \c true if the task is waking due to timeout expiry;
                   \c false if waking due to a successful signal.
        \note      The implementation is responsible for removing this wait object from the synchronization
                   object's wait list (via ISyncObject::RemoveWaitObject) as part of the wake sequence.
    */
    virtual void Wake(bool timeout) = 0;

    /*! \brief     Check if task woke up due to a timeout.
        \return    Returns \a true if timeout, \a false otherwise.
    */
    virtual bool IsTimeout() const = 0;

    /*! \brief     Update wait object's waiting time.
        \param[in] elapsed_ticks: Number of ticks elapsed between this and previous calls, in case of
                   KERNEL_TICKLESS mode this value can be >1, for non-tickless mode it is always 1.
        \return    Returns \a true if update caused a timeout of the object, \a false otherwise.
    */
    virtual bool Tick(Timeout elapsed_ticks) = 0;
};

/*! \class ITraceable
    \brief Traceable object.
    \note  Used for debugging and tracing by tools like SEGGER SystemView. See \a STK_SYNC_DEBUG_NAMES.
*/
class ITraceable
{
public:
#if STK_SYNC_DEBUG_NAMES
    ITraceable() : m_trace_name(nullptr)
    {}
#endif

    /*! \brief     Set name.
        \param[in] name: Null-terminated string or \c NULL.
        \note      If STK_SYNC_DEBUG_NAMES is 0 then calling this function has no effect.
    */
    void SetTraceName(const char *name)
    {
    #if STK_SYNC_DEBUG_NAMES
        m_trace_name = name;
    #else
        STK_UNUSED(name);
    #endif
    }

    /*! \brief     Get name.
        \return    Name string, or \c NULL if not set or if STK_SYNC_DEBUG_NAMES is 0.
    */
    const char *GetTraceName() const
    {
    #if STK_SYNC_DEBUG_NAMES
        return m_trace_name;
    #else
        return nullptr;
    #endif
    }

protected:
#if STK_SYNC_DEBUG_NAMES
    const char *m_trace_name; //!< name (debug/tracing only)
#endif
};

/*! \class ISyncObject
    \brief Synchronization object.
*/
class ISyncObject : public util::DListEntry<ISyncObject, false>
{
    friend class IKernel;

public:
    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~ISyncObject()
    {}

    /*! \typedef   ListHeadType
        \brief     List head type for ISyncObject elements.
    */
    typedef DLHeadType ListHeadType;

    /*! \typedef   ListEntryType
        \brief     List entry type of ISyncObject elements.
    */
    typedef DLEntryType ListEntryType;

    /*! \brief     Called by kernel when a new task starts waiting on this event.
        \param[in] wobj: Wait object representing blocked task.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    virtual void AddWaitObject(IWaitObject *wobj)
    {
        STK_ASSERT(wobj->GetHead() == nullptr);
        m_wait_list.LinkBack(wobj);
    }

    /*! \brief     Called by kernel when a waiting task is being removed (timeout expired, wait aborted, task terminated etc.).
        \param[in] wobj: Wait object to remove from the wait list.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    virtual void RemoveWaitObject(IWaitObject *wobj)
    {
        STK_ASSERT(wobj->GetHead() == &m_wait_list);
        m_wait_list.Unlink(wobj);
    }

    /*! \brief     Called by kernel on every system tick to handle timeout logic of waiting tasks.
        \param[in] elapsed_ticks: Number of ticks elapsed between this and previous calls, in case of
                   KERNEL_TICKLESS mode this value can be >1, for non-tickless mode it is always 1.
        \return    \c true if this synchronization object still has waiters with a finite timeout and
                   requires further tick calls. \c false if the wait list is empty or all remaining
                   waiters have infinite timeouts, signaling to the kernel that it may stop calling
                   Tick() for this object until a new waiter is added.
        \note      When this method returns \c false, the kernel unlinks this object from its active
                   sync list. It will be re-linked automatically when the next waiter is added via
                   AddWaitObject().
        \note      Does not need to be called inside critical section.
    */
    virtual bool Tick(Timeout elapsed_ticks);

    /*! \brief     Find higher weight within linked wait objects.
        \param[in] comp: Weight to compare with.
        \return    Higher weight value than \a comp, or \a NO_WEIGHT if there is no object with a higher weight.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    Weight FindWeightHigherThan(Weight comp) const;

protected:
    /*! \brief     Constructor.
        \note      Can not be standalone object, must be inherited by the implementation.
    */
    explicit ISyncObject() : m_wait_list()
    {}

    /*! \brief     Wake the first task in the wait list (FIFO order).
        \note      The woken task is notified with timeout=false, indicating a successful signal
                   (not a timeout expiry).
        \note      Does nothing if no tasks are currently waiting.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    void WakeOne()
    {
        if (!m_wait_list.IsEmpty())
            static_cast<IWaitObject *>(m_wait_list.GetFirst())->Wake(false);
    }

    /*! \brief     Wake all tasks currently in the wait list.
        \note      Each woken task is notified with timeout=false, indicating a successful signal
                   (not a timeout expiry).
        \note      Does nothing if no tasks are currently waiting.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    void WakeAll()
    {
        while (!m_wait_list.IsEmpty())
            static_cast<IWaitObject *>(m_wait_list.GetFirst())->Wake(false);
    }

    IWaitObject::ListHeadType m_wait_list; //!< tasks blocked on this object
};

/*! \class IMutex
    \brief Interface for mutex synchronization primitive.
    \note  Implementations may be either recursive or non-recursive.
*/
class IMutex
{
public:
    /*! \class ScopedLock
        \brief Locks bound mutex within a scope of execution. Ensures the mutex is always unlocked
               when leaving the scope, even when exceptions are thrown.
        \note  RAII
    */
    class ScopedLock
    {
    public:
        explicit ScopedLock(IMutex &mutex) : m_mutex(mutex) { m_mutex.Lock(); }
        ~ScopedLock() { m_mutex.Unlock(); }

    private:
        STK_NONCOPYABLE_CLASS(ScopedLock);

        IMutex &m_mutex;
    };

    /*! \brief Lock the mutex.
    */
    virtual void Lock() = 0;

    /*! \brief Unlock the mutex.
    */
    virtual void Unlock() = 0;
};

/*! \class ITask
    \brief Interface for a user task.

    \note  Inherit this interface by your thread/task class to make it schedulable by the Kernel.

    Usage example:
    \code
    class MyTask : public stk::Task<256, ACCESS_USER>
    {
        void Run() override
        {
            while (true)
            {
                // task logic here
            }
        }
    };

    MyTask task1, task2;
    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    \endcode
*/
class ITask : public IStackMemory
{
public:
    /*! \brief     Entry point of the user task.
        \note      Called by the Kernel when the task is scheduled for execution.
                   Implement this method with the task's main logic.
        \warning   If \c Kernel is configured as \c KERNEL_STATIC, the body must contain an infinite loop.
        \code
        void Run( override)
        {
            while (true)
            {
                // task logic here
            }
        }
        \endcode
    */
    virtual void Run() = 0;

    /*! \brief     Get hardware access mode of the user task.
    */
    virtual EAccessMode GetAccessMode() const = 0;

    /*! \brief     Called by the scheduler if deadline of the task is missed when Kernel is operating in Hard Real-Time mode (see stk::KERNEL_HRT).
        \param[in] duration: Elapsed active time in ticks at the point the deadline was detected.
                   Always greater than the task's configured deadline (ticks).
        \note      Optional handler. Use it for fault logging.
        \note      After this call returns, IPlatform::ProcessHardFault() is invoked and the
                   system enters a safe state. This function should not attempt to recover scheduling.
    */
    virtual void OnDeadlineMissed(uint32_t duration) { STK_UNUSED(duration); }

    /*! \brief     Called by the kernel before removal from the scheduling (see stk::KERNEL_DYNAMIC).
        \note      The task's stack is no longer in use but the ITask object itself is still valid.
        \note      The default no-op implementation is sufficient for detached tasks.
                   Override to implement join semantics (signal a waiting joiner).
        \note      Called in kernel/tick context - keep it short. ISR-safe primitives
                   only (e.g. stk::sync::Semaphore::Signal(), stk::sync::EventFlags::Set()).
        \note      KERNEL_DYNAMIC only. Never called in KERNEL_STATIC mode.
    */
    virtual void OnExit() {}

    /*! \brief     Get static base weight of the task.
        \return    Static weight value of the task (must be non-zero, positive 24-bit number).
        \see       SwitchStrategyFixedPriority, SwitchStrategySmoothWeightedRoundRobin,
                   IKernelTask::GetWeight, IKernelService::InheritWeight, IKernelService::RestoreWeight
    */
    virtual Weight GetWeight() const { return DEFAULT_WEIGHT; }
    /*! \brief     Get task Id set by application.
        \return    Application-defined task identifier. Return 0 if unused.
        \note      Used for debugging and tracing only. The kernel does not interpret this value.
    */
    TId GetId() const;

    /*! \brief     Get task trace name set by application.
        \return    Null-terminated name string, or \c NULL if unused.
        \note      Used for debugging and tracing only (e.g. SEGGER SystemView). Kernel does not
                   interpret this value.
    */
    virtual const char *GetTraceName() const { return nullptr; }
};

/*! \class IKernelTask
    \brief Scheduling-strategy-facing interface for a kernel task slot.

    Wraps a user ITask and exposes the metadata that task-switching strategy
    implementations (ITaskSwitchStrategy) need to make scheduling decisions:
    sleep state (IsSleeping, Wake), HRT timing (GetHrtPeriodicity, GetHrtDeadline,
    GetHrtRelativeDeadline), and weighted-round-robin support
    (GetWeight, GetCurrentWeight, SetCurrentWeight).
*/
class IKernelTask : public util::DListEntry<IKernelTask, true>
{
public:
    /*! \typedef   ListHeadType
        \brief     List head type for IKernelTask elements.
    */
    typedef DLHeadType ListHeadType;

    /*! \typedef   ListEntryType
        \brief     List entry type of IKernelTask elements.
    */
    typedef DLEntryType ListEntryType;

    /*! \brief     Get user task.
    */
    virtual ITask *GetUserTask() = 0;

    /*! \brief     Get pointer to the user task's stack.
    */
    virtual Stack *GetUserStack() = 0;

    /*! \brief     Get static base weight assigned to the task.
        \return    Static weight value of the task.
        \see       SwitchStrategySmoothWeightedRoundRobin, ITask::GetWeight
        \note      Weight API
    */
    virtual Weight GetWeight() const = 0;

    /*! \brief     Set the current dynamic weight value used by the scheduling strategy.
        \param[in] weight: New current dynamic weight value.
        \see       SwitchStrategySmoothWeightedRoundRobin
        \note      Weight API
    */
    virtual void SetCurrentWeight(Weight weight) = 0;

    /*! \brief     Get the current dynamic weight value of this task.
        \return    Current dynamic weight value.
        \see       SwitchStrategySmoothWeightedRoundRobin
        \note      Weight API
    */
    virtual Weight GetCurrentWeight() const = 0;

    /*! \brief     Get HRT task execution periodicity.
        \return    Periodicity of the task (ticks).
    */
    virtual Timeout GetHrtPeriodicity() const = 0;

    /*! \brief     Get HRT task deadline (max allowed task execution time).
        \return    Deadline of the task (ticks).
    */
    virtual Timeout GetHrtDeadline() const = 0;

    /*! \brief     Get HRT task's relative deadline.
        \return    Relative deadline of the task (ticks): the maximum allowed elapsed time from the
                   start of the current period until the task must complete. Unlike the absolute
                   deadline (GetHrtDeadline), this value is measured relative to the period start
                   and does not depend on the current tick counter.
        \see       GetHrtDeadline, GetHrtPeriodicity
    */
    virtual Timeout GetHrtRelativeDeadline() const = 0;

    /*! \brief     Check whether the task is currently sleeping.
        \return    True if the task is sleeping, false otherwise.
    */
    virtual bool IsSleeping() const = 0;

    /*! \brief     Wake a sleeping task on the next scheduling tick.
        \warning   The task must currently be sleeping (IsSleeping() == true).
                   Calling Wake() on a non-sleeping task is a programming error and
                   will trigger an assertion in the concrete implementation.
    */
    virtual void Wake() = 0;
};

/*! \class IPlatform
    \brief Interface for a platform driver.
    \note  Bridge design pattern. Do not put implementation details in the header of the
           concrete class to avoid breaking this pattern.

    Platform driver represents an underlying hardware and implements the following logic:
     - time tick
     - context switching
     - hardware access from the user task

    All functions are called by the kernel implementation.
*/
class IPlatform
{
public:
    /*! \class IEventHandler
        \brief Interface for a back-end event handler.

        It is inherited by the kernel implementation and delivers events from ISR.
    */
    class IEventHandler
    {
    public:
        /*! \brief      Called by ISR handler to notify that scheduling is about to start.
            \note       This event can be used to change hardware access mode for the first task.
            \param[out] active: Stack of the task which must enter Active state (to which context will switch).
        */
        virtual void OnStart(Stack *&active) = 0;

        /*! \brief  Called by driver to notify that scheduling is stopped.
            \note   Resets internal kernel state so that Start() may be called again
                    (KERNEL_DYNAMIC mode only). Has no effect in KERNEL_STATIC mode.
        */
        virtual void OnStop() = 0;

        /*! \brief      Called by ISR handler to notify about the next system tick.
            \param[out] idle: Stack of the task which must enter Idle state.
            \param[out] active: Stack of the task which must enter Active state (to which context will switch).
            \param[in,out] ticks: (STK_TICKLESS_IDLE=1 only) On entry: actual ticks elapsed since the
                        previous call, as measured by the platform driver. On return: the minimum remaining
                        sleep ticks across all active tasks, clamped to [1, STK_TICKLESS_TICKS_MAX].
                        Platform driver programs this value into the hardware timer to suppress unnecessary
                        wakeups. Absent in non-tickless builds.
            \note       To use a custom tick handler instead of the driver's built-in one, disable the
                        driver's handler in stk_config.h and call ProcessTick() manually:
                        \code
                        #define STK_SYSTICK_HANDLER _STK_SYSTICK_HANDLER_DISABLE
                        \endcode
        */
        virtual bool OnTick(Stack *&idle, Stack *&active
        #if STK_TICKLESS_IDLE
            , Timeout &ticks
        #endif
        ) = 0;

        /*! \brief      Called by Thread process (via IKernelService::SwitchToNext) to switch to a next task.
            \param[in]  caller_SP: Value of Stack Pointer (SP) register (for locating the calling process inside the kernel).
        */
        virtual void OnTaskSwitch(Word caller_SP) = 0;

        /*! \brief      Called by Thread process (via IKernelService::Sleep) for exclusion of the calling process from scheduling (sleeping).
            \param[in]  caller_SP: Value of Stack Pointer (SP) register (for locating the calling process inside the kernel).
            \param[in]  ticks: Time to sleep (ticks).
        */
        virtual void OnTaskSleep(Word caller_SP, Timeout ticks) = 0;

        /*! \brief      Called by Thread process (via IKernelService::SleepUntil) for exclusion of the calling process from scheduling (sleeping).
            \param[in]  caller_SP: Value of Stack Pointer (SP) register (for locating the calling process inside the kernel).
            \param[in]  timestamp: Absolute timestamp (ticks).
            \return     True if sleep succeeded, false otherwise.
        */
        virtual bool OnTaskSleepUntil(Word caller_SP, Ticks timestamp) = 0;

        /*! \brief      Called from the Thread process when task finished (its Run function exited by return).
            \param[out] stack: Stack of the exited task.
        */
        virtual void OnTaskExit(Stack *stack) = 0;

        /*! \brief      Called from the Thread process when task needs to wait.
            \param[in]  caller_SP: Value of Stack Pointer (SP) register (for locating the calling process inside the kernel).
            \param[in]  sync_obj: ISyncObject instance (passed by Wait).
            \param[in]  mutex: IMutex instance (passed by Wait).
            \param[in]  timeout: Time to sleep (ticks).
        */
        virtual IWaitObject *OnTaskWait(Word caller_SP, ISyncObject *sync_obj, IMutex *mutex, Timeout timeout) = 0;

        /*! \brief      Called from the Thread process when for getting task/thread id of the process.
            \param[in]  caller_SP: Value of Stack Pointer (SP) register (for locating the calling process inside the kernel).
            \return     Task/thread id of the process (returns always valid TId belonging to a task).
        */
        virtual TId OnGetTid(Word caller_SP) = 0;

        /*! \brief      Called from the Thread process to suspend scheduling.
            \param[in]  suspended: \c true if scheduling was successfully suspended, \c false otherwise.
        */
        virtual void OnSuspend(bool suspended) = 0;
    };

    /*! \class IEventOverrider
        \brief Interface for a platform event overrider.
        \note  Optional. Can be used to extend functionality of default IPlatform driver handlers from the user-space.
    */
    class IEventOverrider
    {
    public:
        /*! \brief  Called by Kernel when its entering a sleep mode.
            \return True if event is handled otherwise False to let driver handle it.
        */
        virtual bool OnSleep() = 0;

        /*! \brief  Called by Kernel when hard fault happens.
            \note   Normally called by Kernel when one of the scheduled tasks missed its deadline (see stk::KERNEL_HRT, IPlatform::HardFault).
            \return True if event is handled otherwise False to let driver handle it.
        */
        virtual bool OnHardFault() = 0;
    };

    /*! \brief     Initialize scheduler's context.
        \param[in] event_handler: Event handler.
        \param[in] service: Kernel service.
        \param[in] resolution_us: Tick resolution in microseconds (for example 1000 equals to 1 millisecond resolution).
        \param[in] exit_trap: Stack of the Exit trap (optional, provided if kernel is operating in KERNEL_DYNAMIC mode).
        \note      Must be called once before Start().
    */
    virtual void Initialize(IEventHandler *event_handler, IKernelService *service, uint32_t resolution_us, Stack *exit_trap) = 0;

    /*! \brief     Start scheduling.
        \note      This function never returns if kernel is initialized as KERNEL_STATIC.
                   Must be called after Initialize().
    */
    virtual void Start() = 0;

    /*! \brief     Stop scheduling.
    */
    virtual void Stop() = 0;

    /*! \brief     Initialize stack memory of the user task.
        \param[in] stack_type: Stack type.
        \param[in] stack: Stack descriptor.
        \param[in] stack_memory: Stack memory.
        \param[in] user_task: User task to which Stack belongs.
        \return    \c true on success, \c false if the stack memory is too small, misaligned, or the stack type is unsupported.
    */
    virtual bool InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) = 0;

    /*! \brief     Get resolution of the system tick timer in microseconds.
                   Resolution means a number of microseconds between system tick timer ISRs.
        \return    Microseconds.
    */
    virtual uint32_t GetTickResolution() const = 0;

    /*! \brief     Get system timer count value.
        \note      ISR-safe.
        \return    64-bit count value.
    */
    virtual Cycles GetSysTimerCount() const = 0;

    /*! \brief     Get system timer frequency.
        \note      ISR-safe.
        \return    Frequency (Hz).
    */
    virtual uint32_t GetSysTimerFrequency() const = 0;

    /*! \brief     Switch to a next task.
    */
    virtual void SwitchToNext() = 0;

    /*! \brief     Put calling process into a sleep state.
        \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
        \param[in] ticks: Time to sleep (ticks).
    */
    virtual void Sleep(Timeout ticks) = 0;

    /*! \brief     Put calling process into a sleep state until the specified timestamp.
        \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
        \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
        \param[in] timestamp: Absolute timestamp (ticks).
        \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
        \return    True if sleep succeeded, false otherwise.
    */
    virtual bool SleepUntil(Ticks timestamp) = 0;

    /*! \brief     Process one tick.
        \note      Normally system tick is processed by the platform driver implementation.
                   In case system tick handler is used by the application and should not be implemented
                   by the driver then disable driver's handler in stk_config.h like this:
                   \code
                   #define STK_SYSTICK_HANDLER _STK_SYSTICK_HANDLER_DISABLE
                   \endcode
                   and then call ProcessTick() from your custom tick handler.
    */
    virtual void ProcessTick() = 0;

    /*! \brief     Cause a hard fault of the system.
        \note      Normally called by the Kernel when one of the scheduled tasks missed its deadline (see stk::KERNEL_HRT).
    */
    virtual void ProcessHardFault() = 0;

    /*! \brief     Set platform event overrider.
        \note      Must be set prior call to IKernel::Start.
        \param[in] overrider: Platform event overrider.
    */
    virtual void SetEventOverrider(IEventOverrider *overrider) = 0;

    /*! \brief     Get caller's Stack Pointer (SP).
        \note      Valid for a Thread process only.
        \return    Current value of the Stack Pointer (SP) of the calling process.
    */
    virtual Word GetCallerSP() const = 0;

    /*! \brief     Get thread Id.
        \return    Thread Id.
        \warning   ISR-safe.
        \see       TID_ISR_N, TID_NONE, IsIsrTid
    */
    virtual TId GetTid() const = 0;

    /*! \brief     Suspend scheduling.
        \return    Number of ticks available for the suspension period, as determined by the
                   nearest pending wake-up. The caller may use this value to program a hardware
                   timer for the tickless sleep interval.
        \note      ISR-safe. Pair with Resume().
    */
    virtual Timeout Suspend() = 0;

    /*! \brief     Resume scheduling after a prior Suspend() call.
        \param[in] elapsed_ticks: Number of ticks that elapsed during the suspended period.
                   The kernel uses this value to advance internal time counters.
        \note      ISR-safe.
    */
    virtual void Resume(Timeout elapsed_ticks) = 0;
};

/*! \class ITaskSwitchStrategy
    \brief Interface for a task switching strategy implementation.
    \note  Combines the Strategy and Iterator design patterns.
           - Strategy: concrete subclasses encapsulate the scheduling policy (round-robin, EDF,
             rate-monotonic, weighted round-robin, etc.) independently of the kernel.
           - Iterator: GetFirst() and GetNext() expose a stateful forward-iterator over the
             runnable task set. The cursor is owned by the concrete implementation and is
             not exposed directly, iteration is therefore not re-entrant.

    Implementation must declare the following compile-time constants for reporting
    its capabilities to the kernel (place inside EConfig enum):

    Example:
    \code
    enum EConfig
    {
        WEIGHT_API               = 0, // (1) if strategy needs Weight API of the kernel task, (0) otherwise (see \a IKernelTask and Weight API functions)
        SLEEP_EVENT_API          = 0, // (1) if strategy needs Sleep API events generated by the kernel, (0) otherwise (see \a ITaskSwitchStrategy::OnTaskSleep, \a ITaskSwitchStrategy::OnTaskWake)
        DEADLINE_MISSED_API      = 0  // (1) if strategy implements OnTaskDeadlineMissed() and can absorb HRT deadline overruns, (0) otherwise (see \a ITaskSwitchStrategy::OnTaskDeadlineMissed)
        PRIORITY_INHERITANCE_API = 0  // (1) if strategy expects OnTaskWeightChange() events to support priority inheritance requests
    };
    \endcode
*/
class ITaskSwitchStrategy
{
public:
    /*! \brief     Add task.
        \note      Kernel tasks are added by the concrete implementation of IKernel.
        \param[in] task: Pointer to the task to add.
    */
    virtual void AddTask(IKernelTask *task) = 0;

    /*! \brief     Remove task.
        \note      Kernel tasks are removed from the concrete implementation of IKernel.
        \param[in] task: Pointer to the task to remove.
    */
    virtual void RemoveTask(IKernelTask *task) = 0;

    /*! \brief     Get first task.
        \return    Pointer to the first task in the managed set, or \c NULL if no tasks have been added.
    */
    virtual IKernelTask *GetFirst() const = 0;

    /*! \brief     Advance the internal iterator and return the next runnable task.
        \return    Pointer to the next active task to schedule, or \c NULL if no runnable tasks are available
                   (in which case the kernel transitions to \a FSM_STATE_SLEEPING).
        \note      Implementations maintain an internal cursor. This method is not re-entrant, concurrent
                   calls from multiple contexts are not supported.
    */
    virtual IKernelTask *GetNext() = 0;

    /*! \brief     Get number of tasks currently managed by this strategy.
        \return    Total number of tasks in the set (runnable and idle).
    */
    virtual size_t GetSize() const = 0;

    /*! \brief     Notification that a task has entered sleep/blocked state.
        \param[in] task: Pointer to the sleeping task,
        \note      Sleep API. Implementations shall remove the task from runnable set here.
    */
    virtual void OnTaskSleep(IKernelTask *task) = 0;

    /*! \brief     Notification that a task is becoming runnable again.
        \param[in] task: Pointer to the waking task
        \note      Sleep API. Implementations shall re-insert the task into the runnable set here.
    */
    virtual void OnTaskWake(IKernelTask *task) = 0;

    /*! \brief     Notification that a task has exceeded its HRT deadline; returns whether
                   the strategy can recover without a hard fault.
        \param[in] task: The task whose deadline was missed. Must not be \c nullptr.
        \return    \c true  — the strategy has absorbed the overrun (e.g. by escalating its
                              scheduling mode): the kernel must \e not call
                              HrtHardFailDeadline() for this tick.
                   \c false — the strategy cannot recover: the kernel must call
                              HrtHardFailDeadline() as normal.
        \note      Budget Overrun API. Called by the kernel from UpdateTaskState() within a
                   tick, after GetNext() has already been called for that tick. Only invoked
                   when \c DEADLINE_MISSED_API == 1 in the concrete strategy's \c EConfig.
                   Strategies that set \c DEADLINE_MISSED_API = 0 do not need to implement
                   this method; the kernel will not call it and will proceed directly to
                   HrtHardFailDeadline().
        \note      Returning \c true carries no implicit side-effects on task sleep state or
                   duration counters — normal tick-driven scheduling remains responsible for
                   those. This call only communicates "do not hard-fault this tick."
        \note      The base implementation returns \c false (unrecoverable), which is the
                   correct default for strategies that do not implement overrun recovery.
    */
    virtual bool OnTaskDeadlineMissed(IKernelTask *task)
    {
        STK_UNUSED(task);
        return false;
    }

    /*! \brief     Notification that a runnable task's scheduling weight has changed.
        \param[in] task: The task whose weight was just updated via SetWeight().
        \param[in] old_weight: The previous weight of this task (required to remove it from
                   the priority list belonging to that weight).
        \note      Called only for tasks that are currently in the runnable set
                   (not sleeping). The strategy must relink the task to reflect
                   its new weight. For strategies with WEIGHT_API = 0 this is a
                   no-op. For SwitchStrategyFixedPriority it moves the task from
                   its old priority-level list to the new one and updates
                   m_ready_bitmap.
        \note      Called from within a ScopedCriticalSection.
    */
    virtual void OnTaskWeightChange(IKernelTask *task, Weight old_weight)
    {
        STK_UNUSED(task);
        STK_UNUSED(old_weight);
    }
};

/*! \class IKernel
    \brief Interface for the implementation of the kernel of the scheduler. It supports Soft
           and Hard Real-Time modes.
    \note  Mediator design pattern.
*/
class IKernel
{
public:
    /*! \enum    EState
        \brief   Kernel state.
    */
    enum EState : uint8_t
    {
        STATE_INACTIVE = 0, //!< Not ready, IKernel::Initialize() must be called.
        STATE_READY,        //!< Ready to start, IKernel::Start() must be called.
        STATE_RUNNING,      //!< Initialized and running, IKernel::Start() was called successfully.
        STATE_SUSPENDED     //!< Scheduling is suspended with IKernelService::Suspend().
    };

    /*! \brief     Initialize kernel.
        \param[in] resolution_us: Resolution of the system tick (SysTick) timer in microseconds.
                   Defaults to PERIODICITY_DEFAULT (1000 µs = 1 ms).
        \note      Must be called before AddTask() and Start().
        \note      If running on an STM32 device with HAL driver or on QEMU, do not change the default
                   resolution (PERIODICITY_DEFAULT). STM32's HAL expects 1 millisecond resolution and
                   QEMU does not have enough resolution on Windows to operate correctly at sub-millisecond
                   resolution.
        \note      Kernel must be in \a STATE_INACTIVE state.
    */
    virtual void Initialize(uint32_t resolution_us = PERIODICITY_DEFAULT) = 0;

    /*! \brief     Add user task.
        \note      This function is for Soft Real-Time modes only (i.e. \c KERNEL_HRT must not be set in
                   the kernel mode flags).
        \param[in] user_task: Pointer to the user task to add.
    */
    virtual void AddTask(ITask *user_task) = 0;

    /*! \brief     Add user task.
        \note      This function is for Hard Real-time mode only, e.g. stk::KERNEL_HRT is used as parameter.
        \param[in] user_task: Pointer to the user task to add.
        \param[in] periodicity_tc: Periodicity time at which task is scheduled (ticks).
        \param[in] deadline_tc: Deadline time within which a task must complete its work (ticks).
        \param[in] start_delay_tc: Initial start delay for the task (ticks).
    */
    virtual void AddTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc) = 0;

    /*! \brief     Remove a previously added task from the kernel before Start().
        \param[in] user_task: User task to remove. Must not be \c nullptr.
        \note      Only valid before Start() (i.e. while the kernel is not running).
                   To remove tasks after Start() the task should return from its Run function
                   (in KERNEL_DYNAMIC mode the slot is freed automatically on the next tick).
        \warning   Must only be called when kernel is not running, otherwise call ScheduleTaskRemoval.
        \warning   KERNEL_DYNAMIC mode only. Asserts if called in KERNEL_STATIC or KERNEL_HRT mode,
                   or if called after Start().
        \see       ScheduleTaskRemoval
    */
    virtual void RemoveTask(ITask *user_task) = 0;

    /*! \brief     Schedule task removal from scheduling (exit).
        \param[in] user_task: User task to remove. Must not be \c nullptr.
        \warning   KERNEL_DYNAMIC mode only. Asserts if called in KERNEL_STATIC or KERNEL_HRT mode,
                   or if called after Start(). Use RemoveTask to remove task if kernel is not running.
        \see       RemoveTask
    */
    virtual void ScheduleTaskRemoval(ITask *user_task) = 0;

    /*! \brief      Suspend task.
        \param[in]  user_task: Pointer to the user task to suspend.
        \param[out] suspended: Set to true if task is suspended.
        \note       hw::CriticalSection must not be active otherwise a deadlock will
                    happen if task is suspending self.
    */
    virtual void SuspendTask(ITask *user_task, bool &suspended) = 0;

    /*! \brief     Resume task.
        \param[in] user_task: Pointer to the user task to resume.
    */
    virtual void ResumeTask(ITask *user_task) = 0;

    /*! \brief     Enumerate kernel tasks.
        \param[in,out] tasks: Pointer to the array for IKernelTask pointers.
        \param[in] max_size: Max size of the provided array.
        \return    Number of tasks in the array.
        \warning   ISR-safe.
    */
    virtual size_t EnumerateKernelTasks(IKernelTask **tasks, size_t max_size) = 0;

    /*! \brief     Enumerate user tasks.
        \param[in,out] user_tasks: Pointer to the array for ITask pointers.
        \param[in] max_size: Max size of the provided array.
        \return    Number of tasks in the array.
        \warning   ISR-safe.
    */
    virtual size_t EnumerateTasks(ITask **user_tasks, size_t max_size) = 0;

    /*! \brief     Enumerate tasks, invoking a callback for each active task.
        \tparam    TMaxCount: Maximum number of tasks to enumerate.
                   Should match or exceed the kernel's task capacity.
                   Determines the size of the internal stack-allocated buffer
                   (TMaxCount * sizeof(ITask*) bytes on the stack).
        \tparam    TCallback: Callable type, deduced automatically.
                   Must satisfy: bool(ITask*)
        \param[in] callback: Callable invoked for each active task.
                   Return \c true to continue, \c false to stop early.
        \return    Number of tasks visited (up to \p TMaxCount).
        \warning   ISR-safe.

        Example:
        \code
        kernel.EnumerateTasks<_STK_KERNEL_TASKS_COUNT>([](ITask *t) {
            Log(t->GetTraceName());
            return true; // continue
        });
        \endcode
    */
    template <size_t TMaxCount, typename TCallback>
    size_t EnumerateTasksT(TCallback &&callback)
    {
        STK_STATIC_ASSERT(TMaxCount > 0);

        ITask *tasks[TMaxCount] = {};
        size_t i = 0, count = EnumerateTasks(tasks, TMaxCount);

        while (i < count)
        {
            if (!callback(tasks[i++]))
                break;
        }

        return i;
    }

    /*! \brief     Start kernel scheduling.
        \note      This function never returns. Must be called after Initialize() and AddTask().
        \note      Kernel must be in \a STATE_READY state.
    */
    virtual void Start() = 0;

    /*! \brief     Get a snapshot of the kernel state.
        \return    Kernel state.
        \see       EState
    */
    virtual EState GetState() const = 0;

    /*! \brief     Get platform driver instance.
        \return    Pointer to the IPlatform concrete class instance.
    */
    virtual IPlatform *GetPlatform() = 0;

    /*! \brief     Get switch strategy instance.
        \return    Pointer to the ITaskSwitchStrategy concrete class instance.
    */
    virtual ITaskSwitchStrategy *GetSwitchStrategy() = 0;
};

/*! \class IKernelService
    \brief Interface for the kernel services exposed to the user processes during
           run-time when Kernel started scheduling the processes.
    \note  State design pattern: this interface represents the kernel's active running state.
           It becomes valid only after IKernel::Start() is called. Before that point the kernel
           is in an idle/unstarted state and this interface must not be used.
           Obtain the CPU-local instance via IKernelService::GetInstance().
*/
class IKernelService
{
public:
    /*! \brief     Get CPU-local instance of the kernel service.
    */
    static IKernelService *GetInstance();

    /*! \brief     Get thread Id of the currently running task.
        \return    Thread Id.
        \warning   ISR-safe.
        \see       TID_ISR_N, TID_NONE, IsIsrTid
    */
    virtual TId GetTid() const = 0;

    /*! \brief     Get number of ticks elapsed since kernel start.
        \return    Ticks.
        \note      ISR-safe.
    */
    virtual Ticks GetTicks() const = 0;

    /*! \brief     Get number of microseconds in one tick.
        \note      Tick is a periodicity of the system timer expressed in microseconds.
        \note      ISR-safe.
        \return    Microseconds in one tick.
    */
    virtual uint32_t GetTickResolution() const = 0;

    /*! \brief     Get system timer count value.
        \note      ISR-safe.
        \return    64-bit count value.
    */
    virtual Cycles GetSysTimerCount() const = 0;

    /*! \brief     Get system timer frequency.
        \note      ISR-safe.
        \return    Frequency (Hz).
    */
    virtual uint32_t GetSysTimerFrequency() const = 0;

    /*! \brief     Delay calling process.
        \note      Unlike Sleep this function delays code execution by spinning in a loop until deadline expiry.
        \note      Use with care in HRT mode to avoid missed deadline (see stk::KERNEL_HRT, ITask::OnDeadlineMissed).
        \param[in] ticks: Delay time (ticks).
        \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
        \see       Delay
    */
    virtual void Delay(Timeout ticks) = 0;

    /*! \brief     Put calling process into a sleep state.
        \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
        \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
        \param[in] ticks: Sleep time (ticks).
        \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
    */
    virtual void Sleep(Timeout ticks) = 0;

    /*! \brief     Put calling process into a sleep state until the specified timestamp.
        \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
        \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
        \param[in] timestamp: Absolute timestamp (ticks).
        \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
        \return    True if sleep succeeded, false otherwise.
    */
    virtual bool SleepUntil(Ticks timestamp) = 0;

    /*! \brief     Cancel sleep of the task.
        \param[in] task_id: Id of the task.
        \note      No-op if task was not in a sleeping state.
        \note      ISR-safe.
    */
    virtual void SleepCancel(TId task_id) = 0;

    /*! \brief     Notify scheduler to switch to the next task (yield).
        \note      A cooperation mechanism in HRT mode (see stk::KERNEL_HRT).
        \warning   ISR-unsafe.
    */
    virtual void SwitchToNext() = 0;

    /*! \brief     Put calling process into a waiting state until synchronization object is signaled or timeout occurs.
        \note      This function implements core blocking logic using the Monitor pattern to ensure atomicity between state check and suspension.
        \note      The kernel automatically unlocks the provided \a mutex before the task is suspended and re-locks it before this function returns.
        \param[in] sobj: Synchronization object to wait on.
        \param[in] mutex: Mutex protecting the state of the synchronization object.
        \param[in] timeout: Maximum wait time (ticks). Use \c WAIT_INFINITE to block indefinitely, use \c NO_WAIT to poll without blocking.
        \return    Pointer to the wait object representing this wait operation (always non-NULL).
                   The caller must check IWaitObject::IsTimeout() after this function returns to determine whether
                   the wake was caused by a signal or by timeout expiry. The returned pointer is valid until
                   the calling task re-enters a wait or the wait object is explicitly released by the kernel.
                   The return value is guaranteed non nullptr and points to a valid IWaitObject.
        \warning   ISR-unsafe.
    */
    virtual IWaitObject *Wait(ISyncObject *sobj, IMutex *mutex, Timeout timeout) = 0;

    /*! \brief     Suspend scheduling.
        \return    Number of ticks available for the suspension period, as determined by the
                   nearest pending wake-up. The caller may program a hardware timer with
                   this value to avoid unnecessary wakeups (tickless idle).
        \note      ISR-safe. Pair with Resume().
        \see       IKernel::EState::STATE_SUSPENDED
    */
    virtual Timeout Suspend() = 0;

    /*! \brief     Resume scheduling after a prior Suspend() call.
        \param[in] elapsed_ticks: Number of ticks that elapsed during the suspended period.
                   The kernel uses this value to advance internal time counters and
                   wake tasks whose sleep deadlines have expired.
        \note      ISR-safe.
        \see       IKernel::EState::STATE_SUSPENDED
    */
    virtual void Resume(Timeout elapsed_ticks) = 0;

    /*! \brief     Inherit weight for the task.
        \param[in] tid: Task id.
        \param[in] weight: New weight, shall be higher than task's current weight (see \a ITask::GetWeight).
        \note      ISR-safe.
    */
    virtual void InheritWeight(TId tid, Weight weight) = 0;

    /*! \brief     Restore weight of the task to the original value.
        \param[in] tid: Task id.
        \param[in] sobj: Optional, if provided than weight will be restored to the highest weight of the
                   task in the wait list, otherwise to the original value.
        \note      ISR-safe.
    */
    virtual void RestoreWeight(TId tid, ISyncObject *sobj = nullptr) = 0;
};

} // namespace stk

#endif /* STK_COMMON_H_ */
