#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "host/ifconfig.h"
#include "host/tun.h"
#include "lwip/init.h"
#include "lwip/ip6_addr.h"
#include "lwip/l3_tun.h"
#include "lwip/probe_listener.h"
#include "runtime/lwip_loop.h"

#define TCP_SHIFT_P1_IPV6_DEFAULT_PORT 18082U
#define TCP_SHIFT_P1_MTU 1500U

static volatile sig_atomic_t tcp_shift_stop;

static void tcp_shift_handle_signal(int signo)
{
    (void)signo;
    tcp_shift_stop = 1;
}

static int parse_ipv6(const char *text, ip6_addr_t *address)
{
    if (ip6addr_aton(text, address) == 0) {
        fprintf(stderr, "invalid IPv6 address: %s\n", text);
        return -1;
    }
    return 0;
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL ||
        value > 65535UL) {
        fprintf(stderr, "invalid TCP port: %s\n", text);
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <tun-name> <lwip-ipv6> <host-ipv6-cidr> [listen-port]\n"
            "example: %s ts6 2001:db8:231::2 2001:db8:231::1/126 18082\n",
            program, program);
}

int main(int argc, char **argv)
{
    struct tcp_shift_tun tun;
    struct tcp_shift_l3_tun l3;
    struct tcp_shift_lwip_loop loop;
    struct tcp_shift_probe_listener listener;
    ip6_addr_t address;
    uint16_t listen_port = TCP_SHIFT_P1_IPV6_DEFAULT_PORT;
    int listener_started = 0;
    int loop_started = 0;
    int status = EXIT_FAILURE;

    tun.fd = -1;
    loop.epoll_fd = -1;
    listener.pcb = NULL;

    if (argc != 4 && argc != 5) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_ipv6(argv[2], &address) < 0 ||
        (argc == 5 && parse_port(argv[4], &listen_port) < 0)) {
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
    if (tcp_shift_host_configure_ipv6_tun(tun.ifname, argv[3],
                                          TCP_SHIFT_P1_MTU) < 0) {
        perror("configure host IPv6 TUN interface");
        goto out_tun;
    }
    if (tcp_shift_l3_tun_attach_ipv6(&l3, tun.fd, &address) < 0) {
        perror("attach lwIP IPv6 TUN netif");
        goto out_tun;
    }
    if (tcp_shift_probe_listener_start_ipv6(&listener, listen_port) < 0) {
        perror("start lwIP IPv6 TCP probe listener");
        goto out_l3;
    }
    listener_started = 1;
    if (tcp_shift_lwip_loop_init(&loop, &l3) < 0) {
        perror("initialize lwIP event loop");
        goto out_listener;
    }
    loop_started = 1;

    printf("tcp-shift-p1-ipv6: ready tun=%s host-ipv6=%s lwip-ipv6=%s "
           "mtu=%u tcp-port=%u\n",
           tun.ifname, argv[3], argv[2], (unsigned)l3.netif.mtu,
           (unsigned)listen_port);
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
            "tcp-shift-p1-ipv6: rx_packets=%llu rx_drops=%llu rx_errors=%llu "
            "tx_packets=%llu tx_queue_peak_bytes=%u tx_queue_drops=%llu "
            "tcp_accepts=%llu tcp_rx_bytes=%llu tcp_errors=%llu "
            "loop_wait_calls=%llu loop_ready_wakeups=%llu "
            "loop_timeout_wakeups=%llu loop_eintr_wakeups=%llu "
            "loop_tun_readable_wakeups=%llu loop_tun_writable_wakeups=%llu\n",
            (unsigned long long)l3.rx_packets,
            (unsigned long long)l3.rx_drops,
            (unsigned long long)l3.rx_errors,
            (unsigned long long)l3.tx_packets,
            l3.tx_queue_peak_bytes,
            (unsigned long long)l3.tx_queue_drops,
            (unsigned long long)listener.accepts,
            (unsigned long long)listener.rx_bytes,
            (unsigned long long)listener.errors,
            (unsigned long long)loop.wait_calls,
            (unsigned long long)loop.ready_wakeups,
            (unsigned long long)loop.timeout_wakeups,
            (unsigned long long)loop.eintr_wakeups,
            (unsigned long long)loop.tun_readable_wakeups,
            (unsigned long long)loop.tun_writable_wakeups);

    if (loop_started != 0) {
        tcp_shift_lwip_loop_close(&loop);
    }
out_listener:
    if (listener_started != 0) {
        tcp_shift_probe_listener_stop(&listener);
    }
out_l3:
    tcp_shift_l3_tun_detach(&l3);
out_tun:
    tcp_shift_tun_close(&tun);
    return status;
}
