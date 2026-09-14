#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "bridge/bridge.h"
#include "host/ifconfig.h"
#include "host/tun.h"
#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/l3_tun.h"
#include "runtime/lwip_loop.h"

#define TCP_SHIFT_P2_MTU 1500U

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
            "usage: %s <tun-name> <lwip-ipv4> <netmask> <host-ipv4> "
            "<public-port> <backend-port>\n"
            "example: %s ts0 10.0.0.2 255.255.255.252 10.0.0.1 "
            "18090 19090\n",
            program, program);
}

static void print_delivery_stats(const struct tcp_shift_lwip_cc_stats *stats)
{
    fprintf(stderr,
            "tcp-shift-p2-delivery: first_tx_events=%llu "
            "retransmit_events=%llu acked_segment_events=%llu "
            "delivered_payload_bytes=%llu metadata_alloc_failures=%llu "
            "metadata_misses=%llu metadata_abandoned_slots=%llu "
            "clock_errors=%llu timestamp_regressions=%llu "
            "last_tx_ns=%llu last_ack_ns=%llu metadata_bytes_per_slot=%u "
            "live_slots=%u peak_live_slots=%u peak_slots_per_flow=%u "
            "peak_capacity_slots_per_flow=%u\n",
            (unsigned long long)stats->delivery_first_tx_events,
            (unsigned long long)stats->delivery_retransmit_events,
            (unsigned long long)stats->delivery_acked_segment_events,
            (unsigned long long)stats->delivery_payload_bytes,
            (unsigned long long)stats->delivery_metadata_alloc_failures,
            (unsigned long long)stats->delivery_metadata_misses,
            (unsigned long long)stats->delivery_metadata_abandoned_slots,
            (unsigned long long)stats->delivery_clock_errors,
            (unsigned long long)stats->delivery_timestamp_regressions,
            (unsigned long long)stats->delivery_last_tx_ns,
            (unsigned long long)stats->delivery_last_ack_ns,
            stats->delivery_metadata_bytes_per_slot,
            stats->delivery_live_slots,
            stats->delivery_peak_live_slots,
            stats->delivery_peak_slots_per_flow,
            stats->delivery_peak_capacity_slots_per_flow);
}

static void print_rate_stats(const struct tcp_shift_lwip_cc_stats *stats)
{
    fprintf(stderr,
            "tcp-shift-p2-rate: samples=%llu valid_samples=%llu "
            "invalid_samples=%llu app_limited_samples=%llu "
            "retransmitted_samples=%llu partial_ack_events=%llu "
            "multi_segment_ack_events=%llu app_limited_enters=%llu "
            "app_limited_exits=%llu last_rate_bytes_per_sec=%llu "
            "max_rate_bytes_per_sec=%llu last_interval_ns=%llu "
            "last_send_interval_ns=%llu last_ack_interval_ns=%llu "
            "last_rtt_ns=%llu last_delivered_bytes=%u "
            "last_prior_inflight_bytes=%u last_flags=%u\n",
            (unsigned long long)stats->rate_samples,
            (unsigned long long)stats->rate_valid_samples,
            (unsigned long long)stats->rate_invalid_samples,
            (unsigned long long)stats->rate_app_limited_samples,
            (unsigned long long)stats->rate_retransmitted_samples,
            (unsigned long long)stats->rate_partial_ack_events,
            (unsigned long long)stats->rate_multi_segment_ack_events,
            (unsigned long long)stats->app_limited_enters,
            (unsigned long long)stats->app_limited_exits,
            (unsigned long long)stats->rate_last_bytes_per_sec,
            (unsigned long long)stats->rate_max_bytes_per_sec,
            (unsigned long long)stats->rate_last_interval_ns,
            (unsigned long long)stats->rate_last_send_interval_ns,
            (unsigned long long)stats->rate_last_ack_interval_ns,
            (unsigned long long)stats->rate_last_rtt_ns,
            stats->rate_last_delivered_bytes,
            stats->rate_last_prior_inflight_bytes,
            stats->rate_last_flags);
}

int main(int argc, char **argv)
{
    struct tcp_shift_tun tun;
    struct tcp_shift_l3_tun l3;
    struct tcp_shift_lwip_loop loop;
    struct tcp_shift_bridge bridge;
    const struct tcp_shift_lwip_cc_stats *cc_stats;
    ip4_addr_t address;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    uint16_t public_port;
    uint16_t backend_port;
    int l3_attached = 0;
    int loop_started = 0;
    int bridge_started = 0;
    int status = EXIT_FAILURE;

    tun.fd = -1;
    loop.epoll_fd = -1;
    bridge.listener = NULL;
    bridge.flows = NULL;

    if (argc != 7) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_ipv4(argv[2], &address) < 0 ||
        parse_ipv4(argv[3], &netmask) < 0 ||
        parse_ipv4(argv[4], &gateway) < 0 ||
        parse_port(argv[5], &public_port) < 0 ||
        parse_port(argv[6], &backend_port) < 0) {
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
    if (tcp_shift_host_configure_ipv4_tun(tun.ifname, argv[4], argv[3],
                                          TCP_SHIFT_P2_MTU) < 0) {
        perror("configure host TUN interface");
        goto out;
    }
    if (tcp_shift_l3_tun_attach_ipv4(&l3, tun.fd, &address, &netmask,
                                     &gateway) < 0) {
        perror("attach lwIP TUN netif");
        goto out;
    }
    l3_attached = 1;

    if (tcp_shift_lwip_loop_init(&loop, &l3) < 0) {
        perror("initialize lwIP event loop");
        goto out;
    }
    loop_started = 1;

    if (tcp_shift_bridge_start_ipv4(&bridge, &loop, public_port,
                                    backend_port) < 0) {
        perror("start TCP bridge");
        goto out;
    }
    bridge_started = 1;

    printf("tcp-shift-p2: ready tun=%s host-ipv4=%s lwip-ipv4=%s mtu=%u "
           "public-port=%u backend=127.0.0.1:%u\n",
           tun.ifname, argv[4], argv[2], (unsigned)l3.netif.mtu,
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
            "tcp-shift-p2: rx_packets=%llu rx_drops=%llu rx_errors=%llu "
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
            "cc_last_cwnd=%u cc_last_ssthresh=%u\n",
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
            cc_stats->last_ssthresh_bytes);
    print_delivery_stats(cc_stats);
    print_rate_stats(cc_stats);

out:
    if (bridge_started != 0) {
        tcp_shift_bridge_stop(&bridge);
        fprintf(stderr,
                "tcp-shift-p2: shutdown_bridge_active_flows=%llu "
                "shutdown_bridge_pending_public_bytes=%llu\n",
                (unsigned long long)bridge.active_flows,
                (unsigned long long)bridge.pending_public_bytes);
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