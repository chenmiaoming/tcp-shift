#include "lwip/l3_tun.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "lwip/err.h"
#include "lwip/ip4.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include "lwip/priv/nd6_priv.h"

#define TCP_SHIFT_L3_TUN_MAX_IOV 64U
#define TCP_SHIFT_L3_TUN_RX_BUFFER 2048U
#define TCP_SHIFT_IP_VERSION_SHIFT 4U

/*
 * lwIP normally creates ND6 destination-cache entries while resolving a
 * link-layer next hop. A pure L3 TUN output callback deliberately bypasses that
 * Ethernet/ND path, but the native ICMPv6 Packet Too Big handler will only
 * update PMTU for a destination that is already present in this cache.
 *
 * Seed the existing lwIP cache without starting neighbor discovery. This adds
 * no second PMTU table and no extra fixed memory: the destination_cache[] array
 * is already part of the pinned lwIP IPv6 core. Keep the same empty-first,
 * oldest-entry replacement policy used by nd6.c. Once seeded, upstream
 * nd6_input() owns PTB updates and tcp_eff_send_mss_netif() consumes them.
 */
static void tcp_shift_l3_tun_track_ipv6_destination(
    struct netif *netif,
    const ip6_addr_t *destination)
{
    struct nd6_destination_cache_entry *entry;
    unsigned selected = LWIP_ND6_NUM_DESTINATIONS;
    unsigned i;
    u32_t oldest_age = 0U;

    if (netif == NULL || destination == NULL || ip6_addr_isany(destination) ||
        ip6_addr_ismulticast(destination)) {
        return;
    }

    for (i = 0U; i < LWIP_ND6_NUM_DESTINATIONS; i++) {
        entry = &destination_cache[i];
        if (ip6_addr_eq(&entry->destination_addr, destination)) {
            entry->age = 0U;
            return;
        }
        if (ip6_addr_isany(&entry->destination_addr)) {
            selected = i;
            break;
        }
    }

    if (selected == LWIP_ND6_NUM_DESTINATIONS) {
        selected = 0U;
        oldest_age = destination_cache[0].age;
        for (i = 1U; i < LWIP_ND6_NUM_DESTINATIONS; i++) {
            if (destination_cache[i].age > oldest_age) {
                oldest_age = destination_cache[i].age;
                selected = i;
            }
        }
    }

    entry = &destination_cache[selected];
    memset(entry, 0, sizeof(*entry));
    ip6_addr_set(&entry->destination_addr, destination);
    ip6_addr_set(&entry->next_hop_addr, destination);
    entry->pmtu = netif->mtu;
    entry->age = 0U;
}

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

static err_t tcp_shift_l3_tun_output_packet(struct netif *netif, struct pbuf *p)
{
    struct tcp_shift_l3_tun *l3 = netif->state;
    int result;

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

static err_t tcp_shift_l3_tun_output_ipv4(struct netif *netif,
                                           struct pbuf *p,
                                           const ip4_addr_t *destination)
{
    (void)destination;
    return tcp_shift_l3_tun_output_packet(netif, p);
}

static err_t tcp_shift_l3_tun_output_ipv6(struct netif *netif,
                                           struct pbuf *p,
                                           const ip6_addr_t *destination)
{
    tcp_shift_l3_tun_track_ipv6_destination(netif, destination);
    return tcp_shift_l3_tun_output_packet(netif, p);
}

static err_t tcp_shift_l3_tun_input(struct pbuf *p, struct netif *netif)
{
    unsigned char first_byte;
    unsigned version;

    if (p == NULL || netif == NULL || p->tot_len == 0U) {
        return ERR_ARG;
    }
    if (pbuf_copy_partial(p, &first_byte, 1U, 0U) != 1U) {
        return ERR_VAL;
    }

    version = (unsigned)(first_byte >> TCP_SHIFT_IP_VERSION_SHIFT);
    if (version == 4U) {
        return ip4_input(p, netif);
    }
    if (version == 6U) {
        return ip6_input(p, netif);
    }
    return ERR_VAL;
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
    netif->output_ip6 = tcp_shift_l3_tun_output_ipv6;
    return ERR_OK;
}

static void tcp_shift_l3_tun_finish_attach(struct tcp_shift_l3_tun *l3)
{
    l3->attached = 1U;
    netif_set_default(&l3->netif);
    netif_set_up(&l3->netif);
    netif_set_link_up(&l3->netif);
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
                  tcp_shift_l3_tun_netif_init,
                  tcp_shift_l3_tun_input) == NULL) {
        l3->tun_fd = -1;
        errno = EIO;
        return -1;
    }

    tcp_shift_l3_tun_finish_attach(l3);
    return 0;
}

int tcp_shift_l3_tun_attach_ipv6(struct tcp_shift_l3_tun *l3,
                                 int tun_fd,
                                 const ip6_addr_t *address)
{
    if (l3 == NULL || tun_fd < 0 || address == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(l3, 0, sizeof(*l3));
    l3->tun_fd = tun_fd;

    if (netif_add_noaddr(&l3->netif, l3, tcp_shift_l3_tun_netif_init,
                         tcp_shift_l3_tun_input) == NULL) {
        l3->tun_fd = -1;
        errno = EIO;
        return -1;
    }

    netif_ip6_addr_set(&l3->netif, 0, address);
    netif_ip6_addr_set_state(&l3->netif, 0, IP6_ADDR_PREFERRED);
    tcp_shift_l3_tun_finish_attach(l3);
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
        l3->rx_errors++;
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    /* A bad packet is not a process-fatal host I/O failure. Consume/drop it
     * and let the single-owner loop continue servicing subsequent traffic. */
    if ((size_t)length > l3->netif.mtu) {
        l3->rx_drops++;
        return 1;
    }

    p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_RAM);
    if (p == NULL) {
        l3->rx_drops++;
        return 1;
    }

    if (pbuf_take(p, packet, (u16_t)length) != ERR_OK) {
        pbuf_free(p);
        l3->rx_drops++;
        return 1;
    }

    err = l3->netif.input(p, &l3->netif);
    if (err != ERR_OK) {
        pbuf_free(p);
        l3->rx_drops++;
        return 1;
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
