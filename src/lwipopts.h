#ifndef TCP_SHIFT_LWIPOPTS_H
#define TCP_SHIFT_LWIPOPTS_H

/* Single-threaded userspace core. The process event loop owns lwIP. */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1

/* Start with a routed L3 IPv4/TCP endpoint only. */
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_ICMP 1
#define LWIP_IGMP 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_DNS 0

/* Do not pay for lwIP's sequential/socket APIs. */
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

/* Linux userspace baseline: demand-driven libc allocations first; measure it. */
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 8

/* Keep the first transport profile conservative; tune only from measurements. */
#define TCP_MSS 1460
#define TCP_WND (64 * 1024)
#define TCP_SND_BUF (64 * 1024)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + (TCP_MSS - 1)) / TCP_MSS)
#define TCP_QUEUE_OOSEQ 1

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0

#endif /* TCP_SHIFT_LWIPOPTS_H */
