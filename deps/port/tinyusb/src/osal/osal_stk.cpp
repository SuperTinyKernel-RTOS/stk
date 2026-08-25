/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-FileCopyrightText: Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com> (STK port)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 *
 * Must be compiled as C++ (it uses stk::sync::Mutex/Semaphore/MessageQueue,
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

#include <new>
#include <cstdint>

#include "osal/osal.h" // for OSAL_TIMEOUT_WAIT_FOREVER
#include "osal_stk.h"

#if CFG_TUH_OSAL_STK_USE_CPP

/* STK includes. */
#include "stk.h"
#include "sync/stk_sync.h"

//----------------------------------------------------------------------------+
// TASK API
//----------------------------------------------------------------------------+

extern "C" osal_task_handle_t osal_task_get_current_handle(void)
{
  static_assert(sizeof(stk::TId) <= sizeof(osal_task_handle_t),
                "stk::TId no longer fits in osal_task_handle_t (uintptr_t) - widen it");
  return static_cast<osal_task_handle_t>(stk::GetTid());
}

extern "C" uint32_t osal_time_millis(void)
{
  return static_cast<uint32_t>(stk::GetTimeNowMs());
}

extern "C" void osal_task_delay(uint32_t msec)
{
  const stk::Timeout ms_clamped =
      (msec > static_cast<uint32_t>(INT32_MAX)) ? INT32_MAX : static_cast<stk::Timeout>(msec);
  stk::SleepMs(ms_clamped);
}

//----------------------------------------------------------------------------+
// Small helper
//----------------------------------------------------------------------------+

/* OSAL_TIMEOUT_WAIT_FOREVER (UINT32_MAX) -> STK's WAIT_INFINITE; anything else -> STK ticks. */
static inline stk::Timeout osal_stk_timeout_from_ms(uint32_t msec)
{
  if (msec == OSAL_TIMEOUT_WAIT_FOREVER) {
    return stk::WAIT_INFINITE;
  }
  const stk::Timeout ms_clamped =
      (msec > static_cast<uint32_t>(INT32_MAX)) ? INT32_MAX : static_cast<stk::Timeout>(msec);
  return stk::GetTicksFromMsClampedToTimeout(ms_clamped);
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
  (void)ctx;
}

extern "C" void osal_spin_lock(osal_spinlock_t *ctx, bool in_isr)
{
  (void)in_isr; /* CriticalSection is safe from ISR context too - see osal_stk.h */

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
  (void)in_isr;

  const stk::hw::CriticalSection::Session ses =
      ((ctx != nullptr) ? static_cast<stk::hw::CriticalSection::Session>(*ctx)
                        : stk::hw::CriticalSection::DEFAULT_SESSION);
  stk::hw::CriticalSection::Exit(ses);
}

//----------------------------------------------------------------------------+
// Semaphore API
//----------------------------------------------------------------------------+

extern "C" osal_semaphore_t osal_semaphore_create(osal_semaphore_def_t *semdef)
{
  static_assert(sizeof(stk::sync::Semaphore) <= (OSAL_STK_SEMAPHORE_STORAGE_WORDS * sizeof(stk::Word)),
                "stk::sync::Semaphore no longer fits OSAL_STK_SEMAPHORE_STORAGE_WORDS - raise it");
  static_assert(alignof(stk::sync::Semaphore) <= alignof(void *),
                "stk::sync::Semaphore's alignment exceeds osal_semaphore_def_t's - widen its union");

  /* Always constructed at count 0 (not-yet-signaled), matching every other OSAL backend here. */
  (void) new (semdef->storage) stk::sync::Semaphore(0U);
  return semdef;
}

extern "C" bool osal_semaphore_delete(osal_semaphore_t sem_hdl)
{
  reinterpret_cast<stk::sync::Semaphore *>(sem_hdl->storage)->~Semaphore();
  return true;
}

extern "C" bool osal_semaphore_post(osal_semaphore_t sem_hdl, bool in_isr)
{
  (void) in_isr; /* see file-level note: no separate ISR path needed */

  return reinterpret_cast<stk::sync::Semaphore *>(sem_hdl->storage)->TrySignal();
}

extern "C" bool osal_semaphore_wait(osal_semaphore_t sem_hdl, uint32_t msec)
{
  return reinterpret_cast<stk::sync::Semaphore *>(sem_hdl->storage)->Wait(osal_stk_timeout_from_ms(msec));
}

extern "C" void osal_semaphore_reset(osal_semaphore_t sem_hdl)
{
  /* stk::sync::Semaphore has no dedicated reset op; drain any pending count/tokens back to 0
   * instead, mirroring what FreeRTOS's xQueueReset() does for a binary semaphore. */
  stk::sync::Semaphore *s = reinterpret_cast<stk::sync::Semaphore *>(sem_hdl->storage);
  while (s->TryWait()) {
    /* keep draining */
  }
}

//----------------------------------------------------------------------------+
// MUTEX API (STK's Mutex is already recursive; TinyUSB never relies on that, 
// but it's harmless)
//----------------------------------------------------------------------------+

extern "C" osal_mutex_t osal_mutex_create(osal_mutex_def_t *mdef)
{
  static_assert(sizeof(stk::sync::Mutex) <= (OSAL_STK_MUTEX_STORAGE_WORDS * sizeof(stk::Word)),
                "stk::sync::Mutex no longer fits OSAL_STK_MUTEX_STORAGE_WORDS - raise it");
  static_assert(alignof(stk::sync::Mutex) <= alignof(void *),
                "stk::sync::Mutex's alignment exceeds osal_mutex_def_t's - widen its union");

  (void)new (mdef->storage) stk::sync::Mutex();
  return mdef;
}

extern "C" bool osal_mutex_delete(osal_mutex_t mutex_hdl)
{
  reinterpret_cast<stk::sync::Mutex *>(mutex_hdl->storage)->~Mutex();
  return true;
}

extern "C" bool osal_mutex_lock(osal_mutex_t mutex_hdl, uint32_t msec)
{
  return reinterpret_cast<stk::sync::Mutex *>(mutex_hdl->storage)->TimedLock(osal_stk_timeout_from_ms(msec));
}

extern "C" bool osal_mutex_unlock(osal_mutex_t mutex_hdl)
{
  reinterpret_cast<stk::sync::Mutex *>(mutex_hdl->storage)->Unlock();
  return true;
}

//----------------------------------------------------------------------------+
// QUEUE API
//----------------------------------------------------------------------------+

extern "C" osal_queue_t osal_queue_create(osal_queue_def_t *qdef)
{
  static_assert(sizeof(stk::sync::MessageQueue) <= (OSAL_STK_MSGQUEUE_STORAGE_WORDS * sizeof(stk::Word)),
                "stk::sync::MessageQueue no longer fits OSAL_STK_MSGQUEUE_STORAGE_WORDS - raise it");
  static_assert(alignof(stk::sync::MessageQueue) <= alignof(void *),
                "stk::sync::MessageQueue's alignment exceeds osal_queue_def_t.mq's - widen its union");

  if (qdef->depth > stk::sync::MessageQueue::CAPACITY_MAX) {
    return NULL;
  }

  (void)new (qdef->mq.storage) stk::sync::MessageQueue(
      static_cast<uint8_t *>(qdef->buf), qdef->depth, qdef->item_sz);
  return qdef;
}

extern "C" bool osal_queue_delete(osal_queue_t qhdl)
{
  reinterpret_cast<stk::sync::MessageQueue *>(qhdl->mq.storage)->~MessageQueue();
  return true;
}

extern "C" bool osal_queue_receive(osal_queue_t qhdl, void *data, uint32_t msec)
{
  return reinterpret_cast<stk::sync::MessageQueue *>(qhdl->mq.storage)
      ->Get(data, osal_stk_timeout_from_ms(msec));
}

extern "C" bool osal_queue_send(osal_queue_t qhdl, void const *data, bool in_isr)
{
  stk::sync::MessageQueue *mq = reinterpret_cast<stk::sync::MessageQueue *>(qhdl->mq.storage);
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
  return reinterpret_cast<stk::sync::MessageQueue *>(qhdl->mq.storage)->GetCount() == 0U;
}

#else /* !CFG_TUH_OSAL_STK_USE_CPP */

#error "osal_stk.cpp is only needed for CFG_TUH_OSAL_STK_USE_CPP=1 (STK's C++ backend). " \
       "The default C backend is implemented entirely inline in osal_stk.h - remove this " \
       "file from your build, or define CFG_TUH_OSAL_STK_USE_CPP=1 before including osal_stk.h " \
       "if you specifically need the C++ backend."
       
#endif /* CFG_TUH_OSAL_STK_USE_CPP */
