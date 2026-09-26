#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge/bridge.h"
#include "host/ifconfig.h"
#include "host/tun.h"
#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/l3_tun.h"
#include "lwip/tcp_memory.h"
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
#ifdef TCP_SHIFT_INTERNAL_BBR_QUALIFICATION
    fprintf(stderr,
            "usage: %s <tun-name> <lwip-ipv4> <netmask> <host-ipv4> "
            "<public-port> <backend-port> bbr-internal\n",
            program);
#else
    fprintf(stderr,
            "usage: %s <tun-name> <lwip-ipv4> <netmask> <host-ipv4> "
            "<public-port> <backend-port> [cc]\n"
            "  cc: reno (default) | cubic\n"
            "example: %s ts0 10.0.0.2 255.255.255.252 10.0.0.1 "
            "18090 19090 cubic\n",
            program, program);
#endif
}

static int tcp_shift_p2_pacer_schedule(void *arg,
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

static int tcp_shift_p2_pacer_cancel(void *arg,
                                     uint64_t flow_id,
                                     uint32_t generation,
                                     size_t *cancelled)
{
    return tcp_shift_lwip_loop_pacer_cancel(arg, flow_id, generation,
                                             cancelled);
}

static int tcp_shift_p2_pacer_release(void *arg,
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

static const struct tcp_shift_lwip_cc_pacer_ops tcp_shift_p2_pacer_ops = {
    .schedule = tcp_shift_p2_pacer_schedule,
    .cancel = tcp_shift_p2_pacer_cancel,
};

static int tcp_shift_p2_configure_pacer(struct tcp_shift_lwip_loop *loop)
{
    if (tcp_shift_lwip_loop_set_pacer_release(loop,
                                              tcp_shift_p2_pacer_release,
                                              NULL) < 0) {
        return -1;
    }
    return tcp_shift_lwip_cc_configure_pacer(&tcp_shift_p2_pacer_ops, loop);
}

static void print_delivery_stats(const struct tcp_shift_lwip_cc_stats *stats)
{
    fprintf(stderr,
            "tcp-shift-p2-delivery: first_tx_events=%llu "
            "retransmit_events=%llu unique_retransmit_events=%llu "
            "repeat_retransmit_events=%llu acked_segment_events=%llu "
            "sack_events=%llu sack_payload_bytes=%llu "
            "delivered_payload_bytes=%llu metadata_alloc_failures=%llu "
            "metadata_misses=%llu metadata_abandoned_slots=%llu "
            "clock_errors=%llu timestamp_regressions=%llu "
            "last_tx_ns=%llu last_ack_ns=%llu metadata_bytes_per_slot=%u "
            "live_slots=%u peak_live_slots=%u peak_slots_per_flow=%u "
            "peak_capacity_slots_per_flow=%u\n",
            (unsigned long long)stats->delivery_first_tx_events,
            (unsigned long long)stats->delivery_retransmit_events,
            (unsigned long long)stats->delivery_unique_retransmit_events,
            (unsigned long long)stats->delivery_repeat_retransmit_events,
            (unsigned long long)stats->delivery_acked_segment_events,
            (unsigned long long)stats->delivery_sack_events,
            (unsigned long long)stats->delivery_sack_payload_bytes,
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

static void print_pacing_stats(const struct tcp_shift_lwip_cc_stats *stats,
                               const struct tcp_shift_lwip_loop *loop)
{
    const struct tcp_shift_pacer_stats *pacer =
        tcp_shift_lwip_loop_pacer_stats(loop);

    if (pacer == NULL) {
        return;
    }
    fprintf(stderr,
            "tcp-shift-p2-pacing: deferrals=%llu resume_events=%llu "
            "stale_releases=%llu scheduler_errors=%llu tx_events=%llu "
            "tx_bytes=%llu max_tx_gap_ns=%llu "
            "max_tx_gap_last_ack_age_ns=%llu "
            "max_tx_gap_last_release_age_ns=%llu "
            "max_tx_gap_cwnd_bytes=%u max_tx_gap_effective_cwnd_bytes=%u "
            "max_tx_gap_raw_inflight_bytes=%u "
            "max_tx_gap_actual_inflight_bytes=%u "
            "max_tx_gap_send_window_bytes=%u "
            "max_tx_gap_recovery_owned=%u max_tx_gap_tf_infr=%u "
            "max_tx_gap_start_cwnd_bytes=%u "
            "max_tx_gap_start_effective_cwnd_bytes=%u "
            "max_tx_gap_start_raw_inflight_bytes=%u "
            "max_tx_gap_start_actual_inflight_bytes=%u "
            "max_tx_gap_start_send_window_bytes=%u "
            "max_tx_gap_start_snd_buf_bytes=%u "
            "max_tx_gap_start_recovery_owned=%u "
            "max_tx_gap_start_tf_infr=%u "
            "last_rate_bytes_per_sec=%llu "
            "last_deadline_ns=%llu last_actual_release_ns=%llu "
            "loop_pacing_wakeups=%llu loop_release_callbacks=%llu "
            "loop_callback_errors=%llu timerfd_creates=%llu "
            "timer_arms=%llu timer_rearms=%llu timer_disarms=%llu "
            "timer_expirations=%llu scheduled_events=%llu "
            "released_events=%llu cancelled_events=%llu "
            "released_bytes=%llu heap_current=%zu heap_peak=%zu "
            "heap_capacity=%zu max_lateness_ns=%llu\n",
            (unsigned long long)stats->pacing_deferrals,
            (unsigned long long)stats->pacing_resume_events,
            (unsigned long long)stats->pacing_stale_releases,
            (unsigned long long)stats->pacing_scheduler_errors,
            (unsigned long long)stats->pacing_tx_events,
            (unsigned long long)stats->pacing_tx_bytes,
            (unsigned long long)stats->pacing_max_tx_gap_ns,
            (unsigned long long)stats->pacing_max_tx_gap_last_ack_age_ns,
            (unsigned long long)stats->pacing_max_tx_gap_last_release_age_ns,
            stats->pacing_max_tx_gap_cwnd_bytes,
            stats->pacing_max_tx_gap_effective_cwnd_bytes,
            stats->pacing_max_tx_gap_raw_inflight_bytes,
            stats->pacing_max_tx_gap_actual_inflight_bytes,
            stats->pacing_max_tx_gap_send_window_bytes,
            stats->pacing_max_tx_gap_recovery_owned,
            stats->pacing_max_tx_gap_tf_infr,
            stats->pacing_max_tx_gap_start_cwnd_bytes,
            stats->pacing_max_tx_gap_start_effective_cwnd_bytes,
            stats->pacing_max_tx_gap_start_raw_inflight_bytes,
            stats->pacing_max_tx_gap_start_actual_inflight_bytes,
            stats->pacing_max_tx_gap_start_send_window_bytes,
            stats->pacing_max_tx_gap_start_snd_buf_bytes,
            stats->pacing_max_tx_gap_start_recovery_owned,
            stats->pacing_max_tx_gap_start_tf_infr,
            (unsigned long long)stats->pacing_last_rate_bytes_per_sec,
            (unsigned long long)stats->pacing_last_deadline_ns,
            (unsigned long long)stats->pacing_last_actual_release_ns,
            (unsigned long long)loop->pacing_wakeups,
            (unsigned long long)loop->pacing_release_callbacks,
            (unsigned long long)loop->pacing_callback_errors,
            (unsigned long long)pacer->timerfd_creates,
            (unsigned long long)pacer->timer_arms,
            (unsigned long long)pacer->timer_rearms,
            (unsigned long long)pacer->timer_disarms,
            (unsigned long long)pacer->timer_expirations,
            (unsigned long long)pacer->scheduled_events,
            (unsigned long long)pacer->released_events,
            (unsigned long long)pacer->cancelled_events,
            (unsigned long long)pacer->released_bytes,
            pacer->heap_current,
            pacer->heap_peak,
            pacer->heap_capacity,
            (unsigned long long)pacer->max_lateness_ns);
}

static void print_tcp_memory_stats(void)
{
    const struct tcp_shift_tcp_memory_config *config =
        tcp_shift_lwip_tcp_memory_process_config();
    const struct tcp_shift_tcp_memory_stats *stats =
        tcp_shift_lwip_tcp_memory_process_stats();

    if (config == NULL || stats == NULL) {
        return;
    }

    fprintf(stderr,
            "tcp-shift-p2-tcp-memory: wmem_min=%u wmem_initial=%u "
            "wmem_max=%u compile_ceiling=%u mem_low=%llu "
            "mem_pressure=%llu mem_high=%llu flow_inits=%llu "
            "write_events=%llu write_bytes=%llu ack_events=%llu "
            "ack_bytes=%llu charged_bytes=%llu peak_charged_bytes=%llu "
            "pressure_enters=%llu pressure_exits=%llu high_blocks=%llu "
            "sndbuf_blocks=%llu upstream_write_mem_errors=%llu "
            "growth_events=%llu growth_bytes=%llu growth_suppressed=%llu "
            "last_block_snd_buf_bytes=%u last_block_requested_bytes=%u "
            "last_block_capacity_bytes=%u last_block_queued_bytes=%llu "
            "last_block_snd_queuelen=%u accounting_underflows=%llu\n",
            config->wmem.min_bytes,
            config->wmem.initial_bytes,
            config->wmem.max_bytes,
            config->compile_ceiling_bytes,
            (unsigned long long)config->mem.low_bytes,
            (unsigned long long)config->mem.pressure_bytes,
            (unsigned long long)config->mem.high_bytes,
            (unsigned long long)stats->flow_inits,
            (unsigned long long)stats->write_events,
            (unsigned long long)stats->write_bytes,
            (unsigned long long)stats->ack_events,
            (unsigned long long)stats->ack_bytes,
            (unsigned long long)stats->charged_bytes,
            (unsigned long long)stats->peak_charged_bytes,
            (unsigned long long)stats->pressure_enters,
            (unsigned long long)stats->pressure_exits,
            (unsigned long long)stats->high_blocks,
            (unsigned long long)stats->sndbuf_blocks,
            (unsigned long long)stats->upstream_write_mem_errors,
            (unsigned long long)stats->growth_events,
            (unsigned long long)stats->growth_bytes,
            (unsigned long long)stats->growth_suppressed,
            stats->last_block_snd_buf_bytes,
            stats->last_block_requested_bytes,
            stats->last_block_capacity_bytes,
            (unsigned long long)stats->last_block_queued_bytes,
            stats->last_block_snd_queuelen,
            (unsigned long long)stats->accounting_underflows);
}

static void print_bbr_stats(const struct tcp_shift_lwip_cc_stats *stats)
{
    if (stats->bbr_model_observations == 0U) {
        return;
    }

    fprintf(stderr,
            "tcp-shift-p2-bbr: observations=%llu mode=%u "
            "max_bw_bytes_per_sec=%llu min_rtt_ns=%llu "
            "full_bw_bytes_per_sec=%llu full_bw_reached=%u "
            "full_bw_count=%u round_count=%u cycle_index=%u "
            "accepted_bw_samples=%llu ignored_app_limited_bw_samples=%llu "
            "recovery_in_progress=%u recovery_enter_events=%llu "
            "recovery_exit_events=%llu recovery_total_ns=%llu "
            "recovery_max_ns=%llu recovery_packet_conservation_acks=%llu "
            "recovery_packet_conservation_clear_events=%llu "
            "recovery_packet_conservation_total_ns=%llu "
            "recovery_packet_conservation_max_ns=%llu "
            "recovery_last_packet_conservation_clear_ns=%llu "
            "recovery_last_enter_ns=%llu recovery_last_exit_ns=%llu "
            "recovery_last_enter_delivered_bytes=%llu "
            "recovery_last_enter_round_boundary_bytes=%llu "
            "recovery_last_enter_sack_ack_age_ns=%llu "
            "recovery_max_conservation_enter_delivered_bytes=%llu "
            "recovery_max_conservation_round_boundary_bytes=%llu "
            "recovery_max_conservation_prior_below_boundary_bytes=%llu "
            "recovery_max_conservation_clear_prior_delivered_bytes=%llu "
            "recovery_max_conservation_clear_delivered_total_bytes=%llu "
            "recovery_last_enter_cwnd_bytes=%u "
            "recovery_last_enter_inflight_bytes=%u "
            "recovery_min_cwnd_bytes=%u "
            "recovery_last_packet_conservation_cwnd_bytes=%u "
            "recovery_last_packet_conservation_inflight_bytes=%u "
            "recovery_last_enter_round_count=%u "
            "recovery_last_enter_sack_acked_bytes=%u "
            "recovery_max_conservation_enter_round_count=%u "
            "recovery_max_conservation_enter_sack_acked_bytes=%u "
            "recovery_max_conservation_enter_sack_ack_age_ns=%llu "
            "recovery_max_conservation_clear_round_count=%u "
            "recovery_max_conservation_clear_acked_bytes=%u "
            "recovery_max_conservation_clear_rate_flags=%u "
            "timeout_observations=%llu "
            "timeout_last_mode=%u timeout_last_cycle_index=%u "
            "timeout_last_round_count=%u timeout_last_cwnd_bytes=%u "
            "timeout_last_transport_inflight_bytes=%u "
            "timeout_last_max_bw_bytes_per_sec=%llu "
            "timeout_last_pacing_rate_bytes_per_sec=%llu "
            "timeout_last_recovery_in_progress=%u "
            "timeout_last_packet_conservation=%u "
            "timeout_last_lastack=%u timeout_last_snd_nxt=%u "
            "timeout_last_recovery_end_seq=%u timeout_last_dupacks=%u "
            "timeout_last_nrtx=%u timeout_last_unacked_segments=%u "
            "timeout_last_unacked_bytes=%u timeout_last_unsent_segments=%u "
            "timeout_last_unsent_bytes=%u timeout_last_rtime=%d "
            "timeout_last_rto=%d\n",
            (unsigned long long)stats->bbr_model_observations,
            stats->bbr_mode,
            (unsigned long long)stats->bbr_max_bw_bytes_per_sec,
            (unsigned long long)stats->bbr_min_rtt_ns,
            (unsigned long long)stats->bbr_full_bw_bytes_per_sec,
            stats->bbr_full_bw_reached,
            stats->bbr_full_bw_count,
            stats->bbr_round_count,
            stats->bbr_cycle_index,
            (unsigned long long)stats->bbr_accepted_bw_samples,
            (unsigned long long)stats->bbr_ignored_app_limited_bw_samples,
            stats->bbr_recovery_in_progress,
            (unsigned long long)stats->bbr_recovery_enter_events,
            (unsigned long long)stats->bbr_recovery_exit_events,
            (unsigned long long)stats->bbr_recovery_total_ns,
            (unsigned long long)stats->bbr_recovery_max_ns,
            (unsigned long long)stats->bbr_recovery_packet_conservation_acks,
            (unsigned long long)
                stats->bbr_recovery_packet_conservation_clear_events,
            (unsigned long long)
                stats->bbr_recovery_packet_conservation_total_ns,
            (unsigned long long)
                stats->bbr_recovery_packet_conservation_max_ns,
            (unsigned long long)
                stats->bbr_recovery_last_packet_conservation_clear_ns,
            (unsigned long long)stats->bbr_recovery_last_enter_ns,
            (unsigned long long)stats->bbr_recovery_last_exit_ns,
            (unsigned long long)
                stats->bbr_recovery_last_enter_delivered_bytes,
            (unsigned long long)
                stats->bbr_recovery_last_enter_round_boundary_bytes,
            (unsigned long long)
                stats->bbr_recovery_last_enter_sack_ack_age_ns,
            (unsigned long long)
                stats->bbr_recovery_max_conservation_enter_delivered_bytes,
            (unsigned long long)
                stats->bbr_recovery_max_conservation_round_boundary_bytes,
            (unsigned long long)
                stats->bbr_recovery_max_conservation_prior_below_boundary_bytes,
            (unsigned long long)
                stats->bbr_recovery_max_conservation_clear_prior_delivered_bytes,
            (unsigned long long)
                stats->bbr_recovery_max_conservation_clear_delivered_total_bytes,
            stats->bbr_recovery_last_enter_cwnd_bytes,
            stats->bbr_recovery_last_enter_inflight_bytes,
            stats->bbr_recovery_min_cwnd_bytes,
            stats->bbr_recovery_last_packet_conservation_cwnd_bytes,
            stats->bbr_recovery_last_packet_conservation_inflight_bytes,
            stats->bbr_recovery_last_enter_round_count,
            stats->bbr_recovery_last_enter_sack_acked_bytes,
            stats->bbr_recovery_max_conservation_enter_round_count,
            stats->bbr_recovery_max_conservation_enter_sack_acked_bytes,
            (unsigned long long)
                stats->bbr_recovery_max_conservation_enter_sack_ack_age_ns,
            stats->bbr_recovery_max_conservation_clear_round_count,
            stats->bbr_recovery_max_conservation_clear_acked_bytes,
            stats->bbr_recovery_max_conservation_clear_rate_flags,
            (unsigned long long)stats->bbr_timeout_observations,
            stats->bbr_timeout_last_mode,
            stats->bbr_timeout_last_cycle_index,
            stats->bbr_timeout_last_round_count,
            stats->bbr_timeout_last_cwnd_bytes,
            stats->bbr_timeout_last_transport_inflight_bytes,
            (unsigned long long)stats->bbr_timeout_last_max_bw_bytes_per_sec,
            (unsigned long long)stats->bbr_timeout_last_pacing_rate_bytes_per_sec,
            stats->bbr_timeout_last_recovery_in_progress,
            stats->bbr_timeout_last_packet_conservation,
            stats->bbr_timeout_last_lastack,
            stats->bbr_timeout_last_snd_nxt,
            stats->bbr_timeout_last_recovery_end_seq,
            stats->bbr_timeout_last_dupacks,
            stats->bbr_timeout_last_nrtx,
            stats->bbr_timeout_last_unacked_segments,
            stats->bbr_timeout_last_unacked_bytes,
            stats->bbr_timeout_last_unsent_segments,
            stats->bbr_timeout_last_unsent_bytes,
            stats->bbr_timeout_last_rtime,
            stats->bbr_timeout_last_rto);
}

int main(int argc, char **argv)
{
    struct tcp_shift_tun tun;
    struct tcp_shift_l3_tun l3;
    struct tcp_shift_lwip_loop loop;
    struct tcp_shift_bridge bridge;
    const struct tcp_shift_lwip_cc_stats *cc_stats;
    const char *cc_name;
    ip4_addr_t address;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
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

#ifdef TCP_SHIFT_INTERNAL_BBR_QUALIFICATION
    if (argc != 8 || strcmp(argv[7], "bbr-internal") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
#else
    if (argc != 7 && argc != 8) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
#endif
    if (parse_ipv4(argv[2], &address) < 0 ||
        parse_ipv4(argv[3], &netmask) < 0 ||
        parse_ipv4(argv[4], &gateway) < 0 ||
        parse_port(argv[5], &public_port) < 0 ||
        parse_port(argv[6], &backend_port) < 0) {
        return EXIT_FAILURE;
    }

#ifdef TCP_SHIFT_INTERNAL_BBR_QUALIFICATION
    cc_name = "bbr-internal";
#else
    cc_name = argc == 8 ? argv[7] : "reno";
    if (tcp_shift_lwip_cc_configure_controller(cc_name) < 0) {
        fprintf(stderr, "unsupported congestion controller: %s\n", cc_name);
        return EXIT_FAILURE;
    }
    cc_name = tcp_shift_lwip_cc_configured_controller_name();
    if (cc_name == NULL) {
        fprintf(stderr, "congestion controller registry unavailable\n");
        return EXIT_FAILURE;
    }
#endif

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

    if (tcp_shift_p2_configure_pacer(&loop) < 0) {
        fprintf(stderr, "configure P5c pacer service failed\n");
        goto out;
    }
    pacer_configured = 1;

    if (tcp_shift_bridge_start_ipv4(&bridge, &loop, public_port,
                                    backend_port) < 0) {
        perror("start TCP bridge");
        goto out;
    }
    bridge_started = 1;

    printf("tcp-shift-p2: ready tun=%s host-ipv4=%s lwip-ipv4=%s mtu=%u "
           "public-port=%u backend=127.0.0.1:%u cc=%s\n",
           tun.ifname, argv[4], argv[2], (unsigned)l3.netif.mtu,
           (unsigned)public_port, (unsigned)backend_port, cc_name);
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
    print_pacing_stats(cc_stats, &loop);
    print_bbr_stats(cc_stats);
    print_tcp_memory_stats();

out:
    if (bridge_started != 0) {
        tcp_shift_bridge_stop(&bridge);
        fprintf(stderr,
                "tcp-shift-p2: shutdown_bridge_active_flows=%llu "
                "shutdown_bridge_pending_public_bytes=%llu\n",
                (unsigned long long)bridge.active_flows,
                (unsigned long long)bridge.pending_public_bytes);
    }
    if (pacer_configured != 0 && tcp_shift_lwip_cc_clear_pacer() < 0) {
        fprintf(stderr,
                "tcp-shift-p2: pacer_service_retained_until_process_exit=1\n");
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
