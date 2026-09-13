#include "lwip/opt.h"

#if NO_SYS != 1
#error "P0 requires NO_SYS=1"
#endif
#if LWIP_IPV4 != 1 || LWIP_IPV6 != 0
#error "P0 requires IPv4-only lwIP"
#endif
#if LWIP_TCP != 1 || LWIP_UDP != 0 || LWIP_RAW != 0
#error "P0 requires TCP without UDP/raw protocols"
#endif
#if LWIP_NETCONN != 0 || LWIP_SOCKET != 0
#error "P0 must not enable lwIP netconn/socket APIs"
#endif
#if LWIP_ETHERNET != 0 || LWIP_ARP != 0
#error "P0 is an L3 endpoint and must not enable Ethernet/ARP"
#endif
#if MEM_LIBC_MALLOC != 1 || MEMP_MEM_MALLOC != 1
#error "P0 memory accounting assumes libc-backed demand allocation"
#endif
#if LWIP_WND_SCALE != 0
#error "P0 window scaling must remain disabled until high-BDP memory tests exist"
#endif
#if TCP_WND > 65535U
#error "TCP_WND exceeds the unscaled 16-bit lwIP window field"
#endif
#if TCP_SND_BUF > 65535U
#error "TCP_SND_BUF exceeds the unscaled 16-bit lwIP send-buffer field"
#endif

_Static_assert(TCP_MSS > 0, "TCP_MSS must be positive");
_Static_assert(TCP_WND >= (2 * TCP_MSS), "TCP_WND is too small for the P0 TCP profile");
_Static_assert(TCP_SND_BUF >= (2 * TCP_MSS), "TCP_SND_BUF is too small for the P0 TCP profile");

int main(void)
{
    return 0;
}
