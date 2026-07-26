/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
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
    \brief Implementation of synchronization primitive: stk::sync::ConditionVariable.
*/

namespace stk {
namespace sync {

/*! \class ConditionVariable
    \brief Condition Variable primitive for signaling between tasks based on specific predicates.

    Condition Variables are synchronization primitives that enable tasks to wait until a
    particular condition (predicate) is met. They must be used in conjunction with an
    \a IMutex-compatible lock to protect the shared state.

    \note  This implementation follows the Monitor pattern: the \c Wait() operation
           atomically releases the associated lock and suspends the task. Upon
           waking (via signal or timeout), the lock is automatically re-acquired
           before the function returns.

    \code
    // Usage example: Producer-Consumer pattern using a fixed-size pipe
    stk::sync::Mutex                        g_Mtx;
    stk::sync::ConditionVariable            g_Cond;
    stk::sync::Pipe<int, 16>                g_Pipe;

    void Task_Consumer() {
        g_Mtx.Lock();
        while (g_Pipe.IsEmpty()) {
            // releases g_Mtx and sleeps; re-acquires g_Mtx upon waking
            if (!g_Cond.Wait(g_Mtx, 1000)) {
                break; // timeout handling
            }
        }
        int data;
        if (g_Pipe.Read(data)) {
            // ... process data ...
        }
        g_Mtx.Unlock();
    }

    void Task_Producer() {
        g_Mtx.Lock();
        g_Pipe.Write(42);
        // wake one waiting task
        g_Cond.NotifyOne();
        g_Mtx.Unlock();
    }
    \endcode

    \see  Mutex, ISyncObject, IWaitObject, IKernelService::Wait
    \note Only available when kernel is compiled with \a KERNEL_SYNC mode enabled.
*/
class ConditionVariable final : private SyncObjectBase, public ITraceable
{
public:
    explicit ConditionVariable()
    {}

    /*! \brief     Destructor.
        \note      If tasks are still waiting at destruction time it is considered a logical error
                   (dangling waiters). An assertion is triggered in debug builds.
        \note      MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    STK_VIRT_DTOR  ~ConditionVariable()
    {
        STK_ASSERT(m_wait_list.IsEmpty()); // API contract: must not be destroyed with waiting tasks
    }

    /*! \brief     Wait for a signal.
        \details   Atomically releases \a mutex and blocks the calling task. The \a mutex is
                   re-acquired before the function returns, regardless of whether the wake
                   was caused by a signal or a timeout.
        \param[in] mutex: An \a IMutex-compatible lock that must be held by the calling task
                   before Wait() is called. The kernel releases it atomically during suspension
                   and re-acquires it on wake.
        \param[in] timeout_ticks: Maximum time to wait (ticks). Use \a WAIT_INFINITE to block
                   indefinitely, \a NO_WAIT to return immediately without blocking.
        \return    \c true if signaled, \c false if timeout occurred or \a NO_WAIT was passed.
        \warning   ISR-safe only with timeout_ticks=NO_WAIT, ISR-unsafe otherwise.
    */
    bool Wait(IMutex &mutex, Timeout timeout_ticks = WAIT_INFINITE);

    /*! \brief     Wake one waiting task.
        \note      ISR-safe.
    */
    void NotifyOne();

    /*! \brief     Wake one waiting task.
        \warning   Caller must lock Critical Section or use ScopedCriticalSection prior calling this function.
        \note      ISR-safe.
    */
    void NotifyOne_CS();

    /*! \brief     Wake all waiting tasks.
        \note      ISR-safe.
    */
    void NotifyAll();

    /*! \brief     Wake all waiting tasks.
        \warning   Caller must lock Critical Section or use ScopedCriticalSection prior calling this function.
        \note      ISR-safe.
    */
    void NotifyAll_CS();

private:
    STK_NONCOPYABLE_CLASS(ConditionVariable);
};

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

inline bool ConditionVariable::Wait(IMutex &mutex, Timeout timeout_ticks)
{
    // API contract: mutex must be locked by the calling task before Wait() is called.
    // The kernel releases it atomically during suspension and re-acquires it on wake.
    bool success = false;

    if (timeout_ticks != NO_WAIT)
    {
        STK_ASSERT(!hw::IsInsideISR()); // API contract: caller must not be in ISR if timeout_ticks!=NO_WAIT
        
        success = (IKernelService::GetInstance()->Wait(this, &mutex, timeout_ticks) == WAIT_RESULT_SIGNAL);
    }

    return success;
}

// ---------------------------------------------------------------------------
// NotifyOne
// ---------------------------------------------------------------------------

inline void ConditionVariable::NotifyOne()
{
    const ScopedCriticalSection cs_;
    NotifyOne_CS();
}

// ---------------------------------------------------------------------------
// NotifyOne_CS
// ---------------------------------------------------------------------------

inline void ConditionVariable::NotifyOne_CS()
{
    WakeOne(); // wakes the first task in the wait list (FIFO order), if any
}

// ---------------------------------------------------------------------------
// NotifyAll
// ---------------------------------------------------------------------------

inline void ConditionVariable::NotifyAll()
{
    const ScopedCriticalSection cs_;
    NotifyAll_CS(); // wakes all tasks in the wait list simultaneously
}

// ---------------------------------------------------------------------------
// NotifyAll_CS
// ---------------------------------------------------------------------------

inline void ConditionVariable::NotifyAll_CS()
{
    WakeAll(); // wakes all tasks in the wait list simultaneously
}

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_CV_H_ */
