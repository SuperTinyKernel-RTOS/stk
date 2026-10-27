/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <stk.h>
#include "stk_c.h"              // must be included AFTER stk.h
#include <sync/stk_sync.h>
#include <memory/stk_memory.h>  // for placement new

using namespace stk;
using namespace stk::sync;

// =============================================================================
// C-interface
// =============================================================================
extern "C" {

// maps kernel wait result to the C API wait result
static stk_wait_result_t ToCWaitResult(EWaitResult wr)
{
    stk_wait_result_t result;

    switch (wr)
    {
    case WAIT_RESULT_SIGNAL:   result = STK_WAIT_RESULT_SIGNAL;   break;
    case WAIT_RESULT_TIMEOUT:  result = STK_WAIT_RESULT_TIMEOUT;  break;
    case WAIT_RESULT_CANCELED: result = STK_WAIT_RESULT_CANCELED; break;
    default:                   result = STK_WAIT_RESULT_FAIL;     break;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Mutex
// -----------------------------------------------------------------------------
struct stk_mutex_t
{
    Mutex handle;
};

stk_mutex_t *stk_mutex_create(stk_mutex_mem_t *const membuf)
{
    STK_ASSERT(membuf != nullptr);

    stk_mutex_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_mutex_t>(*membuf);
    }

    return result;
}

void stk_mutex_destroy(stk_mutex_t *mtx)
{
    if (mtx != nullptr)
    {
        mtx->~stk_mutex_t();
    }
}

void stk_mutex_lock(stk_mutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    mtx->handle.Lock();
}

bool stk_mutex_trylock(stk_mutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    return mtx->handle.TryLock();
}

void stk_mutex_unlock(stk_mutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    mtx->handle.Unlock();
}

bool stk_mutex_timed_lock(stk_mutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(mtx != nullptr);

    return mtx->handle.TimedLock(timeout);
}

stk_wait_result_t stk_mutex_timed_lock_ex(stk_mutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(mtx != nullptr);

    return ToCWaitResult(mtx->handle.TimedLockEx(timeout));
}

Mutex *stk_mutex_get_instance(stk_mutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    return &mtx->handle;
}

// -----------------------------------------------------------------------------
// FastMutex
// -----------------------------------------------------------------------------
struct stk_fastmutex_t
{
    FastMutex handle;
};

stk_fastmutex_t *stk_fastmutex_create(stk_fastmutex_mem_t *const membuf)
{
    STK_ASSERT(membuf != nullptr);

    stk_fastmutex_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_fastmutex_t>(*membuf);
    }

    return result;
}

void stk_fastmutex_destroy(stk_fastmutex_t *mtx)
{
    if (mtx != nullptr)
    {
        mtx->~stk_fastmutex_t();
    }
}

void stk_fastmutex_lock(stk_fastmutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    mtx->handle.Lock();
}

bool stk_fastmutex_trylock(stk_fastmutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    return mtx->handle.TryLock();
}

void stk_fastmutex_unlock(stk_fastmutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    mtx->handle.Unlock();
}

bool stk_fastmutex_timed_lock(stk_fastmutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(mtx != nullptr);

    return mtx->handle.TimedLock(timeout);
}

stk_wait_result_t stk_fastmutex_timed_lock_ex(stk_fastmutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(mtx != nullptr);

    return ToCWaitResult(mtx->handle.TimedLockEx(timeout));
}

FastMutex *stk_fastmutex_get_instance(stk_fastmutex_t *mtx)
{
    STK_ASSERT(mtx != nullptr);

    return &mtx->handle;
}

// -----------------------------------------------------------------------------
// SpinLock
// -----------------------------------------------------------------------------
struct stk_spinlock_t
{
    sync::SpinLock handle;
};

stk_spinlock_t *stk_spinlock_create(stk_spinlock_mem_t *const membuf)
{
    STK_ASSERT(membuf != nullptr);

    stk_spinlock_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_spinlock_t>(*membuf);
    }

    return result;
}

void stk_spinlock_destroy(stk_spinlock_t *slock)
{
    if (slock != nullptr)
    {
        slock->~stk_spinlock_t();
    }
}

void stk_spinlock_lock(stk_spinlock_t *slock)
{
    STK_ASSERT(slock != nullptr);
    slock->handle.Lock();
}

bool stk_spinlock_trylock(stk_spinlock_t *slock)
{
    STK_ASSERT(slock != nullptr);
    return slock->handle.TryLock();
}

void stk_spinlock_unlock(stk_spinlock_t *slock)
{
    STK_ASSERT(slock != nullptr);
    slock->handle.Unlock();
}

sync::SpinLock *stk_spinlock_get_instance(stk_spinlock_t *slock)
{
    STK_ASSERT(slock != nullptr);

    return &slock->handle;
}

// -----------------------------------------------------------------------------
// ConditionVariable
// -----------------------------------------------------------------------------
struct stk_cv_t
{
    ConditionVariable handle;
};

stk_cv_t *stk_cv_create(stk_cv_mem_t *const membuf)
{
    STK_ASSERT(membuf != nullptr);

    stk_cv_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_cv_t>(*membuf);
    }

    return result;
}

void stk_cv_destroy(stk_cv_t *cv)
{
    if (cv != nullptr)
    {
        cv->~stk_cv_t();
    }
}

bool stk_cv_wait(stk_cv_t *cv, stk_mutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(cv != nullptr);
    STK_ASSERT(mtx != nullptr);

    return cv->handle.Wait(mtx->handle, timeout);
}

stk_wait_result_t stk_cv_wait_ex(stk_cv_t *cv, stk_mutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(cv != nullptr);
    STK_ASSERT(mtx != nullptr);

    return ToCWaitResult(cv->handle.WaitEx(mtx->handle, timeout));
}

bool stk_cv_wait_fastmutex(stk_cv_t *cv, stk_fastmutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(cv != nullptr);
    STK_ASSERT(mtx != nullptr);

    return cv->handle.Wait(mtx->handle, timeout);
}

stk_wait_result_t stk_cv_wait_ex_fastmutex(stk_cv_t *cv, stk_fastmutex_t *mtx, stk_timeout_t timeout)
{
    STK_ASSERT(cv != nullptr);
    STK_ASSERT(mtx != nullptr);

    return ToCWaitResult(cv->handle.WaitEx(mtx->handle, timeout));
}

void stk_cv_notify_one(stk_cv_t *cv)
{
    STK_ASSERT(cv != nullptr);

    cv->handle.NotifyOne();
}

void stk_cv_notify_all(stk_cv_t *cv)
{
    STK_ASSERT(cv != nullptr);

    cv->handle.NotifyAll();
}

ConditionVariable *stk_cv_get_instance(stk_cv_t *cv)
{
    STK_ASSERT(cv != nullptr);

    return &cv->handle;
}

// -----------------------------------------------------------------------------
// Event
// -----------------------------------------------------------------------------
struct stk_event_t
{
    stk_event_t(bool manual_reset) : handle(manual_reset)
    {}

    Event handle;
};

stk_event_t *stk_event_create(stk_event_mem_t *const membuf,
                              bool manual_reset)
{
    STK_ASSERT(membuf != nullptr);

    stk_event_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_event_t>(*membuf, manual_reset);
    }

    return result;
}

void stk_event_destroy(stk_event_t *ev)
{
    if (ev != nullptr)
    {
        ev->~stk_event_t();
    }
}

bool stk_event_wait(stk_event_t *ev, stk_timeout_t timeout)
{
    STK_ASSERT(ev != nullptr);

    return ev->handle.Wait(timeout);
}

stk_wait_result_t stk_event_wait_ex(stk_event_t *ev, stk_timeout_t timeout)
{
    STK_ASSERT(ev != nullptr);

    return ToCWaitResult(ev->handle.WaitEx(timeout));
}

bool stk_event_trywait(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    return ev->handle.TryWait();
}

bool stk_event_set(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    return ev->handle.Set();
}

bool stk_event_reset(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    return ev->handle.Reset();
}

void stk_event_pulse(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    ev->handle.Pulse();
}

Event *stk_event_get_instance(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    return &ev->handle;
}

// -----------------------------------------------------------------------------
// Semaphore
// -----------------------------------------------------------------------------
struct stk_sem_t
{
    stk_sem_t(stk_sem_val_t initial_count, stk_sem_val_t max_count)
        : handle(initial_count, max_count)
    {}

    SemaphoreT<stk_sem_val_t> handle;
};

stk_sem_t *stk_sem_create(stk_sem_mem_t *const membuf,
                          stk_sem_val_t initial_count,
                          stk_sem_val_t max_count)
{
    STK_ASSERT(membuf != nullptr);

    const stk_sem_val_t effective_max = ((max_count == 0U) ? SemaphoreT<stk_sem_val_t>::COUNT_MAX : max_count);

    stk_sem_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_sem_t>(*membuf, initial_count, effective_max);
    }

    return result;
}

void stk_sem_destroy(stk_sem_t *sem)
{
    if (sem != nullptr)
    {
        sem->~stk_sem_t();
    }
}

bool stk_sem_wait(stk_sem_t *sem, stk_timeout_t timeout)
{
    STK_ASSERT(sem != nullptr);

    return sem->handle.Wait(timeout);
}

stk_wait_result_t stk_sem_wait_ex(stk_sem_t *sem, stk_timeout_t timeout)
{
    STK_ASSERT(sem != nullptr);

    return ToCWaitResult(sem->handle.WaitEx(timeout));
}

bool stk_sem_trywait(stk_sem_t *sem)
{
    STK_ASSERT(sem != nullptr);

    return sem->handle.TryWait();
}

void stk_sem_signal(stk_sem_t *sem)
{
    STK_ASSERT(sem != nullptr);

    sem->handle.Signal();
}

bool stk_sem_trysignal(stk_sem_t *sem)
{
    STK_ASSERT(sem != nullptr);

    return sem->handle.TrySignal();
}

bool stk_sem_trysignal_ceiling(stk_sem_t *sem, stk_sem_val_t ceiling)
{
    STK_ASSERT(sem != nullptr);

    return sem->handle.TrySignal(ceiling);
}

stk_sem_val_t stk_sem_get_count(const stk_sem_t *sem)
{
    STK_ASSERT(sem != nullptr);

    return sem->handle.GetCount();
}

SemaphoreT<stk_sem_val_t> *stk_sem_get_instance(stk_sem_t *sem)
{
    STK_ASSERT(sem != nullptr);

    return &sem->handle;
}

// -----------------------------------------------------------------------------
// EventFlags
// -----------------------------------------------------------------------------
struct stk_ef_t
{
    stk_ef_t(stk_ef_val_t initial_flags) : handle(initial_flags)
    {}

    EventFlagsT<stk_ef_val_t> handle;
};

stk_ef_t *stk_ef_create(stk_ef_mem_t *const membuf,
                        stk_ef_val_t initial_flags)
{
    STK_ASSERT(membuf != nullptr);

    stk_ef_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_ef_t>(*membuf, initial_flags);
    }

    return result;
}

void stk_ef_destroy(stk_ef_t *ef)
{
    if (ef != nullptr)
    {
        ef->~stk_ef_t();
    }
}

stk_ef_val_t stk_ef_set(stk_ef_t *ef, stk_ef_val_t flags)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Set(flags);
}

stk_ef_val_t stk_ef_clear(stk_ef_t *ef, stk_ef_val_t flags)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Clear(flags);
}

stk_ef_val_t stk_ef_and(stk_ef_t *ef, stk_ef_val_t mask)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.And(mask);
}

stk_ef_val_t stk_ef_get(stk_ef_t *ef)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Get();
}

stk_ef_val_t stk_ef_wait(stk_ef_t *ef, stk_ef_val_t flags, uint32_t options, stk_timeout_t timeout)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Wait(flags, options, timeout);
}

stk_ef_val_t stk_ef_trywait(stk_ef_t *ef, stk_ef_val_t flags, uint32_t options)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.TryWait(flags, options);
}

stk_wait_result_t stk_ef_wait_ex(stk_ef_t *ef, stk_ef_val_t flags, uint32_t options, stk_timeout_t timeout,
                                 stk_ef_val_t *actual_flags)
{
    STK_ASSERT(ef != nullptr);

    stk_ef_val_t actual = 0U;
    const stk_wait_result_t result = ToCWaitResult(ef->handle.WaitEx(flags, options, timeout, actual));

    if (actual_flags != nullptr)
    {
        *actual_flags = actual;
    }

    return result;
}

stk_wait_result_t stk_ef_trywait_ex(stk_ef_t *ef, stk_ef_val_t flags, uint32_t options, stk_ef_val_t *actual_flags)
{
    return stk_ef_wait_ex(ef, flags, options, STK_NO_WAIT, actual_flags);
}

struct stk_ef_sync_ctx_t
{
    EventFlagsT<stk_ef_val_t>::SyncContext handle;
};

stk_ef_sync_ctx_t *stk_ef_sync_ctx_create(stk_ef_sync_ctx_mem_t *const membuf)
{
    STK_ASSERT(membuf != nullptr);

    stk_ef_sync_ctx_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_ef_sync_ctx_t>(*membuf);
    }

    return result;
}

void stk_ef_sync_ctx_destroy(stk_ef_sync_ctx_t *ctx)
{
    if (ctx != nullptr)
    {
        ctx->~stk_ef_sync_ctx_t();
    }
}

stk_wait_result_t stk_ef_sync(stk_ef_t *ef, stk_ef_sync_ctx_t *ctx, stk_ef_val_t set_flags, stk_ef_val_t wait_flags,
                              stk_timeout_t timeout, stk_ef_val_t *actual_flags)
{
    STK_ASSERT(ef != nullptr);
    STK_ASSERT(ctx != nullptr);

    stk_ef_val_t actual = 0U;
    const stk_wait_result_t result = ToCWaitResult(ef->handle.Sync(ctx->handle, set_flags, wait_flags, timeout, actual));

    if (actual_flags != nullptr)
    {
        *actual_flags = actual;
    }

    return result;
}

EventFlagsT<stk_ef_val_t> *stk_ef_get_instance(stk_ef_t *ef)
{
    STK_ASSERT(ef != nullptr);

    return &ef->handle;
}

// -----------------------------------------------------------------------------
// Pipe (runtime-sized, external-buffer; wraps sync::Pipe)
// -----------------------------------------------------------------------------
struct stk_pipe_t
{
    Pipe handle;

    stk_pipe_t(uint8_t *buf, size_t capacity, size_t element_size)
        : handle(buf, capacity, element_size)
    {}
};

stk_pipe_t *stk_pipe_create(stk_pipe_mem_t *const membuf,
                            uint8_t *buf,
                            uint32_t buf_size,
                            size_t capacity,
                            size_t element_size)
{
    STK_ASSERT(membuf       != nullptr);
    STK_ASSERT(buf          != nullptr);
    STK_ASSERT(capacity     >= 1U);
    STK_ASSERT(element_size >= 1U);
    STK_ASSERT(buf_size     >= capacity * element_size);

    stk_pipe_t *result = nullptr;
    if ((membuf != nullptr) && (buf_size >= (capacity * element_size)))
    {
        result = ConstructIn<stk_pipe_t>(*membuf, buf, capacity, element_size);
    }

    return result;
}

void stk_pipe_destroy(stk_pipe_t *pipe)
{
    if (pipe != nullptr)
    {
        pipe->~stk_pipe_t();
    }
}

bool stk_pipe_write(stk_pipe_t *pipe, const void *data, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);
    STK_ASSERT(data != nullptr);

    return pipe->handle.Write(data, timeout);
}

stk_wait_result_t stk_pipe_write_ex(stk_pipe_t *pipe, const void *data, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);
    STK_ASSERT(data != nullptr);

    return ToCWaitResult(pipe->handle.WriteEx(data, timeout));
}

bool stk_pipe_trywrite(stk_pipe_t *pipe, const void *data)
{
    STK_ASSERT(pipe != nullptr);
    STK_ASSERT(data != nullptr);

    return pipe->handle.TryWrite(data);
}

bool stk_pipe_read(stk_pipe_t *pipe, void *data, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);
    STK_ASSERT(data != nullptr);

    return pipe->handle.Read(data, timeout);
}

stk_wait_result_t stk_pipe_read_ex(stk_pipe_t *pipe, void *data, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);
    STK_ASSERT(data != nullptr);

    return ToCWaitResult(pipe->handle.ReadEx(data, timeout));
}

bool stk_pipe_tryread(stk_pipe_t *pipe, void *data)
{
    STK_ASSERT(pipe != nullptr);
    STK_ASSERT(data != nullptr);

    return pipe->handle.TryRead(data);
}

size_t stk_pipe_write_bulk(stk_pipe_t *pipe, const void *src, size_t count, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.WriteBulk(src, count, timeout);
}

stk_wait_result_t stk_pipe_write_bulk_ex(stk_pipe_t *pipe, const void *src, size_t count,
                                         size_t *written, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    size_t n = 0U;
    const EWaitResult wr = pipe->handle.WriteBulkEx(src, count, n, timeout);

    if (written != nullptr)
    {
        *written = n;
    }

    return ToCWaitResult(wr);
}

size_t stk_pipe_trywrite_bulk(stk_pipe_t *pipe, const void *src, size_t count)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.TryWriteBulk(src, count);
}

size_t stk_pipe_read_bulk(stk_pipe_t *pipe, void *dst, size_t count, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.ReadBulk(dst, count, timeout);
}

stk_wait_result_t stk_pipe_read_bulk_ex(stk_pipe_t *pipe, void *dst, size_t count,
                                        size_t *read_count, stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    size_t n = 0U;
    const EWaitResult wr = pipe->handle.ReadBulkEx(dst, count, n, timeout);

    if (read_count != nullptr)
    {
        *read_count = n;
    }

    return ToCWaitResult(wr);
}

size_t stk_pipe_tryread_bulk(stk_pipe_t *pipe, void *dst, size_t count)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.TryReadBulk(dst, count);
}

size_t stk_pipe_read_bulk_triggered(stk_pipe_t   *pipe, 
                                    void         *dst,
                                    size_t        trigger, 
                                    size_t        max_count, 
                                    stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.ReadBulkTriggered(dst, trigger, max_count, timeout);
}

stk_wait_result_t stk_pipe_read_bulk_triggered_ex(stk_pipe_t   *pipe,
                                                  void         *dst,
                                                  size_t        trigger,
                                                  size_t        max_count,
                                                  size_t       *read_count,
                                                  stk_timeout_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    size_t n = 0U;
    const EWaitResult wr = pipe->handle.ReadBulkTriggeredEx(dst, trigger, max_count, n, timeout);

    if (read_count != nullptr)
    {
        *read_count = n;
    }

    return ToCWaitResult(wr);
}

size_t stk_pipe_tryread_bulk_triggered(stk_pipe_t *pipe, void *dst, size_t max_count)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.TryReadBulkTriggered(dst, max_count);
}

void stk_pipe_reset(stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    pipe->handle.Reset();
}

size_t stk_pipe_get_capacity(const stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.GetCapacity();
}

size_t stk_pipe_get_element_size(const stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.GetElementSize();
}

size_t stk_pipe_get_count(const stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.GetCount();
}

size_t stk_pipe_get_space(const stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.GetSpace();
}

bool stk_pipe_is_empty(const stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.IsEmpty();
}

bool stk_pipe_is_full(const stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.IsFull();
}

bool stk_pipe_is_storage_valid(const stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.IsStorageValid();
}

Pipe *stk_pipe_get_instance(stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return &pipe->handle;
}

// -----------------------------------------------------------------------------
// MessageQueue
// -----------------------------------------------------------------------------
struct stk_msgq_t
{
    stk_msgq_t(uint8_t *buf, size_t capacity, size_t msg_size)
        : handle(buf, capacity, msg_size)
    {}

    MessageQueue handle;
};

stk_msgq_t *stk_msgq_create(stk_msgq_mem_t *const membuf,
                            uint8_t *buf,
                            uint32_t buf_size,
                            size_t capacity,
                            size_t msg_size)
{
    STK_ASSERT(membuf != nullptr);
    STK_ASSERT(buf != nullptr);
    STK_ASSERT(capacity >= 1U);
    STK_ASSERT(msg_size >= 1U);
    STK_ASSERT(buf_size >= capacity * msg_size);

    stk_msgq_t *result = nullptr;
    if ((membuf != nullptr) && (buf_size >= (capacity * msg_size)))
    {
        result = ConstructIn<stk_msgq_t>(*membuf, buf, capacity, msg_size);
    }

    return result;
}

void stk_msgq_destroy(stk_msgq_t *mq)
{
    if (mq != nullptr)
    {
        mq->~stk_msgq_t();
    }
}

bool stk_msgq_put(stk_msgq_t *mq, const void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.Put(msg, timeout);
}

stk_wait_result_t stk_msgq_put_ex(stk_msgq_t *mq, const void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return ToCWaitResult(mq->handle.PutEx(msg, timeout));
}

bool stk_msgq_tryput(stk_msgq_t *mq, const void *msg)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.TryPut(msg);
}

bool stk_msgq_putfront(stk_msgq_t *mq, const void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.PutFront(msg, timeout);
}

stk_wait_result_t stk_msgq_putfront_ex(stk_msgq_t *mq, const void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return ToCWaitResult(mq->handle.PutFrontEx(msg, timeout));
}

bool stk_msgq_tryputfront(stk_msgq_t *mq, const void *msg)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.TryPutFront(msg);
}

bool stk_msgq_get(stk_msgq_t *mq, void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.Get(msg, timeout);
}

stk_wait_result_t stk_msgq_get_ex(stk_msgq_t *mq, void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return ToCWaitResult(mq->handle.GetEx(msg, timeout));
}

bool stk_msgq_tryget(stk_msgq_t *mq, void *msg)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.TryGet(msg);
}

bool stk_msgq_peek(stk_msgq_t *mq, void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.Peek(msg, timeout);
}

stk_wait_result_t stk_msgq_peek_ex(stk_msgq_t *mq, void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return ToCWaitResult(mq->handle.PeekEx(msg, timeout));
}

bool stk_msgq_trypeek(stk_msgq_t *mq, void *msg)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.TryPeek(msg);
}

bool stk_msgq_peekfront(stk_msgq_t *mq, void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.PeekFront(msg, timeout);
}

stk_wait_result_t stk_msgq_peekfront_ex(stk_msgq_t *mq, void *msg, stk_timeout_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return ToCWaitResult(mq->handle.PeekFrontEx(msg, timeout));
}

bool stk_msgq_trypeekfront(stk_msgq_t *mq, void *msg)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return mq->handle.TryPeekFront(msg);
}

void stk_msgq_reset(stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    mq->handle.Reset();
}

size_t stk_msgq_get_capacity(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.GetCapacity();
}

size_t stk_msgq_get_msg_size(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.GetMsgSize();
}

size_t stk_msgq_get_count(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.GetCount();
}

size_t stk_msgq_get_space(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.GetSpace();
}

bool stk_msgq_is_empty(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.IsEmpty();
}

bool stk_msgq_is_full(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.IsFull();
}

uint8_t *stk_msgq_get_buffer(stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.GetBuffer();
}

bool stk_msgq_is_storage_valid(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return mq->handle.IsStorageValid();
}

MessageQueue *stk_msgq_get_instance(stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return &mq->handle;
}

// -----------------------------------------------------------------------------
// RWMutex (Reader-Writer Lock)
// -----------------------------------------------------------------------------
struct stk_rwmutex_t
{
    sync::RWMutex handle;
};

stk_rwmutex_t *stk_rwmutex_create(stk_rwmutex_mem_t *const membuf)
{
    STK_ASSERT(membuf != nullptr);

    stk_rwmutex_t *result = nullptr;
    if (membuf != nullptr)
    {
        result = ConstructIn<stk_rwmutex_t>(*membuf);
    }

    return result;
}

void stk_rwmutex_destroy(stk_rwmutex_t *rw)
{
    if (rw != nullptr)
    {
        rw->~stk_rwmutex_t();
    }
}

void stk_rwmutex_read_lock(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    rw->handle.ReadLock();
}

bool stk_rwmutex_try_read_lock(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    return rw->handle.TryReadLock();
}

bool stk_rwmutex_timed_read_lock(stk_rwmutex_t *rw, stk_timeout_t timeout)
{
    STK_ASSERT(rw != nullptr);

    return rw->handle.TimedReadLock(timeout);
}

stk_wait_result_t stk_rwmutex_timed_read_lock_ex(stk_rwmutex_t *rw, stk_timeout_t timeout)
{
    STK_ASSERT(rw != nullptr);

    return ToCWaitResult(rw->handle.TimedReadLockEx(timeout));
}

void stk_rwmutex_read_unlock(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    rw->handle.ReadUnlock();
}

void stk_rwmutex_lock(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    rw->handle.Lock();
}

bool stk_rwmutex_trylock(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    return rw->handle.TryLock();
}

bool stk_rwmutex_timed_lock(stk_rwmutex_t *rw, stk_timeout_t timeout)
{
    STK_ASSERT(rw != nullptr);

    return rw->handle.TimedLock(timeout);
}

stk_wait_result_t stk_rwmutex_timed_lock_ex(stk_rwmutex_t *rw, stk_timeout_t timeout)
{
    STK_ASSERT(rw != nullptr);

    return ToCWaitResult(rw->handle.TimedLockEx(timeout));
}

void stk_rwmutex_unlock(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    rw->handle.Unlock();
}

sync::RWMutex *stk_rwmutex_get_instance(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    return &rw->handle;
}

// -----------------------------------------------------------------------------
// Barrier
// -----------------------------------------------------------------------------
struct stk_barrier_t
{
    explicit stk_barrier_t(uint32_t count) : handle(count)
    {}

    Barrier handle;
};

stk_barrier_t *stk_barrier_create(stk_barrier_mem_t *const membuf,
                                  uint32_t count)
{
    STK_ASSERT(membuf != nullptr);
    STK_ASSERT(count != 0U);

    stk_barrier_t *result = nullptr;
    if ((membuf != nullptr) && (count != 0U))
    {
        result = ConstructIn<stk_barrier_t>(*membuf, count);
    }

    return result;
}

void stk_barrier_destroy(stk_barrier_t *barrier)
{
    if (barrier != nullptr)
    {
        barrier->~stk_barrier_t();
    }
}

bool stk_barrier_wait(stk_barrier_t *barrier)
{
    STK_ASSERT(barrier != nullptr);

    return barrier->handle.Wait();
}

stk_barrier_result_t stk_barrier_wait_ex(stk_barrier_t *barrier)
{
    STK_ASSERT(barrier != nullptr);

    stk_barrier_result_t result;

    switch (barrier->handle.WaitEx())
    {
    case Barrier::BARRIER_LAST_ARRIVAL: result = STK_BARRIER_LAST_ARRIVAL; break;
    case Barrier::BARRIER_CANCELED:     result = STK_BARRIER_CANCELED;     break;
    default:                            result = STK_BARRIER_RELEASED;     break;
    }

    return result;
}

uint32_t stk_barrier_get_threshold(const stk_barrier_t *barrier)
{
    STK_ASSERT(barrier != nullptr);

    return barrier->handle.GetThreshold();
}

Barrier *stk_barrier_get_instance(stk_barrier_t *barrier)
{
    STK_ASSERT(barrier != nullptr);

    return &barrier->handle;
}

// =============================================================================
} // extern "C"
// =============================================================================
