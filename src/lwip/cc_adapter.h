#ifndef TCP_SHIFT_LWIP_CC_ADAPTER_H
#define TCP_SHIFT_LWIP_CC_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "cc/cc.h"
#include "cc/observation.h"
#include "cc/registry.h"
#include "cc/reno.h"
#include "lwip/cc_hooks.h"
#include "lwip/tcp.h"

struct tcp_shift_lwip_cc_pacer_ops {
    int (*schedule)(void *arg,
                    uint64_t flow_id,
                    uint32_t generation,
                    uint64_t deadline_ns,
                    uint32_t bytes);
    int (*cancel)(void *arg,
                  uint64_t flow_id,
                  uint32_t generation,
                  size_t *cancelled);
};

struct tcp_shift_lwip_cc_stats {
    uint64_t bindings;
    uint64_t bind_failures;
    uint64_t ack_events;
    uint64_t loss_events;
    uint64_t timeout_events;
    uint64_t policy_updates;
    uint64_t controller_errors;
    uint64_t ack_observation_events;
    uint64_t srtt_updates;
    uint64_t ack_last_time_ns;
    uint64_t ack_last_smoothed_rtt_ns;
    uint64_t delivery_first_tx_events;
    uint64_t delivery_retransmit_events;
    uint64_t delivery_unique_retransmit_events;
    uint64_t delivery_repeat_retransmit_events;
    uint64_t delivery_acked_segment_events;
    uint64_t delivery_sack_events;
    uint64_t delivery_sack_payload_bytes;
    uint64_t delivery_payload_bytes;
    uint64_t delivery_metadata_alloc_failures;
    uint64_t delivery_metadata_misses;
    uint64_t delivery_metadata_abandoned_slots;
    uint64_t delivery_clock_errors;
    uint64_t delivery_timestamp_regressions;
    uint64_t delivery_last_tx_ns;
    uint64_t delivery_last_ack_ns;
    uint64_t rate_samples;
    uint64_t rate_valid_samples;
    uint64_t rate_invalid_samples;
    uint64_t rate_app_limited_samples;
    uint64_t rate_retransmitted_samples;
    uint64_t rate_partial_ack_events;
    uint64_t rate_multi_segment_ack_events;
    uint64_t rate_snapshot_samples;
    uint64_t rate_snapshot_errors;
    uint64_t app_limited_enters;
    uint64_t app_limited_exits;
    uint64_t rate_last_bytes_per_sec;
    uint64_t rate_max_bytes_per_sec;
    uint64_t rate_last_interval_ns;
    uint64_t rate_last_send_interval_ns;
    uint64_t rate_last_ack_interval_ns;
    uint64_t rate_last_rtt_ns;
    uint64_t rate_last_prior_delivered_bytes;
    uint64_t rate_last_delivered_total_bytes;
    uint64_t pacing_deferrals;
    uint64_t pacing_resume_events;
    uint64_t pacing_stale_releases;
    uint64_t pacing_scheduler_errors;
    uint64_t pacing_tx_events;
    uint64_t pacing_tx_bytes;
    uint64_t pacing_max_tx_gap_ns;
    uint64_t pacing_max_tx_gap_last_ack_age_ns;
    uint64_t pacing_max_tx_gap_last_release_age_ns;
    uint64_t pacing_last_rate_bytes_per_sec;
    uint64_t pacing_last_deadline_ns;
    uint64_t pacing_last_actual_release_ns;
    uint32_t rate_last_delivered_bytes;
    uint32_t rate_last_prior_inflight_bytes;
    uint32_t rate_last_flags;
    uint32_t delivery_metadata_bytes_per_slot;
    uint32_t delivery_live_slots;
    uint32_t delivery_peak_live_slots;
    uint32_t delivery_peak_slots_per_flow;
    uint32_t delivery_peak_capacity_slots_per_flow;
    uint32_t last_cwnd_bytes;
    uint32_t last_ssthresh_bytes;
    uint32_t pacing_max_tx_gap_cwnd_bytes;
    uint32_t pacing_max_tx_gap_effective_cwnd_bytes;
    uint32_t pacing_max_tx_gap_raw_inflight_bytes;
    uint32_t pacing_max_tx_gap_actual_inflight_bytes;
    uint32_t pacing_max_tx_gap_send_window_bytes;
    uint32_t pacing_max_tx_gap_recovery_owned;
    uint32_t pacing_max_tx_gap_tf_infr;
    uint32_t pacing_max_tx_gap_start_cwnd_bytes;
    uint32_t pacing_max_tx_gap_start_effective_cwnd_bytes;
    uint32_t pacing_max_tx_gap_start_raw_inflight_bytes;
    uint32_t pacing_max_tx_gap_start_actual_inflight_bytes;
    uint32_t pacing_max_tx_gap_start_send_window_bytes;
    uint32_t pacing_max_tx_gap_start_snd_buf_bytes;
    uint32_t pacing_max_tx_gap_start_recovery_owned;
    uint32_t pacing_max_tx_gap_start_tf_infr;

    /* Internal-BBR qualification telemetry. These remain zero for Reno/CUBIC
     * and record the latest compact-controller state for single-flow P6 runs. */
    uint64_t bbr_model_observations;
    uint64_t bbr_max_bw_bytes_per_sec;
    uint64_t bbr_min_rtt_ns;
    uint64_t bbr_full_bw_bytes_per_sec;
    uint64_t bbr_accepted_bw_samples;
    uint64_t bbr_ignored_app_limited_bw_samples;
    uint32_t bbr_round_count;
    uint32_t bbr_full_bw_count;
    uint32_t bbr_mode;
    uint32_t bbr_cycle_index;
    uint32_t bbr_full_bw_reached;
    uint32_t bbr_recovery_in_progress;

    /* Loss-path diagnostics for distinguishing recovery stalls from steady
     * BBR model/pacing limits. Times use CLOCK_MONOTONIC and are populated
     * only by the internal BBR qualification binding. */
    uint64_t bbr_recovery_enter_events;
    uint64_t bbr_recovery_exit_events;
    uint64_t bbr_recovery_total_ns;
    uint64_t bbr_recovery_max_ns;
    uint64_t bbr_recovery_packet_conservation_acks;
    uint64_t bbr_recovery_packet_conservation_clear_events;
    uint64_t bbr_recovery_packet_conservation_total_ns;
    uint64_t bbr_recovery_packet_conservation_max_ns;
    uint64_t bbr_recovery_last_packet_conservation_clear_ns;
    uint64_t bbr_recovery_last_enter_ns;
    uint64_t bbr_recovery_last_enter_delivered_bytes;
    uint64_t bbr_recovery_last_enter_round_boundary_bytes;
    uint64_t bbr_recovery_max_conservation_enter_delivered_bytes;
    uint64_t bbr_recovery_max_conservation_round_boundary_bytes;
    uint64_t bbr_recovery_max_conservation_prior_below_boundary_bytes;
    uint64_t bbr_recovery_max_conservation_clear_prior_delivered_bytes;
    uint64_t bbr_recovery_max_conservation_clear_delivered_total_bytes;
    uint64_t bbr_recovery_last_exit_ns;
    uint32_t bbr_recovery_last_enter_cwnd_bytes;
    uint32_t bbr_recovery_last_enter_inflight_bytes;
    uint32_t bbr_recovery_min_cwnd_bytes;
    uint32_t bbr_recovery_last_packet_conservation_cwnd_bytes;
    uint32_t bbr_recovery_last_packet_conservation_inflight_bytes;
    uint32_t bbr_recovery_last_enter_round_count;
    uint32_t bbr_recovery_max_conservation_enter_round_count;
    uint32_t bbr_recovery_max_conservation_clear_round_count;
    uint32_t bbr_recovery_max_conservation_clear_acked_bytes;
    uint32_t bbr_recovery_max_conservation_clear_rate_flags;

    uint64_t bbr_timeout_observations;
    uint64_t bbr_timeout_last_max_bw_bytes_per_sec;
    uint64_t bbr_timeout_last_pacing_rate_bytes_per_sec;
    uint32_t bbr_timeout_last_cwnd_bytes;
    uint32_t bbr_timeout_last_transport_inflight_bytes;
    uint32_t bbr_timeout_last_round_count;
    uint32_t bbr_timeout_last_mode;
    uint32_t bbr_timeout_last_cycle_index;
    uint32_t bbr_timeout_last_recovery_in_progress;
    uint32_t bbr_timeout_last_packet_conservation;
    uint32_t bbr_timeout_last_lastack;
    uint32_t bbr_timeout_last_snd_nxt;
    uint32_t bbr_timeout_last_recovery_end_seq;
    uint32_t bbr_timeout_last_dupacks;
    uint32_t bbr_timeout_last_nrtx;
    uint32_t bbr_timeout_last_unacked_segments;
    uint32_t bbr_timeout_last_unacked_bytes;
    uint32_t bbr_timeout_last_unsent_segments;
    uint32_t bbr_timeout_last_unsent_bytes;
    int32_t bbr_timeout_last_rtime;
    int32_t bbr_timeout_last_rto;
};

struct tcp_shift_lwip_cc_adapter {
    struct tcp_shift_lwip_cc_hook hook;
    struct tcp_shift_cc controller;
    union {
        union tcp_shift_cc_builtin_state controller_state;
        union tcp_shift_cc_builtin_state reno;
    };
    struct tcp_shift_cc_srtt srtt;
    struct tcp_shift_lwip_cc_stats *stats;
    struct tcp_pcb *pcb;
    void *delivery_slots;
    uint64_t delivered_bytes;
    uint64_t delivered_mstamp_ns;
    uint64_t delivery_last_event_ns;
    uint64_t delivery_last_clock_read_ns;
    uint64_t rate_first_tx_mstamp_ns;
    uint64_t app_limited_until_bytes;
    uint64_t pacing_rate_bytes_per_sec;
    uint64_t pacing_next_send_ns;
    uint64_t pacing_flow_id;
    uint32_t pacing_generation;
    uint32_t last_tx_cwnd_bytes;
    uint32_t last_tx_effective_cwnd_bytes;
    uint32_t last_tx_raw_inflight_bytes;
    uint32_t last_tx_actual_inflight_bytes;
    uint32_t last_tx_send_window_bytes;
    uint32_t last_tx_snd_buf_bytes;
    unsigned last_tx_recovery_owned;
    unsigned last_tx_tf_infr;
    uint16_t delivery_capacity;
    uint16_t delivery_live;
    unsigned pacing_scheduled;
    unsigned bound;
    unsigned heap_owned;
    unsigned sack_delivery_policy;
};

int tcp_shift_lwip_cc_adapter_bind(struct tcp_shift_lwip_cc_adapter *adapter,
                                   struct tcp_pcb *pcb,
                                   struct tcp_shift_lwip_cc_stats *stats);
void tcp_shift_lwip_cc_adapter_unbind(struct tcp_shift_lwip_cc_adapter *adapter);

int tcp_shift_lwip_cc_configure_pacer(
    const struct tcp_shift_lwip_cc_pacer_ops *ops,
    void *arg);
int tcp_shift_lwip_cc_clear_pacer(void);

int tcp_shift_lwip_cc_resume_paced(uint64_t flow_id,
                                   uint32_t generation,
                                   uint64_t actual_release_ns);

int tcp_shift_lwip_cc_configure_controller(const char *name);
const char *tcp_shift_lwip_cc_configured_controller_name(void);

/* Apply the configured selector to an already-bound ordinary Reno adapter.
 * Production calls this before the accepted child is handed to bridge code;
 * the public entrypoint also gives the deterministic integration contract a
 * way to exercise exactly the same reinitialization path. */
int tcp_shift_lwip_cc_apply_configured_controller(
    struct tcp_shift_lwip_cc_adapter *adapter);

/* Internal-only BBR binding used by P6 qualification. This does not add BBR to
 * the public registry or CLI. The full BBR state lives in a lazily allocated
 * PCB sidecar and the existing adapter still owns delivery sampling and pacing.
 * Retransmission and sender recovery remain transport-owned; internal BBR may
 * own only the recovery cwnd through the explicit hook contract. */
int tcp_shift_lwip_cc_apply_internal_bbr(
    struct tcp_shift_lwip_cc_adapter *adapter,
    uint32_t cycle_seed);
int tcp_shift_lwip_cc_internal_bbr_active(
    const struct tcp_shift_lwip_cc_adapter *adapter);

void tcp_shift_lwip_cc_accept_selected(struct tcp_pcb *pcb,
                                       tcp_accept_fn accept);

/* Qualification-only listener wrapper. It chains through the ordinary adapter
 * bind, then replaces Reno with the internal BBR sidecar before handing the
 * accepted PCB to bridge code. It is intentionally not used by production
 * tcp-shift-p2 and does not add BBR to the public selector/CLI. */
void tcp_shift_lwip_cc_accept_internal_bbr(struct tcp_pcb *pcb,
                                           tcp_accept_fn accept);
void tcp_shift_lwip_cc_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);
void tcp_shift_lwip_cc_accept_fixed_pacing(struct tcp_pcb *pcb,
                                           tcp_accept_fn accept);

void tcp_shift_lwip_cc_mark_app_limited(struct tcp_pcb *pcb);

const struct tcp_shift_lwip_cc_stats *tcp_shift_lwip_cc_get_stats(void);

#endif /* TCP_SHIFT_LWIP_CC_ADAPTER_H */
