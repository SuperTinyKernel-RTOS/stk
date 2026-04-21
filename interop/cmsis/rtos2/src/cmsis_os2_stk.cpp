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
 *                            GetCount / Terminate / Suspend / Resume, Join, Detach)
 *   - Thread flags          (osThreadFlagsSet / Clear / Get / Wait)
 *   - Event flags           (osEventFlagsNew / Delete / Set / Clear / Get / Wait)
 *   - Mutex                 (osMutexNew / Delete / Acquire / Release / GetOwner)
 *   - Semaphore             (osSemaphoreNew / Delete / Acquire / Release / GetCount)
 *   - Timer                 (osTimerNew / Delete / Start / Stop / IsRunning)
 *   - Message Queue         (osMessageQueueNew / Delete / Put / Get /
 *                            GetCapacity / GetMsgSize / GetCount / GetSpace / Reset)
 *   - Memory Pool           (osMemoryPoolNew / Delete / Alloc / Free /
 *                            GetCapacity / GetBlockSize / GetCount / GetSpace /
 *                            GetName)
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
 *   - Message queues are backed by stk::sync::MessageQueue, STK's native
 *     fixed-capacity, fixed-message-size ring-buffer with integrated blocking
 *     semantics (Put/Get with configurable timeouts, ISR-safe TryPut/TryGet).
 *
 * Limitations / deviations from the specification:
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
#include "sync/stk_sync.h"
#include "time/stk_time.h"
#include "memory/stk_memory.h"

// ---------------------------------------------------------------------------
// Kernel version / identification
// ---------------------------------------------------------------------------

#define STK_WRAPPER_API_VERSION     20030000UL // 2.3.0
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
template <typename T> static constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1) / sizeof(stk::Word));
}

// Private memory allocators.
void *stk::memory::MemoryAllocator::Allocate(size_t size)
{
    return malloc(size);
}
void stk::memory::MemoryAllocator::Free(void *ptr)
{
    free(ptr);
}

// ---------------------------------------------------------------------------
// Priority mapping:
//   CMSIS range: osPriorityIdle(1) .. osPriorityISR(56)  ->  57 levels
//   STK FP32 range: 0 .. 31                              ->  32 levels
// Linear map: stk_prio = (cmsis_prio * 31) / 56
// ---------------------------------------------------------------------------
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
using StkKernel = stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC
#if STK_TICKLESS_IDLE
    | stk::KERNEL_TICKLESS
#endif
    , CMSIS_STK_MAX_THREADS, stk::SwitchStrategyFP32, stk::PlatformDefault>;

static StkKernel g_StkKernel;
static uint32_t  g_StkKernelLocked = 0;

// ---------------------------------------------------------------------------
// Thread control block
//
// StkThread wraps a single CMSIS thread.
//
//   Stack memory is provided either by the caller (static allocation) or
//   allocated dynamically from operator new[] (heap allocation).
//
//   The CMSIS function pointer + argument are stored here; the STK task's
//   Run() method simply calls func(argument).
//
//   Priority is stored as STK weight (0..31) and returned via GetWeight().
// ---------------------------------------------------------------------------

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
          m_stack_owned(false), m_suspended(false), m_cb_owned(true)
    {}

    virtual ~StkThread()
    {
        if (m_stack_owned && (m_stack != nullptr))
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
    const char *GetTraceName()       const override { return m_name; }

    // ---- Members ----
    osThreadFunc_t               m_func;
    void                        *m_argument;
    const char                  *m_name;
    volatile int32_t             m_stk_priority; // STK priority level 0..31
    stk::Word                   *m_stack;        // pointer to stack memory (may be owned)
    size_t                       m_stack_size;   // stack size in Words
    volatile JoinState           m_join_state;   // guarded by m_join_mutex between other tasks
    stk::sync::ConditionVariable m_join_cv;      // signaled in OnExit()
    stk::sync::EventFlags        m_thread_flags; // Per-thread event flags - backed by STK's native 32-bit EventFlags primitive.
    bool                         m_stack_owned;  // true if we allocated the stack ourselves
    bool                         m_suspended;    // true if suspended
    bool                         m_cb_owned;     // true -> heap-allocated control block, delete on Terminate(), false -> caller-supplied cb_mem, call destructor explicitly
};

// ---------------------------------------------------------------------------
// Mutex control block
// ---------------------------------------------------------------------------

struct StkMutex
{
    explicit StkMutex(const char *n = nullptr) : m_mutex(), m_cb_owned(true)
    {
        m_mutex.SetTraceName(n);
    }

    // ---- Members ----
    stk::sync::Mutex m_mutex;
    bool             m_cb_owned; // true -> heap-allocated, false -> placement-new in caller memory
};

// ---------------------------------------------------------------------------
// Semaphore control block
// ---------------------------------------------------------------------------

struct StkSemaphore
{
    explicit StkSemaphore(uint16_t initial, uint16_t max_count, const char *n = nullptr)
        : m_semaphore(initial, max_count), m_cb_owned(true)
    {
        m_semaphore.SetTraceName(n);
    }

    // ---- Members ----
    stk::sync::Semaphore m_semaphore;
    bool                 m_cb_owned; // true -> heap-allocated, false -> placement-new in caller memory
};

// ---------------------------------------------------------------------------
// Event flags control block
//
// Backed directly by stk::sync::EventFlags - STK's native 32-bit multi-flag
// synchronization primitive (Set/Clear/Get/Wait with ANY/ALL/NO_CLEAR options,
// ISR-safe Set/Clear, absolute-deadline wait loop).
// ---------------------------------------------------------------------------

struct StkEventFlags
{
    explicit StkEventFlags(const char *n = nullptr) : m_ef(0U), m_cb_owned(true)
    {
        m_ef.SetTraceName(n);
    }

    // ---- Members ----
    stk::sync::EventFlags m_ef;
    bool                  m_cb_owned; // true -> heap-allocated, false -> placement-new in caller memory
};

// ---------------------------------------------------------------------------
// Timer control block
//
// CMSIS timers are backed by stk::time::TimerHost.
// A single global TimerHost is created on first osTimerNew().
// ---------------------------------------------------------------------------

static stk::time::TimerHost *g_TimerHost = nullptr;
static stk::Word             g_TimerHostBuf[StkGetWordCountForType<stk::time::TimerHost>()];

struct StkTimer : public stk::time::TimerHost::Timer
{
    explicit StkTimer(osTimerFunc_t f, osTimerType_t t, void *arg, const char *n)
        : m_func(f), m_argument(arg), m_type(t), m_name(n), m_period_ticks(0U), m_cb_owned(true)
    {}
    virtual ~StkTimer()
    {}

    void OnExpired(stk::time::TimerHost * /*host*/) override
    {
        m_func(m_argument);
    }

    static bool EnsureTimerHostCreated()
    {
        if (g_TimerHost == nullptr)
        {
            g_TimerHost = new (g_TimerHostBuf) stk::time::TimerHost();
            g_TimerHost->Initialize(&g_StkKernel, stk::ACCESS_PRIVILEGED);
        }

        return true;
    }

    // ---- Members ----
    osTimerFunc_t  m_func;
    void          *m_argument;
    osTimerType_t  m_type;
    const char    *m_name;
    uint32_t       m_period_ticks; // stored period for restart
    bool           m_cb_owned;     // true -> heap-allocated, false -> placement-new in caller memory
};

// ---------------------------------------------------------------------------
// Memory pool control block
//
// Backed directly by stk::memory::BlockMemoryPool.
// ---------------------------------------------------------------------------

class StkMemPool
{
public:
    // Construct with caller-supplied pool storage (storage_owned = false).
    explicit StkMemPool(uint32_t cap, uint32_t raw_block_size,
                        const char *name, uint8_t *ext_storage)
        : m_mpool(static_cast<size_t>(cap),
                  static_cast<size_t>(raw_block_size),
                  ext_storage,
                  cap * stk::memory::BlockMemoryPool::AlignBlockSize(raw_block_size),
                  name),
          m_cb_owned(true)
    {}

    // Construct with heap-allocated pool storage (storage_owned = true).
    explicit StkMemPool(uint32_t cap, uint32_t raw_block_size, const char *name)
        : m_mpool(static_cast<size_t>(cap),
                  static_cast<size_t>(raw_block_size),
                  name),
          m_cb_owned(true)
    {}

    // ---- Members ----
    stk::memory::BlockMemoryPool m_mpool;
    bool                         m_cb_owned; // true -> heap-allocated; false -> placement-new in caller memory
};

// ---------------------------------------------------------------------------
// Message queue control block
//
// Backed by stk::sync::MessageQueue - STK's native fixed-capacity, fixed-
// message-size FIFO ring-buffer with integrated blocking semantics (Put/Get
// with WAIT_INFINITE / NO_WAIT / timed variants, ISR-safe TryPut/TryGet).
//
// Two independent memory regions are managed:
//
//   Control block (cb_mem / cb_size in osMessageQueueAttr_t):
//     Policy identical to all other object types - PlacementNewOrHeap selects
//     placement-new into caller memory when cb_mem != nullptr and cb_size is
//     sufficient, otherwise heap. m_cb_owned tracks whether to free on Delete().
//
//   Data buffer (mq_mem / mq_size in osMessageQueueAttr_t):
//     1. Caller-supplied: attr->mq_mem / attr->mq_size are used as-is when the
//        region is large enough to hold (msg_count * msg_size) bytes.
//     2. Dynamic fallback: heap buffer of (msg_count * msg_size) bytes.
//        Allocated buffer is freed in the destructor.
// ---------------------------------------------------------------------------

struct StkMessageQueue
{
    // Construct with a caller-supplied data buffer.
    explicit StkMessageQueue(uint32_t cap, uint32_t msz, const char *name,uint8_t *ext_buf)
        : m_mq(ext_buf, static_cast<size_t>(cap), static_cast<size_t>(msz)),
          m_bf_owned(false), m_cb_owned(true)
    {
        m_mq.SetTraceName(name);
    }

    // Construct with a heap-allocated data buffer.
    explicit StkMessageQueue(uint32_t cap, uint32_t msz, const char *name)
        : m_mq(AllocBuffer(cap, msz), static_cast<size_t>(cap), static_cast<size_t>(msz)),
          m_bf_owned(m_mq.IsStorageValid()), m_cb_owned(true)
    {
        m_mq.SetTraceName(name);
    }

    ~StkMessageQueue()
    {
        if (m_bf_owned)
            delete[] m_mq.GetBuffer();
    }

    static uint8_t *AllocBuffer(uint32_t cap, uint32_t msz)
    {
        return new (std::nothrow) uint8_t[static_cast<size_t>(cap) * msz];
    }

    // ---- Members ----
    stk::sync::MessageQueue m_mq;       // STK native message queue (owns blocking semantics)
    bool                    m_bf_owned; // true  -> when we heap-allocated the data buffer, false otherwise
    bool                    m_cb_owned; // true  -> heap-allocated control block (delete on Delete())
                                        // false -> placement-new in caller memory (call dtor only)
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Placement-new helper: construct T in caller-supplied memory when the block
// is large enough; otherwise fall back to heap (operator new).
// Sets obj->m_cb_owned = false for caller-supplied, true for heap.
// Returns nullptr if heap fallback is needed but allocation fails.
template <typename T, typename... Args>
static T *PlacementNewOrHeap(void *cb_mem, size_t cb_size, Args &&...args)
{
    T *obj = nullptr;

    if ((cb_mem != nullptr) && (cb_size >= sizeof(T)))
    {
        obj = new (cb_mem) T(static_cast<Args &&>(args)...);
        obj->m_cb_owned = false;
        return obj;
    }
    else
    {
        obj = new (std::nothrow) T(static_cast<Args &&>(args)...);
        // m_cb_owned is already true from the constructor default
    }

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

    if (osKernelGetState() != osKernelInactive)
        return osError;

    g_StkKernel.Initialize(); // default 1 ms tick resolution
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
        if (copy_len > id_len)
            copy_len = id_len;

        memcpy(id_buf, id, copy_len);
        id_buf[copy_len] = '\0';
    }

    return osOK;
}

osKernelState_t osKernelGetState(void)
{
    if (g_StkKernelLocked != 0U)
        return osKernelLocked;

    switch (g_StkKernel.GetState())
    {
    case stk::IKernel::STATE_INACTIVE:  return osKernelInactive;
    case stk::IKernel::STATE_READY:     return osKernelReady;
    case stk::IKernel::STATE_RUNNING:   return osKernelRunning;
    case stk::IKernel::STATE_SUSPENDED: return osKernelSuspended;
    default:                            return osKernelError;
    }
}

osStatus_t osKernelStart(void)
{
    if (IsIrqContext())
        return osErrorISR;

    if (osKernelGetState() != osKernelReady)
        return osError;

    // Start() does not return for KERNEL_STATIC;
    // for KERNEL_DYNAMIC it returns when all tasks exit.
    g_StkKernel.Start();
    return osOK;
}

int32_t osKernelLock(void)
{
    if (IsIrqContext())
        return static_cast<int32_t>(osErrorISR);

    stk::hw::CriticalSection::Enter();
    ++g_StkKernelLocked;
    return 0;
}

int32_t osKernelUnlock(void)
{
    if (IsIrqContext())
        return static_cast<int32_t>(osErrorISR);
    if (g_StkKernelLocked == 0U)
        return osErrorResource;

    --g_StkKernelLocked;
    stk::hw::CriticalSection::Exit();
    return 0;
}

int32_t osKernelRestoreLock(int32_t lock)
{
    if (IsIrqContext())
        return static_cast<int32_t>(osErrorISR);

    if (lock == 1)
    {
        stk::hw::CriticalSection::Enter();
        ++g_StkKernelLocked;
    }
    else
    {
        if (g_StkKernelLocked == 0U)
            return osErrorResource;

        --g_StkKernelLocked;
        stk::hw::CriticalSection::Exit();
    }

    return lock;
}

uint32_t osKernelSuspend(void)
{
#if STK_TICKLESS_IDLE
    if (osKernelGetState() == osKernelInactive)
        return 0U;

    return stk::IKernelService::GetInstance()->Suspend();
#else
    // Not supported in non-tickless kernel.
    return 0U;
#endif
}

void osKernelResume(uint32_t sleep_ticks)
{
#if STK_TICKLESS_IDLE
    if (osKernelGetState() != osKernelInactive)
        return stk::IKernelService::GetInstance()->Resume(sleep_ticks);
#else
    // Not supported in non-tickless kernel.
    STK_UNUSED(sleep_ticks);
#endif
}

uint32_t osKernelGetTickCount(void)
{
    if (osKernelGetState() == osKernelInactive)
        return 0U;

    return static_cast<uint32_t>(stk::GetTicks());
}

uint32_t osKernelGetTickFreq(void)
{
    if (osKernelGetState() == osKernelInactive)
        return 1000U; // default 1 kHz

    int32_t res_us = stk::GetTickResolution(); // us per tick
    if (res_us <= 0)
        return 1000U;

    return (1000000U / static_cast<uint32_t>(res_us));
}

uint32_t osKernelGetSysTimerCount(void)
{
    return static_cast<uint32_t>(stk::GetSysTimerCount());
}

uint64_t osKernelGetSysTimerCount64(void)
{
    return stk::GetSysTimerCount();
}

uint32_t osKernelGetSysTimerFreq(void)
{
    return stk::GetSysTimerFrequency();
}


// ===========================================================================
// ==== Thread Management Functions ====
// ===========================================================================

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr)
{
    if (IsIrqContext() || (func == nullptr) || (osKernelGetState() == osKernelInactive))
        return nullptr;

    bool     joinable = (attr != nullptr) && (attr->attr_bits & osThreadJoinable);
    void    *cb_mem   = (attr != nullptr ? attr->cb_mem  : nullptr);
    uint32_t cb_size  = (attr != nullptr ? attr->cb_size : 0U);

    StkThread *t = PlacementNewOrHeap<StkThread>(cb_mem, cb_size);
    if (t == nullptr)
        return nullptr;

    t->m_func       = func;
    t->m_argument   = argument;
    t->m_name       = nullptr;
    t->m_join_state = (joinable ? StkThread::JoinState::Joinable : StkThread::JoinState::Detached);

    osPriority_t cmsis_prio = osPriorityNormal;
    size_t stack_words      = CMSIS_STK_DEFAULT_STACK_WORDS;

    if (attr != nullptr)
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

    g_StkKernel.AddTask(t);

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
    if ((osKernelGetState() == osKernelInactive) || IsIrqContext())
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

    StkThread *t = static_cast<StkThread *>(thread_id);
    return static_cast<uint32_t>(t->GetStackSizeBytes());
}

uint32_t osThreadGetStackSpace(osThreadId_t thread_id)
{
    if (thread_id == nullptr)
        return 0U;

    StkThread *t = static_cast<StkThread *>(thread_id);
    return static_cast<uint32_t>(t->GetStackSpace() * sizeof(stk::Word));
}

osStatus_t osThreadSetPriority(osThreadId_t thread_id, osPriority_t priority)
{
    if (IsIrqContext() || (thread_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    if ((priority < osPriorityIdle) || (priority > osPriorityISR))
        return osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);
    t->m_stk_priority = CmsisPrioToStk(priority);

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
    if (osKernelGetState() == osKernelInactive)
        return osError;

    stk::Yield();
    return osOK;
}

osStatus_t osThreadSuspend(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr) || (osKernelGetState() == osKernelInactive))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    g_StkKernel.SuspendTask(t, t->m_suspended);

    return osOK;
}

osStatus_t osThreadResume(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr) || (osKernelGetState() == osKernelInactive))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    stk::sync::ScopedCriticalSection cs_;

    if (!t->m_suspended)
        return osOK; // not suspended, nothing to do

    g_StkKernel.ResumeTask(t);
    t->m_suspended = false;

    return osOK;
}

osStatus_t osThreadDetach(osThreadId_t thread_id)
{
    if (IsIrqContext() || (thread_id == nullptr))
        return IsIrqContext() ? osErrorISR : osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    stk::sync::ScopedCriticalSection cs_;

    switch (t->m_join_state)
    {
    case StkThread::JoinState::Detached:
        // already detached - CMSIS spec says this is an error
        return osError;

    case StkThread::JoinState::Joined:
        // already joined - cannot detach
        return osError;

    case StkThread::JoinState::Exited:
        // thread finished but nobody joined yet, transition to Detached
        // and free the control block now, since no joiner will ever do it
        t->m_join_state = StkThread::JoinState::Detached;
        ObjDestroy(t); // safe: task slot already freed by the kernel
        return osOK;

    case StkThread::JoinState::Joinable:
        // normal case: thread is still running or just hasn't been joined
        t->m_join_state = StkThread::JoinState::Detached;
        return osOK;
    }

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

    stk::sync::ScopedCriticalSection cs_;

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
        // WAIT_INFINITE - CMSIS osThreadJoin has no timeout parameter.
        t->m_join_cv.Wait(cs_, stk::WAIT_INFINITE);
    }

    // At this point m_join_state == Exited (or Detached if someone
    // raced osThreadDetach - treat that as an error).
    if (t->m_join_state != StkThread::JoinState::Exited)
        return osError;

    // Free the control block - the kernel has already freed the slot.
    ObjDestroy(t);

    return osOK;
}

__NO_RETURN void osThreadExit(void)
{
    StkThread *t = static_cast<StkThread *>(osThreadGetId());

    g_StkKernel.ScheduleTaskRemoval(t);

    // Wait for removal.
    for (;;)
        stk::Yield();
}

osStatus_t osThreadTerminate(osThreadId_t thread_id)
{
    if ((thread_id == nullptr) || (osKernelGetState() == osKernelInactive))
        return osErrorParameter;

    StkThread *t = static_cast<StkThread *>(thread_id);

    stk::sync::ScopedCriticalSection cs_;

    // RemoveTask triggers the STATE_REMOVE_PENDING path in the kernel,
    // which will call OnExit() before freeing the slot.
    g_StkKernel.ScheduleTaskRemoval(t);

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
    if (osKernelGetState() == osKernelInactive)
        return 0U;

    // avoid race with OnTick
    stk::sync::ScopedCriticalSection cs_;

    return static_cast<uint32_t>(g_StkKernel.GetSwitchStrategy()->GetSize());
}

uint32_t osThreadEnumerate(osThreadId_t *thread_array, uint32_t array_items)
{
    if (osKernelGetState() == osKernelInactive)
        return 0U;

    // osThreadId_t maps directly to stk::ITask (see StkThread)
    return g_StkKernel.EnumerateTasks(reinterpret_cast<stk::ITask **>(thread_array), array_items);
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
    if (self == nullptr)
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
    if (osKernelGetState() == osKernelInactive)
        return osError;

    stk::Timeout timeout = CmsisTimeoutToStk(ticks);

    stk::Sleep(timeout);
    return osOK;
}

osStatus_t osDelayUntil(uint32_t ticks)
{
    if (IsIrqContext())
        return osErrorISR;
    if (osKernelGetState() == osKernelInactive)
        return osError;

    stk::SleepUntil(static_cast<stk::Ticks>(ticks));
    return osOK;
}


// ===========================================================================
// ==== Timer Management Functions ====
// ===========================================================================

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void *argument,
                       const osTimerAttr_t *attr)
{
    if (IsIrqContext() || (func == nullptr) || (osKernelGetState() == osKernelInactive))
        return nullptr;

    if (!StkTimer::EnsureTimerHostCreated())
        return nullptr;

    const char *name   = (attr != nullptr ? attr->name    : nullptr);
    void       *cb_mem = (attr != nullptr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr != nullptr ? attr->cb_size : 0U);

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

    const char *name   = (attr != nullptr ? attr->name    : nullptr);
    void       *cb_mem = (attr != nullptr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr != nullptr ? attr->cb_size : 0U);

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
    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0U))
        return osFlagsErrorParameter;

    uint32_t result = static_cast<StkEventFlags *>(ef_id)->m_ef.Set(flags);
    return StkFlagsResultToCmsis(result);
}

uint32_t osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags)
{
    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0U))
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
    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0U))
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
    const char *name   = (attr != nullptr ? attr->name    : nullptr);
    void       *cb_mem = (attr != nullptr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr != nullptr ? attr->cb_size : 0U);

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

    // STK Semaphore uses uint16_t counters, clamp to stk::sync::Semaphore::COUNT_MAX.
    uint16_t mc = stk::Min(max_count, static_cast<uint32_t>(stk::sync::Semaphore::COUNT_MAX));
    uint16_t ic = stk::Min(initial_count, static_cast<uint32_t>(stk::sync::Semaphore::COUNT_MAX));

    const char *name   = (attr != nullptr ? attr->name    : nullptr);
    void       *cb_mem = (attr != nullptr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz  = (attr != nullptr ? attr->cb_size : 0U);

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
// ===========================================================================

osMemoryPoolId_t osMemoryPoolNew(uint32_t block_count, uint32_t block_size,
                                 const osMemoryPoolAttr_t *attr)
{
    // ISR context: forbidden per CMSIS spec.
    if (IsIrqContext())
        return nullptr;

    // Zero capacity or zero block size are meaningless.
    if ((block_count == 0U) || (block_size == 0U))
        return nullptr;

    const char *name    = (attr != nullptr ? attr->name    : nullptr);
    void       *cb_mem  = (attr != nullptr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz   = (attr != nullptr ? attr->cb_size : 0U);
    void       *mp_mem  = (attr != nullptr ? attr->mp_mem  : nullptr);
    uint32_t    mp_sz   = (attr != nullptr ? attr->mp_size : 0U);

    // Compute the aligned block size and required storage byte count.
    const uint32_t aligned_blk       = stk::memory::BlockMemoryPool::AlignBlockSize(block_size);
    const uint32_t storage_required  = (block_count * aligned_blk);

    StkMemPool *pool = nullptr;

    if ((mp_mem != nullptr) && (mp_sz >= storage_required))
    {
        // Caller-supplied pool storage - BlockMemoryPool external-storage ctor.
        pool = PlacementNewOrHeap<StkMemPool>(cb_mem, cb_sz,
            block_count, block_size, name, static_cast<uint8_t *>(mp_mem));
    }
    else
    {
        // Heap-allocated pool storage - BlockMemoryPool heap ctor.
        pool = PlacementNewOrHeap<StkMemPool>(cb_mem, cb_sz,
            block_count, block_size, name);

        // If the heap ctor failed to allocate storage, clean up and bail.
        if ((pool != nullptr) && !pool->m_mpool.IsStorageValid())
        {
            ObjDestroy(pool);
            return nullptr;
        }
    }

    return static_cast<osMemoryPoolId_t>(pool);
}

const char *osMemoryPoolGetName(osMemoryPoolId_t mp_id)
{
    if (mp_id == nullptr)
        return nullptr;

    return static_cast<StkMemPool *>(mp_id)->m_mpool.GetTraceName();
}

void *osMemoryPoolAlloc(osMemoryPoolId_t mp_id, uint32_t timeout)
{
    if (mp_id == nullptr)
        return nullptr;

    // ISR context is only valid with timeout == 0 (NO_WAIT / TryAlloc).
    if (IsIrqContext() && (timeout != 0U))
        return nullptr;

    return static_cast<StkMemPool *>(mp_id)->m_mpool.TimedAlloc(CmsisTimeoutToStk(timeout));
}

osStatus_t osMemoryPoolFree(osMemoryPoolId_t mp_id, void *block)
{
    if ((mp_id == nullptr) || (block == nullptr))
        return osErrorParameter;

    if (!static_cast<StkMemPool *>(mp_id)->m_mpool.Free(block))
        return osErrorParameter; // ptr not from this pool

    return osOK;
}

uint32_t osMemoryPoolGetCapacity(osMemoryPoolId_t mp_id)
{
    if (mp_id == nullptr)
        return 0U;

    return static_cast<StkMemPool *>(mp_id)->m_mpool.GetCapacity();
}

uint32_t osMemoryPoolGetBlockSize(osMemoryPoolId_t mp_id)
{
    if (mp_id == nullptr)
        return 0U;

    return static_cast<uint32_t>(static_cast<StkMemPool *>(mp_id)->m_mpool.GetBlockSize());
}

uint32_t osMemoryPoolGetCount(osMemoryPoolId_t mp_id)
{
    if (mp_id == nullptr)
        return 0U;

    return static_cast<StkMemPool *>(mp_id)->m_mpool.GetUsedCount();
}

uint32_t osMemoryPoolGetSpace(osMemoryPoolId_t mp_id)
{
    if (mp_id == nullptr)
        return 0U;

    return static_cast<StkMemPool *>(mp_id)->m_mpool.GetFreeCount();
}

osStatus_t osMemoryPoolDelete(osMemoryPoolId_t mp_id)
{
    if (IsIrqContext() || (mp_id == nullptr))
        return (IsIrqContext() ? osErrorISR : osErrorParameter);

    ObjDestroy(static_cast<StkMemPool *>(mp_id));

    return osOK;
}


// ===========================================================================
// ==== Message Queue Management Functions ====
// ===========================================================================

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
                                     const osMessageQueueAttr_t *attr)
{
    if (IsIrqContext() || (msg_count == 0U) || (msg_size == 0U))
        return nullptr;

    if (msg_count > stk::sync::MessageQueue::CAPACITY_MAX)
        return nullptr;

    const char *name         = (attr != nullptr ? attr->name    : nullptr);
    void       *cb_mem       = (attr != nullptr ? attr->cb_mem  : nullptr);
    uint32_t    cb_sz        = (attr != nullptr ? attr->cb_size : 0U);
    void       *ext_buf      = (attr != nullptr ? attr->mq_mem  : nullptr);
    uint32_t    ext_buf_size = (attr != nullptr ? attr->mq_size : 0U);

    const uint32_t buf_required = msg_count * msg_size;

    StkMessageQueue *mq = nullptr;

    if ((ext_buf != nullptr) && (ext_buf_size >= buf_required))
    {
        // Data buffer: use caller-supplied memory.
        mq = PlacementNewOrHeap<StkMessageQueue>(cb_mem, cb_sz,
            msg_count, msg_size, name, static_cast<uint8_t *>(ext_buf));
    }
    else
    {
        // Data buffer: heap-allocated inside StkMessageQueue constructor.
        mq = PlacementNewOrHeap<StkMessageQueue>(cb_mem, cb_sz,
            msg_count, msg_size, name);

        if ((mq != nullptr) && (mq->m_mq.GetBuffer() == nullptr))
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

    return static_cast<StkMessageQueue *>(mq_id)->m_mq.GetTraceName();
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
                             uint8_t /*msg_prio*/, uint32_t timeout)
{
    if (!mq_id || !msg_ptr)
        return osErrorParameter;
    if (IsIrqContext() && (timeout != 0U))
        return osErrorISR;

    stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

    if (!static_cast<StkMessageQueue *>(mq_id)->m_mq.Put(msg_ptr, stk_timeout))
        return ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);

    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
                             uint8_t *msg_prio, uint32_t timeout)
{
    if (!mq_id || !msg_ptr)
        return osErrorParameter;
    if (IsIrqContext() && (timeout != 0U))
        return osErrorISR;

    stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

    if (!static_cast<StkMessageQueue *>(mq_id)->m_mq.Get(msg_ptr, stk_timeout))
        return ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);

    if (msg_prio)
        *msg_prio = 0U; // STK queues have no priority lanes.

    return osOK;
}

uint32_t osMessageQueueGetCapacity(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    return static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetCapacity());
}

uint32_t osMessageQueueGetMsgSize(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    return static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetMsgSize());
}

uint32_t osMessageQueueGetCount(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    return static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetCount());
}

uint32_t osMessageQueueGetSpace(osMessageQueueId_t mq_id)
{
    if (mq_id == nullptr)
        return 0U;

    return static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetSpace());
}

osStatus_t osMessageQueueReset(osMessageQueueId_t mq_id)
{
    if (IsIrqContext() || (mq_id == nullptr))
        return (IsIrqContext() ? osErrorISR : osErrorParameter);

    static_cast<StkMessageQueue *>(mq_id)->m_mq.Reset();

    return osOK;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
    if (IsIrqContext() || (mq_id == nullptr))
        return (IsIrqContext() ? osErrorISR : osErrorParameter);

    ObjDestroy(static_cast<StkMessageQueue *>(mq_id));

    return osOK;
}
