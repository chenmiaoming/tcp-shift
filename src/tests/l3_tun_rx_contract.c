#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/l3_tun.h"

#define OVERSIZE_BYTES (TCP_SHIFT_L3_TUN_MTU + 1U)

static int fail(const char *message)
{
    fprintf(stderr, "RX contract failed: %s\n", message);
    return EXIT_FAILURE;
}

int main(void)
{
    struct tcp_shift_l3_tun l3;
    ip4_addr_t address;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    unsigned char packet[OVERSIZE_BYTES];
    int sockets[2] = {-1, -1};
    int result;
    int status = EXIT_FAILURE;

    lwip_init();

    if (socketpair(AF_UNIX,
                   SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                   0, sockets) < 0) {
        perror("socketpair");
        return EXIT_FAILURE;
    }

    IP4_ADDR(&address, 10, 0, 0, 2);
    IP4_ADDR(&netmask, 255, 255, 255, 252);
    IP4_ADDR(&gateway, 10, 0, 0, 1);

    if (tcp_shift_l3_tun_attach_ipv4(&l3, sockets[0], &address,
                                     &netmask, &gateway) < 0) {
        perror("tcp_shift_l3_tun_attach_ipv4");
        goto out;
    }

    memset(packet, 0x5a, sizeof(packet));
    if (send(sockets[1], packet, sizeof(packet), MSG_DONTWAIT) !=
        (ssize_t)sizeof(packet)) {
        perror("send oversize packet");
        goto out_detach;
    }

    result = tcp_shift_l3_tun_rx_once(&l3);
    if (result != 1 || l3.rx_drops != 1U || l3.rx_errors != 0U ||
        l3.rx_packets != 0U || l3.rx_bytes != 0U) {
        status = fail("oversize packet was not consumed as a nonfatal drop");
        goto out_detach;
    }

    errno = 0;
    result = tcp_shift_l3_tun_rx_once(&l3);
    if (result != 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        status = fail("empty nonblocking packet fd did not report no work");
        goto out_detach;
    }

    printf("tcp-shift-p1-rx-contract: mtu=%u oversize=%u rx_drops=%llu "
           "rx_errors=%llu runtime_survives_drop=ok\n",
           TCP_SHIFT_L3_TUN_MTU,
           OVERSIZE_BYTES,
           (unsigned long long)l3.rx_drops,
           (unsigned long long)l3.rx_errors);
    status = EXIT_SUCCESS;

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
