#ifndef TCP_SHIFT_CC_BBR_PROBE_H
#define TCP_SHIFT_CC_BBR_PROBE_H

#include <stdint.h>

#include "cc/bbr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN 8U
#define TCP_SHIFT_BBR_PROBE_BW_SEED_SPAN 7U
#define TCP_SHIFT_BBR_PROBE_BW_UP_GAIN_NUM 320U
#define TCP_SHIFT_BBR_PROBE_BW_DOWN_GAIN_NUM 192U
#define TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM 256U
#define TCP_SHIFT_BBR_CWND_GAIN_NUM 512U
#define TCP_SHIFT_BBR_PROBE_RTT_DURATION_NS UINT64_C(200000000)

/* Probe-mode state is kept separate from the bandwidth/min-RTT estimator so
 * extending the steady-state mode machine does not inflate or obscure the
 * estimator contract. A future controller state can compose both structs.
 *
 * cycle_seed is supplied by the caller. The pure-C controller never owns an
 * RNG; production integration should provide a per-flow-distinguishing seed.
 */
struct tcp_shift_bbr_probe_state {
    uint64_t cycle_start_ns;
    uint64_t probe_rtt_done_stamp_ns;
    uint32_t probe_rtt_prior_cwnd_bytes;
    uint8_t cycle_index;
    uint8_t cycle_seed;
    uint8_t cycle_started;
    uint8_t probe_rtt_round_done;
};

void tcp_shift_bbr_probe_state_init(struct tcp_shift_bbr_probe_state *state,
                                    uint32_t cycle_seed);

uint32_t tcp_shift_bbr_probe_bw_pacing_gain_num(
    const struct tcp_shift_bbr_probe_state *state);
uint64_t tcp_shift_bbr_probe_bw_pacing_rate_bytes_per_sec(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_state *state);

/* Advance the Linux-BBRv1-style 8-phase ProbeBW cycle. The first phase is
 * selected from the caller-supplied seed using the same phase set as Linux's
 * randomized reset: probe-up or one of the six cruise phases, never the drain
 * phase. No wall-clock source is owned here; now_ns is supplied by the caller.
 *
 * RETRANSMITTED is used only as the transport-neutral signal that a probe-up
 * phase should stop waiting for its 1.25*BDP inflight target after one min RTT.
 */
int tcp_shift_bbr_probe_bw_update(
    struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_rate_sample *sample,
    uint64_t now_ns);

/* ProbeBW uses the phase pacing gain and a steady-state cwnd target of 2*BDP,
 * with the usual four-packet minimum and transport representation ceiling. */
int tcp_shift_bbr_probe_bw_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint32_t current_cwnd_bytes,
    struct tcp_shift_cc_policy *policy);

/* Enter ProbeRTT when the estimator reports an expired min-RTT window. Once
 * inflight reaches the four-packet target, remain there for at least 200 ms and
 * at least one packet-timed round. Return 1 only when ProbeRTT completes and
 * the mode changes back to ProbeBW or Startup, 0 otherwise, and -1 on invalid
 * input. */
int tcp_shift_bbr_probe_rtt_update(
    struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_rate_sample *sample,
    uint32_t current_cwnd_bytes,
    uint64_t now_ns);

int tcp_shift_bbr_probe_rtt_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy);

/* Linux BBRv1 restores the cwnd saved on ProbeRTT entry. Keep that restoration
 * explicit so the eventual transport controller can apply it on the ACK that
 * exits ProbeRTT without giving the model ownership of a socket object. */
uint32_t tcp_shift_bbr_probe_rtt_restore_cwnd_bytes(
    const struct tcp_shift_bbr_probe_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t cwnd_limit_bytes);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_BBR_PROBE_H */
