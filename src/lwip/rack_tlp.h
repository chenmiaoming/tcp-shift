#ifndef TCP_SHIFT_LWIP_RACK_TLP_H
#define TCP_SHIFT_LWIP_RACK_TLP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SHIFT_RACK_REO_WND_DIVISOR 4U
#define TCP_SHIFT_RACK_REO_WND_PERSIST_RECOVERIES 16U
#define TCP_SHIFT_RACK_DEFAULT_PTO_NS UINT64_C(1000000000)

enum tcp_shift_tlp_ack_result {
    TCP_SHIFT_TLP_ACK_NONE = 0,
    TCP_SHIFT_TLP_ACK_CLEARED = 1,
    TCP_SHIFT_TLP_ACK_LOSS_REPAIRED = 2
};

/* RFC 8985 transport state.
 *
 * Segment.xmit_ts / end_seq / retransmitted remain per-segment metadata owned
 * by the transport sidecar. This object contains only per-connection RACK/TLP
 * state so it can remain independent of Reno/CUBIC/BBR policy.
 */
struct tcp_shift_rack_tlp_state {
    uint64_t rack_xmit_ts_ns;
    uint64_t rack_ack_ts_ns;
    uint64_t rack_rtt_ns;
    uint64_t min_rtt_ns;
    uint64_t srtt_ns;
    uint64_t reo_wnd_ns;

    uint32_t rack_end_seq;
    uint32_t fack;
    uint32_t dsack_round_end_seq;
    uint32_t tlp_end_seq;
    uint32_t segs_sacked;
    uint32_t reo_wnd_mult;
    uint32_t reo_wnd_persist;

    uint8_t reordering_seen;
    uint8_t dsack_round_active;
    uint8_t tlp_is_retrans;
    uint8_t rtt_sample_since_probe;
};

struct tcp_shift_rack_segment {
    uint64_t xmit_ts_ns;
    uint32_t end_seq;
    uint8_t retransmitted;
    uint8_t lost;
};

void tcp_shift_rack_tlp_init(struct tcp_shift_rack_tlp_state *state);

int tcp_shift_rack_sent_after(uint64_t xmit_ts_a_ns,
                              uint32_t end_seq_a,
                              uint64_t xmit_ts_b_ns,
                              uint32_t end_seq_b);

void tcp_shift_rack_set_rtt_estimates(struct tcp_shift_rack_tlp_state *state,
                                      uint64_t min_rtt_ns,
                                      uint64_t srtt_ns);

void tcp_shift_rack_note_sacked_segments(struct tcp_shift_rack_tlp_state *state,
                                         uint32_t segs_sacked);

int tcp_shift_rack_note_delivered(struct tcp_shift_rack_tlp_state *state,
                                  const struct tcp_shift_rack_segment *segment,
                                  uint64_t ack_time_ns);

void tcp_shift_rack_note_dsack(struct tcp_shift_rack_tlp_state *state,
                               uint32_t snd_nxt);

void tcp_shift_rack_note_recovery_exit(struct tcp_shift_rack_tlp_state *state,
                                       uint32_t snd_una);

uint64_t tcp_shift_rack_reo_wnd(struct tcp_shift_rack_tlp_state *state,
                                unsigned in_recovery);

int tcp_shift_rack_loss_remaining(
    struct tcp_shift_rack_tlp_state *state,
    const struct tcp_shift_rack_segment *segment,
    uint64_t now_ns,
    unsigned in_recovery,
    uint64_t *remaining_ns);

int tcp_shift_rack_lost_on_rto(
    struct tcp_shift_rack_tlp_state *state,
    const struct tcp_shift_rack_segment *segment,
    uint32_t snd_una,
    uint64_t now_ns,
    unsigned in_recovery);

uint64_t tcp_shift_tlp_calc_pto_ns(
    const struct tcp_shift_rack_tlp_state *state,
    uint64_t now_ns,
    uint64_t rto_expiration_ns,
    uint32_t flight_segments,
    uint64_t max_ack_delay_ns);

int tcp_shift_tlp_probe_allowed(const struct tcp_shift_rack_tlp_state *state);

void tcp_shift_tlp_note_probe_sent(struct tcp_shift_rack_tlp_state *state,
                                   uint32_t end_seq,
                                   unsigned is_retransmission);

void tcp_shift_tlp_note_rtt_sample(struct tcp_shift_rack_tlp_state *state);

enum tcp_shift_tlp_ack_result tcp_shift_tlp_process_ack(
    struct tcp_shift_rack_tlp_state *state,
    uint32_t ack_seq,
    unsigned dsack_matches_probe,
    unsigned dupack_without_sack);

void tcp_shift_tlp_reset(struct tcp_shift_rack_tlp_state *state);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SHIFT_LWIP_RACK_TLP_H */
