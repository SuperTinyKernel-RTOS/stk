/*
 * Copyright (c) 2017 Simon Goldschmidt
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com> (STK port)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Original author: Simon Goldschmidt <goldsimon@gmx.de>
 * STK port: Neutron Code Limited <stk@neutroncode.com>
 *
 * ---------------------------------------------------------------------------
 * lwIP sys_arch port for SuperTinyKernel(TM) RTOS (STK).
 *
 * Mapping of lwIP sys_arch primitives onto STK:
 *
 *   sys_mutex_t  -> stk::sync::Mutex        (pool-allocated, recursive)
 *   sys_sem_t    -> stk::sync::Semaphore    (pool-allocated, binary 0/1)
 *   sys_mbox_t   -> stk::sync::MessageQueue (pool-allocated, msg_size=sizeof(void*))
 *   sys_thread_t -> a small stk::ITask implementation (pool-allocated, both the
 *                   control block and its stack)
 *
 * IMPORTANT - why these are pool-allocated rather than mem_malloc()'d:
 * lwIP's own heap (mem.c) protects itself with a mutex it creates via
 * sys_mutex_new() the first time mem_malloc()/mem_init() runs (whenever
 * !SYS_LIGHTWEIGHT_PROT). If sys_mutex_new() itself called mem_malloc() to
 * get storage for that very stk::sync::Mutex, the first allocation would
 * recurse into sys_mutex_new() -> mem_malloc() before the heap's own
 * protection exists - a bootstrapping deadlock/failure during lwip_init().
 * Every allocation in this file therefore comes from fixed-capacity
 * stk::memory::BlockMemoryPool instances owned by this port, never from
 * lwIP's mem_malloc()/mem_free(), which fully breaks that dependency cycle
 * (independent of the SYS_LIGHTWEIGHT_PROT setting). See the
 * LWIP_STK_*_POOL_SIZE / *_BLOCK_* config knobs below to size them for your
 * application.
 *
 * SYS_LIGHTWEIGHT_PROT critical sections map directly onto
 * stk::hw::CriticalSection::Enter()/Exit(), which is IRQ-disabling,
 * multi-core-safe, and nests safely.
 *
 * This file must be compiled as C++ (it uses STK's C++ API), but every
 * function lwIP calls is declared extern "C" so plain-C lwIP translation
 * units can link against it directly, exactly as lwip/sys.h expects.
 *
 * See sys_arch.h for the one extra, STK-specific setup step required before
 * sys_init()/tcpip_init(): registering the application's stk::IKernel
 * instance via sys_arch_set_kernel().
 * ---------------------------------------------------------------------------
 */

/* lwIP includes. */
#include "lwip/debug.h"
#include "lwip/def.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
/* Deliberately NOT including lwip/mem.h / calling mem_malloc()/mem_free() anywhere in this
 * file - see the bootstrapping-deadlock note above. All storage here comes from this port's
 * own stk::memory::BlockMemoryPool instances instead. */

#include "arch/sys_arch.h"

#include <cstdint>
#include <new>

/* STK includes. */
#include "stk.h"
#include "sync/stk_sync.h"
#include "memory/stk_memory.h"

/* -----------------------------------------------------------------------------
 * Kernel registration
 * -------------------------------------------------------------------------- */

static stk::IKernel *s_kernel = nullptr;

extern "C" void sys_arch_set_kernel(stk::IKernel *kernel)
{
    LWIP_ASSERT("sys_arch_set_kernel: kernel must not be NULL", kernel != nullptr);
    s_kernel = kernel;
}

/* -----------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */

/* Convert an lwIP millisecond timeout (0 already handled by caller as
 * "infinite") into an STK tick-based Timeout, clamped to WAIT_INFINITE. */
static inline stk::Timeout StkTimeoutFromMs(u32_t timeout_ms)
{
    const stk::Timeout ms_clamped =
        (timeout_ms > static_cast<u32_t>(INT32_MAX)) ? INT32_MAX : static_cast<stk::Timeout>(timeout_ms);

    return stk::GetTicksFromMsClampedToTimeout(ms_clamped);
}

/* -----------------------------------------------------------------------------
 * sys_init (called first, before any other sys_arch/tcpip function)
 * -------------------------------------------------------------------------- */

extern "C" void sys_init(void)
{
    LWIP_ASSERT("STK kernel not registered - call sys_arch_set_kernel() before "
                "sys_init()/tcpip_init()/lwip_init()", s_kernel != nullptr);
}

/* -----------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

#if LWIP_STK_SYS_NOW_FROM_STK
extern "C" u32_t sys_now(void)
{
    return static_cast<u32_t>(stk::GetTimeNowMs());
}
#endif

extern "C" u32_t sys_jiffies(void)
{
    return static_cast<u32_t>(stk::GetTicks());
}

/* -----------------------------------------------------------------------------
 * SYS_LIGHTWEIGHT_PROT
 * -------------------------------------------------------------------------- */

#if SYS_LIGHTWEIGHT_PROT

#if LWIP_STK_SYS_ARCH_PROTECT_SANITY_CHECK
static sys_prot_t s_arch_protect_nesting;
#endif

extern "C" sys_prot_t sys_arch_protect(void)
{
    stk::hw::CriticalSection::Enter();

#if LWIP_STK_SYS_ARCH_PROTECT_SANITY_CHECK
    {
        /* every nested call to sys_arch_protect() returns an increased number */
        sys_prot_t ret = s_arch_protect_nesting;
        s_arch_protect_nesting++;
        LWIP_ASSERT("sys_arch_protect overflow", s_arch_protect_nesting > ret);
        return ret;
    }
#else
    return 1;
#endif
}

extern "C" void sys_arch_unprotect(sys_prot_t pval)
{
#if LWIP_STK_SYS_ARCH_PROTECT_SANITY_CHECK
    LWIP_ASSERT("unexpected sys_arch_protect_nesting", s_arch_protect_nesting > 0);
    s_arch_protect_nesting--;
    LWIP_ASSERT("unexpected sys_arch_protect_nesting", s_arch_protect_nesting == pval);
#endif

    LWIP_UNUSED_ARG(pval);
    stk::hw::CriticalSection::Exit();
}

#endif /* SYS_LIGHTWEIGHT_PROT */

/* -----------------------------------------------------------------------------
 * Sleep
 * -------------------------------------------------------------------------- */

extern "C" void sys_arch_msleep(u32_t delay_ms)
{
    const stk::Timeout ms_clamped =
        (delay_ms > static_cast<u32_t>(INT32_MAX)) ? INT32_MAX : static_cast<stk::Timeout>(delay_ms);

    stk::SleepMs(ms_clamped);
}

/* -----------------------------------------------------------------------------
 * Mutex
 * -------------------------------------------------------------------------- */

#if !LWIP_COMPAT_MUTEX

namespace
{

alignas(alignof(void *)) uint8_t s_mutex_pool_storage[
     LWIP_STK_MUTEX_POOL_SIZE * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(stk::sync::Mutex))];
stk::memory::BlockMemoryPool s_mutex_pool(LWIP_STK_MUTEX_POOL_SIZE, sizeof(stk::sync::Mutex),
        s_mutex_pool_storage, sizeof(s_mutex_pool_storage),
        "lwip_stk.mutex_pool");

} /* anonymous namespace */

extern "C" err_t sys_mutex_new(sys_mutex_t *mutex)
{
    LWIP_ASSERT("mutex != NULL", mutex != nullptr);

    /* TryAlloc(), not the blocking Alloc(): a full pool should fail fast with ERR_MEM (like a
     * failed mem_malloc() would have), not suspend the caller - raise LWIP_STK_MUTEX_POOL_SIZE
     * if this starts happening. */
    void *mem = s_mutex_pool.TryAlloc();
    if (mem == nullptr)
    {
        SYS_STATS_INC(mutex.err);
        return ERR_MEM;
    }

    mutex->mut = new (mem) stk::sync::Mutex();
    SYS_STATS_INC_USED(mutex);
    return ERR_OK;
}

extern "C" void sys_mutex_lock(sys_mutex_t *mutex)
{
    LWIP_ASSERT("mutex != NULL", mutex != nullptr);
    LWIP_ASSERT("mutex->mut != NULL", mutex->mut != nullptr);

    static_cast<stk::sync::Mutex *>(mutex->mut)->Lock();
}

extern "C" void sys_mutex_unlock(sys_mutex_t *mutex)
{
    LWIP_ASSERT("mutex != NULL", mutex != nullptr);
    LWIP_ASSERT("mutex->mut != NULL", mutex->mut != nullptr);

    static_cast<stk::sync::Mutex *>(mutex->mut)->Unlock();
}

extern "C" void sys_mutex_free(sys_mutex_t *mutex)
{
    LWIP_ASSERT("mutex != NULL", mutex != nullptr);
    LWIP_ASSERT("mutex->mut != NULL", mutex->mut != nullptr);

    stk::sync::Mutex *m = static_cast<stk::sync::Mutex *>(mutex->mut);
    m->~Mutex();
    s_mutex_pool.Free(m);

    SYS_STATS_DEC(mutex.used);
    mutex->mut = nullptr;
}

#endif /* !LWIP_COMPAT_MUTEX */

/* -----------------------------------------------------------------------------
 * Semaphore
 *
 * lwIP only ever creates binary semaphores via sys_sem_new() (initial_count is
 * 0 or 1).
 * -------------------------------------------------------------------------- */

namespace
{

alignas(alignof(void *)) uint8_t s_sem_pool_storage[
     LWIP_STK_SEM_POOL_SIZE * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(stk::sync::Semaphore))];
stk::memory::BlockMemoryPool s_sem_pool(LWIP_STK_SEM_POOL_SIZE, sizeof(stk::sync::Semaphore),
                                        s_sem_pool_storage, sizeof(s_sem_pool_storage),
                                        "lwip_stk.sem_pool");

} /* anonymous namespace */

extern "C" err_t sys_sem_new(sys_sem_t *sem, u8_t initial_count)
{
    LWIP_ASSERT("sem != NULL", sem != nullptr);
    LWIP_ASSERT("initial_count invalid (not 0 or 1)",
                (initial_count == 0) || (initial_count == 1));

    /* TryAlloc(), not the blocking Alloc() - see the note in sys_mutex_new(). Also makes
     * sys_sem_new() safe to call from contexts where blocking would be inappropriate, matching
     * mem_malloc()'s original non-blocking behaviour. Raise LWIP_STK_SEM_POOL_SIZE if this
     * starts returning ERR_MEM under normal load. */
    void *mem = s_sem_pool.TryAlloc();
    if (mem == nullptr)
    {
        SYS_STATS_INC(sem.err);
        return ERR_MEM;
    }

    sem->sem = new (mem) stk::sync::Semaphore(static_cast<uint16_t>(initial_count), SYS_SEM_MAX_COUNT);
    SYS_STATS_INC_USED(sem);
    return ERR_OK;
}

extern "C" void sys_sem_signal(sys_sem_t *sem)
{
    LWIP_ASSERT("sem != NULL", sem != nullptr);
    LWIP_ASSERT("sem->sem != NULL", sem->sem != nullptr);

    stk::sync::Semaphore *s = static_cast<stk::sync::Semaphore *>(sem->sem);

    s->TrySignal();
}

extern "C" u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout_ms)
{
    LWIP_ASSERT("sem != NULL", sem != nullptr);
    LWIP_ASSERT("sem->sem != NULL", sem->sem != nullptr);

    stk::sync::Semaphore *s = static_cast<stk::sync::Semaphore *>(sem->sem);
    const stk::Timeout tmo = (timeout_ms == 0U) ? stk::WAIT_INFINITE : StkTimeoutFromMs(timeout_ms);

    if (!s->Wait(tmo))
    {
        /* timed out */
        return SYS_ARCH_TIMEOUT;
    }

    /* Old versions of lwIP required us to return the time waited.
       This is not the case any more. Just returning != SYS_ARCH_TIMEOUT
       here is enough. */
    return 1;
}

extern "C" void sys_sem_free(sys_sem_t *sem)
{
    LWIP_ASSERT("sem != NULL", sem != nullptr);
    LWIP_ASSERT("sem->sem != NULL", sem->sem != nullptr);

    stk::sync::Semaphore *s = static_cast<stk::sync::Semaphore *>(sem->sem);
    s->~Semaphore();
    s_sem_pool.Free(s);

    SYS_STATS_DEC(sem.used);
    sem->sem = nullptr;
}

/* -----------------------------------------------------------------------------
 * Mailbox (sys_mbox_t)
 *
 * Backed by stk::sync::MessageQueue transporting fixed-size sizeof(void*)
 * messages, matching lwIP's "mailbox carries opaque pointers" contract.
 * MessageQueue requires an externally-owned buffer, so the control block
 * (LwipStkMbox) and the ring buffer are two separate pool allocations (from
 * s_mbox_cb_pool / s_mbox_buf_pool below), with LwipStkMbox remembering the
 * buffer pointer so sys_mbox_free() can release both.
 * -------------------------------------------------------------------------- */

namespace
{

struct LwipStkMbox
{
    explicit LwipStkMbox(uint8_t *buf, size_t capacity, size_t msg_size)
        : m_mq(buf, capacity, msg_size), m_buf(buf)
    {}

    stk::sync::MessageQueue m_mq;
    uint8_t                *m_buf;

private:
    STK_NONCOPYABLE_CLASS(LwipStkMbox);
};

alignas(alignof(void *)) uint8_t s_mbox_cb_pool_storage[
     LWIP_STK_MBOX_POOL_SIZE * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(LwipStkMbox))];
stk::memory::BlockMemoryPool s_mbox_cb_pool(LWIP_STK_MBOX_POOL_SIZE, sizeof(LwipStkMbox),
        s_mbox_cb_pool_storage, sizeof(s_mbox_cb_pool_storage),
        "lwip_stk.mbox_cb_pool");

/* One shared block size for every mbox's ring buffer, rather than a size-matched heap
 * allocation - see LWIP_STK_MBOX_BUF_BLOCK_BYTES above for how to size this. */
alignas(alignof(void *)) uint8_t s_mbox_buf_pool_storage[
     LWIP_STK_MBOX_POOL_SIZE * stk::memory::BlockMemoryPool::AlignBlockSize(LWIP_STK_MBOX_BUF_BLOCK_BYTES)];
stk::memory::BlockMemoryPool s_mbox_buf_pool(LWIP_STK_MBOX_POOL_SIZE, LWIP_STK_MBOX_BUF_BLOCK_BYTES,
        s_mbox_buf_pool_storage, sizeof(s_mbox_buf_pool_storage),
        "lwip_stk.mbox_buf_pool");

} /* anonymous namespace */

extern "C" err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    LWIP_ASSERT("mbox != NULL", mbox != nullptr);
    LWIP_ASSERT("size > 0", size > 0);

    const size_t capacity  = static_cast<size_t>(size);
    const size_t msg_size  = sizeof(void *);
    const size_t buf_bytes = capacity * msg_size;

    if (capacity > stk::sync::MessageQueue::CAPACITY_MAX)
    {
        SYS_STATS_INC(mbox.err);
        return ERR_MEM;
    }

    /* Every mbox shares one fixed block size (LWIP_STK_MBOX_BUF_BLOCK_BYTES); a request bigger
     * than that means the macro needs raising for this build's lwipopts.h mbox sizes. */
    LWIP_ASSERT("sys_mbox_new: mbox size exceeds LWIP_STK_MBOX_BUF_BLOCK_BYTES - "
                "raise that macro to cover this mbox's configured size",
                buf_bytes <= LWIP_STK_MBOX_BUF_BLOCK_BYTES);
    if (buf_bytes > LWIP_STK_MBOX_BUF_BLOCK_BYTES)
    {
        SYS_STATS_INC(mbox.err);
        return ERR_MEM;
    }

    uint8_t *buf = static_cast<uint8_t *>(s_mbox_buf_pool.TryAlloc());
    if (buf == nullptr)
    {
        SYS_STATS_INC(mbox.err);
        return ERR_MEM;
    }

    void *cb_mem = s_mbox_cb_pool.TryAlloc();
    if (cb_mem == nullptr)
    {
        s_mbox_buf_pool.Free(buf);
        SYS_STATS_INC(mbox.err);
        return ERR_MEM;
    }

    mbox->mbx = new (cb_mem) LwipStkMbox(buf, capacity, msg_size);
    SYS_STATS_INC_USED(mbox);
    return ERR_OK;
}

extern "C" void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    LWIP_ASSERT("mbox != NULL", mbox != nullptr);
    LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != nullptr);

    LwipStkMbox *q = static_cast<LwipStkMbox *>(mbox->mbx);
    const bool ok = q->m_mq.Put(&msg, stk::WAIT_INFINITE);
    LWIP_ASSERT("mbox post failed", ok);
    LWIP_UNUSED_ARG(ok);
}

extern "C" err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    LWIP_ASSERT("mbox != NULL", mbox != nullptr);
    LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != nullptr);

    LwipStkMbox *q = static_cast<LwipStkMbox *>(mbox->mbx);

    if (!q->m_mq.TryPut(&msg))
    {
        SYS_STATS_INC(mbox.err);
        return ERR_MEM;
    }

    return ERR_OK;
}

extern "C" err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    /* MessageQueue::TryPut() == Put(NO_WAIT), which only ever takes the
     * ScopedCriticalSection-guarded fast path -> ISR-safe. STK's switch
     * strategy re-evaluates priorities as part of the wake itself, so there is
     * no separate "request a context switch" step for the port to perform;
     * this function therefore never returns ERR_NEED_SCHED. */
    return sys_mbox_trypost(mbox, msg);
}

extern "C" u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout_ms)
{
    LWIP_ASSERT("mbox != NULL", mbox != nullptr);
    LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != nullptr);

    LwipStkMbox *q = static_cast<LwipStkMbox *>(mbox->mbx);
    void *msg_dummy;
    if (msg == nullptr)
    {
        msg = &msg_dummy;
    }

    const stk::Timeout tmo = (timeout_ms == 0U) ? stk::WAIT_INFINITE : StkTimeoutFromMs(timeout_ms);

    if (!q->m_mq.Get(msg, tmo))
    {
        /* timed out */
        *msg = nullptr;
        return SYS_ARCH_TIMEOUT;
    }

    /* Old versions of lwIP required us to return the time waited.
       This is not the case any more. Just returning != SYS_ARCH_TIMEOUT
       here is enough. */
    return 1;
}

extern "C" u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    LWIP_ASSERT("mbox != NULL", mbox != nullptr);
    LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != nullptr);

    LwipStkMbox *q = static_cast<LwipStkMbox *>(mbox->mbx);
    void *msg_dummy;
    if (msg == nullptr)
    {
        msg = &msg_dummy;
    }

    if (!q->m_mq.TryGet(msg))
    {
        *msg = nullptr;
        return SYS_MBOX_EMPTY;
    }

    return 1;
}

extern "C" void sys_mbox_free(sys_mbox_t *mbox)
{
    LWIP_ASSERT("mbox != NULL", mbox != nullptr);
    LWIP_ASSERT("mbox->mbx != NULL", mbox->mbx != nullptr);

    LwipStkMbox *q = static_cast<LwipStkMbox *>(mbox->mbx);

#if LWIP_STK_CHECK_QUEUE_EMPTY_ON_FREE
    {
        const size_t msgs_waiting = q->m_mq.GetCount();
        LWIP_ASSERT("mbox queue not empty", msgs_waiting == 0U);

        if (msgs_waiting != 0U)
        {
            SYS_STATS_INC(mbox.err);
        }
    }
#endif

    uint8_t *buf = q->m_buf; /* stash before dtor runs, m_buf is inaccessible after */
    q->~LwipStkMbox();
    s_mbox_buf_pool.Free(buf);
    s_mbox_cb_pool.Free(q);

    SYS_STATS_DEC(mbox.used);
    mbox->mbx = nullptr;
}

/* -----------------------------------------------------------------------------
 * Threads (sys_thread_t)
 * -------------------------------------------------------------------------- */

namespace
{

class LwipStkThread : public stk::ITask
{
public:
    explicit LwipStkThread(lwip_thread_fn fn, void *arg, const char *name, stk::Weight weight)
        : m_fn(fn), m_arg(arg), m_name(name), m_weight(weight), m_stack(nullptr), m_stack_size(0U)
    {}

    void SetStack(stk::Word *stack, size_t words)
    {
        m_stack      = stack;
        m_stack_size = words;
    }

    /* ---- stk::ITask ---- */
    void Run() override
    {
        /* lwip_thread_fn matches STK's Run()-less-arguments contract via the
         * captured (fn, arg) pair below. lwIP threads are expected to run
         * forever (an infinite loop); if one does return, the KERNEL_DYNAMIC
         * kernel simply retires this task slot (see ITask::OnExit). */
        m_fn(m_arg);
    }

    const stk::Word  *GetStack()      const override { return m_stack; }
    size_t            GetStackSize()  const override { return m_stack_size; }
    stk::EAccessMode  GetAccessMode() const override { return LWIP_STK_THREAD_ACCESS_MODE; }
    stk::Weight       GetWeight()     const override { return m_weight; }
    const char       *GetTraceName()  const override { return m_name; }

private:
    STK_NONCOPYABLE_CLASS(LwipStkThread);

    lwip_thread_fn m_fn;
    void          *m_arg;
    const char    *m_name;
    stk::Weight    m_weight;
    stk::Word     *m_stack;
    size_t         m_stack_size;
};

alignas(alignof(void *)) uint8_t s_thread_cb_pool_storage[
     LWIP_STK_THREAD_POOL_SIZE * stk::memory::BlockMemoryPool::AlignBlockSize(sizeof(LwipStkThread))];
stk::memory::BlockMemoryPool s_thread_cb_pool(LWIP_STK_THREAD_POOL_SIZE, sizeof(LwipStkThread),
        s_thread_cb_pool_storage, sizeof(s_thread_cb_pool_storage),
        "lwip_stk.thread_cb_pool");

/* One shared stack block size for every thread - see LWIP_STK_THREAD_STACK_BLOCK_WORDS above
 * for how to size this. */
alignas(alignof(stk::Word)) uint8_t s_thread_stack_pool_storage[
     LWIP_STK_THREAD_POOL_SIZE *
     stk::memory::BlockMemoryPool::AlignBlockSize(LWIP_STK_THREAD_STACK_BLOCK_WORDS * sizeof(stk::Word))];
stk::memory::BlockMemoryPool s_thread_stack_pool(
    LWIP_STK_THREAD_POOL_SIZE, LWIP_STK_THREAD_STACK_BLOCK_WORDS * sizeof(stk::Word),
    s_thread_stack_pool_storage, sizeof(s_thread_stack_pool_storage),
    "lwip_stk.thread_stack_pool");

} /* anonymous namespace */

extern "C" sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)
{
    LWIP_ASSERT("STK kernel not registered - call sys_arch_set_kernel() before "
                "sys_init()/tcpip_init()", s_kernel != nullptr);
    LWIP_ASSERT("invalid stacksize", stacksize > 0);

#if LWIP_STK_THREAD_STACKSIZE_IS_STACKWORDS
    size_t stack_words = static_cast<size_t>(stacksize);
#else
    size_t stack_words = static_cast<size_t>(stacksize) / sizeof(stk::Word);
#endif

    if (stack_words < static_cast<size_t>(stk::STACK_SIZE_MIN))
    {
        stack_words = static_cast<size_t>(stk::STACK_SIZE_MIN);
    }

    /* Every thread shares one fixed stack block size (LWIP_STK_THREAD_STACK_BLOCK_WORDS); a
     * request bigger than that means the macro needs raising for this build. */
    LWIP_ASSERT("sys_thread_new: stacksize exceeds LWIP_STK_THREAD_STACK_BLOCK_WORDS - "
                "raise that macro to cover this thread's requested stack size",
                stack_words <= LWIP_STK_THREAD_STACK_BLOCK_WORDS);
    if (stack_words > LWIP_STK_THREAD_STACK_BLOCK_WORDS)
    {
        stack_words = LWIP_STK_THREAD_STACK_BLOCK_WORDS;
    }

    /* STK weights must be positive and non-zero; lwIP threads use arbitrary
     * small integer priorities (e.g. TCPIP_THREAD_PRIO), which map directly
     * onto weight as long as they are > 0. A non-positive priority falls back
     * to stk::DEFAULT_WEIGHT. */
    const stk::Weight weight = (prio <= 0) ? stk::DEFAULT_WEIGHT : static_cast<stk::Weight>(prio);

    void *cb_mem = s_thread_cb_pool.TryAlloc();
    LWIP_ASSERT("sys_thread_new: task control block allocation failed - "
                "raise LWIP_STK_THREAD_POOL_SIZE", cb_mem != nullptr);

    LwipStkThread *t = new (cb_mem) LwipStkThread(thread, arg, name, weight);

    stk::Word *stack = static_cast<stk::Word *>(s_thread_stack_pool.TryAlloc());
    LWIP_ASSERT("sys_thread_new: stack allocation failed - "
                "raise LWIP_STK_THREAD_POOL_SIZE", stack != nullptr);
    t->SetStack(stack, stack_words);

    s_kernel->AddTask(t);

    sys_thread_t lwip_thread;
    lwip_thread.thread_handle = t;
    return lwip_thread;
}

/* -----------------------------------------------------------------------------
 * Per-thread netconn semaphore (LWIP_NETCONN_SEM_PER_THREAD)
 *
 * STK's ITask has no generic user-data/TLS slot, so this is implemented as a
 * small, bounded side-table keyed by stk::TId rather than storage embedded in
 * the calling task's control block, since STK does not expose one for
 * arbitrary, possibly non-lwIP-owned, tasks.
 * -------------------------------------------------------------------------- */

#if LWIP_NETCONN_SEM_PER_THREAD

namespace
{

struct NetconnSemSlot
{
    stk::TId  tid;
    sys_sem_t sem;
    bool      used;
};

static NetconnSemSlot s_netconn_sems[LWIP_STK_NETCONN_SEM_MAX_THREADS];

} /* anonymous namespace */

extern "C" sys_sem_t *sys_arch_netconn_sem_get(void)
{
    const stk::TId tid = stk::GetTid();

    const stk::sync::ScopedCriticalSection cs_;

    for (size_t i = 0U; i < LWIP_STK_NETCONN_SEM_MAX_THREADS; ++i)
    {
        if (s_netconn_sems[i].used && (s_netconn_sems[i].tid == tid))
        {
            return &s_netconn_sems[i].sem;
        }
    }

    return nullptr;
}

extern "C" void sys_arch_netconn_sem_alloc(void)
{
    const stk::TId tid = stk::GetTid();

    /* Already allocated for this thread? */
    {
        const stk::sync::ScopedCriticalSection cs_;

        for (size_t i = 0U; i < LWIP_STK_NETCONN_SEM_MAX_THREADS; ++i)
        {
            if (s_netconn_sems[i].used && (s_netconn_sems[i].tid == tid))
            {
                return;
            }
        }
    }

    /* Allocate the semaphore itself outside the critical section: sys_sem_new()
     * heap-allocates and placement-constructs, neither of which should run
     * with interrupts/scheduling disabled. */
    sys_sem_t new_sem;
    const err_t err = sys_sem_new(&new_sem, 0);
    LWIP_ASSERT("sys_arch_netconn_sem_alloc: sys_sem_new failed", err == ERR_OK);

    bool inserted = false;
    {
        const stk::sync::ScopedCriticalSection cs_;

        for (size_t i = 0U; i < LWIP_STK_NETCONN_SEM_MAX_THREADS; ++i)
        {
            if (!s_netconn_sems[i].used)
            {
                s_netconn_sems[i].tid  = tid;
                s_netconn_sems[i].sem  = new_sem;
                s_netconn_sems[i].used = true;
                inserted = true;
                break;
            }
        }
    }

    if (!inserted)
    {
        LWIP_ASSERT("sys_arch_netconn_sem_alloc: no free slot, "
                    "increase LWIP_STK_NETCONN_SEM_MAX_THREADS", false);
        sys_sem_free(&new_sem);
    }
}

extern "C" void sys_arch_netconn_sem_free(void)
{
    const stk::TId tid = stk::GetTid();
    sys_sem_t to_free;
    bool found = false;

    {
        const stk::sync::ScopedCriticalSection cs_;

        for (size_t i = 0U; i < LWIP_STK_NETCONN_SEM_MAX_THREADS; ++i)
        {
            if (s_netconn_sems[i].used && (s_netconn_sems[i].tid == tid))
            {
                to_free = s_netconn_sems[i].sem;
                s_netconn_sems[i].used = false;
                found = true;
                break;
            }
        }
    }

    if (found)
    {
        sys_sem_free(&to_free);
    }
}

#endif /* LWIP_NETCONN_SEM_PER_THREAD */

/* -----------------------------------------------------------------------------
 * TCPIP core locking checks
 * -------------------------------------------------------------------------- */

#if LWIP_STK_CHECK_CORE_LOCKING

/** Set this to 1 to delegate sys_lock_tcpip_core()/sys_unlock_tcpip_core() to
 * an external, integration-specific pair of functions -
 * pico_lwip_custom_lock_tcpip_core() / pico_lwip_custom_unlock_tcpip_core() -
 * instead of this port's own stk::sync::Mutex-backed implementation.
 *
 * Used by e.g. lwip_stk.c (the Pico SDK async_context integration), which
 * needs lwIP's tcpip core lock to *be* the async_context's own lock (so code
 * already running inside an async_context callback doesn't deadlock taking
 * it again), rather than an independent stk::sync::Mutex layered on top.
 */
#ifndef LWIP_STK_CUSTOM_CORE_LOCKING
#define LWIP_STK_CUSTOM_CORE_LOCKING  0
#endif

#if LWIP_TCPIP_CORE_LOCKING

#if LWIP_STK_CUSTOM_CORE_LOCKING

/* Provided by the integration that opted into LWIP_STK_CUSTOM_CORE_LOCKING
 * (e.g. lwip_stk.c). */
extern "C" void pico_lwip_custom_lock_tcpip_core(void);
extern "C" void pico_lwip_custom_unlock_tcpip_core(void);

extern "C" void sys_lock_tcpip_core(void)
{
    pico_lwip_custom_lock_tcpip_core();
}

extern "C" void sys_unlock_tcpip_core(void)
{
    pico_lwip_custom_unlock_tcpip_core();
}

#else /* !LWIP_STK_CUSTOM_CORE_LOCKING */

/** Flag the core lock held. A counter for recursive locks. */
static uint8_t  s_stk_core_lock_count;
static stk::TId s_stk_core_lock_holder = stk::TID_NONE;

extern "C" void sys_lock_tcpip_core(void)
{
    sys_mutex_lock(&lock_tcpip_core);
    if (s_stk_core_lock_count == 0U)
    {
        s_stk_core_lock_holder = stk::GetTid();
    }
    s_stk_core_lock_count++;
}

extern "C" void sys_unlock_tcpip_core(void)
{
    s_stk_core_lock_count--;
    if (s_stk_core_lock_count == 0U)
    {
        s_stk_core_lock_holder = stk::TID_NONE;
    }
    sys_mutex_unlock(&lock_tcpip_core);
}

#endif /* LWIP_STK_CUSTOM_CORE_LOCKING */

#endif /* LWIP_TCPIP_CORE_LOCKING */

#if !NO_SYS
static stk::TId s_stk_tcpip_thread_tid = stk::TID_NONE;
#endif

extern "C" void sys_mark_tcpip_thread(void)
{
#if !NO_SYS
    s_stk_tcpip_thread_tid = stk::GetTid();
#endif
}

extern "C" void sys_check_core_locking(void)
{
    /* Embedded systems should check we are NOT in an interrupt context here. */
    LWIP_ASSERT("sys_check_core_locking() called from ISR context", !stk::hw::IsInsideISR());

#if !NO_SYS
    if (s_stk_tcpip_thread_tid != stk::TID_NONE)
    {

#if LWIP_TCPIP_CORE_LOCKING && !LWIP_STK_CUSTOM_CORE_LOCKING
        const stk::TId current_thread = stk::GetTid();
        LWIP_ASSERT("Function called without core lock",
                    (current_thread == s_stk_core_lock_holder) && (s_stk_core_lock_count > 0U));
#elif !LWIP_TCPIP_CORE_LOCKING
        const stk::TId current_thread = stk::GetTid();
        LWIP_ASSERT("Function called from wrong thread", current_thread == s_stk_tcpip_thread_tid);
#else
        /* LWIP_TCPIP_CORE_LOCKING && LWIP_STK_CUSTOM_CORE_LOCKING: ownership of
         * the external lock isn't tracked at this layer (it's the integration's
         * lock to account for, e.g. async_context's own lock-check facility),
         * so only the ISR check above applies here. */
#endif
    }
#endif /* !NO_SYS */
}

#endif /* LWIP_STK_CHECK_CORE_LOCKING */
