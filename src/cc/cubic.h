#ifndef TCP_SHIFT_CC_CUBIC_H
#define TCP_SHIFT_CC_CUBIC_H

#include <stdint.h>

#include "cc/cc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SHIFT_CUBIC_Q_SHIFT 16U
#define TCP_SHIFT_CUBIC_Q_ONE UINT64_C(65536)
#define TCP_SHIFT_CUBIC_TIME_Q_SHIFT 10U
#define TCP_SHIFT_CUBIC_BETA_NUM 7U
#define TCP_SHIFT_CUBIC_BETA_DEN 10U
#define TCP_SHIFT_CUBIC_RENO_ALPHA_NUM 9U
#define TCP_SHIFT_CUBIC_RENO_ALPHA_DEN 17U
#define TCP_SHIFT_CUBIC_FAST_CONVERGENCE_NUM 17U
#define TCP_SHIFT_CUBIC_FAST_CONVERGENCE_DEN 20U

/* Linux tcp_cubic classic HyStart defaults. tcp-shift currently publishes no
 * CUBIC pacing rate, so ACK-train detection uses Linux's unpaced threshold. */
#define TCP_SHIFT_CUBIC_HYSTART_LOW_WINDOW 16U
#define TCP_SHIFT_CUBIC_HYSTART_MIN_SAMPLES 8U
#define TCP_SHIFT_CUBIC_HYSTART_ACK_DELTA_NS UINT64_C(2000000)
#define TCP_SHIFT_CUBIC_HYSTART_DELAY_MIN_NS UINT64_C(4000000)
#define TCP_SHIFT_CUBIC_HYSTART_DELAY_MAX_NS UINT64_C(16000000)

/* With windows in Q16 segments and time in Q10 seconds, C=0.4=2/5 gives:
 *   cubic_term_q16 = abs(t_q10 - K_q10)^3 / 40960
 *   K_q10^3        = (Wmax_q16 - Wepoch_q16) * 40960
 */
#define TCP_SHIFT_CUBIC_SCALE UINT64_C(40960)

/* Deterministic, transport-neutral RFC 9438 model checkpoint.
 *
 * Window quantities use Q16 SMSS-sized segments. Time-dependent ACK handling
 * takes explicit monotonic now_ns and smoothed RTT input; no clock syscall,
 * floating point, heap allocation, or transport object is owned here.
 */
struct tcp_shift_cubic_model {
    uint64_t cwnd_q16;
    uint64_t ssthresh_q16;
    uint64_t w_max_q16;
    uint64_t w_est_q16;
    uint64_t cwnd_prior_q16;
    uint64_t cwnd_epoch_q16;
    uint64_t k_q10;
    uint64_t epoch_start_ns;
    uint64_t last_ack_time_ns;
    uint64_t app_limited_since_ns;
    uint64_t last_target_q16;

    uint64_t hystart_delay_min_ns;
    uint64_t hystart_curr_rtt_ns;
    uint64_t hystart_round_start_ns;
    uint64_t hystart_last_ack_ns;
    uint64_t hystart_next_round_delivered;

    uint32_t mss_bytes;
    uint32_t min_cwnd_bytes;
    uint32_t ack_events;
    uint32_t loss_events;
    uint32_t timeout_events;
    uint32_t app_limited_acks;
    uint32_t hystart_sample_count;
    uint32_t hystart_exit_events;

    uint8_t has_w_max;
    uint8_t epoch_active;
    uint8_t app_limited_paused;
    uint8_t after_timeout;
    uint8_t fast_convergence;
    uint8_t hystart_enabled;
    uint8_t hystart_found;
    uint8_t hystart_ack_train_found;
    uint8_t hystart_delay_found;
};

int tcp_shift_cubic_model_init(struct tcp_shift_cubic_model *model,
                               const struct tcp_shift_cc_transport *transport,
                               const struct tcp_shift_cc_init *init,
                               struct tcp_shift_cc_policy *policy);

int tcp_shift_cubic_model_on_ack(struct tcp_shift_cubic_model *model,
                                 const struct tcp_shift_cc_transport *transport,
                                 const struct tcp_shift_cc_ack *ack,
                                 uint64_t now_ns,
                                 uint64_t smoothed_rtt_ns,
                                 struct tcp_shift_cc_policy *policy);

int tcp_shift_cubic_model_on_loss(struct tcp_shift_cubic_model *model,
                                  const struct tcp_shift_cc_transport *transport,
                                  const struct tcp_shift_cc_loss *loss,
                                  struct tcp_shift_cc_policy *policy);

int tcp_shift_cubic_model_on_timeout(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy);

void tcp_shift_cubic_model_set_fast_convergence(
    struct tcp_shift_cubic_model *model,
    unsigned enabled);

/* Linux-compatible classic HyStart slow-start exit detector. Raw RTT samples
 * and cumulative delivery snapshots come from the generic ACK observation.
 * A return value of 1 means this ACK exited slow start, 0 means no exit, and
 * -1 reports invalid input. */
void tcp_shift_cubic_hystart_reset(struct tcp_shift_cubic_model *model);
int tcp_shift_cubic_hystart_on_ack(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint64_t now_ns);

/* Generic controller wrapper. It consumes only the transport-neutral ACK
 * observations from cc.h; clock acquisition and RTT sampling remain adapter
 * responsibilities. */
extern const struct tcp_shift_cc_ops tcp_shift_cubic_ops;

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_CUBIC_H */
