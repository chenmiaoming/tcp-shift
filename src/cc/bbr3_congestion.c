#include "cc/bbr3_congestion.h"

#include <stddef.h>

static uint64_t min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static uint64_t max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

static uint64_t sat_add(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

/* Product is floor(value * numerator / denominator), with saturation and
 * without non-ISO __int128. All current caller numerators are <= 256. */
static uint64_t ratio(uint64_t value, uint32_t numerator, uint32_t denominator)
{
    uint64_t quotient;
    uint64_t remainder;

    if (denominator == 0U) {
        return UINT64_MAX;
    }
    quotient = value / denominator;
    remainder = value % denominator;
    if (numerator != 0U && quotient > UINT64_MAX / numerator) {
        return UINT64_MAX;
    }
    quotient *= numerator;
    return sat_add(quotient,
                   (remainder * numerator) / denominator);
}

static int greater_than_percent(uint64_t numerator,
                                uint64_t denominator,
                                uint32_t percent)
{
    if (denominator == 0U || numerator == 0U) {
        return 0;
    }
    return numerator > ratio(denominator, percent, 100U);
}

void tcp_shift_bbr3_congestion_init(struct tcp_shift_bbr3_congestion *s)
{
    if (s == NULL) {
        return;
    }
    s->bw_shortterm_bytes_per_sec = TCP_SHIFT_BBR3_UNBOUNDED;
    s->inflight_shortterm_bytes = TCP_SHIFT_BBR3_UNBOUNDED;
    s->inflight_longterm_bytes = TCP_SHIFT_BBR3_UNBOUNDED;
    s->bw_latest_bytes_per_sec = 0U;
    s->inflight_latest_bytes = 0U;
    s->ecn_round_delivered_bytes = 0U;
    s->ecn_round_ce_bytes = 0U;
    s->probe_up_acked_bytes = 0U;
    s->probe_up_acked_per_inc_bytes = 0U;
    s->undo_bw_shortterm = 0U;
    s->undo_inflight_shortterm = 0U;
    s->undo_inflight_longterm = 0U;
    s->ecn_alpha_q8 = TCP_SHIFT_BBR3_ECN_ALPHA_SCALE;
    s->probe_up_rounds = 0U;
    s->loss_in_round = 0U;
    s->ecn_in_round = 0U;
    s->probe_feedback_consumed = 0U;
    s->undo_valid = 0U;
}

void tcp_shift_bbr3_congestion_reset_shortterm(
    struct tcp_shift_bbr3_congestion *s)
{
    if (s == NULL) {
        return;
    }
    s->bw_shortterm_bytes_per_sec = TCP_SHIFT_BBR3_UNBOUNDED;
    s->inflight_shortterm_bytes = TCP_SHIFT_BBR3_UNBOUNDED;
}

void tcp_shift_bbr3_congestion_begin_probe(
    struct tcp_shift_bbr3_congestion *s)
{
    if (s == NULL) {
        return;
    }
    s->probe_feedback_consumed = 0U;
    s->probe_up_acked_bytes = 0U;
    s->probe_up_acked_per_inc_bytes = 0U;
    s->probe_up_rounds = 0U;
    tcp_shift_bbr3_congestion_reset_shortterm(s);
}

int tcp_shift_bbr3_congestion_save_recovery(
    struct tcp_shift_bbr3_congestion *s)
{
    if (s == NULL) {
        return -1;
    }
    s->undo_bw_shortterm = s->bw_shortterm_bytes_per_sec;
    s->undo_inflight_shortterm = s->inflight_shortterm_bytes;
    s->undo_inflight_longterm = s->inflight_longterm_bytes;
    s->undo_valid = 1U;
    return 0;
}

int tcp_shift_bbr3_congestion_undo_spurious(
    struct tcp_shift_bbr3_congestion *s)
{
    if (s == NULL || s->undo_valid == 0U) {
        return -1;
    }
    s->bw_shortterm_bytes_per_sec =
        max_u64(s->bw_shortterm_bytes_per_sec, s->undo_bw_shortterm);
    s->inflight_shortterm_bytes =
        max_u64(s->inflight_shortterm_bytes, s->undo_inflight_shortterm);
    s->inflight_longterm_bytes =
        max_u64(s->inflight_longterm_bytes, s->undo_inflight_longterm);
    s->loss_in_round = 0U;
    s->ecn_in_round = 0U;
    s->undo_valid = 0U;
    return 0;
}

/* 1/16 EWMA of the per-loss-round CE fraction, scaled in [0, 256].
 * The transport must supply the actual CE-marked *public* ACK bytes. */
static void finish_ecn_round(struct tcp_shift_bbr3_congestion *s)
{
    uint64_t measured;
    uint64_t blended;

    if (s->ecn_round_delivered_bytes == 0U) {
        return;
    }
    measured = ratio(s->ecn_round_ce_bytes,
                     TCP_SHIFT_BBR3_ECN_ALPHA_SCALE,
                     (uint32_t)min_u64(
                         s->ecn_round_delivered_bytes, UINT32_MAX));
    /* For delivery windows beyond UINT32_MAX, quotient decomposition using
     * the limited denominator above would overestimate the mark fraction.
     * In that case use a safe percentage decomposition first. */
    if (s->ecn_round_delivered_bytes > UINT32_MAX) {
        uint64_t coarse = s->ecn_round_delivered_bytes /
                          TCP_SHIFT_BBR3_ECN_ALPHA_SCALE;
        measured = coarse != 0U
                       ? s->ecn_round_ce_bytes / coarse
                       : 0U;
    }
    measured = min_u64(measured, TCP_SHIFT_BBR3_ECN_ALPHA_SCALE);
    blended = (uint64_t)s->ecn_alpha_q8 *
              (TCP_SHIFT_BBR3_ECN_ALPHA_GAIN_DEN - 1U) + measured;
    s->ecn_alpha_q8 =
        (uint32_t)(blended / TCP_SHIFT_BBR3_ECN_ALPHA_GAIN_DEN);
}

/* The short-term lower bounds react only at the end of a loss-timed round,
 * and only outside Startup/REFILL/UP bandwidth probing. The lower floor is
 * this round's demonstrated delivery, not merely MSS or a guessed BDP. */
static void finish_loss_round(struct tcp_shift_bbr3_congestion *s,
                              const struct tcp_shift_bbr3_congestion_ack *ack)
{
    uint64_t ecn_candidate;
    uint32_t ecn_cut_q8;

    finish_ecn_round(s);
    if (ack->probing_bandwidth == 0U) {
        if (s->loss_in_round != 0U) {
            if (s->bw_shortterm_bytes_per_sec == TCP_SHIFT_BBR3_UNBOUNDED) {
                s->bw_shortterm_bytes_per_sec =
                    max_u64(ack->delivery_rate_bytes_per_sec,
                            s->bw_latest_bytes_per_sec);
            }
            if (s->inflight_shortterm_bytes == TCP_SHIFT_BBR3_UNBOUNDED) {
                s->inflight_shortterm_bytes = ack->cwnd_bytes;
            }
            s->bw_shortterm_bytes_per_sec =
                max_u64(s->bw_latest_bytes_per_sec,
                        ratio(s->bw_shortterm_bytes_per_sec,
                              TCP_SHIFT_BBR3_LOSS_BETA_NUM,
                              TCP_SHIFT_BBR3_LOSS_BETA_DEN));
            s->inflight_shortterm_bytes =
                max_u64(s->inflight_latest_bytes,
                        ratio(s->inflight_shortterm_bytes,
                              TCP_SHIFT_BBR3_LOSS_BETA_NUM,
                              TCP_SHIFT_BBR3_LOSS_BETA_DEN));
            s->bw_shortterm_bytes_per_sec =
                max_u64(1U, s->bw_shortterm_bytes_per_sec);
        }
        if (s->ecn_in_round != 0U && ack->ecn_eligible != 0U) {
            if (s->inflight_shortterm_bytes == TCP_SHIFT_BBR3_UNBOUNDED) {
                s->inflight_shortterm_bytes = ack->cwnd_bytes;
            }
            ecn_cut_q8 = (uint32_t)(
                (uint64_t)s->ecn_alpha_q8 *
                TCP_SHIFT_BBR3_ECN_FACTOR_Q8 /
                TCP_SHIFT_BBR3_ECN_ALPHA_SCALE);
            ecn_candidate = ratio(s->inflight_shortterm_bytes,
                                  TCP_SHIFT_BBR3_ECN_ALPHA_SCALE -
                                      ecn_cut_q8,
                                  TCP_SHIFT_BBR3_ECN_ALPHA_SCALE);
            s->inflight_shortterm_bytes =
                min_u64(s->inflight_shortterm_bytes, ecn_candidate);
        }
    }

    s->loss_in_round = 0U;
    s->ecn_in_round = 0U;
    s->ecn_round_delivered_bytes = 0U;
    s->ecn_round_ce_bytes = 0U;
    s->bw_latest_bytes_per_sec = ack->rate_valid != 0U
                                     ? ack->delivery_rate_bytes_per_sec
                                     : 0U;
    s->inflight_latest_bytes = ack->newly_delivered_bytes;
}

/* The newly ACKed data in ProbeBW_UP conservatively raises an existing
 * inflight_longterm bound, beginning at ~one MSS per RTT and doubling the
 * growth slope each cwnd-limited packet-timed RTT (max shift 30). */
static void probe_up_growth(struct tcp_shift_bbr3_congestion *s,
                            const struct tcp_shift_bbr3_congestion_ack *ack)
{
    uint64_t units;
    uint32_t rounds;

    if (ack->probe_up == 0U || ack->cwnd_limited == 0U ||
        s->inflight_longterm_bytes == TCP_SHIFT_BBR3_UNBOUNDED ||
        ack->cwnd_bytes < s->inflight_longterm_bytes) {
        return;
    }

    if (s->probe_up_acked_per_inc_bytes == 0U) {
        s->probe_up_acked_per_inc_bytes =
            max_u64(ack->mss_bytes, ack->cwnd_bytes);
    }
    s->probe_up_acked_bytes =
        sat_add(s->probe_up_acked_bytes, ack->newly_acked_bytes);
    units = s->probe_up_acked_bytes / s->probe_up_acked_per_inc_bytes;
    s->probe_up_acked_bytes %= s->probe_up_acked_per_inc_bytes;
    s->inflight_longterm_bytes = sat_add(
        s->inflight_longterm_bytes,
        units > UINT64_MAX / ack->mss_bytes
            ? UINT64_MAX
            : units * ack->mss_bytes);

    if (ack->round_start != 0U) {
        rounds = s->probe_up_rounds > 30U ? 30U : s->probe_up_rounds;
        s->probe_up_acked_per_inc_bytes =
            max_u64(ack->mss_bytes, ack->cwnd_bytes / (UINT64_C(1) << rounds));
        if (s->probe_up_rounds < 30U) {
            s->probe_up_rounds++;
        }
    }
}

int tcp_shift_bbr3_congestion_on_ack(
    struct tcp_shift_bbr3_congestion *s,
    const struct tcp_shift_bbr3_congestion_ack *ack)
{
    uint32_t signal = TCP_SHIFT_BBR3_SIGNAL_NONE;

    if (s == NULL || ack == NULL || ack->mss_bytes == 0U ||
        ack->cwnd_bytes == 0U ||
        ack->newly_ce_bytes > ack->newly_delivered_bytes ||
        (ack->rate_valid == 0U && ack->delivery_rate_bytes_per_sec != 0U) ||
        (ack->ecn_eligible == 0U && ack->newly_ce_bytes != 0U) ||
        (ack->probe_up != 0U && ack->probing_bandwidth == 0U)) {
        return -1;
    }

    if (ack->rate_valid != 0U) {
        s->bw_latest_bytes_per_sec =
            max_u64(s->bw_latest_bytes_per_sec,
                    ack->delivery_rate_bytes_per_sec);
    }
    s->inflight_latest_bytes =
        max_u64(s->inflight_latest_bytes, ack->newly_delivered_bytes);
    if (ack->newly_lost_bytes != 0U) {
        s->loss_in_round = 1U;
    }
    if (ack->newly_ce_bytes != 0U) {
        s->ecn_in_round = 1U;
    }
    if (ack->ecn_eligible != 0U) {
        s->ecn_round_delivered_bytes =
            sat_add(s->ecn_round_delivered_bytes,
                    ack->newly_delivered_bytes);
        s->ecn_round_ce_bytes =
            sat_add(s->ecn_round_ce_bytes, ack->newly_ce_bytes);
    }

    if (ack->probe_sample != 0U &&
        s->probe_feedback_consumed == 0U) {
        if (greater_than_percent(ack->newly_lost_bytes,
                                 ack->tx_inflight_bytes,
                                 TCP_SHIFT_BBR3_EXCESS_LOSS_PERCENT)) {
            signal |= TCP_SHIFT_BBR3_SIGNAL_EXCESS_LOSS;
        }
        if (ack->ecn_eligible != 0U &&
            greater_than_percent(ack->newly_ce_bytes,
                                 ack->newly_delivered_bytes,
                                 TCP_SHIFT_BBR3_ECN_EXCESS_PERCENT)) {
            signal |= TCP_SHIFT_BBR3_SIGNAL_EXCESS_ECN;
        }
        if (signal != TCP_SHIFT_BBR3_SIGNAL_NONE) {
            s->probe_feedback_consumed = 1U;
            /* App-limited samples can stop probing, but may not install a
             * smaller upper bound: they have not exercised the full pipe. */
            if (ack->app_limited == 0U) {
                s->inflight_longterm_bytes = max_u64(
                    ack->tx_inflight_bytes,
                    ratio(ack->target_inflight_bytes,
                          TCP_SHIFT_BBR3_LOSS_BETA_NUM,
                          TCP_SHIFT_BBR3_LOSS_BETA_DEN));
            }
        } else if (s->inflight_longterm_bytes !=
                   TCP_SHIFT_BBR3_UNBOUNDED) {
            s->inflight_longterm_bytes =
                max_u64(s->inflight_longterm_bytes,
                        ack->tx_inflight_bytes);
            probe_up_growth(s, ack);
        }
    }

    if (ack->loss_round_end != 0U) {
        finish_loss_round(s, ack);
    }
    return (int)signal;
}

uint64_t tcp_shift_bbr3_congestion_rate_cap(
    const struct tcp_shift_bbr3_congestion *s,
    enum tcp_shift_bbr3_phase phase,
    uint64_t max_bw_bytes_per_sec)
{
    if (s == NULL) {
        return 0U;
    }
    if (phase == TCP_SHIFT_BBR3_DRAIN ||
        phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN ||
        phase == TCP_SHIFT_BBR3_PROBE_BW_CRUISE ||
        phase == TCP_SHIFT_BBR3_PROBE_RTT) {
        return min_u64(max_bw_bytes_per_sec, s->bw_shortterm_bytes_per_sec);
    }
    return max_bw_bytes_per_sec;
}

uint64_t tcp_shift_bbr3_congestion_cwnd_cap(
    const struct tcp_shift_bbr3_congestion *s,
    enum tcp_shift_bbr3_phase phase,
    uint32_t mss_bytes)
{
    uint64_t cap = UINT64_MAX;
    uint64_t floor;

    if (s == NULL || mss_bytes == 0U) {
        return 0U;
    }
    floor = (uint64_t)mss_bytes * 4U;
    if (phase == TCP_SHIFT_BBR3_STARTUP) {
        return UINT64_MAX;
    }
    if (phase == TCP_SHIFT_BBR3_PROBE_BW_CRUISE ||
        phase == TCP_SHIFT_BBR3_PROBE_RTT) {
        cap = tcp_shift_bbr3_inflight_with_headroom(
            s->inflight_longterm_bytes, mss_bytes);
    } else {
        cap = s->inflight_longterm_bytes;
    }
    if (phase == TCP_SHIFT_BBR3_DRAIN ||
        phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN ||
        phase == TCP_SHIFT_BBR3_PROBE_BW_CRUISE ||
        phase == TCP_SHIFT_BBR3_PROBE_RTT) {
        cap = min_u64(cap, s->inflight_shortterm_bytes);
    }
    return max_u64(cap, floor);
}
