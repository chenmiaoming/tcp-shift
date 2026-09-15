#ifndef TCP_SHIFT_CC_BBR_PROBE_BW_H
#define TCP_SHIFT_CC_BBR_PROBE_BW_H

#include <stdint.h>

#include "cc/bbr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN 8U
#define TCP_SHIFT_BBR_PROBE_BW_UP_GAIN_NUM 320U
#define TCP_SHIFT_BBR_PROBE_BW_DOWN_GAIN_NUM 192U
#define TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM 256U
#define TCP_SHIFT_BBR_PROBE_BW_CWND_GAIN_NUM 512U

/* ProbeBW phase/timer state is controller state, not bandwidth/RTT estimator
 * state. Keeping it separate lets the estimator remain transport-neutral and
 * independently testable. */
struct tcp_shift_bbr_probe_bw_state {
    uint64_t cycle_start_ns;
    uint32_t cycle_index;
    uint8_t initialized;
};

/* Return the fixed-point pacing-gain numerator for one of the classic BBRv1
 * eight ProbeBW phases: 5/4, 3/4, then six 1x cruise phases. Zero means an
 * invalid index. */
uint32_t tcp_shift_bbr_probe_bw_pacing_gain_num(uint32_t cycle_index);

/* Initialize ProbeBW at an explicitly chosen phase and monotonic timestamp.
 * Normal mode reset must not start in the drain phase (index 1), matching the
 * Linux random-start constraint. Entropy/phase choice remains caller-owned so
 * the pure model has no random-number dependency. */
int tcp_shift_bbr_probe_bw_init(
    const struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_bw_state *probe,
    uint32_t initial_cycle_index,
    uint64_t now_ns);

/* Advance the classic phase cycle using explicit ACK time, prior inflight and
 * a current-sample loss signal. tcp-shift uses prior_inflight_bytes in place
 * of Linux's pacing/EDT-aware packets-in-network estimate. */
int tcp_shift_bbr_probe_bw_update_cycle(
    const struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_bw_state *probe,
    const struct tcp_shift_cc_rate_sample *sample,
    uint64_t now_ns,
    uint8_t loss_signal);

/* Publish ProbeBW pacing/cwnd policy. Pacing follows the current phase gain
 * with the common 1% margin; cwnd uses the classic steady-state 2*BDP gain. */
int tcp_shift_bbr_probe_bw_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_bw_state *probe,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint32_t current_cwnd_bytes,
    struct tcp_shift_cc_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_BBR_PROBE_BW_H */
