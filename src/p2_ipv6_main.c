#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "bridge/bridge.h"
#include "host/ifconfig.h"
#include "host/tun.h"
#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/ip6_addr.h"
#include "lwip/l3_tun.h"
#include "runtime/lwip_loop.h"

#define TCP_SHIFT_P2_MTU 1500U

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
            "usage: %s <tun-name> <lwip-ipv6> <host-ipv6-cidr> "
            "<public-port> <backend-port>\n"
            "example: %s ts6 fd00:198:22::2 fd00:198:22::1/126 "
            "18091 19091\n",
            program, program);
}

static int tcp_shift_p2_ipv6_pacer_schedule(void *arg,
                                             uint64_t flow_id,
                                             uint32_t generation,
                                             uint64_t deadline_ns,
                                             uint32_t bytes)
{
    struct tcp_shift_lwip_loop *loop = arg;
    struct tcp_shift_pacer_event event;

    event.deadline_ns = deadline_ns;
    event.flow_id = flow_id;
    event.generation = generation;
    event.bytes = bytes;
    return tcp_shift_lwip_loop_pacer_schedule(loop, &event);
}

static int tcp_shift_p2_ipv6_pacer_cancel(void *arg,
                                           uint64_t flow_id,
                                           uint32_t generation,
                                           size_t *cancelled)
{
    return tcp_shift_lwip_loop_pacer_cancel(arg, flow_id, generation,
                                             cancelled);
}

static int tcp_shift_p2_ipv6_pacer_release(
    void *arg,
    const struct tcp_shift_pacer_event *event,
    uint64_t actual_release_ns)
{
    (void)arg;
    if (event == NULL) {
        errno = EINVAL;
        return -1;
    }
    return tcp_shift_lwip_cc_resume_paced(event->flow_id,
                                          event->generation,
                                          actual_release_ns);
}

static const struct tcp_shift_lwip_cc_pacer_ops tcp_shift_p2_ipv6_pacer_ops = {
    .schedule = tcp_shift_p2_ipv6_pacer_schedule,
    .cancel = tcp_shift_p2_ipv6_pacer_cancel,
};

static int tcp_shift_p2_ipv6_configure_pacer(struct tcp_shift_lwip_loop *loop)
{
    if (tcp_shift_lwip_loop_set_pacer_release(
            loop, tcp_shift_p2_ipv6_pacer_release, NULL) < 0) {
        return -1;
    }
    return tcp_shift_lwip_cc_configure_pacer(&tcp_shift_p2_ipv6_pacer_ops,
                                              loop);
}

int main(int argc, char **argv)
{
    struct tcp_shift_tun tun;
    struct tcp_shift_l3_tun l3;
    struct tcp_shift_lwip_loop loop;
    struct tcp_shift_bridge bridge;
    const struct tcp_shift_lwip_cc_stats *cc_stats;
    ip6_addr_t address;
    uint16_t public_port;
    uint16_t backend_port;
    int l3_attached = 0;
    int loop_started = 0;
    int pacer_configured = 0;
    int bridge_started = 0;
    int status = EXIT_FAILURE;

    tun.fd = -1;
    loop.epoll_fd = -1;
    bridge.listener = NULL;
    bridge.flows = NULL;

    if (argc != 6) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_ipv6(argv[2], &address) < 0 ||
        parse_port(argv[4], &public_port) < 0 ||
        parse_port(argv[5], &backend_port) < 0) {
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
                                          TCP_SHIFT_P2_MTU) < 0) {
        perror("configure host IPv6 TUN interface");
        goto out;
    }
    if (tcp_shift_l3_tun_attach_ipv6(&l3, tun.fd, &address) < 0) {
        perror("attach lwIP IPv6 TUN netif");
        goto out;
    }
    l3_attached = 1;

    if (tcp_shift_lwip_loop_init(&loop, &l3) < 0) {
        perror("initialize lwIP event loop");
        goto out;
    }
    loop_started = 1;

    if (tcp_shift_p2_ipv6_configure_pacer(&loop) < 0) {
        fprintf(stderr, "configure IPv6 P5c pacer service failed\n");
        goto out;
    }
    pacer_configured = 1;

    if (tcp_shift_bridge_start_ipv6(&bridge, &loop, public_port,
                                    backend_port) < 0) {
        perror("start IPv6 TCP bridge");
        goto out;
    }
    bridge_started = 1;

    printf("tcp-shift-p2-ipv6: ready tun=%s host-ipv6=%s lwip-ipv6=%s mtu=%u "
           "public-port=%u backend=127.0.0.1:%u\n",
           tun.ifname, argv[3], argv[2], (unsigned)l3.netif.mtu,
           (unsigned)public_port, (unsigned)backend_port);
    fflush(stdout);

    status = EXIT_SUCCESS;
    while (tcp_shift_stop == 0) {
        if (tcp_shift_lwip_loop_run_once(&loop) < 0) {
            perror("lwIP event loop");
            status = EXIT_FAILURE;
            break;
        }
    }

    cc_stats = tcp_shift_lwip_cc_get_stats();
    fprintf(stderr,
            "tcp-shift-p2-ipv6: rx_packets=%llu rx_drops=%llu rx_errors=%llu "
            "tx_packets=%llu tx_queue_peak_bytes=%u tx_queue_drops=%llu "
            "bridge_accepts=%llu bridge_backend_connects=%llu "
            "bridge_public_to_backend_bytes=%llu "
            "bridge_backend_to_public_bytes=%llu bridge_active_flows=%llu "
            "bridge_peak_active_flows=%llu bridge_backend_failures=%llu "
            "bridge_public_errors=%llu bridge_pending_public_bytes=%llu "
            "bridge_peak_pending_public_bytes=%llu "
            "bridge_backend_write_blocked_events=%llu "
            "bridge_backend_read_blocked_events=%llu "
            "bridge_backend_socket_sndbuf_bytes=%u "
            "bridge_backend_socket_rcvbuf_bytes=%u loop_wait_calls=%llu "
            "loop_ready_wakeups=%llu loop_timeout_wakeups=%llu "
            "loop_eintr_wakeups=%llu loop_tun_readable_wakeups=%llu "
            "loop_tun_writable_wakeups=%llu "
            "cc_bindings=%llu cc_bind_failures=%llu cc_ack_events=%llu "
            "cc_loss_events=%llu cc_timeout_events=%llu "
            "cc_policy_updates=%llu cc_controller_errors=%llu "
            "cc_last_cwnd=%u cc_last_ssthresh=%u "
            "pacing_wakeups=%llu pacing_scheduler_errors=%llu\n",
            (unsigned long long)l3.rx_packets,
            (unsigned long long)l3.rx_drops,
            (unsigned long long)l3.rx_errors,
            (unsigned long long)l3.tx_packets,
            l3.tx_queue_peak_bytes,
            (unsigned long long)l3.tx_queue_drops,
            (unsigned long long)bridge.accepts,
            (unsigned long long)bridge.backend_connects,
            (unsigned long long)bridge.public_to_backend_bytes,
            (unsigned long long)bridge.backend_to_public_bytes,
            (unsigned long long)bridge.active_flows,
            (unsigned long long)bridge.peak_active_flows,
            (unsigned long long)bridge.backend_failures,
            (unsigned long long)bridge.public_errors,
            (unsigned long long)bridge.pending_public_bytes,
            (unsigned long long)bridge.peak_pending_public_bytes,
            (unsigned long long)bridge.backend_write_blocked_events,
            (unsigned long long)bridge.backend_read_blocked_events,
            bridge.backend_socket_sndbuf_bytes,
            bridge.backend_socket_rcvbuf_bytes,
            (unsigned long long)loop.wait_calls,
            (unsigned long long)loop.ready_wakeups,
            (unsigned long long)loop.timeout_wakeups,
            (unsigned long long)loop.eintr_wakeups,
            (unsigned long long)loop.tun_readable_wakeups,
            (unsigned long long)loop.tun_writable_wakeups,
            (unsigned long long)cc_stats->bindings,
            (unsigned long long)cc_stats->bind_failures,
            (unsigned long long)cc_stats->ack_events,
            (unsigned long long)cc_stats->loss_events,
            (unsigned long long)cc_stats->timeout_events,
            (unsigned long long)cc_stats->policy_updates,
            (unsigned long long)cc_stats->controller_errors,
            cc_stats->last_cwnd_bytes,
            cc_stats->last_ssthresh_bytes,
            (unsigned long long)loop.pacing_wakeups,
            (unsigned long long)cc_stats->pacing_scheduler_errors);

out:
    if (bridge_started != 0) {
        tcp_shift_bridge_stop(&bridge);
    }
    if (pacer_configured != 0 && tcp_shift_lwip_cc_clear_pacer() < 0) {
        fprintf(stderr,
                "tcp-shift-p2-ipv6: pacer_service_retained_until_process_exit=1\n");
    }
    if (loop_started != 0) {
        tcp_shift_lwip_loop_close(&loop);
    }
    if (l3_attached != 0) {
        tcp_shift_l3_tun_detach(&l3);
    }
    tcp_shift_tun_close(&tun);
    return status;
}
