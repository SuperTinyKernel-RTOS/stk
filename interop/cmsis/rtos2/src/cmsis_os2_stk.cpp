/*
 * CMSIS RTOS2 wrapper for SuperTinyKernel (STK) RTOS.
 *
 * Maps the CMSIS RTOS2 C API (cmsis_os2.h) onto the STK C++ API.
 *
 * Supported:
 *   - Kernel management     (osKernelInitialize / Start / GetState / GetInfo /
 *                            GetTickCount / GetTickFreq / GetSysTimerCount /
 *                            GetSysTimerFreq / Lock / Unlock / RestoreLock)
 *   - Thread management     (osThreadNew / Delete / Yield / Delay / osDelay /
 *                            GetId / GetName / GetState / GetPriority /
 *                            SetPriority / GetStackSize / GetStackSpace /
 *                            GetCount / Terminate / Suspend / Resume)
 *   - Thread flags          (osThreadFlagsSet / Clear / Get / Wait)
 *   - Event flags           (osEventFlagsNew / Delete / Set / Clear / Get / Wait)
 *   - Mutex                 (osMutexNew / Delete / Acquire / Release / GetOwner)
 *   - Semaphore             (osSemaphoreNew / Delete / Acquire / Release / GetCount)
 *   - Timer                 (osTimerNew / Delete / Start / Stop / IsRunning)
 *   - Message Queue         (osMessageQueueNew / Delete / Put / Get /
 *                            GetCapacity / GetMsgSize / GetCount / GetSpace / Reset)
 *
 * Not supported (return osError / NULL):
 *   - osKernelSuspend / Resume   — no direct STK equivalent
 *   - osThreadJoin / Detach      — STK has no join semantics
 *   - osThreadEnumerate          — no public task list API in STK
 *   - osMemoryPool*              — STK provides no heap pool API
 *
 * Design notes:
 *   - All objects are heap-allocated with operator new/delete.
 *     For a fully static deployment, replace with a static object-pool allocator.
 *   - The wrapper owns one global Kernel instance (g_StkKernel).
 *     Call osKernelInitialize() before any other API, then osKernelStart().
 *   - The kernel is configured with KERNEL_DYNAMIC | KERNEL_SYNC and
 *     SwitchStrategyFP32 (32 fixed-priority levels).
 *     osThread priority values (osPriorityIdle=1 .. osPriorityISR=56) are
 *     linearly mapped to STK priority levels 0..31.
 *   - CMSIS osWaitForever (0xFFFFFFFF) is translated to STK WAIT_INFINITE.
 *   - Timeout values in CMSIS are in ticks; STK Sleep/Delay also take ticks,
 *     so no conversion is required.
 *   - Thread flags and event flags are backed by stk::sync::EventFlags,
 *     STK's native 32-bit multi-flag synchronization primitive.
 *   - Message queues use a custom ring-buffer backed by stk::sync::Mutex and
 *     stk::sync::ConditionVariable (Pipe<> requires compile-time capacity).
 *
 * Limitations / deviations from the specification:
 *   - osThreadSuspend / osThreadResume — not directly available in STK's
 *     public API; these return osError.
 *   - osMutexGetOwner — returns NULL (STK Mutex does not expose owner tid
 *     through a public accessor).
 *   - osKernelGetSysTimerCount / Freq — returns a tick-based approximation;
 *     hardware cycle counter is not accessed here.
 *   - Priority inheritance (osMutexPrioInherit) and robust mutex
 *     (osMutexRobust) attributes are silently ignored; STK Mutex is always
 *     recursive (osMutexRecursive is therefore always effective).
 */

#include "cmsis_os2.h"

#include <cstring>
#include <cstdlib>
#include <new>
#include <stdint.h>

#include "stk.h"
#include "sync/stk_sync_mutex.h"
#include "sync/stk_sync_semaphore.h"
#include "sync/stk_sync_eventflags.h"
#include "time/stk_time_timer.h"

// ---------------------------------------------------------------------------
// Kernel version / identification
// ---------------------------------------------------------------------------

#define STK_WRAPPER_API_VERSION     20030000UL   // 2.3.0
#define STK_WRAPPER_KERNEL_VERSION  20030000UL
#define STK_WRAPPER_KERNEL_ID       "STK RTOS2 Wrapper v1.0"

// ---------------------------------------------------------------------------
// Kernel configuration
// ---------------------------------------------------------------------------

// Number of task slots in the global kernel instance.
// Increase if more concurrent threads are needed.
#ifndef CMSIS_STK_MAX_THREADS
#define CMSIS_STK_MAX_THREADS 16U
#endif

// Default stack size (in Words) when the caller passes stack_size == 0.
#ifndef CMSIS_STK_DEFAULT_STACK_WORDS
#define CMSIS_STK_DEFAULT_STACK_WORDS 256U
#endif

// Minimum stack size (in Words) – mirrors STK's own STACK_SIZE_MIN.
#define CMSIS_STK_MIN_STACK_WORDS STK_STACK_SIZE_MIN

// Returns a size of memory in stk::Word elements required for object allocation.
template <typename T> constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1) / sizeof(stk::Word));
}

// Priority mapping:
//   CMSIS range: osPriorityIdle(1) .. osPriorityISR(56)  ->  57 levels
//   STK FP32 range: 0 .. 31                              ->  32 levels
// Linear map: stk_prio = (cmsis_prio * 31) / 56
static __stk_forceinline int32_t CmsisPrioToStk(osPriority_t p)
{
    if (p <= osPriorityIdle)
        return 0;
    if (p >= osPriorityISR)
        return 31;

    return (static_cast<int32_t>(p) * 31) / 56;
}

static __stk_forceinline osPriority_t StkPrioToCmsis(int32_t p)
{
    // Inverse: cmsis_prio = (stk_prio * 56) / 31
    int32_t r = (p * 56) / 31;
    if (r < static_cast<int32_t>(osPriorityIdle))
        r = static_cast<int32_t>(osPriorityIdle);

    if (r > static_cast<int32_t>(osPriorityISR))
        r = static_cast<int32_t>(osPriorityISR);

    return static_cast<osPriority_t>(r);
}

// ---------------------------------------------------------------------------
// Convert CMSIS timeout (ticks) -> STK timeout (ticks / milliseconds).
//
// CMSIS ticks == STK ticks when tick resolution is 1 ms (PERIODICITY_DEFAULT).
// For other resolutions the conversion uses the kernel's tick resolution.
// osWaitForever -> WAIT_INFINITE.
// ---------------------------------------------------------------------------
static __stk_forceinline stk::Timeout CmsisTimeoutToStk(uint32_t ticks)
{
    if (ticks > stk::WAIT_INFINITE)
        return stk::WAIT_INFINITE;

    if (ticks == 0U)
        return stk::NO_WAIT;

    // CMSIS ticks are kernel ticks – pass through directly as STK ticks
    return static_cast<stk::Timeout>(ticks);
}

// ---------------------------------------------------------------------------
// ISR context check
// ---------------------------------------------------------------------------
static __stk_forceinline bool IsIrqContext()
{
    return stk::hw::IsInsideISR();
}

// ---------------------------------------------------------------------------
// Global kernel type alias
// ---------------------------------------------------------------------------
using StkKernel = stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC,
    CMSIS_STK_MAX_THREADS, stk::SwitchStrategyFP32, stk::PlatformDefault>;

static StkKernel      *g_StkKernel = nullptr;
static stk::Word       g_KernelBuf[StkGetWordCountForType<StkKernel>()];
static osKernelState_t g_KernelState = osKernelInactive;

// ---------------------------------------------------------------------------
// Thread control block
// ---------------------------------------------------------------------------

/*
 * StkThread wraps a single CMSIS thread.
 *
 * Stack memory is provided either by the caller (static allocation) or
 * allocated dynamically from operator new[] (heap allocation).
 *
 * The CMSIS function pointer + argument are stored here; the STK task's
 * Run() method simply calls func(argument).
 *
 * Priority is stored as STK weight (0..31) and returned via GetWeight().
 */
struct StkThread : public stk::ITask
{
    // --- Join support ---
    enum class JoinState : uint8_t
    {
        Detached, // osThreadDetach() was called, or created with osThreadDetached
        Joinable, // created with osThreadJoinable, not yet joined or exited
        Exited,   // Run() has returned; OnExit() has fired; not yet joined
        Joined,   // a joiner has already collected the result; further joins are errors
    };

    explicit StkThread()
        : m_func(nullptr), m_argument(nullptr), m_name(nullptr),
          m_stk_priority(CmsisPrioToStk(osPriorityNormal)),
          m_stack(nullptr), m_stack_size(0), m_join_state(JoinState::Detached),
          m_stack_owned(false), m_suspended(false),
          m_cb_owned(true)
    {}

    virtual ~StkThread()
    {
        if (m_stack_owned && m_stack)
        {
            delete[] m_stack;
            m_stack = nullptr;
        }
    }

    void Run() override
    {
        m_func(m_argument);

        // KERNEL_DYNAMIC: returning from Run() removes the task automatically.
    }

    void OnExit() override
    {
        m_join_state = JoinState::Exited;

        // wake any osThreadJoin() caller
        m_join_cv.NotifyAll();
    }

    // ---- IStackMemory ----
    stk::Word *GetStack()            const override { return m_stack; }
    size_t GetStackSize()            const override { return m_stack_size; }
    size_t GetStackSizeBytes()       const override { return m_stack_size * sizeof(stk::Word); }

    // ---- ITask ----
    stk::EAccessMode GetAccessMode() const override { return stk::ACCESS_PRIVILEGED; }
    void OnDeadlineMissed(uint32_t)  override       {}
    int32_t GetWeight()              const override { return m_stk_priority; }
    stk::TId GetId()                 const override { return stk::hw::PtrToWord(this); }
    const char *GetTraceName()       const override { return m_name; }

    // ---- Members ----
    osThreadFunc_t               m_func;
    void                        *m_argument;
    const char                  *m_name;
    volatile int32_t             m_stk_priority; // STK priority level 0..31
    stk::Word                   *m_stack;        // pointer to stack memory (may be owned)
    size_t                       m_stack_size;   // stack size in Words
    volatile JoinState           m_join_state;   // guarded by m_join_mutex between other tasks
    stk::sync::Mutex             m_join_mutex;
    stk::sync::ConditionVariable m_join_cv;      // signaled in OnExit()
    stk::sync::EventFlags        m_thread_flags; // Per-thread event flags — backed by STK's native 32-bit EventFlags primitive.
    bool                         m_stack_owned;  // true if we allocated the stack ourselves
    bool                         m_suspended;    // true if suspended
    bool                         m_cb_owned;     // true -> heap-allocated control block, delete on Terminate(), false -> caller-supplied cb_mem, call destructor explicitly
};

// ---------------------------------------------------------------------------
// Mutex control block
// ---------------------------------------------------------------------------

struct StkMutex
{
    stk::sync::Mutex m_mutex;
    bool             m_cb_owned; // true -> heap-allocated, false -> placement-new in caller memory

    // The CMSIS name is forwarded to STK's ITraceable interface so it is
    // visible to SEGGER SystemView and other debug tools when
    // STK_SYNC_DEBUG_NAMES == 1.  GetTraceName() returns nullptr when the
    // feature is disabled, which is fine for release builds.
    explicit StkMutex(const char *n = nullptr) : m_mutex(), m_cb_owned(true)
    {
        m_mutex.SetTraceName(n);
    }
};

// ---------------------------------------------------------------------------
// Semaphore control block
// ---------------------------------------------------------------------------

struct StkSemaphore
{
    stk::sync::Semaphore m_semaphore;
    bool                 m_cb_owned; // true -> heap-allocated, false -> placement-new in caller memory

    explicit StkSemaphore(uint16_t initial, uint16_t max_count, const char *n = nullptr)
        : m_semaphore(initial, max_count), m_cb_owned(true)
    {
        m_semaphore.SetTraceName(n);
    }
};

// ---------------------------------------------------------------------------
// Event flags control block
//
// Backed directly by stk::sync::EventFlags — STK's native 32-bit multi-flag
// synchronization primitive (Set/Clear/Get/Wait with ANY/ALL/NO_CLEAR options,
// ISR-safe Set/Clear, absolute-deadline wait loop).
// ---------------------------------------------------------------------------

struct StkEventFlags
{
    stk::sync::EventFlags m_ef;
    bool                  m_cb_owned; // true -> heap-allocated, false -> placement-new in caller memory

    explicit StkEventFlags(const char *n = nullptr) : m_ef(0U), m_cb_owned(true)
    {
        m_ef.SetTraceName(n);
    }
};

// ---------------------------------------------------------------------------
// Timer control block
//
// CMSIS timers are backed by stk::time::TimerHost.
// A single global TimerHost is created on first osTimerNew().
// ---------------------------------------------------------------------------

static stk::time::TimerHost *g_TimerHost = nullptr;
static stk::Word             g_TimerHostBuf[StkGetWordCountForType<stk::time::TimerHost>()];

static bool EnsureTimerHostCreated()
{
    if (g_StkKernel != nullptr)
    {
        if (g_TimerHost == nullptr)
        {
            g_TimerHost = new (g_TimerHostBuf) stk::time::TimerHost();
            g_TimerHost->Initialize(g_StkKernel, stk::ACCESS_PRIVILEGED);
        }

        return true;
    }

    return false;
}

struct StkTimer : public stk::time::TimerHost::Timer
{
    osTimerFunc_t  m_func;
    void          *m_argument;
    osTimerType_t  m_type;
    const char    *m_name;
    uint32_t       m_period_ticks; // stored period for restart
    bool           m_cb_owned;     // true -> heap-allocated, false -> placement-new in caller memory

    explicit StkTimer(osTimerFunc_t f, osTimerType_t t, void *arg, const char *n)
        : m_func(f), m_argument(arg), m_type(t), m_name(n), m_period_ticks(0U), m_cb_owned(true)
    {}
    virtual ~StkTimer()
    {}

    void OnExpired(stk::time::TimerHost * /*host*/) override
    {
        m_func(m_argument);
    }
};

// ---------------------------------------------------------------------------
// Message queue control block
//
// CMSIS message queue stores fixed-size opaque byte messages.
// Because stk::sync::Pipe<> requires a compile-time capacity, we implement a
// thin ring-buffer here that wraps two condition variables for blocking
// semantics, mirroring the design of stk::sync::Pipe.
//
// Two independent memory regions are managed:
//
//   Control block (cb_mem / cb_size in osMessageQueueAttr_t):
//     Policy identical to all other object types — PlacementNewOrHeap selects
//     placement-new into caller memory when cb_mem != nullptr and cb_size is
//     sufficient, otherwise heap. m_cb_owned tracks whether to free on Delete().
//
//   Data buffer (mq_mem / mq_size in osMessageQueueAttr_t):
//     1. Caller-supplied: attr->mq_mem / attr->mq_size are used as-is when the
//        region is large enough to hold (msg_count * msg_size) bytes.
//        buffer_owned = false -> never freed on Delete().
//     2. Dynamic fallback: heap buffer of (msg_count * msg_size) bytes.
//        buffer_owned = true  -> freed in the destructor.
// ---------------------------------------------------------------------------

struct StkMessageQueue
{
    const char   *m_name;
    uint32_t      m_msg_size;      // bytes per message
    uint32_t      m_capacity;      // max message count
    uint32_t      m_count;         // current message count
    uint32_t      m_head;          // write index (in messages)
    uint32_t      m_tail;          // read  index (in messages)
    uint8_t      *m_buffer;        // flat byte buffer [capacity * msg_size]
    bool          m_buffer_owned;  // true  -> buffer was heap-allocated, free in dtor
                                   // false -> buffer is caller-supplied, do NOT free
    bool          m_cb_owned;      // true  -> control block was heap-allocated (delete on Delete())
                                   // false -> placement-new in caller memory (call dtor only)

    stk::sync::Mutex             m_mutex;
    stk::sync::ConditionVariable m_cv_not_empty;
    stk::sync::ConditionVariable m_cv_not_full;

    // Construct with a caller-supplied data buffer (buffer_owned = false).
    explicit StkMessageQueue(uint32_t cap, uint32_t msz, const char *n,
                             uint8_t *ext_buf)
        : m_name(n), m_msg_size(msz), m_capacity(cap),
          m_count(0U), m_head(0U), m_tail(0U),
          m_buffer(ext_buf), m_buffer_owned(false), m_cb_owned(true)
    {
        m_mutex.SetTraceName(n);
    }

    // Construct with a heap-allocated data buffer (buffer_owned = true).
    explicit StkMessageQueue(uint32_t cap, uint32_t msz, const char *n)
        : m_name(n), m_msg_size(msz), m_capacity(cap),
          m_count(0U), m_head(0U), m_tail(0U),
          m_buffer(new (std::nothrow) uint8_t[cap * msz]), m_buffer_owned(true),
          m_cb_owned(true)
    {
        m_mutex.SetTraceName(n);
    }

    ~StkMessageQueue()
    {
        if (m_buffer_owned)
            delete[] m_buffer;
    }

    // Non-copyable
    StkMessageQueue(const StkMessageQueue &)            = delete;
    StkMessageQueue &operator=(const StkMessageQueue &) = delete;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Placement-new helper: construct T in caller-supplied memory when the block
// is large enough; otherwise fall back to heap (operator new).
// Sets obj->m_cb_owned = false for caller-supplied, true for heap.
// Returns nullptr if heap fallback is needed but allocation fails.
template <typename T, typename... Args>
static T *PlacementNewOrHeap(void *cb_mem, uint32_t cb_size, Args &&...args)
{
    if ((cb_mem != nullptr) && (cb_size >= sizeof(T)))
    {
        T *obj = new (cb_mem) T(static_cast<Args &&>(args)...);
        obj->m_cb_owned = false;
        return obj;
    }
    T *obj = new (std::nothrow) T(static_cast<Args &&>(args)...);
    // m_cb_owned is already true from the constructor default
    return obj;
}

// Destroy an object created by PlacementNewOrHeap:
//   - m_cb_owned == false -> call destructor only (memory is caller's)
//   - m_cb_owned == true  -> delete (destructor + free)
template <typename T>
static void ObjDestroy(T *obj)
{
    if (!obj) return;
    if (obj->m_cb_owned)
        delete obj;
    else
        obj->~T();
}

// Helper: map CMSIS flags options -> STK EventFlags options bitmask.
static __stk_forceinline uint32_t CmsisFlagsOptionsToStk(uint32_t options)
{
    uint32_t stk_opts = stk::sync::EventFlags::OPT_WAIT_ANY; // default

    if (options & osFlagsWaitAll)
        stk_opts |= stk::sync::EventFlags::OPT_WAIT_ALL;

    if (options & osFlagsNoClear)
        stk_opts |= stk::sync::EventFlags::OPT_NO_CLEAR;

    return stk_opts;
}

// Helper: map STK EventFlags error sentinel -> CMSIS flags error code.
static __stk_forceinline uint32_t StkFlagsResultToCmsis(uint32_t result)
{
    if (!stk::sync::EventFlags::IsError(result))
        return result;

    if (result == stk::sync::EventFlags::ERROR_TIMEOUT)
        return osFlagsErrorTimeout;

    if (result == stk::sync::EventFlags::ERROR_PARAMETER)
        return osFlagsErrorParameter;

    if (result == stk::sync::EventFlags::ERROR_ISR)
        return osFlagsErrorISR;

    return osFlagsErrorUnknown;
}

// ===========================================================================
// ==== Kernel Management Functions ====
// ===========================================================================

osStatus_t osKernelInitialize(void)
{
    if (IsIrqContext())
        return osErrorISR;

    if (g_KernelState != osKernelInactive)
        return osError;

    g_StkKernel = new (g_KernelBuf) StkKernel();
    g_StkKernel->Initialize(); // default 1 ms tick resolution

    g_KernelState = osKernelReady;
    return osOK;
}

osStatus_t osKernelGetInfo(osVersion_t *version, char *id_buf, uint32_t id_size)
{
    if (version != nullptr)
    {
        version->api    = STK_WRAPPER_API_VERSION;
        version->kernel = STK_WRAPPER_KERNEL_VERSION;
    }

    if ((id_buf != nullptr) && (id_size > 0U))
    {
        const char *id = STK_WRAPPER_KERNEL_ID;
        size_t copy_len = id_size - 1U;
        size_t id_len   = __builtin_strlen(id);
        if (copy_len > id_len) copy_len = id_len;
        memcpy(id_buf, id, copy_len);
        id_buf[copy_len] = '\0';
    }

    return osOK;
}

osKernelState_t osKernelGetState(void)
{
    if (g_StkKernel == nullptr)
        return osKernelInactive;

    switch (g_StkKernel->GetState())
    {
        case stk::IKernel::STATE_INACTIVE: return (g_KernelState == osKernelReady)
                                                  ? osKernelReady : osKernelInactive;
        case stk::IKernel::STATE_READY:    return osKernelReady;
        case stk::IKernel::STATE_RUNNING:  return osKernelRunning;
        default:                           return osKernelError;
    }
}

osStatus_t osKernelStart(void)
{
    if (IsIrqContext())
        return osErrorISR;

    if (g_KernelState != osKernelReady)
        return osError;

    g_KernelState = osKernelRunning;

    // Start() does not return for KERNEL_STATIC;
    // for KERNEL_DYNAMIC it returns when all tasks exit.
    g_StkKernel->Start();
    return osOK;
}

int32_t osKernelLock(void)
{
    // STK does not expose a scheduler-suspend API; use critical section.
    // Return 0 (was unlocked) as a conservative answer.
    if (IsIrqContext())
        return static_cast<int32_t>(osErrorISR);
    if (g_StkKernel == nullptr)
        return static_cast<int32_t>(osError);

    stk::hw::CriticalSection::Enter();
    return 0;
}

int32_t osKernelUnlock(void)
{
    if (IsIrqContext())
        return static_cast<int32_t>(osErrorISR);
    if (g_StkKernel == nullptr)
        return static_cast<int32_t>(osError);

    stk::hw::CriticalSection::Exit();
    return 0;
}

int32_t osKernelRestoreLock(int32_t lock)
{
    if (IsIrqContext())
        return static_cast<int32_t>(osErrorISR);
    if (g_StkKernel == nullptr)
        return static_cast<int32_t>(osError);

    if (lock == 1)
        stk::hw::CriticalSection::Enter();
    else
        stk::hw::CriticalSection::Exit();

    return lock;
}

uint32_t osKernelSuspend(void)
{
    // Not supported in STK.
    return 0U;
}

void osKernelResume(uint32_t /*sleep_ticks*/)
{
    // Not supported in STK.
}

uint32_t osKernelGetTickCount(void)
{
    if (g_StkKernel == nullptr)
        return 0U;

    return static_cast<uint32_t>(stk::GetTicks());
}

uint32_t osKernelGetTickFreq(void)
{
    if (g_StkKernel == nullptr)
        return 1000U; // default 1 kHz

    int32_t res_us = stk::GetTickResolution(); // us per tick
    if (res_us <= 0)
        return 1000U;

    return (1000000U / static_cast<uint32_t>(res_us));
}

uint32_t osKernelGetSysTimerCount(void)
{
    // Return tick count as a sys-timer approximation (no hardware cycle counter).
    return osKernelGetTickCount();
}

uint32_t osKernelGetSysTimerFreq(void)
{
    return osKernelGetTickFreq();
}

// ===========================================================================
// ==== Thread Management Functions ====
// ===========================================================================

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
    if (IsIrqContext() || (func == nullptr) || (g_StkKernel == nullptr))
        return nullptr;

    bool     joinable = attr && (attr->attr_bits & osThreadJoinable);
    void    *cb_mem   = (attr ? attr->cb_mem  : nullptr);
    uint32_t cb_size  = (attr ? attr->cb_size : 0U);

    StkThread *t = PlacementNewOrHeap<StkThread>(cb_mem, cb_size);
    if (t == nullptr)
        return nullptr;

    t->m_func       = func;
    t->m_argument   = argument;
    t->m_name       = nullptr;
    t->m_join_state = (joinable ? StkThread::JoinState::Joinable : StkThread::JoinState::Detached);

    osPriority_t cmsis_prio = osPriorityNormal;
    size_t stack_words      = CMSIS_STK_DEFAULT_STACK_WORDS;

    if (attr)
    {
        t->m_name = attr->name;

        if (attr->priority != osPriorityNone)
            cmsis_prio = attr->priority;

        if ((cmsis_prio < osPriorityIdle) || (cmsis_prio > osPriorityISR))
        {
            ObjDestroy(t);
            return nullptr;
        }

        // Stack: prefer caller-provided memory.
        if ((attr->stack_mem != nullptr) && (attr->stack_size > 0U))
        {
            size_t words = attr->stack_size / sizeof(stk::Word);
            if (words < CMSIS_STK_MIN_STACK_WORDS)
                words = CMSIS_STK_MIN_STACK_WORDS;

            t->m_stack       = static_cast<stk::Word *>(attr->stack_mem);
            t->m_stack_size  = words;
            t->m_stack_owned = false;
        }
        else
        if (attr->stack_size > 0U)
        {
            stack_words = attr->stack_size / sizeof(stk::Word);
            if (stack_words < CMSIS_STK_MIN_STACK_WORDS)
                stack_words = CMSIS_STK_MIN_STACK_WORDS;
        }
    }

    // Allocate stack if not caller-provided.
    if (t->m_stack == nullptr)
    {
        t->m_stack = new (std::nothrow) stk::Word[stack_words];
        if (t->m_stack == nullptr)
        {
            ObjDestroy(t);
            return nullptr;
        }
        t->m_stack_size  = stack_words;
        t->m_stack_owned = true;
    }

    t->m_stk_priority = CmsisPrioToStk(cmsis_prio);

    g_StkKernel->AddTask(t);

    return static_cast<osThreadId_t>(t);
}

const char *osThreadGetName(osThreadId_t thread_id)
{
    if (thread_id == nullptr)
        return nullptr;

    return static_cast<StkThread *>(thread_id)->m_name;
}

osThreadId_t osThreadGetId(void)
{
    if ((g_StkKernel == nullptr) || IsIrqContext())
        return nullptr;

    // STK's GetTid() returns the ITask pointer cast to Word.
    stk::TId tid = stk::GetTid();

    // tid is hw::PtrToWord(this) where 'this' is the StkThread* - cast it back.
    return reinterpret_cast<osThreadId_t>(static_cast<void *>(
        reinterpret_cast<StkThread *>(static_cast<uintptr_t>(tid))));
}

osThreadState_t osThreadGetState(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr))
        return osThreadError;

    StkThread *t = static_cast<StkThread *>(thread_id);

    if (t->m_suspended)
        return osThreadBlocked;

    if (thread_id == osThreadGetId())
        return osThreadRunning;

    return osThreadReady;
}

uint32_t osThreadGetStackSize(osThreadId_t thread_id)
{
    if (thread_id == nullptr)
        return 0U;

    return static_cast<uint32_t>(static_cast<StkThread *>(thread_id)->GetStackSizeBytes());
}

uint32_t osThreadGetStackSpace(osThreadId_t thread_id)
{
    if (thread_id == nullptr)
        return 0U;

    StkThread *t = static_cast<StkThread *>(thread_id);
    const stk::Word *stack = t->GetStack();
    size_t sz = t->GetStackSize();

    // Count leading Words still equal to STK_STACK_MEMORY_FILLER (watermark).
    size_t free_words = 0U;
    for (size_t i = 0U; i < sz; ++i)
    {
        if (stack[i] == STK_STACK_MEMORY_FILLER)
            ++free_words;
        else
            break;
    }

    return static_cast<uint32_t>(free_words * sizeof(stk::Word));
}

osStatus_t osThreadSetPriority(osThreadId_t thread_id, osPriority_t priority)
{
    if (IsIrqContext() || (thread_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    if ((priority < osPriorityIdle) || (priority > osPriorityISR))
        return osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);
    t->m_stk_priority = CmsisPrioToStk(priority);

    // Note: STK fixed-priority strategy reads GetWeight() each scheduling
    // tick, so updating m_stk_priority takes effect immediately.
    return osOK;
}

osPriority_t osThreadGetPriority(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr))
        return osPriorityError;

    StkThread *t = static_cast<StkThread *>(thread_id);
    return StkPrioToCmsis(t->m_stk_priority);
}

osStatus_t osThreadYield(void)
{
    if (IsIrqContext())
        return osErrorISR;
    if (g_StkKernel == nullptr)
        return osError;

    stk::Yield();
    return osOK;
}

osStatus_t osThreadSuspend(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr) || (g_StkKernel == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    g_StkKernel->SuspendTask(t, t->m_suspended);

    return osOK;
}

osStatus_t osThreadResume(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr) || (g_StkKernel == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    stk::hw::CriticalSection::ScopedLock cs_;

    if (!t->m_suspended)
        return osOK;  // not suspended, nothing to do

    g_StkKernel->ResumeTask(t);
    t->m_suspended = false;

    return osOK;
}

osStatus_t osThreadDetach(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    t->m_join_mutex.Lock();

    switch (t->m_join_state)
    {
    case StkThread::JoinState::Detached:
        // Already detached — CMSIS spec says this is an error.
        t->m_join_mutex.Unlock();
        return osError;

    case StkThread::JoinState::Joined:
        // Already joined — cannot detach.
        t->m_join_mutex.Unlock();
        return osError;

    case StkThread::JoinState::Exited:
        // Thread finished but nobody joined yet.
        // Transition to Detached and free the control block now,
        // since no joiner will ever do it.
        t->m_join_state = StkThread::JoinState::Detached;
        t->m_join_mutex.Unlock();
        ObjDestroy(t);           // safe: task slot already freed by kernel
        return osOK;

    case StkThread::JoinState::Joinable:
        // Normal case: thread is still running or just hasn't been joined.
        t->m_join_state = StkThread::JoinState::Detached;
        t->m_join_mutex.Unlock();
        return osOK;
    }

    t->m_join_mutex.Unlock();
    return osError;
}

osStatus_t osThreadJoin(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    // Self-join is undefined behavior per POSIX / CMSIS spec.
    if (thread_id == osThreadGetId())
        return osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    // critical section
    {
        stk::sync::Mutex::ScopedLock guard(t->m_join_mutex);

        // Only joinable threads can be joined.
        if (t->m_join_state == StkThread::JoinState::Detached)
            return osError;

        // Double-join: second caller always gets an error.
        if (t->m_join_state == StkThread::JoinState::Joined)
            return osError;

        t->m_join_state = StkThread::JoinState::Joined;

        // Block until OnExit() fires (transitions state to Exited).
        // m_join_cv.Wait() atomically releases m_join_mutex and suspends.
        while (t->m_join_state == StkThread::JoinState::Joined)
        {
            // WAIT_INFINITE — CMSIS osThreadJoin has no timeout parameter.
            t->m_join_cv.Wait(t->m_join_mutex, stk::WAIT_INFINITE);
        }

        // At this point m_join_state == Exited (or Detached if someone
        // raced osThreadDetach — treat that as an error).
        if (t->m_join_state != StkThread::JoinState::Exited)
            return osError;
    }

    // Free the control block — the kernel has already freed the slot.
    ObjDestroy(t);

    return osOK;
}

__NO_RETURN void osThreadExit(void)
{
    StkThread *t = static_cast<StkThread *>(osThreadGetId());

    g_StkKernel->ScheduleTaskRemoval(t);

    // In KERNEL_DYNAMIC mode, returning from Run() terminates the task.
    // Since we cannot return from a C function, spin-yield until the
    // kernel cleans us up (the task should have already returned from Run).
    for (;;)
        stk::Yield();
}

osStatus_t osThreadTerminate(osThreadId_t thread_id)
{
    if ((thread_id == nullptr) || (g_StkKernel == nullptr))
        return osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    stk::hw::CriticalSection::ScopedLock cs_;

    // RemoveTask triggers the STATE_REMOVE_PENDING path in the kernel,
    // which will call OnExit() before freeing the slot.
    g_StkKernel->ScheduleTaskRemoval(t);

    // For detached threads, free immediately (no joiner expected).
    // For joinable threads, OnExit() will wake the joiner; the joiner
    // calls ObjDestroy(). Do NOT free here.
    {
        if (t->m_join_state == StkThread::JoinState::Detached)
            ObjDestroy(t);
        // else: joiner owns the lifetime
    }

    return osOK;
}

uint32_t osThreadGetCount(void)
{
    if (g_StkKernel == nullptr)
        return 0U;

    return static_cast<uint32_t>(g_StkKernel->GetSwitchStrategy()->GetSize());
}

uint32_t osThreadEnumerate(osThreadId_t * /*thread_array*/, uint32_t /*array_items*/)
{
    // Not supported – STK has no public task enumeration API.
    return 0U;
}

// ===========================================================================
// ==== Thread Flags Functions ====
// ===========================================================================

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
    if ((thread_id == nullptr) || ((flags & osFlagsError) != 0))
        return osFlagsErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    uint32_t result = t->m_thread_flags.Set(flags);
    return StkFlagsResultToCmsis(result);
}

uint32_t osThreadFlagsClear(uint32_t flags)
{
    osThreadId_t self = osThreadGetId();
    if (self == nullptr)
        return osFlagsErrorUnknown;

    StkThread *t = static_cast<StkThread *>(self);

    uint32_t result = t->m_thread_flags.Clear(flags);
    return StkFlagsResultToCmsis(result);
}

uint32_t osThreadFlagsGet(void)
{
    osThreadId_t self = osThreadGetId();
    if (self == nullptr)
        return 0U;

    return static_cast<StkThread *>(self)->m_thread_flags.Get();
}

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
    if (IsIrqContext())
        return osFlagsErrorISR;

    osThreadId_t self = osThreadGetId();
    if (!self)
        return osFlagsErrorUnknown;

    StkThread *t = static_cast<StkThread *>(self);

    uint32_t result = t->m_thread_flags.Wait(flags, CmsisFlagsOptionsToStk(options),
        CmsisTimeoutToStk(timeout));

    return StkFlagsResultToCmsis(result);
}


// ===========================================================================
// ==== Generic Wait Functions ====
// ===========================================================================

osStatus_t osDelay(uint32_t ticks)
{
    if (IsIrqContext())
        return osErrorISR;
    if (g_StkKernel == nullptr)
        return osError;

    stk::Timeout timeout = CmsisTimeoutToStk(ticks);

    if (timeout == 0U)
    {
        stk::Yield();
        return osOK;
    }

    stk::Sleep(timeout);
    return osOK;
}

osStatus_t osDelayUntil(uint32_t ticks)
{
    if (IsIrqContext())
        return osErrorISR;
    if (g_StkKernel == nullptr)
        return osError;

    stk::Ticks now   = stk::GetTicks();
    stk::Ticks until = static_cast<stk::Ticks>(ticks);

    // If the deadline has already passed, just yield.
    if (until <= now)
    {
        stk::Yield();
        return osOK;
    }

    stk::SleepUntil(until);
    return osOK;
}

// ===========================================================================
// ==== Timer Management Functions ====
// ===========================================================================

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void *argument,
                       const osTimerAttr_t *attr)
{
    if (IsIrqContext() || (func == nullptr) || (g_StkKernel == nullptr))
        return nullptr;

    if (!EnsureTimerHostCreated())
        return nullptr;

    const char *name   = (attr ? attr->name    : nullptr);
    void       *cb_mem = (attr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr ? attr->cb_size : 0U);

    StkTimer *timer = PlacementNewOrHeap<StkTimer>(cb_mem, cb_sz, func, type, argument, name);
    return static_cast<osTimerId_t>(timer);
}

const char *osTimerGetName(osTimerId_t timer_id)
{
    if (timer_id == nullptr)
        return nullptr;

    return static_cast<StkTimer *>(timer_id)->m_name;
}

osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks)
{
    if (IsIrqContext() || (timer_id == nullptr) || (g_TimerHost == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkTimer *timer = static_cast<StkTimer *>(timer_id);

    uint32_t period = (timer->m_type == osTimerPeriodic) ? ticks : 0U;
    timer->m_period_ticks = period;

    bool ok = g_TimerHost->Restart(*timer, ticks, period);
    return (ok ? osOK : osError);
}

osStatus_t osTimerStop(osTimerId_t timer_id)
{
    if (IsIrqContext() || (timer_id == nullptr) || (g_TimerHost == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkTimer *timer = static_cast<StkTimer *>(timer_id);

    if (!timer->IsActive())
        return osErrorResource;

    bool ok = g_TimerHost->Stop(*timer);
    return (ok ? osOK : osError);
}

uint32_t osTimerIsRunning(osTimerId_t timer_id)
{
    if (timer_id == nullptr)
        return 0U;

    return (static_cast<StkTimer *>(timer_id)->IsActive() ? 1U : 0U);
}

osStatus_t osTimerDelete(osTimerId_t timer_id)
{
    if (IsIrqContext() || (timer_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkTimer *timer = static_cast<StkTimer *>(timer_id);

    if ((g_TimerHost != nullptr) && timer->IsActive())
        g_TimerHost->Stop(*timer);

    ObjDestroy(timer);
    return osOK;
}

// ===========================================================================
// ==== Event Flags Management Functions ====
// ===========================================================================

osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr)
{
    if (IsIrqContext())
        return nullptr;

    const char *name   = (attr ? attr->name    : nullptr);
    void       *cb_mem = (attr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr ? attr->cb_size : 0U);

    StkEventFlags *ef = PlacementNewOrHeap<StkEventFlags>(cb_mem, cb_sz, name);
    return static_cast<osEventFlagsId_t>(ef);
}

const char *osEventFlagsGetName(osEventFlagsId_t ef_id)
{
    if (ef_id == nullptr)
        return nullptr;

    return static_cast<StkEventFlags *>(ef_id)->m_ef.GetTraceName();
}

uint32_t osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags)
{
    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0))
        return osFlagsErrorParameter;

    uint32_t result = static_cast<StkEventFlags *>(ef_id)->m_ef.Set(flags);
    return StkFlagsResultToCmsis(result);
}

uint32_t osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags)
{
    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0))
        return osFlagsErrorParameter;

    uint32_t result = static_cast<StkEventFlags *>(ef_id)->m_ef.Clear(flags);
    return StkFlagsResultToCmsis(result);
}

uint32_t osEventFlagsGet(osEventFlagsId_t ef_id)
{
    if (ef_id == nullptr)
        return 0U;

    return static_cast<StkEventFlags *>(ef_id)->m_ef.Get();
}

uint32_t osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags, uint32_t options,
                          uint32_t timeout)
{
    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0))
        return osFlagsErrorParameter;

    uint32_t result = static_cast<StkEventFlags *>(ef_id)->m_ef.Wait(flags,
        CmsisFlagsOptionsToStk(options), CmsisTimeoutToStk(timeout));

    return StkFlagsResultToCmsis(result);
}

osStatus_t osEventFlagsDelete(osEventFlagsId_t ef_id)
{
    if (IsIrqContext() || (ef_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    ObjDestroy(static_cast<StkEventFlags *>(ef_id));
    return osOK;
}

// ===========================================================================
// ==== Mutex Management Functions ====
// ===========================================================================

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    if (IsIrqContext())
        return nullptr;

    // osMutexPrioInherit / osMutexRobust: accepted but silently ignored.
    // osMutexRecursive: STK Mutex is always recursive.
    const char *name   = (attr ? attr->name    : nullptr);
    void       *cb_mem = (attr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr ? attr->cb_size : 0U);

    StkMutex *m = PlacementNewOrHeap<StkMutex>(cb_mem, cb_sz, name);
    return static_cast<osMutexId_t>(m);
}

const char *osMutexGetName(osMutexId_t mutex_id)
{
    if (mutex_id == nullptr)
        return nullptr;

    return static_cast<StkMutex *>(mutex_id)->m_mutex.GetTraceName();
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    if (IsIrqContext())
        return osErrorISR;
    if (mutex_id == nullptr)
        return osErrorParameter;

    StkMutex *m = static_cast<StkMutex *>(mutex_id);
    stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

    bool acquired = m->m_mutex.TimedLock(stk_timeout);
    if (!acquired)
        return ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);

    return osOK;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    if (IsIrqContext())
        return osErrorISR;
    if (mutex_id == nullptr)
        return osErrorParameter;

    static_cast<StkMutex *>(mutex_id)->m_mutex.Unlock();
    return osOK;
}

osThreadId_t osMutexGetOwner(osMutexId_t mutex_id)
{
    if (mutex_id == nullptr)
        return nullptr;

    stk::TId tid = static_cast<StkMutex *>(mutex_id)->m_mutex.GetOwner();

    // tid is hw::PtrToWord(this) where 'this' is the StkThread* - cast it back.
    return reinterpret_cast<osThreadId_t>(static_cast<void *>(
        reinterpret_cast<StkThread *>(static_cast<uintptr_t>(tid))));
}

osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
    if (IsIrqContext() || (mutex_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    ObjDestroy(static_cast<StkMutex *>(mutex_id));
    return osOK;
}

// ===========================================================================
// ==== Semaphore Management Functions ====
// ===========================================================================

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
                               const osSemaphoreAttr_t *attr)
{
    if (IsIrqContext())
        return nullptr;

    if ((max_count == 0U) || (initial_count > max_count))
        return nullptr;

    // STK Semaphore uses uint16_t counters; clamp to 0xFFFE.
    uint16_t mc = (max_count     > 0xFFFEU) ? static_cast<uint16_t>(0xFFFEU) : static_cast<uint16_t>(max_count);
    uint16_t ic = (initial_count > 0xFFFEU) ? static_cast<uint16_t>(0xFFFEU) : static_cast<uint16_t>(initial_count);

    const char *name   = (attr ? attr->name    : nullptr);
    void       *cb_mem = (attr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr ? attr->cb_size : 0U);

    StkSemaphore *s = PlacementNewOrHeap<StkSemaphore>(cb_mem, cb_sz, ic, mc, name);
    return static_cast<osSemaphoreId_t>(s);
}

const char *osSemaphoreGetName(osSemaphoreId_t semaphore_id)
{
    if (semaphore_id == nullptr)
        return nullptr;

    return static_cast<StkSemaphore *>(semaphore_id)->m_semaphore.GetTraceName();
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
    if (semaphore_id == nullptr)
        return osErrorParameter;
    if (IsIrqContext() && (timeout != 0U))
        return osErrorISR;

    StkSemaphore *s = static_cast<StkSemaphore *>(semaphore_id);
    stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

    bool acquired = s->m_semaphore.Wait(stk_timeout);
    if (!acquired)
        return (stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout;

    return osOK;
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
    if (semaphore_id == nullptr)
        return osErrorParameter;

    static_cast<StkSemaphore *>(semaphore_id)->m_semaphore.Signal();
    return osOK;
}

uint32_t osSemaphoreGetCount(osSemaphoreId_t semaphore_id)
{
    if (semaphore_id == nullptr)
        return 0U;

    return static_cast<uint32_t>(static_cast<StkSemaphore *>(semaphore_id)->m_semaphore.GetCount());
}

osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)
{
    if (IsIrqContext() || (semaphore_id == nullptr))
        return (IsIrqContext() ? osErrorISR : osErrorParameter);

    ObjDestroy(static_cast<StkSemaphore *>(semaphore_id));
    return osOK;
}


// ===========================================================================
// ==== Memory Pool Management Functions ====
// (Not natively supported by STK; stubs return error / NULL.)
// ===========================================================================

osMemoryPoolId_t osMemoryPoolNew(uint32_t, uint32_t, const osMemoryPoolAttr_t *)
{
    return nullptr;
}

const char *osMemoryPoolGetName(osMemoryPoolId_t)
{
    return nullptr;
}

void *osMemoryPoolAlloc(osMemoryPoolId_t, uint32_t)
{
    return nullptr;
}

osStatus_t osMemoryPoolFree(osMemoryPoolId_t, void *)
{
    return osError;
}

uint32_t osMemoryPoolGetCapacity(osMemoryPoolId_t)  { return 0U; }
uint32_t osMemoryPoolGetBlockSize(osMemoryPoolId_t) { return 0U; }
uint32_t osMemoryPoolGetCount(osMemoryPoolId_t)     { return 0U; }
uint32_t osMemoryPoolGetSpace(osMemoryPoolId_t)     { return 0U; }

osStatus_t osMemoryPoolDelete(osMemoryPoolId_t)
{
    return osError;
}


// ===========================================================================
// ==== Message Queue Management Functions ====
// ===========================================================================

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
                                     const osMessageQueueAttr_t *attr)
{
    if (IsIrqContext() || (msg_count == 0U) || (msg_size == 0U))
        return nullptr;

    const char *name         = (attr ? attr->name    : nullptr);
    void       *cb_mem       = (attr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz        = (attr ? attr->cb_size : 0U);
    void       *ext_buf      = (attr ? attr->mq_mem  : nullptr);
    uint32_t    ext_buf_size = (attr ? attr->mq_size : 0U);

    const uint32_t buf_required = msg_count * msg_size;

    StkMessageQueue *mq;

    if ((ext_buf != nullptr) && (ext_buf_size >= buf_required))
    {
        // Data buffer: use caller-supplied memory (buffer_owned = false).
        // Control block: placement-new into cb_mem if provided, else heap.
        mq = PlacementNewOrHeap<StkMessageQueue>(cb_mem, cb_sz,
            msg_count, msg_size, name, static_cast<uint8_t *>(ext_buf));
    }
    else
    {
        // Data buffer: heap-allocated (buffer_owned = true).
        // Control block: placement-new into cb_mem if provided, else heap.
        mq = PlacementNewOrHeap<StkMessageQueue>(cb_mem, cb_sz,
            msg_count, msg_size, name);

        if ((mq != nullptr) && (mq->m_buffer == nullptr))
        {
            ObjDestroy(mq);
            return nullptr;
        }
    }

    return static_cast<osMessageQueueId_t>(mq);
}

const char *osMessageQueueGetName(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return nullptr;

    return static_cast<StkMessageQueue *>(mq_id)->m_name;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
                             uint8_t /*msg_prio*/, uint32_t timeout)
{
    if (!mq_id || !msg_ptr)
        return osErrorParameter;
    if (IsIrqContext() && (timeout != 0U))
        return osErrorISR;

    StkMessageQueue *mq = static_cast<StkMessageQueue *>(mq_id);
    stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

    stk::sync::Mutex::ScopedLock guard(mq->m_mutex);

    while (mq->m_count >= mq->m_capacity)
    {
        if (!mq->m_cv_not_full.Wait(mq->m_mutex, stk_timeout))
            return ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);
    }

    // Copy message into ring buffer.
    uint8_t *dst = mq->m_buffer + (mq->m_head * mq->m_msg_size);
    memcpy(dst, msg_ptr, mq->m_msg_size);
    mq->m_head = (mq->m_head + 1U) % mq->m_capacity;
    mq->m_count++;

    mq->m_cv_not_empty.NotifyOne();

    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
                             uint8_t *msg_prio, uint32_t timeout)
{
    if (!mq_id || !msg_ptr)
        return osErrorParameter;
    if (IsIrqContext() && (timeout != 0U))
        return osErrorISR;

    StkMessageQueue *mq = static_cast<StkMessageQueue *>(mq_id);
    stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

    stk::sync::Mutex::ScopedLock guard(mq->m_mutex);

    while (mq->m_count == 0U)
    {
        if (!mq->m_cv_not_empty.Wait(mq->m_mutex, stk_timeout))
            return ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);
    }

    // Copy message out of ring buffer.
    const uint8_t *src = mq->m_buffer + (mq->m_tail * mq->m_msg_size);
    memcpy(msg_ptr, src, mq->m_msg_size);
    mq->m_tail = (mq->m_tail + 1U) % mq->m_capacity;
    mq->m_count--;

    if (msg_prio)
        *msg_prio = 0U; // STK queues have no priority lanes.

    mq->m_cv_not_full.NotifyOne();

    return osOK;
}

uint32_t osMessageQueueGetCapacity(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    return static_cast<StkMessageQueue *>(mq_id)->m_capacity;
}

uint32_t osMessageQueueGetMsgSize(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    return static_cast<StkMessageQueue *>(mq_id)->m_msg_size;
}

uint32_t osMessageQueueGetCount(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    return static_cast<StkMessageQueue *>(mq_id)->m_count;
}

uint32_t osMessageQueueGetSpace(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    StkMessageQueue *mq = static_cast<StkMessageQueue *>(mq_id);
    return (mq->m_capacity - mq->m_count);
}

osStatus_t osMessageQueueReset(osMessageQueueId_t mq_id)
{
    if (IsIrqContext() || (mq_id == nullptr))
        return (IsIrqContext() ? osErrorISR : osErrorParameter);

    StkMessageQueue *mq = static_cast<StkMessageQueue *>(mq_id);

    stk::sync::Mutex::ScopedLock guard(mq->m_mutex);

    mq->m_count = 0U;
    mq->m_head  = 0U;
    mq->m_tail  = 0U;

    mq->m_cv_not_full.NotifyAll();

    return osOK;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
    if (IsIrqContext() || (mq_id == nullptr))
        return (IsIrqContext() ? osErrorISR : osErrorParameter);

    ObjDestroy(static_cast<StkMessageQueue *>(mq_id));
    return osOK;
}
