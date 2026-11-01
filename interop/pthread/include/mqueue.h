/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_C_MQUEUE_H_
#define STK_C_MQUEUE_H_

/*! \file     mqueue.h
    \brief    POSIX message queues (<mqueue.h>) for STK. Implemented by stk_c_pthread.cpp on top of
              stk::sync::Mutex / stk::sync::ConditionVariable (no dependency on the STK C API).

    \details  Named queues live in a static table (STK_C_MQ_MAX_QUEUES), message storage is allocated with
              malloc() when the queue is created and freed when it is unlinked and closed by everybody.
              Messages are delivered highest priority first, FIFO within one priority (0..MQ_PRIO_MAX-1).
              All functions return -1 and set errno on failure, mq_open() returns (mqd_t)-1.
              mq_send/mq_receive and the timed variants are cancellation points.

              Not supported: mq_notify() (STK has no signals), it fails with ENOSYS.
*/

/* Same newlib guard as in stk_c_pthread.h: <sys/types.h> must not define its own pthread types, the shim provides
   them (needed here because this header may be included before stk_c_pthread.h). */
#ifndef _SYS__PTHREADTYPES_H_
    #define _SYS__PTHREADTYPES_H_
#endif

#include <stddef.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef STK_C_MQ_MAX_QUEUES
    #define STK_C_MQ_MAX_QUEUES (4U)       /*!< Queues which can exist at the same time. */
#endif
#ifndef STK_C_MQ_MAX_DESCRIPTORS
    #define STK_C_MQ_MAX_DESCRIPTORS (8U)  /*!< Open descriptors (mq_open() calls) at the same time. */
#endif
#ifndef STK_C_MQ_NAME_MAX
    #define STK_C_MQ_NAME_MAX (32U)        /*!< Queue name capacity including the NUL. */
#endif
#ifndef STK_C_MQ_DEFAULT_MAXMSG
    #define STK_C_MQ_DEFAULT_MAXMSG (8)    /*!< mq_maxmsg used when mq_open() gets no attributes. */
#endif
#ifndef STK_C_MQ_DEFAULT_MSGSIZE
    #define STK_C_MQ_DEFAULT_MSGSIZE (64)  /*!< mq_msgsize used when mq_open() gets no attributes. */
#endif

#define MQ_PRIO_MAX (32U) /*!< Message priorities are 0..MQ_PRIO_MAX-1. */

typedef struct stk_mq_desc *mqd_t; /*!< Message queue descriptor. */

struct mq_attr
{
    long mq_flags;   /*!< 0 or O_NONBLOCK. */
    long mq_maxmsg;  /*!< Maximum number of queued messages. */
    long mq_msgsize; /*!< Maximum message size in bytes. */
    long mq_curmsgs; /*!< Messages currently queued. */
};

/*! \brief     Open (and optionally create) a named queue.
    \param[in] oflag: O_RDONLY / O_WRONLY / O_RDWR, optionally with O_CREAT, O_EXCL, O_NONBLOCK.
    \details   With O_CREAT two more arguments follow: a mode_t (ignored) and a const struct mq_attr * (NULL for
               the defaults; only mq_maxmsg and mq_msgsize are used).
    \return    Descriptor, or (mqd_t)-1 with errno = EINVAL, ENAMETOOLONG, ENOENT, EEXIST, ENOSPC (no free queue),
               EMFILE (no free descriptor), ENOMEM.
*/
mqd_t mq_open(const char *name, int oflag, ...);

/*! \brief     Close a descriptor; an unlinked queue is destroyed with its last descriptor.
*/
int mq_close(mqd_t mqdes);

/*! \brief     Remove the name of a queue (existing descriptors keep working).
    \return    0, or -1 with errno = ENOENT, EINVAL.
*/
int mq_unlink(const char *name);

/*! \brief     Send a message. Blocks while the queue is full unless O_NONBLOCK is set.
    \return    0, or -1 with errno = EBADF, EINVAL (priority), EMSGSIZE, EAGAIN, EINTR.
*/
int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned int msg_prio);

/*! \brief     mq_send() with an absolute deadline (STK timeline). Additionally ETIMEDOUT.
*/
int mq_timedsend(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned int msg_prio,
                 const struct timespec *abstime);

/*! \brief     Receive the oldest message of the highest priority.
    \param[in] msg_len: Must be at least mq_msgsize (else EMSGSIZE).
    \return    Message length, or -1 with errno = EBADF, EMSGSIZE, EAGAIN, EINTR.
*/
ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned int *msg_prio);

/*! \brief     mq_receive() with an absolute deadline (STK timeline). Additionally ETIMEDOUT.
*/
ssize_t mq_timedreceive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned int *msg_prio,
                        const struct timespec *abstime);

/*! \brief     Get the attributes of the queue (mq_flags is that of this descriptor).
*/
int mq_getattr(mqd_t mqdes, struct mq_attr *mqstat);

/*! \brief     Set the descriptor flags: only O_NONBLOCK of mq_flags is used.
    \param[out] omqstat: Previous attributes (may be NULL).
*/
int mq_setattr(mqd_t mqdes, const struct mq_attr *mqstat, struct mq_attr *omqstat);

/*! \brief     Not supported (no signals): fails with ENOSYS.
*/
int mq_notify(mqd_t mqdes, const struct sigevent *notification);

#ifdef __cplusplus
}
#endif

#endif /* STK_C_MQUEUE_H_ */
