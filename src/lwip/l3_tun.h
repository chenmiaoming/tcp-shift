#ifndef TCP_SHIFT_LWIP_L3_TUN_H
#define TCP_SHIFT_LWIP_L3_TUN_H

#include <stdint.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#define TCP_SHIFT_L3_TUN_MTU 1500U

struct tcp_shift_l3_tun {
    struct netif netif;
    int tun_fd;
    unsigned attached;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_drops;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_would_block;
    uint64_t tx_errors;
};

int tcp_shift_l3_tun_attach_ipv4(struct tcp_shift_l3_tun *l3,
                                 int tun_fd,
                                 const ip4_addr_t *address,
                                 const ip4_addr_t *netmask,
                                 const ip4_addr_t *gateway);
void tcp_shift_l3_tun_detach(struct tcp_shift_l3_tun *l3);

/*
 * Consume at most one packet from the nonblocking TUN fd.
 * Returns 1 when one packet was injected, 0 for EAGAIN/no packet, and -1 on
 * host I/O or lwIP input failure.
 */
int tcp_shift_l3_tun_rx_once(struct tcp_shift_l3_tun *l3);

#endif /* TCP_SHIFT_LWIP_L3_TUN_H */
