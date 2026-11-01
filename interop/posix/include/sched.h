/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_SCHED_H_
#define STK_C_SCHED_H_

/*! \file     sched.h
    \brief    POSIX-named scheduling API (<sched.h>) for STK: sched_yield(), sched_get_priority_min/max()
              and struct sched_param. Implemented by stk_c_pthread.cpp (no dependency on the STK C API).

    \details  The kernel must use SwitchStrategyFixedPriority: \c sched_priority is the fixed priority level of the
              task (its weight), 0 is the lowest and MAX_PRIORITIES - 1 the highest (SwitchStrategyFP32: 0..31, the
              default level is PRIORITY_NORMAL = MAX_PRIORITIES / 2). The priority of a running thread can be
              changed (pthread_setschedparam(), pthread_setschedprio()) through stk::SetWeight(). The policy is
              always decided by the switch strategy: SCHED_OTHER, SCHED_FIFO and SCHED_RR are accepted and
              remembered for pthread_attr_setschedpolicy() / pthread_getschedparam() only, they do not change how
              the kernel schedules (preemptive, round-robin inside one priority level).
*/

#ifdef __cplusplus
extern "C" {
#endif

/*! \def       SCHED_OTHER / SCHED_FIFO / SCHED_RR
    \brief     Scheduling policy identifiers (informational, see the file description).
*/
#define SCHED_OTHER (0)
#define SCHED_FIFO  (1)
#define SCHED_RR    (2)

/*! \def       STK_C_SCHED_PRIORITY_MIN / STK_C_SCHED_PRIORITY_MAX / STK_C_SCHED_PRIORITY_DEFAULT
    \brief     Valid range of \c sched_priority (priority level = task weight of SwitchStrategyFixedPriority) and
               the level of threads which do not ask for one (PRIORITY_NORMAL).
    \note      Define STK_C_SCHED_PRIORITY_MAX (MAX_PRIORITIES - 1 of your SwitchStrategyFixedPriority instance,
               31 for SwitchStrategyFP32) before including this header, or in the build system. The default level
               follows from it: (MAX + 1) / 2, the same as SwitchStrategyFixedPriority::PRIORITY_NORMAL.
*/
#ifndef STK_C_SCHED_PRIORITY_MIN
    #define STK_C_SCHED_PRIORITY_MIN (0)
#endif
#ifndef STK_C_SCHED_PRIORITY_MAX
    #define STK_C_SCHED_PRIORITY_MAX (31)
#endif
#ifndef STK_C_SCHED_PRIORITY_DEFAULT
    #define STK_C_SCHED_PRIORITY_DEFAULT ((STK_C_SCHED_PRIORITY_MAX + 1) / 2)
#endif

/*! \brief     Scheduling parameters.
*/
struct sched_param
{
    int sched_priority; //!< Priority level (task weight), STK_C_SCHED_PRIORITY_MIN..STK_C_SCHED_PRIORITY_MAX, higher = more urgent.
};

/*! \brief     Yield the CPU to another ready task.
    \return    Always 0.
    \warning   ISR-unsafe.
*/
int sched_yield(void);

/*! \brief     Get the minimum priority of a scheduling policy.
    \param[in] policy: SCHED_OTHER, SCHED_FIFO or SCHED_RR.
    \return    STK_C_SCHED_PRIORITY_MIN, or -1 with errno = EINVAL for an unknown \a policy.
*/
int sched_get_priority_min(int policy);

/*! \brief     Get the maximum priority of a scheduling policy.
    \param[in] policy: SCHED_OTHER, SCHED_FIFO or SCHED_RR.
    \return    STK_C_SCHED_PRIORITY_MAX, or -1 with errno = EINVAL for an unknown \a policy.
*/
int sched_get_priority_max(int policy);

#ifdef __cplusplus
}
#endif

#endif /* STK_C_SCHED_H_ */
