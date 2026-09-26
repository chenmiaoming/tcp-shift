#ifndef TCP_SHIFT_CC_BBR_RECOVERY_H
#define TCP_SHIFT_CC_BBR_RECOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Qualification A/B: provision extra sender headroom while diagnosing
 * long-RTT SACK recovery. Transport memory policy still owns allocation,
 * wmem.max and pressure limits; only the internal BBR hint changes here. */
#define TCP_SHIFT_BBR_SNDBUF_EXPAND_NUM 6U
#define TCP_SHIFT_BBR_SNDBUF_EXPAND_DEN 1U

/* Transport-neutral BBRv1-style loss-recovery state. This deliberately does
 * not know about lwIP fast-recovery flags or Linux CA states. The adapter will
 * eventually translate transport recovery entry/round/exit observations into
 * these operations. */
struct tcp_shift_bbr_recovery_state {
    uint32_t prior_cwnd_bytes;
    uint8_t in_recovery;
    uint8_t packet_conservation;
};

void tcp_shift_bbr_recovery_init(
    struct tcp_shift_bbr_recovery_state *state);

/* Enter the first packet-timed round of Recovery. Linux BBR cuts unused cwnd
 * and starts packet conservation at inflight + newly ACKed data. */
int tcp_shift_bbr_recovery_enter(
    struct tcp_shift_bbr_recovery_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t inflight_bytes,
    uint32_t acked_bytes,
    uint32_t lost_bytes,
    uint32_t mss_bytes,
    uint32_t cwnd_limit_bytes,
    uint32_t *cwnd_bytes);

/* Apply recovery loss accounting and first-round packet conservation.
 * round_start is the controller model's packet-timed round boundary. When it
 * becomes nonzero the first recovery round is complete and normal BBR cwnd
 * policy may resume. owns_cwnd is nonzero only while conservation owns cwnd. */
int tcp_shift_bbr_recovery_on_ack(
    struct tcp_shift_bbr_recovery_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t inflight_bytes,
    uint32_t acked_bytes,
    uint32_t lost_bytes,
    uint32_t mss_bytes,
    uint32_t cwnd_limit_bytes,
    unsigned round_start,
    uint32_t *cwnd_bytes,
    unsigned *owns_cwnd);

/* Leaving recovery restores the last known good cwnd. The caller then applies
 * the current mode's BDP target/caps, matching Linux BBR's ordering. */
int tcp_shift_bbr_recovery_exit(
    struct tcp_shift_bbr_recovery_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t cwnd_limit_bytes,
    uint32_t *cwnd_bytes);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_CC_BBR_RECOVERY_H */
