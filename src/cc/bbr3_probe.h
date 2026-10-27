#ifndef TCP_SHIFT_CC_BBR3_PROBE_H
#define TCP_SHIFT_CC_BBR3_PROBE_H

#include <stdint.h>

#include "cc/bbr3_model.h"

/*
 * Pure-C BBRv3 ProbeBW phase scheduler.
 *
 * Scope: phase transitions, clock/packet-round waits, bandwidth-plateau exit,
 * headroom gating, and feedback-gated max-bw filter aging. No PCB, pacing
 * timer, ACK sampler, or loss/ECN detector is owned here. This is not yet a
 * complete public BBRv3 controller.
 */
#define TCP_SHIFT_BBR3_PROBE_WAIT_BASE_NS UINT64_C(2000000000)
#define TCP_SHIFT_BBR3_PROBE_WAIT_JITTER_NS UINT32_C(1000000000)
#define TCP_SHIFT_BBR3_RENO_MAX_ROUNDS 63U

enum tcp_shift_bbr3_ack_phase {
    TCP_SHIFT_BBR3_ACKS_INIT = 0,
    TCP_SHIFT_BBR3_ACKS_REFILLING,
    TCP_SHIFT_BBR3_ACKS_PROBE_STARTING,
    TCP_SHIFT_BBR3_ACKS_PROBE_FEEDBACK,
    TCP_SHIFT_BBR3_ACKS_PROBE_STOPPING
};

struct tcp_shift_bbr3_probe {
    uint64_t cycle_stamp_ns;
    uint64_t next_round_delivered;
    uint64_t up_full_bw_bytes_per_sec;
    uint64_t wait_ns;
    uint32_t random_state;
    uint32_t rounds_since_probe;
    uint32_t up_stagnant_rounds;
    uint32_t probe_up_rounds;
    enum tcp_shift_bbr3_ack_phase ack_phase;
    uint8_t initialized;
    uint8_t previous_probe_too_high;
    uint8_t previous_probe_precautionary;
    uint8_t probe_bw_sample;
};

/* Input is from the *current* ACK. The transport is responsible for correct
 * RACK/SACK snapshots, monotonic time, congestion accounting and the optional
 * long-term inflight bound. qualified_loss_too_high must be based on a real
 * validated congestion signal, not merely a retransmission counter.
 */
struct tcp_shift_bbr3_probe_ack {
    const struct tcp_shift_cc_rate_sample *rate;
    uint64_t now_ns;
    uint64_t inflight_longterm_bytes; /* UINT64_MAX means unset */
    uint32_t inflight_bytes;
    uint32_t cwnd_bytes;
    uint32_t mss_bytes;
    uint8_t cwnd_limited;
    uint8_t qualified_loss_too_high;
};

/* Enter DOWN only when Startup has established a full pipe and has a valid
 * bandwidth/min-RTT model. A nonzero flow seed makes wait jitter deterministic
 * in contracts and flow-specific in production. */
int tcp_shift_bbr3_probe_enter(struct tcp_shift_bbr3_probe *probe,
                               struct tcp_shift_bbr3_model *model,
                               uint64_t now_ns,
                               uint64_t delivered_total_bytes,
                               uint32_t flow_seed);

/* Call after bbr3_model_on_ack with the same ACK snapshot. */
int tcp_shift_bbr3_probe_on_ack(struct tcp_shift_bbr3_probe *probe,
                                struct tcp_shift_bbr3_model *model,
                                const struct tcp_shift_bbr3_probe_ack *ack);

#endif
