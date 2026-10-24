/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_TIME_H_
#define STK_TIME_H_

/*! \file  stk_time.h
    \brief Collection of time-related primitives (\c stk::time namespace).
*/

/*! \namespace stk::time
    \brief     Time-related primitives: periodic polling, elapsed-cycle measurement and software timers.

    OVERVIEW:
    ------------------------------------------------------------------------------------
    | Class                  | Header           | Description                                                                                                                  |
    | :--------------------- | :--------------- | :--------------------------------------------------------------------------------------------------------------------------- |
    | **PeriodicTrigger**    | stk_time_util.h  | Polling trigger: \c Poll() returns \c true once per period. Absolute next-fire tick, no drift. No kernel tasks needed.       |
    | **Stopwatch**          | stk_time_util.h  | Elapsed-cycle measurement with a caller-supplied time source. Independent of any hardware counter.                           |
    | **TimerHost**          | stk_time_timer.h | Software timer multiplexer: many timers on a fixed set of kernel tasks (task mode) or driven by the application (polled mode). |
    | **TimerHost::Timer**   | stk_time_timer.h | Abstract timer, override \c OnExpired(). One-shot or periodic.                                                               |

    KERNEL REQUIREMENTS:
    ------------------------------------------------------------------------------------
    | Primitive                  | Requirements                                                                                                         |
    | :------------------------- | :------------------------------------------------------------------------------------------------------------------- |
    | **PeriodicTrigger**        | Kernel tick counter (\c GetTicks()). No tasks, no \c KERNEL_SYNC.                                                    |
    | **Stopwatch**              | None. The time source is supplied by the caller.                                                                     |
    | **TimerHost** (task mode)  | \c KERNEL_SYNC and \c KERNEL_DYNAMIC (the host tasks exit on \c Shutdown()). Not usable with \c KERNEL_HRT. Occupies \c 1 + STK_TIMER_THREADS_COUNT task slots. |
    | **TimerHost** (polled mode)| \c KERNEL_SYNC. No tasks. Works with \c KERNEL_HRT.                                                                  |

    TIMERHOST MODES:
    ------------------------------------------------------------------------------------
    - **Task mode** (\c Initialize()): one tick task plus \c STK_TIMER_THREADS_COUNT handler tasks serve all timers,
      \c OnExpired() runs in a handler task. The weights and stacks of these tasks are set with \c TimerHost::Config.
    - **Polled mode** (\c InitializePolled()): no kernel tasks. The application calls \c Process() periodically from
      one context (a task loop or an HRT job). Expired callbacks run inline from \c Process(), which never blocks and
      returns the ticks until the earliest deadline (\c NO_WAIT if a timer is overdue, \c WAIT_INFINITE if none is active).

    ISR / CONTEXT GUIDELINES:
    ------------------------------------------------------------------------------------
    | Primitive            | Context                                                                                                                              |
    | :------------------- | :----------------------------------------------------------------------------------------------------------------------------------- |
    | **PeriodicTrigger**  | \c Restart(), \c Poll(), \c GetPeriod(), \c SetPeriod(): one context only (a task or an ISR), not thread-safe.                      |
    | **Stopwatch**        | \c Start(), \c Update(): one context only (a task or an ISR), not thread-safe. The supplied time source must be callable there.     |
    | **TimerHost**        | \c None: \c Initialize(), \c InitializePolled(), \c Process() and all commands (\c Start(), \c Stop(), \c Reset(), \c Restart(), \c StartOrReset(), \c SetPeriod(), \c Shutdown()) are called from task context. |
    | **TimerHost::Timer** | \c None: \c OnExpired() is never called from an ISR, it runs in a handler task (task mode) or inline in the caller of \c Process() (polled mode). |

    NOTE:
    - **TimerHost** commands are asynchronous: they are queued and applied by the tick task (task mode) or by the next
      \c Process() call (polled mode). They return \c false if the command queue is full.
    - **Weights** (\c TimerHost::Config) are strategy-specific and forwarded to the kernel unchanged. With
      \c SwitchStrategyPreemptionThreshold they must be set explicitly (\c MakeWeight(p, p)): \c DEFAULT_WEIGHT decodes as
      priority 0 / threshold 1 and every timer task would be held while it runs.
    - **Polled mode** resolution is the call period of \c Process(): a timer fires on the first call at or after its
      deadline. An overdue periodic timer fires once per call until its schedule has caught up (no drift).
    - **Callbacks** run in the caller's context and count against its time budget. In an HRT job the worst-case time
      of all callbacks which can expire in one \c Process() call is part of the job's WCET.

    WARNING:
    - **TimerHost** in task mode registers soft real-time tasks: a \c KERNEL_HRT kernel rejects them with
      \c ADD_FAIL_INVALID. Use polled mode there.
    - Do not call \c Process() concurrently or from \c OnExpired(). \c OnExpired() should complete quickly: a long
      callback delays all other expired timers (and, in polled mode, the caller of \c Process()).
    - Timer objects must be allocated in static storage and outlive the \c TimerHost, or be stopped before destruction.
*/
namespace stk {
namespace time {
} // namespace time
} // namespace stk

#include "stk_time_timer.h"
#include "stk_time_util.h"

#endif /* STK_TIME_H_ */
