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
 * controller ABI requires qualified recovery ownership and an lwIP mapping for
 * post-loss in-flight state; until those are implemented, keeping this as an
 * explicit internal lifecycle prevents a partially specified BBR from entering
 * the public registry.
 */
struct tcp_shift_bbr_controller_state {
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_state probe;
    struct tcp_shift_bbr_recovery_state recovery;
    uint64_t pacing_rate_bytes_per_sec;
    uint64_t delivered_bytes;
    uint32_t initial_cwnd_bytes;
    uint32_t cwnd_bytes;
    /* Newly inferred loss inside an already-open transport recovery episode.
     * The transport reports this immediately before the ACK that exposed the
     * next hole. Probe-loss and recovery-cwnd loss are separate because the
     * initial fast-loss entry already uses post-loss inflight and must not be
     * subtracted a second time on the next ACK. */
    uint32_t pending_probe_loss_bytes;
    uint32_t pending_recovery_loss_bytes;
    uint8_t initialized;
};

/* RTO/Loss-state cwnd input after the transport has marked timeout losses.
 * This is deliberately distinct from tcp_shift_cc_transport.inflight_bytes:
 * the latter is raw outstanding sequence space on lwIP, while Linux
 * tcp_enter_loss() uses tcp_packets_in_flight() after timeout loss marking. */
struct tcp_shift_bbr_timeout_observation {
    uint32_t post_loss_inflight_bytes;
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

/* Record a new hole discovered by a partial ACK while the same transport
 * Recovery episode remains open. The bytes are consumed by the immediately
 * following ACK policy update so packet-conservation loss accounting and
 * ProbeBW phase decisions see the same newly-lost signal. */
int tcp_shift_bbr_controller_recovery_loss(
    struct tcp_shift_bbr_controller_state *state,
    uint32_t lost_bytes);

/* Restore the last known-good cwnd when the transport reports Recovery exit.
 * A following ACK observation then applies the current mode's normal BDP
 * target/caps, preserving Linux BBR's restore-before-normal-policy ordering. */
int tcp_shift_bbr_controller_recovery_exit(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy);

/* Apply the controller-side part of Linux BBRv1's TCP_CA_Loss transition.
 * The caller must supply post-loss in-flight bytes, not raw outstanding bytes.
 * Mode, max-bw/min-RTT filters, full_bw_count/full_bw_reached and pacing are
 * preserved; the full_bw baseline is reset and the timeout is treated as a
 * round boundary. cwnd becomes post-loss inflight + one MSS, capped by the
 * transport limit. */
int tcp_shift_bbr_controller_on_timeout(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_bbr_timeout_observation *timeout,
    struct tcp_shift_cc_policy *policy);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_BBR_CONTROLLER_H */
