#include "lwip/bbr3_loss_round.h"

#include <stddef.h>

int tcp_shift_bbr3_loss_round_start(
    struct tcp_shift_bbr3_loss_round *round,
    uint64_t delivered_start)
{
    if (round == NULL || round->active != 0U) {
        return -1;
    }
    round->delivered_start = delivered_start;
    round->newly_lost_bytes = 0U;
    round->snapshot_delivered_at_tx = 0U;
    round->last_generation = 0U;
    round->last_seq_start = 0U;
    round->snapshot_tx_inflight_bytes = 0U;
    round->event_count = 0U;
    round->context_consistent = 1U;
    round->all_probe_samples = 1U;
    round->active = 1U;
    return 0;
}

int tcp_shift_bbr3_loss_round_add(
    struct tcp_shift_bbr3_loss_round *round,
    const struct tcp_shift_bbr3_rack_loss *event)
{
    if (round == NULL || round->active == 0U || event == NULL ||
        event->generation == 0U || event->newly_lost_bytes == 0U ||
        event->tx_ns == 0U ||
        event->tx_inflight_bytes < event->newly_lost_bytes ||
        (event->generation == round->last_generation &&
         event->seq_start == round->last_seq_start) ||
        round->event_count == UINT32_MAX ||
        UINT64_MAX - round->newly_lost_bytes < event->newly_lost_bytes) {
        return -1;
    }
    if (round->event_count == 0U) {
        round->snapshot_delivered_at_tx = event->delivered_at_tx;
        round->snapshot_tx_inflight_bytes = event->tx_inflight_bytes;
    } else if (round->snapshot_delivered_at_tx != event->delivered_at_tx ||
               round->snapshot_tx_inflight_bytes !=
                   event->tx_inflight_bytes) {
        round->context_consistent = 0U;
    }
    if (event->probe_sample == 0U) {
        round->all_probe_samples = 0U;
    }
    round->last_generation = event->generation;
    round->last_seq_start = event->seq_start;
    round->event_count++;
    round->newly_lost_bytes += event->newly_lost_bytes;
    return 0;
}

int tcp_shift_bbr3_loss_round_finish(
    struct tcp_shift_bbr3_loss_round *round,
    uint64_t delivered_end,
    unsigned packet_timed_round_complete,
    struct tcp_shift_bbr3_loss_round_result *result)
{
    uint8_t ratio_valid;

    if (round == NULL || result == NULL || round->active == 0U ||
        packet_timed_round_complete != 1U ||
        delivered_end <= round->delivered_start) {
        return -1;
    }
    ratio_valid = round->event_count != 0U &&
                  round->context_consistent != 0U &&
                  round->snapshot_tx_inflight_bytes != 0U;
    result->delivered_start = round->delivered_start;
    result->delivered_end = delivered_end;
    result->newly_lost_bytes = round->newly_lost_bytes;
    result->events = round->event_count;
    result->tx_inflight_bytes =
        ratio_valid != 0U ? round->snapshot_tx_inflight_bytes : 0U;
    result->same_send_snapshot =
        round->event_count != 0U && round->context_consistent != 0U;
    result->loss_fraction_valid = ratio_valid;
    result->qualified_probe_excess_loss =
        ratio_valid != 0U && round->all_probe_samples != 0U &&
        (round->newly_lost_bytes > UINT32_MAX ||
         round->newly_lost_bytes * UINT64_C(100) >
             (uint64_t)round->snapshot_tx_inflight_bytes * 2U)
            ? 1U : 0U;
    round->active = 0U;
    return 0;
}
