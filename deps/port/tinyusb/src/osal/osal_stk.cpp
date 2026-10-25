/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-FileCopyrightText: Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com> (STK port)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 *
 * Must be compiled as C++ (it uses stk::sync::FastMutex/Semaphore/MessageQueue,
 * stk::hw::CriticalSection, stk::ITask-adjacent helpers), but every function osal_stk.h
 * declares is extern "C" so it links cleanly against TinyUSB's plain-C .c sources - see the
 * file-level comment in osal_stk.h for why this port is split across two files at all.
 *
 * in_isr handling: STK's switch strategies re-evaluate scheduling internally as part of a
 * primitive's Signal()/Put() itself (see this project's other STK ports for the same point),
 * so unlike FreeRTOS's *FromISR() calls there is no separate "please reschedule" step for this
 * port to perform. Blocking operations (osal_mutex_lock, osal_semaphore_wait,
 * osal_queue_receive) are never called from ISR context by TinyUSB itself, so in_isr only
 * affects osal_semaphore_post/osal_queue_send below, where it selects a non-blocking primitive
 * instead of a blocking one - not a different STK API.
 */

#include <new> // for placement new
#include <cstdint>

#include "osal/osal.h" // for OSAL_TIMEOUT_WAIT_FOREVER
#include "osal_stk.h"

#if CFG_TUH_OSAL_STK_USE_CPP

/* STK includes. */
#include "stk.h"
#include "sync/stk_sync.h"

//----------------------------------------------------------------------------+
// Helper
//----------------------------------------------------------------------------+

template <typename T, typename Mem, typename... Args>
static inline T *StkConstructIn(Mem &mem, Args &&... args)
{
    static_assert((sizeof(T) <= sizeof(Mem)),
        "osal_*_def_t is too small for the C++ object");
    static_assert((alignof(T) <= alignof(Mem)),
        "osal_*_def_t is not sufficiently aligned for the C++ object");

    STK_ASSERT((reinterpret_cast<uintptr_t>(mem.storage) % alignof(T)) == 0U);

    return new (static_cast<void *>(mem.storage)) T(static_cast<Args &&>(args)...);
}

/* Counterpart of StkConstructIn(): takes a pointer to the same storage-holding struct (an
 * osal_*_def_t, or osal_queue_def_t::mq) and returns the object placement-constructed in it. */
template <typename T, typename Mem>
static inline T *StkCastTo(Mem *mem)
{
    return reinterpret_cast<T *>(mem->storage);
}

static inline stk::Timeout StkClampTimeout(uint32_t msec)
{
    const stk::Timeout msec_clamped = (msec > static_cast<uint32_t>(stk::WAIT_INFINITE)) ?
      stk::WAIT_INFINITE : static_cast<stk::Timeout>(msec);

    return msec_clamped;
}

/* OSAL_TIMEOUT_WAIT_FOREVER (UINT32_MAX) -> STK's WAIT_INFINITE; anything else -> STK ticks.
 *
 * A non-zero msec always yields at least 1 tick: with a coarse tick (e.g. 10 ms) the ms->ticks
 * conversion truncates sub-tick values to 0, and STK treats 0 as NO_WAIT / Sleep(0) - which neither
 * blocks nor yields. For osal_task_delay() that lets a higher-priority task starve the caller
 * (e.g. the polling loop in tuh_control_xfer(), which only ever calls osal_task_delay(1)); for
 * timed waits it would turn a short blocking timeout into a non-blocking poll. Only msec == 0
 * maps to 0 ticks. */
static inline stk::Timeout StkTimeoutFromMs(uint32_t msec)
{
  stk::Timeout to;

  if (msec == OSAL_TIMEOUT_WAIT_FOREVER) {
    to = stk::WAIT_INFINITE;
  } else {
    to = stk::GetTicksFromMsClampedToTimeout(StkClampTimeout(msec));

    if ((msec != 0U) && (to == 0)) {
      to = 1; /* round sub-tick durations up to one full tick */
    }
  }

  return to;
}

//----------------------------------------------------------------------------+
// TASK API
//----------------------------------------------------------------------------+

extern "C" osal_task_handle_t osal_task_get_current_handle(void)
{
  static_assert(sizeof(stk::TId) <= sizeof(uintptr_t),
                "stk::TId no longer fits in uintptr_t");
  static_assert(sizeof(stk::TId) <= sizeof(osal_task_handle_t),
                "stk::TId no longer fits in osal_task_handle_t (osal_task_handle_t)");

  const uintptr_t tid = static_cast<uintptr_t>(stk::GetTid());
  return reinterpret_cast<osal_task_handle_t>(tid);
}

extern "C" uint32_t osal_time_millis(void)
{
  return static_cast<uint32_t>(stk::GetTimeNowMs());
}

extern "C" void osal_task_delay(uint32_t msec)
{
  if (msec == 0U) {
    /* Sleep(0) is a no-op in STK; yield instead, mirroring FreeRTOS's vTaskDelay(0). Note: in
     * KERNEL_HRT mode Yield() signals job completion, so don't call this with 0 there. */
    stk::Yield();
  } else {
    stk::Sleep(StkTimeoutFromMs(msec));
  }
}

//----------------------------------------------------------------------------+
// Spinlock API
//----------------------------------------------------------------------------+

extern "C" void osal_spin_init(osal_spinlock_t *ctx)
{
  if (ctx != nullptr) {
    *ctx = stk::hw::CriticalSection::DEFAULT_SESSION;
  }
}

extern "C" void osal_spin_deinit(osal_spinlock_t *ctx)
{
  STK_UNUSED(ctx);
}

extern "C" void osal_spin_lock(osal_spinlock_t *ctx, bool in_isr)
{
  STK_UNUSED(in_isr); /* CriticalSection is safe from ISR context too - see osal_stk.h */

  /* Enter()'s return value encodes which privileged/unprivileged path was taken and MUST be
   * handed back to the matching Exit() - passing DEFAULT_SESSION there instead (as an earlier
   * version of this file did) restores the wrong path on unprivileged/TrustZone callers. */
  const stk::hw::CriticalSection::Session ses = stk::hw::CriticalSection::Enter();
  if (ctx != nullptr) {
    *ctx = ses;
  }
}

extern "C" void osal_spin_unlock(osal_spinlock_t *ctx, bool in_isr)
{
  STK_UNUSED(in_isr);

  const stk::hw::CriticalSection::Session ses = ((ctx != nullptr) ?
    static_cast<stk::hw::CriticalSection::Session>(*ctx) : stk::hw::CriticalSection::DEFAULT_SESSION);

  stk::hw::CriticalSection::Exit(ses);
}

//----------------------------------------------------------------------------+
// Semaphore API
//----------------------------------------------------------------------------+

/* stk::sync::Semaphore is a typedef of stk::sync::SemaphoreT<uint16_t>: a destructor call needs a name that
 * is found from this scope, so go through this alias (an unqualified ~Semaphore() would not be found here). */
typedef stk::sync::Semaphore StkSemaphore;

extern "C" osal_semaphore_t osal_semaphore_create(osal_semaphore_def_t *semdef)
{
  if (semdef != nullptr) {
      /* Binary semaphore, as in every other OSAL backend here: constructed at count 0
       * (not-yet-signaled) with max_count 1, so a second post before a wait cannot leave a stale
       * extra token. */
      STK_UNUSED(StkConstructIn<StkSemaphore>(*semdef, 0U, 1U));
  }

  return semdef;
}

extern "C" bool osal_semaphore_delete(osal_semaphore_t sem_hdl)
{
  StkCastTo<StkSemaphore>(sem_hdl)->~StkSemaphore();
  return true;
}


extern "C" bool osal_semaphore_post(osal_semaphore_t sem_hdl, bool in_isr)
{
  STK_UNUSED(in_isr); /* see file-level note: no separate ISR path needed */

  return StkCastTo<StkSemaphore>(sem_hdl)->TrySignal();
}

extern "C" bool osal_semaphore_wait(osal_semaphore_t sem_hdl, uint32_t msec)
{
  return StkCastTo<StkSemaphore>(sem_hdl)->Wait(StkTimeoutFromMs(msec));
}

extern "C" void osal_semaphore_reset(osal_semaphore_t sem_hdl)
{
  /* stk::sync::Semaphore has no dedicated reset op; drain any pending count/tokens back to 0
   * instead, mirroring what FreeRTOS's xQueueReset() does for a binary semaphore. */
  StkSemaphore *s = StkCastTo<StkSemaphore>(sem_hdl);
  while (s->TryWait()) {
    /* keep draining */
  }
}

//----------------------------------------------------------------------------+
// MUTEX API (backed by stk::sync::FastMutex: non-recursive, TinyUSB does not need recursion.
// Locking a FastMutex the caller already owns is a contract violation that panics with
// KERNEL_PANIC_SYNC_DEADLOCK - including a NO_WAIT try-lock)
//----------------------------------------------------------------------------+

extern "C" osal_mutex_t osal_mutex_create(osal_mutex_def_t *mdef)
{
  /* Size/alignment vs. OSAL_STK_MUTEX_STORAGE_WORDS is static_assert-ed inside StkConstructIn(). */
  if (mdef != nullptr) {
    STK_UNUSED(StkConstructIn<stk::sync::FastMutex>(*mdef));
  }

  return mdef;
}

extern "C" bool osal_mutex_delete(osal_mutex_t mutex_hdl)
{
  StkCastTo<stk::sync::FastMutex>(mutex_hdl)->~FastMutex();
  return true;
}

extern "C" bool osal_mutex_lock(osal_mutex_t mutex_hdl, uint32_t msec)
{
  return StkCastTo<stk::sync::FastMutex>(mutex_hdl)->TimedLock(StkTimeoutFromMs(msec));
}

extern "C" bool osal_mutex_unlock(osal_mutex_t mutex_hdl)
{
  StkCastTo<stk::sync::FastMutex>(mutex_hdl)->Unlock();
  return true;
}

//----------------------------------------------------------------------------+
// QUEUE API
//----------------------------------------------------------------------------+

extern "C" osal_queue_t osal_queue_create(osal_queue_def_t *qdef)
{
  /* Size/alignment vs. OSAL_STK_MSGQUEUE_STORAGE_WORDS is static_assert-ed inside StkConstructIn()
   * (against the qdef->mq storage struct - the ring buffer itself is qdef->buf). */
  if ((qdef == nullptr) || (qdef->depth > stk::sync::MessageQueue::CAPACITY_MAX)) {
    return NULL;
  }

  STK_UNUSED(StkConstructIn<stk::sync::MessageQueue>(qdef->mq,
    static_cast<uint8_t *>(qdef->buf), qdef->depth, qdef->item_sz));

  return qdef;
}

extern "C" bool osal_queue_delete(osal_queue_t qhdl)
{
  StkCastTo<stk::sync::MessageQueue>(&qhdl->mq)->~MessageQueue();
  return true;
}

extern "C" bool osal_queue_receive(osal_queue_t qhdl, void *data, uint32_t msec)
{
  return StkCastTo<stk::sync::MessageQueue>(&qhdl->mq)->Get(data, StkTimeoutFromMs(msec));
}

extern "C" bool osal_queue_send(osal_queue_t qhdl, void const *data, bool in_isr)
{
  stk::sync::MessageQueue *mq = StkCastTo<stk::sync::MessageQueue>(&qhdl->mq);
  bool result;

  /* Never block from ISR context, regardless of the caller's requested timeout - matching every
   * other OSAL backend here, whose FromISR() send variants are inherently non-blocking too. */
  if (in_isr) {
    result = mq->TryPut(data);
  } else {
    result = mq->Put(data, stk::WAIT_INFINITE);
  }
  
  return result;
}

extern "C" bool osal_queue_empty(osal_queue_t qhdl)
{
  return StkCastTo<stk::sync::MessageQueue>(&qhdl->mq)->GetCount() == 0U;
}

#else /* !CFG_TUH_OSAL_STK_USE_CPP */

#error "osal_stk.cpp is only needed for CFG_TUH_OSAL_STK_USE_CPP=1 (STK's C++ backend). " \
       "The default C backend is implemented entirely inline in osal_stk.h - remove this " \
       "file from your build, or define CFG_TUH_OSAL_STK_USE_CPP=1 before including osal_stk.h " \
       "if you specifically need the C++ backend."
       
#endif /* CFG_TUH_OSAL_STK_USE_CPP */
