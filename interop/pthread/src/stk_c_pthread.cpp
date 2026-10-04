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

#include "stk_c.h"
#include "stk_c_memory.h"
#include "stk_c_pthread.h"

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
 *      Rule 5-2-7/8/9 : reinterpret_cast in ToKernel() and the stack pool bootstrap.
 *      Rule 7-3-4   : using-directives (namespace stk / stk::pthread).
 *      Rule 18-4-1  : malloc()/free() for non-default stack sizes, placement new for sync objects.
 */

// RAII critical section (same session semantics as stk_critical_section_enter_ex(DEFAULT)).
typedef stk::hw::CriticalSection::ScopedLock CsLock;

// Private malloc/free declarations (mirrors stk_c_memory.cpp): overcomes absence of
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

// -----------------------------------------------------------------------------
// PthreadTask: the kernel-facing task object embedded in every thread control block.
// -----------------------------------------------------------------------------
class PthreadTask final : public ITask
{
public:
    explicit PthreadTask() : m_ctrl(nullptr), m_stack(nullptr), m_stack_size(0U), m_mode(ACCESS_USER)
    {}

    /*! \brief Destructor.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR ~PthreadTask() = default;

    void Initialize(pthread_stk_ctrl_t *ctrl, stk_word_t *stack, size_t stack_size, EAccessMode mode)
    {
        m_ctrl       = ctrl;
        m_stack      = stack;
        m_stack_size = stack_size;
        m_mode       = mode;
    }

    // ITask
    EAccessMode GetAccessMode() const override { return m_mode; }
    int32_t GetWeight()         const override { return DEFAULT_WEIGHT; }
    const char *GetTraceName()  const override { return "pthread"; }

    // IStackMemory
    const Word *GetStack() const override { return m_stack; }
    size_t GetStackSize()  const override { return m_stack_size; }

private:
    STK_NONCOPYABLE_CLASS(PthreadTask);

    void Run() override;    // thread trampoline, see below
    void OnExit() override; // sets pthread_stk_ctrl_t::exited

    pthread_stk_ctrl_t *m_ctrl;
    stk_word_t         *m_stack;
    size_t              m_stack_size;
    EAccessMode         m_mode;
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
        exited          = false;
        adopted         = false;
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
    bool             finished;   // start_routine returned / pthread_exit() called
    bool             detached;
    bool             joined;
    bool             reclaimed;  // resources already freed or handed to the reaper
    bool             exited;     // kernel invoked PthreadTask::OnExit(): task is gone, stack no longer in use
    bool             adopted;    // control block wraps a foreign task (e.g. bootstrap task), see CurrentCtrlOrAdopt()

    stk::pthread::PthreadTask task;
    void            *(*start_routine)(void *);
    void            *arg;
    void            *retval;

    stk_word_t      *stack;
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
    static_cast<std::size_t>(STK_C_PTHREAD_DEFAULT_STACK_WORDS) * sizeof(stk_word_t);

// pthread_once_t::__state values
constexpr int kOnceRunning = 1;
constexpr int kOnceDone    = 2;

static ThreadCtrl s_Threads[STK_C_PTHREAD_MAX_THREADS];

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

// Same cast the C layer (stk_c.cpp) uses: an stk_kernel_t* *is* the IKernel object.
static inline IKernel *ToKernel(stk_kernel_t *k)
{
    return reinterpret_cast<IKernel *>(reinterpret_cast<void *>(k));
}

// Acquire/release accessors used by every lazy-init fast path below. The unlocked
// "is it initialized yet?" check of a double-checked pattern must be an *acquire*
// load, and the publishing store a *release* store; otherwise on a multi-core
// configuration (STK_C_CPU_COUNT > 1) another core can observe the non-NULL handle
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
static stk_kernel_t *s_BoundKernel = nullptr;

// -----------------------------------------------------------------------------
// Static stack pool (zero heap) for default-size stacks
// -----------------------------------------------------------------------------
STK_BLOCKPOOL_STORAGE_DECL(s_PthreadStackStorage, STK_C_PTHREAD_MAX_THREADS, kDefaultStackBytes);
static stk_blockpool_t *s_StackPool = nullptr;

static stk_blockpool_t *EnsureStackPool()
{
    stk_blockpool_t *pool = LoadAcquire(&s_StackPool);
    if (pool == nullptr)
    {
        const CsLock cs_;

        pool = s_StackPool;
        if (pool == nullptr)
        {
            pool = stk_blockpool_create_static(
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

static sync::Semaphore *s_ReapSem = nullptr;
static struct {
    alignas(sync::Semaphore) uint8_t data[sizeof(sync::Semaphore)];
} s_ReapSemBuf;
static stk_task_t  *s_ReaperTask = nullptr;
static stk_word_t   s_ReaperStack[STK_C_PTHREAD_REAPER_STACK_WORDS];

static void ReleaseThreadResources(ThreadCtrl *ctrl);
static void WaitForTaskExit(ThreadCtrl *ctrl);
static void ReaperEntry(void *arg);

static void EnsureReaper()
{
    if (s_ReaperTask == nullptr)
    {
        STK_C_ASSERT(s_BoundKernel != nullptr);

        if (s_ReapSem == nullptr)
        {
            s_ReapSem = ConstructIn<sync::Semaphore>(s_ReapSemBuf, 0U);
        }

        s_ReaperTask = stk_task_create_user(ReaperEntry, nullptr, s_ReaperStack,
                                            STK_C_PTHREAD_REAPER_STACK_WORDS);
        if (s_ReaperTask != nullptr)
        {
            stk_task_set_name(s_ReaperTask, "pthread_reaper");
            stk_kernel_add_task(s_BoundKernel, s_ReaperTask);
        }
    }
}

static void EnqueueReap(ThreadCtrl *ctrl)
{
    STK_C_ASSERT(s_ReaperTask != nullptr);

    const CsLock cs_;

    STK_C_ASSERT(s_ReapCount < STK_C_PTHREAD_MAX_THREADS);
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

static void ReaperEntry(void * /*arg*/)
{
    for (;;)
    {
        STK_C_ASSERT(s_ReapSem != nullptr);
        (void)s_ReapSem->Wait(); // WAIT_INFINITE

        ThreadCtrl *const ctrl = DequeueReap();
        if (ctrl != nullptr)
        {
            WaitForTaskExit(ctrl);
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

// Wait until the kernel has invoked PthreadTask::OnExit() for this thread, i.e. the task
// has been fully torn down and its stack is no longer in use, so it is safe to free
// the stack and recycle the control block. The flag is exact: no task-list scan, no
// large on-stack buffer, and no ABA hazard from recycled task slots.
//
// Waiting is a 1 ms sleep-poll on purpose: OnExit() runs in kernel context where
// blocking/signalling primitives must not be used, and the wait is normally a few ticks.
static void WaitForTaskExit(ThreadCtrl *ctrl)
{
    bool exited = false;

    while (!exited)
    {
        {
            const CsLock cs_;

            exited = ctrl->exited;
        }

        if (!exited)
        {
            SleepMs(1);
        }
    }
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
            (void)stk_blockpool_free(s_StackPool, ctrl->stack);
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
            stk_blockpool_t *const pool = EnsureStackPool();
            if (pool != nullptr)
            {
                blk = stk_blockpool_try_alloc(pool);
            }
        }

        if (blk != nullptr)
        {
            ctrl->stack           = static_cast<stk_word_t *>(blk);
            ctrl->stack_owned     = true;
            ctrl->stack_from_pool = true;
        }
        else
        {
            ctrl->stack           = static_cast<stk_word_t *>(malloc(stack_bytes));
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

    (void)ctrl->done_event->Set();
    MaybeReap(ctrl);

    // Natural return: STK tears the task down and calls OnExit(). A joiner (or the
    // reaper, for detached threads) reclaims *our* resources (stack, control block)
    // only after OnExit() has set ctrl->exited.
}

void PthreadTask::OnExit()
{
    // Kernel context: no blocking calls. This is the last time the kernel touches
    // this task object; after the flag is set the control block may be recycled.
    const CsLock cs_;
    m_ctrl->exited = true;
}

// -----------------------------------------------------------------------------
// Absolute-deadline -> relative-timeout helper
//
// STK has no wall clock; abstime is interpreted as a point on the same timeline as
// stk::GetTimeNowMs() (see the "timedwait limitation" note in stk_c_pthread.h).
// -----------------------------------------------------------------------------
static Timeout TimespecToRelativeTimeout(const struct timespec *abstime)
{
    const stk_time_t now_ms    = static_cast<stk_time_t>(GetTimeNowMs());
    const stk_time_t target_ms = (static_cast<stk_time_t>(abstime->tv_sec) * 1000LL) +
                                 (static_cast<stk_time_t>(abstime->tv_nsec) / 1000000LL);

    stk_time_t rel_ms = target_ms - now_ms;
    if (rel_ms < 0LL)
    {
        rel_ms = 0LL;
    }
    else
    if (rel_ms > static_cast<stk_time_t>(INT32_MAX))
    {
        rel_ms = static_cast<stk_time_t>(INT32_MAX);
    }

    return GetTicksFromMsClampedToTimeout(static_cast<Timeout>(rel_ms));
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
void stk_pthread_bind_kernel(stk_kernel_t *kernel)
{
    STK_C_ASSERT(kernel != nullptr);

    s_BoundKernel = kernel;

    EnsureReaper();
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

    if ((attr != nullptr) && (stackaddr != nullptr) && (stacksize >= sizeof(stk_word_t)))
    {
        if ((reinterpret_cast<uintptr_t>(stackaddr) & STK_ALIGN_MASK) == 0U)
        {
            attr->__ext_stack   = static_cast<stk_word_t *>(stackaddr);
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
    STK_C_ASSERT(s_BoundKernel != nullptr);
    STK_C_ASSERT(thread != nullptr);
    STK_C_ASSERT(start_routine != nullptr);

    int result = 0;

    if ((s_BoundKernel == nullptr) || (thread == nullptr) || (start_routine == nullptr))
    {
        result = EINVAL;
    }
    else
    {
        pthread_stk_ctrl_t *const ctrl = AcquireThreadSlot();

        if (ctrl == nullptr)
        {
            result = EAGAIN;
        }
        else
        {
            const size_t stack_bytes = ((attr != nullptr) && (attr->__stack_bytes != 0U)) ?
                attr->__stack_bytes : kDefaultStackBytes;
            const uint32_t stack_words =
                static_cast<uint32_t>((stack_bytes + sizeof(stk_word_t) - 1U) / sizeof(stk_word_t));

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

                ctrl->task.Initialize(ctrl, ctrl->stack, stack_words,
                                      privileged ? stk::ACCESS_PRIVILEGED : stk::ACCESS_USER);

                ToKernel(s_BoundKernel)->AddTask(&ctrl->task);

                *thread = ctrl;
            }
        }
    }

    return result;
}

int pthread_join(pthread_t thread, void **retval)
{
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
            (void)ctrl->done_event->Wait(); // WAIT_INFINITE

            if (retval != nullptr)
            {
                *retval = ctrl->retval;
            }

            WaitForTaskExit(ctrl);
            ReleaseThreadResources(ctrl);
        }
    }

    return result;
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

    return result;
}

void pthread_exit(void *retval)
{
    pthread_stk_ctrl_t *const ctrl = CurrentCtrl();

    if (ctrl != nullptr)
    {
        RunKeyDestructors(ctrl);

        // An adopted (foreign) task has no ITask we could ask the kernel to remove, so for
        // it pthread_exit() only runs the destructors and then parks (see below).
        if (!ctrl->adopted)
        {
            {
                const CsLock cs_;

                ctrl->retval   = retval;
                ctrl->finished = true;
            }

            (void)ctrl->done_event->Set();
            MaybeReap(ctrl); // reaper/joiner will wait for ctrl->exited before freeing anything

            STK_C_ASSERT(s_BoundKernel != nullptr);
            ToKernel(s_BoundKernel)->ScheduleTaskRemoval(&ctrl->task);
        }
    }

    // Park here instead of falling back into caller code that isn't expecting to
    // regain control (the removal above takes effect on the next scheduling round).
    for (;;)
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
        attr->__type = PTHREAD_MUTEX_DEFAULT;
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
            result = m->TimedLock(TimespecToRelativeTimeout(abstime)) ? 0 : ETIMEDOUT;
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
        attr->__reserved = 0;
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
        (void)EnsureCond(cond)->Wait(*HandleOf(mutex)); // WAIT_INFINITE
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
        const bool signaled = EnsureCond(cond)->Wait(*HandleOf(mutex), TimespecToRelativeTimeout(abstime));
        result = signaled ? 0 : ETIMEDOUT;
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
        const bool locked = EnsureRWLock(rwlock)->TimedReadLock(TimespecToRelativeTimeout(abstime));
        result = locked ? 0 : ETIMEDOUT;
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
        if (EnsureRWLock(rwlock)->TimedLock(TimespecToRelativeTimeout(abstime)))
        {
            rwlock->__wrlocked = true;
            result = 0;
        }
        else
        {
            result = ETIMEDOUT;
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
        result = HandleOf(barrier)->Wait() ? PTHREAD_BARRIER_SERIAL_THREAD : 0;
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

// =============================================================================
} // extern "C"
// =============================================================================
