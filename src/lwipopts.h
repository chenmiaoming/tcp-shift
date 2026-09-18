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
 * Slot 0 is the tcp-shift CC/pacing hook ABI. Slot 1 is transport TCP-memory
 * state used by runtime tcp_wmem/tcp_mem policy. Slot 2 is an internal-only
 * BBR lifecycle sidecar so the full BBR state is allocated only for BBR flows
 * instead of inflating every generic adapter/PCB. All slots store only
 * project-owned opaque pointers; patched lwIP remains independent from src/cc.
 */
#define LWIP_TCP_PCB_NUM_EXT_ARGS 3
#define LWIP_HOOK_FILENAME "lwip/cc_hooks.h"

/*
 * Enable upstream lwIP RFC 7323 window-scaling support so sender-side window,
 * cwnd and send-buffer accounting use 32-bit tcpwnd_size_t. Keep the local
 * receive scale at zero: tcp-shift negotiates window scaling but continues to
 * advertise the existing small receive window.
 *
 * TCP_SND_BUF is no longer the per-flow production allocation. It is a
 * compile-time capability ceiling used by upstream queue-length checks. The
 * transport memory layer lowers each new production flow to its runtime
 * tcp_wmem initial value (32 KiB by default) and grows it on demand up to the
 * configured runtime maximum. MEM_LIBC_MALLOC/MEMP_MEM_MALLOC mean this ceiling
 * does not preallocate that many bytes for every PCB.
 *
 * TCP_SHIFT_TCP_SND_BUF_BYTES is retained as a build-profile compatibility
 * alias: existing qualification jobs that define it now select the compile
 * ceiling, not a fixed per-flow runtime send buffer.
 */
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 0
#define TCP_MSS 1460
#define TCP_WND (32 * 1024)
#ifndef TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES
#ifdef TCP_SHIFT_TCP_SND_BUF_BYTES
#define TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES TCP_SHIFT_TCP_SND_BUF_BYTES
#else
#define TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES (4 * 1024 * 1024)
#endif
#endif
#ifndef TCP_SHIFT_TCP_SND_BUF_BYTES
#define TCP_SHIFT_TCP_SND_BUF_BYTES TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES
#endif
#if TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES < (2 * TCP_MSS)
#error "TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES is too small for the TCP profile"
#endif
#if TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES > (16 * 1024 * 1024)
#error "TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES exceeds the reviewed capability ceiling"
#endif
#define TCP_SND_BUF TCP_SHIFT_TCP_SND_BUF_CEILING_BYTES
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + (TCP_MSS - 1)) / TCP_MSS)

/*
 * Upstream's default TCP_SNDLOWAT follows max(TCP_SND_BUF/2, 2*MSS+1), but
 * the field and writable-space arithmetic are still u16_t constrained. Keep
 * the compile-time low-water value below the upstream u16 safety boundary;
 * runtime tcp_wmem controls the actual per-flow capacity.
 */
#define TCP_SHIFT_TCP_SNDLOWAT_HALF      (TCP_SND_BUF / 2U)
#define TCP_SHIFT_TCP_SNDLOWAT_MIN       ((2U * TCP_MSS) + 1U)
#define TCP_SHIFT_TCP_SNDLOWAT_U16_MAX   (0xFFFFU - (4U * TCP_MSS) - 1U)
#define TCP_SHIFT_TCP_SNDLOWAT_BASE \
    ((TCP_SHIFT_TCP_SNDLOWAT_HALF > TCP_SHIFT_TCP_SNDLOWAT_MIN) ? \
     TCP_SHIFT_TCP_SNDLOWAT_HALF : TCP_SHIFT_TCP_SNDLOWAT_MIN)
#define TCP_SNDLOWAT \
    ((TCP_SHIFT_TCP_SNDLOWAT_BASE < TCP_SHIFT_TCP_SNDLOWAT_U16_MAX) ? \
     TCP_SHIFT_TCP_SNDLOWAT_BASE : TCP_SHIFT_TCP_SNDLOWAT_U16_MAX)

/* Keep upstream max(TCP_SND_QUEUELEN/2, 5) semantics as a constant expression. */
#define TCP_SHIFT_TCP_SNDQUEUELOWAT_HALF (TCP_SND_QUEUELEN / 2U)
#define TCP_SNDQUEUELOWAT \
    ((TCP_SHIFT_TCP_SNDQUEUELOWAT_HALF > 5U) ? \
     TCP_SHIFT_TCP_SNDQUEUELOWAT_HALF : 5U)

#define TCP_QUEUE_OOSEQ 1

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0

#endif /* TCP_SHIFT_LWIPOPTS_H */
