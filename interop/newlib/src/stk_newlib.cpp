/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

/*! \file  stk_newlib.cpp
    \brief GCC Newlib retargetable locking implemented with stk::sync::Mutex / stk::sync::FastMutex.
    \see   stk_newlib.h

    MISRA C++:2008 deviations (all dictated by the Newlib retargetable locking interface):
    - Rule 17-0-1, 7-3-1: reserved identifiers and global-scope definitions (struct __lock,
      __lock___xxx, __retarget_lock_*) are mandated by Newlib.
    - Rule 3-9-2: __retarget_lock_try_acquire*() return \c int as mandated by Newlib.
    - Rule 18-0-1: <sys/lock.h> is the Newlib interface itself.
    - Rule 5-2-3: Close() downcasts IMutex to the concrete mutex type (known from the lock kind)
      to run the right destructor.
    Lock objects are placement-new constructed in static storage (no heap is used).
*/

#include <new>
#include <stdint.h>

#include "stk_newlib.h"
#include "sync/stk_sync_mutex.h"
#include "sync/stk_sync_fastmutex.h"
#include "memory/stk_memory_blockpool.h"

// ---------------------------------------------------------------------------
// Lock object
// ---------------------------------------------------------------------------

/*! \brief Lock object behind Newlib's opaque _LOCK_T (struct __lock *).

    Holds either a FastMutex (plain lock) or a Mutex (recursive lock), constructed in-place in
    \c storage. The same layout serves dynamic locks (taken from the static lock pool by
    __retarget_lock_init*) and Newlib's static locks (zero-initialized BSS objects, constructed
    lazily on first use).
*/
struct __lock
{
    enum EKind : uint8_t
    {
        KIND_NONE = 0, //!< not constructed (zero-initialized static lock)
        KIND_PLAIN,    //!< FastMutex
        KIND_RECURSIVE //!< Mutex
    };

    //! storage large enough for either mutex type
    static const size_t STORAGE_SIZE = (sizeof(stk::sync::FastMutex) > sizeof(stk::sync::Mutex)) ?
                                        sizeof(stk::sync::FastMutex) : sizeof(stk::sync::Mutex);

    EKind        kind;      //!< kind of the constructed mutex
    bool         dynamic;   //!< true if taken from the lock pool by __retarget_lock_init* (and therefore returned by close)
    stk::IMutex *mutex;     //!< constructed FastMutex (KIND_PLAIN) or Mutex (KIND_RECURSIVE), otherwise nullptr

    alignas(stk::sync::FastMutex) alignas(stk::sync::Mutex) uint8_t storage[STORAGE_SIZE];
};

namespace stk {
namespace newlib {
namespace {

// ---------------------------------------------------------------------------
// Pool of dynamic lock objects
// ---------------------------------------------------------------------------

typedef memory::BlockMemoryPool LockPool;

//! size of one pool block: the lock object rounded up to the pool block granularity
static const size_t LOCK_BLOCK_SIZE = LockPool::AlignBlockSize(sizeof(__lock));

STK_STATIC_ASSERT_DESC(((STK_NEWLIB_MAX_LOCKS > 0U) && (STK_NEWLIB_MAX_LOCKS <= LockPool::CAPACITY_MAX)),
    "STK_NEWLIB_MAX_LOCKS is out of the pool capacity range");
STK_STATIC_ASSERT_DESC(((LOCK_BLOCK_SIZE % alignof(__lock)) == 0U),
    "pool block size must preserve the alignment of the lock object");

//! storage of the pool blocks, zero-initialized BSS (no constructor involved)
alignas(__lock) alignas(void *) uint8_t s_LockPoolStorage[STK_NEWLIB_MAX_LOCKS * LOCK_BLOCK_SIZE];

//! raw storage for the pool object itself, the pool is constructed lazily on first use because
//! the first __retarget_lock_init*() call can happen before global constructors have run
struct LockPoolBuffer
{
    alignas(LockPool) uint8_t data[sizeof(LockPool)];
};
LockPoolBuffer s_LockPoolBuffer;
LockPool      *s_LockPool = nullptr;

/*! \brief Get the lock pool, constructing it on first use.
    \note  Guarded by a critical section so concurrent first use cannot construct it twice.
*/
LockPool *GetLockPool()
{
    const hw::CriticalSection::ScopedLock cs_;

    if (s_LockPool == nullptr)
    {
        s_LockPool = new (static_cast<void *>(s_LockPoolBuffer.data))
            LockPool(STK_NEWLIB_MAX_LOCKS, sizeof(__lock), s_LockPoolStorage, sizeof(s_LockPoolStorage));
    }

    return s_LockPool;
}

/*! \brief Check if the caller runs inside a STK task.
    \note  Before the kernel starts (startup code, static constructors) there is a single flow of
           execution, so locking is skipped. Adjust here if your port exposes a dedicated
           "kernel is running" query.
*/
bool IsTaskContext()
{
    bool running = false;

    IKernelService *const svc = IKernelService::GetInstance();

    if (svc != nullptr)
    {
        running = (svc->GetTid() != TID_NONE);
    }

    return running;
}

/*! \brief     Construct object T in-place inside a C memory container.
    \details   Compile-time checks that the container storage is large enough and the container
               is sufficiently aligned for T. No run-time alignment check is needed: \c storage is
               declared with alignas for every mutex type, and the container itself is either a
               BSS object or a block of the lock pool, whose block size is a multiple of
               alignof(__lock) (checked at compile time).
    \param[in] mem: Memory container, must expose a \c storage member.
    \param[in] args: Constructor arguments of T.
    \return    Pointer to the constructed object.
*/
template <typename T, typename Mem, typename... Args>
static inline T *ConstructIn(Mem &mem, Args &&... args)
{
    STK_STATIC_ASSERT_DESC((sizeof(T) <= sizeof(mem.storage)),
        "storage is too small for the C++ object");
    STK_STATIC_ASSERT_DESC((alignof(T) <= alignof(Mem)),
        "storage is not sufficiently aligned for the C++ object");

    return new (static_cast<void *>(mem.storage)) T(static_cast<Args &&>(args)...);
}

/*! \brief Construct the mutex of the requested kind (no-op if already constructed).
    \note  Guarded by a critical section so concurrent first use of a static lock cannot
           construct it twice.
*/
void Construct(_LOCK_T lock, __lock::EKind kind)
{
    const hw::CriticalSection::ScopedLock cs_;

    if (lock->kind == __lock::KIND_NONE)
    {
        if (kind == __lock::KIND_PLAIN)
        {
            lock->mutex = ConstructIn<sync::FastMutex>(*lock);
        }
        else
        {
            lock->mutex = ConstructIn<sync::Mutex>(*lock);
        }

        lock->kind = kind;
    }
}

/*! \brief Get the plain mutex, constructing it first for a lazily-initialized static lock.
*/
stk::IMutex *GetMutex(_LOCK_T lock, __lock::EKind kind)
{
    STK_ASSERT(lock != nullptr); // API contract: lock object must be valid

    if (lock->kind == __lock::KIND_NONE)
    {
        Construct(lock, kind);
    }

    STK_ASSERT(lock->kind == kind); // API contract: plain lock must use the non-recursive API

    return lock->mutex;
}

/*! \brief Take a lock object from the pool and construct the mutex in it.
    \note  Non-blocking: Newlib calls the init hooks while holding its own locks and possibly
           before the kernel starts, so waiting for a free block is never an option.
*/
void Init(_LOCK_T *lock, __lock::EKind kind)
{
    STK_ASSERT(lock != nullptr); // API contract: pointer to the lock object must be valid

    _LOCK_T new_lock = GetLockPool()->TryAllocT<__lock>();

    if (new_lock == nullptr)
    {
        // Newlib's init callbacks cannot report failure: STK_NEWLIB_MAX_LOCKS is too small
        STK_KERNEL_PANIC(KERNEL_PANIC_NO_MEM);
    }
    else
    {
        new_lock->kind    = __lock::KIND_NONE;
        new_lock->dynamic = true;
        new_lock->mutex   = nullptr;

        Construct(new_lock, kind);
    }

    *lock = new_lock;
}

/*! \brief Destroy a lock object and return it to the pool if it was taken from there.
*/
void Close(_LOCK_T lock)
{
    if (lock != nullptr)
    {
        if (lock->kind == __lock::KIND_PLAIN)
        {
            static_cast<sync::FastMutex *>(lock->mutex)->~FastMutex();
            lock->mutex = nullptr;
        }
        else if (lock->kind == __lock::KIND_RECURSIVE)
        {
            static_cast<sync::Mutex *>(lock->mutex)->~Mutex();
            lock->mutex = nullptr;
        }
        else
        {
            // never constructed, nothing to destroy
        }

        lock->kind = __lock::KIND_NONE;

        // static locks live in BSS and never go back to the pool
        if (lock->dynamic)
        {
            if (!GetLockPool()->Free(lock))
            {
                // the pointer does not belong to the pool memory region: the lock object was corrupted or foreign
                STK_KERNEL_PANIC(KERNEL_PANIC_BAD_MEMORY_REGION);
            }
        }
    }
}

/*! \brief Acquire the lock (blocking).
*/
void Acquire(_LOCK_T lock, __lock::EKind kind)
{
    if (IsTaskContext())
    {
        GetMutex(lock, kind)->Lock();
    }
}

/*! \brief Try to acquire the lock without blocking.
    \return Zero for success, non-zero if the lock cannot be acquired.
*/
int TryAcquire(_LOCK_T lock, __lock::EKind kind)
{
    int result = 0; // success

    if (IsTaskContext())
    {
        if (!GetMutex(lock, kind)->TryLock())
        {
            result = 1;
        }
    }

    return result;
}

/*! \brief Release the lock.
    \note  A lock acquired before the kernel started (no-op acquire) has nothing to release.
*/
void Release(_LOCK_T lock, __lock::EKind kind)
{
    if (IsTaskContext() && (lock != nullptr) && (lock->kind == kind) && (lock->mutex != nullptr))
    {
        lock->mutex->Unlock();
    }
}

} // namespace
} // namespace newlib
} // namespace stk

// ---------------------------------------------------------------------------
// Static locks used internally by Newlib
// ---------------------------------------------------------------------------

#if STK_NEWLIB_STATIC_LOCKS

extern "C" {

// zero-initialized (kind = KIND_NONE), constructed on first use
struct __lock __lock___sinit_recursive_mutex;
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

} // extern "C"

#endif // STK_NEWLIB_STATIC_LOCKS

// ---------------------------------------------------------------------------
// Newlib retargetable locking API
// ---------------------------------------------------------------------------

extern "C" {

void __retarget_lock_init(_LOCK_T *lock)
{
    stk::newlib::Init(lock, __lock::KIND_PLAIN);
}

void __retarget_lock_init_recursive(_LOCK_T *lock)
{
    stk::newlib::Init(lock, __lock::KIND_RECURSIVE);
}

void __retarget_lock_close(_LOCK_T lock)
{
    stk::newlib::Close(lock);
}

void __retarget_lock_close_recursive(_LOCK_T lock)
{
    stk::newlib::Close(lock);
}

void __retarget_lock_acquire(_LOCK_T lock)
{
    stk::newlib::Acquire(lock, __lock::KIND_PLAIN);
}

void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    stk::newlib::Acquire(lock, __lock::KIND_RECURSIVE);
}

int __retarget_lock_try_acquire(_LOCK_T lock)
{
    return stk::newlib::TryAcquire(lock, __lock::KIND_PLAIN);
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
    return stk::newlib::TryAcquire(lock, __lock::KIND_RECURSIVE);
}

void __retarget_lock_release(_LOCK_T lock)
{
    stk::newlib::Release(lock, __lock::KIND_PLAIN);
}

void __retarget_lock_release_recursive(_LOCK_T lock)
{
    stk::newlib::Release(lock, __lock::KIND_RECURSIVE);
}

} // extern "C"
