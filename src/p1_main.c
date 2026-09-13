#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "host/tun.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/l3_tun.h"
#include "runtime/lwip_loop.h"

static volatile sig_atomic_t tcp_shift_stop;

static void tcp_shift_handle_signal(int signo)
{
    (void)signo;
    tcp_shift_stop = 1;
}

static int parse_ipv4(const char *text, ip4_addr_t *address)
{
    if (ip4addr_aton(text, address) == 0) {
        fprintf(stderr, "invalid IPv4 address: %s\n", text);
        return -1;
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <tun-name> <lwip-ipv4> <netmask> <gateway>\n"
            "example: %s ts0 10.0.0.2 255.255.255.252 10.0.0.1\n",
            program, program);
}

int main(int argc, char **argv)
{
    struct tcp_shift_tun tun;
    struct tcp_shift_l3_tun l3;
    struct tcp_shift_lwip_loop loop;
    ip4_addr_t address;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    int status = EXIT_FAILURE;

    tun.fd = -1;
    loop.epoll_fd = -1;

    if (argc != 5) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_ipv4(argv[2], &address) < 0 ||
        parse_ipv4(argv[3], &netmask) < 0 ||
        parse_ipv4(argv[4], &gateway) < 0) {
        return EXIT_FAILURE;
    }

    if (signal(SIGINT, tcp_shift_handle_signal) == SIG_ERR ||
        signal(SIGTERM, tcp_shift_handle_signal) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    lwip_init();

    if (tcp_shift_tun_open(&tun, argv[1]) < 0) {
        perror("open TUN");
        return EXIT_FAILURE;
    }
    if (tcp_shift_l3_tun_attach_ipv4(&l3, tun.fd, &address, &netmask,
                                     &gateway) < 0) {
        perror("attach lwIP TUN netif");
        goto out_tun;
    }
    if (tcp_shift_lwip_loop_init(&loop, &l3) < 0) {
        perror("initialize lwIP event loop");
        goto out_l3;
    }

    printf("tcp-shift-p1: ready tun=%s lwip-ipv4=%s mtu=%u\n",
           tun.ifname, argv[2], (unsigned)l3.netif.mtu);
    fflush(stdout);

    status = EXIT_SUCCESS;
    while (tcp_shift_stop == 0) {
        if (tcp_shift_lwip_loop_run_once(&loop) < 0) {
            perror("lwIP event loop");
            status = EXIT_FAILURE;
            break;
        }
    }

    fprintf(stderr,
            "tcp-shift-p1: rx_packets=%llu tx_packets=%llu "
            "tx_queue_peak_bytes=%u tx_queue_drops=%llu\n",
            (unsigned long long)l3.rx_packets,
            (unsigned long long)l3.tx_packets,
            l3.tx_queue_peak_bytes,
            (unsigned long long)l3.tx_queue_drops);

    tcp_shift_lwip_loop_close(&loop);
out_l3:
    tcp_shift_l3_tun_detach(&l3);
out_tun:
    tcp_shift_tun_close(&tun);
    return status;
}
