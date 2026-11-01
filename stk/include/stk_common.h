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
class ISyncObject;
namespace tz { namespace nsec { namespace util {
    class CmseISyncObjectWrapper;
}}}

/*! \enum    EAccessMode
    \brief   Hardware access modes by the task.
    \note    Can be used as a bit-field.
*/
enum EAccessMode : uint32_t
{
    ACCESS_USER       = 0,        //!< Unprivileged access mode (access to some hardware is restricted, see CPU manual for details). If compiled as ARM TrustZone, this is also a Non-Secure mode.
    ACCESS_PRIVILEGED = (1 << 0), //!< Privileged access mode (access to hardware is fully unrestricted).
    ACCESS_SECURE     = (1 << 1)  //!< Secure access mode (ARM TrustZone, Secure binary)
};

/*! \enum  EKernelMode
    \brief Kernel operating mode.
*/
enum EKernelMode : uint8_t
{
    KERNEL_STATIC    = (1 << 0), //!< All tasks are static and can not exit.
    KERNEL_DYNAMIC   = (1 << 1), //!< Tasks can be added or removed and therefore exit when done.
    KERNEL_HRT       = (1 << 2), //!< Hard Real-Time (HRT) behavior (tasks are scheduled periodically and have an execution deadline, whole system is failed when task's deadline is failed).
    KERNEL_SYNC      = (1 << 3), //!< Synchronization support (see \a Event).
    KERNEL_TICKLESS  = (1 << 4), //!< Tickless mode. To use this mode STK_TICKLESS_IDLE must be defined to 1 in stk_config.h.
    KERNEL_TIMESLICE = (1 << 5), //!< Time-slicing: running task is rotated behind its same-priority peers when its quantum (see ITask::GetTimeSlice) expires. Requires a strategy with TIMESLICE_API = 1, incompatible with KERNEL_HRT.
    KERNEL_TELEMETRY = (1 << 6)  //!< Enable introspection functions for diagnostics / telemetry (see IKernel::GetTaskState).
};

/*! \enum  EKernelPanicId
    \brief Identifies the source of a kernel panic.
*/
enum EKernelPanicId : uint8_t
{
    KERNEL_PANIC_NONE                = 0U,  //!< Panic is absent (no fault).
    KERNEL_PANIC_SPINLOCK_DEADLOCK   = 1U,  //!< Spin-lock timeout expired: lock owner never released.
    KERNEL_PANIC_STACK_CORRUPT       = 2U,  //!< Stack integrity check failed.
    KERNEL_PANIC_ASSERT              = 3U,  //!< Internal assertion failed (maps from STK_ASSERT).
    KERNEL_PANIC_HRT_HARD_FAULT      = 4U,  //!< Kernel running in KERNEL_HRT mode reported deadline failure of the task.
    KERNEL_PANIC_CPU_EXCEPTION       = 5U,  //!< CPU reported an exception and halted execution.
    KERNEL_PANIC_CS_NESTING_OVERFLOW = 6U,  //!< Critical section nesting limit exceeded: violation of STK_CS_NESTINGS_MAX.
    KERNEL_PANIC_UNKNOWN_SVC         = 7U,  //!< Unknown service command received by SVC handler.
    KERNEL_PANIC_BAD_STATE           = 8U,  //!< Kernel entered unexpected (bad) state.
    KERNEL_PANIC_BAD_MODE            = 9U,  //!< Kernel is in bad/unsupported mode for the current operation.
    KERNEL_PANIC_BAD_STACK_TYPE      = 10U, //!< Stack type is unknown.
    KERNEL_PANIC_NS_ACCESS           = 11U, //!< Non-secure access to protected resource.
    KERNEL_PANIC_BAD_MEMORY_REGION   = 12U, //!< Bad memory region (ARM TrustZone: provided memory region overlaps with another one).
    KERNEL_PANIC_SYNC_DEADLOCK       = 13U  //!< Deadlock caused by a synchronization primitive: a task tried to acquire a non-recursive primitive (e.g. sync::FastMutex) that it already owns and would block forever. Not used for spin-lock timeouts, see KERNEL_PANIC_SPINLOCK_DEADLOCK.
};

/*! \enum  ETaskState
    \brief Kernel-side state of a task, see IKernel::GetTaskState().
*/
enum ETaskState : uint8_t
{
    TASK_STATE_REMOVED = 0, //!< The task is not bound to a kernel slot (never added, or already removed).
    TASK_STATE_READY,       //!< Runnable: running, or waiting for the CPU.
    TASK_STATE_SLEEPING,    //!< Sleeping via Sleep()/SleepUntil(), with a finite or an infinite timeout.
    TASK_STATE_WAITING,     //!< Blocked on a synchronization object (IKernelService::Wait). KERNEL_SYNC mode only.
    TASK_STATE_SUSPENDED,   //!< Suspended by IKernel::SuspendTask().
    TASK_STATE_EXITING      //!< Run() has returned, the slot is released on the next tick (KERNEL_DYNAMIC mode only).
};

/*! \enum  EStackType
    \brief Stack type.
    \see   IPlatform::InitStack
*/
enum EStackType : uint8_t
{
    STACK_USER_TASK = 0, //!< Stack of the user task.
    STACK_SLEEP_TRAP,    //!< Stack of the Sleep trap.
    STACK_EXIT_TRAP      //!< Stack of the Exit trap.
};

/*! \enum  EConsts
    \brief Constants.
*/
enum EConsts : uint32_t
{
    PERIODICITY_MAX      = 99000,             //!< Maximum periodicity (microseconds), 99 milliseconds (note: this value is the highest working on a real hardware and QEMU).
    PERIODICITY_DEFAULT  = 1000,              //!< Default periodicity (microseconds), 1 millisecond.
    STACK_SIZE_MIN       = STK_STACK_SIZE_MIN //!< Minimum stack size in elements of Word. Used as a lower bound for all stack allocations (user task, sleep trap, exit trap). See: StackMemoryDef, StackMemoryWrapper.
};

/*! \enum  ESystemTaskId
    \brief System task id.
*/
enum ESystemTaskId : uint32_t
{
    SYS_TASK_ID_SLEEP = 0xFFFFFFFF, //!< Sleep trap.
    SYS_TASK_ID_EXIT  = 0xFFFFFFFE  //!< Exit trap.
};

/*! \enum  ETraceEventId
    \brief Trace event identifiers for tracing task suspension and resume with debugging tools (e.g. SEGGER SystemView).
    \note  Values are offset by 1000 to avoid collisions with the host tool's built-in system event range (0–999).
*/
enum ETraceEventId : uint32_t
{
    TRACE_EVENT_UNKNOWN = 0,        //!< Unknown / uninitialized trace event.
    TRACE_EVENT_SWITCH  = 1000 + 0, //!< Task blocked by the context switch.
    TRACE_EVENT_SLEEP   = 1000 + 1, //!< Task entered a sleep state.
    TRACE_EVENT_WAIT    = 1000 + 2, //!< Task suspended on a sync object.
    TRACE_EVENT_SUSPEND = 1000 + 3, //!< Task suspended via Suspend.
};

/*! \enum  EWaitResult
    \brief Wait result (see \c IKernelService::Wait).
*/
enum EWaitResult : int8_t
{
    WAIT_RESULT_FAIL     = -1, //!< IKernelService::Wait returned with error without waiting.
    WAIT_RESULT_SIGNAL   = 0,  //!< The wake was caused by a signal.
    WAIT_RESULT_TIMEOUT  = 1,  //!< The wake was caused by a timeout expiry.
    WAIT_RESULT_CANCELED = 2   //!< The wait was cancelled by another task via IKernel::CancelTaskWait(), independent of signal/timeout.
};

/*! \enum  EAddResult
    \brief Result of IKernel::AddTask() (both the Soft Real-Time and the Hard Real-Time overloads).
*/
enum EAddResult : int8_t
{
    ADD_FAIL_INVALID   = -3, //!< Invalid call: task is \c nullptr, kernel is not initialized, or adding is not allowed in the current kernel mode/state (KERNEL_HRT, or KERNEL_STATIC after Start()).
    ADD_FAIL_DUPLICATE = -2, //!< The task (or its stack) is already added to the kernel. Application design failure.
    ADD_FAIL_POOL_FULL = -1, //!< No free kernel slot left (TASKS_MAX reached). In KERNEL_DYNAMIC mode retry later, once a task has exited.
    ADD_OK             = 0   //!< Task was added.
};

/*! \enum  ELockResult
    \brief Result of IKernel::SetSchedulerLock(), IKernelService::LockScheduler(), UnlockScheduler() and
           RestoreSchedulerLock(): the scheduler lock state.
*/
enum ELockResult : int8_t
{
    LOCK_FAIL_ISR     = -2, //!< Called from an ISR.
    LOCK_FAIL         = -1, //!< Kernel is not running, is suspended, or is in KERNEL_HRT mode.
    LOCK_WAS_UNLOCKED = 0,  //!< LockScheduler/UnlockScheduler: previous state was unlocked. RestoreSchedulerLock: now unlocked.
    LOCK_WAS_LOCKED   = 1   //!< LockScheduler/UnlockScheduler: previous state was locked. RestoreSchedulerLock: now locked.
};

/*! \enum  ESuspendResult
    \brief Result of IKernel::SuspendTask().
*/
enum ESuspendResult : int8_t
{
    SUSPEND_FAIL     = -1, //!< Task is not bound to the kernel, or is exiting.
    SUSPEND_DONE     = 0,  //!< Task was runnable and is suspended now.
    SUSPEND_DEFERRED = 1,  //!< Task is sleeping/waiting: it will be suspended when the sleep/wait ends (wait result is preserved).
    SUSPEND_ALREADY  = 2   //!< Task is already suspended or already has a deferred suspension pending.
};

/*! \enum  EResumeResult
    \brief Result of IKernel::ResumeTask().
*/
enum EResumeResult : int8_t
{
    RESUME_FAIL          = -1, //!< Task is not bound to the kernel, or is exiting.
    RESUME_NOT_SUSPENDED = 0,  //!< Task was not suspended by SuspendTask() (no-op).
    RESUME_DONE          = 1,  //!< Suspended task has been resumed.
    RESUME_LIFTED        = 2   //!< Deferred suspension was cancelled before it took effect.
};

/*! \enum  EHwException
    \brief Hardware exception id (see \c IPlatform::IEventOverrider::OnException).
*/
enum EHwException : int8_t
{
    HW_EXCEPT_FATAL     = 0, //!< HardFault on ARM / Unhandled Fatal Trap on RISC-V.
    HW_EXCEPT_MEMACCESS = 1  //!< MemManage on ARM / Page Fault or PMP violation on RISC-V.
};

/*! \typedef Word
    \brief   Native processor word type.
    \details Represents natural data width of the CPU (matching uintptr_t).
             Used for stack allocation, register storage, pointer value storage,
             and memory alignment to ensure atomic access, optimal performance,
             and hardware compatibility.
*/
typedef uintptr_t Word;

/*! \typedef TId
    \brief   Task (thread) id.
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
static constexpr Weight NO_WEIGHT = -1;

/*! \var     DEFAULT_WEIGHT
    \brief   Weight value: default weight of value (1) (see \c SwitchStrategySmoothWeightedRoundRobin).
*/
static constexpr Weight DEFAULT_WEIGHT = 1;

/*! \var     NO_TIME_SLICE
    \brief   Time-slice quantum value: no time-slicing for the task (it runs until it blocks, yields or is preempted).
    \see     ITask::GetTimeSlice, stk::KERNEL_TIMESLICE
*/
static constexpr Timeout NO_TIME_SLICE = 0;

/*! \brief     Test whether a task identifier represents an ISR context.

    Returns \c true if \a tid was produced by GetTid() called from an
    interrupt service routine, i.e. its upper 20 bits match \c TID_ISR_N.
    Use this predicate instead of comparing against \c TID_ISR_N directly.

    \param[in] id: Task identifier to test.
    \return    \c true if \a tid encodes an ISR context, \c false otherwise.
    \note      ISR-safe (bitmask arithmetic only, no kernel calls).
    \see       TID_ISR_N
*/
static constexpr bool IsIsrTid(TId id) { return ((id & TID_ISR_N) == TID_ISR_N); }

/*! \class ArrayView
    \brief Lightweight, non-owning view over a contiguous sequence of elements.
    \note  Provides a safe encapsulation over raw pointers without copying the underlying data.
           Includes binary serialization methods for compact state transfer on 32-bit platforms.

    Usage example:
    \code
    stk::ITask *tasks[TASKS_MAX];
    ArrayView<stk::ITask *> view(tasks, TASKS_MAX);
    \endcode
*/
template <typename T> class ArrayView
{
public:
    typedef T value_type;

    /*! \brief Construct an empty ArrayView with a null pointer and zero size.
    */
    ArrayView() : m_ptr(nullptr), m_size(0U)
    {}

    /*! \brief     Construct an ArrayView from a raw pointer and size.
        \param[in] ptr Pointer to the first element of the contiguous memory block.
        \param[in] size Number of elements available in the memory block.
    */
    ArrayView(T *ptr, size_t size) : m_ptr(ptr), m_size(size)
    {}
    
    /*! \brief     Subscript operator for element access.
        \param[in] index: Element index to access.
        \return    Reference to the element at the specified index.
        \warning   Triggers STK_ASSERT if index is out of bounds in a Debug build.
    */
    T &operator[](size_t index) const
    {
        STK_ASSERT(index < m_size); // enforces MISRA Rule 5-0-16
        return UncheckedAt(index);  // MISRA Rule 5-0-15 deviation centralized
    }
    
    /*! \brief    Get pointer to the beginning of elements in the view.
        \return   Pointer to the elements of array.
    */
    T *GetPtr() { return m_ptr; }

    /*! \brief    Get constant pointer to the beginning of elements in the view.
        \return   Constant pointer to the elements of array.
    */
    const T *GetPtr() const { return m_ptr; }

    /*! \brief    Get number of elements in the view.
        \return   The size of the array view.
    */
    size_t GetSize() const { return m_size; }

private:
    /*! \brief  Deviation MISRA-CPP-2008-5-0-15
        \reason Array indexing on a base pointer is required for a dynamic-size
                non-owning view. The pointer and size are externally supplied.
                Bounds are enforced by the public API (STK_ASSERT / operator[]).
    */
    __stk_forceinline T &UncheckedAt(size_t index) const
    {
        return m_ptr[index]; // deviation: indexing non-array pointer
    }

    T     *m_ptr;  //!< Pointer to the underlying memory block.
    size_t m_size; //!< Total number of elements in the view.
};

/*! \struct TaskStateInfo
    \brief  Snapshot of the kernel-side state of a task, filled in by IKernel::GetTaskState().
*/
struct TaskStateInfo
{
    ETaskState   state;     //!< Task state.
    ISyncObject *wait_obj;  //!< Object the task is blocked on for TASK_STATE_WAITING, \c nullptr otherwise.
                            //!< Identity only: the pointer is not guaranteed to stay valid after the call.
    Timeout      remaining; //!< Ticks left until the sleep/wait expires for TASK_STATE_SLEEPING and
                            //!< TASK_STATE_WAITING, \c WAIT_INFINITE if unbounded or not applicable.
};

/*! \class   MpuRegion
    \brief   MPU region descriptor.
    \warning Referenced by driver, member offsets must not change.
*/
#if STK_MPU
struct MpuRegion
{
    Word addr; //!< Base address (ARMv7-M/ARMv8-M MPU: MPU->RBAR value).
    Word attr; //!< Address attributes (ARMv7-M: MPU->RASR value; ARMv8-M: MPU->RLAR value).
};
#endif

/*! \class   TaskMpu
    \brief   MPU descriptor of the task.
    \warning Fields marked with 'Offset' have static position in the structure and can't move (driver dependent).
*/
#if STK_MPU && STK_MPU_STACK_GUARD
template <uint8_t REGIONS_COUNT>
struct TaskMpuT
{
    /*! \var   NUM_REGIONS
        \brief Number of MPU regions per task.
        \note  Configurable via \a STK_MPU_TASK_REGIONS (2, 4, 6, 8, 12 or 16). Last available slot is
               always the automatic stack guard; the other remaining NUM_REGIONS-1 slots are
               available to the application via \a ITask::GetMpuRegions().
        \see   Stack, STK_MPU_TASK_REGIONS
    */
    static constexpr uint8_t NUM_REGIONS = REGIONS_COUNT;

    MpuRegion region[NUM_REGIONS]; //!< Array of MPU regions of size NUM_REGIONS.
};
//! Per-task MPU region. If TrustZone then relates to MPU on Secure side.
typedef TaskMpuT<STK_MPU_TASK_REGIONS> TaskMpu;
//! Per-task MPU region related to MPU_NS on TrustZone's Non-Secure side.
typedef TaskMpuT<STK_MPU_TASK_REGIONS_NS> TaskMpuNs;
#endif // STK_MPU && STK_MPU_STACK_GUARD

/*! \typedef MpuRegions
    \brief   List of MPU regions.
    \see     ITask::GetMpuRegions, IPlatform::IEventOverrider::OnConfigureMpu
*/
#if STK_MPU
typedef ArrayView<const struct MpuRegionConfig> MpuRegionList;
#endif

/*! \struct  MpuConfig
    \brief   Aggregated hardware MPU setup configuration and state descriptor.
    \details Wraps a static fixed-capacity collection of MPU region descriptors (`MpuRegionList`)
             along with an overall enable flag controlling hardware MPU initialization and activation.
    \see     ITask::GetMpuRegions, IPlatform::IEventOverrider::OnConfigureMpu, MpuRegionList
*/
#if STK_MPU
struct MpuConfig
{
    /*! \var     DEFAULT_MODE
        \brief   Default mode.
    */
    static constexpr uint32_t MPU_OFF = 0U;

    /*! \brief Default constructor initializing an empty, disabled MPU configuration.
    */
    MpuConfig() : regions(), mode(MPU_OFF)
    {}

    /*! \brief     Parameterized constructor initializing MPU region settings and enable state.
        \param[in] regions_: Initial collection of MPU region descriptors.
        \param[in] enable_:  Activation state flag for the hardware MPU subsystem.
    */
    MpuConfig(const MpuRegionList &regions_, uint32_t mode_) : regions(regions_), mode(mode_)
    {}

    MpuRegionList regions; //!< Fixed-capacity collection of configured MPU region descriptors.
    uint32_t      mode;    //!< Mode flags (hardware-dependent, see hw::mpu::EMpuConfigFlags).
};
#endif

/*! \class   Stack
    \brief   Stack descriptor.
    \warning Fields marked with 'Offset' have static position in the structure and can't move (driver dependent).
*/
struct Stack
{
    Word      SP;          //!< Offset 0: Stack Pointer (SP) register (note: must always be at offset 0).
    uint32_t  access_mode; //!< Bitfield with hardware access mode of the task (see \a EAccessMode).
#if STK_MPU && STK_MPU_STACK_GUARD
    TaskMpu   mpu;         //!< MPU regions of the task (if TrustZone: always Secure side for any task).
    #ifdef _STK_CORTEX_M_TRUSTZONE
    TaskMpuNs mpu_ns;      //!< MPU regions of Non-Secure side task (only for TrustZone).
    #endif
#endif
#if STK_TLS && !STK_TLS_PREFER_REGISTER
    Word      tls;         //!< Thread-local storage if not using ARM Cortex-M R9 register for a fast inline access to TLS.
#endif
#if STK_STACK_NEEDS_TASK_ID
    TId       tid;         //!< Task id (see \a STK_SEGGER_SYSVIEW, \a STK_MPU_STACK_GUARD).
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
    virtual const Word *GetStack() const = 0;

    /*! \brief   Get number of elements of the stack memory array.
    */
    virtual size_t GetStackSize() const = 0;

    /*! \brief   Get available stack space.
        \return  Number of elements of the stack memory array remaining on the stack (computed via the
                 watermark pattern).
                 Returns 0 if the stack has been fully used or the watermark \a STK_STACK_MEMORY_FILLER
                 was overwritten.
        \warning Stack type: Bottom to Top (index[0]).
    */
    virtual size_t GetStackSpace() const
    {
        const ArrayView<const Word> stack(GetStack(), GetStackSize());
        const size_t total_size = stack.GetSize();
        size_t space = 0U;

        for (size_t i = 0U; i < total_size; ++i)
        {
            if (stack[i] == STK_STACK_MEMORY_FILLER)
            {
                space = i + 1U;
            }
            else
            {
                break; // terminate loop as soon as watermark ends
            }
        }

        return space;
    }

protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~IStackMemory() = default;
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

    /*! \enum  EState
        \brief State of the WaitObject.
    */
    enum EState : uint8_t
    {
        STATE_NONE = 0U,
        STATE_WAIT,      //!< Wait is active.
        STATE_SIGNALED,  //!< Wake signaled.
        STATE_TIMEOUT,   //!< Wait expired due to timeout rather than a Wake() signal.
        STATE_CANCELLED  //!< Wait was cancelled via Cancel() rather than Wake()/timeout.
    };

    /*! \brief     Get thread Id of the task owning .
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

    /*! \brief     Get current state.
        \return    State value.
        \see       EState
    */
    virtual EState GetState() const = 0;

    /*! \brief     Update wait object's waiting time.
        \param[in] elapsed_ticks: Number of ticks elapsed between this and previous calls, in case of
                   KERNEL_TICKLESS mode this value can be >1, for non-tickless mode it is always 1.
        \return    Returns \a true if update caused a timeout of the object, \a false otherwise.
    */
    virtual bool Tick(Timeout elapsed_ticks) = 0;

protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~IWaitObject() = default;
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
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~ITraceable() = default;
  
#if STK_SYNC_DEBUG_NAMES
    const char *m_trace_name; //!< name (debug/tracing only)
#endif
};

/*! \class ISyncObject
    \brief Synchronization object interface.

    Pure contract that the kernel programs against. Carries no wait-list storage of its
    own so that alternative backings (e.g. a cross-domain wrapper such as
    \a tz::nsec::util::CmseISyncObjectWrapper) can implement this interface without
    inheriting an unused/incompatible wait list. \see SyncObjectBase for the default,
    storage-owning implementation used by the built-in primitives.
*/
class ISyncObject : public util::DListEntry<ISyncObject, false>
{
    friend class IKernelService;
    friend class tz::nsec::util::CmseISyncObjectWrapper;

public:
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
        \note      Utility for AddWaitObject and ARM TrustZone support.
    */
    static inline void AddWaitObject(IWaitObject::ListHeadType &wlist, IWaitObject *wobj)
    {
        STK_ASSERT(wobj->GetHead() == nullptr);
        wlist.LinkBack(wobj);
    }

    /*! \brief     Called by kernel when a new task starts waiting on this event.
        \param[in] wobj: Wait object representing blocked task.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    virtual void AddWaitObject(IWaitObject *wobj) = 0;

    /*! \brief     Called by kernel when a waiting task is being removed (timeout expired, wait aborted, task terminated etc.).
        \param[in] wobj: Wait object to remove from the wait list.
        \note      Utility for AddWaitObject and ARM TrustZone support.
    */
    static inline void RemoveWaitObject(IWaitObject::ListHeadType &wlist, IWaitObject *wobj)
    {
        STK_ASSERT(wobj->GetHead() == &wlist);
        wlist.Unlink(wobj);
    }

    /*! \brief     Called by kernel when a waiting task is being removed (timeout expired, wait aborted, task terminated etc.).
        \param[in] wobj: Wait object to remove from the wait list.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    virtual void RemoveWaitObject(IWaitObject *wobj) = 0;

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

    /*! \brief     Wake the first task in the wait list (FIFO order).
        \note      The woken task is notified with timeout=false, indicating a successful signal
                   (not a timeout expiry).
        \note      Does nothing if no tasks are currently waiting.
        \note      Utility for AddWaitObject and ARM TrustZone support.
    */
    static inline void WakeOne(IWaitObject::ListHeadType &wlist)
    {
        IWaitObject *const obj = util::DListCast::ListEntryToParent<IWaitObject>(wlist.GetFirst());
        if (obj != nullptr)
        {
            obj->Wake(false);
        }
    }

    /*! \brief     Wake all tasks currently in the wait list.
        \note      Each woken task is notified with timeout=false, indicating a successful signal
                   (not a timeout expiry).
        \note      Does nothing if no tasks are currently waiting.
        \note      Utility for AddWaitObject and ARM TrustZone support.
    */
    static inline void WakeAll(IWaitObject::ListHeadType &wlist)
    {
        for (IWaitObject *obj = util::DListCast::ListEntryToParent<IWaitObject>(wlist.GetFirst());
             obj != nullptr;
             obj = util::DListCast::ListEntryToParent<IWaitObject>(wlist.GetFirst()))
        {
            obj->Wake(false);
        }
    }

    /*! \brief     Get list of tasks blocked on this object.
        \note      Read-only access for diagnostics / telemetry.
        \note      The result is a snapshot: call inside the critical section (see \a hw::CriticalSection,
                   \a sync::ScopedCrticalSection) if the list may change concurrently.
    */
    virtual const IWaitObject::ListHeadType &GetWaitList() const = 0;

    /*! \brief     Get id of the task which is first in the wait list (FIFO order), i.e. the one
                   that WakeOne() would wake next.
        \return    TId of the first waiter, or \a TID_NONE if no tasks are currently waiting.
        \note      Read-only introspection for diagnostics / telemetry.
        \note      The result is a snapshot: call inside the critical section (see \a hw::CriticalSection,
                   \a sync::ScopedCrticalSection) if the list may change concurrently.
        \see       GetWaitList
    */
    TId GetFirstWaiterTid() const
    {
        const IWaitObject *const obj = util::DListCast::ListEntryToParent<const IWaitObject>(
                GetWaitList().GetFirst());

        return ((obj != nullptr) ? obj->GetTid() : TID_NONE);
    }

protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~ISyncObject() = default;

    /*! \brief     Wake the first task in the wait list (FIFO order).
        \note      The woken task is notified with timeout=false, indicating a successful signal
                   (not a timeout expiry).
        \note      Does nothing if no tasks are currently waiting.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    virtual void WakeOne() = 0;

    /*! \brief     Wake all tasks currently in the wait list.
        \note      Each woken task is notified with timeout=false, indicating a successful signal
                   (not a timeout expiry).
        \note      Does nothing if no tasks are currently waiting.
        \note      Must be called inside the critical section (see \a hw::CriticalSection, \a sync::ScopedCrticalSection).
    */
    virtual void WakeAll() = 0;

    /*! \brief     Get list of tasks blocked on this object.
    */
    virtual IWaitObject::ListHeadType &GetMutableWaitList() = 0;
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
        explicit ScopedLock(IMutex &mutex) : m_mutex(mutex)
        {
            m_mutex.Lock();
        }
        ~ScopedLock()
        {
            m_mutex.Unlock();
        }

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
    
protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~IMutex() = default;
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

    /*! \brief     Get pointer to the stack memory.
        \note      Optional. ARM TrustZone only.
        \return    Pointer to the Secure stack memory.
    */
#if STK_TZ_SECURE
    virtual IStackMemory *GetSecureStackMemory() { return nullptr; }
#endif

    /*! \brief     Get up to (STK_MPU_TASK_REGIONS - 1) application-defined MPU regions for this task.
        \details   When \c STK_MPU_STACK_GUARD is enabled, each task owns \c STK_MPU_TASK_REGIONS
                   (2, 4, 6, 8, 12 or 16, see \ref STK_MPU_TASK_REGIONS) hardware MPU region slots.
                   Slot 0 is always the automatic stack guard, computed by the driver from the task's own
                   stack memory (see IStackMemory::GetStack/GetStackSize). This hook supplies the
                   remaining \c STK_MPU_TASK_REGIONS-1 slots, letting an application additionally
                   sandbox a task to e.g. a private data buffer, a specific peripheral block, or
                   a shared IPC region - on top of its stack guard.
        \return    Pointer to a constant \c MpuRegionList containing up to (STK_MPU_TASK_REGIONS - 1)
                   region descriptors, or \c nullptr if no extra task regions are configured.
                   Array element \c i is applied at task-relative region index \a i \c +1 (i.e. the slots following the stack guard).
        \note      Optional. Only consulted when \c STK_MPU_STACK_GUARD is enabled; a
                   no-op on platforms/builds without MPU stack-guard support.
        \note      Read once, when the task is bound via Kernel::AddTask() (through
                   IPlatform::InitStack()), not re-evaluated on every context switch.
                   The underlying \c MpuRegionList instance referenced by the returned pointer must remain valid
                   (e.g. a \c static const instance); a stack-local temporary is invalid once this
                   function returns.
        \warning   An element count greater than (STK_MPU_TASK_REGIONS - 1) is clamped by the driver
                   (STK_ASSERT fires in debug builds); only the first STK_MPU_TASK_REGIONS-1
                   entries are ever applied.
        \code
        class MyTask : public stk::Task<256, stk::ACCESS_USER>
        {
            static constexpr stk::MpuRegionConfig s_extra_regions[] =
            {
                { .addr = MY_BUFFER_ADDR, .size = MY_BUFFER_SIZE,
                  .access_perm = stk::hw::mpu::ACCESS_FULL,
                  .mem_type    = stk::hw::mpu::TYPE_NORMAL_CACHEABLE,
                  .exec        = stk::hw::mpu::EXEC_NEVER },
            };

            const stk::MpuRegionList *GetMpuRegions() const override
            {
                static const stk::MpuRegionList mpu_regions(s_extra_regions, 1U);
                return &mpu_regions;
            }

            void Run() override { ... }
        };
        \endcode
        \see       TaskMpu::Configure, IPlatform::InitStack, MpuRegionList
    */
#if STK_MPU
    virtual const MpuRegionList *GetMpuRegions() const
    {
        return nullptr;
    }
#endif

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
        \note      This is the completion notification of a task removal (returning from Run(),
                   IKernelService::ExitTask() or IKernel::ScheduleTaskRemoval()): after it has been observed by
                   another task the kernel no longer references the ITask object, its stack and its slot, so they
                   can be released or reused, and the same ITask can be added again with IKernel::AddTask() (a
                   restart, e.g. tx_thread_reset()). The removal is an atomic step of the kernel tick: the slot
                   is freed right after this call, before any other task can run (single-core kernel). Until then
                   IKernel::AddTask() of the same task fails with \a ADD_FAIL_DUPLICATE.
    */
    virtual void OnExit() {}

    /*! \brief     Get static base weight of the task.
        \return    Static weight value of the task (must be non-zero, positive 24-bit number).
        \see       SwitchStrategyFixedPriority, SwitchStrategySmoothWeightedRoundRobin,
                   IKernelTask::GetWeight, IKernelService::InheritWeight, IKernelService::RestoreWeight
    */
    virtual Weight GetWeight() const { return DEFAULT_WEIGHT; }

    /*! \brief     Get time-slice quantum of the task (ticks). KERNEL_TIMESLICE only.
        \return    Number of ticks the task may run before being rotated behind its same-priority
                   peers, or NO_TIME_SLICE (default) to run until it blocks, yields or is preempted.
        \note      Read once when the task is bound by Kernel::AddTask(). Use IKernelService::SetTimeSlice()
                   to change it at run-time.
        \see       stk::KERNEL_TIMESLICE, IKernelService::SetTimeSlice
    */
    virtual Timeout GetTimeSlice() const { return NO_TIME_SLICE; }

    /*! \brief     Get task trace name set by application.
        \return    Null-terminated name string, or \c NULL if unused.
        \note      Used for debugging and tracing only (e.g. SEGGER SystemView). Kernel does not
                   interpret this value.
    */
    virtual const char *GetTraceName() const { return nullptr; }

protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~ITask() = default;
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
    
    /*! \brief     Get user task's Stack info.
        \return    Snapshot of Stack info.
    */
    virtual Stack GetUserStack() const = 0;

    /*! \brief     Get static base weight assigned to the task.
        \return    Static weight value of the task.
        \see       SwitchStrategySmoothWeightedRoundRobin, ITask::GetWeight
        \note      Weight API
    */
    virtual Weight GetWeight() const = 0;

    /*! \brief     Get the base (configured) weight of the task, ignoring any priority-inheritance boost.
        \return    ITask::GetWeight() if WEIGHT_API is true; DEFAULT_WEIGHT otherwise. Unlike GetWeight(),
                   the value does not change while the task has inherited a higher weight.
        \see       SwitchStrategyPreemptionThreshold
        \note      Weight API
    */
    virtual Weight GetBaseWeight() const = 0;

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

protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~IKernelTask() = default;
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
        virtual void OnStart(Stack *&enable) = 0;

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
            \return     True if context switch is required, False otherwise.
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
        virtual EWaitResult OnTaskWait(Word caller_SP, ISyncObject *sync_obj, IMutex *mutex, Timeout timeout) = 0;

        /*! \brief      Called from the Thread process when for getting task/thread id of the process.
            \param[in]  caller_SP: Value of Stack Pointer (SP) register (for locating the calling process inside the kernel).
            \return     Task/thread id of the process (returns always valid TId belonging to a task).
        */
        virtual TId OnGetTid(Word caller_SP) = 0;

        /*! \brief      Called from the Thread process to suspend scheduling.
            \param[in]  suspended: \c true if scheduling was successfully suspended, \c false otherwise.
        */
        virtual void OnSuspend(bool suspended) = 0;

        /*! \brief      Called when task state change forces a context switch (Sleep, SleepUntil, Wait).
            \param[in]  id: Task id causing this forced context switch.
            \param[out] idle: Stack of the task which must enter Idle state.
            \param[out] active: Stack of the task which must enter Active state (to which context will switch).
            \return     True if context switch is required, False otherwise.
        */
        virtual bool OnForceContextSwitch(TId id, Stack *&idle, Stack *&active) = 0;
    };

    /*! \class IEventOverrider
        \brief Interface for a platform event overrider.
        \note  Optional. Can be used to extend functionality of default IPlatform driver handlers from the user-space.
    */
    class IEventOverrider
    {
    public:
        /*! \brief  Called by the Kernel when it is entering a sleep mode.
            \return True if event is handled otherwise False to let driver handle it.
        */
        virtual bool OnSleep(Timeout sleep_ticks)
        {
            STK_UNUSED(sleep_ticks);
            return false;
        }

        /*! \brief  Called by Kernel when hard fault happens.
            \note   Normally called by Kernel when one of the scheduled tasks missed its deadline (see stk::KERNEL_HRT, IPlatform::HardFault).
            \return True if event is handled otherwise False to let driver handle it.
        */
        virtual bool OnHardFault()
        {
            return false;
        }

        /*! \brief     Called by the platform driver during initialization to obtain global, application-defined MPU configurations.
            \details   Allows user-space code to configure custom MPU regions (e.g., shared data/code sections or peripheral blocks)
                       and enable state at system startup. Unlike per-task MPU regions set via \c ITask::GetMpuRegions(),
                       configurations returned by this callback apply globally across the platform driver.
            \return    Pointer to a constant \c MpuConfig structure encapsulating global region descriptors and enable state,
                       or \c nullptr if no custom global MPU configuration is supplied.
            \note      Optional. The default implementation returns \c nullptr.
            \note      The underlying \c MpuConfig instance referenced by the returned pointer must remain valid for the duration
                       of platform initialization (e.g., point to a \c static instance).
            \see       MpuConfig, ITask::GetMpuRegions
        */
    #if STK_MPU
        virtual const MpuConfig *OnConfigureMpu() const
        {
            return nullptr;
        }
    #endif

        /*! \brief     Called by platform driver when hardware exception occurred.
            \param[in] exc_id: Hardware exception id, see \a EHwException.
            \param[in] tid: \c TId of the currently active task.
            \param[in] ctx: FaultContext which includes hardware exception frame captured at
                       fault entry (see \a hw::ExceptionFrame). PC is the true faulting
                       instruction address; may be nullptr if unavailable.
            \return    True if event is handled otherwise False to let driver handle it.
            \see       EHwException
        */
        virtual bool OnException(EHwException exc_id, TId tid, const struct FaultContext *const ctx)
        {
            STK_UNUSED(exc_id);
            STK_UNUSED(tid);
            STK_UNUSED(ctx);
            return false;
        }
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
    */
    virtual void InitStack(EStackType stack_type, Stack *stack, IStackMemory *stack_memory, ITask *user_task) = 0;

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

    /*! \brief     Force context switch.
    */
    virtual void ForceContextSwitch(TId id) = 0;

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

    /*! \brief     Put calling process into a waiting state until synchronization object is signaled or timeout occurs.
        \note      This function implements core blocking logic using the Monitor pattern to ensure atomicity between state check and suspension.
        \note      The kernel automatically unlocks the provided \a mutex before the task is suspended and re-locks it before this function returns.
        \param[in] sobj: Synchronization object to wait on.
        \param[in] mutex: Mutex protecting the state of the synchronization object.
        \param[in] timeout: Maximum wait time (ticks). Use \c WAIT_INFINITE to block indefinitely, use \c NO_WAIT to poll without blocking.
        \return    Wait result.
        \see       ISyncObject, IMutex, EWaitResult
        \warning   ISR-unsafe.
    */
    virtual EWaitResult Wait(ISyncObject *sobj, IMutex *mutex, Timeout timeout) = 0;

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
        \note      Called by the Kernel when task missed its deadline in HRT mode (see stk::KERNEL_HRT).
    */
    virtual void ProcessHardFault() = 0;

    /*! \brief     Set platform event overrider.
        \note      Must be set prior call to IKernel::Start.
        \param[in] overrider: Platform event overrider.
        \param[in] non_secure: True if instance comes from a Non-Secure binary (TrustZone only).
    */
    virtual void SetEventOverrider(IEventOverrider *overrider, bool non_secure = false) = 0;

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
    
    /*! \brief     Notify the scheduler of a CPU core's operating frequency.
        \param[in] core_id: Target CPU core ID, or 0xFF to set the frequency for all cores.
        \param[in] frequency:  CPU core frequency in Hz.
        \note      ISR-safe.
        \warning   Specifying an incorrect frequency will degrade system timing accuracy.
                   Always call this function whenever the core clock speed changes.
    */
    virtual void SetCpuFrequency(uint8_t core_id, uint32_t frequency) = 0;

protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~IPlatform() = default;
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
        DEADLINE_MISSED_API      = 0, // (1) if strategy implements OnTaskDeadlineMissed() and can absorb HRT deadline overruns, (0) otherwise (see \a ITaskSwitchStrategy::OnTaskDeadlineMissed)
        PRIORITY_INHERITANCE_API = 0, // (1) if strategy expects OnTaskWeightChange() events to support priority inheritance requests
        NOSLEEP_YIELD_API        = 0, // (1) if strategy implements OnTaskYield() and handles Yield() without putting the task to sleep, (0) otherwise (see \a ITaskSwitchStrategy::OnTaskYield)
        TIMESLICE_API            = 0  // (1) if GetNext() is sticky (does not rotate same-priority peers by itself) and OnTaskTimeSlice() rotates them, (0) otherwise (see \a ITaskSwitchStrategy::OnTaskTimeSlice)
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
    virtual IKernelTask *GetFirst() = 0;
    
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

    /*! \brief     Notification that the running task called Yield() (non-HRT kernel modes only).
        \param[in] task: Pointer to the yielding task, currently in the runnable set.
        \return    \c true if the strategy handled the yield itself: the task stays runnable (it is
                   NOT put to sleep) and the strategy must arrange for the following GetNext() to
                   return another task according to its own policy; \c false (default) if the kernel
                   shall perform the legacy yield, i.e. sleep the task for YIELD_TICKS.
        \note      Called from within a critical section, in the context of the yielding task.
        \note      Never called in KERNEL_HRT mode, where Yield() signals job completion.
        \see       SwitchStrategyPreemptionThreshold::OnTaskYield
    */
    virtual bool OnTaskYield(IKernelTask *task)
    {
        STK_UNUSED(task);
        return false;
    }

    /*! \brief     Notification that the running task's time slice expired (KERNEL_TIMESLICE only).
        \param[in] task: Pointer to the running task, currently in the runnable set.
        \return    \c true if the strategy rotated to a same-priority peer (the following GetNext()
                   returns it); \c false to keep running the task (no peer, or the task is not
                   eligible for time-slicing).
        \note      Unlike OnTaskYield() this must not release preemption-threshold style holds.
        \note      Called from the tick handler. Only invoked when \c TIMESLICE_API == 1.
        \see       SwitchStrategyPreemptionThreshold::OnTaskTimeSlice
    */
    virtual bool OnTaskTimeSlice(IKernelTask *task)
    {
        STK_UNUSED(task);
        return false;
    }

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
    
protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~ITaskSwitchStrategy() = default;
};

/*! \class IKernel
    \brief Interface for the implementation of the kernel of the scheduler. It supports Soft
           and Hard Real-Time modes.
    \note  Mediator design pattern.
*/
class IKernel
{
public:
    /*! \enum    EKernelState
        \brief   Kernel state.
    */
    enum EKernelState : uint8_t
    {
        KSTATE_INACTIVE = 0, //!< Not ready, IKernel::Initialize() must be called.
        KSTATE_READY,        //!< Ready to start, IKernel::Start() must be called.
        KSTATE_RUNNING,      //!< Initialized and running, IKernel::Start() was called successfully.
        KSTATE_SUSPENDED,    //!< Scheduling is suspended with IKernelService::Suspend().
        KSTATE_LOCKED        //!< Running, but task switching is disabled with IKernelService::LockScheduler(). Ticks, timeouts and ISRs keep running.
    };

    /*! \brief     Initialize kernel.
        \param[in] resolution_us: Resolution of the system tick (SysTick) timer in microseconds.
                   Defaults to PERIODICITY_DEFAULT (1000 µs = 1 ms).
        \note      Must be called before AddTask() and Start().
        \note      If running on an STM32 device with HAL driver or on QEMU, do not change the default
                   resolution (PERIODICITY_DEFAULT). STM32's HAL expects 1 millisecond resolution and
                   QEMU does not have enough resolution on Windows to operate correctly at sub-millisecond
                   resolution.
        \note      Kernel must be in \a KSTATE_INACTIVE state.
    */
    virtual void Initialize(uint32_t resolution_us = PERIODICITY_DEFAULT) = 0;

    /*! \brief     Add user task.
        \note      This function is for Soft Real-Time modes only (i.e. \c KERNEL_HRT must not be set in
                   the kernel mode flags).
        \note      Before Start(): the task is bound and added to the strategy immediately.
        \note      After Start() (\c KERNEL_DYNAMIC only): the task is bound and added immediately, inside a
                   critical section, then scheduled by the next tick. The call does not block, does not yield
                   and is not deferred to the tick handler, so it is also valid while the caller holds a
                   critical section.
        \note      ISR-safe in \c KERNEL_DYNAMIC mode. Restrictions for an ISR caller:
                   - the whole operation (stack initialization, MPU setup, ITask::GetStack(), GetAccessMode(),
                     GetMpuRegions(), GetTimeSlice() and GetWeight()) runs with interrupts masked, so keep
                     the stack of such a task modest and call from a low-priority ISR;
                   - on Cortex-M the ISR priority must be numerically above (less urgent than) the SVCall
                     priority (STK_CORTEX_M_SVCALL_ISR_PRIORITY), otherwise it is not masked by the critical
                     section of an unprivileged task;
                   - with \c KERNEL_TICKLESS, a task added from an ISR while all other tasks are sleeping is
                     first scheduled when the already armed timer expires (up to the current idle period),
                     not on the next tick; a call from a task is not affected;
                   - the kernel is assumed to be per-core: the critical section excludes the tick of the
                     local core only.
        \param[in] user_task: Pointer to the user task to add.
        \return    See EAddResult. The kernel is not changed if the result is not \a ADD_OK.
        \warning   Error handling depends on the kernel mode:
                   - \c KERNEL_DYNAMIC: any failure is only returned, never asserted. The caller is
                     responsible for checking the result (assert, retry or recover), a task which is not
                     added is silently dropped otherwise (e.g. \a ADD_FAIL_POOL_FULL, \a ADD_FAIL_DUPLICATE);
                   - other modes: any failure asserts (application design failure), in a release build the
                     failure is not reported and the task is not added.
        \note      A task which has been removed (ITask::OnExit() was called) can be added again, the same ITask
                   object and stack are reused. Before the removal is completed the call fails with
                   \a ADD_FAIL_DUPLICATE.
        \warning   The task and stack collision check is always performed, also in a release build.
    */
    virtual EAddResult AddTask(ITask *user_task) = 0;

    /*! \brief     Add user task.
        \note      This function is for Hard Real-time mode only, e.g. stk::KERNEL_HRT is used as parameter.
        \param[in] user_task: Pointer to the user task to add.
        \param[in] periodicity_tc: Periodicity time at which task is scheduled (ticks).
        \param[in] deadline_tc: Deadline time within which a task must complete its work (ticks).
        \param[in] start_delay_tc: Initial start delay for the task (ticks). 0 means activate immediately.
        \note      Periodicity and deadline must be > 0 and < INT32_MAX, start delay must be >= 0.
        \return    See EAddResult. The kernel is not changed if the result is not \a ADD_OK.
                   \a ADD_FAIL_INVALID is returned for a null task, an uninitialized or already started kernel,
                   invalid timing parameters, or a call outside \c KERNEL_HRT mode.
        \warning   Tasks must be added before the scheduler is started (before Start()). Adding an HRT task
                   after Start() is not supported, not even from an ISR or in \c KERNEL_DYNAMIC mode: the
                   HRT schedule (periodicity, deadlines, start delays) is built from the complete task set
                   and is not recomputed at runtime.
        \warning   Error handling depends on the kernel mode:
                   - \c KERNEL_DYNAMIC: any failure is only returned, never asserted. The caller is
                     responsible for checking the result;
                   - \c KERNEL_STATIC: any failure asserts (application design failure), in a release build
                     the failure is only returned and the task is not added.
        \warning   The task and stack collision check is always performed, also in a release build.
    */
    virtual EAddResult AddTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc) = 0;

    /*! \brief     Remove a previously added task from the kernel when it is not started.
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
                   or if called before Start() (i.e. while the kernel is not yet running).
                   Use RemoveTask instead to remove a task while the kernel is not running.
        \see       RemoveTask
    */
    virtual void ScheduleTaskRemoval(ITask *user_task) = 0;

    /*! \brief      Suspend task.
        \param[in]  user_task: Pointer to the user task to suspend.
        \return     See ESuspendResult. A task that is sleeping or blocked on a synchronization object
                    gets a deferred suspension (SUSPEND_DEFERRED): its sleep/wait finishes normally
                    (the wait result is preserved) and the task is then parked until ResumeTask().
        \note       ISR-safe. From an ISR the interrupted task is switched out by the next tick, no
                    forced context switch is requested (SVC is illegal in handler mode).
        \note       When a task suspends itself (task context) the call returns after it has been resumed.
        \note       hw::CriticalSection must not be active otherwise a deadlock will
                    happen if task is suspending self.
        \warning    A task which holds the scheduler lock (see IKernelService::LockScheduler) must not suspend itself: asserts in debug builds,
                    in release builds does nothing and returns SUSPEND_FAIL. Suspending another task is allowed.
    */
    virtual ESuspendResult SuspendTask(ITask *user_task) = 0;

    /*! \brief      Suspend task (legacy form).
        \param[in]  user_task: Pointer to the user task to suspend.
        \param[out] suspended: Set to true if the task is suspended immediately (SUSPEND_DONE).
        \see        SuspendTask(ITask *)
    */
    void SuspendTask(ITask *user_task, bool &suspended)
    {
        suspended = (SuspendTask(user_task) == SUSPEND_DONE);
    }

    /*! \brief     Resume task suspended by SuspendTask(), or lift its deferred suspension.
        \param[in] user_task: Pointer to the user task to resume.
        \return    See EResumeResult.
        \note      ISR-safe.
        \note      A task blocked in IKernelService::Wait() is not woken by this call (its wait is not
                   a suspension). Use CancelTaskWait() to interrupt that kind of wait.
    */
    virtual EResumeResult ResumeTask(ITask *user_task) = 0;

    /*! \brief     Cancel a task's in-progress wait on a synchronization object, waking it early.
        \param[in] user_task: Pointer to the user task whose wait should be cancelled.
        \note      Only has effect if the task is currently blocked in IKernelService::Wait()
                   (i.e. inside OnTaskWait()). No-op otherwise (e.g. task is running, suspended, or
                   sleeping via Sleep()/SleepUntil()).
        \note      The cancelled task's IKernelService::Wait() call returns WAIT_RESULT_CANCELED -
                   distinct from WAIT_RESULT_SIGNAL (resource was NOT acquired/signalled) and
                   WAIT_RESULT_TIMEOUT (the timeout, if any, had not yet expired).
        \note      Primitives built on top of IKernelService::Wait() surface this in their own
                   terms: the boolean Mutex/FastMutex::TimedLock(), Semaphore::Wait() and
                   Event::Wait() collapse it into their failure result, while the extended
                   variants (FastMutex::TimedLockEx(), Semaphore::WaitEx(), Event::WaitEx())
                   return WAIT_RESULT_CANCELED as is; EventFlags::Wait() reports it as
                   EventFlags::ERROR_CANCELED, distinct from EventFlags::ERROR_TIMEOUT.
        \note      KERNEL_SYNC mode only.
        \see       IKernelService::SleepCancel, which additionally ends a plain Sleep()/SleepUntil() and
                   cancels a sync-object wait identically.
    */
    virtual void CancelTaskWait(ITask *user_task) = 0;

    /*! \brief      Get a snapshot of the kernel-side state of a task.
        \param[in]  user_task: Pointer to the user task to query. Must not be \c nullptr.
        \param[out] info: Receives the snapshot (see TaskStateInfo). Written only if the function returns \c true,
                         left unchanged otherwise.
        \return     \c true if the query succeeded and \a info is valid (TaskStateInfo::state is TASK_STATE_REMOVED
                    if the task is not bound to a kernel slot), \c false if \a user_task is \c nullptr or the kernel
                    is built without KERNEL_TELEMETRY.
        \note       The check order resolves overlaps: a task suspended by SuspendTask() or blocked in
                    IKernelService::Wait() is also asleep internally, but is reported as TASK_STATE_SUSPENDED
                    or TASK_STATE_WAITING respectively, never as TASK_STATE_SLEEPING.
        \note       The result is a snapshot, the task can change state immediately after the call.
        \note       KERNEL_TELEMETRY mode only.
        \warning    ISR-safe. Execution time is proportional to the number of task slots.
        \see        SuspendTask, CancelTaskWait, ETaskState, TaskStateInfo, KERNEL_TELEMETRY
    */
    virtual bool GetTaskState(ITask *user_task, TaskStateInfo &info) const = 0;

    /*! \brief     Enumerate kernel tasks.
        \param[in] tasks: Reference to the ArrayView of IKernelTask pointers.
        \return    Number of tasks in the array.
        \warning   ISR-safe.
    */
    virtual size_t EnumerateKernelTasks(ArrayView<IKernelTask *> tasks) = 0;

    /*! \brief     Enumerate user tasks.
        \param[in] user_tasks: Reference to the ArrayView of ITask pointers.
        \return    Number of tasks in the array.
        \warning   ISR-safe.
    */
    virtual size_t EnumerateTasks(ArrayView<ITask *> user_tasks) = 0;

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
        STK_STATIC_ASSERT(TMaxCount > 0U);

        ITask *tasks[TMaxCount] = {};        
        size_t count = EnumerateTasks(ArrayView<ITask *>(tasks, TMaxCount));
        size_t i = 0U;
        bool fetch_next = true;

        while ((i < count) && fetch_next)
        {
            fetch_next = callback(tasks[i]);
            ++i;
        }

        return i;
    }

    /*! \brief     Lock or unlock task switching. Ticks, timeouts and ISRs keep running.
        \param[in] lock: \c true to lock, \c false to unlock.
        \return    Previous lock state (LOCK_WAS_UNLOCKED / LOCK_WAS_LOCKED), or LOCK_FAIL / LOCK_FAIL_ISR.
        \note      Single kernel-level primitive: IKernelService::LockScheduler(), UnlockScheduler() and
                   RestoreSchedulerLock() (and their stk:: helpers) are built on it. Application code should
                   use those. Always reports the PREVIOUS state, RestoreSchedulerLock() converts it to the new one.
        \note      Not nested.
        \note      Task context only. Not available in KERNEL_HRT mode (LOCK_FAIL).
        \note      While locked the kernel state is KSTATE_LOCKED.
        \warning   The lock holder must keep the CPU. While locked, Sleep(), SleepUntil(), Yield(), IKernelService::Wait()
                   and SuspendTask() of the calling task must not be called: they assert in debug builds. In release
                   builds they are no-ops (SleepUntil() returns false, Wait() returns WAIT_RESULT_FAIL,
                   SuspendTask() returns SUSPEND_FAIL). Delay() is allowed. Suspending another task is allowed.
        \warning   Blocking on a synchronization object that is owned by another task would never complete,
                   because no other task can run while locked. A synchronization object which is free can be used.
        \note      Exiting (IKernelService::ExitTask() or returning from ITask::Run()) can not be refused: it asserts
                   in debug builds, in release builds the task exits and the lock is released when it leaves the CPU,
                   so the lock never passes on to another task.
        \note      Time-slice accounting (KERNEL_TIMESLICE) continues while locked, rotation of an expired slice is
                   deferred until the lock is released.
        \note      Entering tickless suspend (IKernelService::Suspend()) drops the lock, the kernel state becomes
                   KSTATE_SUSPENDED.
        \see       ELockResult, IKernelService::LockScheduler, IKernelService::UnlockScheduler, IKernelService::RestoreSchedulerLock
    */
    virtual ELockResult SetSchedulerLock(bool lock) = 0;

    /*! \brief     Start kernel scheduling.
        \note      This function never returns. Must be called after Initialize() and AddTask().
        \note      Kernel must be in \a KSTATE_READY state.
    */
    virtual void Start() = 0;

    /*! \brief     Get a snapshot of the kernel state.
        \return    Kernel state.
        \see       EKernelState
    */
    virtual EKernelState GetState() const = 0;

    /*! \brief     Get platform driver instance.
        \return    Pointer to the IPlatform concrete class instance.
    */
    virtual IPlatform *GetPlatform() = 0;

    /*! \brief     Get switch strategy instance.
        \return    Pointer to the ITaskSwitchStrategy concrete class instance.
    */
    virtual ITaskSwitchStrategy *GetSwitchStrategy() = 0;
    
protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~IKernel() = default;
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
        \warning   Must not be called while the scheduler is locked (see LockScheduler): asserts in debug builds,
                   no-op in release builds.
    */
    virtual void Sleep(Timeout ticks) = 0;

    /*! \brief     Put calling process into a sleep state until the specified timestamp.
        \note      Unlike Delay this function does not waste CPU cycles and allows kernel to put CPU into a low-power state.
        \note      Unsupported in HRT mode (see stk::KERNEL_HRT); in HRT mode tasks sleep automatically according to their periodicity and workload.
        \param[in] timestamp: Absolute timestamp (ticks).
        \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
        \warning   Must not be called while the scheduler is locked (see LockScheduler): asserts in debug builds,
                   no-op in release builds (returns false).
        \return    True if sleep succeeded, false otherwise.
    */
    virtual bool SleepUntil(Ticks timestamp) = 0;

    /*! \brief     Cancel sleep of the task.
        \param[in] task_id: Id of the task.
        \details   Ends a sleep started by Sleep()/SleepUntil() early. If the task is instead blocked on a
                   synchronization object (KERNEL_SYNC mode), the wait is cancelled the same way as
                   IKernel::CancelTaskWait() does: the task is removed from the object's wait list and its
                   Wait() returns \c WAIT_RESULT_CANCELED (not \c WAIT_RESULT_TIMEOUT or
                   \c WAIT_RESULT_SIGNAL, no resource is acquired or consumed).
        \note      No-op if task was neither sleeping nor waiting on a synchronization object.
        \note      ISR-safe.
        \see       IKernel::CancelTaskWait, EWaitResult
    */
    virtual void SleepCancel(TId task_id) = 0;

    /*! \brief     Notify scheduler to switch to the next task (yield).
        \note      A cooperation mechanism in HRT mode (see stk::KERNEL_HRT).
        \warning   ISR-unsafe.
        \warning   Must not be called while the scheduler is locked (see LockScheduler): asserts in debug builds,
                   no-op in release builds.
    */
    virtual void SwitchToNext() = 0;

    /*! \brief     Terminate the calling task: schedule its removal and switch it out. Does not return.
        \note      Equivalent to returning from ITask::Run(): the kernel completes the removal on the next tick,
                   calls ITask::OnExit() and releases the kernel slot. Use it to end a task from a nested call,
                   without unwinding (C++ destructors of the task's local objects are NOT run).
        \note      The task, its stack and the kernel slot are no longer referenced by the kernel once
                   ITask::OnExit() has been called, see ITask::OnExit() for re-adding the task.
        \warning   KERNEL_DYNAMIC mode only: panics with KERNEL_PANIC_BAD_MODE otherwise.
        \warning   ISR-unsafe. Calling from an ISR context is not permitted and will trigger an assertion.
        \warning   Should not be called while the scheduler is locked (see LockScheduler): asserts in debug builds.
                   The exit can not be refused, in release builds the task exits and the lock is released.
        \see       IKernel::ScheduleTaskRemoval, ITask::OnExit
    */
    virtual void ExitTask() = 0;

    /*! \brief     Put calling process into a waiting state until synchronization object is
                   signaled or timeout occurs.
        \note      This function implements core blocking logic using the Monitor pattern to
                   ensure atomicity between state check and suspension.
        \note      The kernel automatically unlocks the provided \a mutex before the task is
                   suspended and re-locks it before this function returns.
        \param[in] sobj: Synchronization object to wait on.
        \param[in] mutex: Mutex protecting the state of the synchronization object.
        \param[in] timeout: Maximum wait time (ticks). Use \c WAIT_INFINITE to block indefinitely,
                   use \c NO_WAIT to poll without blocking.
        \return    Wait result (see \c EWaitResult).
        \warning   ISR-unsafe.
        \warning   Must not be called while the scheduler is locked (see LockScheduler), whatever the timeout:
                   asserts in debug builds, in release builds returns \c WAIT_RESULT_FAIL without waiting.
    */
    virtual EWaitResult Wait(ISyncObject *sobj, IMutex *mutex, Timeout timeout) = 0;

    /*! \brief     Wake one or all tasks currently waiting on a synchronization object.
        \param[in] sobj: Synchronization object whose waiting queue will be notified.
        \param[in] all: If \c true, unblocks all waiting tasks (broadcast mode); if \c false,
                   unblocks only the next waiting task (signal mode).
        \see       Wait
        \warning   ISR-unsafe.
    */
    virtual void Wake(ISyncObject *sobj, bool all) = 0;

    /*! \brief     Suspend scheduling.
        \return    Number of ticks available for the suspension period, as determined by the
                   nearest pending wake-up. The caller may program a hardware timer with
                   this value to avoid unnecessary wakeups (tickless idle).
        \note      After suspending the scheduler you can change the CPU frequency and then
                   resume scheduling by calling Resume().
        \note      ISR-safe. Pair with Resume().
        \warning   Requires the kernel to be instantiated with \c stk::KERNEL_TICKLESS; asserts otherwise.
        \see       IKernel::EKernelState::KSTATE_SUSPENDED
    */
    virtual Timeout Suspend() = 0;

    /*! \brief     Resume scheduling after a prior Suspend() call.
        \param[in] elapsed_ticks: Number of ticks that elapsed during the suspended period.
                   The kernel uses this value to advance internal time counters and
                   wake tasks whose sleep deadlines have expired.
        \note      When resuming, the timer will start with a current CPU frequency, therefore
                   you can change CPU frequency after Suspend() and restart scheduler with a
                   new frequency with Resume().
        \note      ISR-safe.
        \warning   Requires the kernel to be instantiated with \c stk::KERNEL_TICKLESS; asserts otherwise.
        \see       IKernel::EKernelState::KSTATE_SUSPENDED
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

    /*! \brief     Change the base weight of the task at run-time (replaces ITask::GetWeight() value).
        \param[in] tid: Task id.
        \param[in] weight: New base weight. Must not be \a NO_WEIGHT; must satisfy the constraints
                   of the active strategy (see e.g. SwitchStrategyPreemptionThreshold::MakeWeight).
        \return    Previous base weight (can be passed back to restore it).
        \note      Requires a strategy with WEIGHT_API = 1 and PRIORITY_INHERITANCE_API = 1 (the
                   strategy must handle OnTaskWeightChange()); asserts otherwise.
        \note      If the task currently inherits a higher weight the new base weight takes effect
                   once the inheritance ends, unless it is higher than the inherited weight.
        \note      Enters the critical section internally; may be called from task context.
        \see       SwitchStrategyPreemptionThreshold::ChangeThreshold
    */
    virtual Weight SetWeight(TId tid, Weight weight) = 0;

    /*! \brief     Change the time-slice quantum of the task at run-time and reload its slice.
        \param[in] tid: Task id.
        \param[in] ticks: New quantum (ticks), NO_TIME_SLICE disables time-slicing for the task.
        \return    Previous quantum (can be passed back to restore it).
        \note      Requires KERNEL_TIMESLICE, asserts otherwise.
        \note      Enters the critical section internally; may be called from task context.
        \see       ITask::GetTimeSlice, stk::KERNEL_TIMESLICE
    */
    virtual Timeout SetTimeSlice(TId tid, Timeout ticks) = 0;
    
    /*! \brief     Disable task switching (equivalent of CMSIS-RTOS2 osKernelLock). Ticks, timeouts and ISRs keep running.
        \return    Previous lock state (LOCK_WAS_UNLOCKED / LOCK_WAS_LOCKED), or LOCK_FAIL / LOCK_FAIL_ISR.
        \note      Not nested: pass the returned value to RestoreSchedulerLock() to emulate nesting.
        \note      Task context only. Not available in KERNEL_HRT mode (LOCK_FAIL).
        \note      While locked the kernel state is KSTATE_LOCKED.
        \warning   The lock holder must keep the CPU. While locked, Sleep(), SleepUntil(), Yield(), IKernelService::Wait()
                   and SuspendTask() of the calling task must not be called: they assert in debug builds. In release
                   builds they are no-ops (SleepUntil() returns false, Wait() returns WAIT_RESULT_FAIL,
                   SuspendTask() returns SUSPEND_FAIL). Delay() is allowed. Suspending another task is allowed.
        \warning   Blocking on a synchronization object that is owned by another task would never complete,
                   because no other task can run while locked. A synchronization object which is free can be used.
        \warning   Synchronization primitives (stk::sync): only non-blocking use is valid while locked, i.e. calls with
                   NO_WAIT / Try*() variants and acquisitions which succeed immediately (free Mutex, signaled Event,
                   available Semaphore token, ...). A call which would have to block asserts in debug builds; in release
                   builds the timed / bool / EWaitResult variants fail with WAIT_RESULT_FAIL, while the void Lock() /
                   ReadLock() of Mutex, FastMutex and RWMutex can not report a failure and panic
                   (KERNEL_PANIC_ASSERT) instead of no-op, because a contended lock under a scheduler lock is a deadlock.
        \note      Exiting (IKernelService::ExitTask() or returning from ITask::Run()) can not be refused: it asserts
                   in debug builds, in release builds the task exits and the lock is released when it leaves the CPU,
                   so the lock never passes on to another task.
        \note      Time-slice accounting (KERNEL_TIMESLICE) continues while locked, rotation of an expired slice is
                   deferred until the lock is released.
        \note      Entering tickless suspend (IKernelService::Suspend()) drops the lock, the kernel state becomes
                   KSTATE_SUSPENDED.
        \see       UnlockScheduler, RestoreSchedulerLock, IKernel::SetSchedulerLock
    */
    virtual ELockResult LockScheduler() = 0;

    /*! \brief     Enable task switching (equivalent of CMSIS-RTOS2 osKernelUnlock).
        \return    Previous lock state (LOCK_WAS_UNLOCKED / LOCK_WAS_LOCKED), or LOCK_FAIL / LOCK_FAIL_ISR.
        \note      A ready task is switched in on the next tick (not immediately).
        \note      Task context only. Not available in KERNEL_HRT mode (LOCK_FAIL).
        \see       LockScheduler, RestoreSchedulerLock, IKernel::SetSchedulerLock
    */
    virtual ELockResult UnlockScheduler() = 0;

    /*! \brief     Set the lock state (equivalent of CMSIS-RTOS2 osKernelRestoreLock).
        \param[in] locked: \c true to lock, \c false to unlock.
        \return    New lock state (LOCK_WAS_LOCKED if now locked, LOCK_WAS_UNLOCKED if now unlocked), or LOCK_FAIL / LOCK_FAIL_ISR.
        \note      Unlike LockScheduler() and UnlockScheduler() it reports the NEW state, not the previous one.
        \note      Use with the value returned by LockScheduler() to emulate nesting.
        \note      Task context only. Not available in KERNEL_HRT mode (LOCK_FAIL).
        \see       LockScheduler, UnlockScheduler, IKernel::SetSchedulerLock
    */
    virtual ELockResult RestoreSchedulerLock(bool locked) = 0;

    /*! \class ScopedSchedulerLock
        \brief Disable task switching (equivalent of CMSIS-RTOS2 osKernelLock). Ticks, timeouts and ISRs keep
               running. Ensures the lock is always unlocked when leaving the scope, even when exceptions
               are thrown.
        \note  RAII
        \note  Restores the lock state which was before construction on scope exit, i.e. scopes can be nested:
               the scheduler is unlocked only when the outermost scope (which found it unlocked) is left.
        \note  If the lock could not be taken (LOCK_FAIL / LOCK_FAIL_ISR) the destructor does nothing useful: it
               tries to restore the "unlocked" state, which fails the same way.
        \note  See LockScheduler() for the restrictions that apply to the lock holder.
        \see   LockScheduler, UnlockScheduler, RestoreSchedulerLock
    */
    class ScopedSchedulerLock
    {
    public:
        explicit ScopedSchedulerLock() : m_result(GetInstance()->LockScheduler())
        {}
        ~ScopedSchedulerLock()
        {
            // restore the state which was before construction: keeps an outer lock held (nesting)
            STK_UNUSED(GetInstance()->RestoreSchedulerLock(m_result == LOCK_WAS_LOCKED));
        }

    private:
        STK_NONCOPYABLE_CLASS(ScopedSchedulerLock);

        const ELockResult m_result;
    };

protected:
    /*! \brief Destructor.
        \note  Protected by design. Prevents unsafe polymorphic delete.
    */
    ~IKernelService() = default;

    /*! \brief Access helper to the mutable IWaitObject::GetWaitList().
    */
    static __stk_forceinline IWaitObject::ListHeadType &GetMutableWaitList(ISyncObject *sobj)
    {
        return sobj->GetMutableWaitList();
    }
};

} // namespace stk

#endif /* STK_COMMON_H_ */
