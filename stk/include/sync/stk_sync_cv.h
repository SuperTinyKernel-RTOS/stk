/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_CV_H_
#define STK_SYNC_CV_H_

#include "stk_sync_cs.h"

/*! \file  stk_sync_cv.h
    \brief Implementation of synchronization primitive: ConditionVariable.
*/

namespace stk {
namespace sync {

/*! \class ConditionVariable
    \brief Condition Variable primitive for signaling between tasks based on specific predicates.

    Condition Variables are synchronization primitives that enable tasks to wait until a
    particular condition (predicate) is met. They must be used in conjunction with a
    \a Mutex to protect the shared state.

    \note  This implementation follows the Monitor pattern: the \c Wait() operation
           atomically releases the associated \a Mutex and suspends the task. Upon
           waking (via signal or timeout), the \a Mutex is automatically re-acquired
           before the function returns.

    \code
    // Usage example: Producer-Consumer pattern
    stk::sync::Mutex             g_Mtx;
    stk::sync::ConditionVariable g_Cond;
    std::queue<int>              g_Queue;

    void Task_Consumer() {
        g_Mtx.Lock();
        while (g_Queue.empty()) {
            // releases g_Mtx and sleeps; re-acquires g_Mtx upon waking
            if (!g_Cond.Wait(g_Mtx, 1000)) {
                break; // timeout handling
            }
        }
        if (!g_Queue.empty()) {
            int data = g_Queue.front();
            g_Queue.pop();
        }
        g_Mtx.Unlock();
    }

    void Task_Producer() {
        g_Mtx.Lock();
        g_Queue.push(42);
        // wake one waiting task
        g_Cond.NotifyOne();
        g_Mtx.Unlock();
    }
    \endcode

    \see  Mutex, ISyncObject, IWaitObject, IKernelService::Wait
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
class ConditionVariable : private ISyncObject, public ITraceable
{
public:
    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error (dangling waiters).
                   An assertion is triggered in debug builds.
    */
    ~ConditionVariable() { STK_ASSERT(m_wait_list.IsEmpty()); }

    /*! \brief     Wait for a signal.
        \details   Atomically releases mutex and blocks the task.
                   The mutex is re-acquired before the function returns.
        \param[in] mutex: Locked mutex protecting the shared state/condition.
        \param[in] timeout: Maximum time to wait (ticks).
        \return    True if signaled, false if timeout occurred.
        \warning   ISR-unsafe unless timeout is NO_WAIT.
    */
    bool Wait(IMutex &mutex, Timeout timeout = WAIT_INFINITE);

    /*! \brief     Wake one waiting task.
        \note      ISR-safe.
    */
    void NotifyOne();

    /*! \brief     Wake all waiting tasks.
        \note      ISR-safe.
    */
    void NotifyAll();

private:
    bool Tick();
};

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

inline bool ConditionVariable::Wait(IMutex &mutex, Timeout timeout)
{
    if (timeout == NO_WAIT)
        return false;

    // not supported inside ISR, calls Wait
    STK_ASSERT(!hw::IsInsideISR());

    return !IKernelService::GetInstance()->Wait(this, &mutex, timeout)->IsTimeout();
}

// ---------------------------------------------------------------------------
// NotifyOne
// ---------------------------------------------------------------------------

inline void ConditionVariable::NotifyOne()
{
    ScopedCriticalSection __cs;
    WakeOne();
}

// ---------------------------------------------------------------------------
// NotifyAll
// ---------------------------------------------------------------------------

inline void ConditionVariable::NotifyAll()
{
    ScopedCriticalSection __cs;
    WakeAll();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

inline bool ConditionVariable::Tick()
{
    // required for multi-core CPU and multiple instances of STK (one per core)
#if (_STK_ARCH_CPU_COUNT > 1)
    ScopedCriticalSection __cs;
#endif

    return ISyncObject::Tick();
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_CV_H_ */
