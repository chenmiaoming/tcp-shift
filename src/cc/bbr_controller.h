#ifndef TCP_SHIFT_CC_BBR_CONTROLLER_H
#define TCP_SHIFT_CC_BBR_CONTROLLER_H

#include <stdint.h>

#include "cc/bbr.h"
#include "cc/bbr_probe.h"
#include "cc/bbr_recovery.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal compact BBR lifecycle state.
 *
 * This deliberately is not exposed as tcp_shift_cc_ops yet. The generic
 * controller ABI requires qualified recovery ownership and timeout semantics;
 * until those are implemented, keeping this as an explicit internal lifecycle
 * prevents a partially specified BBR from entering the public registry.
 */
struct tcp_shift_bbr_controller_state {
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_state probe;
    struct tcp_shift_bbr_recovery_state recovery;
    uint64_t pacing_rate_bytes_per_sec;
    uint64_t delivered_bytes;
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
 * ack_time_ns must be a nonzero monotonic timestamp. While first-round packet
 * conservation owns cwnd, estimator/mode/pacing updates still run and only the
 * final cwnd is replaced by the recovery budget, matching Linux BBR ordering. */
int tcp_shift_bbr_controller_on_ack(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy);

/* Enter transport-observed loss recovery. The controller resets its
 * packet-timed round marker to the latest cumulative delivered snapshot before
 * enabling packet conservation, equivalent to Linux BBR assigning
 * next_rtt_delivered=delivered on Recovery entry. */
int tcp_shift_bbr_controller_recovery_enter(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    uint32_t lost_bytes,
    struct tcp_shift_cc_policy *policy);

/* Restore the last known-good cwnd when the transport reports Recovery exit.
 * A following ACK observation then applies the current mode's normal BDP
 * target/caps, preserving Linux BBR's restore-before-normal-policy ordering. */
int tcp_shift_bbr_controller_recovery_exit(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_BBR_CONTROLLER_H */
