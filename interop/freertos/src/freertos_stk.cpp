/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stdio.h>  // snprintf (vTaskList)

#include "stk.h"
#include "sync/stk_sync.h"
#include "time/stk_time.h"
#include "memory/stk_memory.h"

// See design notes, API coverage and other details in FreeRTOS.h.

#include "FreeRTOS.h"

// ---------------------------------------------------------------------------
// Wrapper version info
// ---------------------------------------------------------------------------

#define FREERTOS_STK_WRAPPER_VERSION   "FreeRTOS-STK Wrapper v1.0"

// ---------------------------------------------------------------------------
// Kernel configuration
// ---------------------------------------------------------------------------

// Maximum concurrent tasks. Mirrors FREERTOS_STK_MAX_TASKS from the header.
#ifndef FREERTOS_STK_MAX_TASKS
#   define FREERTOS_STK_MAX_TASKS 16U
#endif

#ifndef FREERTOS_STK_DEFAULT_STACK_WORDS
#   define FREERTOS_STK_DEFAULT_STACK_WORDS 256U
#endif

// Minimum usable stack in STK Words.
#define FREERTOS_STK_MIN_STACK_WORDS  STK_STACK_SIZE_MIN

// Returns a size of memory in stk::Word elements required for object allocation.
template <typename T> static constexpr size_t StkGetWordCountForType()
{
    return ((sizeof(T) + sizeof(stk::Word) - 1) / sizeof(stk::Word));
}

// ---------------------------------------------------------------------------
// Private memory allocators (we define malloc, free here to overcome absence of declaration in
// case of -ffreestanding compiler flag).
// Similar to FreeRTOS's heap_3.c.
// ---------------------------------------------------------------------------

extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);

static stk::memory::MemoryAllocator::Stats s_MemStats(configTOTAL_HEAP_SIZE);

void *stk::memory::MemoryAllocator::Allocate(size_t size)
{
    if (size == 0)
        return nullptr;

    // align to platform word
    const size_t alignment = alignof(size_t);

    // add header and round up to the nearest multiple of 'alignment'
    size = (size + sizeof(size_t) + (alignment - 1)) & ~(alignment - 1);

    stk::hw::CriticalSection::ScopedLock cs_;

    if (s_MemStats.GetAvailable() < size)
        return nullptr;

    size_t *region = static_cast<size_t *>(malloc(size));
    if (region != nullptr)
    {
        s_MemStats.RecordAllocate(size);

        // save size
        region[0] = size;

        // set to usable memory region skipping header
        region = region + 1;
    }

    return region;
}

void stk::memory::MemoryAllocator::Free(void *ptr)
{
    if (ptr == nullptr)
        return;

    stk::hw::CriticalSection::ScopedLock cs_;

    // get region
    size_t *region = static_cast<size_t *>(ptr) - 1;
    size_t size = region[0];

    free(region);

    s_MemStats.RecordFree(size);
}

stk::memory::MemoryAllocator::Stats stk::memory::MemoryAllocator::GetStats()
{
    return s_MemStats;
}

// ===========================================================================
// Internal helpers
// ===========================================================================

// Heap-allocate an object (operator new with nothrow).
template <typename T, typename... Args>
static T *ObjAlloc(Args &&...args)
{
    T *obj = nullptr;

    void *ptr = pvPortMalloc(sizeof(T));
    if (ptr != nullptr)
        obj = new (ptr) T(static_cast<Args &&>(args)...);

    return obj;
}

// Delete an object previously created by ObjAlloc.
template <typename T>
static void ObjFree(T *obj)
{
    if (obj != nullptr)
    {
        obj->~T();

        if (obj->m_cb_owned)
            vPortFree(obj);
    }
}

// Heap-allocate a raw byte array of 'count' elements of type T via pvPortMalloc.
// Equivalent to: new (std::nothrow) T[count]
template <typename T>
static T *ObjAllocArray(size_t count)
{
    if (count == 0U)
        return nullptr;

    void *ptr = pvPortMalloc(sizeof(T) * count);
    return static_cast<T *>(ptr);
}

// Free a raw array previously allocated by ObjAllocArray (no destructor calls).
// Equivalent to: delete[] ptr
static inline void ObjFreeArray(void *ptr)
{
    vPortFree(ptr);
}

// Destroy and free a single heap-allocated object that was created by ObjAlloc
// but is not tracked by m_cb_owned (e.g. self-deleting timers, error-path cleanup).
// Equivalent to: delete obj
template <typename T>
static void ObjFreeRaw(T *obj)
{
    if (obj != nullptr)
    {
        obj->~T();
        vPortFree(obj);
    }
}

// ---------------------------------------------------------------------------
// Heap API — pvPortMalloc / vPortFree
//
// Both functions delegate directly to stk::memory::MemoryAllocator::Allocate
// and ::Free, which are the single allocation seam for the entire wrapper.
// Redefining those two functions (e.g. to point at a static pool allocator)
// automatically redirects both internal STK allocations and any application
// code that calls pvPortMalloc / vPortFree.
// ---------------------------------------------------------------------------

__stk_weak void *pvPortMalloc(size_t xWantedSize)
{
    return stk::memory::MemoryAllocator::Allocate(xWantedSize);
}

__stk_weak void vPortFree(void *pv)
{
    stk::memory::MemoryAllocator::Free(pv);
}

// ---------------------------------------------------------------------------
// Heap query API — xPortGetFreeHeapSize / xPortGetMinimumEverFreeHeapSize /
//                  vPortGetHeapStats / MemoryAllocator::GetStats
//
// All three functions read directly from s_MemStats, which is the single
// authoritative accounting structure maintained by Allocate() and Free().
//
// xPortGetFreeHeapSize
//   Returns GetAvailable() — the number of bytes not yet handed out.
//   This is a point-in-time snapshot and may lag by one allocation if called
//   concurrently, matching FreeRTOS heap_4/heap_5 behaviour.
//
// xPortGetMinimumEverFreeHeapSize
//   Returns the minimum value GetAvailable() has ever reached since
//   system start (i.e. the high-water mark of heap pressure).  The watermark
//   is updated inside Allocate() immediately after the 'allocated' counter is
//   incremented, so it is always <= the current free value.
//
// vPortGetHeapStats
//   Fills a HeapStats_t snapshot consistent with the FreeRTOS heap_4/heap_5
//   contract.  Fields that require a traversal of free-block lists (number of
//   free blocks, smallest/largest free block) are not available without a
//   block-list allocator; they are reported as 0 and 1 respectively to signal
//   "at least one contiguous region exists" without asserting false precision.
//
// MemoryAllocator::GetStats
//   Returns the raw Stats struct for STK-native callers.
// ---------------------------------------------------------------------------

size_t xPortGetFreeHeapSize(void)
{
    return s_MemStats.GetAvailable();
}

size_t xPortGetMinimumEverFreeHeapSize(void)
{
    return s_MemStats.min_ever_free;
}

void vPortGetHeapStats(HeapStats_t *pxHeapStats)
{
    if (pxHeapStats == nullptr)
        return;

    const stk::memory::MemoryAllocator::Stats snap = s_MemStats;
    const size_t free_now = snap.GetAvailable();

    (*pxHeapStats) = {};

    // Fields derived directly from s_MemStats accounting.
    pxHeapStats->xAvailableHeapSpaceInBytes      = free_now;
    pxHeapStats->xMinimumEverFreeBytesRemaining  = snap.min_ever_free;
    pxHeapStats->xNumberOfSuccessfulAllocations  = snap.allocate_count;
    pxHeapStats->xNumberOfSuccessfulFrees        = snap.free_count;

    // Fields that require free-block-list traversal are unavailable without a
    // block-list allocator (we delegate to malloc).  Report conservative values:
    //   - largest free block  : current free bytes (treat heap as one region)
    //   - smallest free block : 0  (unknown subdivision)
    //   - number of free blocks: 1  (at least one region exists while free > 0)
    pxHeapStats->xSizeOfLargestFreeBlockInBytes  = free_now;
    pxHeapStats->xSizeOfSmallestFreeBlockInBytes = (free_now > 0U) ? 1U : 0U;
    pxHeapStats->xNumberOfFreeBlocks             = (free_now > 0U) ? 1U : 0U;
}

// ---------------------------------------------------------------------------
// Priority mapping:
//   FreeRTOS range       : 0 (lowest/idle) .. configMAX_PRIORITIES-1 (highest)
//   STK FP32 level range : 0               .. 31
//
//   SwitchStrategyFixedPriority interprets GetWeight() as the raw priority
//   level directly (not a proportional weight).  Higher numeric value means
//   higher priority and strictly preempts all lower levels.  The mapping is
//   therefore a direct clamp — no shift needed:
//
//     stk_priority = clamp(freertos_priority, 0, 31)
//
//   configMAX_PRIORITIES must be <= 32 (compile-time assertion below).
//   If it is less than 32, only levels 0..configMAX_PRIORITIES-1 are used
//   and the upper FP32 slots remain empty, which is perfectly fine.
// ---------------------------------------------------------------------------

// Enforce that configMAX_PRIORITIES fits within the 32-level FP32 strategy.
static_assert(configMAX_PRIORITIES <= 32U,
    "configMAX_PRIORITIES exceeds SwitchStrategyFP32's 32 priority levels. "
    "Reduce configMAX_PRIORITIES or instantiate SwitchStrategyFixedPriority "
    "with a larger MAX_PRIORITIES template parameter and update FrtosKernel.");

static inline int32_t FrtosPrioToStkWeight(UBaseType_t p)
{
    // Clamp to [0 .. configMAX_PRIORITIES-1] then pass through directly:
    // STK FP32 level == FreeRTOS priority (both 0 = lowest).
    if (p >= (UBaseType_t)configMAX_PRIORITIES)
        p = (UBaseType_t)configMAX_PRIORITIES - 1U;

    return static_cast<int32_t>(p);
}

static inline UBaseType_t StkWeightToFrtosPrio(int32_t w)
{
    if (w < 0)
        return 0U;

    UBaseType_t p = static_cast<UBaseType_t>(w);

    if (p >= (UBaseType_t)configMAX_PRIORITIES)
        p = (UBaseType_t)configMAX_PRIORITIES - 1U;

    return p;
}

// ---------------------------------------------------------------------------
// Timeout conversion:
//   portMAX_DELAY (0xFFFFFFFF) -> stk::WAIT_INFINITE
//   0                          -> stk::NO_WAIT
//   N                          -> N  (ticks pass through directly)
// ---------------------------------------------------------------------------
static inline stk::Timeout FrtosTimeoutToStk(TickType_t t)
{
    if (t == portMAX_DELAY)
        return stk::WAIT_INFINITE;

    if (t == 0U)
        return stk::NO_WAIT;

    return static_cast<stk::Timeout>(t);
}

// ---------------------------------------------------------------------------
// ISR context check
// ---------------------------------------------------------------------------
static inline bool IsIrqContext()
{
    return stk::hw::IsInsideISR();
}

// ---------------------------------------------------------------------------
// Global kernel type alias.
// KERNEL_DYNAMIC  : tasks can be created/deleted at runtime.
// KERNEL_SYNC     : enables all synchronisation primitives.
// SwitchStrategyFP32: Fixed-priority preemptive, 32 levels (0=lowest, 31=highest),
//   with round-robin within each level.  This exactly mirrors FreeRTOS scheduling
//   semantics: the highest-priority ready task always runs immediately.
// ---------------------------------------------------------------------------
using FrtosKernel = stk::Kernel<stk::KERNEL_DYNAMIC | stk::KERNEL_SYNC
#if STK_TICKLESS_IDLE
    | stk::KERNEL_TICKLESS
#endif
    , FREERTOS_STK_MAX_TASKS,
    stk::SwitchStrategyFP32,
    stk::PlatformDefault>;

static FrtosKernel g_StkKernel;

// ---------------------------------------------------------------------------
// vPortEnterCritical / vPortExitCritical (back taskENTER/EXIT_CRITICAL macros)
// ---------------------------------------------------------------------------
void vPortEnterCritical(void)
{
    stk::hw::CriticalSection::Enter();
}

void vPortExitCritical(void)
{
    stk::hw::CriticalSection::Exit();
}

void taskYIELD_impl(void)
{
    stk::Yield();
}

// ===========================================================================
// Task control block
//
// FrtosTask wraps a single FreeRTOS task.  It implements stk::ITask so the
// kernel can schedule it, and IStackMemory so its own stack can be registered.
//
// Task notifications (index 0) are modelled as a counting semaphore whose
// count represents the notification value.  xTaskNotify / xTaskNotifyWait
// provide the richer set-bits / overwrite semantics on top of a raw uint32_t
// notification word protected by a critical section.
// ===========================================================================

struct FrtosTask : public stk::ITask
{
    enum class State : uint8_t
    {
        Ready,      // in the ready/running queue
        Suspended,  // explicitly suspended via vTaskSuspend()
        Deleted,    // marked for removal, kernel slot being freed
    };

    explicit FrtosTask()
        : m_func(nullptr), m_argument(nullptr), m_name(nullptr),
          m_weight(FrtosPrioToStkWeight(tskIDLE_PRIORITY)),
          m_stack(nullptr), m_stack_size(0U),
          m_stack_owned(false), m_cb_owned(true),
          m_state(State::Ready),
          m_task_number(++s_task_counter)
    {
        for (size_t i = 0U; i < configNUM_THREAD_LOCAL_STORAGE_POINTERS; ++i)
            m_tls[i] = nullptr;
    }

    virtual ~FrtosTask()
    {
        if (m_stack_owned && (m_stack != nullptr))
        {
            ObjFreeArray(m_stack);
            m_stack = nullptr;
        }
    }

    // ---- stk::ITask --------------------------------------------------------
    void Run() override
    {
        m_func(m_argument);
        // KERNEL_DYNAMIC: returning removes the task automatically.
    }

    void OnExit() override
    {
        m_state = State::Deleted;
    }

    stk::Word  *GetStack()           const override { return m_stack; }
    size_t      GetStackSize()       const override { return m_stack_size; }
    size_t      GetStackSizeBytes()  const override { return m_stack_size * sizeof(stk::Word); }
    stk::EAccessMode GetAccessMode() const override { return stk::ACCESS_PRIVILEGED; }
    int32_t     GetWeight()          const override { return m_weight; }
    const char *GetTraceName()       const override { return m_name; }

    void OnDeadlineMissed(uint32_t) override {}

    // ---- Stack high-water mark inspection ----------------------------------
    // Returns the number of untouched Words at the stack base (filled with
    // STK_STACK_MEMORY_FILLER during init).
    size_t GetStackHighWaterMark() const { return GetStackSpace(); }

    // ---- Members -----------------------------------------------------------
    TaskFunction_t          m_func;
    void                   *m_argument;
    const char             *m_name;
    volatile int32_t        m_weight;       // STK SWRR weight (priority+1)
    stk::Word              *m_stack;
    size_t                  m_stack_size;   // Words
    bool                    m_stack_owned;
    bool                    m_cb_owned;     // true -> heap-alloc, delete on removal
    volatile State          m_state;
    uint32_t                m_task_number;  // monotonic serial, assigned at construction

    // Monotonic counter incremented once per FrtosTask construction.
    // Stored as a file-scope static so all tasks share a single sequence.
    static uint32_t s_task_counter;

    // Thread-local storage slots (configNUM_THREAD_LOCAL_STORAGE_POINTERS entries).
    // Initialised to nullptr at construction; accessed via
    // vTaskSetThreadLocalStoragePointer / pvTaskGetThreadLocalStoragePointer.
    void *m_tls[configNUM_THREAD_LOCAL_STORAGE_POINTERS];

#if configUSE_TASK_NOTIFICATIONS
    // ---- Per-slot task notification state ----------------------------------
    // FreeRTOS supports configTASK_NOTIFICATION_ARRAY_ENTRIES independent
    // notification slots per task.  Each slot is fully independent: it has its
    // own 32-bit value word, a pending flag (for eSetValueWithoutOverwrite), and
    // a binary stk::sync::Semaphore that serves as the blocking primitive.
    //
    // The non-indexed API (xTaskNotifyGive / xTaskNotify / etc.) is implemented
    // as thin wrappers that always address slot 0.
    struct NotifySlot
    {
        volatile uint32_t    value;   //!< notification value word
        volatile bool        pending; //!< true if a value was set but not yet consumed
        stk::sync::Semaphore sem;     //!< binary semaphore: blocks the waiter, signaled by notifier

        explicit NotifySlot() : value(0U), pending(false), sem(0U, 1U) {}

    private:
        STK_NONCOPYABLE_CLASS(NotifySlot);
    };

    NotifySlot m_notify[configTASK_NOTIFICATION_ARRAY_ENTRIES];
#endif // configUSE_TASK_NOTIFICATIONS
};

// Monotonic task serial counter; incremented once per FrtosTask construction.
uint32_t FrtosTask::s_task_counter = 0U;

// ===========================================================================
// Queue control block
//
// Backed by stk::sync::MessageQueue.
// ===========================================================================

// Forward declaration: FrtosQueueSet is defined after FrtosQueue and FrtosSemaphore
// so that both control blocks can hold a non-owning back-pointer to their registered set.
struct FrtosQueueSet;

struct FrtosQueue
{
    // External storage constructor (caller supplies the data buffer).
    explicit FrtosQueue(uint32_t cap, uint32_t msg_size, const char *name,
                        uint8_t *ext_buf)
        : m_mq(ext_buf, static_cast<size_t>(cap), static_cast<size_t>(msg_size)),
          m_buf_owned(false), m_cb_owned(true)
    #if configUSE_QUEUE_SETS
          , m_set(nullptr)
    #endif
    {
        m_mq.SetTraceName(name);
    }

    // Heap-allocated data buffer constructor.
    explicit FrtosQueue(uint32_t cap, uint32_t msg_size, const char *name)
        : m_mq(AllocBuffer(cap, msg_size),
                static_cast<size_t>(cap),
                static_cast<size_t>(msg_size)),
          m_buf_owned(m_mq.IsStorageValid()), m_cb_owned(true)
    #if configUSE_QUEUE_SETS
          , m_set(nullptr)
    #endif
    {
        m_mq.SetTraceName(name);
    }

    ~FrtosQueue()
    {
        if (m_buf_owned)
            ObjFreeArray(m_mq.GetBuffer());
    }

    static uint8_t *AllocBuffer(uint32_t cap, uint32_t msg_size)
    {
        return ObjAllocArray<uint8_t>(static_cast<size_t>(cap) * msg_size);
    }

    stk::sync::MessageQueue m_mq;
    bool                    m_buf_owned;
    bool                    m_cb_owned;
#if configUSE_QUEUE_SETS
    FrtosQueueSet          *m_set; //!< non-owning ptr to the queue set this member belongs to (nullptr if none)
#endif
};

// ===========================================================================
// Semaphore / Mutex control block
//
// A single struct covers:
//   - Binary semaphore     (max_count=1, initial_count=0)  SemKind::Counting (0x80)
//   - Counting semaphore   (max_count=N, initial_count=K)  SemKind::Counting (0x80)
//   - Mutex / Recursive    (uses stk::sync::Mutex)         SemKind::Mutex    (0x81)
//
// The first byte of every FrtosSemaphore is its SemKind discriminant.
// Call GetSemKindFromHandle() to safely classify an opaque handle.
// ===========================================================================

// SemKind — type discriminant stored as the first byte of every FrtosSemaphore.
//
// Values are chosen so they can never collide with byte[0] of a FrtosQueue or
// any other FrtosXxx object:
//
//   FrtosQueue byte[0] is byte[0] of stk::sync::MessageQueue, which inherits
//   from ITraceable.  ITraceable starts with a vtable pointer (or equivalent
//   first data member); on all supported 32/64-bit targets that pointer value
//   is always in high memory (>= 4), so byte[0] is always >= 4 under normal
//   linking.  Choosing 0x80 and 0x81 for the SemKind values places them well
//   above the 0x00–0x03 range previously used and well below any realistic
//   vtable-pointer low byte on little-endian targets, making the discriminant
//   robust even if future struct layout changes shift ITraceable internals.
//
// GetSemKindFromHandle() is the single authoritative function that reads and
// validates the discriminant; all type-switching code must call it instead of
// reading byte[0] directly.
enum class SemKind : uint8_t
{
    None     = 0x00U, //!< Sentinel: not a FrtosSemaphore (e.g. plain FrtosQueue).
    Counting = 0x80U, //!< Binary or counting semaphore (backed by stk::sync::Semaphore).
    Mutex    = 0x81U, //!< Mutex or recursive mutex    (backed by stk::sync::Mutex).
};

// Return the SemKind discriminant for the object at \a obj, or SemKind::None
// if the byte does not match any known SemKind value (i.e. the handle points
// to a FrtosQueue or another non-semaphore object).
//
// The switch enumerates only the valid SemKind enumerators; any other byte
// value falls through to the default and returns SemKind::None, making the
// check both exhaustive and forward-safe.
static SemKind GetSemKindFromHandle(const void *obj)
{
    const uint8_t first_byte = *static_cast<const uint8_t *>(obj);

    switch (first_byte)
    {
        case static_cast<uint8_t>(SemKind::Counting): return SemKind::Counting;
        case static_cast<uint8_t>(SemKind::Mutex):    return SemKind::Mutex;
        default:                                       return SemKind::None;
    }
}

struct FrtosSemaphore
{
    explicit FrtosSemaphore(SemKind kind, uint16_t initial, uint16_t max_count)
        : m_kind(kind), m_cb_owned(true),
          m_sem(nullptr)
#if configUSE_MUTEXES
          , m_mtx(nullptr)
#endif
        #if configUSE_QUEUE_SETS
          , m_set(nullptr)
        #endif
    {
        if (kind == SemKind::Counting)
            m_sem = ObjAlloc<stk::sync::Semaphore>(initial, max_count);
#if configUSE_MUTEXES
        else
            m_mtx = ObjAlloc<stk::sync::Mutex>();
#endif
    }

    ~FrtosSemaphore()
    {
        ObjFreeRaw(m_sem);
#if configUSE_MUTEXES
        ObjFreeRaw(m_mtx);
#endif
    }

    SemKind               m_kind;
    bool                  m_cb_owned;
    stk::sync::Semaphore *m_sem; // non-null for Counting kind
#if configUSE_MUTEXES
    stk::sync::Mutex     *m_mtx; // non-null for Mutex kind
#endif
#if configUSE_QUEUE_SETS
    FrtosQueueSet        *m_set; //!< non-owning ptr to the queue set this member belongs to (nullptr if none)
#endif
};

// ===========================================================================
// Queue Set control block
//
// A queue set is a supervising FIFO whose payload elements are void* handles
// (sizeof(void*) bytes each).  When a member queue or semaphore transitions
// from empty to non-empty, the member's own handle is written into this FIFO
// via QueueSetNotify().  xQueueSelectFromSet() then does a blocking Get()
// of one handle from the FIFO, returning it to the caller.
//
// The internal MessageQueue stores pointer-sized tokens and is given a
// capacity equal to uxEventQueueLength supplied by the application (which
// per the FreeRTOS API contract must be >= the sum of all member capacities).
//
// Thread safety:
//   QueueSetNotify() is called from both task and ISR context immediately
//   after a successful Put/Signal on a member, before the critical section
//   opened by that Put/Signal is released.  TryPut() (NO_WAIT) on the set
//   queue is ISR-safe per the STK MessageQueue contract, so no additional
//   locking is required here.
//
// Ownership model:
//   FrtosQueueSet is always heap-allocated (m_cb_owned = true).
//   Member pointers stored in m_mq are non-owning; the application owns all
//   member objects independently and must call xQueueRemoveFromSet() before
//   deleting any member or the set.
// ===========================================================================

struct FrtosQueueSet
{
    // The payload of every slot in m_token_mq is exactly one void* — the
    // handle of the member that fired.  We use a raw byte array as the
    // MessageQueue backing store to avoid a separate heap allocation.
    explicit FrtosQueueSet(UBaseType_t uxEventQueueLength)
        : m_buf(nullptr), m_cb_owned(true),
          m_token_mq(nullptr)
    {
        // Allocate the flat byte ring-buffer: N slots × sizeof(void*) bytes.
        const size_t buf_bytes =
            static_cast<size_t>(uxEventQueueLength) * sizeof(void *);

        m_buf = ObjAllocArray<uint8_t>(buf_bytes);
        if (m_buf == nullptr)
            return;

        m_token_mq = ObjAlloc<stk::sync::MessageQueue>(
            m_buf,
            static_cast<size_t>(uxEventQueueLength),
            sizeof(void *));
    }

    ~FrtosQueueSet()
    {
        ObjFreeRaw(m_token_mq);
        ObjFreeArray(m_buf);
    }

    bool IsValid() const { return (m_token_mq != nullptr); }

    uint8_t                  *m_buf;       //!< raw backing store for m_token_mq
    bool                      m_cb_owned;  //!< true = heap-allocated, delete in ObjFree
    stk::sync::MessageQueue  *m_token_mq;  //!< FIFO of fired-member handles (void*)
};

// ---------------------------------------------------------------------------
// QueueSetNotify — called after every successful send/signal on a member.
//
// Posts the member's handle (a void*) into the set's token queue using a
// non-blocking TryPut().  If the set queue is full the notification is
// silently dropped, matching the FreeRTOS behaviour (which documents that
// xEventQueueLength must be large enough to hold every possible concurrent
// notification from all members without overflow).
//
// This function is ISR-safe: TryPut() uses NO_WAIT and a ScopedCriticalSection
// internally, both of which are safe from interrupt context.
//
// Parameters:
//   member_handle — the void* handle of the queue or semaphore that fired.
//   set          — the FrtosQueueSet that member belongs to.
// ---------------------------------------------------------------------------
template <typename THost>
static inline void QueueSetNotify(void *member_handle, THost *host)
{
#if configUSE_QUEUE_SETS
    // m_set is non-null only when the member was registered with xQueueAddToSet.
    // Post the member handle as a pointer-sized token. TryPut is ISR-safe.
    if (host->m_set != nullptr)
        host->m_set->m_token_mq->TryPut(&member_handle);
#else
    STK_UNUSED(member_handle);
    STK_UNUSED(host);
#endif
}

// ===========================================================================
// Software timer control block  [configUSE_TIMERS]
//
// Backed by stk::time::TimerHost.
// A single global TimerHost is created lazily on the first xTimerCreate().
// ===========================================================================

#if configUSE_TIMERS

static stk::time::TimerHost *g_TimerHost = nullptr;

// Static storage for the TimerHost (avoids heap for the host object itself).
static stk::Word g_TimerHostBuf[StkGetWordCountForType<stk::time::TimerHost>()];

struct FrtosTimer : public stk::time::TimerHost::Timer
{
    explicit FrtosTimer(const char             *name,
                        TickType_t              period,
                        bool                    auto_reload,
                        void                   *timer_id,
                        TimerCallbackFunction_t  cb)
        : m_name(name), m_period(period), m_auto_reload(auto_reload),
          m_timer_id(timer_id), m_callback(cb), m_cb_owned(true)
    {}

    virtual ~FrtosTimer() {}

    void OnExpired(stk::time::TimerHost * /*host*/) override
    {
        m_callback(static_cast<TimerHandle_t>(this));
    }

    static bool EnsureTimerHost()
    {
        if (g_TimerHost == nullptr)
        {
            g_TimerHost = new (g_TimerHostBuf) stk::time::TimerHost();
            g_TimerHost->Initialize(&g_StkKernel, stk::ACCESS_PRIVILEGED);
        }
        return (g_TimerHost != nullptr);
    }

    const char             *m_name;
    TickType_t              m_period;       // ticks, stored for Reset/ChangePeriod
    bool                    m_auto_reload;
    void                   *m_timer_id;
    TimerCallbackFunction_t m_callback;
    bool                    m_cb_owned;
};

// ===========================================================================
// PendCall / g_PendCallPipe / FrtosPendDrainer
//
// Design
// ------
// A deferred call is represented as a plain value struct (PendCall) holding
// the callback pointer and its two parameters.  All in-flight calls live in a
// statically-allocated ring-buffer:
//
//   static stk::sync::PipeT<PendCall, FREERTOS_STK_PEND_CALL_QUEUE_SIZE>
//       g_PendCallPipe;
//
// FrtosPendDrainer is a singleton TimerHost::Timer that is started once (as a
// 1-tick auto-reload timer) the first time xTimerPendFunctionCall[FromISR]()
// is called.  On every OnExpired() tick it drains g_PendCallPipe to completion,
// invoking each PendCall callback in the TimerHost handler task context —
// exactly where FreeRTOS's timer daemon would run them.
//
// Advantages
// --------------------------------
//   * Zero heap allocation per call — no pvPortMalloc / ObjFreeRaw per pend.
//   * ISR path no longer requires the allocator to be ISR-reentrant.
//   * Static RAM cost is fixed and visible at link time
//     (FREERTOS_STK_PEND_CALL_QUEUE_SIZE * sizeof(PendCall) bytes).
//   * No self-deleting object pattern; ownership is unconditionally clear.
//   * PipeT::TryWrite() (used from ISR) is ISR-safe via ScopedCriticalSection.
//   * PipeT::Write() (used from task context) supports a real blocking timeout.
//
// Lifecycle of g_PendDrainer
// --------------------------
//   Constructed in static storage (g_PendDrainerBuf) on first use.
//   Started as a 1-tick auto-reload timer so OnExpired() is called every tick
//   while the pipe is non-empty.  To avoid burning timer ticks when idle the
//   drainer stops itself when it finds the pipe empty, and is re-started by
//   xTimerPendFunctionCall[FromISR]() whenever a new call is enqueued.
// ===========================================================================

// Value type: one deferred call record (no virtual dispatch, no heap).
struct PendCall
{
    PendedFunction_t func;
    void            *param1;
    uint32_t         param2;
};

// Static ring-buffer — capacity set by FREERTOS_STK_PEND_CALL_QUEUE_SIZE.
static stk::sync::PipeT<PendCall, FREERTOS_STK_PEND_CALL_QUEUE_SIZE> g_PendCallPipe;

// Static storage for the singleton drainer timer (avoids a heap allocation).
static stk::Word g_PendDrainerBuf[StkGetWordCountForType<stk::time::TimerHost::Timer>()];
static stk::time::TimerHost::Timer *g_PendDrainer = nullptr;

// Singleton drainer: drains g_PendCallPipe each time it fires, then stops
// itself when the pipe is empty to avoid unnecessary timer ticks.
struct FrtosPendDrainer : public stk::time::TimerHost::Timer
{
    void OnExpired(stk::time::TimerHost *host) override
    {
        PendCall call;
        while (g_PendCallPipe.TryRead(call))
            call.func(call.param1, call.param2);

        // Pipe is empty: stop the auto-reload drainer until the next enqueue.
        // host->Stop() is safe to call from OnExpired() because the TimerHost
        // removes the timer from the active list before dispatching OnExpired.
        if (host != nullptr)
            host->Stop(*this);
    }
};

// Ensure the drainer timer is constructed and started (idempotent; ISR-unsafe).
// Must be called from task context only (same restriction as EnsureTimerHost).
static bool EnsurePendDrainer()
{
    if (g_PendDrainer == nullptr)
        g_PendDrainer = new (g_PendDrainerBuf) FrtosPendDrainer();

    // Re-start as a 1-tick auto-reload timer each time we have new work.
    // Restart() is idempotent if already active.
    return g_TimerHost->Restart(*g_PendDrainer, 1U, 1U);
}

// Kick the drainer from ISR context after a TryWrite succeeds.
// Uses Start() with NO_WAIT which is ISR-safe via PipeT / ScopedCriticalSection.
// If the drainer is already active this is a no-op (Restart would re-arm it,
// which is fine; the extra tick is harmless).
static void KickPendDrainerFromISR()
{
    if ((g_PendDrainer != nullptr) && (g_TimerHost != nullptr))
        g_TimerHost->Restart(*g_PendDrainer, 1U, 1U);
}

#endif // configUSE_TIMERS

// ===========================================================================
// Event group control block  [configUSE_EVENT_GROUPS]
//
// Backed by stk::sync::EventFlags (32-bit; bits 0..30 are usable,
// bit 31 is reserved by STK for error sentinels).
// FreeRTOS conventionally uses only bits 0..23 for event groups.
// ===========================================================================

#if configUSE_EVENT_GROUPS

struct FrtosEventGroup
{
    explicit FrtosEventGroup() : m_ef(0U), m_cb_owned(true)
    {}

    stk::sync::EventFlags m_ef;
    bool                  m_cb_owned;
};

#endif // configUSE_EVENT_GROUPS

// ---------------------------------------------------------------------------
// FrtosStreamBuffer  [configUSE_STREAM_BUFFERS]
//
// Backed by a stk::sync::Pipe with element_size = 1 (byte ring-buffer).
// sync::Pipe is chosen over sync::MessageQueue because it exposes WriteBulk /
// ReadBulk / TryWriteBulk / TryReadBulk directly, which are required for
// efficient multi-byte stream transfers and for the trigger-level logic in
// xStreamBufferReceive().
//
// The data buffer is either heap-owned (m_buf_owned = true) or external
// (caller-supplied via xStreamBufferCreateStatic).
//
// Trigger level: xStreamBufferReceive() delegates entirely to
// Pipe::ReadBulkTriggered(), which blocks until m_trigger bytes are present
// and then drains up to xBufferLengthBytes in one atomic CS pass —
// no busy-spin, no second call, no lost-wakeup risk.
// ---------------------------------------------------------------------------

#if configUSE_STREAM_BUFFERS

struct FrtosStreamBuffer
{
    // Constructor: caller pre-allocates buf and may transfer ownership via
    // m_buf_owned / m_cb_owned (overridden by the Create helpers after construction).
    // pSendCb / pRecvCb are optional per-instance notification callbacks;
    // both default to nullptr (no callback).
    explicit FrtosStreamBuffer(uint8_t                        *buf,
                                size_t                          capacity,
                                size_t                          trigger,
                                StreamBufferCallbackFunction_t  pSendCb = nullptr,
                                StreamBufferCallbackFunction_t  pRecvCb = nullptr)
        : m_pipe(buf, capacity, 1U),
          m_trigger(trigger >= 1U ? trigger : 1U),
          m_buf_owned(false),  // overridden to true by xStreamBufferCreate after ctor
          m_cb_owned(false),   // overridden to true by xStreamBufferCreate after ctor
          m_send_cb(pSendCb),
          m_recv_cb(pRecvCb)
    {}

    ~FrtosStreamBuffer()
    {
        if (m_buf_owned)
            ObjFreeArray(m_pipe.GetBuffer());
    }

    stk::sync::Pipe                m_pipe;       //!< byte ring-buffer (element_size = 1)
    size_t                         m_trigger;    //!< minimum bytes before Receive() unblocks
    bool                           m_buf_owned;  //!< true -> data buffer heap-allocated, freed in dtor
    bool                           m_cb_owned;   //!< true -> struct heap-allocated, deleted in vStreamBufferDelete
    StreamBufferCallbackFunction_t m_send_cb;    //!< optional callback fired after a successful Send
    StreamBufferCallbackFunction_t m_recv_cb;    //!< optional callback fired after a successful Receive
};

// ---------------------------------------------------------------------------
// FrtosMessageBuffer
//
// An envelope struct is pushed into m_eq for every message.  The payload lives
// in a block pool block.  On Receive() the envelope is popped, payload copied
// out, and the block returned to the pool.
//
// Layout of the caller-supplied flat buffer for the static constructor:
//   [ block_pool_storage | envelope_queue_storage ]
// where
//   block_pool_storage  = xMessageCount * AlignBlockSize(xMaxMessageSize)
//   envelope_queue_storage = xMessageCount * sizeof(MsgEnvelope)
// ---------------------------------------------------------------------------
struct FrtosMessageBuffer
{
    struct MsgEnvelope
    {
        size_t  len; //!< payload length in bytes
        void   *blk; //!< pointer to block pool block holding the payload
    };

    static constexpr size_t ENVELOPE_SIZE = sizeof(MsgEnvelope);

    // Heap constructor.
    // pSendCb / pRecvCb are optional per-instance notification callbacks;
    // both default to nullptr (no callback).
    explicit FrtosMessageBuffer(size_t                         max_msg_size,
                                 size_t                         msg_count,
                                 StreamBufferCallbackFunction_t pSendCb = nullptr,
                                 StreamBufferCallbackFunction_t pRecvCb = nullptr)
        : m_pool(msg_count,
                 stk::memory::BlockMemoryPool::AlignBlockSize(max_msg_size)),
          m_eq(ObjAllocArray<uint8_t>(msg_count * ENVELOPE_SIZE),
               msg_count, ENVELOPE_SIZE),
          m_max_msg_size(max_msg_size),
          m_eq_buf_owned(true),
          m_cb_owned(true),
          m_send_cb(pSendCb),
          m_recv_cb(pRecvCb)
    {}

    // Static constructor: uses caller-supplied flat storage buffer.
    // Layout: [pool_storage | eq_storage] as described above.
    explicit FrtosMessageBuffer(size_t                         max_msg_size,
                                 size_t                         msg_count,
                                 uint8_t                       *storage,
                                 size_t                         storage_size,
                                 StreamBufferCallbackFunction_t pSendCb = nullptr,
                                 StreamBufferCallbackFunction_t pRecvCb = nullptr)
        : m_pool(msg_count,
                 stk::memory::BlockMemoryPool::AlignBlockSize(max_msg_size),
                 storage,
                 msg_count * stk::memory::BlockMemoryPool::AlignBlockSize(max_msg_size)),
          m_eq(storage + msg_count * stk::memory::BlockMemoryPool::AlignBlockSize(max_msg_size),
               msg_count, ENVELOPE_SIZE),
          m_max_msg_size(max_msg_size),
          m_eq_buf_owned(false),
          m_cb_owned(false),
          m_send_cb(pSendCb),
          m_recv_cb(pRecvCb)
    {
        STK_UNUSED(storage_size);
    }

    ~FrtosMessageBuffer()
    {
        if (m_eq_buf_owned)
            ObjFreeArray(static_cast<uint8_t *>(m_eq.GetBuffer()));
    }

    stk::memory::BlockMemoryPool   m_pool;         //!< payload block allocator
    stk::sync::MessageQueue        m_eq;           //!< envelope FIFO {len, blk}
    size_t                         m_max_msg_size; //!< max payload bytes per message
    bool                           m_eq_buf_owned; //!< true -> envelope buffer heap-allocated
    bool                           m_cb_owned;     //!< true -> struct heap-allocated
    StreamBufferCallbackFunction_t m_send_cb;      //!< optional callback fired after a successful Send
    StreamBufferCallbackFunction_t m_recv_cb;      //!< optional callback fired after a successful Receive
};

#endif // configUSE_STREAM_BUFFERS

// Ensure kernel is initialized.
static void EnsureKernelInitialized()
{
    if (g_StkKernel.GetState() == stk::IKernel::STATE_INACTIVE)
        g_StkKernel.Initialize(); // default 1 ms tick resolution
}

// ===========================================================================
// Kernel control
// ===========================================================================

void vTaskStartScheduler(void)
{
    EnsureKernelInitialized();

    g_StkKernel.Start(); // does not return for KERNEL_DYNAMIC until all tasks exit
}

void vTaskEndScheduler(void)
{
    g_StkKernel.EnumerateTasksT<FREERTOS_STK_MAX_TASKS>([&](stk::ITask *task) -> bool
    {
        g_StkKernel.ScheduleTaskRemoval(task);
        return true;
    });

    stk::Yield();
}

void vTaskSuspendAll(void)
{
    stk::hw::CriticalSection::Enter();
}

BaseType_t xTaskResumeAll(void)
{
    stk::hw::CriticalSection::Exit();
    return pdFALSE; // no pending switch tracked at wrapper level
}

TickType_t xTaskGetTickCount(void)
{
    return static_cast<TickType_t>(stk::GetTicks());
}

TickType_t xTaskGetTickCountFromISR(void)
{
    return static_cast<TickType_t>(stk::GetTicks()); // GetTicks() is ISR-safe
}

UBaseType_t uxTaskGetNumberOfTasks(void)
{
    stk::sync::ScopedCriticalSection cs_;
    return static_cast<UBaseType_t>(g_StkKernel.GetSwitchStrategy()->GetSize());
}

BaseType_t xTaskGetSchedulerState(void)
{
    // Map the four STK kernel states onto the three FreeRTOS scheduler states:
    //   STATE_INACTIVE / STATE_READY -> NOT_STARTED  (scheduler never ran)
    //   STATE_RUNNING               -> RUNNING
    //   STATE_SUSPENDED             -> SUSPENDED
    switch (g_StkKernel.GetState())
    {
    case stk::IKernel::STATE_RUNNING:   return taskSCHEDULER_RUNNING;
    case stk::IKernel::STATE_SUSPENDED: return taskSCHEDULER_SUSPENDED;
    default:                            return taskSCHEDULER_NOT_STARTED;
    }
}

// ===========================================================================
// Task management
// ===========================================================================

BaseType_t xTaskCreate(TaskFunction_t    pvTaskCode,
                                  const char       *pcName,
                                  uint32_t          usStackDepth,
                                  void             *pvParameters,
                                  UBaseType_t       uxPriority,
                                  TaskHandle_t     *pxCreatedTask)
{
    if (pvTaskCode == nullptr)
        return pdFAIL;

    FrtosTask *t = ObjAlloc<FrtosTask>();
    if (t == nullptr)
        return pdFAIL;

    t->m_func      = pvTaskCode;
    t->m_argument  = pvParameters;
    t->m_name      = pcName;
    t->m_weight    = FrtosPrioToStkWeight(uxPriority);

    // Determine stack size in Words.
    size_t stack_words = (usStackDepth > 0U)
                         ? static_cast<size_t>(usStackDepth)
                         : FREERTOS_STK_DEFAULT_STACK_WORDS;

    if (stack_words < FREERTOS_STK_MIN_STACK_WORDS)
        stack_words = FREERTOS_STK_MIN_STACK_WORDS;

    t->m_stack = ObjAllocArray<stk::Word>(stack_words);
    if (t->m_stack == nullptr)
    {
        ObjFree(t);
        return pdFAIL;
    }

    t->m_stack_size  = stack_words;
    t->m_stack_owned = true;

    EnsureKernelInitialized();

    g_StkKernel.AddTask(t);

    if (pxCreatedTask != nullptr)
        *pxCreatedTask = static_cast<TaskHandle_t>(t);

    return pdPASS;
}

TaskHandle_t xTaskCreateStatic(TaskFunction_t  pvTaskCode,
                               const char     *pcName,
                               uint32_t        ulStackDepth,
                               void           *pvParameters,
                               UBaseType_t     uxPriority,
                               StackType_t    *puxStackBuffer,
                               StaticTask_t   *pxTaskBuffer)
{
    // All three pointer arguments are mandatory for static allocation.
    if ((pvTaskCode == nullptr) || (puxStackBuffer == nullptr) || (pxTaskBuffer == nullptr))
        return nullptr;

    // Placement-new the FrtosTask control block into the caller-supplied TCB
    // buffer.  Static assert guards against the buffer being too small.
    static_assert(sizeof(StaticTask_t) >= sizeof(FrtosTask),
        "StaticTask_t is too small to hold FrtosTask. "
        "Increase STATIC_TASK_TCB_SIZE_WORDS in freertos_stk.h.");

    FrtosTask *t = new (pxTaskBuffer) FrtosTask();

    t->m_func        = pvTaskCode;
    t->m_argument    = pvParameters;
    t->m_name        = pcName;
    t->m_weight      = FrtosPrioToStkWeight(uxPriority);
    t->m_stack       = static_cast<stk::Word *>(static_cast<void *>(puxStackBuffer));
    t->m_stack_size  = (ulStackDepth >= FREERTOS_STK_MIN_STACK_WORDS)
                       ? static_cast<size_t>(ulStackDepth)
                       : FREERTOS_STK_MIN_STACK_WORDS;
    t->m_stack_owned = false; // caller owns both the stack and the TCB
    t->m_cb_owned    = false; // destructor must not delete — caller owns memory

    EnsureKernelInitialized();

    g_StkKernel.AddTask(t);

    return static_cast<TaskHandle_t>(t);
}

void vTaskDelete(TaskHandle_t xTaskToDelete)
{
    FrtosTask *t = (xTaskToDelete == nullptr) ? static_cast<FrtosTask *>(
        reinterpret_cast<FrtosTask *>(static_cast<uintptr_t>(stk::GetTid()))) : static_cast<FrtosTask *>(xTaskToDelete);

    if (t == nullptr)
        return;

    stk::sync::ScopedCriticalSection cs_;

    g_StkKernel.ScheduleTaskRemoval(t);

    // Detached tasks are freed when the slot is released.
    // For this wrapper, all tasks are considered detached (no join semantics).
    ObjFree(t);
}

void vTaskSuspend(TaskHandle_t xTaskToSuspend)
{
    FrtosTask *t = (xTaskToSuspend == nullptr) ? static_cast<FrtosTask *>(
        reinterpret_cast<FrtosTask *>(static_cast<uintptr_t>(stk::GetTid()))) :  static_cast<FrtosTask *>(xTaskToSuspend);

    if (t == nullptr)
        return;

    bool already = false;
    g_StkKernel.SuspendTask(t, already);
    t->m_state = FrtosTask::State::Suspended;
}

void vTaskResume(TaskHandle_t xTaskToResume)
{
    if (xTaskToResume == nullptr)
        return;

    FrtosTask *t = static_cast<FrtosTask *>(xTaskToResume);

    stk::sync::ScopedCriticalSection cs_;

    if (t->m_state != FrtosTask::State::Suspended)
        return;

    g_StkKernel.ResumeTask(t);
    t->m_state = FrtosTask::State::Ready;
}

BaseType_t xTaskResumeFromISR(TaskHandle_t xTaskToResume)
{
    if (xTaskToResume == nullptr)
        return pdFALSE;

    FrtosTask *t = static_cast<FrtosTask *>(xTaskToResume);

    stk::sync::ScopedCriticalSection cs_;

    if (t->m_state != FrtosTask::State::Suspended)
        return pdFALSE;

    g_StkKernel.ResumeTask(t);
    t->m_state = FrtosTask::State::Ready;

    return pdTRUE;
}

BaseType_t xTaskAbortDelay(TaskHandle_t xTask)
{
    if (xTask == nullptr)
        return pdFALSE;

    // Resolve NULL -> calling task (same convention used throughout the wrapper).
    const stk::TId tid = static_cast<stk::TId>(reinterpret_cast<uintptr_t>(xTask));

    const FrtosTask *t = static_cast<const FrtosTask *>(xTask);
    if (t->m_state != FrtosTask::State::Ready)
        return pdFAIL; // suspended or otherwise not in a delay-able state

    stk::SleepCancel(tid);
    return pdPASS;
}

void vTaskDelay(TickType_t xTicksToDelay)
{
    if (IsIrqContext())
        return;

    stk::Sleep(FrtosTimeoutToStk(xTicksToDelay));
}

void vTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement)
{
    static_cast<void>(xTaskDelayUntil(pxPreviousWakeTime, xTimeIncrement));
}

BaseType_t xTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement)
{
    if (IsIrqContext() || (pxPreviousWakeTime == nullptr))
        return pdFALSE;

    const stk::Ticks wake_at = static_cast<stk::Ticks>(*pxPreviousWakeTime) +
                               static_cast<stk::Ticks>(xTimeIncrement);

    *pxPreviousWakeTime = static_cast<TickType_t>(wake_at);

    return stk::SleepUntil(wake_at) ? pdTRUE : pdFALSE;
}

void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority)
{
    FrtosTask *t = (xTask == nullptr) ? reinterpret_cast<FrtosTask *>(
            static_cast<uintptr_t>(stk::GetTid())) : static_cast<FrtosTask *>(xTask);

    if (t == nullptr)
        return;

    t->m_weight = FrtosPrioToStkWeight(uxNewPriority);
}

UBaseType_t uxTaskPriorityGet(TaskHandle_t xTask)
{
    const FrtosTask *t = (xTask == nullptr) ? reinterpret_cast<const FrtosTask *>(
        static_cast<uintptr_t>(stk::GetTid())) : static_cast<const FrtosTask *>(xTask);

    if (t == nullptr)
        return 0U;

    return StkWeightToFrtosPrio(t->m_weight);
}

UBaseType_t uxTaskPriorityGetFromISR(TaskHandle_t xTask)
{
    return uxTaskPriorityGet(xTask); // same implementation; GetTid() is ISR-safe
}

eTaskState eTaskGetState(TaskHandle_t xTask)
{
    if (xTask == nullptr)
        return eInvalid;

    FrtosTask *t = static_cast<FrtosTask *>(xTask);

    if (t->m_state == FrtosTask::State::Deleted)
        return eDeleted;

    if (t->m_state == FrtosTask::State::Suspended)
        return eSuspended;

    // Check whether this is the currently running task.
    if (static_cast<uintptr_t>(stk::GetTid()) == reinterpret_cast<uintptr_t>(t))
        return eRunning;

    return eReady;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    if (IsIrqContext())
        return nullptr;

    return reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(stk::GetTid()));
}

TaskHandle_t xTaskGetHandle(const char *pcNameToQuery)
{
    if (pcNameToQuery == nullptr)
        return nullptr;

    // Enumerate all tasks and compare names.
    TaskHandle_t found = nullptr;

    g_StkKernel.EnumerateTasksT<FREERTOS_STK_MAX_TASKS>([&](stk::ITask *task) -> bool
    {
        if ((task->GetTraceName() != nullptr) &&
            (strcmp(task->GetTraceName(), pcNameToQuery) == 0))
        {
            found = static_cast<TaskHandle_t>(task);
            return false; // stop iteration
        }
        return true;
    });

    return found;
}

const char *pcTaskGetName(TaskHandle_t xTaskToQuery)
{
    if (xTaskToQuery == nullptr)
        return nullptr;

    return static_cast<FrtosTask *>(xTaskToQuery)->m_name;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask)
{
    const FrtosTask *t = (xTask == nullptr) ? reinterpret_cast<const FrtosTask *>(
            static_cast<uintptr_t>(stk::GetTid())) : static_cast<const FrtosTask *>(xTask);

    if (t == nullptr)
        return 0U;

    return static_cast<UBaseType_t>(t->GetStackHighWaterMark());
}

configSTACK_DEPTH_TYPE uxTaskGetStackHighWaterMark2(TaskHandle_t xTask)
{
    const FrtosTask *t = (xTask == nullptr) ? reinterpret_cast<const FrtosTask *>(
            static_cast<uintptr_t>(stk::GetTid())) : static_cast<const FrtosTask *>(xTask);

    if (t == nullptr)
        return 0U;

    return static_cast<configSTACK_DEPTH_TYPE>(t->GetStackHighWaterMark());
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t    *pxTaskStatusArray,
                                  UBaseType_t      uxArraySize,
                                  uint32_t        *pulTotalRunTime)
{
    // STK has no global CPU run-time accumulator; report 0 per the FreeRTOS
    // convention for targets that do not implement run-time statistics.
    if (pulTotalRunTime != nullptr)
        *pulTotalRunTime = 0U;

    if ((pxTaskStatusArray == nullptr) || (uxArraySize == 0U))
        return 0U;

    UBaseType_t filled = 0U;

    // Identify the currently running task once, outside the enumeration
    // loop, so the eRunning check is consistent across all entries.
    const uintptr_t running_tid = static_cast<uintptr_t>(stk::GetTid());

    g_StkKernel.EnumerateTasksT<FREERTOS_STK_MAX_TASKS>([&](stk::ITask *itask) -> bool
    {
        if (filled >= uxArraySize)
            return false; // array full — stop enumeration

        const FrtosTask *t = static_cast<const FrtosTask *>(itask);
        TaskStatus_t    &s = pxTaskStatusArray[filled];

        // xHandle — the FrtosTask pointer cast to an opaque handle.
        s.xHandle = static_cast<TaskHandle_t>(const_cast<FrtosTask *>(t));

        // pcTaskName — direct pointer into the task's name buffer (not a copy).
        s.pcTaskName = (t->m_name != nullptr) ? t->m_name : "";

        // eCurrentState — mirrors eTaskGetState() logic.
        if (t->m_state == FrtosTask::State::Deleted)
            s.eCurrentState = eDeleted;
        else
        if (t->m_state == FrtosTask::State::Suspended)
            s.eCurrentState = eSuspended;
        else
        if (reinterpret_cast<uintptr_t>(t) == running_tid)
            s.eCurrentState = eRunning;
        else
            s.eCurrentState = eReady;

        // uxCurrentPriority / uxBasePriority — same value: STK has no
        // priority inheritance so the current and base priority are identical.
        s.uxCurrentPriority = StkWeightToFrtosPrio(t->m_weight);
        s.uxBasePriority    = s.uxCurrentPriority;

        // ulRunTimeCounter — always 0 (STK has no per-task CPU accounting).
        s.ulRunTimeCounter = 0U;

        // pxStackBase — bottom of the stack array (index 0).
        s.pxStackBase = reinterpret_cast<StackType_t *>(t->m_stack);

        // usStackHighWaterMark — minimum observed free Words (watermark scan).
        s.usStackHighWaterMark =
            static_cast<configSTACK_DEPTH_TYPE>(t->GetStackHighWaterMark());

        // xTaskNumber — monotonic serial assigned at construction.
        s.xTaskNumber = t->m_task_number;

        ++filled;
        return true; // continue enumeration
    });

    return filled;
}

BaseType_t xTaskCreateRestrictedStatic(
    const TaskParameters_restricted_t *pxTaskDefinition,
    TaskHandle_t                       *pxCreatedTask)
{
    // Mandatory pointer guard.
    if ((pxTaskDefinition == nullptr) ||
        (pxTaskDefinition->pvTaskCode     == nullptr) ||
        (pxTaskDefinition->puxStackBuffer == nullptr) ||
        (pxTaskDefinition->pxTaskBuffer   == nullptr))
        return pdFAIL;

    // STK does not implement MPU support.  Forward to xTaskCreateStatic(),
    // accepting but ignoring the xRegions MPU region table.
    // TODO: program pxTaskDefinition->xRegions into the MPU when STK gains
    //       hardware MPU support.
    TaskHandle_t h = xTaskCreateStatic(
        pxTaskDefinition->pvTaskCode,
        pxTaskDefinition->pcName,
        pxTaskDefinition->usStackDepth,
        pxTaskDefinition->pvParameters,
        pxTaskDefinition->uxPriority,
        pxTaskDefinition->puxStackBuffer,
        pxTaskDefinition->pxTaskBuffer);

    if (h == nullptr)
        return pdFAIL;

    if (pxCreatedTask != nullptr)
        *pxCreatedTask = h;

    return pdPASS;
}

BaseType_t xTaskCreateRestricted(
    const TaskParameters_restricted_t *pxTaskDefinition,
    TaskHandle_t                       *pxCreatedTask)
{
    // Mandatory pointer guard (only pvTaskCode is required; the caller need not
    // supply puxStackBuffer or pxTaskBuffer — both are heap-allocated here).
    if ((pxTaskDefinition == nullptr) ||
        (pxTaskDefinition->pvTaskCode == nullptr))
        return pdFAIL;

    // STK does not implement MPU support.  Forward to xTaskCreate() which
    // heap-allocates both the TCB and the task stack, accepting but ignoring
    // the xRegions MPU region table.
    // TODO: program pxTaskDefinition->xRegions into the MPU when STK gains
    //       hardware MPU support.
    return xTaskCreate(
        pxTaskDefinition->pvTaskCode,
        pxTaskDefinition->pcName,
        pxTaskDefinition->usStackDepth,
        pxTaskDefinition->pvParameters,
        pxTaskDefinition->uxPriority,
        pxCreatedTask);
}

void vTaskList(char *pcWriteBuffer)
{
    if (pcWriteBuffer == nullptr)
        return;

    // Write the column header that FreeRTOS vTaskList() produces, so that
    // existing log parsers find what they expect.
    int off = snprintf(pcWriteBuffer, 64U,
        "%-12s %c %4s %6s %4s\r\n",
        "Name", 'S', "Prio", "Stack", "Num");

    if (off < 0) off = 0;
    char *p = pcWriteBuffer + off;

    // Enumerate all tasks and fill one row per task.
    UBaseType_t task_num = 0U;

    g_StkKernel.EnumerateTasksT<FREERTOS_STK_MAX_TASKS>([&](stk::ITask *itask) -> bool
    {
        ++task_num;
        FrtosTask *t = static_cast<FrtosTask *>(itask);

        // Determine state letter, matching FreeRTOS convention:
        //   X = Running, R = Ready, B = Blocked, S = Suspended, D = Deleted
        char state_letter;
        if (t->m_state == FrtosTask::State::Deleted)
            state_letter = 'D';
        else
        if (t->m_state == FrtosTask::State::Suspended)
            state_letter = 'S';
        else
        if (static_cast<uintptr_t>(stk::GetTid()) == reinterpret_cast<uintptr_t>(t))
            state_letter = 'X';
        else
            state_letter = 'R';

        const char   *name = (t->m_name != nullptr) ? t->m_name : "(unnamed)";
        UBaseType_t   prio = StkWeightToFrtosPrio(t->m_weight);
        size_t        hwm  = t->GetStackHighWaterMark();

        int n = snprintf(p, 48U, "%-12s %c %4u %6u %4u\r\n",
                         name, state_letter,
                         static_cast<unsigned>(prio),
                         static_cast<unsigned>(hwm),
                         static_cast<unsigned>(task_num));
        if (n > 0) p += n;

        return true; // continue enumeration
    });

    *p = '\0'; // null-terminate
}

// ---------------------------------------------------------------------------
// vTaskGetRunTimeStats
// ---------------------------------------------------------------------------
// Produces the standard FreeRTOS run-time statistics table:
//
//   Task            Abs Time    % Time
//   ----------------------------------------
//   TaskName               0        0%
//   ...
//
// STK has no per-task CPU run-time accumulator (ulRunTimeCounter is always 0
// and the total run time reported by uxTaskGetSystemState() is always 0).
// Following the FreeRTOS convention for targets where
// configGENERATE_RUN_TIME_STATS is disabled, both columns are emitted as 0.
// The function exists for link compatibility with middleware and diagnostic
// tools that call it unconditionally.
// ---------------------------------------------------------------------------

void vTaskGetRunTimeStats(char *pcWriteBuffer)
{
    if (pcWriteBuffer == nullptr)
        return;

    // Column header matching FreeRTOS vTaskGetRunTimeStats() output format so
    // that existing log parsers (SystemView, Tracealyzer, custom scripts) find
    // the layout they expect.
    int off = snprintf(pcWriteBuffer, 64U,
        "%-12s %12s %8s\r\n",
        "Task", "Abs Time", "% Time");

    if (off < 0) off = 0;
    char *p = pcWriteBuffer + off;

    g_StkKernel.EnumerateTasksT<FREERTOS_STK_MAX_TASKS>([&](stk::ITask *itask) -> bool
    {
        const FrtosTask *t    = static_cast<const FrtosTask *>(itask);
        const char      *name = (t->m_name != nullptr) ? t->m_name : "(unnamed)";

        // ulRunTimeCounter is always 0: STK has no per-task CPU accounting.
        // Percentage is therefore also 0.  Emit "<1%" only when a non-zero
        // total is available; here total is always 0 so we emit "0%".
        int n = snprintf(p, 48U, "%-12s %12lu %7lu%%\r\n",
                         name,
                         0UL,  // ulRunTimeCounter
                         0UL); // percentage
        if (n > 0) p += n;

        return true; // continue enumeration
    });

    *p = '\0'; // null-terminate
}

// ===========================================================================
// Queue API
// ===========================================================================

QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength,
                           UBaseType_t uxItemSize)
{
    if (IsIrqContext() || (uxQueueLength == 0U) || (uxItemSize == 0U))
        return nullptr;

    if (uxQueueLength > stk::sync::MessageQueue::CAPACITY_MAX)
        return nullptr;

    FrtosQueue *q = ObjAlloc<FrtosQueue>(
        static_cast<uint32_t>(uxQueueLength),
        static_cast<uint32_t>(uxItemSize),
        nullptr /* name */);

    if (q == nullptr)
        return nullptr;

    if (!q->m_mq.IsStorageValid())
    {
        ObjFree(q);
        return nullptr;
    }

    return static_cast<QueueHandle_t>(q);
}

QueueHandle_t xQueueCreateStatic(UBaseType_t    uxQueueLength,
                                  UBaseType_t    uxItemSize,
                                  uint8_t       *pucQueueStorage,
                                  StaticQueue_t *pxStaticQueue)
{
    // All pointer arguments are mandatory for static allocation.
    if ((pucQueueStorage == nullptr) || (pxStaticQueue == nullptr))
        return nullptr;

    if (IsIrqContext() || (uxQueueLength == 0U) || (uxItemSize == 0U))
        return nullptr;

    if (uxQueueLength > stk::sync::MessageQueue::CAPACITY_MAX)
        return nullptr;

    // Placement-new the FrtosQueue control block into the caller-supplied buffer.
    // Static assert guards against the buffer being too small.
    static_assert(sizeof(StaticQueue_t) >= sizeof(FrtosQueue),
        "StaticQueue_t is too small to hold FrtosQueue. "
        "Increase STATIC_QUEUE_TCB_SIZE_WORDS in freertos_stk.h.");

    // Use the external-storage FrtosQueue constructor: no heap allocation for
    // either the control block or the data buffer.
    FrtosQueue *q = new (pxStaticQueue) FrtosQueue(
        static_cast<uint32_t>(uxQueueLength),
        static_cast<uint32_t>(uxItemSize),
        nullptr /* name */,
        pucQueueStorage);

    q->m_cb_owned = false; // caller owns the memory; destructor must not delete

    return static_cast<QueueHandle_t>(q);
}

void vQueueDelete(QueueHandle_t xQueue)
{
    if (xQueue == nullptr)
        return;

    ObjFree(static_cast<FrtosQueue *>(xQueue));
}

BaseType_t xQueueSend(QueueHandle_t xQueue,
                      const void   *pvItemToQueue,
                      TickType_t    xTicksToWait)
{
    if ((xQueue == nullptr) || (pvItemToQueue == nullptr))
        return pdFAIL;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return pdFAIL;

    FrtosQueue *q = static_cast<FrtosQueue *>(xQueue);

    if (!q->m_mq.Put(pvItemToQueue, FrtosTimeoutToStk(xTicksToWait)))
        return pdFAIL;

    QueueSetNotify(xQueue, q);
    return pdPASS;
}

BaseType_t xQueueSendToBack(QueueHandle_t xQueue,
                            const void   *pvItemToQueue,
                            TickType_t    xTicksToWait)
{
    return xQueueSend(xQueue, pvItemToQueue, xTicksToWait);
}

BaseType_t xQueueSendToFront(QueueHandle_t xQueue,
                             const void   *pvItemToQueue,
                             TickType_t    xTicksToWait)
{
    if ((xQueue == nullptr) || (pvItemToQueue == nullptr))
        return pdFAIL;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return pdFAIL;

    FrtosQueue *q = static_cast<FrtosQueue *>(xQueue);

    if (!q->m_mq.PutFront(pvItemToQueue, FrtosTimeoutToStk(xTicksToWait)))
        return pdFAIL;

    QueueSetNotify(xQueue, q);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t xQueue,
                         void         *pvBuffer,
                         TickType_t    xTicksToWait)
{
    if ((xQueue == nullptr) || (pvBuffer == nullptr))
        return pdFAIL;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return pdFAIL;

    return static_cast<FrtosQueue *>(xQueue)->m_mq.Get(pvBuffer, FrtosTimeoutToStk(xTicksToWait))
       ? pdPASS : pdFAIL;
}

BaseType_t xQueuePeek(QueueHandle_t xQueue,
                      void         *pvBuffer,
                      TickType_t    xTicksToWait)
{
    // Delegates to Peek(), which copies the oldest message without consuming
    // it.  The operation is fully atomic and preserves queue ordering — no
    // Get + Put-back workaround required.
    if ((xQueue == nullptr) || (pvBuffer == nullptr))
        return pdFAIL;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return pdFAIL;

    return static_cast<FrtosQueue *>(xQueue)->m_mq.Peek(
               pvBuffer, FrtosTimeoutToStk(xTicksToWait))
           ? pdPASS : pdFAIL;
}

BaseType_t xQueuePeekFromISR(QueueHandle_t xQueue,
                              void         *pvBuffer)
{
    // Delegates to TryPeek() (= Peek(NO_WAIT)), which is ISR-safe and copies
    // the oldest message atomically without removing it.
    if ((xQueue == nullptr) || (pvBuffer == nullptr))
        return pdFAIL;

    return static_cast<FrtosQueue *>(xQueue)->m_mq.TryPeek(pvBuffer) ? pdPASS : pdFAIL;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t xQueue)
{
    if (xQueue == nullptr)
        return 0U;

    return static_cast<UBaseType_t>(
        static_cast<FrtosQueue *>(xQueue)->m_mq.GetCount());
}

UBaseType_t uxQueueMessagesWaitingFromISR(QueueHandle_t xQueue)
{
    // GetCount() is ISR-safe on targets where a size_t-aligned read is atomic
    // (per the STK MessageQueue documentation).
    if (xQueue == nullptr)
        return 0U;

    return static_cast<UBaseType_t>(static_cast<FrtosQueue *>(xQueue)->m_mq.GetCount());
}

UBaseType_t uxQueueSpacesAvailable(QueueHandle_t xQueue)
{
    if (xQueue == nullptr)
        return 0U;

    return static_cast<UBaseType_t>(
        static_cast<FrtosQueue *>(xQueue)->m_mq.GetSpace());
}

BaseType_t xQueueReset(QueueHandle_t xQueue)
{
    if (xQueue == nullptr)
        return pdFAIL;

    static_cast<FrtosQueue *>(xQueue)->m_mq.Reset();
    return pdPASS;
}

BaseType_t xQueueOverwrite(QueueHandle_t xQueue, const void *pvItemToQueue)
{
    // Mailbox (length-1 queue) overwrite pattern.
    // Reset() atomically discards any existing item and wakes blocked producers,
    // guaranteeing TryPut() will always find a free slot immediately after.
    if ((xQueue == nullptr) || (pvItemToQueue == nullptr))
        return pdFAIL;

    FrtosQueue *q = static_cast<FrtosQueue *>(xQueue);
    q->m_mq.Reset();
    q->m_mq.TryPut(pvItemToQueue);
    QueueSetNotify(xQueue, q);

    return pdPASS;
}

BaseType_t xQueueOverwriteFromISR(QueueHandle_t  xQueue,
                                   const void    *pvItemToQueue,
                                   BaseType_t    *pxHigherPriorityTaskWoken)
{
    // ISR-safe variant of xQueueOverwrite.  Reset() and TryPut() are both
    // ISR-safe per the STK MessageQueue contract.
    if ((xQueue == nullptr) || (pvItemToQueue == nullptr))
        return pdFAIL;

    FrtosQueue *q = static_cast<FrtosQueue *>(xQueue);
    q->m_mq.Reset();
    q->m_mq.TryPut(pvItemToQueue);
    QueueSetNotify(xQueue, q);

    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return pdPASS;
}

BaseType_t xQueueSendFromISR(QueueHandle_t  xQueue,
                                        const void    *pvItemToQueue,
                                        BaseType_t    *pxHigherPriorityTaskWoken)
{
    if ((xQueue == nullptr) || (pvItemToQueue == nullptr))
        return pdFAIL;

    FrtosQueue *q = static_cast<FrtosQueue *>(xQueue);
    bool ok = q->m_mq.TryPut(pvItemToQueue);

    if (ok)
        QueueSetNotify(xQueue, q);

    // STK handles the wake-up internally; the wrapper does not need to
    // request an explicit yield from ISR because SWRR re-evaluates on the
    // next tick.  Set the flag to pdFALSE to avoid spurious portYIELD_FROM_ISR.
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return ok ? pdPASS : pdFAIL;
}

BaseType_t xQueueReceiveFromISR(QueueHandle_t  xQueue,
                                           void          *pvBuffer,
                                           BaseType_t    *pxHigherPriorityTaskWoken)
{
    if ((xQueue == nullptr) || (pvBuffer == nullptr))
        return pdFAIL;

    bool ok = static_cast<FrtosQueue *>(xQueue)->m_mq.TryGet(pvBuffer);

    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return ok ? pdPASS : pdFAIL;
}

BaseType_t xQueueSendToBackFromISR(QueueHandle_t  xQueue,
                                   const void    *pvItemToQueue,
                                   BaseType_t    *pxHigherPriorityTaskWoken)
{
    // Send-to-back from ISR is identical to xQueueSendFromISR: TryPut()
    // appends to the back of the ring buffer.
    return xQueueSendFromISR(xQueue, pvItemToQueue, pxHigherPriorityTaskWoken);
}

BaseType_t xQueueSendToFrontFromISR(QueueHandle_t  xQueue,
                                    const void    *pvItemToQueue,
                                    BaseType_t    *pxHigherPriorityTaskWoken)
{
    if ((xQueue == nullptr) || (pvItemToQueue == nullptr))
        return pdFAIL;

    FrtosQueue *q = static_cast<FrtosQueue *>(xQueue);
    bool ok = q->m_mq.TryPutFront(pvItemToQueue);

    if (ok)
        QueueSetNotify(xQueue, q);

    // STK handles the wake-up internally; set the flag to pdFALSE to avoid
    // spurious portYIELD_FROM_ISR.
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return ok ? pdPASS : pdFAIL;
}

BaseType_t xQueueIsQueueEmptyFromISR(const QueueHandle_t xQueue)
{
    // IsEmpty() reads m_count which is size_t-aligned; ISR-safe on targets
    // where such a read is atomic (per stk::sync::MessageQueue contract).
    if (xQueue == nullptr)
        return pdTRUE;

    return static_cast<const FrtosQueue *>(xQueue)->m_mq.IsEmpty() ? pdTRUE : pdFALSE;
}

BaseType_t xQueueIsQueueFullFromISR(const QueueHandle_t xQueue)
{
    // IsFull() reads m_count and m_capacity; both are size_t-aligned and
    // m_capacity is const, so the read is ISR-safe on naturally-atomic targets
    // (per stk::sync::MessageQueue contract).
    if (xQueue == nullptr)
        return pdTRUE;

    return static_cast<const FrtosQueue *>(xQueue)->m_mq.IsFull() ? pdTRUE : pdFALSE;
}

// ---------------------------------------------------------------------------
// xQueueGetMutexHolder / xQueueGetMutexHolderFromISR
// ---------------------------------------------------------------------------
// FreeRTOS re-uses its internal queue struct for mutex semaphores, so these
// two functions exist as QueueHandle_t-typed aliases of the semaphore
// counterparts.  In this STK wrapper the object types are distinct:
//
//   FrtosQueue      — wraps stk::sync::MessageQueue; no mutex, no owner.
//   FrtosSemaphore  — wraps stk::sync::Semaphore (Counting) or
//                     stk::sync::Mutex (Mutex kind).
//
// Type discrimination:
//   The first byte at the handle address is read via GetSemKindFromHandle(),
//   which returns SemKind::None for any byte that is not a known SemKind
//   enumerator (0x80 = Counting, 0x81 = Mutex).  FrtosQueue byte[0] is
//   byte[0] of stk::sync::MessageQueue (an ITraceable vtable/data pointer)
//   and is never 0x80 or 0x81 under normal linking, so GetSemKindFromHandle()
//   reliably returns SemKind::None for plain queue handles.
//
//   GetSemKindFromHandle() == SemKind::Mutex  -> FrtosSemaphore (Mutex kind)
//   GetSemKindFromHandle() != SemKind::Mutex  -> FrtosQueue or other (no owner)
//
// If the handle is a FrtosSemaphore with SemKind::Mutex the call is forwarded
// to xSemaphoreGetMutexHolder[FromISR]() which reads Mutex::GetOwner().
// For a plain FrtosQueue, or for a counting/binary semaphore, NULL is returned
// because the owner concept does not apply to those object types.
//
// STK Mutex always supports priority inheritance; no additional bookkeeping is
// required here — GetOwner() already reflects the current holder.
// ---------------------------------------------------------------------------

#if configUSE_MUTEXES

// Helper: given a raw QueueHandle_t, return the FrtosSemaphore* if the handle
// is actually a mutex-kind semaphore, or nullptr otherwise.
static inline FrtosSemaphore *QueueHandleAsMutex(QueueHandle_t xQueue)
{
    if (xQueue == nullptr)
        return nullptr;

    // GetSemKindFromHandle() returns SemKind::None for FrtosQueue handles
    // (and any unrecognised object), SemKind::Counting or SemKind::Mutex for
    // FrtosSemaphore handles.  Only SemKind::Mutex carries an owner field.
    if (GetSemKindFromHandle(xQueue) != SemKind::Mutex)
        return nullptr;

    return static_cast<FrtosSemaphore *>(xQueue);
}

TaskHandle_t xQueueGetMutexHolder(QueueHandle_t xQueue)
{
    // Resolve to FrtosSemaphore (Mutex kind) or bail out.
    FrtosSemaphore *s = QueueHandleAsMutex(xQueue);
    if (s == nullptr)
        return nullptr;

    // Delegate to the semaphore variant which acquires a ScopedCriticalSection
    // to make the TId snapshot consistent with concurrent Unlock() calls.
    return xSemaphoreGetMutexHolder(static_cast<SemaphoreHandle_t>(xQueue));
}

TaskHandle_t xQueueGetMutexHolderFromISR(QueueHandle_t xQueue)
{
    // Resolve to FrtosSemaphore (Mutex kind) or bail out.
    FrtosSemaphore *s = QueueHandleAsMutex(xQueue);
    if (s == nullptr)
        return nullptr;

    // Delegate to the ISR-safe semaphore variant which reads GetOwner() via a
    // single pointer-sized atomic load — no additional critical section needed.
    return xSemaphoreGetMutexHolderFromISR(static_cast<SemaphoreHandle_t>(xQueue));
}

#endif // configUSE_MUTEXES

// ===========================================================================
// Queue Set API
//
// A queue set acts as a fan-in multiplexer: one task can block on multiple
// queues and/or binary/counting semaphores simultaneously, waking as soon as
// any member receives an item.
//
// Implementation model:
//   FrtosQueueSet owns an internal stk::sync::MessageQueue whose payload
//   element size is sizeof(void*).  Whenever a member queue or semaphore
//   successfully receives a new item it writes its own handle (a void*)
//   into this FIFO via QueueSetNotify().  xQueueSelectFromSet() performs a
//   blocking Get() from the same FIFO and returns the handle to the caller.
//
// FreeRTOS API contracts enforced here:
//   - Members must not already belong to another set (asserted).
//   - Members must be empty when removed from a set (asserted).
//   - Mutexes must not be added to queue sets (FreeRTOS API restriction).
//   - xEventQueueLength must be >= sum of all member capacities; the caller
//     is responsible for sizing correctly.  Overflow is silently dropped by
//     TryPut() in QueueSetNotify(), matching the FreeRTOS reference behaviour.
//
// Restrictions (matching FreeRTOS):
//   - xQueueOverwrite / xQueueOverwriteFromISR should not be used with queues
//     that are members of a set, as the overwrite generates a set notification
//     even when the old value is replaced rather than a new slot filled, which
//     can produce spurious wakeups.  This mirrors the documented FreeRTOS
//     caveat.
// ===========================================================================
#if configUSE_QUEUE_SETS

QueueSetHandle_t xQueueCreateSet(UBaseType_t uxEventQueueLength)
{
    if (IsIrqContext() || (uxEventQueueLength == 0U))
        return nullptr;

    if (uxEventQueueLength > stk::sync::MessageQueue::CAPACITY_MAX)
        return nullptr;

    FrtosQueueSet *qs = ObjAlloc<FrtosQueueSet>(uxEventQueueLength);

    if ((qs == nullptr) || !qs->IsValid())
    {
        ObjFree(qs);
        return nullptr;
    }

    return static_cast<QueueSetHandle_t>(qs);
}

BaseType_t xQueueAddToSet(QueueSetMemberHandle_t xQueueOrSemaphore,
                          QueueSetHandle_t       xQueueSet)
{
    if ((xQueueOrSemaphore == nullptr) || (xQueueSet == nullptr))
        return pdFAIL;

    FrtosQueueSet *qs = static_cast<FrtosQueueSet *>(xQueueSet);

    // Type discrimination between FrtosQueue and FrtosSemaphore via
    // GetSemKindFromHandle(): returns SemKind::None for queues (and any
    // unrecognised handle), SemKind::Counting or SemKind::Mutex for semaphores.
    {
        const SemKind kind = GetSemKindFromHandle(xQueueOrSemaphore);

        if (kind != SemKind::None)
        {
            // Semaphore or mutex path.
            FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xQueueOrSemaphore);

            // FreeRTOS API contract: mutexes cannot be queue set members.
            if (s->m_kind == SemKind::Mutex)
                return pdFAIL;

            STK_ASSERT(s->m_set == nullptr); // must not already belong to a set
            if (s->m_set != nullptr)
                return pdFAIL;

            STK_ASSERT(s->m_sem->GetCount() == 0U); // must be empty when added
            if (s->m_sem->GetCount() != 0U)
                return pdFAIL;

            s->m_set = qs;
        }
        else
        {
            // Queue path.
            FrtosQueue *q = static_cast<FrtosQueue *>(xQueueOrSemaphore);

            STK_ASSERT(q->m_set == nullptr); // must not already belong to a set
            if (q->m_set != nullptr)
                return pdFAIL;

            STK_ASSERT(q->m_mq.IsEmpty()); // must be empty when added
            if (!q->m_mq.IsEmpty())
                return pdFAIL;

            q->m_set = qs;
        }
    }

    return pdPASS;
}

BaseType_t xQueueRemoveFromSet(QueueSetMemberHandle_t xQueueOrSemaphore,
                               QueueSetHandle_t       xQueueSet)
{
    if ((xQueueOrSemaphore == nullptr) || (xQueueSet == nullptr))
        return pdFAIL;

    {
        const SemKind kind = GetSemKindFromHandle(xQueueOrSemaphore);

        if (kind != SemKind::None)
        {
            FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xQueueOrSemaphore);

            // API contract: member must belong to this specific set.
            STK_ASSERT(s->m_set == static_cast<FrtosQueueSet *>(xQueueSet));
            if (s->m_set != static_cast<FrtosQueueSet *>(xQueueSet))
                return pdFAIL;

            // API contract: semaphore must be empty when removed.
            STK_ASSERT(s->m_sem->GetCount() == 0U);
            if (s->m_sem->GetCount() != 0U)
                return pdFAIL;

            s->m_set = nullptr;
        }
        else
        {
            FrtosQueue *q = static_cast<FrtosQueue *>(xQueueOrSemaphore);

            STK_ASSERT(q->m_set == static_cast<FrtosQueueSet *>(xQueueSet));
            if (q->m_set != static_cast<FrtosQueueSet *>(xQueueSet))
                return pdFAIL;

            // API contract: queue must be empty when removed.
            STK_ASSERT(q->m_mq.IsEmpty());
            if (!q->m_mq.IsEmpty())
                return pdFAIL;

            q->m_set = nullptr;
        }
    }

    return pdPASS;
}

QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t xQueueSet,
                                           TickType_t       xTicksToWait)
{
    if (xQueueSet == nullptr)
        return nullptr;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return nullptr;

    FrtosQueueSet *qs = static_cast<FrtosQueueSet *>(xQueueSet);

    // Block until a member handle arrives in the token FIFO or timeout fires.
    void *handle = nullptr;
    if (!qs->m_token_mq->Get(&handle, FrtosTimeoutToStk(xTicksToWait)))
        return nullptr; // timeout

    return static_cast<QueueSetMemberHandle_t>(handle);
}

QueueSetMemberHandle_t xQueueSelectFromSetFromISR(QueueSetHandle_t xQueueSet)
{
    if (xQueueSet == nullptr)
        return nullptr;

    FrtosQueueSet *qs = static_cast<FrtosQueueSet *>(xQueueSet);

    // Non-blocking: TryGet() is ISR-safe per the STK MessageQueue contract.
    void *handle = nullptr;
    if (!qs->m_token_mq->TryGet(&handle))
        return nullptr; // set is empty

    return static_cast<QueueSetMemberHandle_t>(handle);
}

#endif // configUSE_QUEUE_SETS

// ===========================================================================
// Semaphore / Mutex API
// ===========================================================================

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    // Binary semaphore: max count = 1, initial count = 0.
    FrtosSemaphore *s = ObjAlloc<FrtosSemaphore>(SemKind::Counting,
                                                  uint16_t(0U),
                                                  uint16_t(1U));
    if ((s == nullptr) || (s->m_sem == nullptr))
    {
        ObjFree(s);
        return nullptr;
    }
    return static_cast<SemaphoreHandle_t>(s);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *pxSemaphoreBuffer)
{
    if (pxSemaphoreBuffer == nullptr)
        return nullptr;

    // Static assert guards against the buffer being too small.
    static_assert(sizeof(StaticSemaphore_t) >= sizeof(FrtosSemaphore),
        "StaticSemaphore_t is too small to hold FrtosSemaphore. "
        "Increase STATIC_SEMAPHORE_TCB_SIZE_WORDS in freertos_stk.h.");

    // Placement-new the FrtosSemaphore control block into the caller-supplied
    // buffer.  Binary semaphore: max count = 1, initial count = 0.
    // The inner stk::sync::Semaphore is a value type embedded inside
    // FrtosSemaphore, so no additional heap allocation is needed.
    FrtosSemaphore *s = new (pxSemaphoreBuffer) FrtosSemaphore(
        SemKind::Counting, uint16_t(0U), uint16_t(1U));

    if (s->m_sem == nullptr)
    {
        s->~FrtosSemaphore(); // clean up without freeing
        return nullptr;
    }

    s->m_cb_owned = false; // caller owns the memory; destructor must not delete

    return static_cast<SemaphoreHandle_t>(s);
}

#if configUSE_COUNTING_SEMAPHORES

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount)
{
    if (uxMaxCount == 0U || uxInitialCount > uxMaxCount)
        return nullptr;

    if (uxMaxCount > stk::sync::Semaphore::COUNT_MAX)
        uxMaxCount = stk::sync::Semaphore::COUNT_MAX;

    FrtosSemaphore *s = ObjAlloc<FrtosSemaphore>(
        SemKind::Counting,
        static_cast<uint16_t>(uxInitialCount),
        static_cast<uint16_t>(uxMaxCount));

    if ((s == nullptr) || (s->m_sem == nullptr))
    {
        ObjFree(s);
        return nullptr;
    }
    return static_cast<SemaphoreHandle_t>(s);
}

SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t        uxMaxCount,
                                                  UBaseType_t        uxInitialCount,
                                                  StaticSemaphore_t *pxSemaphoreBuffer)
{
    if (pxSemaphoreBuffer == nullptr)
        return nullptr;

    if (uxMaxCount == 0U || uxInitialCount > uxMaxCount)
        return nullptr;

    if (uxMaxCount > stk::sync::Semaphore::COUNT_MAX)
        uxMaxCount = stk::sync::Semaphore::COUNT_MAX;

    static_assert(sizeof(StaticSemaphore_t) >= sizeof(FrtosSemaphore),
        "StaticSemaphore_t is too small to hold FrtosSemaphore. "
        "Increase STATIC_SEMAPHORE_TCB_SIZE_WORDS in freertos_stk.h.");

    FrtosSemaphore *s = new (pxSemaphoreBuffer) FrtosSemaphore(
        SemKind::Counting,
        static_cast<uint16_t>(uxInitialCount),
        static_cast<uint16_t>(uxMaxCount));

    if (s->m_sem == nullptr)
    {
        s->~FrtosSemaphore();
        return nullptr;
    }

    s->m_cb_owned = false;
    return static_cast<SemaphoreHandle_t>(s);
}

#endif // configUSE_COUNTING_SEMAPHORES

#if configUSE_MUTEXES

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    FrtosSemaphore *s = ObjAlloc<FrtosSemaphore>(SemKind::Mutex,
                                                  uint16_t(0U),
                                                  uint16_t(1U));
    if ((s == nullptr) || (s->m_mtx == nullptr))
    {
        ObjFree(s);
        return nullptr;
    }

    return static_cast<SemaphoreHandle_t>(s);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *pxMutexBuffer)
{
    if (pxMutexBuffer == nullptr)
        return nullptr;

    static_assert(sizeof(StaticSemaphore_t) >= sizeof(FrtosSemaphore),
        "StaticSemaphore_t is too small to hold FrtosSemaphore. "
        "Increase STATIC_SEMAPHORE_TCB_SIZE_WORDS in freertos_stk.h.");

    FrtosSemaphore *s = new (pxMutexBuffer) FrtosSemaphore(
        SemKind::Mutex, uint16_t(0U), uint16_t(1U));

    if (s->m_mtx == nullptr)
    {
        s->~FrtosSemaphore();
        return nullptr;
    }

    s->m_cb_owned = false;
    return static_cast<SemaphoreHandle_t>(s);
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    // STK Mutex is always recursive.
    return xSemaphoreCreateMutex();
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *pxMutexBuffer)
{
    // STK Mutex is always recursive; identical to xSemaphoreCreateMutexStatic.
    return xSemaphoreCreateMutexStatic(pxMutexBuffer);
}

#endif // configUSE_MUTEXES

void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    if (xSemaphore == nullptr)
        return;

    ObjFree(static_cast<FrtosSemaphore *>(xSemaphore));
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
    if (xSemaphore == nullptr)
        return pdFAIL;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return pdFAIL;

    FrtosSemaphore *s   = static_cast<FrtosSemaphore *>(xSemaphore);
    stk::Timeout    tmo = FrtosTimeoutToStk(xTicksToWait);

    if (s->m_kind == SemKind::Mutex)
    {
#if configUSE_MUTEXES
        return s->m_mtx->TimedLock(tmo) ? pdPASS : pdFAIL;
#else
        return pdFAIL;
#endif
    }
    else
        return s->m_sem->Wait(tmo) ? pdPASS : pdFAIL;
}

BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore,
                                  BaseType_t       *pxHigherPriorityTaskWoken)
{
    if (xSemaphore == nullptr)
        return pdFAIL;

    FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xSemaphore);

    // Mutex take from ISR is not permitted — mirrors xSemaphoreGiveFromISR.
    if (s->m_kind == SemKind::Mutex)
        return pdFAIL;

    // TryWait() is Wait(NO_WAIT): decrement count if > 0, return immediately.
    // ISR-safe per the STK Semaphore contract.
    bool ok = s->m_sem->TryWait();

    // STK handles priority re-evaluation internally on the next tick.
    // Set pdFALSE to avoid spurious portYIELD_FROM_ISR, matching the
    // pattern used by xQueueReceiveFromISR and xSemaphoreGiveFromISR.
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return ok ? pdPASS : pdFAIL;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xMutex, TickType_t xTicksToWait)
{
    // STK Mutex is always recursive; identical to xSemaphoreTake.
    return xSemaphoreTake(xMutex, xTicksToWait);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    if (xSemaphore == nullptr)
        return pdFAIL;

    FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xSemaphore);

    if (s->m_kind == SemKind::Mutex)
    {
#if configUSE_MUTEXES
        s->m_mtx->Unlock();
        // Mutexes are not eligible for queue sets (FreeRTOS API contract),
        // so no QueueSetNotify call is needed here.
        return pdPASS;
#else
        return pdFAIL;
#endif
    }
    else
    {
        // Guard against overflow: TryWait + Signal pattern.
        if (s->m_sem->GetCount() >= stk::sync::Semaphore::COUNT_MAX)
            return pdFAIL;

        s->m_sem->Signal();
        QueueSetNotify(xSemaphore, s);
        return pdPASS;
    }
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xMutex)
{
    return xSemaphoreGive(xMutex);
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore,
                                 BaseType_t       *pxHigherPriorityTaskWoken)
{
    if (xSemaphore == nullptr)
        return pdFAIL;

    FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xSemaphore);

    if (s->m_kind == SemKind::Mutex)
        return pdFAIL; // Mutex give from ISR is not permitted.

    if (s->m_sem->GetCount() >= stk::sync::Semaphore::COUNT_MAX)
        return pdFAIL;

    s->m_sem->Signal();
    QueueSetNotify(xSemaphore, s);

    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return pdPASS;
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t xSemaphore)
{
    if (xSemaphore == nullptr)
        return 0U;

    FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xSemaphore);

    if (s->m_kind == SemKind::Counting)
        return static_cast<UBaseType_t>(s->m_sem->GetCount());

#if configUSE_MUTEXES
    // For a mutex, count = 0 means locked, 1 means unlocked.
    return (s->m_mtx->GetOwner() == stk::TID_NONE) ? 1U : 0U;
#else
    return 0U;
#endif
}

#if configUSE_MUTEXES

TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t xMutex)
{
    // Returns the task that currently owns the mutex, or NULL if the mutex
    // is unlocked or xMutex is not a mutex-kind semaphore.
    //
    // FreeRTOS documents this function as not ISR-safe and requiring the
    // scheduler to be running.  We guard it with a ScopedCriticalSection
    // so that the TId snapshot is consistent: if Unlock() is executing
    // concurrently, we see either the old owner or TID_NONE, never a torn
    // pointer.
    //
    // TId -> TaskHandle_t: STK stores task pointers as TId values via
    //   TId = static_cast<TId>(reinterpret_cast<uintptr_t>(task_ptr))
    // so the inverse is:
    //   task_ptr = reinterpret_cast<FrtosTask *>(static_cast<uintptr_t>(tid))
    // TID_NONE maps to nullptr (TaskHandle_t == nullptr means "no owner").
    if (xMutex == nullptr)
        return nullptr;

    FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xMutex);

    if (s->m_kind != SemKind::Mutex)
        return nullptr; // not a mutex — owner concept does not apply

    stk::sync::ScopedCriticalSection cs_;

    const stk::TId owner = s->m_mtx->GetOwner();

    if (owner == stk::TID_NONE)
        return nullptr; // mutex is currently unlocked

    return reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(owner));
}

TaskHandle_t xSemaphoreGetMutexHolderFromISR(SemaphoreHandle_t xMutex)
{
    // ISR-safe variant.  GetOwner() reads a single TId (pointer-sized, aligned)
    // which is an atomic read on all supported STK architectures, so no
    // ScopedCriticalSection is needed here beyond what the caller already holds.
    // We still validate the handle and the semaphore kind before touching the
    // mutex state.
    if (xMutex == nullptr)
        return nullptr;

    FrtosSemaphore *s = static_cast<FrtosSemaphore *>(xMutex);

    if (s->m_kind != SemKind::Mutex)
        return nullptr;

    const stk::TId owner = s->m_mtx->GetOwner();

    if (owner == stk::TID_NONE)
        return nullptr;

    return reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(owner));
}

#endif // configUSE_MUTEXES

// ===========================================================================
// Software Timer API  [configUSE_TIMERS]
// ===========================================================================

#if configUSE_TIMERS

TimerHandle_t xTimerCreate(const char             *pcTimerName,
                           TickType_t              xTimerPeriodInTicks,
                           UBaseType_t             uxAutoReload,
                           void                   *pvTimerID,
                           TimerCallbackFunction_t pxCallbackFunction)
{
    if (IsIrqContext() || (pxCallbackFunction == nullptr) || (xTimerPeriodInTicks == 0U))
        return nullptr;

    if (!FrtosTimer::EnsureTimerHost())
        return nullptr;

    FrtosTimer *t = ObjAlloc<FrtosTimer>(
        pcTimerName,
        xTimerPeriodInTicks,
        (uxAutoReload == pdTRUE),
        pvTimerID,
        pxCallbackFunction);

    return static_cast<TimerHandle_t>(t);
}

TimerHandle_t xTimerCreateStatic(const char             *pcTimerName,
                                  TickType_t              xTimerPeriodInTicks,
                                  UBaseType_t             uxAutoReload,
                                  void                   *pvTimerID,
                                  TimerCallbackFunction_t pxCallbackFunction,
                                  StaticTimer_t          *pxTimerBuffer)
{
    if (pxTimerBuffer == nullptr)
        return nullptr;

    if (IsIrqContext() || (pxCallbackFunction == nullptr) || (xTimerPeriodInTicks == 0U))
        return nullptr;

    if (!FrtosTimer::EnsureTimerHost())
        return nullptr;

    static_assert(sizeof(StaticTimer_t) >= sizeof(FrtosTimer),
        "StaticTimer_t is too small to hold FrtosTimer. "
        "Increase STATIC_TIMER_TCB_SIZE_WORDS in freertos_stk.h.");

    // Placement-new the FrtosTimer into the caller-supplied buffer.
    FrtosTimer *t = new (pxTimerBuffer) FrtosTimer(
        pcTimerName,
        xTimerPeriodInTicks,
        (uxAutoReload == pdTRUE),
        pvTimerID,
        pxCallbackFunction);

    t->m_cb_owned = false; // caller owns the memory; xTimerDelete must not delete it

    return static_cast<TimerHandle_t>(t);
}

BaseType_t xTimerDelete(TimerHandle_t xTimer, TickType_t /*xTicksToWait*/)
{
    if (xTimer == nullptr)
        return pdFAIL;

    FrtosTimer *t = static_cast<FrtosTimer *>(xTimer);

    if ((g_TimerHost != nullptr) && t->IsActive())
        g_TimerHost->Stop(*t);

    ObjFree(t);
    return pdPASS;
}

BaseType_t xTimerStart(TimerHandle_t xTimer, TickType_t /*xTicksToWait*/)
{
    if (IsIrqContext() || (xTimer == nullptr) || (g_TimerHost == nullptr))
        return pdFAIL;

    FrtosTimer *t = static_cast<FrtosTimer *>(xTimer);

    uint32_t period = t->m_auto_reload
                      ? static_cast<uint32_t>(t->m_period)
                      : 0U; // 0 = one-shot (no reload period)

    return g_TimerHost->Restart(*t, static_cast<uint32_t>(t->m_period), period)
           ? pdPASS : pdFAIL;
}

BaseType_t xTimerStop(TimerHandle_t xTimer, TickType_t /*xTicksToWait*/)
{
    if (IsIrqContext() || (xTimer == nullptr) || (g_TimerHost == nullptr))
        return pdFAIL;

    FrtosTimer *t = static_cast<FrtosTimer *>(xTimer);

    if (!t->IsActive())
        return pdFAIL;

    return g_TimerHost->Stop(*t) ? pdPASS : pdFAIL;
}

BaseType_t xTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait)
{
    return xTimerStart(xTimer, xTicksToWait); // Restart restarts from now
}

BaseType_t xTimerChangePeriod(TimerHandle_t xTimer,
                              TickType_t    xNewPeriod,
                              TickType_t    xTicksToWait)
{
    if ((xTimer == nullptr) || (xNewPeriod == 0U))
        return pdFAIL;

    FrtosTimer *t = static_cast<FrtosTimer *>(xTimer);
    t->m_period   = xNewPeriod;

    return xTimerStart(xTimer, xTicksToWait);
}

// ===========================================================================
// Timer ISR API
//
// TimerHost::PushCommand() calls m_commands.Write(cmd, NO_WAIT), which maps to
// PipeT<TimerCommand, N>::Write(..., NO_WAIT).  With NO_WAIT, Write() acquires
// a ScopedCriticalSection (interrupt-safe on all STK targets), checks for
// space, copies the command if available, and returns immediately without
// sleeping — making it unconditionally safe to call from an ISR.
//
// xTicksToWait is accepted for FreeRTOS API compatibility but always ignored.
// pxHigherPriorityTaskWoken is always set to pdFALSE: the tick task wakes
// itself when it drains the command queue, no manual yield is required.
//
// Each ISR function is a thin wrapper that:
//   1. Sets *pxHigherPriorityTaskWoken = pdFALSE.
//   2. Validates arguments (null handle, null host, zero period).
//   3. Delegates to the matching task-context function, which calls PushCommand.
//      The IsIrqContext() guard in xTimerStart/Stop is bypassed here because
//      ISR variants call PushCommand directly through g_TimerHost.
// ===========================================================================

BaseType_t xTimerStartFromISR(TimerHandle_t xTimer,
                               BaseType_t   *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if ((xTimer == nullptr) || (g_TimerHost == nullptr))
        return pdFAIL;

    FrtosTimer *t  = static_cast<FrtosTimer *>(xTimer);
    uint32_t period = t->m_auto_reload ? static_cast<uint32_t>(t->m_period) : 0U;

    return g_TimerHost->Restart(*t, static_cast<uint32_t>(t->m_period), period)
           ? pdPASS : pdFAIL;
}

BaseType_t xTimerStopFromISR(TimerHandle_t xTimer,
                              BaseType_t   *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if ((xTimer == nullptr) || (g_TimerHost == nullptr))
        return pdFAIL;

    FrtosTimer *t = static_cast<FrtosTimer *>(xTimer);

    if (!t->IsActive())
        return pdFAIL;

    return g_TimerHost->Stop(*t) ? pdPASS : pdFAIL;
}

BaseType_t xTimerResetFromISR(TimerHandle_t xTimer,
                               BaseType_t   *pxHigherPriorityTaskWoken)
{
    // Reset = Restart from now, same as xTimerStart.
    return xTimerStartFromISR(xTimer, pxHigherPriorityTaskWoken);
}

BaseType_t xTimerChangePeriodFromISR(TimerHandle_t xTimer,
                                      TickType_t    xNewPeriod,
                                      BaseType_t   *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if ((xTimer == nullptr) || (xNewPeriod == 0U) || (g_TimerHost == nullptr))
        return pdFAIL;

    FrtosTimer *t = static_cast<FrtosTimer *>(xTimer);
    t->m_period   = xNewPeriod;

    // Restart with the new period, taking effect immediately.
    uint32_t period = t->m_auto_reload ? static_cast<uint32_t>(xNewPeriod) : 0U;

    return g_TimerHost->Restart(*t, static_cast<uint32_t>(xNewPeriod), period) ? pdPASS : pdFAIL;
}

// ===========================================================================
// xTimerPendFunctionCall / xTimerPendFunctionCallFromISR
//
// Implementation strategy
// -----------------------
// Both functions write a PendCall value into g_PendCallPipe — a statically-
// allocated stk::sync::PipeT<PendCall, FREERTOS_STK_PEND_CALL_QUEUE_SIZE>.
// No heap allocation is performed.
//
// The singleton FrtosPendDrainer timer (stored in g_PendDrainerBuf) is
// (re-)started as a 1-tick auto-reload timer after every successful enqueue.
// On each tick it drains g_PendCallPipe to completion inside the TimerHost
// handler task — exactly where FreeRTOS's timer daemon would invoke the
// callbacks — then stops itself when the pipe is empty.
//
// Task-context variant (xTimerPendFunctionCall)
// ---------------------------------------------
//   Uses PipeT::Write() with the caller-supplied xTicksToWait timeout, so
//   the call can block if the pipe is momentarily full.  Returns pdFAIL only
//   if the timeout expires before a free slot becomes available.
//
// ISR-context variant (xTimerPendFunctionCallFromISR)
// ---------------------------------------------------
//   Uses PipeT::TryWrite() (= Write with NO_WAIT), which acquires a
//   ScopedCriticalSection internally and is unconditionally ISR-safe.
//   No allocator call is made; pvPortMalloc ISR-reentrancy is not required.
//   Returns pdFAIL immediately if the pipe is full.
//
// TimerHost initialization
// ------------------------
//   EnsureTimerHost() is called from the task-context variant for the same
//   reason as in xTimerCreate(): the API must work even if the application
//   has not created any explicit timers.  From ISR, the host must already be
//   running (g_TimerHost == nullptr -> pdFAIL), matching FreeRTOS behaviour.
// ===========================================================================

BaseType_t xTimerPendFunctionCall(PendedFunction_t xFunctionToPend,
                                   void            *pvParameter1,
                                   uint32_t         ulParameter2,
                                   TickType_t       xTicksToWait)
{
    // API contract: must not be called from ISR context.
    if (IsIrqContext())
        return pdFAIL;

    if (xFunctionToPend == nullptr)
        return pdFAIL;

    // Ensure the TimerHost (and hence the drainer's scheduling context) exists.
    if (!FrtosTimer::EnsureTimerHost())
        return pdFAIL;

    // Write the call record into the static pipe (blocking with timeout).
    // PipeT::Write() acquires a ScopedCriticalSection internally.
    const PendCall call = { xFunctionToPend, pvParameter1, ulParameter2 };
    if (!g_PendCallPipe.Write(call, FrtosTimeoutToStk(xTicksToWait)))
        return pdFAIL;

    // (Re-)start the singleton drainer so it wakes within 1 tick.
    // EnsurePendDrainer() constructs the drainer on first call and
    // calls TimerHost::Restart() which is task-safe.
    EnsurePendDrainer();

    return pdPASS;
}

BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t  xFunctionToPend,
                                          void             *pvParameter1,
                                          uint32_t          ulParameter2,
                                          BaseType_t       *pxHigherPriorityTaskWoken)
{
    // STK wakes the tick task internally; no manual context switch needed.
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if (xFunctionToPend == nullptr)
        return pdFAIL;

    // From ISR the TimerHost must already be running (a timer was created
    // before the ISR fired — the only realistic usage pattern).
    if (g_TimerHost == nullptr)
        return pdFAIL;

    // Non-blocking enqueue: TryWrite() holds a ScopedCriticalSection internally
    // — unconditionally ISR-safe, no allocator call, no blocking.
    const PendCall call = { xFunctionToPend, pvParameter1, ulParameter2 };
    if (!g_PendCallPipe.TryWrite(call))
        return pdFAIL;

    // Kick the drainer via ISR-safe TimerHost::Restart().
    KickPendDrainerFromISR();

    return pdPASS;
}

BaseType_t xTimerIsTimerActive(TimerHandle_t xTimer)
{
    if (xTimer == nullptr)
        return pdFALSE;

    return static_cast<FrtosTimer *>(xTimer)->IsActive() ? pdTRUE : pdFALSE;
}

void *pvTimerGetTimerID(TimerHandle_t xTimer)
{
    if (xTimer == nullptr)
        return nullptr;

    return static_cast<FrtosTimer *>(xTimer)->m_timer_id;
}

void vTimerSetTimerID(TimerHandle_t xTimer, void *pvNewID)
{
    if (xTimer == nullptr)
        return;

    static_cast<FrtosTimer *>(xTimer)->m_timer_id = pvNewID;
}

const char *pcTimerGetName(TimerHandle_t xTimer)
{
    if (xTimer == nullptr)
        return nullptr;

    return static_cast<FrtosTimer *>(xTimer)->m_name;
}

TickType_t xTimerGetPeriod(TimerHandle_t xTimer)
{
    if (xTimer == nullptr)
        return 0U;

    return static_cast<FrtosTimer *>(xTimer)->m_period;
}

TickType_t xTimerGetExpiryTime(TimerHandle_t xTimer)
{
    if ((xTimer == nullptr) || (g_TimerHost == nullptr))
        return 0U;

    FrtosTimer *t = static_cast<FrtosTimer *>(xTimer);
    return t->IsActive() ? static_cast<TickType_t>(t->GetDeadline()) : 0U;
}

#endif // configUSE_TIMERS

// ===========================================================================
// Event Group API  [configUSE_EVENT_GROUPS]
// ===========================================================================

#if configUSE_EVENT_GROUPS

// Translate STK EventFlags error -> FreeRTOS bits representation.
// FreeRTOS returns the bits value on success; on timeout it returns the
// bits that were set when the timeout occurred (we return 0 for simplicity
// as STK does not expose that snapshot).
static inline EventBits_t StkFlagsToFrtos(uint32_t result)
{
    if (stk::sync::EventFlags::IsError(result))
        return 0U; // timeout or error — caller checks via the return value

    return static_cast<EventBits_t>(result);
}

// Build STK EventFlags options from FreeRTOS xWaitForAllBits / xClearOnExit.
static inline uint32_t BuildStkFlagsOpts(BaseType_t xClearOnExit,
                                          BaseType_t xWaitForAllBits)
{
    uint32_t opts = stk::sync::EventFlags::OPT_WAIT_ANY;

    if (xWaitForAllBits == pdTRUE)
        opts |= stk::sync::EventFlags::OPT_WAIT_ALL;

    if (xClearOnExit == pdFALSE)
        opts |= stk::sync::EventFlags::OPT_NO_CLEAR;

    return opts;
}

EventGroupHandle_t xEventGroupCreate(void)
{
    if (IsIrqContext())
        return nullptr;

    FrtosEventGroup *eg = ObjAlloc<FrtosEventGroup>();
    return static_cast<EventGroupHandle_t>(eg);
}

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *pxEventGroupBuffer)
{
    if (pxEventGroupBuffer == nullptr)
        return nullptr;

    if (IsIrqContext())
        return nullptr;

    static_assert(sizeof(StaticEventGroup_t) >= sizeof(FrtosEventGroup),
        "StaticEventGroup_t is too small to hold FrtosEventGroup. "
        "Increase STATIC_EVENT_GROUP_TCB_SIZE_WORDS in freertos_stk.h.");

    FrtosEventGroup *eg = new (pxEventGroupBuffer) FrtosEventGroup();
    eg->m_cb_owned = false; // caller owns the memory; vEventGroupDelete must not delete it

    return static_cast<EventGroupHandle_t>(eg);
}

void vEventGroupDelete(EventGroupHandle_t xEventGroup)
{
    if (xEventGroup == nullptr)
        return;

    ObjFree(static_cast<FrtosEventGroup *>(xEventGroup));
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup,
                               EventBits_t        uxBitsToSet)
{
    if (xEventGroup == nullptr)
        return 0U;

    uint32_t result = static_cast<FrtosEventGroup *>(xEventGroup)->m_ef.Set(uxBitsToSet);
    return StkFlagsToFrtos(result);
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t xEventGroup,
                                 EventBits_t        uxBitsToClear)
{
    if (xEventGroup == nullptr)
        return 0U;

    // STK Clear() returns the value BEFORE clearing - matches FreeRTOS contract.
    uint32_t prev = static_cast<FrtosEventGroup *>(xEventGroup)->m_ef.Clear(uxBitsToClear);
    return StkFlagsToFrtos(prev);
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t xEventGroup)
{
    if (xEventGroup == nullptr)
        return 0U;

    return static_cast<EventBits_t>(static_cast<FrtosEventGroup *>(xEventGroup)->m_ef.Get());
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup,
                                EventBits_t        uxBitsToWaitFor,
                                BaseType_t         xClearOnExit,
                                BaseType_t         xWaitForAllBits,
                                TickType_t         xTicksToWait)
{
    if (IsIrqContext() || (xEventGroup == nullptr) || (uxBitsToWaitFor == 0U))
        return 0U;

    uint32_t opts   = BuildStkFlagsOpts(xClearOnExit, xWaitForAllBits);
    uint32_t result = static_cast<FrtosEventGroup *>(xEventGroup)->m_ef.Wait(
                          static_cast<uint32_t>(uxBitsToWaitFor),
                          opts,
                          FrtosTimeoutToStk(xTicksToWait));

    return StkFlagsToFrtos(result);
}

BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t xEventGroup,
                                     EventBits_t        uxBitsToSet,
                                     BaseType_t        *pxHigherPriorityTaskWoken)
{
    if (xEventGroup == nullptr)
        return pdFAIL;

    uint32_t result = static_cast<FrtosEventGroup *>(xEventGroup)->m_ef.Set(
                          static_cast<uint32_t>(uxBitsToSet));

    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return stk::sync::EventFlags::IsError(result) ? pdFAIL : pdPASS;
}

EventBits_t xEventGroupClearBitsFromISR(EventGroupHandle_t xEventGroup,
                                        EventBits_t        uxBitsToClear)
{
    if (xEventGroup == nullptr)
        return 0U;

    uint32_t prev = static_cast<FrtosEventGroup *>(xEventGroup)->m_ef.Clear(
                        static_cast<uint32_t>(uxBitsToClear));
    return StkFlagsToFrtos(prev);
}

// ---------------------------------------------------------------------------
// xEventGroupSync
//
// Design rationale
// ----------------
// FreeRTOS xEventGroupSync() is a multi-task rendezvous (barrier):
//
//   1. Atomically set this task's "I'm ready" bits.
//   2. Wait (with AND semantics) until ALL bits in uxBitsToWaitFor are set.
//   3. On success, atomically clear uxBitsToWaitFor and return the snapshot
//      taken just before the clear.
//
// The set-then-wait sequence must appear atomic from every other task's point
// of view, so that no task can wake from step 2 before every peer has
// completed step 1.
//
// STK provides this guarantee naturally:
//   - EventFlags::Set() acquires a ScopedCriticalSection, ORs the bits,
//     calls ConditionVariable::NotifyAll() (which reschedules waiters but
//     cannot actually run them until the CS is released), then exits the CS.
//   - EventFlags::Wait() enters its own ScopedCriticalSection and loops,
//     re-checking the predicate on every wakeup.
//
// Because both operations hold the same type of CS and the scheduler cannot
// pre-empt inside one, the sequence "set my bit -> enter wait loop" is
// indivisible:  a waiter woken by our Set() will observe our bits already
// present when it re-evaluates its own predicate.
//
// Auto-clear policy
// -----------------
// The first task to find the AND predicate satisfied will attempt to clear
// uxBitsToWaitFor.  Subsequent tasks that already passed the predicate (or
// are woken in the same tick) receive the snapshot value that was current
// when they sampled the flags (before any clear).  This matches FreeRTOS
// behaviour: every unblocked task sees the full set of sync bits in its
// return value, and the clear is performed by the internal Wait() path under
// the critical section, making it race-free across concurrent waiters.
//
// If OPT_WAIT_ALL + (default) clear-on-exit are passed together to
// EventFlags::Wait(), the STK implementation clears exactly the matched
// bits (== uxBitsToWaitFor) when the predicate is first satisfied by each
// waiter independently — which is safe because all tasks wait for the same
// superset mask, and STK's "m_flags &= ~matched" inside Wait() is idempotent
// once the bits are already 0.
// ---------------------------------------------------------------------------

EventBits_t xEventGroupSync(EventGroupHandle_t xEventGroup,
                            EventBits_t        uxBitsToSet,
                            EventBits_t        uxBitsToWaitFor,
                            TickType_t         xTicksToWait)
{
    // Contract checks that mirror the FreeRTOS reference implementation.
    if (xEventGroup    == nullptr)  return 0U;
    if (uxBitsToWaitFor == 0U)      return 0U;
    if (IsIrqContext())             return 0U;  // blocking wait is ISR-unsafe

    FrtosEventGroup *eg  = static_cast<FrtosEventGroup *>(xEventGroup);
    stk::sync::EventFlags &ef = eg->m_ef;

    // Step 1: Set this task's rendezvous bit(s).
    //
    // uxBitsToSet == 0 is legal (observer role: task waits without contributing
    // a bit).  Only call Set() when there is actually something to set.
    if (uxBitsToSet != 0U)
    {
        uint32_t set_result = ef.Set(static_cast<uint32_t>(uxBitsToSet));

        // Treat an ERROR_PARAMETER return as a hard usage fault — the caller
        // violated the API (e.g. set bit 31).
        STK_ASSERT(!stk::sync::EventFlags::IsError(set_result));

        if (stk::sync::EventFlags::IsError(set_result))
            return 0U;
    }

    // Step 2 + 3: Wait for ALL bits in uxBitsToWaitFor, clear them on success.
    //
    // OPT_WAIT_ALL   — AND semantics: all bits must be set simultaneously.
    // OPT_NO_CLEAR is NOT passed — matched bits are cleared atomically inside
    // Wait() when the predicate is first satisfied, exactly matching the
    // FreeRTOS "clear bits on exit" contract for xEventGroupSync().
    const uint32_t opts = stk::sync::EventFlags::OPT_WAIT_ALL;
                          // (OPT_NO_CLEAR absent -> clear on success)

    uint32_t result = ef.Wait(static_cast<uint32_t>(uxBitsToWaitFor),
                              opts,
                              FrtosTimeoutToStk(xTicksToWait));

    // On timeout or error, return 0 (matches FreeRTOS reference behaviour).
    return StkFlagsToFrtos(result);
}

#endif // configUSE_EVENT_GROUPS

// ===========================================================================
// Task Notification API — indexed implementation  [configUSE_TASK_NOTIFICATIONS]
//
// Each FrtosTask carries m_notify[configTASK_NOTIFICATION_ARRAY_ENTRIES], a
// fixed-size array of NotifySlot structs.  Every slot is fully independent:
//   - m_notify[i].value   : 32-bit notification word (eSetBits / eIncrement / etc.)
//   - m_notify[i].pending : set-without-overwrite guard
//   - m_notify[i].sem     : stk::sync::Semaphore (binary) used as the blocking
//                           primitive; the non-indexed API used this exact model
//                           for slot 0.
//
// Non-indexed functions (xTaskNotifyGive, ulTaskNotifyTake, xTaskNotify,
// xTaskNotifyWait, xTaskNotifyFromISR) are thin forwarding wrappers to slot 0.
//
// Out-of-range slot index: assertion in debug builds, early-return/pdFAIL in
// release builds — matching FreeRTOS configASSERT behaviour.
// ===========================================================================

#if configUSE_TASK_NOTIFICATIONS

// ---------------------------------------------------------------------------
// Internal helper: validate slot index and resolve a NULL handle to self.
// Returns the FrtosTask pointer on success, nullptr on failure.
// ---------------------------------------------------------------------------
static FrtosTask *ResolveNotifyTarget(TaskHandle_t xTask, UBaseType_t uxIndex)
{
    STK_ASSERT(uxIndex < (UBaseType_t)configTASK_NOTIFICATION_ARRAY_ENTRIES);

    if (uxIndex >= (UBaseType_t)configTASK_NOTIFICATION_ARRAY_ENTRIES)
        return nullptr;

    if (xTask == nullptr)
        xTask = xTaskGetCurrentTaskHandle();

    return static_cast<FrtosTask *>(xTask);
}

// ---------------------------------------------------------------------------
// Internal helper: apply a notification action to a slot under a held CS.
// Returns pdPASS / pdFAIL (mirrors the public xTaskNotify contract).
// ---------------------------------------------------------------------------
static BaseType_t NotifyApplyAction(FrtosTask::NotifySlot &slot,
                                    uint32_t               ulValue,
                                    eNotifyAction          eAction)
{
    switch (eAction)
    {
    case eNoAction:
        break;

    case eSetBits:
        slot.value |= ulValue;
        break;

    case eIncrement:
        slot.value++;
        break;

    case eSetValueWithOverwrite:
        slot.value = ulValue;
        break;

    case eSetValueWithoutOverwrite:
        if (slot.pending)
            return pdFAIL; // value not consumed yet
        slot.value   = ulValue;
        slot.pending = true;
        break;

    default:
        return pdFAIL;
    }

    return pdPASS;
}

// ===========================================================================
// Indexed API
// ===========================================================================

BaseType_t xTaskNotifyGiveIndexed(TaskHandle_t xTaskToNotify,
                                  UBaseType_t  uxIndexToNotify)
{
    FrtosTask *t = ResolveNotifyTarget(xTaskToNotify, uxIndexToNotify);
    if (t == nullptr)
        return pdFAIL;

    t->m_notify[uxIndexToNotify].sem.Signal(); // ISR-safe
    return pdPASS;
}

uint32_t ulTaskNotifyTakeIndexed(UBaseType_t uxIndexToWait,
                                 BaseType_t  ulClearCountOnExit,
                                 TickType_t  xTicksToWait)
{
    if (IsIrqContext())
        return 0U;

    FrtosTask *t = ResolveNotifyTarget(nullptr, uxIndexToWait);
    if (t == nullptr)
        return 0U;

    FrtosTask::NotifySlot &slot = t->m_notify[uxIndexToWait];

    if (!slot.sem.Wait(FrtosTimeoutToStk(xTicksToWait)))
        return 0U;

    uint32_t val = 0U;

    {
        stk::sync::ScopedCriticalSection cs_;
        val = slot.value;

        if (ulClearCountOnExit == pdTRUE)
            slot.value = 0U;
        else
        if (slot.value > 0U)
            slot.value--;
    }

    return val + 1U; // +1: the semaphore signal itself counts
}

BaseType_t xTaskNotifyIndexed(TaskHandle_t  xTaskToNotify,
                               UBaseType_t   uxIndexToNotify,
                               uint32_t      ulValue,
                               eNotifyAction eAction)
{
    FrtosTask *t = ResolveNotifyTarget(xTaskToNotify, uxIndexToNotify);
    if (t == nullptr)
        return pdFAIL;

    FrtosTask::NotifySlot &slot = t->m_notify[uxIndexToNotify];
    BaseType_t result = pdPASS;

    {
        stk::sync::ScopedCriticalSection cs_;
        result = NotifyApplyAction(slot, ulValue, eAction);
    }

    if (result == pdPASS)
        slot.sem.Signal();

    return result;
}

BaseType_t xTaskNotifyWaitIndexed(UBaseType_t  uxIndexToWait,
                                   uint32_t     ulBitsToClearOnEntry,
                                   uint32_t     ulBitsToClearOnExit,
                                   uint32_t    *pulNotificationValue,
                                   TickType_t   xTicksToWait)
{
    if (IsIrqContext())
        return pdFAIL;

    FrtosTask *t = ResolveNotifyTarget(nullptr, uxIndexToWait);
    if (t == nullptr)
        return pdFAIL;

    FrtosTask::NotifySlot &slot = t->m_notify[uxIndexToWait];

    // Clear entry bits before blocking.
    {
        stk::sync::ScopedCriticalSection cs_;
        slot.value &= ~ulBitsToClearOnEntry;
    }

    // Block until notified or timeout.
    if (!slot.sem.Wait(FrtosTimeoutToStk(xTicksToWait)))
        return pdFAIL;

    // Read and apply exit-clear.
    {
        stk::sync::ScopedCriticalSection cs_;

        if (pulNotificationValue != nullptr)
            *pulNotificationValue = slot.value;

        slot.value  &= ~ulBitsToClearOnExit;
        slot.pending = false;
    }

    return pdPASS;
}

BaseType_t xTaskNotifyFromISRIndexed(TaskHandle_t  xTaskToNotify,
                                      UBaseType_t   uxIndexToNotify,
                                      uint32_t      ulValue,
                                      eNotifyAction eAction,
                                      BaseType_t   *pxHigherPriorityTaskWoken)
{
    BaseType_t result = xTaskNotifyIndexed(xTaskToNotify, uxIndexToNotify, ulValue, eAction);

    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    return result;
}

// ===========================================================================
// Non-indexed API — thin wrappers around slot 0
// ===========================================================================

BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify)
{
    return xTaskNotifyGiveIndexed(xTaskToNotify, 0U);
}

uint32_t ulTaskNotifyTake(BaseType_t ulClearCountOnExit, TickType_t xTicksToWait)
{
    return ulTaskNotifyTakeIndexed(0U, ulClearCountOnExit, xTicksToWait);
}

BaseType_t xTaskNotify(TaskHandle_t  xTaskToNotify,
                       uint32_t      ulValue,
                       eNotifyAction eAction)
{
    return xTaskNotifyIndexed(xTaskToNotify, 0U, ulValue, eAction);
}

BaseType_t xTaskNotifyWait(uint32_t   ulBitsToClearOnEntry,
                           uint32_t   ulBitsToClearOnExit,
                           uint32_t  *pulNotificationValue,
                           TickType_t xTicksToWait)
{
    return xTaskNotifyWaitIndexed(0U, ulBitsToClearOnEntry, ulBitsToClearOnExit,
                                  pulNotificationValue, xTicksToWait);
}

BaseType_t xTaskNotifyFromISR(TaskHandle_t  xTaskToNotify,
                              uint32_t      ulValue,
                              eNotifyAction eAction,
                              BaseType_t   *pxHigherPriorityTaskWoken)
{
    return xTaskNotifyFromISRIndexed(xTaskToNotify, 0U, ulValue, eAction,
                                     pxHigherPriorityTaskWoken);
}

// ===========================================================================
// Task Notification — AndQuery / StateClear / ValueClear extensions
//
// xTaskNotifyAndQuery[Indexed]
//   Like xTaskNotify[Indexed] but additionally snapshots the slot value
//   *before* the action is applied and writes it to *pulPreviousNotifyValue.
//   This gives the caller an atomic read-modify-notify without a separate
//   critical section in application code.
//
// xTaskNotifyAndQueryFromISR[Indexed]
//   ISR-safe wrappers around the Indexed variant; pxHigherPriorityTaskWoken
//   is always set to pdFALSE (STK handles the context switch internally).
//
// xTaskNotifyStateClear[Indexed]
//   Clears the pending flag of the selected slot and drains the binary
//   semaphore (one TryWait() is sufficient because max_count = 1).
//   Returns pdTRUE when a notification was pending, pdFALSE otherwise.
//   The notification value word is NOT modified — only the pending state.
//
// ulTaskNotifyValueClear[Indexed]
//   Atomically clears the specified bits in slot.value (under a critical
//   section) and returns the value *before* the clear.  The pending flag
//   and semaphore are not touched.
// ===========================================================================

// ---------------------------------------------------------------------------
// xTaskNotifyAndQueryIndexed
// ---------------------------------------------------------------------------

BaseType_t xTaskNotifyAndQueryIndexed(TaskHandle_t  xTaskToNotify,
                                      UBaseType_t   uxIndexToNotify,
                                      uint32_t      ulValue,
                                      eNotifyAction eAction,
                                      uint32_t     *pulPreviousNotifyValue)
{
    FrtosTask *t = ResolveNotifyTarget(xTaskToNotify, uxIndexToNotify);
    if (t == nullptr)
        return pdFAIL;

    FrtosTask::NotifySlot &slot = t->m_notify[uxIndexToNotify];
    BaseType_t result = pdPASS;

    {
        stk::sync::ScopedCriticalSection cs_;

        // Snapshot the value *before* the action — this is the only difference
        // from xTaskNotifyIndexed.
        if (pulPreviousNotifyValue != nullptr)
            *pulPreviousNotifyValue = slot.value;

        result = NotifyApplyAction(slot, ulValue, eAction);
    }

    if (result == pdPASS)
        slot.sem.Signal();

    return result;
}

// ---------------------------------------------------------------------------
// xTaskNotifyAndQuery (slot 0 wrapper)
// ---------------------------------------------------------------------------

BaseType_t xTaskNotifyAndQuery(TaskHandle_t  xTaskToNotify,
                               uint32_t      ulValue,
                               eNotifyAction eAction,
                               uint32_t     *pulPreviousNotifyValue)
{
    return xTaskNotifyAndQueryIndexed(xTaskToNotify, 0U, ulValue, eAction,
                                      pulPreviousNotifyValue);
}

// ---------------------------------------------------------------------------
// xTaskNotifyAndQueryFromISRIndexed
// ---------------------------------------------------------------------------

BaseType_t xTaskNotifyAndQueryFromISRIndexed(TaskHandle_t  xTaskToNotify,
                                             UBaseType_t   uxIndexToNotify,
                                             uint32_t      ulValue,
                                             eNotifyAction eAction,
                                             uint32_t     *pulPreviousNotifyValue,
                                             BaseType_t   *pxHigherPriorityTaskWoken)
{
    BaseType_t result = xTaskNotifyAndQueryIndexed(xTaskToNotify, uxIndexToNotify,
                                                   ulValue, eAction,
                                                   pulPreviousNotifyValue);

    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE; // STK handles the context switch internally

    return result;
}

// ---------------------------------------------------------------------------
// xTaskNotifyAndQueryFromISR (slot 0 wrapper)
// ---------------------------------------------------------------------------

BaseType_t xTaskNotifyAndQueryFromISR(TaskHandle_t  xTaskToNotify,
                                      uint32_t      ulValue,
                                      eNotifyAction eAction,
                                      uint32_t     *pulPreviousNotifyValue,
                                      BaseType_t   *pxHigherPriorityTaskWoken)
{
    return xTaskNotifyAndQueryFromISRIndexed(xTaskToNotify, 0U, ulValue, eAction,
                                             pulPreviousNotifyValue,
                                             pxHigherPriorityTaskWoken);
}

// ---------------------------------------------------------------------------
// xTaskNotifyStateClearIndexed
//
// Clears the pending notification state for the specified slot.
//
// FreeRTOS contract:
//   - Returns pdTRUE  if a notification was pending (slot was "notified").
//   - Returns pdFALSE if no notification was pending.
//   - The 32-bit value word is NOT modified; only the pending/signaled state.
//
// STK mapping:
//   The "pending" state is represented by two cooperating members:
//     slot.pending  — set by eSetValueWithoutOverwrite; guards value overwrite.
//     slot.sem      — binary semaphore; its count > 0 means a notification
//                     arrived and has not yet been consumed by Wait/Take.
//   Both are cleared here atomically under a critical section so that a
//   concurrent xTaskNotifyWait or ulTaskNotifyTake cannot race with the clear.
//
//   The semaphore is binary (max_count = 1), so one TryWait() is sufficient
//   to drain it.  We must not call sem.Wait() (blocking) from either task or
//   ISR context — TryWait() (NO_WAIT, ISR-safe) is the correct choice here.
// ---------------------------------------------------------------------------

BaseType_t xTaskNotifyStateClearIndexed(TaskHandle_t xTask,
                                        UBaseType_t  uxIndexToClear)
{
    FrtosTask *t = ResolveNotifyTarget(xTask, uxIndexToClear);
    if (t == nullptr)
        return pdFALSE;

    FrtosTask::NotifySlot &slot = t->m_notify[uxIndexToClear];

    stk::sync::ScopedCriticalSection cs_;

    // Determine whether a notification was pending before we clear anything.
    // A notification is considered "pending" when the semaphore has a count
    // (i.e. Signal() was called but Wait/Take has not yet consumed it) OR
    // when the value-without-overwrite guard flag is set.
    const bool was_pending = (slot.sem.GetCount() != 0U) || slot.pending;

    if (was_pending)
    {
        // Drain the semaphore (binary, at most 1 token to consume).
        slot.sem.TryWait();

        // Clear the eSetValueWithoutOverwrite pending guard so that a
        // subsequent xTaskNotify(eSetValueWithoutOverwrite) can write a new
        // value without being rejected.
        slot.pending = false;
    }

    return was_pending ? pdTRUE : pdFALSE;
}

// ---------------------------------------------------------------------------
// xTaskNotifyStateClear (slot 0 wrapper)
// ---------------------------------------------------------------------------

BaseType_t xTaskNotifyStateClear(TaskHandle_t xTask)
{
    return xTaskNotifyStateClearIndexed(xTask, 0U);
}

// ---------------------------------------------------------------------------
// ulTaskNotifyValueClearIndexed
//
// Atomically clears the specified bits in the slot's 32-bit value word and
// returns the value *before* the clear.
//
// FreeRTOS contract:
//   - Performs: old = slot.value; slot.value &= ~ulBitsToClear; return old;
//   - The pending flag and semaphore are NOT touched.
//   - Pass ulBitsToClear = 0xFFFFFFFF to clear all bits.
// ---------------------------------------------------------------------------

uint32_t ulTaskNotifyValueClearIndexed(TaskHandle_t xTask,
                                       UBaseType_t  uxIndexToClear,
                                       uint32_t     ulBitsToClear)
{
    FrtosTask *t = ResolveNotifyTarget(xTask, uxIndexToClear);
    if (t == nullptr)
        return 0U;

    FrtosTask::NotifySlot &slot = t->m_notify[uxIndexToClear];

    stk::sync::ScopedCriticalSection cs_;

    const uint32_t prev = slot.value;
    slot.value &= ~ulBitsToClear;

    return prev;
}

// ---------------------------------------------------------------------------
// ulTaskNotifyValueClear (slot 0 wrapper)
// ---------------------------------------------------------------------------

uint32_t ulTaskNotifyValueClear(TaskHandle_t xTask,
                                uint32_t     ulBitsToClear)
{
    return ulTaskNotifyValueClearIndexed(xTask, 0U, ulBitsToClear);
}

#endif // configUSE_TASK_NOTIFICATIONS

// ===========================================================================
// Thread-local storage (TLS) API
//
// Each FrtosTask carries a fixed-size m_tls[] array of void* slots
// (configNUM_THREAD_LOCAL_STORAGE_POINTERS entries, zero-initialised).
// The task to operate on is resolved via its TaskHandle_t, which is the
// FrtosTask* cast to void*.  NULL resolves to the calling task via
// xTaskGetCurrentTaskHandle().
//
// STK TLS note:
//   stk::hw::GetTlsPtr / SetTlsPtr manage the kernel's own per-task context
//   pointer (used internally by the scheduler). Application TLS lives in
//   the m_tls[] member of FrtosTask and does NOT touch the STK TP register,
//   preserving the kernel's internal invariants.
// ===========================================================================

void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet,
                                       BaseType_t   xIndex,
                                       void        *pvValue)
{
    if (xIndex < 0 || static_cast<size_t>(xIndex) >= configNUM_THREAD_LOCAL_STORAGE_POINTERS)
        return;

    // Resolve NULL -> calling task.
    if (xTaskToSet == nullptr)
        xTaskToSet = xTaskGetCurrentTaskHandle();

    if (xTaskToSet == nullptr)
        return;

    FrtosTask *t = static_cast<FrtosTask *>(xTaskToSet);

    // A critical section is not strictly required for a pointer-sized store on
    // aligned memory on single-issue cores, but we guard anyway for strict
    // correctness on SMP targets (RP2040, dual-core Cortex-M33).
    stk::sync::ScopedCriticalSection cs_;
    t->m_tls[static_cast<size_t>(xIndex)] = pvValue;
}

void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery,
                                          BaseType_t   xIndex)
{
    if (xIndex < 0 || static_cast<size_t>(xIndex) >= configNUM_THREAD_LOCAL_STORAGE_POINTERS)
        return nullptr;

    // Resolve NULL -> calling task.
    if (xTaskToQuery == nullptr)
        xTaskToQuery = xTaskGetCurrentTaskHandle();

    if (xTaskToQuery == nullptr)
        return nullptr;

    const FrtosTask *t = static_cast<const FrtosTask *>(xTaskToQuery);

    stk::sync::ScopedCriticalSection cs_;
    return t->m_tls[static_cast<size_t>(xIndex)];
}

// ===========================================================================
// Stream Buffer API  [configUSE_STREAM_BUFFERS]
//
// FrtosStreamBuffer wraps stk::sync::Pipe (element_size = 1), using it as a
// byte ring-buffer.  sync::Pipe is preferred over sync::MessageQueue here
// because it provides WriteBulk / ReadBulk / TryWriteBulk / TryReadBulk
// natively, enabling efficient multi-byte transfers and clean trigger-level
// blocking without any single-byte Get() workarounds.
//
// Send    -> Pipe::WriteBulk()  (blocking) / TryWriteBulk() (ISR, NO_WAIT)
// Receive -> Pipe::ReadBulk()   with trigger-level gating (see below)
//            Pipe::TryReadBulk() (ISR, NO_WAIT)
//
// Trigger level (xStreamBufferReceive only):
//   When m_trigger > 1 and the caller requested a blocking wait, Receive()
//   calls ReadBulk(dst, m_trigger, timeout) to block atomically until at
//   least m_trigger bytes are consumed, then TryReadBulk() drains the rest
//   of the caller's buffer non-blocking.  This reuses Pipe's own CV wait
//   loop — no busy-spin, no lost-wakeup race, and no single-byte iteration.
// ===========================================================================

#if configUSE_STREAM_BUFFERS

static_assert(sizeof(StaticStreamBuffer_t) >= sizeof(FrtosStreamBuffer),
    "Increase STATIC_STREAM_BUFFER_TCB_SIZE_WORDS in FreeRTOS.h.");

StreamBufferHandle_t xStreamBufferCreate(size_t xBufferSizeBytes,
                                          size_t xTriggerLevelBytes)
{
    if (xBufferSizeBytes == 0U)
        return nullptr;

    uint8_t *buf = ObjAllocArray<uint8_t>(xBufferSizeBytes);
    if (buf == nullptr)
        return nullptr;

    FrtosStreamBuffer *sb = ObjAlloc<FrtosStreamBuffer>(buf, xBufferSizeBytes, xTriggerLevelBytes);
    if (sb == nullptr)
    {
        ObjFreeArray(buf);
        return nullptr;
    }

    sb->m_buf_owned = true;
    sb->m_cb_owned  = true;

    return static_cast<StreamBufferHandle_t>(sb);
}

StreamBufferHandle_t xStreamBufferCreateStatic(
    size_t                xBufferSizeBytes,
    size_t                xTriggerLevelBytes,
    uint8_t              *pucStreamBufferStorageArea,
    StaticStreamBuffer_t *pxStaticStreamBuffer)
{
    if ((pucStreamBufferStorageArea == nullptr) ||
        (pxStaticStreamBuffer      == nullptr) ||
        (xBufferSizeBytes == 0U))
        return nullptr;

    static_assert(sizeof(StaticStreamBuffer_t) >= sizeof(FrtosStreamBuffer),
        "Increase STATIC_STREAM_BUFFER_TCB_SIZE_WORDS in FreeRTOS.h.");

    FrtosStreamBuffer *sb = new (pxStaticStreamBuffer)
        FrtosStreamBuffer(pucStreamBufferStorageArea,
                          xBufferSizeBytes,
                          xTriggerLevelBytes);

    // m_buf_owned = false, m_cb_owned = false already set by the ctor
    return static_cast<StreamBufferHandle_t>(sb);
}

void vStreamBufferDelete(StreamBufferHandle_t xStreamBuffer)
{
    if (xStreamBuffer == nullptr)
        return;

    FrtosStreamBuffer *sb = static_cast<FrtosStreamBuffer *>(xStreamBuffer);

    ObjFree(sb);
}

size_t xStreamBufferSend(StreamBufferHandle_t xStreamBuffer,
                          const void          *pvTxData,
                          size_t               xDataLengthBytes,
                          TickType_t           xTicksToWait)
{
    if ((xStreamBuffer == nullptr) || (pvTxData == nullptr) || (xDataLengthBytes == 0U))
        return 0U;

    FrtosStreamBuffer *sb = static_cast<FrtosStreamBuffer *>(xStreamBuffer);

    const size_t sent = sb->m_pipe.WriteBulk(
        static_cast<const uint8_t *>(pvTxData),
        xDataLengthBytes,
        FrtosTimeoutToStk(xTicksToWait));

    // Fire send-complete callback outside any critical section.
    if ((sent > 0U) && (sb->m_send_cb != nullptr))
    {
        BaseType_t woken = pdFALSE;
        sb->m_send_cb(xStreamBuffer, &woken);
    }

    return sent;
}

size_t xStreamBufferSendFromISR(StreamBufferHandle_t  xStreamBuffer,
                                 const void           *pvTxData,
                                 size_t                xDataLengthBytes,
                                 BaseType_t           *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if ((xStreamBuffer == nullptr) || (pvTxData == nullptr) || (xDataLengthBytes == 0U))
        return 0U;

    FrtosStreamBuffer *sb = static_cast<FrtosStreamBuffer *>(xStreamBuffer);

    const size_t sent = sb->m_pipe.TryWriteBulk(
        static_cast<const uint8_t *>(pvTxData),
        xDataLengthBytes);

    // Fire send-complete callback outside any critical section.
    // pxHigherPriorityTaskWoken is always pdFALSE per STK convention.
    if ((sent > 0U) && (sb->m_send_cb != nullptr))
    {
        BaseType_t woken = pdFALSE;
        sb->m_send_cb(xStreamBuffer, &woken);
    }

    return sent;
}

size_t xStreamBufferReceive(StreamBufferHandle_t xStreamBuffer,
                             void                *pvRxData,
                             size_t               xBufferLengthBytes,
                             TickType_t           xTicksToWait)
{
    if ((xStreamBuffer == nullptr) || (pvRxData == nullptr) || (xBufferLengthBytes == 0U))
        return 0U;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return 0U;

    FrtosStreamBuffer *sb = static_cast<FrtosStreamBuffer *>(xStreamBuffer);

    // ReadBulkTriggered handles all cases in one call:
    //   - NO_WAIT (xTicksToWait == 0): trigger not enforced, returns whatever
    //     is available immediately (guaranteed by CV NO_WAIT fast-path).
    //   - Blocking with trigger: waits until m_trigger bytes are present, then
    //     drains up to xBufferLengthBytes in one atomic CS pass.
    //   - Timeout before trigger: drains whatever arrived, possibly 0.
    const size_t total = sb->m_pipe.ReadBulkTriggered(
        static_cast<uint8_t *>(pvRxData),
        sb->m_trigger,
        xBufferLengthBytes,
        FrtosTimeoutToStk(xTicksToWait));

    // Fire receive-complete callback outside any critical section.
    if ((total > 0U) && (sb->m_recv_cb != nullptr))
    {
        BaseType_t woken = pdFALSE;
        sb->m_recv_cb(xStreamBuffer, &woken);
    }

    return total;
}

size_t xStreamBufferReceiveFromISR(StreamBufferHandle_t  xStreamBuffer,
                                    void                 *pvRxData,
                                    size_t                xBufferLengthBytes,
                                    BaseType_t           *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if ((xStreamBuffer == nullptr) || (pvRxData == nullptr) || (xBufferLengthBytes == 0U))
        return 0U;

    FrtosStreamBuffer *sb = static_cast<FrtosStreamBuffer *>(xStreamBuffer);

    const size_t received = sb->m_pipe.TryReadBulk(
        static_cast<uint8_t *>(pvRxData),
        xBufferLengthBytes);

    // Fire receive-complete callback outside any critical section.
    // pxHigherPriorityTaskWoken is always pdFALSE per STK convention.
    if ((received > 0U) && (sb->m_recv_cb != nullptr))
    {
        BaseType_t woken = pdFALSE;
        sb->m_recv_cb(xStreamBuffer, &woken);
    }

    return received;
}

size_t xStreamBufferBytesAvailable(StreamBufferHandle_t xStreamBuffer)
{
    if (xStreamBuffer == nullptr)
        return 0U;

    return static_cast<FrtosStreamBuffer *>(xStreamBuffer)->m_pipe.GetCount();
}

size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t xStreamBuffer)
{
    if (xStreamBuffer == nullptr)
        return 0U;

    return static_cast<FrtosStreamBuffer *>(xStreamBuffer)->m_pipe.GetSpace();
}

BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t xStreamBuffer)
{
    return (xStreamBufferBytesAvailable(xStreamBuffer) == 0U) ? pdTRUE : pdFALSE;
}

BaseType_t xStreamBufferIsFull(StreamBufferHandle_t xStreamBuffer)
{
    return (xStreamBufferSpacesAvailable(xStreamBuffer) == 0U) ? pdTRUE : pdFALSE;
}

BaseType_t xStreamBufferReset(StreamBufferHandle_t xStreamBuffer)
{
    if (xStreamBuffer == nullptr)
        return pdFAIL;

    static_cast<FrtosStreamBuffer *>(xStreamBuffer)->m_pipe.Reset();
    return pdPASS;
}

BaseType_t xStreamBufferSetTriggerLevel(StreamBufferHandle_t xStreamBuffer,
                                         size_t               xTriggerLevelBytes)
{
    if (xStreamBuffer == nullptr)
        return pdFALSE;

    FrtosStreamBuffer *sb = static_cast<FrtosStreamBuffer *>(xStreamBuffer);

    if (xTriggerLevelBytes > sb->m_pipe.GetCapacity())
        return pdFALSE;

    stk::sync::ScopedCriticalSection cs_;
    sb->m_trigger = (xTriggerLevelBytes >= 1U ? xTriggerLevelBytes : 1U);

    return pdTRUE;
}

BaseType_t xStreamBufferResetFromISR(StreamBufferHandle_t  xStreamBuffer,
                                      BaseType_t           *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if (xStreamBuffer == nullptr)
        return pdFAIL;

    // Pipe::Reset() acquires ScopedCriticalSection internally — ISR-safe.
    static_cast<FrtosStreamBuffer *>(xStreamBuffer)->m_pipe.Reset();
    return pdPASS;
}

size_t xStreamBufferGetTriggerLevel(StreamBufferHandle_t xStreamBuffer)
{
    if (xStreamBuffer == nullptr)
        return 0U;

    // m_trigger is a plain size_t written only under ScopedCriticalSection
    // (in xStreamBufferSetTriggerLevel).  A size_t-aligned read is atomic on
    // all Cortex-M targets — same rationale as Pipe::GetCount().
    return static_cast<const FrtosStreamBuffer *>(xStreamBuffer)->m_trigger;
}

// ---------------------------------------------------------------------------
// xStreamBufferNextMessageLengthBytes
// ---------------------------------------------------------------------------
// A stream buffer carries an unframed byte sequence with no length-prefix
// headers, so there is no concept of a discrete "next message" boundary.
// The FreeRTOS reference implementation therefore defines
// xStreamBufferNextMessageLengthBytes() for stream buffers as identical to
// xStreamBufferBytesAvailable(): it returns the total number of bytes that
// could be consumed by the next xStreamBufferReceive() call without blocking.
//
// The function exists primarily so that code written against the combined
// stream/message buffer API can call the same introspection function on both
// handle types without a type-discriminating branch.
// ---------------------------------------------------------------------------

size_t xStreamBufferNextMessageLengthBytes(StreamBufferHandle_t xStreamBuffer)
{
    // Delegates to xStreamBufferBytesAvailable() which performs the NULL guard
    // and returns Pipe::GetCount() — ISR-safe on all supported targets.
    return xStreamBufferBytesAvailable(xStreamBuffer);
}

// ---------------------------------------------------------------------------
// xStreamBufferCreateWithCallback / xStreamBufferCreateStaticWithCallback
//
// Identical to xStreamBufferCreate / xStreamBufferCreateStatic but store two
// optional per-instance notification callbacks (m_send_cb, m_recv_cb) in
// FrtosStreamBuffer.  Callbacks fire at the end of every successful Send /
// Receive (including ISR variants), outside any critical section.
// ---------------------------------------------------------------------------

StreamBufferHandle_t xStreamBufferCreateWithCallback(
    size_t                         xBufferSizeBytes,
    size_t                         xTriggerLevelBytes,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback)
{
    if (xBufferSizeBytes == 0U)
        return nullptr;

    uint8_t *buf = ObjAllocArray<uint8_t>(xBufferSizeBytes);
    if (buf == nullptr)
        return nullptr;

    FrtosStreamBuffer *sb = ObjAlloc<FrtosStreamBuffer>(
        buf,
        xBufferSizeBytes,
        xTriggerLevelBytes,
        pxSendCompletedCallback,
        pxReceiveCompletedCallback);

    if (sb == nullptr)
    {
        ObjFreeArray(buf);
        return nullptr;
    }

    sb->m_buf_owned = true;
    sb->m_cb_owned  = true;

    return static_cast<StreamBufferHandle_t>(sb);
}

StreamBufferHandle_t xStreamBufferCreateStaticWithCallback(
    size_t                         xBufferSizeBytes,
    size_t                         xTriggerLevelBytes,
    uint8_t                       *pucStreamBufferStorageArea,
    StaticStreamBuffer_t          *pxStaticStreamBuffer,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback)
{
    if ((pucStreamBufferStorageArea == nullptr) ||
        (pxStaticStreamBuffer      == nullptr) ||
        (xBufferSizeBytes == 0U))
        return nullptr;

    static_assert(sizeof(StaticStreamBuffer_t) >= sizeof(FrtosStreamBuffer),
        "Increase STATIC_STREAM_BUFFER_TCB_SIZE_WORDS in FreeRTOS.h.");

    FrtosStreamBuffer *sb = new (pxStaticStreamBuffer)
        FrtosStreamBuffer(pucStreamBufferStorageArea,
                          xBufferSizeBytes,
                          xTriggerLevelBytes,
                          pxSendCompletedCallback,
                          pxReceiveCompletedCallback);
    // m_buf_owned = false, m_cb_owned = false already set by ctor
    return static_cast<StreamBufferHandle_t>(sb);
}

// ===========================================================================
// Message Buffer API
//
// FrtosMessageBuffer uses two STK primitives:
//
//   m_pool (BlockMemoryPool) — payload blocks of AlignBlockSize(max_msg_size).
//   m_eq   (MessageQueue)    — envelope FIFO: each slot holds a MsgEnvelope
//                              {size_t len; void* blk}.
//
// Send:
//   1. TimedAlloc() a block from m_pool  (blocks if pool is empty).
//   2. memcpy payload into the block.
//   3. Put({len, blk}) envelope into m_eq (blocks if eq is full — which can
//      only happen if TimedAlloc succeeded, i.e. m_pool and m_eq are always
//      in sync: one envelope slot per pool block).
//
// Receive:
//   1. Get() envelope from m_eq (blocks if empty).
//   2. memcpy payload out of the block.
//   3. Free() the block back to m_pool — wakes one blocked sender if any.
//
// Reset:
//   Drain all envelopes, free their blocks, then Reset() the eq.
// ===========================================================================

static_assert(sizeof(StaticMessageBuffer_t) >= sizeof(FrtosMessageBuffer),
    "Increase STATIC_MESSAGE_BUFFER_TCB_SIZE_WORDS in FreeRTOS.h.");

// Derive slot count from a flat byte budget.
static size_t MsgBufSlotCount(size_t budget_bytes, size_t max_msg_size)
{
    const size_t block_size = stk::memory::BlockMemoryPool::AlignBlockSize(max_msg_size);
    const size_t slot_cost  = block_size + FrtosMessageBuffer::ENVELOPE_SIZE;

    if (slot_cost == 0U)
        return 0U;

    return budget_bytes / slot_cost;
}

MessageBufferHandle_t xMessageBufferCreate(size_t xBufferSizeBytes,
                                            size_t xMaxMessageSize)
{
    if ((xBufferSizeBytes == 0U) || (xMaxMessageSize == 0U))
        return nullptr;

    const size_t count = MsgBufSlotCount(xBufferSizeBytes, xMaxMessageSize);

    if (count == 0U)
        return nullptr;

    FrtosMessageBuffer *mb = ObjAlloc<FrtosMessageBuffer>(xMaxMessageSize, count);

    if (mb == nullptr)
        return nullptr;

    if (!mb->m_pool.IsStorageValid() || !mb->m_eq.IsStorageValid())
    {
        ObjFreeRaw(mb);
        return nullptr;
    }

    return static_cast<MessageBufferHandle_t>(mb);
}

MessageBufferHandle_t xMessageBufferCreateStatic(
    size_t                 xMaxMessageSize,
    size_t                 xMessageCount,
    uint8_t               *pucMessageBufferStorageArea,
    StaticMessageBuffer_t *pxStaticMessageBuffer)
{
    if ((pucMessageBufferStorageArea == nullptr) ||
        (pxStaticMessageBuffer       == nullptr) ||
        (xMaxMessageSize  == 0U) ||
        (xMessageCount    == 0U))
        return nullptr;

    const size_t block_size    = stk::memory::BlockMemoryPool::AlignBlockSize(xMaxMessageSize);
    const size_t storage_size  = xMessageCount * (block_size + FrtosMessageBuffer::ENVELOPE_SIZE);

    FrtosMessageBuffer *mb = new (pxStaticMessageBuffer)
        FrtosMessageBuffer(xMaxMessageSize, xMessageCount,
                           pucMessageBufferStorageArea, storage_size);

    return static_cast<MessageBufferHandle_t>(mb);
}

// ---------------------------------------------------------------------------
// xMessageBufferCreateWithCallback / xMessageBufferCreateStaticWithCallback
//
// Identical to xMessageBufferCreate / xMessageBufferCreateStatic but store two
// optional per-instance notification callbacks (m_send_cb, m_recv_cb) in
// FrtosMessageBuffer.  Callbacks fire at the end of every successful Send /
// Receive (including ISR variants), outside any critical section.
// The handle passed to each callback is the MessageBufferHandle_t cast to
// StreamBufferHandle_t, matching the FreeRTOS convention for this callback type.
// ---------------------------------------------------------------------------

MessageBufferHandle_t xMessageBufferCreateWithCallback(
    size_t                         xBufferSizeBytes,
    size_t                         xMaxMessageSize,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback)
{
    if ((xBufferSizeBytes == 0U) || (xMaxMessageSize == 0U))
        return nullptr;

    const size_t block_size = stk::memory::BlockMemoryPool::AlignBlockSize(xMaxMessageSize);
    const size_t slot_cost  = block_size + FrtosMessageBuffer::ENVELOPE_SIZE;
    const size_t count      = xBufferSizeBytes / slot_cost;

    if (count == 0U)
        return nullptr;

    FrtosMessageBuffer *mb = ObjAlloc<FrtosMessageBuffer>(
        xMaxMessageSize, count,
                           pxSendCompletedCallback, pxReceiveCompletedCallback);

    if (mb == nullptr)
        return nullptr;

    // Detect heap failure in the envelope buffer allocation performed by the
    // heap constructor (m_eq is backed by a separately ObjAllocArray'd byte array).
    if (!mb->m_eq.IsStorageValid())
    {
        ObjFreeRaw(mb);
        return nullptr;
    }

    return static_cast<MessageBufferHandle_t>(mb);
}

MessageBufferHandle_t xMessageBufferCreateStaticWithCallback(
    size_t                         xMaxMessageSize,
    size_t                         xMessageCount,
    uint8_t                       *pucMessageBufferStorageArea,
    StaticMessageBuffer_t         *pxStaticMessageBuffer,
    StreamBufferCallbackFunction_t pxSendCompletedCallback,
    StreamBufferCallbackFunction_t pxReceiveCompletedCallback)
{
    if ((pucMessageBufferStorageArea == nullptr) ||
        (pxStaticMessageBuffer       == nullptr) ||
        (xMaxMessageSize  == 0U) ||
        (xMessageCount    == 0U))
        return nullptr;

    static_assert(sizeof(StaticMessageBuffer_t) >= sizeof(FrtosMessageBuffer),
        "Increase STATIC_MESSAGE_BUFFER_TCB_SIZE_WORDS in FreeRTOS.h.");

    const size_t block_size   = stk::memory::BlockMemoryPool::AlignBlockSize(xMaxMessageSize);
    const size_t storage_size = xMessageCount * (block_size + FrtosMessageBuffer::ENVELOPE_SIZE);

    FrtosMessageBuffer *mb = new (pxStaticMessageBuffer)
        FrtosMessageBuffer(xMaxMessageSize, xMessageCount,
                           pucMessageBufferStorageArea, storage_size,
                           pxSendCompletedCallback, pxReceiveCompletedCallback);

    return static_cast<MessageBufferHandle_t>(mb);
}

void vMessageBufferDelete(MessageBufferHandle_t xMessageBuffer)
{
    if (xMessageBuffer == nullptr)
        return;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    ObjFree(mb);
}

size_t xMessageBufferSend(MessageBufferHandle_t xMessageBuffer,
                           const void           *pvTxData,
                           size_t                xDataLengthBytes,
                           TickType_t            xTicksToWait)
{
    if ((xMessageBuffer == nullptr) || (pvTxData == nullptr) || (xDataLengthBytes == 0U))
        return 0U;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    if (xDataLengthBytes > mb->m_max_msg_size)
    {
        STK_ASSERT(false); // API contract: message too large for this buffer
        return 0U;
    }

    const stk::Timeout stk_timeout = FrtosTimeoutToStk(xTicksToWait);

    // 1. Acquire a payload block (blocks until one is free or timeout).
    void *blk = mb->m_pool.TimedAlloc(stk_timeout);
    if (blk == nullptr)
        return 0U;

    // 2. Copy payload into the block.
    memcpy(blk, pvTxData, xDataLengthBytes);

    // 3. Enqueue the envelope (should always succeed: pool and eq are 1:1,
    //    but guard with NO_WAIT to avoid deadlock if they desync).
    FrtosMessageBuffer::MsgEnvelope env = { xDataLengthBytes, blk };

    if (!mb->m_eq.Put(&env, stk::NO_WAIT))
    {
        // Envelope queue unexpectedly full — return block and report failure.
        STK_ASSERT(false);
        mb->m_pool.Free(blk);
        return 0U;
    }

    // Fire send-complete callback outside any critical section.
    if (mb->m_send_cb != nullptr)
    {
        BaseType_t woken = pdFALSE;
        mb->m_send_cb(static_cast<StreamBufferHandle_t>(xMessageBuffer), &woken);
    }

    return xDataLengthBytes;
}

size_t xMessageBufferReceive(MessageBufferHandle_t xMessageBuffer,
                              void                 *pvRxData,
                              size_t                xBufferLengthBytes,
                              TickType_t            xTicksToWait)
{
    if ((xMessageBuffer == nullptr) || (pvRxData == nullptr) || (xBufferLengthBytes == 0U))
        return 0U;

    if (IsIrqContext() && (xTicksToWait != 0U))
        return 0U;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    // 1. Dequeue the next envelope.
    FrtosMessageBuffer::MsgEnvelope env = { 0U, nullptr };

    if (!mb->m_eq.Get(&env, FrtosTimeoutToStk(xTicksToWait)))
        return 0U;

    // 2. Validate destination capacity.
    if (xBufferLengthBytes < env.len)
    {
        // Destination too small: re-enqueue the envelope so the message is
        // not lost (best-effort; if re-enqueue also fails the message is lost).
        STK_ASSERT(false); // API contract: pvRxData buffer too small
        mb->m_eq.TryPut(&env);
        return 0U;
    }

    // 3. Copy payload out and return the block.
    memcpy(pvRxData, env.blk, env.len);
    mb->m_pool.Free(env.blk);

    // Fire receive-complete callback outside any critical section.
    if (mb->m_recv_cb != nullptr)
    {
        BaseType_t woken = pdFALSE;
        mb->m_recv_cb(static_cast<StreamBufferHandle_t>(xMessageBuffer), &woken);
    }

    return env.len;
}

BaseType_t xMessageBufferIsEmpty(MessageBufferHandle_t xMessageBuffer)
{
    if (xMessageBuffer == nullptr)
        return pdTRUE;

    return static_cast<FrtosMessageBuffer *>(xMessageBuffer)->m_eq.IsEmpty()
           ? pdTRUE : pdFALSE;
}

BaseType_t xMessageBufferIsFull(MessageBufferHandle_t xMessageBuffer)
{
    if (xMessageBuffer == nullptr)
        return pdTRUE;

    return static_cast<FrtosMessageBuffer *>(xMessageBuffer)->m_pool.IsFull()
           ? pdTRUE : pdFALSE;
}

size_t xMessageBufferSpacesAvailable(MessageBufferHandle_t xMessageBuffer)
{
    if (xMessageBuffer == nullptr)
        return 0U;

    // Free pool blocks == available envelope slots (they are 1:1).
    return static_cast<FrtosMessageBuffer *>(xMessageBuffer)->m_pool.GetFreeCount();
}

BaseType_t xMessageBufferReset(MessageBufferHandle_t xMessageBuffer)
{
    if (xMessageBuffer == nullptr)
        return pdFAIL;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    // Drain all pending envelopes and return their blocks to the pool.
    FrtosMessageBuffer::MsgEnvelope env = { 0U, nullptr };

    while (mb->m_eq.TryGet(&env))
    {
        if (env.blk != nullptr)
            mb->m_pool.Free(env.blk);
    }

    // Reset the envelope queue (wakes any blocked senders).
    mb->m_eq.Reset();

    return pdPASS;
}

// ===========================================================================
// Message Buffer ISR / extended API
//
// xMessageBufferSendFromISR
// -------------------------
// Mirrors xMessageBufferSend() but is strictly non-blocking:
//   1. TimedAlloc(NO_WAIT) — grabs a pool block without blocking.
//      BlockMemoryPool::TimedAlloc() with NO_WAIT uses a ScopedCriticalSection
//      internally, making it ISR-safe on all STK targets.
//   2. memcpy payload into the block.
//   3. TryPut (= Put(NO_WAIT)) — enqueues the envelope without blocking.
//      MessageQueue::Put(NO_WAIT) is also ScopedCriticalSection-guarded.
//
// If step 1 fails (pool empty) we return 0 immediately — no block was
// consumed so there is nothing to free.  If step 3 fails (should not happen
// given pool/eq are 1:1) we release the block and return 0, matching the
// defensive pattern in xMessageBufferSend().
//
// pxHigherPriorityTaskWoken is always set to pdFALSE: STK's condition
// variable machinery reschedules waiting tasks via the kernel's own
// priority mechanism without requiring the ISR to request a context switch.
//
// xMessageBufferReceiveFromISR
// ----------------------------
// Uses TryGet (= Get(NO_WAIT)) on m_eq, which is ISR-safe.  On success the
// envelope length is validated against the caller's buffer, the payload is
// copied, and the pool block is freed (Pool::Free() is also CS-guarded).
// On buffer-too-small the envelope is put back at the front so the message
// is not lost — TryPutFront (= PutFront(NO_WAIT)) retreats the tail pointer
// under the same CS, restoring the queue state exactly.
//
// xMessageBufferResetFromISR
// --------------------------
// Identical logic to xMessageBufferReset(); all underlying operations
// (TryGet, Pool::Free, MessageQueue::Reset) are already ISR-safe.
// pxHigherPriorityTaskWoken is always pdFALSE for the same reason as above.
//
// xMessageBufferNextLengthBytes
// -----------------------------
// Implements a non-destructive peek of the oldest envelope's length field
// using TryPeek(), which copies the envelope atomically without consuming it.
// No manual Get + PutFront pair or outer ScopedCriticalSection is needed:
// TryPeek() acquires its own internal CS and leaves m_tail and m_count
// unchanged.  Returns 0 if the buffer is empty.
// ===========================================================================

size_t xMessageBufferSendFromISR(MessageBufferHandle_t  xMessageBuffer,
                                  const void            *pvTxData,
                                  size_t                 xDataLengthBytes,
                                  BaseType_t            *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if ((xMessageBuffer == nullptr) || (pvTxData == nullptr) || (xDataLengthBytes == 0U))
        return 0U;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    if (xDataLengthBytes > mb->m_max_msg_size)
    {
        STK_ASSERT(false); // API contract: message exceeds max size
        return 0U;
    }

    // Step 1: non-blocking pool allocation.
    void *blk = mb->m_pool.TimedAlloc(stk::NO_WAIT);
    if (blk == nullptr)
        return 0U; // pool full — no block consumed, nothing to free

    // Step 2: copy payload.
    memcpy(blk, pvTxData, xDataLengthBytes);

    // Step 3: non-blocking enqueue.
    FrtosMessageBuffer::MsgEnvelope env = { xDataLengthBytes, blk };
    if (!mb->m_eq.TryPut(&env))
    {
        // Pool and eq are 1:1, so this should never happen.  Defensively free
        // the block to avoid a pool leak and report failure.
        STK_ASSERT(false);
        mb->m_pool.Free(blk);
        return 0U;
    }

    // Fire send-complete callback outside any critical section.
    // pxHigherPriorityTaskWoken is always pdFALSE per STK convention.
    if (mb->m_send_cb != nullptr)
    {
        BaseType_t woken = pdFALSE;
        mb->m_send_cb(static_cast<StreamBufferHandle_t>(xMessageBuffer), &woken);
    }

    return xDataLengthBytes;
}

size_t xMessageBufferReceiveFromISR(MessageBufferHandle_t  xMessageBuffer,
                                     void                  *pvRxData,
                                     size_t                 xBufferLengthBytes,
                                     BaseType_t            *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if ((xMessageBuffer == nullptr) || (pvRxData == nullptr) || (xBufferLengthBytes == 0U))
        return 0U;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    // Step 1: non-blocking dequeue of the oldest envelope.
    FrtosMessageBuffer::MsgEnvelope env = { 0U, nullptr };
    if (!mb->m_eq.TryGet(&env))
        return 0U; // buffer empty

    // Step 2: validate destination capacity.
    if (xBufferLengthBytes < env.len)
    {
        // Destination too small: put the envelope back at the front so the
        // message is not lost.  TryPutFront retreats the tail pointer under
        // its own CS — identical to Get never having happened.
        STK_ASSERT(false); // API contract: pvRxData buffer too small
        mb->m_eq.TryPutFront(&env);
        return 0U;
    }

    // Step 3: copy payload out and release pool block.
    memcpy(pvRxData, env.blk, env.len);
    mb->m_pool.Free(env.blk);

    // Fire receive-complete callback outside any critical section.
    // pxHigherPriorityTaskWoken is always pdFALSE per STK convention.
    if (mb->m_recv_cb != nullptr)
    {
        BaseType_t woken = pdFALSE;
        mb->m_recv_cb(static_cast<StreamBufferHandle_t>(xMessageBuffer), &woken);
    }

    return env.len;
}

BaseType_t xMessageBufferResetFromISR(MessageBufferHandle_t  xMessageBuffer,
                                       BaseType_t            *pxHigherPriorityTaskWoken)
{
    if (pxHigherPriorityTaskWoken != nullptr)
        *pxHigherPriorityTaskWoken = pdFALSE;

    if (xMessageBuffer == nullptr)
        return pdFAIL;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    // Drain all pending envelopes, returning every pool block before resetting
    // the queue.  All three operations are ISR-safe (ScopedCriticalSection).
    FrtosMessageBuffer::MsgEnvelope env = { 0U, nullptr };

    while (mb->m_eq.TryGet(&env))
    {
        if (env.blk != nullptr)
            mb->m_pool.Free(env.blk);
    }

    // Reset the envelope queue; wakes any sender blocked on a full pool.
    mb->m_eq.Reset();

    return pdPASS;
}

size_t xMessageBufferNextLengthBytes(MessageBufferHandle_t xMessageBuffer)
{
    if (xMessageBuffer == nullptr)
        return 0U;

    FrtosMessageBuffer *mb = static_cast<FrtosMessageBuffer *>(xMessageBuffer);

    // TryPeek copies the oldest envelope without consuming it.  The internal
    // ScopedCriticalSection makes this ISR-safe; no outer CS is required.
    FrtosMessageBuffer::MsgEnvelope env = { 0U, nullptr };

    if (!mb->m_eq.TryPeek(&env))
        return 0U; // buffer is empty — nothing to peek at

    return env.len;
}

#endif // configUSE_STREAM_BUFFERS
