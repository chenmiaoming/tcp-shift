#ifndef TCP_SHIFT_LWIP_BBR3_LOSS_ROUND_H
#define TCP_SHIFT_LWIP_BBR3_LOSS_ROUND_H

#include <stdint.h>
#include "lwip/bbr3_rack_evidence.h"

/*
 * A compact BBRv3 loss-round accounting layer: no second packet ledger.
 * Inputs are the exact-once RACK-qualified events emitted by a P5 slot.
 * The caller alone decides when a packet-timed loss round is complete.
 *
 * A ratio can be constructed only when every event shares the same original
 * delivered-at-TX and TX-inflight snapshot; events with mismatched contexts
 * are retained for counters but never combined into an invented denominator.
 * ProbeBW congestion response additionally needs *all* loss events to carry
 * validated transmit-time probe provenance. The current P5 observer cannot
 * yet supply that provenance, so live traffic must not consume this model.
 */
struct tcp_shift_bbr3_loss_round {
    uint64_t delivered_start;
    uint64_t newly_lost_bytes;
    uint64_t snapshot_delivered_at_tx;
    uint64_t last_generation;
    uint32_t last_seq_start;
    uint32_t snapshot_tx_inflight_bytes;
    uint32_t event_count;
    uint8_t active;
    uint8_t context_consistent;
    uint8_t all_probe_samples;
};

struct tcp_shift_bbr3_loss_round_result {
    uint64_t delivered_start;
    uint64_t delivered_end;
    uint64_t newly_lost_bytes;
    uint32_t events;
    uint32_t tx_inflight_bytes;
    uint8_t same_send_snapshot;
    uint8_t loss_fraction_valid;
    uint8_t qualified_probe_excess_loss; /* > 2% of compatible flight */
};

int tcp_shift_bbr3_loss_round_start(
    struct tcp_shift_bbr3_loss_round *round,
    uint64_t delivered_start);

int tcp_shift_bbr3_loss_round_add(
    struct tcp_shift_bbr3_loss_round *round,
    const struct tcp_shift_bbr3_rack_loss *event);

int tcp_shift_bbr3_loss_round_finish(
    struct tcp_shift_bbr3_loss_round *round,
    uint64_t delivered_end,
    unsigned packet_timed_round_complete,
    struct tcp_shift_bbr3_loss_round_result *result);

#endif
