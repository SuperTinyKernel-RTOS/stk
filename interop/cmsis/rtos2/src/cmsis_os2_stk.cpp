/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include "stk.h"
#include "sync/stk_sync.h"
#include "time/stk_time.h"
#include "memory/stk_memory.h"

// See design notes, API coverage and other details in cmsis_os2.h.

#include "cmsis_os2.h"

// -----------------------------------------------------------------------------
// Kernel version / identification
// -----------------------------------------------------------------------------

#define STK_WRAPPER_API_VERSION     20030000UL // 2.3.0
#define STK_WRAPPER_KERNEL_VERSION  20030000UL
#define STK_WRAPPER_KERNEL_ID       "STK RTOS2 Wrapper v1.0"

// -----------------------------------------------------------------------------
// Kernel configuration
// -----------------------------------------------------------------------------

// Number of task slots in the global kernel instance.
// Increase if more concurrent threads are needed.
#ifndef CMSIS_STK_MAX_THREADS
#define CMSIS_STK_MAX_THREADS (16U)
#endif

// Default stack size (in Words) when the caller passes stack_size == 0.
#ifndef CMSIS_STK_DEFAULT_STACK_WORDS
#define CMSIS_STK_DEFAULT_STACK_WORDS (256U)
#endif

// Minimum stack size (in Words) – mirrors STK's own STACK_SIZE_MIN.
#define CMSIS_STK_MIN_STACK_WORDS (STK_STACK_SIZE_MIN)

// Returns a size of memory in stk::Word elements required for object allocation.
template <typename T> static constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1U) / sizeof(stk::Word));
}

// Custom strlen replacement.
static size_t CmsisStrlen(const char str[]) // MISRA: declared as array, not pointer to allow indexed access
{
    size_t total_len = 0U;
    
    while (str[total_len] != '\0')
    {
        total_len++;
    }
    
    return total_len;
}

// Private memory allocators (we define malloc, free here to overcome absence of declaration in
// case of -ffreestanding compiler flag).
extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);
void *stk::memory::MemoryAllocator::Allocate(size_t size) { return malloc(size); }
void stk::memory::MemoryAllocator::Free(void *ptr) { free(ptr); }

// -----------------------------------------------------------------------------
// Priority mapping:
//   CMSIS range: osPriorityIdle(1) .. osPriorityISR(56)  ->  57 levels
//   STK FP32 range: 0 .. 31                              ->  32 levels
// Linear map: stk_prio = (cmsis_prio * 31) / 56
// -----------------------------------------------------------------------------
static __stk_forceinline int32_t CmsisPrioToStk(osPriority_t p)
{
    int32_t result;

    if (p <= osPriorityIdle)
    {
        result = 0;
    }
    else if (p >= osPriorityISR)
    {
        result = 31;
    }
    else
    {
        result = ((static_cast<int32_t>(p) * 31) / 56);
    }

    return result;
}
static __stk_forceinline osPriority_t StkPrioToCmsis(int32_t p)
{
    // Inverse: cmsis_prio = (stk_prio * 56) / 31
    int32_t r = (p * 56) / 31;
    if (r < static_cast<int32_t>(osPriorityIdle))
    {
        r = static_cast<int32_t>(osPriorityIdle);
    }

    if (r > static_cast<int32_t>(osPriorityISR))
    {
        r = static_cast<int32_t>(osPriorityISR);
    }

    return static_cast<osPriority_t>(r);
}

// -----------------------------------------------------------------------------
// Convert CMSIS timeout (ticks) -> STK timeout (ticks / milliseconds).
//
// CMSIS ticks == STK ticks when tick resolution is 1 ms (PERIODICITY_DEFAULT).
// For other resolutions the conversion uses the kernel's tick resolution.
// osWaitForever -> WAIT_INFINITE.
// -----------------------------------------------------------------------------
static __stk_forceinline stk::Timeout CmsisTimeoutToStk(uint32_t ticks)
{
    stk::Timeout result;

    if (ticks > static_cast<uint32_t>(stk::WAIT_INFINITE))
    {
        result = stk::WAIT_INFINITE;
    }
    else if (ticks == 0U)
    {
        result = stk::NO_WAIT;
    }
    else
    {
        // CMSIS ticks are kernel ticks – pass through directly as STK ticks
        result = static_cast<stk::Timeout>(ticks);
    }

    return result;
}

// -----------------------------------------------------------------------------
// ISR context check
// -----------------------------------------------------------------------------
static __stk_forceinline bool IsIrqContext()
{
    return stk::hw::IsInsideISR();
}

// -----------------------------------------------------------------------------
// Global kernel type alias
// -----------------------------------------------------------------------------
using StkKernel = stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC
#if STK_TICKLESS_IDLE
    | stk::KERNEL_TICKLESS
#endif
    , CMSIS_STK_MAX_THREADS, stk::SwitchStrategyFP32, stk::PlatformDefault>;

static StkKernel g_StkKernel;
static uint32_t  g_StkKernelLocked = 0U;

// -----------------------------------------------------------------------------
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
// -----------------------------------------------------------------------------

class StkThread final : public stk::ITask
{
    STK_NONCOPYABLE_CLASS(StkThread);
    
public:
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
          m_stack(nullptr), m_stack_size(0U), m_join_state(JoinState::Detached),
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
    const stk::Word *GetStack()      const override { return m_stack; }
    size_t GetStackSize()            const override { return m_stack_size; }
    size_t GetStackSizeBytes()       const override { return m_stack_size * sizeof(stk::Word); }

    // ---- ITask ----
    stk::EAccessMode GetAccessMode() const override { return stk::ACCESS_PRIVILEGED; }
    void OnDeadlineMissed(uint32_t)  override       {}
    int32_t GetWeight()              const override { return m_stk_priority; }
    const char *GetTraceName()       const override { return m_name; }
    
    static inline osThreadId_t ConvertTIdToThreadId(const stk::TId tid)
    {
        // tid is hw::PtrToWord(this) where 'this' is the StkThread* - cast it back.
        return reinterpret_cast<osThreadId_t>(static_cast<void *>(
            reinterpret_cast<StkThread *>(static_cast<uintptr_t>(tid))));
    }

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

// -----------------------------------------------------------------------------
// Mutex control block
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Semaphore control block
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Event flags control block
//
// Backed directly by stk::sync::EventFlags - STK's native 32-bit multi-flag
// synchronization primitive (Set/Clear/Get/Wait with ANY/ALL/NO_CLEAR options,
// ISR-safe Set/Clear, absolute-deadline wait loop).
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Timer control block
//
// CMSIS timers are backed by stk::time::TimerHost.
// A single global TimerHost is created on first osTimerNew().
// -----------------------------------------------------------------------------

static stk::time::TimerHost *g_TimerHost = nullptr;
static stk::Word             g_TimerHostBuf[StkGetWordCountForType<stk::time::TimerHost>()];

class StkTimer final : public stk::time::TimerHost::Timer
{
    STK_NONCOPYABLE_CLASS(StkTimer);
  
public:
    explicit StkTimer(osTimerFunc_t const func, osTimerType_t tt, void *arg, const char *name)
        : m_func(func), m_argument(arg), m_type(tt), m_name(name), m_period_ticks(0U), m_cb_owned(true)
    {}
    virtual ~StkTimer() = default;

    void OnExpired(stk::time::TimerHost * /*host*/) override
    {
        m_func(m_argument);
    }

    static void EnsureTimerHostCreated()
    {
        if (g_TimerHost == nullptr)
        {
            g_TimerHost = new (g_TimerHostBuf) stk::time::TimerHost();
            g_TimerHost->Initialize(&g_StkKernel, stk::ACCESS_PRIVILEGED);
        }
    }

    // ---- Members ----
    osTimerFunc_t  m_func;
    void          *m_argument;
    osTimerType_t  m_type;
    const char    *m_name;
    uint32_t       m_period_ticks; // stored period for restart
    bool           m_cb_owned;     // true -> heap-allocated, false -> placement-new in caller memory
};

// -----------------------------------------------------------------------------
// Memory pool control block
//
// Backed directly by stk::memory::BlockMemoryPool.
// -----------------------------------------------------------------------------

class StkMemPool
{
    STK_NONCOPYABLE_CLASS(StkMemPool);
  
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

// -----------------------------------------------------------------------------
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
// -----------------------------------------------------------------------------

class StkMessageQueue
{
    STK_NONCOPYABLE_CLASS(StkMessageQueue);
  
public:
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
        {
            delete[] m_mq.GetBuffer();
        }
    }

    static uint8_t *AllocBuffer(uint32_t cap, uint32_t msz)
    {
        uint8_t *const ret = new (std::nothrow) uint8_t[static_cast<size_t>(cap) * msz];
        STK_ASSERT(ret != nullptr); // fail in Debug: increase Heap size in linker settings
        return ret;
    }
    
    // ---- Members ----
    stk::sync::MessageQueue m_mq;       // STK native message queue (owns blocking semantics)
    bool                    m_bf_owned; // true  -> when we heap-allocated the data buffer, false otherwise
    bool                    m_cb_owned; // true  -> heap-allocated control block (delete on Delete())
                                        // false -> placement-new in caller memory (call dtor only)
};

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

// Placement-new helper: construct T in caller-supplied memory when the block
// is large enough; otherwise fall back to heap (operator new).
// Sets obj->m_cb_owned = false for caller-supplied, true for heap.
// Returns nullptr if heap fallback is needed but allocation fails.
template <typename T, typename... Args>
static T *PlacementNewOrHeap(void *mem, size_t size, const Args &... args)
{
    T *obj;

    if ((mem != nullptr) && (size >= sizeof(T)))
    {
        obj = new (mem) T(args...);
        obj->m_cb_owned = false;
    }
    else
    {
        obj = new (std::nothrow) T(args...);
        // m_cb_owned is already true from the constructor default
        STK_ASSERT(obj != nullptr);
    }

    return obj;
}

// Destroy an object created by PlacementNewOrHeap:
//   - m_cb_owned == false -> call destructor only (memory is caller's)
//   - m_cb_owned == true  -> delete (destructor + free)
template <typename T>
static void ObjDestroy(T *obj)
{
    if (obj->m_cb_owned)
    {
        delete obj;
    }
    else
    {
        obj->~T();
    }
}

// Helper: map CMSIS flags options -> STK EventFlags options bitmask.
static __stk_forceinline uint32_t CmsisFlagsOptionsToStk(uint32_t options)
{
    uint32_t stk_opts = stk::sync::EventFlags::OPT_WAIT_ANY; // default

    if ((options & osFlagsWaitAll) != 0U)
    {
        stk_opts |= stk::sync::EventFlags::OPT_WAIT_ALL;
    }

    if ((options & osFlagsNoClear) != 0U)
    {
        stk_opts |= stk::sync::EventFlags::OPT_NO_CLEAR;
    }

    return stk_opts;
}

// Helper: map STK EventFlags error sentinel -> CMSIS flags error code.
static __stk_forceinline uint32_t StkFlagsResultToCmsis(uint32_t result)
{
    uint32_t cmsis_result;

    if (!stk::sync::EventFlags::IsError(result))
    {
        cmsis_result = result;
    }
    else if (result == stk::sync::EventFlags::ERROR_TIMEOUT)
    {
        cmsis_result = osFlagsErrorTimeout;
    }
    else if (result == stk::sync::EventFlags::ERROR_PARAMETER)
    {
        cmsis_result = osFlagsErrorParameter;
    }
    else if (result == stk::sync::EventFlags::ERROR_ISR)
    {
        cmsis_result = osFlagsErrorISR;
    }
    else
    {
        cmsis_result = osFlagsErrorUnknown;
    }

    return cmsis_result;
}


// ===========================================================================
// ==== Kernel Management Functions ====
// ===========================================================================

osStatus_t osKernelInitialize(void)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (osKernelGetState() != osKernelInactive)
    {
        result = osError;
    }
    else
    {
        g_StkKernel.Initialize(); // default 1 ms tick resolution
        result = osOK;
    }

    return result;
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
        const char *const id = STK_WRAPPER_KERNEL_ID;
        size_t copy_len      = id_size - 1U;
        const size_t id_len  = CmsisStrlen(id);
        if (copy_len > id_len)
        {
            copy_len = id_len;
        }

        STK_MEMCPY(id_buf, id, copy_len);
        id_buf[copy_len] = '\0';
    }

    return osOK;
}

osKernelState_t osKernelGetState(void)
{
    osKernelState_t state;

    if (g_StkKernelLocked != 0U)
    {
        state = osKernelLocked;
    }
    else
    {
        switch (g_StkKernel.GetState())
        {
        case stk::IKernel::KSTATE_INACTIVE:
            state = osKernelInactive;
            break;
        case stk::IKernel::KSTATE_READY:
            state = osKernelReady;
            break;
        case stk::IKernel::KSTATE_RUNNING:
            state = osKernelRunning;
            break;
        case stk::IKernel::KSTATE_SUSPENDED:
            state = osKernelSuspended;
            break;
        default:
            state = osKernelError;
            break;
        }
    }

    return state;
}

osStatus_t osKernelStart(void)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (osKernelGetState() != osKernelReady)
    {
        result = osError;
    }
    else
    {
        // Start() does not return for KERNEL_STATIC;
        // for KERNEL_DYNAMIC it returns when all tasks exit.
        g_StkKernel.Start();
        result = osOK;
    }

    return result;
}

int32_t osKernelLock(void)
{
    int32_t result;

    if (IsIrqContext())
    {
        result = static_cast<int32_t>(osErrorISR);
    }
    else
    {
        stk::hw::CriticalSection::Enter();
        ++g_StkKernelLocked;
        result = 0;
    }

    return result;
}

int32_t osKernelUnlock(void)
{
    int32_t result;

    if (IsIrqContext())
    {
        result = static_cast<int32_t>(osErrorISR);
    }
    else if (g_StkKernelLocked == 0U)
    {
        result = static_cast<int32_t>(osErrorResource);
    }
    else
    {
        --g_StkKernelLocked;
        stk::hw::CriticalSection::Exit();
        result = 0;
    }

    return result;
}

int32_t osKernelRestoreLock(int32_t lock)
{
    int32_t result;

    if (IsIrqContext())
    {
        result = static_cast<int32_t>(osErrorISR);
    }
    else if (lock == 1)
    {
        stk::hw::CriticalSection::Enter();
        ++g_StkKernelLocked;
        result = lock;
    }
    else if (g_StkKernelLocked == 0U)
    {
        result = static_cast<int32_t>(osErrorResource);
    }
    else
    {
        --g_StkKernelLocked;
        stk::hw::CriticalSection::Exit();
        result = lock;
    }

    return result;
}

uint32_t osKernelSuspend(void)
{
    uint32_t result;

#if STK_TICKLESS_IDLE
    if (osKernelGetState() == osKernelInactive)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(stk::IKernelService::GetInstance()->Suspend());
    }
#else
    // Not supported in non-tickless kernel.
    result = 0U;
#endif

    return result;
}

void osKernelResume(uint32_t sleep_ticks)
{
#if STK_TICKLESS_IDLE
    if (osKernelGetState() != osKernelInactive)
    {
        stk::IKernelService::GetInstance()->Resume(sleep_ticks);
    }
#else
    // Not supported in non-tickless kernel.
    STK_UNUSED(sleep_ticks);
#endif
}

uint32_t osKernelGetTickCount(void)
{
    uint32_t result;

    if (osKernelGetState() == osKernelInactive)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(stk::GetTicks());
    }

    return result;
}

uint32_t osKernelGetTickFreq(void)
{
    uint32_t result;

    if (osKernelGetState() == osKernelInactive)
    {
        result = 1000U; // default 1 kHz
    }
    else
    {
        const int32_t res_us = stk::GetTickResolution(); // us per tick
        if (res_us <= 0)
        {
            result = 1000U;
        }
        else
        {
            result = (1000000U / static_cast<uint32_t>(res_us));
        }
    }

    return result;
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
    osThreadId_t result = nullptr;

    const bool          is_irq         = IsIrqContext();
    const osKernelState_t kernel_state = osKernelGetState();

    // validate environment and arguments
    bool is_valid = (!is_irq && (func != nullptr) && (kernel_state != osKernelInactive));

    // validate priority value
    osPriority_t cmsis_prio = osPriorityNormal;
    if (is_valid && (attr != nullptr))
    {
        if (attr->priority != osPriorityNone)
        {
            cmsis_prio = attr->priority;
        }

        if ((cmsis_prio < osPriorityIdle) || (cmsis_prio > osPriorityISR))
        {
            is_valid = false; // invalidate object creation
        }
    }

    if (is_valid)
    {
        const bool    is_joinable = (attr != nullptr) && ((attr->attr_bits & osThreadJoinable) != 0U);
        void *const   cb_mem      = ((attr != nullptr) ? attr->cb_mem  : nullptr);
        const uint32_t cb_size    = ((attr != nullptr) ? attr->cb_size : 0U);

        StkThread *th = PlacementNewOrHeap<StkThread>(cb_mem, cb_size);
        if (th != nullptr)
        {
            th->m_func       = func;
            th->m_argument   = argument;
            th->m_name       = (attr != nullptr) ? attr->name : nullptr;
            th->m_join_state = (is_joinable ? StkThread::JoinState::Joinable : StkThread::JoinState::Detached);

            // stack configuration
            if (attr != nullptr)
            {
                if ((attr->stack_mem != nullptr) && (attr->stack_size != 0U))
                {
                    th->m_stack       = static_cast<stk::Word *>(attr->stack_mem);
                    th->m_stack_size  = stk::Max<size_t>(attr->stack_size / sizeof(stk::Word), CMSIS_STK_MIN_STACK_WORDS);
                    th->m_stack_owned = false;
                }
            }

            // allocate stack memory if not caller-provided
            if (th->m_stack == nullptr)
            {
                const size_t stack_words = CMSIS_STK_DEFAULT_STACK_WORDS;

                th->m_stack = new (std::nothrow) stk::Word[stack_words];
                STK_ASSERT(th->m_stack != nullptr);
                
                if (th->m_stack == nullptr)
                {
                    ObjDestroy(th);
                    th = nullptr;
                }
                else
                {
                    th->m_stack_size  = stack_words;
                    th->m_stack_owned = true;
                }
            }

            // finalize and register the thread
            if (th != nullptr)
            {
                th->m_stk_priority = CmsisPrioToStk(cmsis_prio);
                g_StkKernel.AddTask(th);
                result = static_cast<osThreadId_t>(th);
            }
        }
    }

    return result; // The solitary exit point
}

const char *osThreadGetName(osThreadId_t thread_id)
{
    const char *result;

    if (thread_id == nullptr)
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkThread *>(thread_id)->m_name;
    }

    return result;
}

osThreadId_t osThreadGetId(void)
{
    osThreadId_t threadId = nullptr;

    const osKernelState_t kernel_state = osKernelGetState();
    const bool            is_irq       = IsIrqContext();

    if ((kernel_state != osKernelInactive) && !is_irq)
    {
        // STK's GetTid() returns the ITask pointer cast to Word.
        threadId = StkThread::ConvertTIdToThreadId(stk::GetTid());
    }

    return threadId;
}

osThreadState_t osThreadGetState(osThreadId_t thread_id)
{
    osThreadState_t threadState = osThreadError;

    if (!IsIrqContext() && (thread_id != nullptr))
    {
        const StkThread *const th = static_cast<StkThread *>(thread_id);

        if (th->m_suspended)
        {
            threadState = osThreadBlocked;
        }
        else if (thread_id == osThreadGetId())
        {
            threadState = osThreadRunning;
        }
        else
        {
            threadState = osThreadReady;
        }
    }

    return threadState;
}

uint32_t osThreadGetStackSize(osThreadId_t thread_id)
{
    uint32_t result;

    if (thread_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);
        result = static_cast<uint32_t>(th->GetStackSizeBytes());
    }

    return result;
}

uint32_t osThreadGetStackSpace(osThreadId_t thread_id)
{
    uint32_t result;

    if (thread_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);
        result = static_cast<uint32_t>(th->GetStackSpace() * sizeof(stk::Word));
    }

    return result;
}

osStatus_t osThreadSetPriority(osThreadId_t thread_id, osPriority_t priority)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (thread_id == nullptr)
    {
        result = osErrorParameter;
    }
    else if ((priority < osPriorityIdle) || (priority > osPriorityISR))
    {
        result = osErrorParameter;
    }
    else
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);
        th->m_stk_priority = CmsisPrioToStk(priority);
        result = osOK;
    }

    return result;
}

osPriority_t osThreadGetPriority(osThreadId_t thread_id)
{
    osPriority_t result;

    if (IsIrqContext() || (thread_id == nullptr))
    {
        result = osPriorityError;
    }
    else
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);
        result = StkPrioToCmsis(th->m_stk_priority);
    }

    return result;
}

osStatus_t osThreadYield(void)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (osKernelGetState() == osKernelInactive)
    {
        result = osError;
    }
    else
    {
        stk::Yield();
        result = osOK;
    }

    return result;
}

osStatus_t osThreadSuspend(osThreadId_t thread_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (thread_id == nullptr)
    {
        result = osErrorParameter;
    }
    else if (osKernelGetState() == osKernelInactive)
    {
        result = osErrorParameter;
    }
    else
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);
        g_StkKernel.SuspendTask(th, th->m_suspended);
        result = osOK;
    }

    return result;
}

osStatus_t osThreadResume(osThreadId_t thread_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (thread_id == nullptr)
    {
        result = osErrorParameter;
    }
    else if (osKernelGetState() == osKernelInactive)
    {
        result = osErrorParameter;
    }
    else
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);

        const stk::sync::ScopedCriticalSection cs_;

        if (!th->m_suspended)
        {
            result = osOK; // not suspended, nothing to do
        }
        else
        {
            g_StkKernel.ResumeTask(th);
            th->m_suspended = false;
            result = osOK;
        }
    }

    return result;
}

osStatus_t osThreadDetach(osThreadId_t thread_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (thread_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);

        const stk::sync::ScopedCriticalSection cs_;

        switch (th->m_join_state)
        {
        case StkThread::JoinState::Detached:
            // already detached - CMSIS spec says this is an error
            result = osError;
            break;

        case StkThread::JoinState::Joined:
            // already joined - cannot detach
            result = osError;
            break;

        case StkThread::JoinState::Exited:
            // thread finished but nobody joined yet, transition to Detached
            // and free the control block now, since no joiner will ever do it
            th->m_join_state = StkThread::JoinState::Detached;
            ObjDestroy(th); // safe: task slot already freed by the kernel
            result = osOK;
            break;

        case StkThread::JoinState::Joinable:
            // normal case: thread is still running or just hasn't been joined
            th->m_join_state = StkThread::JoinState::Detached;
            result = osOK;
            break;

        default:
            result = osError;
            break;
        }
    }

    return result;
}

osStatus_t osThreadJoin(osThreadId_t thread_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (thread_id == nullptr)
    {
        result = osErrorParameter;
    }
    // Self-join is undefined behavior per POSIX / CMSIS spec.
    else if (thread_id == osThreadGetId())
    {
        result = osErrorParameter;
    }
    else
    {
        StkThread *th = static_cast<StkThread *>(thread_id);

        stk::sync::ScopedCriticalSection cs_;

        // Only joinable threads can be joined.
        if (th->m_join_state == StkThread::JoinState::Detached)
        {
            result = osError;
        }
        // Double-join: second caller always gets an error.
        else if (th->m_join_state == StkThread::JoinState::Joined)
        {
            result = osError;
        }
        else
        {
            th->m_join_state = StkThread::JoinState::Joined;

            // Block until OnExit() fires (transitions state to Exited).
            // m_join_cv.Wait() atomically releases m_join_mutex and suspends.
            while (th->m_join_state == StkThread::JoinState::Joined)
            {
                // WAIT_INFINITE - CMSIS osThreadJoin has no timeout parameter.
                STK_UNUSED(th->m_join_cv.Wait(cs_, stk::WAIT_INFINITE));
            }

            // At this point m_join_state == Exited (or Detached if someone
            // raced osThreadDetach - treat that as an error).
            if (th->m_join_state != StkThread::JoinState::Exited)
            {
                result = osError;
            }
            else
            {
                // Free the control block - the kernel has already freed the slot.
                ObjDestroy(th);
                result = osOK;
            }
        }
    }

    return result;
}

__NO_RETURN void osThreadExit(void)
{
    StkThread *const th = static_cast<StkThread *>(osThreadGetId());

    g_StkKernel.ScheduleTaskRemoval(th);

    // Wait for removal.
    for (;;)
    {
        stk::Yield();
    }
}

osStatus_t osThreadTerminate(osThreadId_t thread_id)
{
    osStatus_t status = osErrorParameter;
    const bool is_active = (osKernelGetState() != osKernelInactive);

    if ((thread_id != nullptr) && !is_active)
    {
        StkThread *const th = static_cast<StkThread *>(thread_id);

        // avoid race conditions during termination
        const stk::sync::ScopedCriticalSection cs_;

        // RemoveTask triggers the STATE_REMOVE_PENDING path in the kernel,
        // which will call OnExit() before freeing the slot.
        g_StkKernel.ScheduleTaskRemoval(th);

        // For detached threads, free immediately (no joiner expected).
        // For joinable threads, OnExit() will wake the joiner; the joiner
        // calls ObjDestroy(). Do NOT free here.
        if (th->m_join_state == StkThread::JoinState::Detached)
        {
            ObjDestroy(th);
        }
        // else: joiner owns the lifetime

        status = osOK;
    }

    return status;
}

uint32_t osThreadGetCount(void)
{
    uint32_t count = 0U;

    if (osKernelGetState() != osKernelInactive)
    {
        // avoid race with OnTick
        const stk::sync::ScopedCriticalSection cs_;

        count = static_cast<uint32_t>(g_StkKernel.GetSwitchStrategy()->GetSize());
    }

    return count;
}

uint32_t osThreadEnumerate(osThreadId_t *thread_array, uint32_t array_items)
{
    uint32_t result_count = 0U;
    const osKernelState_t kstate = osKernelGetState();

    // kernel must be active and buffer must be valid
    if ((kstate != osKernelInactive) && (thread_array != nullptr) && (array_items != 0U))
    {
        // cast the raw pointer array to the expected ITask* destination type
        stk::ITask **const tasks_destination = reinterpret_cast<stk::ITask **>(
            reinterpret_cast<void *>(thread_array));

        // bind raw destination buffer into a temporary ArrayView object
        const size_t count = g_StkKernel.EnumerateTasks(
            stk::ArrayView<stk::ITask *>(tasks_destination, static_cast<size_t>(array_items)));

        result_count = static_cast<uint32_t>(count);
    }

    return result_count;
}

// ===========================================================================
// ==== Thread Flags Functions ====
// ===========================================================================

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
    uint32_t result;

    if ((thread_id == nullptr) || ((flags & osFlagsError) != 0U))
    {
        result = osFlagsErrorParameter;
    }
    else
    {
        StkThread *th = static_cast<StkThread *>(thread_id);
        result = StkFlagsResultToCmsis(th->m_thread_flags.Set(flags));
    }

    return result;
}

uint32_t osThreadFlagsClear(uint32_t flags)
{
    uint32_t result;

    osThreadId_t const self = osThreadGetId();
    if (self == nullptr)
    {
        result = osFlagsErrorUnknown;
    }
    else
    {
        StkThread *th = static_cast<StkThread *>(self);
        result = StkFlagsResultToCmsis(th->m_thread_flags.Clear(flags));
    }

    return result;
}

uint32_t osThreadFlagsGet(void)
{
    uint32_t result;

    osThreadId_t const self = osThreadGetId();
    if (self == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<StkThread *>(self)->m_thread_flags.Get();
    }

    return result;
}

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
    uint32_t result;

    if (IsIrqContext())
    {
        result = osFlagsErrorISR;
    }
    else
    {
        osThreadId_t const self = osThreadGetId();
        if (self == nullptr)
        {
            result = osFlagsErrorUnknown;
        }
        else
        {
            StkThread *th = static_cast<StkThread *>(self);
            result = StkFlagsResultToCmsis(th->m_thread_flags.Wait(flags,
                CmsisFlagsOptionsToStk(options), CmsisTimeoutToStk(timeout)));
        }
    }

    return result;
}


// ===========================================================================
// ==== Generic Wait Functions ====
// ===========================================================================

osStatus_t osDelay(uint32_t ticks)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (osKernelGetState() == osKernelInactive)
    {
        result = osError;
    }
    else
    {
        stk::Sleep(CmsisTimeoutToStk(ticks));
        result = osOK;
    }

    return result;
}

osStatus_t osDelayUntil(uint32_t ticks)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (osKernelGetState() == osKernelInactive)
    {
        result = osError;
    }
    else
    {
        result = (stk::SleepUntil(static_cast<stk::Ticks>(ticks)) ? osOK : osError);
    }

    return result;
}


// ===========================================================================
// ==== Timer Management Functions ====
// ===========================================================================

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void *argument,
                       const osTimerAttr_t *attr)
{
    osTimerId_t result;

    if (IsIrqContext() || (func == nullptr))
    {
        result = nullptr;
    }
    else  if (osKernelGetState() == osKernelInactive)
    {
        result = nullptr;
    }
    else
    {
        const char *const name   = ((attr != nullptr) ? attr->name    : nullptr);
        void       *const cb_mem = ((attr != nullptr) ? attr->cb_mem  : nullptr);
        const uint32_t    cb_sz  = ((attr != nullptr) ? attr->cb_size : 0U);
        
        StkTimer::EnsureTimerHostCreated();

        StkTimer *const tmr = PlacementNewOrHeap<StkTimer>(cb_mem, cb_sz, func, type, argument, name);
        result = static_cast<osTimerId_t>(tmr);
    }

    return result;
}

const char *osTimerGetName(osTimerId_t timer_id)
{
    const char *result;

    if (timer_id == nullptr)
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkTimer *>(timer_id)->m_name;
    }

    return result;
}

osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if ((timer_id == nullptr) || (g_TimerHost == nullptr))
    {
        result = osErrorParameter;
    }
    else
    {
        StkTimer *const tmr = static_cast<StkTimer *>(timer_id);

        const uint32_t period_ticks = ((tmr->m_type == osTimerPeriodic) ? ticks : 0U);
        tmr->m_period_ticks = period_ticks;

        const bool ok = g_TimerHost->Restart(*tmr, ticks, period_ticks);
        result = (ok ? osOK : osError);
    }

    return result;
}

osStatus_t osTimerStop(osTimerId_t timer_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if ((timer_id == nullptr) || (g_TimerHost == nullptr))
    {
        result = osErrorParameter;
    }
    else
    {
        StkTimer *const tmr = static_cast<StkTimer *>(timer_id);

        if (!tmr->IsActive())
        {
            result = osErrorResource;
        }
        else
        {
            result = (g_TimerHost->Stop(*tmr) ? osOK : osError);
        }
    }

    return result;
}

uint32_t osTimerIsRunning(osTimerId_t timer_id)
{
    uint32_t result;

    if (timer_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = (static_cast<StkTimer *>(timer_id)->IsActive() ? 1U : 0U);
    }

    return result;
}

osStatus_t osTimerDelete(osTimerId_t timer_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if ((timer_id == nullptr) || (g_TimerHost == nullptr))
    {
        result = osErrorParameter;
    }
    else
    {
        result = osOK;
      
        StkTimer *const tmr = static_cast<StkTimer *>(timer_id);

        if (tmr->IsActive())
        {
            if (!g_TimerHost->Stop(*tmr))
            {
                result = osError;
            }
        }

        if (result == osOK)
        {
            ObjDestroy(tmr);
        }
    }

    return result;
}


// ===========================================================================
// ==== Event Flags Management Functions ====
// ===========================================================================

osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr)
{
    osEventFlagsId_t result;

    if (IsIrqContext())
    {
        result = nullptr;
    }
    else
    {
        const char *const name   = ((attr != nullptr) ? attr->name    : nullptr);
        void       *const cb_mem = ((attr != nullptr) ? attr->cb_mem  : nullptr);
        const uint32_t    cb_sz  = ((attr != nullptr) ? attr->cb_size : 0U);

        StkEventFlags *const ef = PlacementNewOrHeap<StkEventFlags>(cb_mem, cb_sz, name);
        result = static_cast<osEventFlagsId_t>(ef);
    }

    return result;
}

const char *osEventFlagsGetName(osEventFlagsId_t ef_id)
{
    const char *result;

    if (ef_id == nullptr)
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkEventFlags *>(ef_id)->m_ef.GetTraceName();
    }

    return result;
}

uint32_t osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags)
{
    uint32_t result;

    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0U))
    {
        result = osFlagsErrorParameter;
    }
    else
    {
        result = StkFlagsResultToCmsis(static_cast<StkEventFlags *>(ef_id)->m_ef.Set(flags));
    }

    return result;
}

uint32_t osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags)
{
    uint32_t result;

    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0U))
    {
        result = osFlagsErrorParameter;
    }
    else
    {
        result = StkFlagsResultToCmsis(static_cast<StkEventFlags *>(ef_id)->m_ef.Clear(flags));
    }

    return result;
}

uint32_t osEventFlagsGet(osEventFlagsId_t ef_id)
{
    uint32_t result;

    if (ef_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<StkEventFlags *>(ef_id)->m_ef.Get();
    }

    return result;
}

uint32_t osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags, uint32_t options,
                          uint32_t timeout)
{
    uint32_t result;

    if ((ef_id == nullptr) || ((flags & osFlagsError) != 0U))
    {
        result = osFlagsErrorParameter;
    }
    else
    {
        result = StkFlagsResultToCmsis(static_cast<StkEventFlags *>(ef_id)->m_ef.Wait(flags,
            CmsisFlagsOptionsToStk(options), CmsisTimeoutToStk(timeout)));
    }

    return result;
}

osStatus_t osEventFlagsDelete(osEventFlagsId_t ef_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (ef_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        ObjDestroy(static_cast<StkEventFlags *>(ef_id));
        result = osOK;
    }

    return result;
}


// ===========================================================================
// ==== Mutex Management Functions ====
// ===========================================================================

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    osMutexId_t result;

    if (IsIrqContext())
    {
        result = nullptr;
    }
    else
    {
        // osMutexPrioInherit: ignored, supported by default.
        // osMutexRecursive: ignored, sync::Mutex is always recursive.
        // osMutexRobust: will assert as unsafe code.
        const char *const name   = ((attr != nullptr) ? attr->name    : nullptr);
        void       *const cb_mem = ((attr != nullptr) ? attr->cb_mem  : nullptr);
        const uint32_t    cb_sz  = ((attr != nullptr) ? attr->cb_size : 0U);
        const bool        robust = ((attr != nullptr) && ((attr->attr_bits & osMutexRobust) != 0U));

        // disallow osMutexRobust
        STK_ASSERT(!robust);
        if (robust)
        {
            result = nullptr;
        }
        else
        {
            StkMutex *const m = PlacementNewOrHeap<StkMutex>(cb_mem, cb_sz, name);
            result = static_cast<osMutexId_t>(m);
        }
    }

    return result;
}

const char *osMutexGetName(osMutexId_t mutex_id)
{
    const char *result;

    if (mutex_id == nullptr)
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkMutex *>(mutex_id)->m_mutex.GetTraceName();
    }

    return result;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (mutex_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        StkMutex *m = static_cast<StkMutex *>(mutex_id);
        const stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

        const bool acquired = m->m_mutex.TimedLock(stk_timeout);
        if (!acquired)
        {
            result = ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);
        }
        else
        {
            result = osOK;
        }
    }

    return result;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (mutex_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        static_cast<StkMutex *>(mutex_id)->m_mutex.Unlock();
        result = osOK;
    }

    return result;
}

osThreadId_t osMutexGetOwner(osMutexId_t mutex_id)
{
    osThreadId_t result;

    if (mutex_id == nullptr)
    {
        result = nullptr;
    }
    else
    {        
        result = StkThread::ConvertTIdToThreadId(
            static_cast<StkMutex *>(mutex_id)->m_mutex.GetOwner());
    }

    return result;
}

osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (mutex_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        ObjDestroy(static_cast<StkMutex *>(mutex_id));
        result = osOK;
    }

    return result;
}


// ===========================================================================
// ==== Semaphore Management Functions ====
// ===========================================================================

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
                               const osSemaphoreAttr_t *attr)
{
    osSemaphoreId_t result;

    if (IsIrqContext() || (max_count == 0U) || (initial_count > max_count))
    {
        result = nullptr;
    }
    else
    {
        // STK Semaphore uses uint16_t counters, clamp to stk::sync::Semaphore::COUNT_MAX.
        const uint16_t mc = static_cast<uint16_t>(stk::Min(max_count,     static_cast<uint32_t>(stk::sync::Semaphore::COUNT_MAX)));
        const uint16_t ic = static_cast<uint16_t>(stk::Min(initial_count, static_cast<uint32_t>(stk::sync::Semaphore::COUNT_MAX)));

        const char *const name   = ((attr != nullptr) ? attr->name    : nullptr);
        void       *const cb_mem = ((attr != nullptr) ? attr->cb_mem  : nullptr);
        const uint32_t    cb_sz  = ((attr != nullptr) ? attr->cb_size : 0U);

        StkSemaphore *const sem = PlacementNewOrHeap<StkSemaphore>(cb_mem, cb_sz, ic, mc, name);
        result = static_cast<osSemaphoreId_t>(sem);
    }

    return result;
}

const char *osSemaphoreGetName(osSemaphoreId_t semaphore_id)
{
    const char *result;

    if (semaphore_id == nullptr)
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkSemaphore *>(semaphore_id)->m_semaphore.GetTraceName();
    }

    return result;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
    osStatus_t result;

    if (semaphore_id == nullptr)
    {
        result = osErrorParameter;
    }
    else if (IsIrqContext() && (timeout != 0U))
    {
        result = osErrorISR;
    }
    else
    {
        StkSemaphore *sem = static_cast<StkSemaphore *>(semaphore_id);
        const stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

        const bool acquired = sem->m_semaphore.Wait(stk_timeout);
        if (!acquired)
        {
            result = ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);
        }
        else
        {
            result = osOK;
        }
    }

    return result;
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
    osStatus_t result;

    if (semaphore_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        static_cast<StkSemaphore *>(semaphore_id)->m_semaphore.Signal();
        result = osOK;
    }

    return result;
}

uint32_t osSemaphoreGetCount(osSemaphoreId_t semaphore_id)
{
    uint32_t result;

    if (semaphore_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(
            static_cast<StkSemaphore *>(semaphore_id)->m_semaphore.GetCount());
    }

    return result;
}

osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (semaphore_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        ObjDestroy(static_cast<StkSemaphore *>(semaphore_id));
        result = osOK;
    }

    return result;
}


// ===========================================================================
// ==== Memory Pool Management Functions ====
// ===========================================================================

osMemoryPoolId_t osMemoryPoolNew(uint32_t block_count, uint32_t block_size,
                                 const osMemoryPoolAttr_t *attr)
{
    osMemoryPoolId_t result;

    // ISR context: forbidden per CMSIS spec.
    // Zero capacity or zero block size are meaningless.
    if (IsIrqContext() || (block_count == 0U) || (block_size == 0U))
    {
        result = nullptr;
    }
    else
    {
        const char *const name   = ((attr != nullptr) ? attr->name    : nullptr);
        void       *const cb_mem = ((attr != nullptr) ? attr->cb_mem  : nullptr);
        const uint32_t    cb_sz  = ((attr != nullptr) ? attr->cb_size : 0U);
        void       *const mp_mem = ((attr != nullptr) ? attr->mp_mem  : nullptr);
        const uint32_t    mp_sz  = ((attr != nullptr) ? attr->mp_size : 0U);

        // Compute the aligned block size and required storage byte count.
        const uint32_t aligned_blk      = stk::memory::BlockMemoryPool::AlignBlockSize(block_size);
        const uint32_t storage_required = (block_count * aligned_blk);

        StkMemPool *pool;

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
                pool = nullptr;
            }
        }

        result = static_cast<osMemoryPoolId_t>(pool);
    }

    return result;
}

const char *osMemoryPoolGetName(osMemoryPoolId_t mp_id)
{
    const char *result;

    if (mp_id == nullptr)
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkMemPool *>(mp_id)->m_mpool.GetTraceName();
    }

    return result;
}

void *osMemoryPoolAlloc(osMemoryPoolId_t mp_id, uint32_t timeout)
{
    void *result;

    if (mp_id == nullptr)
    {
        result = nullptr;
    }
    // ISR context is only valid with timeout == 0 (NO_WAIT / TryAlloc).
    else if (IsIrqContext() && (timeout != 0U))
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkMemPool *>(mp_id)->m_mpool.TimedAlloc(CmsisTimeoutToStk(timeout));
    }

    return result;
}

osStatus_t osMemoryPoolFree(osMemoryPoolId_t mp_id, void *block)
{
    osStatus_t result;

    if ((mp_id == nullptr) || (block == nullptr))
    {
        result = osErrorParameter;
    }
    else if (!static_cast<StkMemPool *>(mp_id)->m_mpool.Free(block))
    {
        result = osErrorParameter; // ptr not from this pool
    }
    else
    {
        result = osOK;
    }

    return result;
}

uint32_t osMemoryPoolGetCapacity(osMemoryPoolId_t mp_id)
{
    uint32_t result;

    if (mp_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<StkMemPool *>(mp_id)->m_mpool.GetCapacity();
    }

    return result;
}

uint32_t osMemoryPoolGetBlockSize(osMemoryPoolId_t mp_id)
{
    uint32_t result;

    if (mp_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(static_cast<StkMemPool *>(mp_id)->m_mpool.GetBlockSize());
    }

    return result;
}

uint32_t osMemoryPoolGetCount(osMemoryPoolId_t mp_id)
{
    uint32_t result;

    if (mp_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<StkMemPool *>(mp_id)->m_mpool.GetUsedCount();
    }

    return result;
}

uint32_t osMemoryPoolGetSpace(osMemoryPoolId_t mp_id)
{
    uint32_t result;

    if (mp_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<StkMemPool *>(mp_id)->m_mpool.GetFreeCount();
    }

    return result;
}

osStatus_t osMemoryPoolDelete(osMemoryPoolId_t mp_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (mp_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        ObjDestroy(static_cast<StkMemPool *>(mp_id));
        result = osOK;
    }

    return result;
}


// ===========================================================================
// ==== Message Queue Management Functions ====
// ===========================================================================

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
                                     const osMessageQueueAttr_t *attr)
{
    osMessageQueueId_t result;

    if (IsIrqContext() || (msg_count == 0U) || (msg_size == 0U) || 
        (msg_count > stk::sync::MessageQueue::CAPACITY_MAX))
    {
        result = nullptr;
    }
    else
    {
        const char      *const name    = ((attr != nullptr) ? attr->name    : nullptr);
        void            *const cb_mem  = ((attr != nullptr) ? attr->cb_mem  : nullptr);
        const uint32_t   cb_sz         = ((attr != nullptr) ? attr->cb_size : 0U);
        void            *const ext_buf = ((attr != nullptr) ? attr->mq_mem  : nullptr);
        const uint32_t   ext_buf_size  = ((attr != nullptr) ? attr->mq_size : 0U);

        const uint32_t buf_required = (msg_count * msg_size);

        StkMessageQueue *mq;

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
            
            // Validate
            if (mq != nullptr)
            {
                if (mq->m_mq.GetBuffer() == nullptr)
                {
                    ObjDestroy(mq);
                    mq = nullptr;
                }
            }
        }

        result = static_cast<osMessageQueueId_t>(mq);
    }

    return result;
}

const char *osMessageQueueGetName(osMessageQueueId_t mq_id)
{
    const char *result;

    if (mq_id == nullptr)
    {
        result = nullptr;
    }
    else
    {
        result = static_cast<StkMessageQueue *>(mq_id)->m_mq.GetTraceName();
    }

    return result;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
                             uint8_t /*msg_prio*/, uint32_t timeout)
{
    osStatus_t result;

    if ((mq_id == nullptr) || (msg_ptr == nullptr))
    {
        result = osErrorParameter;
    }
    else if (IsIrqContext() && (timeout != 0U))
    {
        result = osErrorISR;
    }
    else
    {
        const stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

        if (!static_cast<StkMessageQueue *>(mq_id)->m_mq.Put(msg_ptr, stk_timeout))
        {
            result = ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);
        }
        else
        {
            result = osOK;
        }
    }

    return result;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
                             uint8_t *msg_prio, uint32_t timeout)
{
    osStatus_t result;

    if ((mq_id == nullptr) || (msg_ptr == nullptr))
    {
        result = osErrorParameter;
    }
    else if (IsIrqContext() && (timeout != 0U))
    {
        result = osErrorISR;
    }
    else
    {
        const stk::Timeout stk_timeout = CmsisTimeoutToStk(timeout);

        if (!static_cast<StkMessageQueue *>(mq_id)->m_mq.Get(msg_ptr, stk_timeout))
        {
            result = ((stk_timeout == stk::NO_WAIT) ? osErrorResource : osErrorTimeout);
        }
        else
        {
            if (msg_prio != nullptr)
            {
                *msg_prio = 0U; // STK queues have no priority lanes.
            }
            result = osOK;
        }
    }

    return result;
}

uint32_t osMessageQueueGetCapacity(osMessageQueueId_t mq_id)
{
    uint32_t result;

    if (mq_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetCapacity());
    }

    return result;
}

uint32_t osMessageQueueGetMsgSize(osMessageQueueId_t mq_id)
{
    uint32_t result;

    if (mq_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetMsgSize());
    }

    return result;
}

uint32_t osMessageQueueGetCount(osMessageQueueId_t mq_id)
{
    uint32_t result;

    if (mq_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetCount());
    }

    return result;
}

uint32_t osMessageQueueGetSpace(osMessageQueueId_t mq_id)
{
    uint32_t result;

    if (mq_id == nullptr)
    {
        result = 0U;
    }
    else
    {
        result = static_cast<uint32_t>(static_cast<StkMessageQueue *>(mq_id)->m_mq.GetSpace());
    }

    return result;
}

osStatus_t osMessageQueueReset(osMessageQueueId_t mq_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (mq_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        static_cast<StkMessageQueue *>(mq_id)->m_mq.Reset();
        result = osOK;
    }

    return result;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
    osStatus_t result;

    if (IsIrqContext())
    {
        result = osErrorISR;
    }
    else if (mq_id == nullptr)
    {
        result = osErrorParameter;
    }
    else
    {
        ObjDestroy(static_cast<StkMessageQueue *>(mq_id));
        result = osOK;
    }

    return result;
}
