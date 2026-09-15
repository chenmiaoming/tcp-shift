#ifndef TCP_SHIFT_CC_BBR_H
#define TCP_SHIFT_CC_BBR_H

#include <stdint.h>

#include "cc/cc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS 10U
#define TCP_SHIFT_BBR_PROBE_RTT_INTERVAL_NS UINT64_C(5000000000)
#define TCP_SHIFT_BBR_MIN_RTT_FILTER_NS UINT64_C(10000000000)
#define TCP_SHIFT_BBR_FULL_BW_ROUNDS 3U

/* Linux BBRv1 represents gains in BBR_SCALE=8 fixed point. Its Startup
 * high_gain is BBR_UNIT * 2885 / 1000 + 1 = 739/256. Keep the quantized
 * value explicit so compact-bbr arithmetic matches the reference rather than
 * an unquantized decimal approximation. */
#define TCP_SHIFT_BBR_GAIN_DEN 256U
#define TCP_SHIFT_BBR_STARTUP_GAIN_NUM 739U
#define TCP_SHIFT_BBR_PACING_MARGIN_NUM 99U
#define TCP_SHIFT_BBR_PACING_MARGIN_DEN 100U
#define TCP_SHIFT_BBR_MIN_CWND_PACKETS 4U

enum tcp_shift_bbr_mode {
    TCP_SHIFT_BBR_MODE_STARTUP = 0,
    TCP_SHIFT_BBR_MODE_DRAIN = 1,
    TCP_SHIFT_BBR_MODE_PROBE_BW = 2,
    TCP_SHIFT_BBR_MODE_PROBE_RTT = 3,
};

/* Pure transport-independent state for tcp-shift's compact BBR controller.
 *
 * This is deliberately not a BBRv3-equivalence claim. The compact controller
 * follows the BBRv1-style four-mode shape and selectively retains independently
 * useful newer semantics, such as conservative app-limited sample admission.
 * Runtime clocks, timers, packet queues, recovery and pacing stay outside this
 * state; the caller supplies monotonic time and delivery observations.
 */
struct tcp_shift_bbr_model {
    uint64_t max_bw_bytes_per_sec;
    uint64_t max_bw_filter[TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS];

    uint64_t min_rtt_ns;
    uint64_t min_rtt_stamp_ns;
    uint64_t probe_rtt_min_delay_ns;
    uint64_t probe_rtt_min_stamp_ns;
    uint64_t last_update_ns;

    uint64_t valid_rate_samples;
    uint64_t accepted_bw_samples;
    uint64_t ignored_app_limited_bw_samples;
    uint64_t valid_rtt_samples;

    uint64_t next_round_delivered;
    uint64_t full_bw_bytes_per_sec;

    uint32_t bw_filter_round;
    uint32_t round_count;
    uint32_t full_bw_count;
    enum tcp_shift_bbr_mode mode;

    uint8_t has_min_rtt;
    uint8_t has_probe_rtt_min;
    uint8_t has_update_time;
    uint8_t has_bw_filter_round;
    uint8_t probe_rtt_expired;
    uint8_t min_rtt_expired;
    uint8_t round_start;
    uint8_t full_bw_now;
    uint8_t full_bw_reached;
};

void tcp_shift_bbr_model_init(struct tcp_shift_bbr_model *model);

int tcp_shift_bbr_model_on_ack(struct tcp_shift_bbr_model *model,
                               const struct tcp_shift_cc_rate_sample *sample,
                               uint64_t now_ns);

/* Overflow-safe model arithmetic. BDP is rounded upward so integer truncation
 * cannot create a negative feedback loop. Saturation is UINT64_MAX. */
uint64_t tcp_shift_bbr_bdp_bytes(uint64_t bandwidth_bytes_per_sec,
                                 uint64_t rtt_ns);

/* Startup target helpers use the Linux BBRv1 fixed-point high gain. The pacing
 * helper additionally applies the 1% pacing margin. */
uint64_t tcp_shift_bbr_startup_pacing_rate_bytes_per_sec(
    uint64_t bandwidth_bytes_per_sec);
uint32_t tcp_shift_bbr_startup_cwnd_target_bytes(
    uint64_t bandwidth_bytes_per_sec,
    uint64_t min_rtt_ns,
    uint32_t mss_bytes,
    uint32_t cwnd_limit_bytes,
    uint32_t initial_cwnd_bytes);

/* Publish compact-BBR Startup cwnd/pacing policy from explicit current policy
 * state. This is pure policy math only: no clock, lwIP object, timer, heap, or
 * registry binding is owned here. Before full pipe the pacing rate never
 * decreases; cwnd grows by newly ACKed bytes while below the target or until
 * the initial window has been delivered. */
int tcp_shift_bbr_startup_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint32_t initial_cwnd_bytes,
    uint32_t current_cwnd_bytes,
    uint64_t current_pacing_rate_bytes_per_sec,
    struct tcp_shift_cc_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_BBR_H */
