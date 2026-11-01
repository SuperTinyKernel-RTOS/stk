/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <cstddef>         // for std::size_t
#include <cstdint>
#include <new>             // placement new
#include <cerrno>

#include "stk.h"           // C++ API: ITask / IKernel / hw::* / Sleep / Yield / GetTimeNowMs
#include "sync/stk_sync.h" // C++ API: sync::Mutex / ConditionVariable / RWMutex / SpinLock / Barrier / Event / Semaphore

#include <memory/stk_memory.h> // C++ API: memory::BlockMemoryPool (stack pool)

// Independent of the C API: neither stk_c.h nor any stk_* C function is used, everything below talks to the
// kernel through the C++ API directly (the ABI types come from stk_c_pthread.h itself).
#include "stk_c_pthread.h"
#include "semaphore.h" // sem_*: sync::SemaphoreT based, shares the helpers of this file
#include "mqueue.h"    // mq_*: Mutex + ConditionVariable based
#include <time.h>      // clock_gettime(), nanosleep(), timer_*: extended by time.h of this shim
#include <sys/time.h>  // gettimeofday(): extended by sys/time.h of this shim
#if !STK_C_DEFINE_SIGEVENT
#include <signal.h>     // union sigval / struct sigevent (layer's signal.h: defines _POSIX_THREADS for sigev_notify_function)
#endif
#include <unistd.h>    // sysconf(), getpid(): extended by unistd.h of this shim
#include <cstring>
#include <cstdarg>
#include <time/stk_time.h> // stk::time::TimerHost (POSIX timers)

#if !STK_TLS
    #error "stk_c_pthread.cpp requires STK_TLS (pthread_self()/pthread_getspecific() are built on the per-task TLS slot)."
#endif

/*
 * MISRA C++ notes for this translation unit:
 *  - Every function has a single point of exit (Rule 6-6-5). Error paths are expressed with
 *    a 'result' variable and nested if/else, never early return.
 *  - No 'continue' / 'break' is used in loops (Rules 6-6-3, 6-6-4): loop exit conditions are
 *    expressed in the loop condition itself.
 *  - Every 'else if' chain is terminated by an 'else' (Rule 6-4-2).
 *  - Objects and functions private to this file have internal linkage (Rule 3-3-1).
 *  - Remaining, intentional deviations:
 *      Rule 3-9-2   : pthread API signatures mandate the use of 'int' / 'unsigned int'.
 *      Rule 5-2-5   : const_cast in pthread_setspecific() (POSIX takes 'const void *').
 *      Rule 5-2-7/8/9 : reinterpret_cast of the stack pool storage and of the bound kernel pointer.
 *      Rule 7-3-4   : using-directives (namespace stk / stk::pthread).
 *      Rule 18-4-1  : malloc()/free() for non-default stack sizes, placement new for sync objects.
 */

// RAII critical section (default session semantics).
typedef stk::hw::CriticalSection::ScopedLock CsLock;

// Private malloc/free declarations (same as in stk_c_memory.cpp): overcomes absence of
// declarations under a -ffreestanding compiler flag. Only used for the "unusual"
// stack-size fallback path; the common-size path is served entirely from a static
// BlockMemoryPool with zero heap use.
extern "C" void *malloc(std::size_t size);
extern "C" void free(void *ptr);

// =============================================================================
// Internal state
// =============================================================================

// Opaque type forward-declared in stk_c_pthread.h as
// `typedef struct pthread_stk_ctrl_t *pthread_t;`. Must live at global scope
// (matching the header's forward declaration) so that pthread_t in caller code and
// this definition refer to the exact same type.
struct pthread_stk_ctrl_t;

using namespace stk;

namespace stk {
namespace pthread {

// Constructs T in-place inside an opaque storage container (anything exposing a 'data' member, e.g. the
// pthread_stk_*_mem_t of stk_c_pthread.h). Compile-time checks that the container is large and aligned enough
// for T (a mismatch with the C++ object sizes is a build error, not memory corruption).
template <typename T, typename Mem, typename... Args>
static inline T *ConstructIn(Mem &mem, Args &&... args)
{
    static_assert(sizeof(T) <= sizeof(Mem), "opaque storage is too small for the C++ object");
    static_assert(alignof(T) <= alignof(Mem), "opaque storage is not sufficiently aligned for the C++ object");

    STK_ASSERT((reinterpret_cast<uintptr_t>(mem.data) % alignof(T)) == 0U);

    return new (static_cast<void *>(mem.data)) T(static_cast<Args &&>(args)...);
}

// Fixed-capacity name helpers (no <cstring> dependency).
static inline std::size_t NameLen(const char *s)
{
    std::size_t n = 0U;

    while (s[n] != '\0')
    {
        ++n;
    }

    return n;
}

// Copies \a src into a buffer of \a cap bytes (including the NUL). Returns false (dst untouched) if it does not fit.
static inline bool NameCopy(char *dst, std::size_t cap, const char *src)
{
    const std::size_t n  = NameLen(src);
    const bool        ok = (n < cap);

    if (ok)
    {
        for (std::size_t i = 0U; i <= n; ++i)
        {
            dst[i] = src[i];
        }
    }

    return ok;
}

// -----------------------------------------------------------------------------
// PthreadTask: the kernel-facing task object embedded in every thread control block.
// -----------------------------------------------------------------------------
class PthreadTask final : public ITask
{
public:
    explicit PthreadTask()
        : m_ctrl(nullptr), m_stack(nullptr), m_stack_size(0U), m_mode(ACCESS_USER),
          m_weight(STK_C_SCHED_PRIORITY_DEFAULT), m_policy(SCHED_OTHER)
    {
        (void)NameCopy(m_name, sizeof(m_name), "pthread");
    }

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~PthreadTask() = default;

    void Initialize(pthread_stk_ctrl_t *ctrl, pthread_stk_word_t *stack, size_t stack_size, EAccessMode mode,
                    int32_t weight, int policy, const char *name)
    {
        m_ctrl       = ctrl;
        m_stack      = stack;
        m_stack_size = stack_size;
        m_mode       = mode;
        m_weight     = weight;
        m_policy     = policy;

        if ((name == nullptr) || (name[0] == '\0') || !NameCopy(m_name, sizeof(m_name), name))
        {
            (void)NameCopy(m_name, sizeof(m_name), "pthread");
        }
    }

    int GetPolicy() const { return m_policy; }

    // records the scheduling parameters applied to the running task (the kernel copy is changed by stk::SetWeight())
    void SetSchedule(int32_t weight, int policy)
    {
        const CsLock cs_;

        m_weight = weight;
        m_policy = policy;
    }

    // Name accessors: the name is read by the kernel/trace tools, so it is updated under a critical section.
    bool SetName(const char *name)
    {
        const CsLock cs_;

        return NameCopy(m_name, sizeof(m_name), name);
    }

    bool GetName(char *buf, std::size_t buf_size) const
    {
        const CsLock cs_;

        return NameCopy(buf, buf_size, m_name);
    }

    // ITask
    EAccessMode GetAccessMode() const override { return m_mode; }
    int32_t GetWeight()         const override { return m_weight; }
    const char *GetTraceName()  const override { return m_name; }

    // IStackMemory
    const Word *GetStack() const override { return m_stack; }
    size_t GetStackSize()  const override { return m_stack_size; }

private:
    STK_NONCOPYABLE_CLASS(PthreadTask);

    void Run() override;    // thread trampoline, see below
    void OnExit() override; // signals pthread_stk_ctrl_t::done_event (kernel context, ISR-safe primitive only)

    pthread_stk_ctrl_t *m_ctrl;
    pthread_stk_word_t         *m_stack;
    size_t              m_stack_size;
    EAccessMode         m_mode;
    int32_t             m_weight; // task weight = sched_priority
    int                 m_policy; // SCHED_* (informational)
    char                m_name[STK_C_PTHREAD_NAME_MAX];
};

} // namespace pthread
} // namespace stk

struct pthread_stk_ctrl_t
{
    pthread_stk_ctrl_t()
    {
        Reset();
    }

    // Returns the block to its pristine state. The embedded task object is NOT touched
    // (it is non-copyable and is re-armed via PthreadTask::Initialize() on creation).
    void Reset()
    {
        busy            = false;
        finished        = false;
        detached        = false;
        joined          = false;
        reclaimed       = false;
        adopted         = false;
        tid             = TID_NONE;
        cancel_enabled  = true;
        cancel_pending  = false;
        cleanup_count   = 0U;
        start_routine   = nullptr;
        arg             = nullptr;
        retval          = nullptr;
        stack           = nullptr;
        stack_owned     = false;
        stack_from_pool = false;
        done_event      = nullptr;

        for (std::size_t i = 0U; i < STK_C_PTHREAD_KEYS_MAX; ++i)
        {
            tsd[i] = nullptr;
        }
    }

    bool             busy;
    bool             finished;   // start_routine returned / pthread_exit() called (retval is valid)
    bool             detached;
    bool             joined;
    bool             reclaimed;  // resources already freed or handed to the reaper
    bool             adopted;    // control block wraps a foreign task (e.g. bootstrap task), see CurrentCtrlOrAdopt()

    TId              tid;            // kernel id of an ADOPTED task (a created thread: id derived from its ITask, see TidOf())
    bool             cancel_enabled; // PTHREAD_CANCEL_ENABLE
    bool             cancel_pending; // pthread_cancel() was requested
    uint8_t          cleanup_count;  // pthread_cleanup_push() depth
    struct {
        void (*fn)(void *);
        void  *arg;
    } cleanup[STK_C_PTHREAD_CLEANUP_MAX];

    stk::pthread::PthreadTask task;
    void            *(*start_routine)(void *);
    void            *arg;
    void            *retval;

    pthread_stk_word_t      *stack;
    bool             stack_owned;     // false => caller-supplied via pthread_attr_setstack()
    bool             stack_from_pool; // true  => came from s_StackPool, else malloc()

    stk::sync::Event *done_event; // constructed in place in done_event_buf by pthread_create()
    struct {
        alignas(stk::sync::Event) uint8_t data[sizeof(stk::sync::Event)];
    } done_event_buf;

    void            *tsd[STK_C_PTHREAD_KEYS_MAX]; // pthread_setspecific()/getspecific() storage
};

namespace stk {
namespace pthread {

using ThreadCtrl = pthread_stk_ctrl_t;

constexpr std::size_t kDefaultStackBytes =
    static_cast<std::size_t>(STK_C_PTHREAD_DEFAULT_STACK_WORDS) * sizeof(pthread_stk_word_t);

// pthread_once_t::__state values
constexpr int kOnceRunning = 1;
constexpr int kOnceDone    = 2;

static ThreadCtrl s_Threads[STK_C_PTHREAD_MAX_THREADS];

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

// Acquire/release accessors used by every lazy-init fast path below. The unlocked
// "is it initialized yet?" check of a double-checked pattern must be an *acquire*
// load, and the publishing store a *release* store; otherwise on a multi-core
// configuration (multi-core kernel) another core can observe the non-NULL handle
// before it observes the object the handle points to.
#if defined(__GNUC__) || defined(__clang__)
template <typename T> static inline T LoadAcquire(T *p)          { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
template <typename T> static inline void StoreRelease(T *p, T v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#elif defined(__ICCARM__)
// IAR: no generic __atomic_load_n/__atomic_store_n for plain scalars (xatomic.h only offers
// std::atomic internals and atomic_flag), so build acquire/release from a volatile access
// plus a full data memory barrier. __DMB() is also a compiler barrier.
template <typename T> static inline T LoadAcquire(T *p)
{
    const T v = *static_cast<volatile T *>(p);
    __stk_dmb(); // acquire: later accesses cannot be reordered before this load
    return v;
}
template <typename T> static inline void StoreRelease(T *p, T v)
{
    __stk_dmb(); // release: earlier accesses are visible before this store
    *static_cast<volatile T *>(p) = v;
}
#else
// Fallback for compilers without __atomic_*: correct on single-core targets only.
template <typename T> static inline T LoadAcquire(T *p)          { return *static_cast<volatile T *>(p); }
template <typename T> static inline void StoreRelease(T *p, T v) { *static_cast<volatile T *>(p) = v; }
#endif

// Lazily constructs the object for statically-initialized pthread_* variables and returns it.
// 'slot' (the pthread object's __handle) doubles as the "constructed" flag.
template <typename T, typename Mem>
static T *EnsureObject(void **slot, Mem &mem)
{
    void *h = LoadAcquire(slot);
    if (h == nullptr)
    {
        const CsLock cs_;

        h = *slot; // re-check under the lock
        if (h == nullptr)
        {
            h = ConstructIn<T>(mem);
            StoreRelease(slot, h);
        }
    }

    return static_cast<T *>(h);
}

// Typed views of an already-constructed object (NULL-checks are the callers' job).
static inline sync::Mutex             *HandleOf(pthread_mutex_t *o)    { return static_cast<sync::Mutex *>(o->__handle); }
static inline sync::Mutex             *HandleOf(pthread_once_t *o)     { return static_cast<sync::Mutex *>(o->__handle); }
static inline sync::ConditionVariable *HandleOf(pthread_cond_t *o)     { return static_cast<sync::ConditionVariable *>(o->__handle); }
static inline sync::RWMutex           *HandleOf(pthread_rwlock_t *o)   { return static_cast<sync::RWMutex *>(o->__handle); }
static inline hw::SpinLock            *HandleOf(pthread_spinlock_t *o) { return static_cast<hw::SpinLock *>(o->__handle); }
static inline sync::Barrier           *HandleOf(pthread_barrier_t *o)  { return static_cast<sync::Barrier *>(o->__handle); }

// -----------------------------------------------------------------------------
// Local functions
// -----------------------------------------------------------------------------
static void RunKeyDestructors(ThreadCtrl *ctrl);

// -----------------------------------------------------------------------------
// Bound kernel
// -----------------------------------------------------------------------------
static IKernel *s_BoundKernel = nullptr;

// -----------------------------------------------------------------------------
// Static stack pool (zero heap) for default-size stacks: a plain C++ BlockMemoryPool over static storage
// -----------------------------------------------------------------------------
typedef stk::memory::BlockMemoryPool StackPool;

static constexpr std::size_t kStackPoolBytes = STK_C_PTHREAD_MAX_THREADS * StackPool::AlignBlockSize(kDefaultStackBytes);
static_assert((kStackPoolBytes % sizeof(pthread_stk_word_t)) == 0U, "stack pool block size must be a multiple of the stack word size");

alignas(STK_STACK_MEMORY_ALIGN) static pthread_stk_word_t s_PthreadStackStorage[kStackPoolBytes / sizeof(pthread_stk_word_t)];
static StackPool *s_StackPool = nullptr;
static struct {
    alignas(StackPool) uint8_t data[sizeof(StackPool)];
} s_StackPoolBuf;

static StackPool *EnsureStackPool()
{
    StackPool *pool = LoadAcquire(&s_StackPool);
    if (pool == nullptr)
    {
        const CsLock cs_;

        pool = s_StackPool;
        if (pool == nullptr)
        {
            pool = ConstructIn<StackPool>(s_StackPoolBuf,
                                          STK_C_PTHREAD_MAX_THREADS,
                                          kDefaultStackBytes,
                                          reinterpret_cast<uint8_t *>(s_PthreadStackStorage),
                                          sizeof(s_PthreadStackStorage),
                                          "pthread_stacks");
            StoreRelease(&s_StackPool, pool);
        }
    }

    return pool;
}

// -----------------------------------------------------------------------------
// Reaper task: reclaims detached-thread resources once they finish.
// -----------------------------------------------------------------------------
static ThreadCtrl  *s_ReapQueue[STK_C_PTHREAD_MAX_THREADS];
static std::size_t  s_ReapHead  = 0U;
static std::size_t  s_ReapCount = 0U;

// Counts the entries of s_ReapQueue: bounded by the queue capacity, so a signal beyond it (a defect) is caught
// by the semaphore's own contract (Signal() asserts) instead of silently over-counting.
static_assert(sizeof(pthread_stk_word_t) == sizeof(Word), "pthread_stk_word_t must match stk::Word");

typedef sync::SemaphoreT<uint16_t> ReapSemaphore;
static_assert(STK_C_PTHREAD_MAX_THREADS <= ReapSemaphore::COUNT_MAX, "ReapSemaphore counter is too narrow for STK_C_PTHREAD_MAX_THREADS");

static ReapSemaphore *s_ReapSem = nullptr;
static struct {
    alignas(ReapSemaphore) uint8_t data[sizeof(ReapSemaphore)];
} s_ReapSemBuf;

// Internal reaper task: a statically allocated ITask (no C task handle, no heap), added to the kernel on first use.
class ReaperTask final : public ITask
{
public:
    explicit ReaperTask() : m_stack()
    {}

    STK_VIRT_DTOR ~ReaperTask() = default;

    EAccessMode GetAccessMode() const override { return ACCESS_USER; }
    int32_t     GetWeight()     const override { return STK_C_SCHED_PRIORITY_DEFAULT; }
    const char *GetTraceName()  const override { return "pthread_reaper"; }
    const Word *GetStack()      const override { return m_stack; }
    size_t      GetStackSize()  const override { return STK_C_PTHREAD_REAPER_STACK_WORDS; }

private:
    STK_NONCOPYABLE_CLASS(ReaperTask);

    void Run() override; // reaper loop, see below

    alignas(STK_STACK_MEMORY_ALIGN) pthread_stk_word_t m_stack[STK_C_PTHREAD_REAPER_STACK_WORDS];
};

static ReaperTask s_Reaper;
static bool       s_ReaperAdded = false;

static void ReleaseThreadResources(ThreadCtrl *ctrl);

// Adds the reaper task to the kernel on first use. Returns true if the reaper is running afterwards. A failure
// (the kernel has no free task slot) is not final: the next call retries, so the callers which need the reaper
// (detached threads) report it instead of leaking the thread. Safe to call concurrently.
static bool EnsureReaper()
{
    if (!LoadAcquire(&s_ReaperAdded))
    {
        STK_ASSERT(s_BoundKernel != nullptr);

        const CsLock cs_; // serializes concurrent retries: the reaper task may be added only once

        if (!s_ReaperAdded)
        {
            if (s_ReapSem == nullptr)
            {
                s_ReapSem = ConstructIn<ReapSemaphore>(s_ReapSemBuf,
                                                       0U,
                                                       static_cast<uint16_t>(STK_C_PTHREAD_MAX_THREADS));
            }

            // KERNEL_DYNAMIC: a failure leaves the kernel untouched, so the same task object can be added again later
            const EAddResult ar = s_BoundKernel->AddTask(&s_Reaper);
            if (ar == ADD_OK)
            {
                StoreRelease(&s_ReaperAdded, true); // published only once the kernel has accepted the task
            }
            else
            {
                // out of kernel task slots is retryable, anything else (invalid/duplicate) is a design error
                STK_ASSERT(ar == ADD_FAIL_POOL_FULL);
                STK_UNUSED(ar);
            }
        }
    }

    return LoadAcquire(&s_ReaperAdded);
}

static void EnqueueReap(ThreadCtrl *ctrl)
{
    STK_ASSERT(s_ReaperAdded);

    const CsLock cs_;

    STK_ASSERT(s_ReapCount < STK_C_PTHREAD_MAX_THREADS);
    if (s_ReapCount < STK_C_PTHREAD_MAX_THREADS)
    {
        const std::size_t tail = (s_ReapHead + s_ReapCount) % STK_C_PTHREAD_MAX_THREADS;
        s_ReapQueue[tail] = ctrl;
        ++s_ReapCount;
    }
}

static ThreadCtrl *DequeueReap()
{
    ThreadCtrl *result = nullptr;

    {
        const CsLock cs_;

        if (s_ReapCount > 0U)
        {
            result = s_ReapQueue[s_ReapHead];
            s_ReapHead = (s_ReapHead + 1U) % STK_C_PTHREAD_MAX_THREADS;
            --s_ReapCount;
        }
    }

    return result;
}

// Wait forever for a signal on an Event/Semaphore and return only once it was signaled. A cancelled
// wait (IKernel::CancelTaskWait(), IKernelService::SleepCancel()) is absorbed and retried: the callers
// cannot report it (pthread_join() must not release the thread's resources before the thread is done),
// and a cancelled wait never consumes the signal. A wait the kernel rejected is an internal defect.
template <typename TObj>
static void WaitSignaled(TObj &obj)
{
    EWaitResult wr;

    do
    {
        wr = obj.WaitEx(WAIT_INFINITE);
    }
    while (wr == WAIT_RESULT_CANCELED);

    if (wr != WAIT_RESULT_SIGNAL)
    {
        STK_KERNEL_PANIC(KERNEL_PANIC_ASSERT);
    }
}

void ReaperTask::Run()
{
    for (;;)
    {
        STK_ASSERT(s_ReapSem != nullptr);
        WaitSignaled(*s_ReapSem);

        ThreadCtrl *const ctrl = DequeueReap();
        if (ctrl != nullptr)
        {
            // done_event is set by PthreadTask::OnExit(): the task is torn down and its stack is free to reuse
            WaitSignaled(*ctrl->done_event);
            ReleaseThreadResources(ctrl);
        }
    }
}

// Exactly-once trigger: called both when a detached thread finishes and when an
// already-running thread is detached. Whichever observes "finished && detached"
// first hands the thread off to the reaper.
static void MaybeReap(ThreadCtrl *ctrl)
{
    bool do_reap = false;

    {
        const CsLock cs_;

        if (ctrl->finished && ctrl->detached && !ctrl->reclaimed)
        {
            ctrl->reclaimed = true;
            do_reap = true;
        }
    }

    if (do_reap)
    {
        EnqueueReap(ctrl);
        s_ReapSem->Signal();
    }
}

// -----------------------------------------------------------------------------
// Slot management
// -----------------------------------------------------------------------------
static ThreadCtrl *AcquireThreadSlot()
{
    ThreadCtrl *result = nullptr;

    {
        const CsLock cs_;

        for (std::size_t i = 0U; (result == nullptr) && (i < STK_C_PTHREAD_MAX_THREADS); ++i)
        {
            if (!s_Threads[i].busy)
            {
                s_Threads[i].Reset();
                s_Threads[i].busy = true;
                result = &s_Threads[i];
            }
        }
    }

    return result;
}

static void ReleaseThreadResources(ThreadCtrl *ctrl)
{
    if (ctrl->done_event != nullptr)
    {
        ctrl->done_event->~Event();
        ctrl->done_event = nullptr;
    }

    if (ctrl->stack_owned && (ctrl->stack != nullptr))
    {
        if (ctrl->stack_from_pool)
        {
            (void)s_StackPool->Free(ctrl->stack);
        }
        else
        {
            free(ctrl->stack);
        }
    }
    ctrl->stack = nullptr;

    {
        const CsLock cs_;

        ctrl->busy = false;
    }
}

// Provides the thread stack: either the caller-supplied one, a pool block (default size),
// or a malloc() block. Returns true if ctrl->stack is valid afterwards.
static bool AllocateStack(ThreadCtrl *ctrl, const pthread_attr_t *attr, std::size_t stack_bytes)
{
    if ((attr != nullptr) && (attr->__ext_stack != nullptr))
    {
        ctrl->stack           = attr->__ext_stack;
        ctrl->stack_owned     = false;
        ctrl->stack_from_pool = false;
    }
    else
    {
        void *blk = nullptr;

        if (stack_bytes == kDefaultStackBytes)
        {
            StackPool *const pool = EnsureStackPool();
            if (pool != nullptr)
            {
                blk = pool->TryAlloc();
            }
        }

        if (blk != nullptr)
        {
            ctrl->stack           = static_cast<pthread_stk_word_t *>(blk);
            ctrl->stack_owned     = true;
            ctrl->stack_from_pool = true;
        }
        else
        {
            ctrl->stack           = static_cast<pthread_stk_word_t *>(malloc(stack_bytes));
            ctrl->stack_owned     = true;
            ctrl->stack_from_pool = false;
        }
    }

    return (ctrl->stack != nullptr);
}

// -----------------------------------------------------------------------------
// Thread trampoline (PthreadTask::Run) and exit notification (PthreadTask::OnExit)
// -----------------------------------------------------------------------------
void PthreadTask::Run()
{
    ThreadCtrl *const ctrl = m_ctrl;

    hw::SetTlsPtr(static_cast<void *>(ctrl));

    void *const result = ctrl->start_routine(ctrl->arg);

    RunKeyDestructors(ctrl);

    {
        const CsLock cs_;

        ctrl->retval   = result;
        ctrl->finished = true;
    }

    MaybeReap(ctrl);

    // Natural return: STK tears the task down and calls OnExit(), which signals done_event. A joiner (or the
    // reaper, for detached threads) reclaims *our* resources (stack, control block) only after that signal.
}

void PthreadTask::OnExit()
{
    // Kernel context: only ISR-safe primitives may be used here (ITask::OnExit() contract), Event::Set() is one.
    // This is the last time the kernel touches this task: once done_event is signaled the thread is finished
    // (retval is valid, written before the task ended) and its stack is no longer in use, so the control block
    // may be recycled. Replaces the former flag + 1 ms sleep-poll.
    (void)m_ctrl->done_event->Set();
}

// -----------------------------------------------------------------------------
// Absolute-deadline -> relative-timeout helper
//
// STK has no wall clock; abstime is interpreted as a point on the same timeline as
// stk::GetTimeNowMs() (see the "timedwait limitation" note in stk_c_pthread.h).
// -----------------------------------------------------------------------------
static Timeout TimespecToRelativeTimeout(const struct timespec *abstime)
{
    const Time now_ms    = GetTimeNowMs();
    // ceil: a deadline must never be reported as passed before abstime
    const Time target_ms = (static_cast<Time>(abstime->tv_sec) * 1000LL) +
                           ((static_cast<Time>(abstime->tv_nsec) + 999999LL) / 1000000LL);

    Time rel_ms = target_ms - now_ms;
    Timeout result = NO_WAIT; // deadline already in the past: non-blocking attempt

    if (rel_ms > 0LL)
    {
        if (rel_ms > static_cast<Time>(INT32_MAX))
        {
            rel_ms = static_cast<Time>(INT32_MAX);
        }

        // round UP to whole ticks: truncation could turn a deadline shorter than one tick into NO_WAIT
        // (an early ETIMEDOUT), or wake up to one tick before abstime
        const Time res_us = static_cast<Time>(GetTickResolution());
        Time ticks = (res_us > 0LL) ? (((rel_ms * 1000LL) + res_us - 1LL) / res_us) : rel_ms;

        // stay below WAIT_INFINITE: a very distant deadline must remain a timed wait
        const Time ticks_max = static_cast<Time>(WAIT_INFINITE) - 1LL;
        if (ticks > ticks_max)
        {
            ticks = ticks_max;
        }

        result = static_cast<Timeout>(ticks);
    }

    return result;
}

// Map the outcome of a timed lock attempt to errno. CANCELED must be handled by the caller first
// (retried, the deadline is absolute so the remaining time is simply recomputed): POSIX timed locks have
// no "interrupted" error, and ETIMEDOUT must not be reported before abstime has passed.
static int TimedResultToErrno(EWaitResult wr)
{
    int result;

    switch (wr)
    {
    case WAIT_RESULT_SIGNAL:
        result = 0;
        break;

    case WAIT_RESULT_TIMEOUT:
        result = ETIMEDOUT;
        break;

    case WAIT_RESULT_CANCELED:
    case WAIT_RESULT_FAIL:
    default:
        result = EINVAL; // kernel rejected the wait
        break;
    }

    return result;
}

// POSIX requires EINVAL for a malformed abstime
static inline bool IsValidTimespec(const struct timespec *ts)
{
    return (ts != nullptr) && (ts->tv_nsec >= 0) && (ts->tv_nsec < 1000000000L);
}

// STK has one timeline (GetTimeNowMs()), CLOCK_REALTIME and CLOCK_MONOTONIC both map to it
static inline bool IsSupportedClock(clockid_t clock_id)
{
    return (clock_id == CLOCK_REALTIME) || (clock_id == CLOCK_MONOTONIC);
}

// Whole ticks, rounded UP, of a relative time (never reports less time than requested)
static inline int64_t RelativeTicksCeil(int64_t sec, int64_t nsec)
{
    const int64_t res_us = static_cast<int64_t>(GetTickResolution());
    const int64_t sec_max = (INT64_MAX / 1000000LL) - 1LL; // keeps the microsecond count in range
    const int64_t s = (sec > sec_max) ? sec_max : sec;
    const int64_t us = (s * 1000000LL) + ((nsec + 999LL) / 1000LL);

    return (res_us > 0LL) ? ((us + res_us - 1LL) / res_us) : us;
}

// Sleeps for \a ticks. Returns the ticks NOT slept (0 if the full time elapsed): the sleep can be ended early by
// IKernelService::SleepCancel() (pthread_cancel()).
static int64_t SleepTicksInterruptible(int64_t ticks)
{
    int64_t remaining = ticks;
    const int64_t chunk_max = static_cast<int64_t>(WAIT_INFINITE) - 1LL;

    while (remaining > 0LL)
    {
        const int64_t chunk = (remaining > chunk_max) ? chunk_max : remaining;
        const Ticks   start = GetTicks();

        stk::Sleep(static_cast<Timeout>(chunk));

        const int64_t slept = static_cast<int64_t>(GetTicks() - start);
        if (slept >= chunk)
        {
            remaining -= chunk;
        }
        else
        {
            remaining -= slept; // interrupted
            break;
        }
    }

    return remaining;
}

static inline void TicksToTimespec(int64_t ticks, struct timespec *ts)
{
    const int64_t us = ticks * static_cast<int64_t>(GetTickResolution());

    ts->tv_sec  = static_cast<time_t>(us / 1000000LL);
    ts->tv_nsec = static_cast<long>((us % 1000000LL) * 1000LL);
}

static inline bool IsValidSchedPolicy(int policy)
{
    return (policy == SCHED_OTHER) || (policy == SCHED_FIFO) || (policy == SCHED_RR);
}

static inline bool IsValidSchedPriority(int prio)
{
    return (prio >= STK_C_SCHED_PRIORITY_MIN) && (prio <= STK_C_SCHED_PRIORITY_MAX);
}

// pshared setter shared by all the attribute types: only PTHREAD_PROCESS_PRIVATE exists
static inline int CheckPShared(int pshared)
{
    return (pshared == PTHREAD_PROCESS_PRIVATE) ? 0 : ((pshared == PTHREAD_PROCESS_SHARED) ? ENOTSUP : EINVAL);
}

static sync::Mutex *EnsureMutex(pthread_mutex_t *m)
{
    return EnsureObject<sync::Mutex>(&m->__handle, m->__mem);
}

static inline bool IsMutexOwner(const sync::Mutex *m)
{
    return (m->GetOwner() == IKernelService::GetInstance()->GetTid());
}

static inline bool IsValidMutexType(int type)
{
    return (type == PTHREAD_MUTEX_NORMAL) ||
           (type == PTHREAD_MUTEX_ERRORCHECK) ||
           (type == PTHREAD_MUTEX_RECURSIVE);
}

// Checks that must run before locking a mutex the caller may already own. Safe without a
// critical section: only the calling thread can make "owner == self" true or false, and
// the recursion counter is stable while the caller owns the lock.
// Returns 0 if the lock attempt may proceed, otherwise the errno to return;
// errorcheck_code is the error for relocking an ERRORCHECK mutex (EDEADLK or EBUSY).
static int PreLockCheck(const pthread_mutex_t *pm, const sync::Mutex *m, int errorcheck_code)
{
    int result = 0;

    if (IsMutexOwner(m))
    {
        if (pm->__type == PTHREAD_MUTEX_ERRORCHECK)
        {
            result = errorcheck_code;
        }
        else
        {
            result = (m->GetRecursionCount() >= sync::Mutex::RECURSION_MAX) ? EAGAIN : 0;
        }
    }

    return result;
}

static sync::ConditionVariable *EnsureCond(pthread_cond_t *c)
{
    return EnsureObject<sync::ConditionVariable>(&c->__handle, c->__mem);
}

static sync::RWMutex *EnsureRWLock(pthread_rwlock_t *rw)
{
    return EnsureObject<sync::RWMutex>(&rw->__handle, rw->__mem);
}

static sync::Mutex *EnsureOnceMutex(pthread_once_t *o)
{
    return EnsureObject<sync::Mutex>(&o->__handle, o->__mem);
}

// -----------------------------------------------------------------------------
// Current-thread identity (TLS), including adoption of foreign tasks
// -----------------------------------------------------------------------------

// Control block of the calling task if it already has one (pthread_create()'d, or
// previously adopted); NULL otherwise. Never allocates.
static ThreadCtrl *CurrentCtrl()
{
    return hw::GetTlsPtr<ThreadCtrl>();
}

// True if the calling thread has a pending cancellation request and cancellation is enabled.
static bool CancelPending()
{
    ThreadCtrl *const ctrl = CurrentCtrl();

    return (ctrl != nullptr) && ctrl->cancel_enabled && LoadAcquire(&ctrl->cancel_pending);
}

// Cancellation point (deferred cancellation): terminates the calling thread, same as pthread_exit(PTHREAD_CANCELED).
// Must not be called while holding a shim-internal lock.
static void TestCancelPoint()
{
    if (CancelPending())
    {
        pthread_exit(PTHREAD_CANCELED);
    }
}

// Kernel id of the task of a thread: a created thread is identified by the address of its ITask, an adopted one by
// the id recorded at adoption
static TId TidOf(ThreadCtrl *ctrl)
{
    return ctrl->adopted ? ctrl->tid : GetTidFromUserTask(&ctrl->task);
}

// Runs the pushed cleanup handlers in LIFO order.
static void RunCleanupHandlers(ThreadCtrl *ctrl)
{
    while (ctrl->cleanup_count != 0U)
    {
        --ctrl->cleanup_count;

        void (*const fn)(void *) = ctrl->cleanup[ctrl->cleanup_count].fn;
        void *const arg          = ctrl->cleanup[ctrl->cleanup_count].arg;

        fn(arg);
    }
}

// Gives the calling task a pthread identity on first use. This is what makes
// pthread_self()/pthread_setspecific() work from tasks the shim did not create
// (typically the application's bootstrap task). The block comes from the same
// s_Threads[] pool, so each adopted task permanently consumes one
// STK_C_PTHREAD_MAX_THREADS slot. An adopted thread is never joinable (it is marked
// detached), is never reaped, and its TSD destructors only run if it calls
// pthread_exit(). Only the calling task touches its own TLS slot, so no locking is needed.
static ThreadCtrl *CurrentCtrlOrAdopt()
{
    ThreadCtrl *ctrl = CurrentCtrl();
    if (ctrl == nullptr)
    {
        ctrl = AcquireThreadSlot();
        if (ctrl != nullptr)
        {
            ctrl->adopted  = true;
            ctrl->tid      = stk::GetTid();
            ctrl->detached = true; // not joinable, never handed to the reaper
            hw::SetTlsPtr(static_cast<void *>(ctrl));
        }
    }

    return ctrl;
}

// -----------------------------------------------------------------------------
// Thread-specific data (keys)
// -----------------------------------------------------------------------------
struct KeyEntry
{
    bool  used;
    void (*destructor)(void *);
};

static KeyEntry s_Keys[STK_C_PTHREAD_KEYS_MAX];

// True if 'key' is in range and currently allocated. The range check short-circuits
// the array access.
static inline bool IsKeyUsed(pthread_key_t key)
{
    return (key < STK_C_PTHREAD_KEYS_MAX) && s_Keys[key].used;
}

// Runs at thread termination (natural return or pthread_exit()) for a thread that
// was created via pthread_create() - see the "thread-specific data limitation"
// note in stk_c_pthread.h. Mirrors POSIX: each pass calls the destructor for every
// key whose value is currently non-NULL (clearing the value first, so a
// destructor that reads it back via pthread_getspecific() sees NULL), and repeats
// while any destructor call left a *new* non-NULL value, up to
// PTHREAD_DESTRUCTOR_ITERATIONS passes.
static void RunKeyDestructors(ThreadCtrl *ctrl)
{
    bool more = true;

    for (int iter = 0; more && (iter < PTHREAD_DESTRUCTOR_ITERATIONS); ++iter)
    {
        bool any_ran = false;

        for (std::size_t i = 0U; i < STK_C_PTHREAD_KEYS_MAX; ++i)
        {
            void *const value = ctrl->tsd[i];

            if (value != nullptr)
            {
                void (*destructor)(void *) = nullptr;
                {
                    const CsLock cs_;

                    if (s_Keys[i].used)
                    {
                        destructor = s_Keys[i].destructor;
                    }
                }

                ctrl->tsd[i] = nullptr; // clear before calling, per POSIX
                if (destructor != nullptr)
                {
                    destructor(value);
                    any_ran = true;
                }
            }
        }

        more = any_ran;
    }
}

// C++ entry point: binds the kernel threads are created on and adds the internal reaper task to it. Declare it in
// stk_c_pthread.h (inside __cplusplus) to call it directly from C++ code, stk_pthread_bind_kernel() wraps it.
void BindKernel(IKernel *kernel);

void BindKernel(IKernel *kernel)
{
    STK_ASSERT(kernel != nullptr);

    s_BoundKernel = kernel;

    // no reaper means detached threads can not be reclaimed: increase the kernel task count. Not final, as
    // pthread_create(DETACHED) and pthread_detach() retry and report EAGAIN while the reaper is missing
    const bool reaper_ok = EnsureReaper();
    STK_ASSERT(reaper_ok);
    STK_UNUSED(reaper_ok);
}

} // namespace pthread
} // namespace stk

using namespace stk::pthread;

// =============================================================================
// C-interface
// =============================================================================
extern "C" {

// -----------------------------------------------------------------------------
// Bootstrap
// -----------------------------------------------------------------------------
void stk_pthread_bind_kernel(void *kernel)
{
    STK_ASSERT(kernel != nullptr);

    // the pointer is the stk::IKernel object itself (a stk_kernel_t* of the C API is the same object)
    BindKernel(static_cast<IKernel *>(kernel));
}

// -----------------------------------------------------------------------------
// Thread attributes
// -----------------------------------------------------------------------------
int pthread_attr_init(pthread_attr_t *attr)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        attr->__stack_bytes = 0U;
        attr->__ext_stack   = nullptr;
        attr->__detachstate = PTHREAD_CREATE_JOINABLE;
        attr->__privileged  = PTHREAD_CREATE_USER_NP;
        attr->__policy      = SCHED_OTHER;
        attr->__priority    = STK_C_SCHED_PRIORITY_DEFAULT;
        attr->__inheritsched = PTHREAD_EXPLICIT_SCHED;
        attr->__scope       = PTHREAD_SCOPE_SYSTEM;
        attr->__guardsize   = 0U;
        attr->__name[0]     = '\0';
        result = 0;
    }

    return result;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    return (attr != nullptr) ? 0 : EINVAL;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    int result = EINVAL;

    if ((attr != nullptr) && (stacksize != 0U))
    {
        attr->__stack_bytes = stacksize;
        result = 0;
    }

    return result;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
    int result = EINVAL;

    if ((attr != nullptr) && (stacksize != nullptr))
    {
        *stacksize = (attr->__stack_bytes != 0U) ? attr->__stack_bytes : kDefaultStackBytes;
        result = 0;
    }

    return result;
}

int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize)
{
    int result = EINVAL;

    if ((attr != nullptr) && (stackaddr != nullptr) && (stacksize >= sizeof(pthread_stk_word_t)))
    {
        if ((reinterpret_cast<uintptr_t>(stackaddr) & (sizeof(pthread_stk_word_t) - 1U)) == 0U)
        {
            attr->__ext_stack   = static_cast<pthread_stk_word_t *>(stackaddr);
            attr->__stack_bytes = stacksize;
            result = 0;
        }
    }

    return result;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, size_t *stacksize)
{
    int result = EINVAL;

    if ((attr != nullptr) && (stackaddr != nullptr) && (stacksize != nullptr))
    {
        *stackaddr = attr->__ext_stack;
        *stacksize = attr->__stack_bytes;
        result = 0;
    }

    return result;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    int result = EINVAL;

    if ((attr != nullptr) &&
        ((detachstate == PTHREAD_CREATE_JOINABLE) || (detachstate == PTHREAD_CREATE_DETACHED)))
    {
        attr->__detachstate = detachstate;
        result = 0;
    }

    return result;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    int result = EINVAL;

    if ((attr != nullptr) && (detachstate != nullptr))
    {
        *detachstate = attr->__detachstate;
        result = 0;
    }

    return result;
}

int pthread_attr_setprivileged_np(pthread_attr_t *attr, int mode)
{
    int result = EINVAL;

    if ((attr != nullptr) &&
        ((mode == PTHREAD_CREATE_USER_NP) || (mode == PTHREAD_CREATE_PRIVILEGED_NP)))
    {
        attr->__privileged = mode;
        result = 0;
    }

    return result;
}

int pthread_attr_getprivileged_np(const pthread_attr_t *attr, int *mode)
{
    int result = EINVAL;

    if ((attr != nullptr) && (mode != nullptr))
    {
        *mode = attr->__privileged;
        result = 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Thread lifecycle
// -----------------------------------------------------------------------------
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    STK_ASSERT(s_BoundKernel != nullptr);
    STK_ASSERT(thread != nullptr);
    STK_ASSERT(start_routine != nullptr);

    int result = 0;

    if ((s_BoundKernel == nullptr) || (thread == nullptr) || (start_routine == nullptr))
    {
        result = EINVAL;
    }
    else
    {
        // a detached thread is reclaimed by the reaper: do not create one which can never be released
        const bool want_detached = ((attr != nullptr) && (attr->__detachstate == PTHREAD_CREATE_DETACHED));

        pthread_stk_ctrl_t *const ctrl = (want_detached && !EnsureReaper()) ? nullptr : AcquireThreadSlot();

        if (ctrl == nullptr)
        {
            result = EAGAIN;
        }
        else
        {
            const size_t stack_bytes = ((attr != nullptr) && (attr->__stack_bytes != 0U)) ?
                attr->__stack_bytes : kDefaultStackBytes;
            const uint32_t stack_words =
                static_cast<uint32_t>((stack_bytes + sizeof(pthread_stk_word_t) - 1U) / sizeof(pthread_stk_word_t));

            if (!AllocateStack(ctrl, attr, stack_bytes))
            {
                {
                    const CsLock cs_;

                    ctrl->busy = false;
                }
                result = EAGAIN;
            }
            else
            {
                ctrl->start_routine = start_routine;
                ctrl->arg           = arg;
                ctrl->retval        = nullptr;
                ctrl->detached      = ((attr != nullptr) && (attr->__detachstate == PTHREAD_CREATE_DETACHED));
                ctrl->done_event    = ConstructIn<sync::Event>(ctrl->done_event_buf, true /* manual_reset */);

                const bool privileged = (attr != nullptr) && (attr->__privileged == PTHREAD_CREATE_PRIVILEGED_NP);

                // scheduling: the weight of an STK task is fixed when it is added to the kernel
                int32_t     weight = STK_C_SCHED_PRIORITY_DEFAULT;
                int         policy = SCHED_OTHER;
                const char *name   = nullptr;

                if (attr != nullptr)
                {
                    policy = attr->__policy;
                    name   = attr->__name;

                    const ThreadCtrl *const creator = CurrentCtrl();
                    if ((attr->__inheritsched == PTHREAD_INHERIT_SCHED) && (creator != nullptr))
                    {
                        weight = creator->task.GetWeight();
                        policy = creator->task.GetPolicy();
                    }
                    else if (attr->__inheritsched != PTHREAD_INHERIT_SCHED)
                    {
                        weight = static_cast<int32_t>(attr->__priority);
                    }
                    else
                    {
                        // inherit requested by a thread which is not created by pthread_create(): default weight
                    }
                }

                ctrl->task.Initialize(ctrl, ctrl->stack, stack_words,
                                      privileged ? stk::ACCESS_PRIVILEGED : stk::ACCESS_USER,
                                      weight, policy, name);

                // KERNEL_DYNAMIC: a failure (out of kernel task slots) is returned, not asserted
                const EAddResult ar = s_BoundKernel->AddTask(&ctrl->task);
                if (ar == ADD_OK)
                {
                    *thread = ctrl;
                }
                else
                {
                    // the kernel never referenced the task: it is safe to release everything at once
                    // (the event, the stack and the control block), no thread was created
                    ReleaseThreadResources(ctrl);

                    // POSIX EAGAIN means "retry later": only a full kernel task pool qualifies, a kernel which
                    // rejects the task (invalid state / duplicate) can not succeed on a retry
                    result = (ar == ADD_FAIL_POOL_FULL) ? EAGAIN : EINVAL;
                }
            }
        }
    }

    return result;
}

// Common join path. \a poll: single non-blocking attempt (pthread_tryjoin_np), otherwise a wait until \a abstime
// (NULL = forever). A join which did not complete (timeout) leaves the thread joinable.
static int JoinThread(pthread_t thread, void **retval, const struct timespec *abstime, bool poll)
{
    if (!poll)
    {
        TestCancelPoint(); // pthread_join() is a cancellation point
    }

    pthread_stk_ctrl_t *const ctrl = thread;
    int result = 0;

    if (ctrl == nullptr)
    {
        result = EINVAL;
    }
    else if (ctrl == CurrentCtrl())
    {
        result = EDEADLK; // joining yourself would wait forever
    }
    else
    {
        bool ok = false;
        {
            const CsLock cs_;

            if (!ctrl->detached && !ctrl->joined)
            {
                ctrl->joined = true;
                ok = true;
            }
        }

        if (!ok)
        {
            result = EINVAL;
        }
        else
        {
            // done_event is signaled by PthreadTask::OnExit(): the thread is finished and its task is torn down
            EWaitResult wr;

            for (;;)
            {
                const Timeout timeout = poll ? NO_WAIT :
                    ((abstime != nullptr) ? TimespecToRelativeTimeout(abstime) : WAIT_INFINITE);

                wr = ctrl->done_event->WaitEx(timeout);

                if (wr != WAIT_RESULT_CANCELED)
                {
                    break;
                }

                if (CancelPending())
                {
                    {
                        const CsLock cs_;

                        ctrl->joined = false; // give the join claim back before this thread terminates
                    }

                    TestCancelPoint();
                }
            }

            if (wr == WAIT_RESULT_SIGNAL)
            {
                if (retval != nullptr)
                {
                    *retval = ctrl->retval;
                }

                ReleaseThreadResources(ctrl);
            }
            else
            {
                // not finished (timeout) or the wait was rejected: give the join claim back
                {
                    const CsLock cs_;

                    ctrl->joined = false;
                }

                result = (wr == WAIT_RESULT_TIMEOUT) ? (poll ? EBUSY : ETIMEDOUT) : EINVAL;
            }
        }
    }

    return result;
}

int pthread_join(pthread_t thread, void **retval)
{
    return JoinThread(thread, retval, nullptr, false);
}

int pthread_tryjoin_np(pthread_t thread, void **retval)
{
    return JoinThread(thread, retval, nullptr, true);
}

int pthread_timedjoin_np(pthread_t thread, void **retval, const struct timespec *abstime)
{
    return IsValidTimespec(abstime) ? JoinThread(thread, retval, abstime, false) : EINVAL;
}

int pthread_detach(pthread_t thread)
{
    pthread_stk_ctrl_t *const ctrl = thread;
    int result = 0;

    if (ctrl == nullptr)
    {
        result = EINVAL;
    }
    else
    {
        // the reaper reclaims a detached thread: if it is missing the thread stays joinable
        if (!EnsureReaper())
        {
            result = EAGAIN;
        }
        else
        {
            bool ok = false;
            {
                const CsLock cs_;

                if (!ctrl->joined && !ctrl->detached)
                {
                    ctrl->detached = true;
                    ok = true;
                }
            }

            if (!ok)
            {
                result = EINVAL;
            }
            else
            {
                MaybeReap(ctrl);
            }
        }
    }

    return result;
}

// Ends the calling thread without running handlers or destructors: bookkeeping, then the kernel removes the task
// (stk::ExitTask() works for any task, also for an adopted one). Does not return.
static void TerminateSelf(ThreadCtrl *ctrl, void *retval)
{
    if (ctrl != nullptr)
    {
        if (ctrl->adopted)
        {
            ReleaseThreadResources(ctrl); // nobody joins an adopted task: free its slot
        }
        else
        {
            {
                const CsLock cs_;

                ctrl->retval   = retval;
                ctrl->finished = true;
            }

            MaybeReap(ctrl); // joiner/reaper wait for done_event (signaled by OnExit()) before freeing anything
        }
    }

    stk::ExitTask(); // does not return; also correct for a task which never used the pthread API
}

void pthread_exit(void *retval)
{
    pthread_stk_ctrl_t *const ctrl = CurrentCtrl();

    if (ctrl != nullptr)
    {
        RunCleanupHandlers(ctrl);
        RunKeyDestructors(ctrl);
    }

    TerminateSelf(ctrl, retval);

    for (;;) // not reached: ExitTask() does not return
    {
        stk::Sleep(stk::WAIT_INFINITE);
    }
}

pthread_t pthread_self(void)
{
    return static_cast<pthread_t>(CurrentCtrlOrAdopt()); // NULL only if the slot pool is exhausted
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return (t1 == t2) ? 1 : 0;
}

int pthread_yield(void)
{
    stk::Yield();
    return 0;
}

// -----------------------------------------------------------------------------
// Mutex
// -----------------------------------------------------------------------------
int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        attr->__type     = PTHREAD_MUTEX_DEFAULT;
        attr->__protocol = PTHREAD_PRIO_NONE;
        result = 0;
    }

    return result;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    return (attr != nullptr) ? 0 : EINVAL;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    int result = EINVAL;

    if ((attr != nullptr) && IsValidMutexType(type))
    {
        attr->__type = type;
        result = 0;
    }

    return result;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
    int result = EINVAL;

    if ((attr != nullptr) && (type != nullptr))
    {
        *type = attr->__type;
        result = 0;
    }

    return result;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    int result = EINVAL;

    if (mutex != nullptr)
    {
        const int type = (attr != nullptr) ? attr->__type : PTHREAD_MUTEX_DEFAULT;

        if (IsValidMutexType(type))
        {
            mutex->__type   = type;
            mutex->__handle = ConstructIn<stk::sync::Mutex>(mutex->__mem);
            result = 0;
        }
    }

    return result;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    int result = EINVAL;

    if (mutex != nullptr)
    {
        if (mutex->__handle != nullptr)
        {
            HandleOf(mutex)->~Mutex();
            mutex->__handle = nullptr;
        }
        result = 0;
    }

    return result;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    int result = EINVAL;

    if (mutex != nullptr)
    {
        sync::Mutex *const m = EnsureMutex(mutex);

        result = PreLockCheck(mutex, m, EDEADLK);
        if (result == 0)
        {
            m->Lock();
        }
    }

    return result;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    int result = EINVAL;

    if (mutex != nullptr)
    {
        sync::Mutex *const m = EnsureMutex(mutex);

        result = PreLockCheck(mutex, m, EBUSY);
        if (result == 0)
        {
            result = m->TryLock() ? 0 : EBUSY;
        }
    }

    return result;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    int result = EINVAL;

    if ((mutex != nullptr) && (mutex->__handle != nullptr))
    {
        sync::Mutex *const m = HandleOf(mutex);

        if (IsMutexOwner(m))
        {
            m->Unlock();
            result = 0;
        }
        else
        {
            result = EPERM;
        }
    }

    return result;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime)
{
    int result = EINVAL;

    if ((mutex != nullptr) && (abstime != nullptr))
    {
        sync::Mutex *const m = EnsureMutex(mutex);

        result = PreLockCheck(mutex, m, EDEADLK);
        if (result == 0)
        {
            EWaitResult wr;

            do
            {
                wr = m->TimedLockEx(TimespecToRelativeTimeout(abstime));
            }
            while (wr == WAIT_RESULT_CANCELED);

            result = TimedResultToErrno(wr);
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// Condition variable
// -----------------------------------------------------------------------------
int pthread_condattr_init(pthread_condattr_t *attr)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        attr->__clock = CLOCK_REALTIME;
        result = 0;
    }

    return result;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    return (attr != nullptr) ? 0 : EINVAL;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t * /*attr*/)
{
    int result = EINVAL;

    if (cond != nullptr)
    {
        cond->__handle = ConstructIn<stk::sync::ConditionVariable>(cond->__mem);
        result = 0;
    }

    return result;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    int result = EINVAL;

    if (cond != nullptr)
    {
        if (cond->__handle != nullptr)
        {
            HandleOf(cond)->~ConditionVariable();
            cond->__handle = nullptr;
        }
        result = 0;
    }

    return result;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    int result = 0;

    if ((cond == nullptr) || (mutex == nullptr) || (mutex->__handle == nullptr))
    {
        result = EINVAL;
    }
    else if ((mutex->__type == PTHREAD_MUTEX_ERRORCHECK) && !IsMutexOwner(HandleOf(mutex)))
    {
        result = EPERM;
    }
    else
    {
        // a cancelled wait is reported as a spurious wake-up (allowed by POSIX, callers re-check their
        // predicate); the mutex is re-acquired in all cases
        TestCancelPoint();

        const EWaitResult wr = EnsureCond(cond)->WaitEx(*HandleOf(mutex), WAIT_INFINITE);

        result = (wr == WAIT_RESULT_FAIL) ? EINVAL : 0;

        TestCancelPoint(); // a cancellation request ends the wait: the mutex is held again here, as POSIX requires
    }

    return result;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
    int result = 0;

    if ((cond == nullptr) || (mutex == nullptr) || (mutex->__handle == nullptr) || (abstime == nullptr))
    {
        result = EINVAL;
    }
    else if ((mutex->__type == PTHREAD_MUTEX_ERRORCHECK) && !IsMutexOwner(HandleOf(mutex)))
    {
        result = EPERM;
    }
    else
    {
        TestCancelPoint();

        const EWaitResult wr = EnsureCond(cond)->WaitEx(*HandleOf(mutex), TimespecToRelativeTimeout(abstime));

        // a cancelled wait is a spurious wake-up (0), not ETIMEDOUT: abstime may not have passed yet
        result = (wr == WAIT_RESULT_CANCELED) ? 0 : TimedResultToErrno(wr);

        TestCancelPoint();
    }

    return result;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    int result = EINVAL;

    if (cond != nullptr)
    {
        EnsureCond(cond)->NotifyOne();
        result = 0;
    }

    return result;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    int result = EINVAL;

    if (cond != nullptr)
    {
        EnsureCond(cond)->NotifyAll();
        result = 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Read-write lock
// -----------------------------------------------------------------------------
int pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        attr->__reserved = 0;
        result = 0;
    }

    return result;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
    return (attr != nullptr) ? 0 : EINVAL;
}

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t * /*attr*/)
{
    int result = EINVAL;

    if (rwlock != nullptr)
    {
        rwlock->__handle   = ConstructIn<stk::sync::RWMutex>(rwlock->__mem);
        rwlock->__wrlocked = false;
        result = 0;
    }

    return result;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
    int result = EINVAL;

    if (rwlock != nullptr)
    {
        if (rwlock->__handle != nullptr)
        {
            HandleOf(rwlock)->~RWMutex();
            rwlock->__handle = nullptr;
        }
        result = 0;
    }

    return result;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    int result = EINVAL;

    if (rwlock != nullptr)
    {
        EnsureRWLock(rwlock)->ReadLock();
        result = 0;
    }

    return result;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    int result = EINVAL;

    if (rwlock != nullptr)
    {
        result = EnsureRWLock(rwlock)->TryReadLock() ? 0 : EBUSY;
    }

    return result;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock, const struct timespec *abstime)
{
    int result = EINVAL;

    if ((rwlock != nullptr) && (abstime != nullptr))
    {
        EWaitResult wr;

        do
        {
            wr = EnsureRWLock(rwlock)->TimedReadLockEx(TimespecToRelativeTimeout(abstime));
        }
        while (wr == WAIT_RESULT_CANCELED);

        result = TimedResultToErrno(wr);
    }

    return result;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    int result = EINVAL;

    if (rwlock != nullptr)
    {
        EnsureRWLock(rwlock)->Lock();
        rwlock->__wrlocked = true;
        result = 0;
    }

    return result;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    int result = EINVAL;

    if (rwlock != nullptr)
    {
        if (EnsureRWLock(rwlock)->TryLock())
        {
            rwlock->__wrlocked = true;
            result = 0;
        }
        else
        {
            result = EBUSY;
        }
    }

    return result;
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock, const struct timespec *abstime)
{
    int result = EINVAL;

    if ((rwlock != nullptr) && (abstime != nullptr))
    {
        EWaitResult wr;

        do
        {
            wr = EnsureRWLock(rwlock)->TimedLockEx(TimespecToRelativeTimeout(abstime));
        }
        while (wr == WAIT_RESULT_CANCELED);

        result = TimedResultToErrno(wr);
        if (result == 0)
        {
            rwlock->__wrlocked = true;
        }
    }

    return result;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    int result = EINVAL;

    if ((rwlock != nullptr) && (rwlock->__handle != nullptr))
    {
        // See "pthread_rwlock_unlock() disambiguation" in stk_c_pthread.h: a write
        // hold is always exclusive, so __wrlocked unambiguously identifies which
        // underlying call (Unlock() vs ReadUnlock()) the current holder must have made. Clear it before
        // releasing so a racing new writer's own acquisition can't be clobbered.
        if (rwlock->__wrlocked)
        {
            rwlock->__wrlocked = false;
            HandleOf(rwlock)->Unlock();
        }
        else
        {
            HandleOf(rwlock)->ReadUnlock();
        }
        result = 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Spin lock
// -----------------------------------------------------------------------------
int pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
    int result = EINVAL;

    if (lock != nullptr)
    {
        if (pshared != PTHREAD_PROCESS_PRIVATE)
        {
            result = ENOTSUP;
        }
        else
        {
            lock->__handle = ConstructIn<stk::hw::SpinLock>(lock->__mem);
            result = 0;
        }
    }

    return result;
}

int pthread_spin_destroy(pthread_spinlock_t *lock)
{
    int result = EINVAL;

    if (lock != nullptr)
    {
        if (lock->__handle != nullptr)
        {
            HandleOf(lock)->~SpinLock();
            lock->__handle = nullptr;
        }
        result = 0;
    }

    return result;
}

int pthread_spin_lock(pthread_spinlock_t *lock)
{
    int result = EINVAL;

    if ((lock != nullptr) && (lock->__handle != nullptr))
    {
        HandleOf(lock)->Lock();
        result = 0;
    }

    return result;
}

int pthread_spin_trylock(pthread_spinlock_t *lock)
{
    int result = EINVAL;

    if ((lock != nullptr) && (lock->__handle != nullptr))
    {
        result = HandleOf(lock)->TryLock() ? 0 : EBUSY;
    }

    return result;
}

int pthread_spin_unlock(pthread_spinlock_t *lock)
{
    int result = EINVAL;

    if ((lock != nullptr) && (lock->__handle != nullptr))
    {
        HandleOf(lock)->Unlock();
        result = 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Barrier
// -----------------------------------------------------------------------------
int pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        attr->__reserved = 0;
        result = 0;
    }

    return result;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
    return (attr != nullptr) ? 0 : EINVAL;
}

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t * /*attr*/,
                          unsigned int count)
{
    int result = EINVAL;

    if ((barrier != nullptr) && (count != 0U))
    {
        barrier->__handle = ConstructIn<stk::sync::Barrier>(barrier->__mem, static_cast<uint32_t>(count));
        result = 0;
    }

    return result;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    int result = EINVAL;

    if (barrier != nullptr)
    {
        if (barrier->__handle != nullptr)
        {
            HandleOf(barrier)->~Barrier();
            barrier->__handle = nullptr;
        }
        result = 0;
    }

    return result;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    int result = EINVAL;

    if ((barrier != nullptr) && (barrier->__handle != nullptr))
    {
        // a cancelled wait rolls this arrival back (the barrier did not trip), reporting 0 would let the
        // caller run on as if it had: pthread_barrier_wait() has no "interrupted" result, so arrive again
        sync::Barrier::EResult br;

        do
        {
            br = HandleOf(barrier)->WaitEx();
        }
        while (br == sync::Barrier::BARRIER_CANCELED);

        result = (br == sync::Barrier::BARRIER_LAST_ARRIVAL) ? PTHREAD_BARRIER_SERIAL_THREAD : 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Once
// -----------------------------------------------------------------------------
int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    int result = 0;

    if ((once_control == nullptr) || (init_routine == nullptr))
    {
        result = EINVAL;
    }
    else if (LoadAcquire(&once_control->__state) != kOnceDone) // fast path: already done, no lock needed
    {
        stk::sync::Mutex *const guard = EnsureOnceMutex(once_control);

        // The guard is held only while init_routine() runs, so "owned by the caller" means
        // init_routine() re-entered pthread_once() on the same control object. The guard is a
        // recursive Mutex, so Lock() would succeed and init_routine() would recurse until the
        // stack overflows; report the misuse instead (POSIX leaves it undefined).
        if (IsMutexOwner(guard))
        {
            result = EDEADLK;
        }
        else
        {
            guard->Lock();

            // Re-check under the lock: another thread may have finished init_routine()
            // (or be running it right now, in which case this lock() call already blocked
            // until it was done) between our unlocked fast-path check above and here.
            if (once_control->__state != kOnceDone)
            {
                once_control->__state = kOnceRunning;
                init_routine();
                StoreRelease(&once_control->__state, kOnceDone); // release: publish init_routine()'s effects
            }

            guard->Unlock();
        }
    }
    else
    {
        // already initialized: nothing to do
    }

    return result;
}

// -----------------------------------------------------------------------------
// Thread-specific data (keys)
// -----------------------------------------------------------------------------
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    int result = EINVAL;

    if (key != nullptr)
    {
        result = EAGAIN;

        const CsLock cs_;

        for (std::size_t i = 0U; (result != 0) && (i < STK_C_PTHREAD_KEYS_MAX); ++i)
        {
            if (!s_Keys[i].used)
            {
                s_Keys[i].used       = true;
                s_Keys[i].destructor = destructor;
                *key                 = static_cast<pthread_key_t>(i);
                result               = 0;
            }
        }
    }

    return result;
}

int pthread_key_delete(pthread_key_t key)
{
    int result = EINVAL;

    if (key < STK_C_PTHREAD_KEYS_MAX)
    {
        const CsLock cs_;

        if (s_Keys[key].used)
        {
            s_Keys[key].used       = false;
            s_Keys[key].destructor = nullptr;
            result                 = 0;
        }
    }

    return result;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    int result = EINVAL;

    if (IsKeyUsed(key))
    {
        ThreadCtrl *const ctrl = CurrentCtrlOrAdopt(); // foreign tasks are adopted on first use

        if (ctrl == nullptr)
        {
            result = ENOMEM; // pthread slot pool exhausted
        }
        else
        {
            ctrl->tsd[key] = const_cast<void *>(value);
            result = 0;
        }
    }

    return result;
}

void *pthread_getspecific(pthread_key_t key)
{
    void *result = nullptr;

    if (IsKeyUsed(key))
    {
        ThreadCtrl *const ctrl = CurrentCtrl(); // no adoption: a task that never called setspecific() has no values

        if (ctrl != nullptr)
        {
            result = ctrl->tsd[key];
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// Scheduling (<sched.h>) and scheduling attributes
// -----------------------------------------------------------------------------
int sched_yield(void)
{
    stk::Yield();
    return 0;
}

int sched_get_priority_min(int policy)
{
    int result = -1;

    if (IsValidSchedPolicy(policy))
    {
        result = STK_C_SCHED_PRIORITY_MIN;
    }
    else
    {
        errno = EINVAL;
    }

    return result;
}

int sched_get_priority_max(int policy)
{
    int result = -1;

    if (IsValidSchedPolicy(policy))
    {
        result = STK_C_SCHED_PRIORITY_MAX;
    }
    else
    {
        errno = EINVAL;
    }

    return result;
}

int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param)
{
    int result = EINVAL;

    if ((attr != nullptr) && (param != nullptr) && IsValidSchedPriority(param->sched_priority))
    {
        attr->__priority = param->sched_priority;
        result = 0;
    }

    return result;
}

int pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param)
{
    int result = EINVAL;

    if ((attr != nullptr) && (param != nullptr))
    {
        param->sched_priority = attr->__priority;
        result = 0;
    }

    return result;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
    int result = EINVAL;

    if ((attr != nullptr) && IsValidSchedPolicy(policy))
    {
        attr->__policy = policy;
        result = 0;
    }

    return result;
}

int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy)
{
    int result = EINVAL;

    if ((attr != nullptr) && (policy != nullptr))
    {
        *policy = attr->__policy;
        result = 0;
    }

    return result;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched)
{
    int result = EINVAL;

    if ((attr != nullptr) &&
        ((inheritsched == PTHREAD_INHERIT_SCHED) || (inheritsched == PTHREAD_EXPLICIT_SCHED)))
    {
        attr->__inheritsched = inheritsched;
        result = 0;
    }

    return result;
}

int pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inheritsched)
{
    int result = EINVAL;

    if ((attr != nullptr) && (inheritsched != nullptr))
    {
        *inheritsched = attr->__inheritsched;
        result = 0;
    }

    return result;
}

int pthread_attr_setscope(pthread_attr_t *attr, int scope)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        if (scope == PTHREAD_SCOPE_SYSTEM)
        {
            attr->__scope = scope;
            result = 0;
        }
        else if (scope == PTHREAD_SCOPE_PROCESS)
        {
            result = ENOTSUP;
        }
        else
        {
            // invalid value: EINVAL
        }
    }

    return result;
}

int pthread_attr_getscope(const pthread_attr_t *attr, int *scope)
{
    int result = EINVAL;

    if ((attr != nullptr) && (scope != nullptr))
    {
        *scope = attr->__scope;
        result = 0;
    }

    return result;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        attr->__guardsize = guardsize;
        result = 0;
    }

    return result;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize)
{
    int result = EINVAL;

    if ((attr != nullptr) && (guardsize != nullptr))
    {
        *guardsize = attr->__guardsize;
        result = 0;
    }

    return result;
}

int pthread_attr_setname_np(pthread_attr_t *attr, const char *name)
{
    int result = EINVAL;

    if ((attr != nullptr) && (name != nullptr))
    {
        result = NameCopy(attr->__name, sizeof(attr->__name), name) ? 0 : ERANGE;
    }

    return result;
}

int pthread_attr_getname_np(const pthread_attr_t *attr, char *name, size_t len)
{
    int result = EINVAL;

    if ((attr != nullptr) && (name != nullptr))
    {
        result = NameCopy(name, len, attr->__name) ? 0 : ERANGE;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Run-time scheduling parameters and names of a thread
// -----------------------------------------------------------------------------
int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param)
{
    int result = EINVAL;

    if ((thread != nullptr) && (policy != nullptr) && (param != nullptr))
    {
        *policy               = thread->task.GetPolicy();
        param->sched_priority = static_cast<int>(thread->task.GetWeight());
        result = 0;
    }

    return result;
}

// Applies policy/priority to a running thread: the priority level is the task weight of the switch strategy
// (SwitchStrategyFixedPriority), changed at run-time with stk::SetWeight()
static int ApplySchedParam(pthread_t thread, int policy, int prio)
{
    int result = 0;

    if ((policy != thread->task.GetPolicy()) || (prio != static_cast<int>(thread->task.GetWeight())))
    {
        if (prio == static_cast<int>(thread->task.GetWeight()))
        {
            thread->task.SetSchedule(thread->task.GetWeight(), policy); // policy is informational only
        }
        else
        {
#if STK_C_PTHREAD_RUNTIME_PRIORITY
            if (!thread->busy || thread->finished || thread->reclaimed)
            {
                result = ESRCH;
            }
            else
            {
                // requires WEIGHT_API=1 and PRIORITY_INHERITANCE_API=1 in the strategy (asserts otherwise)
                (void)stk::SetWeight(TidOf(thread), static_cast<Weight>(prio));

                thread->task.SetSchedule(static_cast<int32_t>(prio), policy);
            }
#else
            result = ENOTSUP;
#endif
        }
    }

    return result;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
{
    int result = EINVAL;

    if ((thread != nullptr) && (param != nullptr) && IsValidSchedPolicy(policy) &&
        IsValidSchedPriority(param->sched_priority))
    {
        result = ApplySchedParam(thread, policy, param->sched_priority);
    }

    return result;
}

int pthread_setschedprio(pthread_t thread, int prio)
{
    int result = EINVAL;

    if ((thread != nullptr) && IsValidSchedPriority(prio))
    {
        result = ApplySchedParam(thread, thread->task.GetPolicy(), prio);
    }

    return result;
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    int result = EINVAL;

    if ((thread != nullptr) && (name != nullptr))
    {
        result = thread->task.SetName(name) ? 0 : ERANGE;
    }

    return result;
}

int pthread_getname_np(pthread_t thread, char *name, size_t len)
{
    int result = EINVAL;

    if ((thread != nullptr) && (name != nullptr))
    {
        result = thread->task.GetName(name, len) ? 0 : ERANGE;
    }

    return result;
}

int pthread_setconcurrency(int level)
{
    return (level >= 0) ? 0 : EINVAL;
}

int pthread_getconcurrency(void)
{
    return 0;
}

// -----------------------------------------------------------------------------
// Synchronization attributes: process-shared, protocol, clock
// -----------------------------------------------------------------------------
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
    return (attr != nullptr) ? CheckPShared(pshared) : EINVAL;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *pshared)
{
    int result = EINVAL;

    if ((attr != nullptr) && (pshared != nullptr))
    {
        *pshared = PTHREAD_PROCESS_PRIVATE;
        result = 0;
    }

    return result;
}

int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
    return (attr != nullptr) ? CheckPShared(pshared) : EINVAL;
}

int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared)
{
    int result = EINVAL;

    if ((attr != nullptr) && (pshared != nullptr))
    {
        *pshared = PTHREAD_PROCESS_PRIVATE;
        result = 0;
    }

    return result;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared)
{
    return (attr != nullptr) ? CheckPShared(pshared) : EINVAL;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr, int *pshared)
{
    int result = EINVAL;

    if ((attr != nullptr) && (pshared != nullptr))
    {
        *pshared = PTHREAD_PROCESS_PRIVATE;
        result = 0;
    }

    return result;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared)
{
    return (attr != nullptr) ? CheckPShared(pshared) : EINVAL;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr, int *pshared)
{
    int result = EINVAL;

    if ((attr != nullptr) && (pshared != nullptr))
    {
        *pshared = PTHREAD_PROCESS_PRIVATE;
        result = 0;
    }

    return result;
}

int pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr, int protocol)
{
    int result = EINVAL;

    if (attr != nullptr)
    {
        if ((protocol == PTHREAD_PRIO_NONE) || (protocol == PTHREAD_PRIO_INHERIT))
        {
            attr->__protocol = protocol;
            result = 0;
        }
        else if (protocol == PTHREAD_PRIO_PROTECT)
        {
            result = ENOTSUP; // no priority ceiling in STK
        }
        else
        {
            // invalid value: EINVAL
        }
    }

    return result;
}

int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *attr, int *protocol)
{
    int result = EINVAL;

    if ((attr != nullptr) && (protocol != nullptr))
    {
        *protocol = attr->__protocol;
        result = 0;
    }

    return result;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
    int result = EINVAL;

    if ((attr != nullptr) && IsSupportedClock(clock_id))
    {
        attr->__clock = static_cast<int>(clock_id);
        result = 0;
    }

    return result;
}

int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock_id)
{
    int result = EINVAL;

    if ((attr != nullptr) && (clock_id != nullptr))
    {
        *clock_id = static_cast<clockid_t>(attr->__clock);
        result = 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Clock-selecting timed waits (POSIX.1-2024): one STK timeline, so they validate and forward
// -----------------------------------------------------------------------------
int pthread_mutex_clocklock(pthread_mutex_t *mutex, clockid_t clock_id, const struct timespec *abstime)
{
    return (IsSupportedClock(clock_id) && IsValidTimespec(abstime)) ? pthread_mutex_timedlock(mutex, abstime) : EINVAL;
}

int pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex, clockid_t clock_id,
                           const struct timespec *abstime)
{
    return (IsSupportedClock(clock_id) && IsValidTimespec(abstime)) ?
        pthread_cond_timedwait(cond, mutex, abstime) : EINVAL;
}

int pthread_rwlock_clockrdlock(pthread_rwlock_t *rwlock, clockid_t clock_id, const struct timespec *abstime)
{
    return (IsSupportedClock(clock_id) && IsValidTimespec(abstime)) ?
        pthread_rwlock_timedrdlock(rwlock, abstime) : EINVAL;
}

int pthread_rwlock_clockwrlock(pthread_rwlock_t *rwlock, clockid_t clock_id, const struct timespec *abstime)
{
    return (IsSupportedClock(clock_id) && IsValidTimespec(abstime)) ?
        pthread_rwlock_timedwrlock(rwlock, abstime) : EINVAL;
}

// -----------------------------------------------------------------------------
// POSIX semaphores (<semaphore.h>): unnamed semaphores on top of stk::sync::SemaphoreT
// -----------------------------------------------------------------------------
typedef sync::SemaphoreT<uint32_t> PosixSemaphore;

static inline PosixSemaphore *SemOf(sem_t *o) { return static_cast<PosixSemaphore *>(o->__handle); }

static inline bool IsSemReady(const sem_t *sem) { return (sem != nullptr) && (sem->__handle != nullptr); }

// POSIX sem_* convention: 0 on success, otherwise -1 and errno
static int SemFail(int err)
{
    errno = err;
    return -1;
}

// \a timeout_errno: errno for a result of WAIT_RESULT_TIMEOUT (EAGAIN for the poll, ETIMEDOUT for the timed wait)
static int SemWaitResult(EWaitResult wr, int timeout_errno)
{
    int result = 0;

    switch (wr)
    {
    case WAIT_RESULT_SIGNAL:
        break;

    case WAIT_RESULT_TIMEOUT:
        result = SemFail(timeout_errno);
        break;

    case WAIT_RESULT_CANCELED:
        TestCancelPoint(); // a pthread_cancel() request terminates the thread here
        result = SemFail(EINTR); // otherwise like a signal interruption, the caller retries
        break;

    case WAIT_RESULT_FAIL:
    default:
        result = SemFail(EINVAL);
        break;
    }

    return result;
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    int result;

    if (sem == nullptr)
    {
        result = SemFail(EINVAL);
    }
    else if (pshared != 0)
    {
        result = SemFail(ENOSYS); // process-shared semaphores are not supported
    }
    else if (value > static_cast<unsigned int>(SEM_VALUE_MAX))
    {
        result = SemFail(EINVAL);
    }
    else
    {
        sem->__handle = ConstructIn<PosixSemaphore>(sem->__mem, static_cast<uint32_t>(value),
                                                    static_cast<uint32_t>(SEM_VALUE_MAX));
        result = 0;
    }

    return result;
}

int sem_destroy(sem_t *sem)
{
    int result;

    if (!IsSemReady(sem))
    {
        result = SemFail(EINVAL);
    }
    else
    {
        SemOf(sem)->~PosixSemaphore();
        sem->__handle = nullptr;
        result = 0;
    }

    return result;
}

int sem_wait(sem_t *sem)
{
    TestCancelPoint();

    return IsSemReady(sem) ? SemWaitResult(SemOf(sem)->WaitEx(WAIT_INFINITE), EINVAL) : SemFail(EINVAL);
}

int sem_trywait(sem_t *sem)
{
    return IsSemReady(sem) ? SemWaitResult(SemOf(sem)->WaitEx(NO_WAIT), EAGAIN) : SemFail(EINVAL);
}

int sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
    int result;

    TestCancelPoint();

    if (!IsSemReady(sem) || !IsValidTimespec(abstime))
    {
        result = SemFail(EINVAL);
    }
    else
    {
        // a deadline in the past is a non-blocking attempt: an available token is still taken (POSIX)
        result = SemWaitResult(SemOf(sem)->WaitEx(TimespecToRelativeTimeout(abstime)), ETIMEDOUT);
    }

    return result;
}

int sem_clockwait(sem_t *sem, clockid_t clock_id, const struct timespec *abstime)
{
    return IsSupportedClock(clock_id) ? sem_timedwait(sem, abstime) : SemFail(EINVAL);
}

int sem_post(sem_t *sem)
{
    int result;

    if (!IsSemReady(sem))
    {
        result = SemFail(EINVAL);
    }
    else
    {
        // TrySignal() fails (without asserting) if the count is at its maximum and nobody is waiting
        result = SemOf(sem)->TrySignal() ? 0 : SemFail(EOVERFLOW);
    }

    return result;
}

int sem_getvalue(sem_t *sem, int *sval)
{
    int result;

    if (!IsSemReady(sem) || (sval == nullptr))
    {
        result = SemFail(EINVAL);
    }
    else
    {
        *sval  = static_cast<int>(SemOf(sem)->GetCount());
        result = 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Cooperative cancellation, cleanup handlers, pthread_getattr_np()
// -----------------------------------------------------------------------------
int pthread_cancel(pthread_t thread)
{
    int result = EINVAL;

    if (thread != nullptr)
    {
        bool alive = false;
        bool wake  = false;

        {
            const CsLock cs_;

            alive = thread->busy && !thread->finished && !thread->reclaimed;
            if (alive)
            {
                StoreRelease(&thread->cancel_pending, true);
                wake = thread->cancel_enabled;
            }
        }

        if (!alive)
        {
            result = ESRCH;
        }
        else
        {
            result = 0;

            // wake the target out of a Sleep() or a sync-object wait: it then reaches its next cancellation point
            // (the id of a user task is derived from the ITask address, no need to wait until the thread has started)
            const TId tid = TidOf(thread);
            if (wake && (tid != TID_NONE))
            {
                stk::SleepCancel(tid);
            }
        }
    }

    return result;
}

int pthread_setcancelstate(int state, int *oldstate)
{
    int result = EINVAL;
    ThreadCtrl *const ctrl = CurrentCtrlOrAdopt();

    if ((ctrl != nullptr) && ((state == PTHREAD_CANCEL_ENABLE) || (state == PTHREAD_CANCEL_DISABLE)))
    {
        if (oldstate != nullptr)
        {
            *oldstate = ctrl->cancel_enabled ? PTHREAD_CANCEL_ENABLE : PTHREAD_CANCEL_DISABLE;
        }

        ctrl->cancel_enabled = (state == PTHREAD_CANCEL_ENABLE);
        result = 0;
    }

    return result;
}

int pthread_setcanceltype(int type, int *oldtype)
{
    int result = EINVAL;

    if (type == PTHREAD_CANCEL_DEFERRED)
    {
        if (oldtype != nullptr)
        {
            *oldtype = PTHREAD_CANCEL_DEFERRED;
        }

        result = 0;
    }
    else if (type == PTHREAD_CANCEL_ASYNCHRONOUS)
    {
        result = ENOTSUP; // a running task can not be stopped at an arbitrary instruction
    }
    else
    {
        // invalid value: EINVAL
    }

    return result;
}

void pthread_testcancel(void)
{
    TestCancelPoint();
}

void pthread_cleanup_push(void (*routine)(void *), void *arg)
{
    ThreadCtrl *const ctrl = CurrentCtrlOrAdopt();

    STK_ASSERT(routine != nullptr);

    if ((ctrl != nullptr) && (routine != nullptr))
    {
        STK_ASSERT(ctrl->cleanup_count < STK_C_PTHREAD_CLEANUP_MAX); // API contract: nesting depth

        if (ctrl->cleanup_count < STK_C_PTHREAD_CLEANUP_MAX)
        {
            ctrl->cleanup[ctrl->cleanup_count].fn  = routine;
            ctrl->cleanup[ctrl->cleanup_count].arg = arg;
            ++ctrl->cleanup_count;
        }
    }
}

void pthread_cleanup_pop(int execute)
{
    ThreadCtrl *const ctrl = CurrentCtrl();

    if ((ctrl != nullptr) && (ctrl->cleanup_count != 0U))
    {
        --ctrl->cleanup_count;

        if (execute != 0)
        {
            ctrl->cleanup[ctrl->cleanup_count].fn(ctrl->cleanup[ctrl->cleanup_count].arg);
        }
    }
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr)
{
    int result = EINVAL;

    if ((thread != nullptr) && (attr != nullptr))
    {
        if (!thread->busy)
        {
            result = ESRCH;
        }
        else
        {
            result = pthread_attr_init(attr);

            if (result == 0)
            {
                attr->__ext_stack    = thread->stack;
                attr->__stack_bytes  = thread->task.GetStackSize() * sizeof(pthread_stk_word_t);
                attr->__detachstate  = thread->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
                attr->__priority     = static_cast<int>(thread->task.GetWeight());
                attr->__policy       = thread->task.GetPolicy();
                (void)thread->task.GetName(attr->__name, sizeof(attr->__name));
            }
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// <time.h>: clocks and sleeping
// -----------------------------------------------------------------------------
int clock_gettime(clockid_t clk_id, struct timespec *ts)
{
    int result = -1;

    if ((ts == nullptr) || !IsSupportedClock(clk_id))
    {
        errno = EINVAL;
    }
    else
    {
        TicksToTimespec(static_cast<int64_t>(stk::GetTicks()), ts); // one timeline for both clocks
        result = 0;
    }

    return result;
}

int clock_getres(clockid_t clk_id, struct timespec *res)
{
    int result = -1;

    if (!IsSupportedClock(clk_id))
    {
        errno = EINVAL;
    }
    else
    {
        if (res != nullptr)
        {
            TicksToTimespec(1, res);
        }

        result = 0;
    }

    return result;
}

int timespec_get(struct timespec *ts, int base)
{
    return ((base == TIME_UTC) && (clock_gettime(CLOCK_REALTIME, ts) == 0)) ? base : 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    int result = -1;

    if (!IsValidTimespec(req) || (req->tv_sec < 0))
    {
        errno = EINVAL;
    }
    else
    {
        TestCancelPoint();

        const int64_t left = SleepTicksInterruptible(
            RelativeTicksCeil(static_cast<int64_t>(req->tv_sec), static_cast<int64_t>(req->tv_nsec)));

        TestCancelPoint();

        if (left == 0LL)
        {
            result = 0;
        }
        else
        {
            if (rem != nullptr)
            {
                TicksToTimespec(left, rem);
            }

            errno = EINTR;
        }
    }

    return result;
}

int clock_nanosleep(clockid_t clk_id, int flags, const struct timespec *req, struct timespec *rem)
{
    int result = EINVAL;

    if (IsSupportedClock(clk_id) && IsValidTimespec(req) && (req->tv_sec >= 0))
    {
        if ((flags & TIMER_ABSTIME) != 0)
        {
            TestCancelPoint();

            // absolute deadline: sleep for what is left of it (the deadline may be in the past: no sleep)
            const Timeout left = TimespecToRelativeTimeout(req);
            const int64_t not_slept = (left != NO_WAIT) ? SleepTicksInterruptible(static_cast<int64_t>(left)) : 0LL;

            TestCancelPoint();

            result = (not_slept == 0LL) ? 0 : EINTR;
        }
        else
        {
            result = (nanosleep(req, rem) == 0) ? 0 : errno;
        }
    }

    return result;
}

int usleep(useconds_t usec)
{
    struct timespec ts;

    ts.tv_sec  = static_cast<time_t>(usec / 1000000UL);
    ts.tv_nsec = static_cast<long>((usec % 1000000UL) * 1000UL);

    return nanosleep(&ts, nullptr);
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec ts;
    struct timespec rem;
    unsigned int result = 0U;

    ts.tv_sec  = static_cast<time_t>(seconds);
    ts.tv_nsec = 0;

    if (nanosleep(&ts, &rem) != 0)
    {
        // unslept seconds, rounded up
        result = static_cast<unsigned int>(rem.tv_sec) + ((rem.tv_nsec != 0) ? 1U : 0U);
    }

    return result;
}

// -----------------------------------------------------------------------------
// <time.h>: POSIX timers on top of an application-owned stk::time::TimerHost
// -----------------------------------------------------------------------------
class PosixTimer final : public stk::time::Timer
{
public:
    explicit PosixTimer() : m_used(false), m_notify(nullptr), m_value()
    {
        m_value.sival_ptr = nullptr;
    }

    STK_VIRT_DTOR ~PosixTimer() = default;

    // handler task of the TimerHost
    void OnExpired(stk::time::TimerHost * /*host*/) override
    {
        if (m_notify != nullptr)
        {
            m_notify(m_value);
        }
    }

    bool         m_used;                    // slot is allocated (guarded by a critical section)
    void       (*m_notify)(union sigval);   // SIGEV_THREAD function, NULL for SIGEV_NONE
    union sigval m_value;

private:
    STK_NONCOPYABLE_CLASS(PosixTimer);
};

static PosixTimer               s_PosixTimers[STK_C_TIMERS_MAX];
static stk::time::TimerHost    *s_TimerHost = nullptr;

static inline PosixTimer *TimerOf(timer_t id)
{
    // timer_t is a pointer (default) or an integer wide enough for one (STK_C_NO_TIMER_T): a C-style cast covers both
    PosixTimer *const tmr = (PosixTimer *)(uintptr_t)id;
    bool valid = false;

    for (std::size_t i = 0U; i < STK_C_TIMERS_MAX; ++i)
    {
        valid = valid || ((&s_PosixTimers[i] == tmr) && tmr->m_used);
    }

    return valid ? tmr : nullptr;
}

static int TimerResultToErrno(stk::time::TimerHost::EResult res)
{
    int result = EINVAL;

    if ((res == stk::time::TimerHost::RES_OK) || (res == stk::time::TimerHost::RES_QUEUED) ||
        (res == stk::time::TimerHost::RES_INACTIVE))
    {
        result = 0;
    }
    else if (res == stk::time::TimerHost::RES_QUEUE_FULL)
    {
        result = EAGAIN;
    }
    else if (res == stk::time::TimerHost::RES_CANCELLED)
    {
        result = EINTR;
    }
    else
    {
        // RES_NOT_READY / RES_FAIL / ...: EINVAL
    }

    return result;
}

static inline uint32_t ClampTicks32(int64_t ticks)
{
    return (ticks > static_cast<int64_t>(0xFFFFFFFFLL)) ? 0xFFFFFFFFU : static_cast<uint32_t>(ticks);
}

// -----------------------------------------------------------------------------
// Wall time: gettimeofday(), time() (kernel timeline + offset set by stk_posix_set_wall_time())
// -----------------------------------------------------------------------------
static int64_t s_WallOffsetUs = 0; // wall time minus kernel time; 64-bit: accessed under a critical section only

// Microseconds since the Unix epoch (or since kernel start while the offset is 0)
static int64_t WallTimeUs()
{
    const int64_t mono_us = static_cast<int64_t>(stk::GetTicks()) * static_cast<int64_t>(GetTickResolution());
    const CsLock cs_;

    return mono_us + s_WallOffsetUs;
}

static void SetWallTimeUs(int64_t unix_us)
{
    const int64_t mono_us = static_cast<int64_t>(stk::GetTicks()) * static_cast<int64_t>(GetTickResolution());
    const CsLock cs_;

    s_WallOffsetUs = unix_us - mono_us;
}

void stk_posix_set_wall_time(int64_t unix_seconds)
{
    SetWallTimeUs(unix_seconds * 1000000LL);
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    int result = -1;

    STK_UNUSED(tz);

    if ((tv == nullptr) || (tv->tv_usec < 0) || (tv->tv_usec >= 1000000))
    {
        errno = EINVAL;
    }
    else
    {
        SetWallTimeUs((static_cast<int64_t>(tv->tv_sec) * 1000000LL) + static_cast<int64_t>(tv->tv_usec));
        result = 0;
    }

    return result;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    int result = -1;

    STK_UNUSED(tz);

    if (tv == nullptr)
    {
        errno = EINVAL;
    }
    else
    {
        const int64_t us = WallTimeUs();

        tv->tv_sec  = static_cast<time_t>(us / 1000000LL);
        tv->tv_usec = static_cast<suseconds_t>(us % 1000000LL);
        result = 0;
    }

    return result;
}

time_t time(time_t *tloc)
{
    const time_t now = static_cast<time_t>(WallTimeUs() / 1000000LL);

    if (tloc != nullptr)
    {
        *tloc = now;
    }

    return now;
}

// -----------------------------------------------------------------------------
// <unistd.h>: sysconf(), getpid()
// -----------------------------------------------------------------------------
pid_t getpid(void)
{
    return static_cast<pid_t>(STK_C_POSIX_PID);
}

pid_t getppid(void)
{
    return static_cast<pid_t>(STK_C_POSIX_PPID);
}

pid_t gettid(void)
{
    return static_cast<pid_t>(stk::GetTid());
}

int getpagesize(void)
{
    return static_cast<int>(STK_C_SYSCONF_PAGESIZE);
}

uid_t getuid(void)  { return static_cast<uid_t>(STK_C_POSIX_UID); }
uid_t geteuid(void) { return static_cast<uid_t>(STK_C_POSIX_UID); }
gid_t getgid(void)  { return static_cast<gid_t>(STK_C_POSIX_GID); }
gid_t getegid(void) { return static_cast<gid_t>(STK_C_POSIX_GID); }

long sysconf(int name)
{
    long result = -1L; // -1 without errno: the option/limit is not supported
    bool known  = true;

    switch (name)
    {
#ifdef _SC_CLK_TCK
    case _SC_CLK_TCK:
        result = static_cast<long>(1000000LL / static_cast<int64_t>(GetTickResolution()));
        break;
#endif
#ifdef _SC_NPROCESSORS_ONLN
    case _SC_NPROCESSORS_ONLN:
        result = STK_C_SYSCONF_NPROCESSORS;
        break;
#endif
#ifdef _SC_NPROCESSORS_CONF
    case _SC_NPROCESSORS_CONF:
        result = STK_C_SYSCONF_NPROCESSORS;
        break;
#endif
#ifdef _SC_PAGESIZE
    case _SC_PAGESIZE:
        result = STK_C_SYSCONF_PAGESIZE;
        break;
#endif
#if defined(_SC_PAGE_SIZE) && (!defined(_SC_PAGESIZE) || (_SC_PAGE_SIZE != _SC_PAGESIZE))
    case _SC_PAGE_SIZE:
        result = STK_C_SYSCONF_PAGESIZE;
        break;
#endif
#ifdef _SC_VERSION
    case _SC_VERSION:
        result = 200809L;
        break;
#endif
#ifdef _SC_THREAD_THREADS_MAX
    case _SC_THREAD_THREADS_MAX:
        result = static_cast<long>(STK_C_PTHREAD_MAX_THREADS);
        break;
#endif
#ifdef _SC_THREAD_KEYS_MAX
    case _SC_THREAD_KEYS_MAX:
        result = static_cast<long>(STK_C_PTHREAD_KEYS_MAX);
        break;
#endif
#ifdef _SC_THREAD_DESTRUCTOR_ITERATIONS
    case _SC_THREAD_DESTRUCTOR_ITERATIONS:
        result = PTHREAD_DESTRUCTOR_ITERATIONS;
        break;
#endif
#ifdef _SC_THREAD_STACK_MIN
    case _SC_THREAD_STACK_MIN:
        result = static_cast<long>(STK_C_PTHREAD_DEFAULT_STACK_WORDS * sizeof(pthread_stk_word_t)); // practical minimum
        break;
#endif
#ifdef _SC_SEM_VALUE_MAX
    case _SC_SEM_VALUE_MAX:
        result = SEM_VALUE_MAX;
        break;
#endif
#ifdef _SC_MQ_OPEN_MAX
    case _SC_MQ_OPEN_MAX:
        result = static_cast<long>(STK_C_MQ_MAX_DESCRIPTORS);
        break;
#endif
#ifdef _SC_MQ_PRIO_MAX
    case _SC_MQ_PRIO_MAX:
        result = static_cast<long>(MQ_PRIO_MAX);
        break;
#endif
#ifdef _SC_TIMER_MAX
    case _SC_TIMER_MAX:
        result = static_cast<long>(STK_C_TIMERS_MAX);
        break;
#endif
    // supported options: the POSIX.1-2008 option version
#ifdef _SC_THREADS
    case _SC_THREADS:
#endif
#ifdef _SC_TIMERS
    case _SC_TIMERS:
#endif
#ifdef _SC_SEMAPHORES
    case _SC_SEMAPHORES:
#endif
#ifdef _SC_MESSAGE_PASSING
    case _SC_MESSAGE_PASSING:
#endif
#ifdef _SC_BARRIERS
    case _SC_BARRIERS:
#endif
#ifdef _SC_SPIN_LOCKS
    case _SC_SPIN_LOCKS:
#endif
#ifdef _SC_READER_WRITER_LOCKS
    case _SC_READER_WRITER_LOCKS:
#endif
#ifdef _SC_CLOCK_SELECTION
    case _SC_CLOCK_SELECTION:
#endif
#ifdef _SC_MONOTONIC_CLOCK
    case _SC_MONOTONIC_CLOCK:
#endif
#ifdef _SC_THREAD_PRIORITY_SCHEDULING
    case _SC_THREAD_PRIORITY_SCHEDULING:
#endif
#ifdef _SC_THREAD_PRIO_INHERIT
    case _SC_THREAD_PRIO_INHERIT:
#endif
        result = 200809L;
        break;

    // options which STK does not provide: reported as unsupported (-1, errno unchanged)
#ifdef _SC_THREAD_PROCESS_SHARED
    case _SC_THREAD_PROCESS_SHARED:
#endif
#ifdef _SC_THREAD_PRIO_PROTECT
    case _SC_THREAD_PRIO_PROTECT:
#endif
#ifdef _SC_THREAD_ROBUST_PRIO_INHERIT
    case _SC_THREAD_ROBUST_PRIO_INHERIT:
#endif
#ifdef _SC_ASYNCHRONOUS_IO
    case _SC_ASYNCHRONOUS_IO:
#endif
#ifdef _SC_SHARED_MEMORY_OBJECTS
    case _SC_SHARED_MEMORY_OBJECTS:
#endif
#ifdef _SC_REALTIME_SIGNALS
    case _SC_REALTIME_SIGNALS:
#endif
        break;

    default:
        known = false;
        break;
    }

    if (!known)
    {
        errno = EINVAL;
    }

    return result;
}

void stk_posix_timer_bind_host(void *timer_host)
{
    s_TimerHost = static_cast<stk::time::TimerHost *>(timer_host);
}

int timer_create(clockid_t clk_id, struct sigevent *sevp, timer_t *timerid)
{
    int err = 0;

    if ((timerid == nullptr) || !IsSupportedClock(clk_id))
    {
        err = EINVAL;
    }
    else if ((sevp != nullptr) && (sevp->sigev_notify != SIGEV_NONE) && (sevp->sigev_notify != SIGEV_THREAD))
    {
        err = ENOTSUP; // no signals
    }
    else if ((sevp != nullptr) && (sevp->sigev_notify == SIGEV_THREAD) && (sevp->sigev_notify_function == nullptr))
    {
        err = EINVAL;
    }
    else if (s_TimerHost == nullptr)
    {
        err = EAGAIN; // stk_posix_timer_bind_host() has not been called
    }
    else
    {
        PosixTimer *slot = nullptr;

        {
            const CsLock cs_;

            for (std::size_t i = 0U; (i < STK_C_TIMERS_MAX) && (slot == nullptr); ++i)
            {
                if (!s_PosixTimers[i].m_used)
                {
                    slot = &s_PosixTimers[i];
                    slot->m_used = true;
                }
            }
        }

        if (slot == nullptr)
        {
            err = EAGAIN;
        }
        else
        {
            const bool thread_notify = (sevp != nullptr) && (sevp->sigev_notify == SIGEV_THREAD);

            slot->m_notify = thread_notify ? sevp->sigev_notify_function : nullptr;
            slot->m_value  = thread_notify ? sevp->sigev_value : slot->m_value;
            *timerid       = (timer_t)(uintptr_t)slot;
        }
    }

    if (err != 0)
    {
        errno = err;
    }

    return (err == 0) ? 0 : -1;
}

int timer_delete(timer_t timerid)
{
    PosixTimer *const tmr = TimerOf(timerid);
    int result = -1;

    if ((tmr == nullptr) || (s_TimerHost == nullptr))
    {
        errno = EINVAL;
    }
    else
    {
        // waits until a notification already started by a handler task has returned: the storage can be reused
        (void)s_TimerHost->StopSync(*tmr);

        {
            const CsLock cs_;

            tmr->m_notify = nullptr;
            tmr->m_used   = false;
        }

        result = 0;
    }

    return result;
}

int timer_gettime(timer_t timerid, struct itimerspec *curr_value)
{
    PosixTimer *const tmr = TimerOf(timerid);
    int err = EINVAL;

    if ((tmr != nullptr) && (curr_value != nullptr) && (s_TimerHost != nullptr))
    {
        stk::time::TimerHost::TimerInfo info;

        err = TimerResultToErrno(s_TimerHost->QuerySync(*tmr, info));

        if (err == 0)
        {
            TicksToTimespec(info.active ? static_cast<int64_t>(info.remaining) : 0LL, &curr_value->it_value);
            TicksToTimespec(static_cast<int64_t>(info.period), &curr_value->it_interval);
        }
    }

    if (err != 0)
    {
        errno = err;
    }

    return (err == 0) ? 0 : -1;
}

int timer_settime(timer_t timerid, int flags, const struct itimerspec *new_value, struct itimerspec *old_value)
{
    PosixTimer *const tmr = TimerOf(timerid);
    int err = EINVAL;

    if ((tmr != nullptr) && (new_value != nullptr) && (s_TimerHost != nullptr) &&
        IsValidTimespec(&new_value->it_value) && IsValidTimespec(&new_value->it_interval) &&
        (new_value->it_value.tv_sec >= 0) && (new_value->it_interval.tv_sec >= 0))
    {
        err = 0;

        if (old_value != nullptr)
        {
            err = (timer_gettime(timerid, old_value) == 0) ? 0 : errno;
        }

        if (err == 0)
        {
            const bool disarm = (new_value->it_value.tv_sec == 0) && (new_value->it_value.tv_nsec == 0);

            if (disarm)
            {
                err = TimerResultToErrno(s_TimerHost->StopSync(*tmr));
            }
            else
            {
                // first expiration: relative, or what is left until the absolute time (at least one tick)
                int64_t delay = ((flags & TIMER_ABSTIME) != 0) ?
                    static_cast<int64_t>(TimespecToRelativeTimeout(&new_value->it_value)) :
                    RelativeTicksCeil(static_cast<int64_t>(new_value->it_value.tv_sec),
                                      static_cast<int64_t>(new_value->it_value.tv_nsec));
                if (delay < 1LL)
                {
                    delay = 1LL;
                }

                const int64_t period = RelativeTicksCeil(static_cast<int64_t>(new_value->it_interval.tv_sec),
                                                         static_cast<int64_t>(new_value->it_interval.tv_nsec));

                err = TimerResultToErrno(s_TimerHost->RestartSync(*tmr, ClampTicks32(delay), ClampTicks32(period)));
            }
        }
    }

    if (err != 0)
    {
        errno = err;
    }

    return (err == 0) ? 0 : -1;
}

int timer_getoverrun(timer_t timerid)
{
    int result = 0;

    if (TimerOf(timerid) == nullptr)
    {
        errno = EINVAL;
        result = -1;
    }

    return result;
}

// -----------------------------------------------------------------------------
// <mqueue.h>: named message queues (priority ordered) on Mutex + ConditionVariable
// -----------------------------------------------------------------------------
struct MqMsgHdr
{
    uint32_t len;
    uint32_t prio;
};

struct MqQueue
{
    explicit MqQueue()
        : used(false), unlinked(false), opens(0U), maxmsg(0), msgsize(0), stride(0U), count(0U), buf(nullptr)
    {
        name[0] = '\0';
    }

    bool              used;
    bool              unlinked; // name removed, destroyed with the last descriptor
    uint32_t          opens;    // open descriptors
    char              name[STK_C_MQ_NAME_MAX];
    long              maxmsg;
    long              msgsize;
    std::size_t       stride;   // bytes per slot: header + msgsize
    std::size_t       count;
    uint8_t          *buf;      // maxmsg slots, sorted: slot 0 = next message to receive
    sync::Mutex       mtx;      // protects count/buf
    sync::ConditionVariable not_empty;
    sync::ConditionVariable not_full;

private:
    STK_NONCOPYABLE_CLASS(MqQueue);
};

struct stk_mq_desc
{
    bool     used;
    int      flags; // O_RDONLY / O_WRONLY / O_RDWR | O_NONBLOCK
    MqQueue *queue;
};

static MqQueue     s_MqQueues[STK_C_MQ_MAX_QUEUES];
static stk_mq_desc s_MqDescs[STK_C_MQ_MAX_DESCRIPTORS];

#define STK_MQ_FAILED (reinterpret_cast<mqd_t>(static_cast<intptr_t>(-1)))

static mqd_t MqFailOpen(int err)
{
    errno = err;
    return STK_MQ_FAILED;
}

static int MqFail(int err)
{
    errno = err;
    return -1;
}

static bool IsMqDesc(mqd_t d)
{
    bool valid = false;

    for (std::size_t i = 0U; i < STK_C_MQ_MAX_DESCRIPTORS; ++i)
    {
        valid = valid || ((&s_MqDescs[i] == d) && d->used);
    }

    return valid;
}

static inline MqMsgHdr *MqSlot(MqQueue *q, std::size_t idx)
{
    return reinterpret_cast<MqMsgHdr *>(q->buf + (idx * q->stride));
}

// Frees a queue which is unlinked and no longer open (\a q is already detached under the critical section)
static void MqDestroy(uint8_t *buf)
{
    free(buf);
}

mqd_t mq_open(const char *name, int oflag, ...)
{
    long maxmsg  = STK_C_MQ_DEFAULT_MAXMSG;
    long msgsize = STK_C_MQ_DEFAULT_MSGSIZE;
    const bool create = ((oflag & O_CREAT) != 0);

    if ((name == nullptr) || (name[0] == '\0'))
    {
        return MqFailOpen(EINVAL);
    }

    if (NameLen(name) >= STK_C_MQ_NAME_MAX)
    {
        return MqFailOpen(ENAMETOOLONG);
    }

    if (create)
    {
        va_list ap;
        va_start(ap, oflag);
        (void)va_arg(ap, unsigned int); // mode_t: ignored
        const struct mq_attr *const attr = va_arg(ap, const struct mq_attr *);
        va_end(ap);

        if (attr != nullptr)
        {
            maxmsg  = attr->mq_maxmsg;
            msgsize = attr->mq_msgsize;
        }

        if ((maxmsg <= 0) || (msgsize <= 0))
        {
            return MqFailOpen(EINVAL);
        }
    }

    // storage is allocated before taking the critical section (freed below if the queue already exists)
    uint8_t *newbuf = nullptr;
    const std::size_t stride = sizeof(MqMsgHdr) + static_cast<std::size_t>(msgsize);

    if (create)
    {
        newbuf = static_cast<uint8_t *>(malloc(stride * static_cast<std::size_t>(maxmsg)));
        if (newbuf == nullptr)
        {
            return MqFailOpen(ENOMEM);
        }
    }

    int         err  = 0;
    stk_mq_desc *desc = nullptr;
    bool        newbuf_used = false;

    {
        const CsLock cs_;

        MqQueue *q = nullptr;

        for (std::size_t i = 0U; i < STK_C_MQ_MAX_QUEUES; ++i)
        {
            if (s_MqQueues[i].used && !s_MqQueues[i].unlinked && (std::strcmp(s_MqQueues[i].name, name) == 0))
            {
                q = &s_MqQueues[i];
            }
        }

        if ((q != nullptr) && create && ((oflag & O_EXCL) != 0))
        {
            err = EEXIST;
        }
        else if ((q == nullptr) && !create)
        {
            err = ENOENT;
        }
        else
        {
            for (std::size_t i = 0U; (i < STK_C_MQ_MAX_DESCRIPTORS) && (desc == nullptr); ++i)
            {
                if (!s_MqDescs[i].used)
                {
                    desc = &s_MqDescs[i];
                }
            }

            if (desc == nullptr)
            {
                err = EMFILE;
            }
            else if (q == nullptr)
            {
                for (std::size_t i = 0U; (i < STK_C_MQ_MAX_QUEUES) && (q == nullptr); ++i)
                {
                    if (!s_MqQueues[i].used)
                    {
                        q = &s_MqQueues[i];
                    }
                }

                if (q == nullptr)
                {
                    err = ENOSPC;
                }
                else
                {
                    q->used     = true;
                    q->unlinked = false;
                    q->opens    = 0U;
                    q->maxmsg   = maxmsg;
                    q->msgsize  = msgsize;
                    q->stride   = stride;
                    q->count    = 0U;
                    q->buf      = newbuf;
                    newbuf_used = true;
                    (void)NameCopy(q->name, sizeof(q->name), name);
                }
            }
            else
            {
                // existing queue: attributes of the creator apply
            }

            if (err == 0)
            {
                ++q->opens;
                desc->used  = true;
                desc->flags = oflag & (O_ACCMODE | O_NONBLOCK);
                desc->queue = q;
            }
        }
    }

    if (!newbuf_used)
    {
        free(newbuf);
    }

    return (err == 0) ? desc : MqFailOpen(err);
}

int mq_close(mqd_t mqdes)
{
    int result = -1;
    uint8_t *to_free = nullptr;

    {
        const CsLock cs_;

        if (!IsMqDesc(mqdes))
        {
            errno = EBADF;
        }
        else
        {
            MqQueue *const q = mqdes->queue;

            mqdes->used  = false;
            mqdes->queue = nullptr;
            --q->opens;

            if (q->unlinked && (q->opens == 0U))
            {
                to_free  = q->buf;
                q->buf   = nullptr;
                q->used  = false;
            }

            result = 0;
        }
    }

    MqDestroy(to_free);

    return result;
}

int mq_unlink(const char *name)
{
    int result = -1;
    uint8_t *to_free = nullptr;

    if (name == nullptr)
    {
        errno = EINVAL;
    }
    else
    {
        const CsLock cs_;

        MqQueue *q = nullptr;

        for (std::size_t i = 0U; i < STK_C_MQ_MAX_QUEUES; ++i)
        {
            if (s_MqQueues[i].used && !s_MqQueues[i].unlinked && (std::strcmp(s_MqQueues[i].name, name) == 0))
            {
                q = &s_MqQueues[i];
            }
        }

        if (q == nullptr)
        {
            errno = ENOENT;
        }
        else
        {
            q->unlinked = true;
            q->name[0]  = '\0';

            if (q->opens == 0U)
            {
                to_free = q->buf;
                q->buf  = nullptr;
                q->used = false;
            }

            result = 0;
        }
    }

    // outside the critical section (the block above is scoped), a destroyed queue has no users left
    MqDestroy(to_free);

    return result;
}

// Common send path. \a abstime: absolute deadline or NULL (wait forever); O_NONBLOCK descriptors never wait.
static int MqSendImpl(mqd_t mqdes, const char *msg_ptr, std::size_t msg_len, unsigned int msg_prio,
                      const struct timespec *abstime)
{
    if (!IsMqDesc(mqdes) || ((mqdes->flags & O_ACCMODE) == O_RDONLY))
    {
        return MqFail(EBADF);
    }

    MqQueue *const q = mqdes->queue;

    if ((msg_ptr == nullptr) || (msg_prio >= MQ_PRIO_MAX) || ((abstime != nullptr) && !IsValidTimespec(abstime)))
    {
        return MqFail(EINVAL);
    }

    if (msg_len > static_cast<std::size_t>(q->msgsize))
    {
        return MqFail(EMSGSIZE);
    }

    const bool nonblock = ((mqdes->flags & O_NONBLOCK) != 0);

    if (!nonblock)
    {
        TestCancelPoint();
    }

    int  err      = 0;
    bool canceled = false;

    q->mtx.Lock();

    while (static_cast<long>(q->count) >= q->maxmsg)
    {
        if (nonblock)
        {
            err = EAGAIN;
            break;
        }

        const Timeout timeout = (abstime != nullptr) ? TimespecToRelativeTimeout(abstime) : WAIT_INFINITE;
        if ((abstime != nullptr) && (timeout == NO_WAIT))
        {
            err = ETIMEDOUT;
            break;
        }

        const EWaitResult wr = q->not_full.WaitEx(q->mtx, timeout);

        if (wr == WAIT_RESULT_FAIL)
        {
            err = EINVAL;
            break;
        }

        if ((wr == WAIT_RESULT_CANCELED) && CancelPending())
        {
            canceled = true;
            break;
        }

        if ((wr == WAIT_RESULT_TIMEOUT) && (static_cast<long>(q->count) >= q->maxmsg))
        {
            err = ETIMEDOUT;
            break;
        }
    }

    if ((err == 0) && !canceled)
    {
        // insert after the last message with a priority >= msg_prio: highest priority first, FIFO inside a priority
        std::size_t pos = q->count;

        while ((pos > 0U) && (MqSlot(q, pos - 1U)->prio < msg_prio))
        {
            --pos;
        }

        if (pos < q->count)
        {
            std::memmove(q->buf + ((pos + 1U) * q->stride), q->buf + (pos * q->stride), (q->count - pos) * q->stride);
        }

        MqMsgHdr *const hdr = MqSlot(q, pos);

        hdr->len  = static_cast<uint32_t>(msg_len);
        hdr->prio = msg_prio;
        std::memcpy(reinterpret_cast<uint8_t *>(hdr) + sizeof(MqMsgHdr), msg_ptr, msg_len);

        ++q->count;
        q->not_empty.NotifyOne();
    }

    q->mtx.Unlock();

    if (canceled)
    {
        TestCancelPoint(); // terminates the thread
        err = EINTR;
    }

    return (err == 0) ? 0 : MqFail(err);
}

static ssize_t MqReceiveImpl(mqd_t mqdes, char *msg_ptr, std::size_t msg_len, unsigned int *msg_prio,
                             const struct timespec *abstime)
{
    if (!IsMqDesc(mqdes) || ((mqdes->flags & O_ACCMODE) == O_WRONLY))
    {
        return MqFail(EBADF);
    }

    MqQueue *const q = mqdes->queue;

    if ((msg_ptr == nullptr) || ((abstime != nullptr) && !IsValidTimespec(abstime)))
    {
        return MqFail(EINVAL);
    }

    if (msg_len < static_cast<std::size_t>(q->msgsize))
    {
        return MqFail(EMSGSIZE); // POSIX: the buffer must hold the largest possible message
    }

    const bool nonblock = ((mqdes->flags & O_NONBLOCK) != 0);

    if (!nonblock)
    {
        TestCancelPoint();
    }

    int     err      = 0;
    bool    canceled = false;
    ssize_t result   = -1;

    q->mtx.Lock();

    while (q->count == 0U)
    {
        if (nonblock)
        {
            err = EAGAIN;
            break;
        }

        const Timeout timeout = (abstime != nullptr) ? TimespecToRelativeTimeout(abstime) : WAIT_INFINITE;
        if ((abstime != nullptr) && (timeout == NO_WAIT))
        {
            err = ETIMEDOUT;
            break;
        }

        const EWaitResult wr = q->not_empty.WaitEx(q->mtx, timeout);

        if (wr == WAIT_RESULT_FAIL)
        {
            err = EINVAL;
            break;
        }

        if ((wr == WAIT_RESULT_CANCELED) && CancelPending())
        {
            canceled = true;
            break;
        }

        if ((wr == WAIT_RESULT_TIMEOUT) && (q->count == 0U))
        {
            err = ETIMEDOUT;
            break;
        }
    }

    if ((err == 0) && !canceled)
    {
        const MqMsgHdr *const hdr = MqSlot(q, 0U);

        result = static_cast<ssize_t>(hdr->len);

        if (msg_prio != nullptr)
        {
            *msg_prio = hdr->prio;
        }

        std::memcpy(msg_ptr, reinterpret_cast<const uint8_t *>(hdr) + sizeof(MqMsgHdr), hdr->len);

        --q->count;
        if (q->count != 0U)
        {
            std::memmove(q->buf, q->buf + q->stride, q->count * q->stride);
        }

        q->not_full.NotifyOne();
    }

    q->mtx.Unlock();

    if (canceled)
    {
        TestCancelPoint(); // terminates the thread
        err = EINTR;
    }

    if (err != 0)
    {
        errno  = err;
        result = -1;
    }

    return result;
}

int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned int msg_prio)
{
    return MqSendImpl(mqdes, msg_ptr, msg_len, msg_prio, nullptr);
}

int mq_timedsend(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned int msg_prio,
                 const struct timespec *abstime)
{
    return (abstime != nullptr) ? MqSendImpl(mqdes, msg_ptr, msg_len, msg_prio, abstime) : MqFail(EINVAL);
}

ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned int *msg_prio)
{
    return MqReceiveImpl(mqdes, msg_ptr, msg_len, msg_prio, nullptr);
}

ssize_t mq_timedreceive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned int *msg_prio,
                        const struct timespec *abstime)
{
    return (abstime != nullptr) ? MqReceiveImpl(mqdes, msg_ptr, msg_len, msg_prio, abstime) : MqFail(EINVAL);
}

int mq_getattr(mqd_t mqdes, struct mq_attr *mqstat)
{
    int result = -1;

    if (!IsMqDesc(mqdes))
    {
        errno = EBADF;
    }
    else if (mqstat == nullptr)
    {
        errno = EINVAL;
    }
    else
    {
        const CsLock cs_;

        mqstat->mq_flags   = ((mqdes->flags & O_NONBLOCK) != 0) ? O_NONBLOCK : 0;
        mqstat->mq_maxmsg  = mqdes->queue->maxmsg;
        mqstat->mq_msgsize = mqdes->queue->msgsize;
        mqstat->mq_curmsgs = static_cast<long>(mqdes->queue->count);
        result = 0;
    }

    return result;
}

int mq_setattr(mqd_t mqdes, const struct mq_attr *mqstat, struct mq_attr *omqstat)
{
    int result = -1;

    if (!IsMqDesc(mqdes))
    {
        errno = EBADF;
    }
    else if (mqstat == nullptr)
    {
        errno = EINVAL;
    }
    else
    {
        if (omqstat != nullptr)
        {
            (void)mq_getattr(mqdes, omqstat);
        }

        const CsLock cs_;

        mqdes->flags = (mqdes->flags & O_ACCMODE) | (((mqstat->mq_flags & O_NONBLOCK) != 0) ? O_NONBLOCK : 0);
        result = 0;
    }

    return result;
}

int mq_notify(mqd_t mqdes, const struct sigevent *notification)
{
    STK_UNUSED(mqdes);
    STK_UNUSED(notification);

    return MqFail(ENOSYS); // no signals
}

// -----------------------------------------------------------------------------
// pthread_kill(): existence check, SIGKILL / SIGTERM; fork/signal-mask stubs
// -----------------------------------------------------------------------------
int pthread_kill(pthread_t thread, int sig)
{
    int result = EINVAL;

    if ((thread != nullptr) && (sig >= 0))
    {
        const bool alive = thread->busy && !thread->finished && !thread->reclaimed;

        if (!alive)
        {
            result = ESRCH;
        }
        else if (sig == 0)
        {
            result = 0; // existence check only
        }
        else if ((sig != SIGKILL) && (sig != SIGTERM))
        {
            result = ENOSYS; // no signal delivery
        }
        else if (thread == CurrentCtrl())
        {
            // the calling thread ends itself: SIGTERM like pthread_exit(), SIGKILL cannot be caught: no handlers
            if (sig == SIGTERM)
            {
                pthread_exit(PTHREAD_CANCELED);
            }

            TerminateSelf(thread, PTHREAD_CANCELED);
        }
        else if (thread->adopted || (s_BoundKernel == nullptr))
        {
            result = thread->adopted ? ENOTSUP : EINVAL; // an adopted task has no ITask the kernel could remove
        }
        else
        {
            bool terminate = false;

            {
                const CsLock cs_;

                if (!thread->finished)
                {
                    thread->retval   = PTHREAD_CANCELED;
                    thread->finished = true;
                    terminate = true;
                }
            }

            if (terminate)
            {
                MaybeReap(thread); // detached: the reaper waits for done_event, which OnExit() signals on removal

                s_BoundKernel->ScheduleTaskRemoval(&thread->task);
            }

            result = 0;
        }
    }

    return result;
}

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    STK_UNUSED(prepare);
    STK_UNUSED(parent);
    STK_UNUSED(child);

    return 0; // there is no fork()
}

#if !STK_C_DEFINE_SIGEVENT
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    STK_UNUSED(how);
    STK_UNUSED(set);

    if (oldset != nullptr)
    {
        std::memset(oldset, 0, sizeof(*oldset)); // no signals are ever pending or blocked
    }

    return 0;
}
#endif

// -----------------------------------------------------------------------------
// <sched.h>: process-level calls, the "process" is the calling thread
// -----------------------------------------------------------------------------
static pthread_stk_ctrl_t *SchedTarget(pid_t pid)
{
    pthread_stk_ctrl_t *ctrl = nullptr;

    if ((pid == 0) || (pid == static_cast<pid_t>(STK_C_POSIX_PID)))
    {
        ctrl = CurrentCtrlOrAdopt();
    }

    if (ctrl == nullptr)
    {
        errno = ESRCH;
    }

    return ctrl;
}

int sched_getscheduler(pid_t pid)
{
    const pthread_stk_ctrl_t *const ctrl = SchedTarget(pid);

    return (ctrl != nullptr) ? ctrl->task.GetPolicy() : -1;
}

int sched_getparam(pid_t pid, struct sched_param *param)
{
    int result = -1;
    const pthread_stk_ctrl_t *const ctrl = SchedTarget(pid);

    if (ctrl != nullptr)
    {
        if (param == nullptr)
        {
            errno = EINVAL;
        }
        else
        {
            param->sched_priority = static_cast<int>(ctrl->task.GetWeight());
            result = 0;
        }
    }

    return result;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
    int result = -1;
    pthread_stk_ctrl_t *const ctrl = SchedTarget(pid);

    if (ctrl != nullptr)
    {
        const int old_policy = ctrl->task.GetPolicy();
        const int err        = pthread_setschedparam(ctrl, policy, param);

        if (err == 0)
        {
            result = old_policy; // POSIX: the previous policy
        }
        else
        {
            errno = err;
        }
    }

    return result;
}

int sched_setparam(pid_t pid, const struct sched_param *param)
{
    int result = -1;
    pthread_stk_ctrl_t *const ctrl = SchedTarget(pid);

    if (ctrl != nullptr)
    {
        const int err = pthread_setschedparam(ctrl, ctrl->task.GetPolicy(), param);

        if (err == 0)
        {
            result = 0;
        }
        else
        {
            errno = err;
        }
    }

    return result;
}

int sched_rr_get_interval(pid_t pid, struct timespec *tp)
{
    int result = -1;

    if (SchedTarget(pid) != nullptr)
    {
        if (tp == nullptr)
        {
            errno = EINVAL;
        }
        else
        {
            TicksToTimespec(1, tp); // time slice of the kernel: one tick (GetTickResolution())
            result = 0;
        }
    }

    return result;
}

// =============================================================================
} // extern "C"
// =============================================================================
