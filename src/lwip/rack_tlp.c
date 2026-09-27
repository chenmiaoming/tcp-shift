#include "lwip/rack_tlp.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static uint64_t tcp_shift_rack_add_sat_u64(uint64_t left, uint64_t right)
{
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

static uint64_t tcp_shift_rack_mul_sat_u64(uint64_t left, uint64_t right)
{
    if (left == 0U || right == 0U) {
        return 0U;
    }
    return left > UINT64_MAX / right ? UINT64_MAX : left * right;
}

static int tcp_shift_rack_seq_after(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

static int tcp_shift_rack_seq_after_eq(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) >= 0;
}

void tcp_shift_rack_tlp_init(struct tcp_shift_rack_tlp_state *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->reo_wnd_mult = 1U;
}

int tcp_shift_rack_sent_after(uint64_t xmit_ts_a_ns,
                              uint32_t end_seq_a,
                              uint64_t xmit_ts_b_ns,
                              uint32_t end_seq_b)
{
    if (xmit_ts_a_ns > xmit_ts_b_ns) {
        return 1;
    }
    if (xmit_ts_a_ns < xmit_ts_b_ns) {
        return 0;
    }
    return tcp_shift_rack_seq_after(end_seq_a, end_seq_b);
}

void tcp_shift_rack_set_rtt_estimates(struct tcp_shift_rack_tlp_state *state,
                                      uint64_t min_rtt_ns,
                                      uint64_t srtt_ns)
{
    if (state == NULL) {
        return;
    }
    state->min_rtt_ns = min_rtt_ns;
    state->srtt_ns = srtt_ns;
}

void tcp_shift_rack_note_sacked_segments(struct tcp_shift_rack_tlp_state *state,
                                         uint32_t segs_sacked)
{
    if (state != NULL) {
        state->segs_sacked = segs_sacked;
    }
}

int tcp_shift_rack_note_delivered(struct tcp_shift_rack_tlp_state *state,
                                  const struct tcp_shift_rack_segment *segment,
                                  uint64_t ack_time_ns)
{
    uint64_t rtt_ns;

    if (state == NULL || segment == NULL || segment->xmit_ts_ns == 0U ||
        ack_time_ns < segment->xmit_ts_ns) {
        return -1;
    }

    rtt_ns = ack_time_ns - segment->xmit_ts_ns;

    /* RFC 8985 section 6.2 step 2: without a usable timestamp echo, a
     * retransmitted segment whose apparent RTT is below min_RTT is ambiguous
     * and must not advance RACK.segment. */
    if (segment->retransmitted != 0U && state->min_rtt_ns != 0U &&
        rtt_ns < state->min_rtt_ns) {
        return 0;
    }

    state->rack_rtt_ns = rtt_ns;
    state->rack_ack_ts_ns = ack_time_ns;

    if (tcp_shift_rack_sent_after(segment->xmit_ts_ns, segment->end_seq,
                                  state->rack_xmit_ts_ns,
                                  state->rack_end_seq)) {
        state->rack_xmit_ts_ns = segment->xmit_ts_ns;
        state->rack_end_seq = segment->end_seq;
    }

    return 1;
}

void tcp_shift_rack_detect_reordering(
    struct tcp_shift_rack_tlp_state *state,
    const struct tcp_shift_rack_segment *segment)
{
    if (state == NULL || segment == NULL) {
        return;
    }

    /* RFC 8985 section 6.2 step 3 is a distinct pass in ascending end_seq
     * order. Keeping it separate from the xmit_ts-ordered RACK.segment update
     * avoids treating multiple segments delivered by one ACK as reordering. */
    if (state->fack == 0U || tcp_shift_rack_seq_after(segment->end_seq,
                                                       state->fack)) {
        state->fack = segment->end_seq;
    } else if (segment->end_seq != state->fack &&
               segment->retransmitted == 0U) {
        state->reordering_seen = 1U;
    }
}

void tcp_shift_rack_note_dsack(struct tcp_shift_rack_tlp_state *state,
                               uint32_t snd_nxt)
{
    if (state == NULL || state->dsack_round_active != 0U) {
        return;
    }

    state->dsack_round_active = 1U;
    state->dsack_round_end_seq = snd_nxt;
    if (state->reo_wnd_mult != UINT32_MAX) {
        state->reo_wnd_mult++;
    }
    state->reo_wnd_persist = TCP_SHIFT_RACK_REO_WND_PERSIST_RECOVERIES;
}

void tcp_shift_rack_note_recovery_exit(struct tcp_shift_rack_tlp_state *state,
                                       uint32_t snd_una)
{
    if (state == NULL) {
        return;
    }

    if (state->dsack_round_active != 0U &&
        tcp_shift_rack_seq_after_eq(snd_una, state->dsack_round_end_seq)) {
        state->dsack_round_active = 0U;
        state->dsack_round_end_seq = 0U;
    }

    if (state->reo_wnd_persist != 0U) {
        state->reo_wnd_persist--;
        if (state->reo_wnd_persist == 0U) {
            state->reo_wnd_mult = 1U;
        }
    }
}

uint64_t tcp_shift_rack_reo_wnd(struct tcp_shift_rack_tlp_state *state,
                                unsigned in_recovery)
{
    uint64_t base;
    uint64_t scaled;

    if (state == NULL || state->min_rtt_ns == 0U) {
        return 0U;
    }

    if (state->reordering_seen == 0U &&
        (in_recovery != 0U || state->segs_sacked >= 3U)) {
        state->reo_wnd_ns = 0U;
        return 0U;
    }

    base = state->min_rtt_ns / TCP_SHIFT_RACK_REO_WND_DIVISOR;
    if (base == 0U) {
        base = 1U;
    }

    scaled = tcp_shift_rack_mul_sat_u64(base, state->reo_wnd_mult);
    if (state->srtt_ns != 0U && scaled > state->srtt_ns) {
        scaled = state->srtt_ns;
    }
    state->reo_wnd_ns = scaled;
    return scaled;
}

int tcp_shift_rack_loss_remaining(
    struct tcp_shift_rack_tlp_state *state,
    const struct tcp_shift_rack_segment *segment,
    uint64_t now_ns,
    unsigned in_recovery,
    uint64_t *remaining_ns)
{
    uint64_t deadline;
    uint64_t reo_wnd;

    if (remaining_ns != NULL) {
        *remaining_ns = 0U;
    }
    if (state == NULL || segment == NULL || segment->xmit_ts_ns == 0U ||
        state->rack_xmit_ts_ns == 0U || state->rack_rtt_ns == 0U ||
        segment->lost != 0U) {
        return 0;
    }
    if (!tcp_shift_rack_sent_after(state->rack_xmit_ts_ns,
                                   state->rack_end_seq,
                                   segment->xmit_ts_ns,
                                   segment->end_seq)) {
        return 0;
    }

    reo_wnd = tcp_shift_rack_reo_wnd(state, in_recovery);
    deadline = tcp_shift_rack_add_sat_u64(segment->xmit_ts_ns,
                                           state->rack_rtt_ns);
    deadline = tcp_shift_rack_add_sat_u64(deadline, reo_wnd);

    if (now_ns >= deadline) {
        return 1;
    }
    if (remaining_ns != NULL) {
        *remaining_ns = deadline - now_ns;
    }
    return 0;
}

int tcp_shift_rack_lost_on_rto(
    struct tcp_shift_rack_tlp_state *state,
    const struct tcp_shift_rack_segment *segment,
    uint32_t snd_una,
    uint64_t now_ns,
    unsigned in_recovery)
{
    uint64_t remaining = 0U;

    if (segment == NULL) {
        return 0;
    }

    /* RFC 8985 section 6.3: SND.UNA is always marked lost when the RTO
     * expires. Other segments still require their RACK RTT + reo_wnd
     * deadline to have elapsed. */
    if (segment->seq_start == snd_una) {
        return 1;
    }

    return tcp_shift_rack_loss_remaining(
        state, segment, now_ns, in_recovery, &remaining);
}

uint64_t tcp_shift_tlp_calc_pto_ns(
    const struct tcp_shift_rack_tlp_state *state,
    uint64_t now_ns,
    uint64_t rto_expiration_ns,
    uint32_t flight_segments,
    uint64_t max_ack_delay_ns)
{
    uint64_t pto;
    uint64_t pto_expiration;

    if (state != NULL && state->srtt_ns != 0U) {
        pto = tcp_shift_rack_mul_sat_u64(state->srtt_ns, 2U);
        if (flight_segments == 1U) {
            pto = tcp_shift_rack_add_sat_u64(pto, max_ack_delay_ns);
        }
    } else {
        pto = TCP_SHIFT_RACK_DEFAULT_PTO_NS;
    }

    pto_expiration = tcp_shift_rack_add_sat_u64(now_ns, pto);
    if (rto_expiration_ns > now_ns && pto_expiration > rto_expiration_ns) {
        return rto_expiration_ns - now_ns;
    }
    return pto;
}

int tcp_shift_tlp_probe_allowed(const struct tcp_shift_rack_tlp_state *state)
{
    return state != NULL && state->tlp_end_seq == 0U &&
                   state->rtt_sample_since_probe != 0U
               ? 1
               : 0;
}

void tcp_shift_tlp_note_probe_sent(struct tcp_shift_rack_tlp_state *state,
                                   uint32_t end_seq,
                                   unsigned is_retransmission)
{
    if (state == NULL) {
        return;
    }
    state->tlp_end_seq = end_seq;
    state->tlp_is_retrans = is_retransmission != 0U ? 1U : 0U;
    state->rtt_sample_since_probe = 0U;
}

void tcp_shift_tlp_note_rtt_sample(struct tcp_shift_rack_tlp_state *state)
{
    if (state != NULL) {
        state->rtt_sample_since_probe = 1U;
    }
}

enum tcp_shift_tlp_ack_result tcp_shift_tlp_process_ack(
    struct tcp_shift_rack_tlp_state *state,
    uint32_t ack_seq,
    unsigned dsack_matches_probe,
    unsigned dupack_without_sack)
{
    if (state == NULL || state->tlp_end_seq == 0U ||
        !tcp_shift_rack_seq_after_eq(ack_seq, state->tlp_end_seq)) {
        return TCP_SHIFT_TLP_ACK_NONE;
    }

    if (state->tlp_is_retrans == 0U || dsack_matches_probe != 0U ||
        dupack_without_sack != 0U) {
        state->tlp_end_seq = 0U;
        state->tlp_is_retrans = 0U;
        return TCP_SHIFT_TLP_ACK_CLEARED;
    }

    if (tcp_shift_rack_seq_after(ack_seq, state->tlp_end_seq)) {
        state->tlp_end_seq = 0U;
        state->tlp_is_retrans = 0U;
        return TCP_SHIFT_TLP_ACK_LOSS_REPAIRED;
    }

    return TCP_SHIFT_TLP_ACK_NONE;
}

void tcp_shift_tlp_reset(struct tcp_shift_rack_tlp_state *state)
{
    if (state == NULL) {
        return;
    }
    state->tlp_end_seq = 0U;
    state->tlp_is_retrans = 0U;
}
