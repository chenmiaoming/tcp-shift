#include "lwip/opt.h"

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
#if LWIP_WND_SCALE != 0
#error "window scaling remains disabled until high-BDP memory tests exist"
#endif
#if TCP_WND > 65535U
#error "TCP_WND exceeds the unscaled 16-bit lwIP window field"
#endif
#if TCP_SND_BUF > 65535U
#error "TCP_SND_BUF exceeds the unscaled 16-bit lwIP send-buffer field"
#endif

_Static_assert(TCP_MSS > 0, "TCP_MSS must be positive");
_Static_assert(TCP_WND >= (2 * TCP_MSS), "TCP_WND is too small for the TCP profile");
_Static_assert(TCP_SND_BUF >= (2 * TCP_MSS), "TCP_SND_BUF is too small for the TCP profile");

int main(void)
{
    return 0;
}
