#include "lwip/bbr3_rack_evidence.h"

#include <stddef.h>

void tcp_shift_bbr3_rack_attempt_init(
    struct tcp_shift_bbr3_rack_attempt *a)
{
    if (a == NULL) {
        return;
    }
    a->generation = 0U;
    a->tx_ns = 0U;
    a->delivered_at_tx = 0U;
    a->qualified_generation = 0U;
    a->revoked_generation = 0U;
    a->seq_start = 0U;
    a->prior_inflight_bytes = 0U;
    a->payload_bytes = 0U;
    a->delivered_bytes = 0U;
    a->retransmitted = 0U;
    a->app_limited = 0U;
    a->probe_sample = 0U;
}

int tcp_shift_bbr3_rack_note_tx(
    struct tcp_shift_bbr3_rack_attempt *a,
    const struct tcp_shift_bbr3_rack_tx *tx)
{
    if (a == NULL || tx == NULL || tx->generation == 0U ||
        tx->generation <= a->generation || tx->tx_ns == 0U ||
        tx->payload_bytes == 0U ||
        tx->already_delivered_bytes >= tx->payload_bytes ||
        tx->prior_inflight_bytes < tx->payload_bytes ||
        (a->generation != 0U && tx->tx_ns < a->tx_ns)) {
        return -1;
    }
    a->generation = tx->generation;
    a->tx_ns = tx->tx_ns;
    a->delivered_at_tx = tx->delivered_at_tx;
    a->seq_start = tx->seq_start;
    a->prior_inflight_bytes = tx->prior_inflight_bytes;
    a->payload_bytes = tx->payload_bytes;
    a->delivered_bytes = tx->already_delivered_bytes;
    a->retransmitted = tx->retransmitted != 0U ? 1U : 0U;
    a->app_limited = tx->app_limited != 0U ? 1U : 0U;
    a->probe_sample = tx->probe_sample != 0U ? 1U : 0U;
    return 0;
}

int tcp_shift_bbr3_rack_note_delivery(
    struct tcp_shift_bbr3_rack_attempt *a,
    uint16_t total_delivered_bytes)
{
    if (a == NULL || a->generation == 0U ||
        total_delivered_bytes < a->delivered_bytes ||
        total_delivered_bytes > a->payload_bytes) {
        return -1;
    }
    a->delivered_bytes = total_delivered_bytes;
    return 0;
}

int tcp_shift_bbr3_rack_qualify_loss(
    struct tcp_shift_bbr3_rack_attempt *a,
    struct tcp_shift_rack_tlp_state *rack,
    uint64_t now_ns,
    unsigned in_recovery,
    struct tcp_shift_bbr3_rack_loss *out)
{
    struct tcp_shift_rack_segment segment;
    uint64_t remaining_ns = 0U;

    if (a == NULL || rack == NULL || out == NULL || now_ns == 0U ||
        a->generation == 0U || now_ns < a->tx_ns ||
        in_recovery > 1U) {
        return -1;
    }
    if (a->qualified_generation == a->generation ||
        a->delivered_bytes >= a->payload_bytes) {
        return 0;
    }
    segment.xmit_ts_ns = a->tx_ns;
    segment.seq_start = a->seq_start;
    segment.end_seq = a->seq_start + a->payload_bytes;
    segment.retransmitted = a->retransmitted;
    segment.lost = 0U;

    if (!tcp_shift_rack_loss_remaining(
            rack, &segment, now_ns, in_recovery, &remaining_ns)) {
        return 0;
    }

    /* Persist the classification per *transmission* before returning it.
     * A repeated RACK timer, TLP callback, or SACK reordering pass may see
     * the same segment again but cannot double-consume this evidence. */
    a->qualified_generation = a->generation;
    out->generation = a->generation;
    out->tx_ns = a->tx_ns;
    out->delivered_at_tx = a->delivered_at_tx;
    out->seq_start = a->seq_start;
    out->tx_inflight_bytes = a->prior_inflight_bytes;
    out->newly_lost_bytes =
        (uint16_t)(a->payload_bytes - a->delivered_bytes);
    out->retransmitted = a->retransmitted;
    out->app_limited = a->app_limited;
    out->probe_sample = a->probe_sample;
    return 1;
}

int tcp_shift_bbr3_rack_undo_spurious(
    struct tcp_shift_bbr3_rack_attempt *a,
    uint64_t generation)
{
    if (a == NULL || generation == 0U) {
        return -1;
    }
    if (generation != a->qualified_generation ||
        generation == a->revoked_generation) {
        return 0;
    }
    a->revoked_generation = generation;
    return 1;
}
