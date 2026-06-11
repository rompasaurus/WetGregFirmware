#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

/* ============================================================================
 *  lwipopts.h  —  configuration for lwIP (the TCP/IP networking stack)
 * ============================================================================
 *
 *  lwIP ("lightweight IP") is the small open-source networking stack the Pico
 *  uses for Wi-Fi: DHCP (get an IP address), DNS (resolve names), UDP/TCP, and
 *  SNTP (fetch the time). Like FreeRTOS, it is configured entirely with #defines
 *  in a file the build looks for by the name `lwipopts.h`.
 *
 *  WHY THIS FILE CHANGED FOR THE RTOS BUILD
 *  ----------------------------------------
 *  The original dilder-hub used lwIP in "NO_SYS" (bare-metal/polled) mode: there
 *  was no operating system, so the main loop had to call cyw43_arch_poll()
 *  constantly to let the stack do work. That is exactly the busy-polling we are
 *  trying to escape.
 *
 *  Under FreeRTOS we instead use lwIP's "OS mode" (NO_SYS = 0). In this mode lwIP
 *  runs inside its own FreeRTOS task (the "tcpip thread"), driven by the cyw43
 *  driver's async-context. No more manual polling — the kernel schedules the
 *  networking work like any other task. The cost is a few extra settings below
 *  (thread stack size, mailbox sizes) that only exist in OS mode.
 *
 *  See docs/03-DESIGN-AND-ARCHITECTURE.md for how this fits the task model.
 * ========================================================================== */

/* ----------------------------------------------------------------------------
 *  OS mode vs bare-metal mode
 * ------------------------------------------------------------------------- */

/* 0 = run lwIP under an operating system (FreeRTOS). This is THE key change from
 * the original firmware's `NO_SYS 1`. It makes lwIP create and use a background
 * task instead of requiring us to poll it. */
#define NO_SYS                      0

/* "Core locking" is lwIP's simplest thread-safety model: a single lock protects
 * the whole stack. Application code (our NTP/scan triggers) must hold that lock
 * while calling lwIP — the Pico SDK gives us cyw43_arch_lwip_begin()/_end() that
 * take/release it for us. This is simpler and faster than the alternative
 * (message-passing into the tcpip thread) and is the SDK's recommended setup. */
#define LWIP_TCPIP_CORE_LOCKING     1

/* In OS mode lwIP needs a lightweight critical-section primitive for short
 * internal protections; enable it. */
#define SYS_LIGHTWEIGHT_PROT        1

/* We use only lwIP's "raw" callback API plus SNTP. We do NOT use the BSD-style
 * sockets API or the netconn API (those need extra threads/RAM). Keep them off. */
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

/* ----------------------------------------------------------------------------
 *  The tcpip thread (only exists because NO_SYS = 0)
 * ------------------------------------------------------------------------- */

/* Stack size (bytes) for lwIP's background task. 1 KB is the SDK default and is
 * enough for DHCP/DNS/SNTP. */
#define TCPIP_THREAD_STACKSIZE      1024

/* FreeRTOS priority for the tcpip thread. Kept modest — networking should not
 * outrank our Input task. (A small positive number; the kernel idle task is 0.) */
#define TCPIP_THREAD_PRIO           4

/* "Mailboxes" are the queues lwIP uses to hand packets/requests to the tcpip
 * thread. These sizes must be defined in OS mode. 8 entries each is plenty for a
 * device that only talks to an NTP server and scans Wi-Fi. */
#define TCPIP_MBOX_SIZE             8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8

/* ----------------------------------------------------------------------------
 *  Memory pools (unchanged from the original — sized for a tiny client)
 * ------------------------------------------------------------------------- */
#define MEM_LIBC_MALLOC             0   /* use lwIP's own pool, not C malloc */
#define MEM_ALIGNMENT               4   /* 32-bit CPU → 4-byte alignment */
#define MEM_SIZE                    4000

#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_SEG            16
#define MEMP_NUM_ARP_QUEUE          4
#define MEMP_NUM_NETBUF             2
#define MEMP_NUM_NETCONN            4

#define PBUF_POOL_SIZE              16

/* ----------------------------------------------------------------------------
 *  Protocol features we need (DHCP for an IP, DNS + SNTP for the clock)
 * ------------------------------------------------------------------------- */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_IPV4                   1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1

/* SNTP — the Simple Network Time Protocol client that sets our clock. The macro
 * hands the received time to our callback (defined in the firmware). */
#define LWIP_SNTP                   1
#define SNTP_SERVER_DNS             1
#define SNTP_MAX_SERVERS            2
#define SNTP_SET_SYSTEM_TIME_US(sec, us) sntp_set_system_time_cb(sec, us)

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

#define TCP_WND                     (4 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (4 * TCP_MSS)

#define LWIP_CHKSUM_ALGORITHM       3

#define LWIP_HTTPD_CGI              0
#define LWIP_HTTPD_SSI              0

#endif
