#include "lwip/l3_tun.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "lwip/err.h"
#include "lwip/ip4.h"
#include "lwip/pbuf.h"

#define TCP_SHIFT_L3_TUN_MAX_IOV 64U
#define TCP_SHIFT_L3_TUN_RX_BUFFER 2048U

static int tcp_shift_l3_tun_write_packet(struct tcp_shift_l3_tun *l3,
                                         struct pbuf *p)
{
    struct iovec iov[TCP_SHIFT_L3_TUN_MAX_IOV];
    struct pbuf *q;
    size_t iov_count = 0;
    ssize_t written;

    for (q = p; q != NULL; q = q->next) {
        if (q->len == 0U) {
            continue;
        }
        if (iov_count == TCP_SHIFT_L3_TUN_MAX_IOV) {
            l3->tx_errors++;
            errno = EMSGSIZE;
            return -1;
        }
        iov[iov_count].iov_base = q->payload;
        iov[iov_count].iov_len = q->len;
        iov_count++;
    }

    if (iov_count == 0U) {
        return 1;
    }

    do {
        written = writev(l3->tun_fd, iov, (int)iov_count);
    } while (written < 0 && errno == EINTR);

    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            l3->tx_would_block++;
            return 0;
        }
        l3->tx_errors++;
        return -1;
    }

    if ((u16_t)written != p->tot_len) {
        l3->tx_errors++;
        errno = EIO;
        return -1;
    }

    l3->tx_packets++;
    l3->tx_bytes += (uint64_t)written;
    return 1;
}

static err_t tcp_shift_l3_tun_enqueue(struct tcp_shift_l3_tun *l3,
                                      struct pbuf *p)
{
    unsigned tail;
    uint32_t packet_bytes = p->tot_len;

    if (l3->tx_queue_count == TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS ||
        packet_bytes > TCP_SHIFT_L3_TUN_TX_QUEUE_BYTES - l3->tx_queue_bytes) {
        l3->tx_queue_drops++;
        return ERR_MEM;
    }

    tail = (l3->tx_queue_head + l3->tx_queue_count) %
           TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS;
    pbuf_ref(p);
    l3->tx_queue[tail] = p;
    l3->tx_queue_count++;
    l3->tx_queue_bytes += packet_bytes;
    if (l3->tx_queue_bytes > l3->tx_queue_peak_bytes) {
        l3->tx_queue_peak_bytes = l3->tx_queue_bytes;
    }
    return ERR_OK;
}

static void tcp_shift_l3_tun_pop_tx(struct tcp_shift_l3_tun *l3)
{
    struct pbuf *p = l3->tx_queue[l3->tx_queue_head];

    l3->tx_queue[l3->tx_queue_head] = NULL;
    l3->tx_queue_head = (l3->tx_queue_head + 1U) %
                        TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS;
    l3->tx_queue_count--;
    l3->tx_queue_bytes -= p->tot_len;
    pbuf_free(p);
}

static err_t tcp_shift_l3_tun_output_ipv4(struct netif *netif,
                                           struct pbuf *p,
                                           const ip4_addr_t *destination)
{
    struct tcp_shift_l3_tun *l3 = netif->state;
    int result;

    (void)destination;

    if (l3 == NULL || l3->tun_fd < 0 || p == NULL) {
        return ERR_IF;
    }

    /* Preserve packet ordering once one packet is waiting for writability. */
    if (l3->tx_queue_count != 0U) {
        return tcp_shift_l3_tun_enqueue(l3, p);
    }

    result = tcp_shift_l3_tun_write_packet(l3, p);
    if (result > 0) {
        return ERR_OK;
    }
    if (result == 0) {
        return tcp_shift_l3_tun_enqueue(l3, p);
    }
    return ERR_IF;
}

static err_t tcp_shift_l3_tun_netif_init(struct netif *netif)
{
    if (netif == NULL || netif->state == NULL) {
        return ERR_ARG;
    }

    netif->name[0] = 't';
    netif->name[1] = 's';
    netif->mtu = TCP_SHIFT_L3_TUN_MTU;
    netif->output = tcp_shift_l3_tun_output_ipv4;
    return ERR_OK;
}

int tcp_shift_l3_tun_attach_ipv4(struct tcp_shift_l3_tun *l3,
                                 int tun_fd,
                                 const ip4_addr_t *address,
                                 const ip4_addr_t *netmask,
                                 const ip4_addr_t *gateway)
{
    if (l3 == NULL || tun_fd < 0 || address == NULL || netmask == NULL ||
        gateway == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(l3, 0, sizeof(*l3));
    l3->tun_fd = tun_fd;

    if (netif_add(&l3->netif, address, netmask, gateway, l3,
                  tcp_shift_l3_tun_netif_init, ip4_input) == NULL) {
        l3->tun_fd = -1;
        errno = EIO;
        return -1;
    }

    l3->attached = 1U;
    netif_set_default(&l3->netif);
    netif_set_up(&l3->netif);
    netif_set_link_up(&l3->netif);
    return 0;
}

void tcp_shift_l3_tun_detach(struct tcp_shift_l3_tun *l3)
{
    if (l3 == NULL) {
        return;
    }

    while (l3->tx_queue_count != 0U) {
        tcp_shift_l3_tun_pop_tx(l3);
    }

    if (l3->attached != 0U) {
        if (netif_default == &l3->netif) {
            netif_set_default(NULL);
        }
        netif_set_link_down(&l3->netif);
        netif_set_down(&l3->netif);
        netif_remove(&l3->netif);
    }
    l3->attached = 0U;
    l3->tun_fd = -1;
}

int tcp_shift_l3_tun_rx_once(struct tcp_shift_l3_tun *l3)
{
    unsigned char packet[TCP_SHIFT_L3_TUN_RX_BUFFER];
    struct pbuf *p;
    ssize_t length;
    err_t err;

    if (l3 == NULL || l3->attached == 0U || l3->tun_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    do {
        length = read(l3->tun_fd, packet, sizeof(packet));
    } while (length < 0 && errno == EINTR);

    if (length < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    if ((size_t)length > l3->netif.mtu) {
        l3->rx_drops++;
        errno = EMSGSIZE;
        return -1;
    }

    p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_RAM);
    if (p == NULL) {
        l3->rx_drops++;
        errno = ENOMEM;
        return -1;
    }

    if (pbuf_take(p, packet, (u16_t)length) != ERR_OK) {
        pbuf_free(p);
        l3->rx_drops++;
        errno = ENOMEM;
        return -1;
    }

    err = l3->netif.input(p, &l3->netif);
    if (err != ERR_OK) {
        pbuf_free(p);
        l3->rx_drops++;
        errno = EIO;
        return -1;
    }

    l3->rx_packets++;
    l3->rx_bytes += (uint64_t)length;
    return 1;
}

int tcp_shift_l3_tun_flush_tx(struct tcp_shift_l3_tun *l3)
{
    int flushed = 0;

    if (l3 == NULL || l3->attached == 0U || l3->tun_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    while (l3->tx_queue_count != 0U) {
        struct pbuf *p = l3->tx_queue[l3->tx_queue_head];
        int result = tcp_shift_l3_tun_write_packet(l3, p);

        if (result < 0) {
            return -1;
        }
        if (result == 0) {
            break;
        }
        tcp_shift_l3_tun_pop_tx(l3);
        flushed++;
    }

    return flushed;
}

int tcp_shift_l3_tun_wants_write(const struct tcp_shift_l3_tun *l3)
{
    return l3 != NULL && l3->tx_queue_count != 0U;
}
