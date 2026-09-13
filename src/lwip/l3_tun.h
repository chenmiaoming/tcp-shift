#ifndef TCP_SHIFT_LWIP_L3_TUN_H
#define TCP_SHIFT_LWIP_L3_TUN_H

#include <stdint.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

#define TCP_SHIFT_L3_TUN_MTU 1500U
#define TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS 64U
#define TCP_SHIFT_L3_TUN_TX_QUEUE_BYTES (96U * 1024U)

struct tcp_shift_l3_tun {
    struct netif netif;
    int tun_fd;
    unsigned attached;

    struct pbuf *tx_queue[TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS];
    unsigned tx_queue_head;
    unsigned tx_queue_count;
    uint32_t tx_queue_bytes;
    uint32_t tx_queue_peak_bytes;

    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_drops;
    uint64_t rx_errors;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_would_block;
    uint64_t tx_queue_drops;
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
 * Returns 1 when one packet was consumed (including a per-packet drop), 0 for
 * EAGAIN/no packet, and -1 only for fatal host-fd I/O/state errors.
 */
int tcp_shift_l3_tun_rx_once(struct tcp_shift_l3_tun *l3);

/* Flush queued whole packets until the queue drains or TUN blocks again. */
int tcp_shift_l3_tun_flush_tx(struct tcp_shift_l3_tun *l3);
int tcp_shift_l3_tun_wants_write(const struct tcp_shift_l3_tun *l3);

#endif /* TCP_SHIFT_LWIP_L3_TUN_H */
