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

    \details  The kernel must use sched::SwitchStrategyFixedPriority: \c sched_priority is the fixed priority level of the
              task (its weight), 0 is the lowest and MAX_PRIORITIES - 1 the highest (sched::SwitchStrategyFP32: 0..31, the
              default level is PRIORITY_NORMAL = MAX_PRIORITIES / 2). The priority of a running thread can be
              changed (pthread_setschedparam(), pthread_setschedprio()) through stk::SetWeight(). The policy is
              always decided by the switch strategy: SCHED_OTHER, SCHED_FIFO and SCHED_RR are accepted and
              remembered for pthread_attr_setschedpolicy() / pthread_getschedparam() only, they do not change how
              the kernel schedules (preemptive, round-robin inside one priority level).
*/

/* same newlib guard as in stk_c_pthread.h: <sys/types.h> (pid_t) must not define pthread types of its own */
#ifndef _SYS__PTHREADTYPES_H_
    #define _SYS__PTHREADTYPES_H_
    #define STK_C_PTHREADTYPES_GUARD
#endif

#include <sys/types.h>

struct timespec; /* complete type: <time.h> */

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
    \brief     Valid range of \c sched_priority (priority level = task weight of sched::SwitchStrategyFixedPriority) and
               the level of threads which do not ask for one (PRIORITY_NORMAL).
    \note      Define STK_C_SCHED_PRIORITY_MAX (MAX_PRIORITIES - 1 of your sched::SwitchStrategyFixedPriority instance,
               31 for sched::SwitchStrategyFP32) before including this header, or in the build system. The default level
               follows from it: (MAX + 1) / 2, the same as sched::SwitchStrategyFixedPriority::PRIORITY_NORMAL.
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

/* ---- process-level scheduling calls: "process" = the calling thread (see below) ---------------------------------- */

/*! \brief     Get the scheduling policy of a thread.
    \param[in] pid: 0 or the value of getpid() select the CALLING thread (STK has one process, its threads have no
               pids); any other value fails with ESRCH.
    \return    The policy (SCHED_OTHER / SCHED_FIFO / SCHED_RR), or -1 with errno = ESRCH.
*/
int sched_getscheduler(pid_t pid);

/*! \brief     Set the policy and priority of the calling thread (see pthread_setschedparam()).
    \return    The previous policy, or -1 with errno = EINVAL, ESRCH or ENOTSUP.
*/
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);

/*! \brief     Get the priority of the calling thread.
    \return    0, or -1 with errno = EINVAL or ESRCH.
*/
int sched_getparam(pid_t pid, struct sched_param *param);

/*! \brief     Set the priority of the calling thread, keeping its policy (see pthread_setschedprio()).
    \return    0, or -1 with errno = EINVAL, ESRCH or ENOTSUP.
*/
int sched_setparam(pid_t pid, const struct sched_param *param);

/*! \brief     Get the round-robin time slice: one kernel tick (stk::GetTickResolution()).
    \param[out] tp: Receives the interval.
    \return    0, or -1 with errno = EINVAL or ESRCH.
*/
int sched_rr_get_interval(pid_t pid, struct timespec *tp);

#ifdef __cplusplus
}
#endif

#endif /* STK_C_SCHED_H_ */
