/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_SYNC_CS_H_
#define STK_SYNC_CS_H_

#include "stk_helper.h"

/*! \file  stk_sync_cs.h
    \brief Implementation of synchronization primitive: stk::sync::ScopedCriticalSection.
*/

namespace stk {
namespace sync {

/*! \class ScopedCriticalSection
    \brief RAII-style low-level synchronization primitive for atomic code execution.
           Used as building brick for other stk::sync classes, consider using
           hw::CriticalSection::ScopedLock implementation instead.

    Disables interrupts on caller's CPU core and guards from access by another CPU
    core in case of multi-core system. Enters a critical section upon construction
    and exits automatically when the object goes out of scope.

    \code
    // Example: Protecting a global shared variable
    uint32_t g_SharedCounter = 0;

    void IncrementCounter() {
        {                                                // code execution scope starts here
            stk::sync::CriticalSection::ScopedLock __cs; // critical section starts here
            g_SharedCounter++;                           // atomic update
        }                                                // critical section ends here (RAII)
    }
    \endcode

    \note  Global Impact: Use with extreme care. This primitive has a global effect
           on the system by preventing preemption. Long-running code inside a critical
           section will increase interrupt latency and may cause other tasks to miss
           their deadlines.
    \note  Unlike higher-level synchronization primitives, this is always available
           and does not depend on the \a KERNEL_SYNC configuration.
    \note  ISR-safe, the only safe primitive along with hw::CriticalSection for guarding
           code which can be accessed by ISR or another CPU core.
    \see   IMutex, hw::CriticalSection
*/
class ScopedCriticalSection : private IMutex
{
    friend class Event;
    friend class EventFlags;
    friend class Mutex;
    friend class RWMutex;
    friend class Semaphore;
    template <typename T, size_t N> friend class Pipe;

public:
    /*! \brief Enters critical section.
    */
    explicit ScopedCriticalSection() { Lock(); }

    /*! \brief Exits critical section.
        \note  MISRA deviation: [STK-DEV-005] Rule 10-3-2.
    */
    ~ScopedCriticalSection() { Unlock(); }

private:
    STK_NONCOPYABLE_CLASS(ScopedCriticalSection);

    void Lock() { hw::CriticalSection::Enter(); }
    void Unlock() { hw::CriticalSection::Exit(); }
};

} // namespace sync
} // namespace stk

#endif /* STK_SYNC_CS_H_ */
