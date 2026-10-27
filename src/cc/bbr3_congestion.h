#ifndef TCP_SHIFT_CC_BBR3_CONGESTION_H
#define TCP_SHIFT_CC_BBR3_CONGESTION_H

#include <stdint.h>
#include "cc/bbr3_model.h"

/*
 * Per-flow, independent BBRv3 congestion model.
 *
 * Loss and CE observations MUST originate from the public TCP's validated
 * SACK/RACK loss ledger and negotiated ECN counter, respectively. The model
 * does not read a Linux backend socket's TCP_INFO/iperf3 retransmit field.
 * UINT64_MAX means that a given bound has not been measured yet.
 *
 * This is not a production BBRv3 controller; its input API does not yet bind
 * to lwIP, and no cc="bbr3" selector is exposed.
 */
#define TCP_SHIFT_BBR3_UNBOUNDED UINT64_MAX
#define TCP_SHIFT_BBR3_LOSS_BETA_NUM 7U
#define TCP_SHIFT_BBR3_LOSS_BETA_DEN 10U
#define TCP_SHIFT_BBR3_EXCESS_LOSS_PERCENT 2U
#define TCP_SHIFT_BBR3_ECN_ALPHA_SCALE 256U
#define TCP_SHIFT_BBR3_ECN_ALPHA_GAIN_DEN 16U
#define TCP_SHIFT_BBR3_ECN_FACTOR_Q8 85U
#define TCP_SHIFT_BBR3_ECN_EXCESS_PERCENT 50U

enum tcp_shift_bbr3_congestion_signal {
    TCP_SHIFT_BBR3_SIGNAL_NONE = 0,
    TCP_SHIFT_BBR3_SIGNAL_EXCESS_LOSS = 1,
    TCP_SHIFT_BBR3_SIGNAL_EXCESS_ECN = 2
};

struct tcp_shift_bbr3_congestion {
    uint64_t bw_shortterm_bytes_per_sec;
    uint64_t inflight_shortterm_bytes;
    uint64_t inflight_longterm_bytes;
    uint64_t bw_latest_bytes_per_sec;
    uint64_t inflight_latest_bytes;
    uint64_t ecn_round_delivered_bytes;
    uint64_t ecn_round_ce_bytes;
    uint64_t probe_up_acked_bytes;
    uint64_t probe_up_acked_per_inc_bytes;
    uint64_t undo_bw_shortterm;
    uint64_t undo_inflight_shortterm;
    uint64_t undo_inflight_longterm;
    uint32_t ecn_alpha_q8;
    uint32_t probe_up_rounds;
    uint8_t loss_in_round;
    uint8_t ecn_in_round;
    uint8_t probe_feedback_consumed;
    uint8_t undo_valid;
};

struct tcp_shift_bbr3_congestion_ack {
    /* A single ACK observation: bytes, not packet counts. */
    uint64_t delivery_rate_bytes_per_sec;
    uint64_t newly_delivered_bytes;
    uint64_t newly_lost_bytes;
    uint64_t newly_ce_bytes;
    uint64_t tx_inflight_bytes; /* transmit-time, NOT current outstanding */
    uint64_t cwnd_bytes;
    uint64_t target_inflight_bytes; /* BBRv3 target for a qualified probe */
    uint64_t newly_acked_bytes;
    uint32_t mss_bytes;
    uint8_t rate_valid;
    uint8_t app_limited;
    uint8_t ecn_eligible; /* negotiated and supported by the transport */
    uint8_t probing_bandwidth; /* STARTUP, ProbeBW REFILL, or UP */
    uint8_t probe_sample; /* originated from actual probing transmissions */
    uint8_t probe_up; /* specifically ProbeBW_UP */
    uint8_t cwnd_limited;
    uint8_t loss_round_end; /* packet-timed round, from delivery ledger */
    uint8_t round_start; /* packet-timed ACK round for probe-up slope */
};

/* Initialize/reset independently from the older compact BBR. */
void tcp_shift_bbr3_congestion_init(struct tcp_shift_bbr3_congestion *state);
void tcp_shift_bbr3_congestion_reset_shortterm(
    struct tcp_shift_bbr3_congestion *state);
void tcp_shift_bbr3_congestion_begin_probe(
    struct tcp_shift_bbr3_congestion *state);

/* Explicit recovery snapshot/undo for validated spurious-loss detection. */
int tcp_shift_bbr3_congestion_save_recovery(
    struct tcp_shift_bbr3_congestion *state);
int tcp_shift_bbr3_congestion_undo_spurious(
    struct tcp_shift_bbr3_congestion *state);

/* Returns a signal bitmask (>=0), or -1 for invalid input. Updates the model
 * once per completed loss round, never once per ACK for loss cuts. */
int tcp_shift_bbr3_congestion_on_ack(
    struct tcp_shift_bbr3_congestion *state,
    const struct tcp_shift_bbr3_congestion_ack *ack);

/* Only bounded phases use the short-term rate cap. In STARTUP and REFILL/UP,
 * no bw_shortterm cap is applied. */
uint64_t tcp_shift_bbr3_congestion_rate_cap(
    const struct tcp_shift_bbr3_congestion *state,
    enum tcp_shift_bbr3_phase phase,
    uint64_t max_bw_bytes_per_sec);

/* Cwnd upper bound before cwnd growth / recovery conservation is applied;
 * at least the four-MSS pipe floor. */
uint64_t tcp_shift_bbr3_congestion_cwnd_cap(
    const struct tcp_shift_bbr3_congestion *state,
    enum tcp_shift_bbr3_phase phase,
    uint32_t mss_bytes);

#endif /* TCP_SHIFT_CC_BBR3_CONGESTION_H */
