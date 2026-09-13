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
 * Keep the current transport within lwIP's native 16-bit window fields.
 * Window scaling is a later transport milestone and must be introduced together
 * with explicit memory and high-BDP tests.
 */
#define LWIP_WND_SCALE 0
#define TCP_MSS 1460
#define TCP_WND (32 * 1024)
#define TCP_SND_BUF (32 * 1024)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + (TCP_MSS - 1)) / TCP_MSS)
#define TCP_QUEUE_OOSEQ 1

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0

#endif /* TCP_SHIFT_LWIPOPTS_H */
