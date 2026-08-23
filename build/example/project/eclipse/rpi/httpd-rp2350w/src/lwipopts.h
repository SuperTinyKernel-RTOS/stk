#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Generally you would define your own explicit list of lwIP options
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html)
//
// This example uses a common include to avoid repetition
#include "lwipopts_examples_common.h"

// The following is needed to test mDns
#define LWIP_MDNS_RESPONDER 1
#define LWIP_IGMP 1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define MDNS_RESP_USENETIF_EXTCALLBACK  1
#define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 3 + (LWIP_MDNS_RESPONDER ? 2 : 0))
#define MEMP_NUM_TCP_PCB 12

// Enable some httpd features
#define LWIP_HTTPD_CGI 1
#define LWIP_HTTPD_SSI 1
#define LWIP_HTTPD_SSI_MULTIPART 1
#define LWIP_HTTPD_SUPPORT_POST 1
#define LWIP_HTTPD_SSI_INCLUDE_TAG 0

// Generated file containing html data
//#define HTTPD_FSDATA_FILE "pico_fsdata.inc"

#define LWIP_PROVIDE_ERRNO 1

#define LWIP_STK_THREAD_STACKSIZE_IS_STACKWORDS 1   // Interpret sys_thread_new()'s stacksize as bytes (0) or stk::Words (1)
#define LWIP_STK_SYS_ARCH_PROTECT_SANITY_CHECK  0   // Verify sys_arch_protect/unprotect nest correctly
#define LWIP_STK_CHECK_QUEUE_EMPTY_ON_FREE      0   // Assert mailboxes are drained before sys_mbox_free()
#define LWIP_STK_CHECK_CORE_LOCKING             1   // Enable sys_mark_tcpip_thread()/sys_check_core_locking()
#define LWIP_STK_SYS_NOW_FROM_STK               1   // Implement sys_now() via stk::GetTimeNowMs() (set 0 to supply your own)
#define LWIP_STK_NETCONN_SEM_MAX_THREADS        16U
#define LWIP_STK_THREAD_STACK_BLOCK_WORDS       1024U

#define LWIP_NETCONN_SEM_PER_THREAD (LWIP_STK_NETCONN_SEM_MAX_THREADS)
#define TCPIP_MBOX_SIZE             16U
#define TCPIP_THREAD_STACKSIZE      LWIP_STK_THREAD_STACK_BLOCK_WORDS

#endif
