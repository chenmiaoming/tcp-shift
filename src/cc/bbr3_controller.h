#ifndef TCP_SHIFT_CC_BBR3_CONTROLLER_H
#define TCP_SHIFT_CC_BBR3_CONTROLLER_H

#include <stdint.h>

#include "cc/bbr3_model.h"
#include "cc/bbr3_probe.h"
#include "cc/bbr3_congestion.h"

/*
 * Transport-independent controller integration, not a production CC adapter.
 * The caller must supply a consistent validated public-leg RACK/SACK delivery
 * snapshot. This is deliberately NOT registered in the production selector.
 */
#define TCP_SHIFT_BBR3_EXTRA_ACKED_ROUNDS 10U
#define TCP_SHIFT_BBR3_PROBE_RTT_DURATION_NS UINT64_C(200000000)
#define TCP_SHIFT_BBR3_PACING_MARGIN_NUM 99U

struct tcp_shift_bbr3_controller_ack {
    struct tcp_shift_cc_rate_sample rate;
    uint64_t now_ns;
    uint64_t lost_bytes;
    uint64_t ce_bytes;
    uint64_t tx_inflight_bytes;
    uint32_t acked_bytes;
    uint32_t inflight_bytes;
    uint8_t validated_loss;
    uint8_t ecn_eligible;
    uint8_t loss_round_end;
    uint8_t probe_sample;
    uint8_t cwnd_limited;
    uint8_t idle_restart;
};

struct tcp_shift_bbr3_controller_policy {
    uint64_t pacing_rate_bytes_per_sec;
    uint64_t max_inflight_bytes;
    uint64_t extra_acked_bytes;
    uint32_t cwnd_bytes;
    uint32_t send_quantum_bytes;
    uint8_t mark_app_limited;
    enum tcp_shift_bbr3_phase phase;
};

struct tcp_shift_bbr3_controller {
    struct tcp_shift_bbr3_model model;
    struct tcp_shift_bbr3_probe probe;
    struct tcp_shift_bbr3_congestion congestion;
    uint64_t extra_acked_values[TCP_SHIFT_BBR3_EXTRA_ACKED_ROUNDS];
    uint32_t extra_acked_rounds[TCP_SHIFT_BBR3_EXTRA_ACKED_ROUNDS];
    uint64_t extra_acked_interval_stamp_ns;
    uint64_t extra_acked_delivered_bytes;
    uint64_t extra_acked_bytes;
    uint64_t probe_rtt_done_stamp_ns;
    uint64_t probe_rtt_round_delivered;
    uint64_t pacing_rate_bytes_per_sec;
    uint32_t initial_cwnd_bytes;
    uint32_t cwnd_bytes;
    uint32_t prior_cwnd_bytes;
    uint32_t mss_bytes;
    uint32_t flow_seed;
    uint32_t drain_start_round;
    uint8_t probe_rtt_round_done;
    uint8_t probe_rtt_started;
};

int tcp_shift_bbr3_controller_init(struct tcp_shift_bbr3_controller *state,
                                    uint32_t initial_cwnd_bytes,
                                    uint32_t mss_bytes,
                                    uint32_t flow_seed,
                                    uint64_t now_ns);

int tcp_shift_bbr3_controller_on_ack(
    struct tcp_shift_bbr3_controller *state,
    const struct tcp_shift_bbr3_controller_ack *ack,
    struct tcp_shift_bbr3_controller_policy *policy);

#endif
