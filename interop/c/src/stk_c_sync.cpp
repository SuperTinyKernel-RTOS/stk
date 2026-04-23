/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#include <cstddef> // for std::size_t

#include <stk_config.h>
#include <stk.h>
#include <sync/stk_sync.h>
#include <memory/stk_memory.h>

#include "stk_c.h"

using namespace stk;
using namespace stk::sync;

// ---------------------------------------------------------------------------
// C-interface
// ---------------------------------------------------------------------------
extern "C" {

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------
struct stk_mutex_t
{
    Mutex handle;
};

stk_mutex_t *stk_mutex_create(stk_mutex_mem_t *memory, uint32_t memory_size)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_mutex_t));
    if (memory_size < sizeof(stk_mutex_t))
        return nullptr;

    return new (memory->data) stk_mutex_t();
}

void stk_mutex_destroy(stk_mutex_t *mtx)
{
    if (mtx != NULL)
        mtx->~stk_mutex_t();
}

void stk_mutex_lock(stk_mutex_t *mtx)
{
    STK_ASSERT(mtx != NULL);

    mtx->handle.Lock();
}

bool stk_mutex_trylock(stk_mutex_t *mtx)
{
    STK_ASSERT(mtx != NULL);

    return mtx->handle.TryLock();
}

void stk_mutex_unlock(stk_mutex_t *mtx)
{
    STK_ASSERT(mtx != NULL);

    mtx->handle.Unlock();
}

bool stk_mutex_timed_lock(stk_mutex_t *mtx, int32_t timeout)
{
    STK_ASSERT(mtx != NULL);

    return mtx->handle.TimedLock(timeout);
}

// ---------------------------------------------------------------------------
// SpinLock
// ---------------------------------------------------------------------------
struct stk_spinlock_t
{
    sync::SpinLock handle;
};

stk_spinlock_t *stk_spinlock_create(stk_spinlock_mem_t *memory, uint32_t memory_size)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_spinlock_t));
    if (memory_size < sizeof(stk_spinlock_t))
        return nullptr;

    return new (memory->data) stk_spinlock_t();
}

void stk_spinlock_destroy(stk_spinlock_t *lock)
{
    if (lock != nullptr)
        lock->~stk_spinlock_t();
}

void stk_spinlock_lock(stk_spinlock_t *lock)
{
    STK_ASSERT(lock != nullptr);
    lock->handle.Lock();
}

bool stk_spinlock_trylock(stk_spinlock_t *lock)
{
    STK_ASSERT(lock != nullptr);
    return lock->handle.TryLock();
}

void stk_spinlock_unlock(stk_spinlock_t *lock)
{
    STK_ASSERT(lock != nullptr);
    lock->handle.Unlock();
}

// ---------------------------------------------------------------------------
// ConditionVariable
// ---------------------------------------------------------------------------
struct stk_cv_t
{
    ConditionVariable handle;
};

stk_cv_t *stk_cv_create(stk_cv_mem_t *memory, uint32_t memory_size)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_cv_t));
    if (memory_size < sizeof(stk_cv_t))
        return nullptr;

    return new (memory->data) stk_cv_t();
}

void stk_cv_destroy(stk_cv_t *cv)
{
    if (cv != nullptr)
        cv->~stk_cv_t();
}

bool stk_cv_wait(stk_cv_t *cv, stk_mutex_t *mtx, int32_t timeout)
{
    STK_ASSERT(cv != nullptr);
    STK_ASSERT(mtx != nullptr);

    return cv->handle.Wait(mtx->handle, timeout);
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

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------
struct stk_event_t
{
    stk_event_t(bool manual_reset) : handle(manual_reset)
    {}

    Event handle;
};

stk_event_t *stk_event_create(stk_event_mem_t *memory, uint32_t memory_size, bool manual_reset)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_event_t));
    if (memory_size < sizeof(stk_event_t))
        return nullptr;

    return new (memory->data) stk_event_t(manual_reset);
}

void stk_event_destroy(stk_event_t *ev)
{
    if (ev != nullptr)
        ev->~stk_event_t();
}

bool stk_event_wait(stk_event_t *ev, int32_t timeout)
{
    STK_ASSERT(ev != nullptr);

    return ev->handle.Wait(timeout);
}

bool stk_event_trywait(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    return ev->handle.TryWait();
}

void stk_event_set(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    ev->handle.Set();
}

void stk_event_reset(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    ev->handle.Reset();
}

void stk_event_pulse(stk_event_t *ev)
{
    STK_ASSERT(ev != nullptr);

    ev->handle.Pulse();
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------
struct stk_sem_t
{
    stk_sem_t(uint32_t initial_count) : handle(initial_count)
    {}

    Semaphore handle;
};

stk_sem_t *stk_sem_create(stk_sem_mem_t *memory, uint32_t memory_size, uint32_t initial_count)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_sem_t));
    if (memory_size < sizeof(stk_sem_t))
        return nullptr;

    return new (memory->data) stk_sem_t(initial_count);
}

void stk_sem_destroy(stk_sem_t *sem)
{
    if (sem != nullptr)
        sem->~stk_sem_t();
}

bool stk_sem_wait(stk_sem_t *sem, int32_t timeout)
{
    STK_ASSERT(sem != nullptr);

    return sem->handle.Wait(timeout);
}

void stk_sem_signal(stk_sem_t *sem)
{
    STK_ASSERT(sem != nullptr);

    sem->handle.Signal();
}

// ---------------------------------------------------------------------------
// EventFlags
// ---------------------------------------------------------------------------
struct stk_ef_t
{
    stk_ef_t(uint32_t initial_flags) : handle(initial_flags)
    {}

    EventFlags handle;
};

stk_ef_t *stk_ef_create(stk_ef_mem_t *memory, uint32_t memory_size, uint32_t initial_flags)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_ef_t));
    if (memory_size < sizeof(stk_ef_t))
        return nullptr;

    return new (memory->data) stk_ef_t(initial_flags);
}

void stk_ef_destroy(stk_ef_t *ef)
{
    if (ef != nullptr)
        ef->~stk_ef_t();
}

uint32_t stk_ef_set(stk_ef_t *ef, uint32_t flags)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Set(flags);
}

uint32_t stk_ef_clear(stk_ef_t *ef, uint32_t flags)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Clear(flags);
}

uint32_t stk_ef_get(stk_ef_t *ef)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Get();
}

uint32_t stk_ef_wait(stk_ef_t *ef, uint32_t flags, uint32_t options, int32_t timeout)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.Wait(flags, options, timeout);
}

uint32_t stk_ef_trywait(stk_ef_t *ef, uint32_t flags, uint32_t options)
{
    STK_ASSERT(ef != nullptr);

    return ef->handle.TryWait(flags, options);
}

// ---------------------------------------------------------------------------
// Pipe (template instantiation for stk_word_t, STK_PIPE_SIZE)
// ---------------------------------------------------------------------------
typedef PipeT<stk_word_t, STK_PIPE_SIZE> PipeX;

struct stk_pipe_t
{
    PipeX handle;
};

stk_pipe_t *stk_pipe_create(stk_pipe_mem_t *memory, uint32_t memory_size)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_pipe_t));
    if (memory_size < sizeof(stk_pipe_t))
        return nullptr;

    return new (memory->data) stk_pipe_t();
}

void stk_pipe_destroy(stk_pipe_t *pipe)
{
    if (pipe != nullptr)
        pipe->~stk_pipe_t();
}

bool stk_pipe_write(stk_pipe_t *pipe, stk_word_t data, int32_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.Write(data, timeout);
}

bool stk_pipe_read(stk_pipe_t *pipe, stk_word_t *data, int32_t timeout)
{
    STK_ASSERT(pipe != nullptr);
    STK_ASSERT(data != nullptr);

    return pipe->handle.Read(*data, timeout);
}

size_t stk_pipe_write_bulk(stk_pipe_t *pipe, const stk_word_t *src, size_t count, int32_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.WriteBulk(src, count, timeout);
}

size_t stk_pipe_read_bulk(stk_pipe_t *pipe, stk_word_t *dst, size_t count, int32_t timeout)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.ReadBulk(dst, count, timeout);
}

size_t stk_pipe_get_count(stk_pipe_t *pipe)
{
    STK_ASSERT(pipe != nullptr);

    return pipe->handle.GetCount();
}

// ---------------------------------------------------------------------------
// MessageQueue
// ---------------------------------------------------------------------------
struct stk_msgq_t
{
    stk_msgq_t(uint8_t *buf, size_t capacity, size_t msg_size)
        : handle(buf, capacity, msg_size)
    {}

    MessageQueue handle;
};

stk_msgq_t *stk_msgq_create(stk_msgq_mem_t *memory,
                             uint32_t        memory_size,
                             uint8_t        *buf,
                             uint32_t        buf_size,
                             size_t          capacity,
                             size_t          msg_size)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(buf != nullptr);
    STK_ASSERT(capacity >= 1U);
    STK_ASSERT(msg_size >= 1U);
    STK_ASSERT(memory_size >= sizeof(stk_msgq_t));
    STK_ASSERT(buf_size >= capacity * msg_size);

    if ((memory_size < sizeof(stk_msgq_t)) || (buf_size < (capacity * msg_size)))
        return nullptr;

    return new (memory) stk_msgq_t(buf, capacity, msg_size);
}

void stk_msgq_destroy(stk_msgq_t *mq)
{
    if (mq != nullptr)
        mq->~stk_msgq_t();
}

bool stk_msgq_put(stk_msgq_t *mq, const void *msg, int32_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return reinterpret_cast<stk_msgq_t *>(mq)->handle.Put(msg, timeout);
}

bool stk_msgq_tryput(stk_msgq_t *mq, const void *msg)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return reinterpret_cast<stk_msgq_t *>(mq)->handle.TryPut(msg);
}

bool stk_msgq_get(stk_msgq_t *mq, void *msg, int32_t timeout)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return reinterpret_cast<stk_msgq_t *>(mq)->handle.Get(msg, timeout);
}

bool stk_msgq_tryget(stk_msgq_t *mq, void *msg)
{
    STK_ASSERT(mq != nullptr);
    STK_ASSERT(msg != nullptr);

    return reinterpret_cast<stk_msgq_t *>(mq)->handle.TryGet(msg);
}

void stk_msgq_reset(stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    reinterpret_cast<stk_msgq_t *>(mq)->handle.Reset();
}

size_t stk_msgq_get_capacity(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return reinterpret_cast<const stk_msgq_t *>(mq)->handle.GetCapacity();
}

size_t stk_msgq_get_msg_size(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return reinterpret_cast<const stk_msgq_t *>(mq)->handle.GetMsgSize();
}

size_t stk_msgq_get_count(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return reinterpret_cast<const stk_msgq_t *>(mq)->handle.GetCount();
}

size_t stk_msgq_get_space(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return reinterpret_cast<const stk_msgq_t *>(mq)->handle.GetSpace();
}

bool stk_msgq_is_empty(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return reinterpret_cast<const stk_msgq_t *>(mq)->handle.IsEmpty();
}

bool stk_msgq_is_full(const stk_msgq_t *mq)
{
    STK_ASSERT(mq != nullptr);

    return reinterpret_cast<const stk_msgq_t *>(mq)->handle.IsFull();
}

// ---------------------------------------------------------------------------
// RWMutex (Reader-Writer Lock)
// ---------------------------------------------------------------------------
struct stk_rwmutex_t
{
    sync::RWMutex handle;
};

stk_rwmutex_t *stk_rwmutex_create(stk_rwmutex_mem_t *memory, uint32_t memory_size)
{
    STK_ASSERT(memory != nullptr);
    STK_ASSERT(memory_size >= sizeof(stk_rwmutex_t));
    if (memory_size < sizeof(stk_rwmutex_t))
        return nullptr;

    return (stk_rwmutex_t *)new (memory->data) stk_rwmutex_t{};
}

void stk_rwmutex_destroy(stk_rwmutex_t *rw)
{
    if (rw != nullptr)
        rw->~stk_rwmutex_t();
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

bool stk_rwmutex_timed_read_lock(stk_rwmutex_t *rw, int32_t timeout)
{
    STK_ASSERT(rw != nullptr);

    return rw->handle.TimedReadLock(timeout);
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

bool stk_rwmutex_timed_lock(stk_rwmutex_t *rw, int32_t timeout)
{
    STK_ASSERT(rw != nullptr);

    return rw->handle.TimedLock(timeout);
}

void stk_rwmutex_unlock(stk_rwmutex_t *rw)
{
    STK_ASSERT(rw != nullptr);

    rw->handle.Unlock();
}

} // extern "C"
