#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/err.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/l3_tun.h"
#include "lwip/pbuf.h"

#define TEST_PACKET_BYTES 1500U
#define TEST_FILLER_BYTES 512U
#define TEST_FILL_LIMIT 100000U

static int fail(const char *message)
{
    fprintf(stderr, "backpressure contract failed: %s\n", message);
    return EXIT_FAILURE;
}

static int fill_send_buffer(int fd)
{
    unsigned char filler[TEST_FILLER_BYTES];
    unsigned count;

    memset(filler, 0xa5, sizeof(filler));
    for (count = 0; count < TEST_FILL_LIMIT; count++) {
        ssize_t written = send(fd, filler, sizeof(filler), MSG_DONTWAIT);

        if (written == (ssize_t)sizeof(filler)) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return (int)count;
        }
        return -1;
    }

    errno = EOVERFLOW;
    return -1;
}

static int drain_discard(int fd)
{
    unsigned char packet[2048];
    int count = 0;

    for (;;) {
        ssize_t length = recv(fd, packet, sizeof(packet), MSG_DONTWAIT);

        if (length >= 0) {
            count++;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return count;
        }
        return -1;
    }
}

static struct pbuf *make_packet(uint32_t sequence)
{
    unsigned char packet[TEST_PACKET_BYTES];
    struct pbuf *p;

    memset(packet, (int)(sequence & 0xffU), sizeof(packet));
    memcpy(packet, &sequence, sizeof(sequence));

    p = pbuf_alloc(PBUF_RAW, (u16_t)sizeof(packet), PBUF_RAM);
    if (p == NULL) {
        return NULL;
    }
    if (pbuf_take(p, packet, sizeof(packet)) != ERR_OK) {
        pbuf_free(p);
        return NULL;
    }
    return p;
}

static int queue_packet(struct tcp_shift_l3_tun *l3,
                        const ip4_addr_t *destination,
                        uint32_t sequence,
                        err_t expected)
{
    struct pbuf *p = make_packet(sequence);
    err_t result;

    if (p == NULL) {
        return -1;
    }
    result = l3->netif.output(&l3->netif, p, destination);
    pbuf_free(p);
    return result == expected ? 0 : -1;
}

static int drain_verify_sequences(int fd, uint32_t *expected)
{
    unsigned char packet[TEST_PACKET_BYTES + 64U];
    int count = 0;

    for (;;) {
        uint32_t sequence;
        ssize_t length = recv(fd, packet, sizeof(packet), MSG_DONTWAIT);

        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return count;
            }
            return -1;
        }
        if ((size_t)length != TEST_PACKET_BYTES) {
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(&sequence, packet, sizeof(sequence));
        if (sequence != *expected) {
            errno = EILSEQ;
            return -1;
        }
        (*expected)++;
        count++;
    }
}

int main(void)
{
    struct tcp_shift_l3_tun l3;
    ip4_addr_t address;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4_addr_t destination;
    uint32_t expected_sequence = 0;
    uint32_t expected_peak =
        TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS * TEST_PACKET_BYTES;
    int sockets[2] = {-1, -1};
    int socket_buffer = 4096;
    unsigned i;
    int status = EXIT_FAILURE;

    lwip_init();

    if (socketpair(AF_UNIX,
                   SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                   0, sockets) < 0) {
        perror("socketpair");
        return EXIT_FAILURE;
    }
    if (setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                   &socket_buffer, sizeof(socket_buffer)) < 0 ||
        setsockopt(sockets[1], SOL_SOCKET, SO_RCVBUF,
                   &socket_buffer, sizeof(socket_buffer)) < 0) {
        perror("setsockopt");
        goto out;
    }

    IP4_ADDR(&address, 10, 0, 0, 2);
    IP4_ADDR(&netmask, 255, 255, 255, 252);
    IP4_ADDR(&gateway, 10, 0, 0, 1);
    IP4_ADDR(&destination, 10, 0, 0, 1);

    if (tcp_shift_l3_tun_attach_ipv4(&l3, sockets[0], &address,
                                     &netmask, &gateway) < 0) {
        perror("tcp_shift_l3_tun_attach_ipv4");
        goto out;
    }

    if (fill_send_buffer(sockets[0]) <= 0) {
        status = fail("could not force packet fd to EAGAIN");
        goto out_detach;
    }

    for (i = 0; i < TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS; i++) {
        if (queue_packet(&l3, &destination, i, ERR_OK) < 0) {
            status = fail("packet did not enter bounded FIFO");
            goto out_detach;
        }
    }

    if (l3.tx_queue_count != TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS ||
        l3.tx_queue_bytes != expected_peak ||
        l3.tx_queue_peak_bytes != expected_peak ||
        l3.tx_queue_drops != 0U ||
        !tcp_shift_l3_tun_wants_write(&l3)) {
        status = fail("queue occupancy/peak contract mismatch");
        goto out_detach;
    }

    if (queue_packet(&l3, &destination,
                     TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS, ERR_MEM) < 0 ||
        l3.tx_queue_count != TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS ||
        l3.tx_queue_drops != 1U) {
        status = fail("queue ceiling did not reject packet with ERR_MEM");
        goto out_detach;
    }

    if (drain_discard(sockets[1]) < 0) {
        status = fail("failed to drain EAGAIN filler datagrams");
        goto out_detach;
    }

    while (l3.tx_queue_count != 0U) {
        int flushed = tcp_shift_l3_tun_flush_tx(&l3);
        int received;

        if (flushed < 0) {
            status = fail("flush returned host I/O error");
            goto out_detach;
        }
        received = drain_verify_sequences(sockets[1], &expected_sequence);
        if (received < 0 || received != flushed) {
            status = fail("FIFO output ordering/count mismatch");
            goto out_detach;
        }
        if (flushed == 0) {
            status = fail("flush made no progress after peer drain");
            goto out_detach;
        }
    }

    if (expected_sequence != TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS ||
        l3.tx_packets != TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS ||
        l3.tx_bytes != expected_peak ||
        l3.tx_queue_bytes != 0U ||
        tcp_shift_l3_tun_wants_write(&l3)) {
        status = fail("drained queue accounting mismatch");
        goto out_detach;
    }

    /* Recreate backpressure and leave packets queued to qualify detach cleanup. */
    if (fill_send_buffer(sockets[0]) <= 0 ||
        queue_packet(&l3, &destination, 1000U, ERR_OK) < 0 ||
        queue_packet(&l3, &destination, 1001U, ERR_OK) < 0 ||
        l3.tx_queue_count != 2U ||
        l3.tx_queue_bytes != 2U * TEST_PACKET_BYTES) {
        status = fail("could not create queued-detach fixture");
        goto out_detach;
    }

    tcp_shift_l3_tun_detach(&l3);
    if (l3.attached != 0U || l3.tun_fd != -1 ||
        l3.tx_queue_count != 0U || l3.tx_queue_bytes != 0U ||
        tcp_shift_l3_tun_wants_write(&l3)) {
        status = fail("detach did not release bounded TX queue");
        goto out;
    }

    printf("tcp-shift-p1-backpressure: packets=%u peak_bytes=%u drops=%llu "
           "tx_packets=%llu tx_would_block=%llu fifo_order=ok detach_cleanup=ok\n",
           TCP_SHIFT_L3_TUN_TX_QUEUE_PACKETS,
           expected_peak,
           (unsigned long long)l3.tx_queue_drops,
           (unsigned long long)l3.tx_packets,
           (unsigned long long)l3.tx_would_block);
    status = EXIT_SUCCESS;
    goto out;

out_detach:
    tcp_shift_l3_tun_detach(&l3);
out:
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    return status;
}
