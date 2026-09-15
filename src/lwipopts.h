#ifndef TCP_SHIFT_LWIPOPTS_H
#define TCP_SHIFT_LWIPOPTS_H

/* Single-threaded userspace core. The process event loop owns lwIP. */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_TIMERS 1

/* Routed L3 dual-stack TCP endpoint. */
#define LWIP_IPV4 1
#define LWIP_IPV6 1
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_ICMP 1
#define LWIP_ICMP6 1
#define LWIP_IGMP 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_DNS 0
#define LWIP_ETHERNET 0
#define LWIP_ARP 0

/* P1b uses statically configured L3 IPv6 addresses. Do not pay for Ethernet-
 * oriented multicast/autoconfiguration control planes that are not part of the
 * TUN design. ND6 remains compiled because lwIP IPv6 timers/ICMP6 depend on it. */
#define LWIP_IPV6_DHCP6 0
#define LWIP_IPV6_AUTOCONFIG 0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT 0
#define LWIP_IPV6_ADDRESS_LIFETIMES 0
#define LWIP_IPV6_MLD 0
#define LWIP_ND6_QUEUEING 0
#define LWIP_ND6_ALLOW_RA_UPDATES 0
#define LWIP_IPV6_FORWARD 0

/* The P1b server path relies on MTU/PMTU rather than IPv6 endpoint
 * fragmentation. Routers never fragment IPv6, and keeping reassembly disabled
 * avoids per-fragment state on the low-memory target. PTB qualification is a
 * separate gate and must not be replaced by silently enabling fragmentation. */
#define LWIP_IPV6_FRAG 0
#define LWIP_IPV6_REASS 0

/* Do not pay for lwIP's sequential/socket APIs. */
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

/* Linux userspace baseline: demand-driven libc allocations first; measure it. */
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 8

/*
 * P4 reserves exactly one TCP PCB extension slot for the tcp-shift CC hook ABI.
 * The slot stores only an opaque hook pointer. Unbound P0/P1/probe PCBs execute
 * native lwIP congestion control. The hook header is intentionally independent
 * from src/cc so patched lwIP never links the controller core directly.
 */
#define LWIP_TCP_PCB_NUM_EXT_ARGS 1
#define LWIP_HOOK_FILENAME "lwip/cc_hooks.h"

/*
 * Enable upstream lwIP RFC 7323 window-scaling support so sender-side window,
 * cwnd and send-buffer accounting use 32-bit tcpwnd_size_t. Keep the local
 * receive scale at zero: tcp-shift negotiates window scaling but continues to
 * advertise the existing small receive window.
 *
 * The production low-memory sender profile remains 32 KiB. Qualification builds
 * may define TCP_SHIFT_TCP_SND_BUF_BYTES at compile time to measure larger
 * sender capacities without silently changing the production default. The cap
 * below prevents accidental multi-megabyte-per-flow profiles from entering CI
 * without an explicit source review.
 */
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 0
#define TCP_MSS 1460
#define TCP_WND (32 * 1024)
#ifndef TCP_SHIFT_TCP_SND_BUF_BYTES
#define TCP_SHIFT_TCP_SND_BUF_BYTES (32 * 1024)
#endif
#if TCP_SHIFT_TCP_SND_BUF_BYTES < (2 * TCP_MSS)
#error "TCP_SHIFT_TCP_SND_BUF_BYTES is too small for the TCP profile"
#endif
#if TCP_SHIFT_TCP_SND_BUF_BYTES > (4 * 1024 * 1024)
#error "TCP_SHIFT_TCP_SND_BUF_BYTES exceeds the reviewed qualification ceiling"
#endif
#define TCP_SND_BUF TCP_SHIFT_TCP_SND_BUF_BYTES
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + (TCP_MSS - 1)) / TCP_MSS)
#define TCP_QUEUE_OOSEQ 1

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0

#endif /* TCP_SHIFT_LWIPOPTS_H */
