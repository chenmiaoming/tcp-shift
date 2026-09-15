#include <stdint.h>
#include <stdio.h>

#include "lwip/opt.h"
#include "lwip/tcpbase.h"

#if NO_SYS != 1
#error "tcp-shift requires NO_SYS=1"
#endif
#if LWIP_IPV4 != 1 || LWIP_IPV6 != 1
#error "P1b product core requires dual-stack IPv4+IPv6"
#endif
#if LWIP_TCP != 1 || LWIP_UDP != 0 || LWIP_RAW != 0
#error "tcp-shift requires TCP without UDP/raw-IP protocols"
#endif
#if LWIP_ICMP != 1 || LWIP_ICMP6 != 1
#error "dual-stack packet qualification requires ICMP and ICMPv6"
#endif
#if LWIP_NETCONN != 0 || LWIP_SOCKET != 0
#error "tcp-shift must not enable lwIP netconn/socket APIs"
#endif
#if LWIP_ETHERNET != 0 || LWIP_ARP != 0
#error "tcp-shift is an L3 endpoint and must not enable Ethernet/ARP"
#endif
#if LWIP_IPV6_DHCP6 != 0 || LWIP_IPV6_AUTOCONFIG != 0 || \
    LWIP_IPV6_SEND_ROUTER_SOLICIT != 0 || LWIP_IPV6_MLD != 0
#error "P1b uses static L3 IPv6 and must not enable DHCP6/SLAAC/RS/MLD"
#endif
#if LWIP_ND6_QUEUEING != 0 || LWIP_ND6_ALLOW_RA_UPDATES != 0
#error "L3 TUN must not queue for L2 neighbor resolution or accept RA MTU updates"
#endif
#if LWIP_IPV6_FRAG != 0 || LWIP_IPV6_REASS != 0
#error "P1b low-memory server profile must rely on PMTU, not IPv6 fragmentation state"
#endif
#if MEM_LIBC_MALLOC != 1 || MEMP_MEM_MALLOC != 1
#error "memory accounting assumes libc-backed demand allocation"
#endif
#if LWIP_TCP_PCB_NUM_EXT_ARGS != 1
#error "P4 reserves exactly one TCP PCB ext-arg slot for CC integration"
#endif
#if LWIP_WND_SCALE != 1
#error "tcp-shift requires upstream lwIP window-scaling capability"
#endif
#if TCP_RCV_SCALE != 0
#error "current low-memory profile keeps the local receive scale at zero"
#endif
#if TCP_WND > 65535U
#error "current low-memory profile keeps the local receive window unscaled"
#endif

_Static_assert(sizeof(tcpwnd_size_t) == sizeof(uint32_t),
               "window scaling must select 32-bit tcpwnd_size_t");
_Static_assert(TCP_MSS > 0, "TCP_MSS must be positive");
_Static_assert(TCP_WND >= (2 * TCP_MSS), "TCP_WND is too small for the TCP profile");
_Static_assert(TCP_SND_BUF >= (2 * TCP_MSS), "TCP_SND_BUF is too small for the TCP profile");
_Static_assert(TCP_SND_BUF == TCP_SHIFT_TCP_SND_BUF_BYTES,
               "sender-buffer build profile must match lwIP TCP_SND_BUF");
_Static_assert(TCP_SND_QUEUELEN <= UINT16_MAX,
               "TCP_SND_QUEUELEN must fit lwIP's queue-length accounting");

int main(void)
{
    printf("config_contract=ok window_scaling=%u tcp_rcv_scale=%u "
           "tcpwnd_size_bytes=%zu tcp_wnd=%u tcp_snd_buf=%u "
           "tcp_snd_queuelen=%u\n",
           (unsigned)LWIP_WND_SCALE,
           (unsigned)TCP_RCV_SCALE,
           sizeof(tcpwnd_size_t),
           (unsigned)TCP_WND,
           (unsigned)TCP_SND_BUF,
           (unsigned)TCP_SND_QUEUELEN);
    return 0;
}
