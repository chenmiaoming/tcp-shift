#ifndef TCP_SHIFT_CC_BBR_CONTROLLER_H
#define TCP_SHIFT_CC_BBR_CONTROLLER_H

#include <stdint.h>

#include "cc/bbr.h"
#include "cc/bbr_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal, ACK-driven compact BBR lifecycle state.
 *
 * This deliberately is not exposed as tcp_shift_cc_ops yet. The generic
 * controller ABI requires qualified loss and timeout semantics; until those
 * are implemented, keeping this as an explicit internal lifecycle prevents a
 * partially specified BBR from entering the public controller registry.
 */
struct tcp_shift_bbr_controller_state {
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_state probe;
    uint64_t pacing_rate_bytes_per_sec;
    uint32_t initial_cwnd_bytes;
    uint32_t cwnd_bytes;
    uint8_t initialized;
};

int tcp_shift_bbr_controller_init(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    uint32_t cycle_seed,
    struct tcp_shift_cc_policy *policy);

/* Consume one ACK observation and publish the policy for the final mode after
 * estimator updates and any STARTUP/DRAIN/ProbeBW/ProbeRTT transitions.
 * ack_time_ns must be a nonzero monotonic timestamp. */
int tcp_shift_bbr_controller_on_ack(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_BBR_CONTROLLER_H */
