#include "cc/bbr.h"

#include <stddef.h>

#define TCP_SHIFT_BBR_NSEC_PER_SEC UINT64_C(1000000000)

static uint64_t tcp_shift_bbr_max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

static uint64_t tcp_shift_bbr_sat_add_u64(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static uint64_t tcp_shift_bbr_sat_mul_u64(uint64_t a, uint64_t b)
{
    if (a == 0U || b == 0U) {
        return 0U;
    }
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* Scale a u64 by a small rational without constructing value*numerator first.
 * numerator and denominator are u32, so the remainder product is strictly
 * smaller than 2^64. */
static uint64_t tcp_shift_bbr_scale_floor_u64(uint64_t value,
                                               uint32_t numerator,
                                               uint32_t denominator)
{
    uint64_t whole;
    uint64_t remainder_product;

    if (denominator == 0U) {
        return UINT64_MAX;
    }

    whole = tcp_shift_bbr_sat_mul_u64(value / denominator, numerator);
    if (whole == UINT64_MAX) {
        return UINT64_MAX;
    }

    remainder_product = (value % denominator) * (uint64_t)numerator;
    return tcp_shift_bbr_sat_add_u64(
        whole, remainder_product / denominator);
}

static uint64_t tcp_shift_bbr_scale_ceil_u64(uint64_t value,
                                              uint32_t numerator,
                                              uint32_t denominator)
{
    uint64_t whole;
    uint64_t remainder_product;

    if (denominator == 0U) {
        return UINT64_MAX;
    }

    whole = tcp_shift_bbr_sat_mul_u64(value / denominator, numerator);
    if (whole == UINT64_MAX) {
        return UINT64_MAX;
    }

    remainder_product = (value % denominator) * (uint64_t)numerator;
    whole = tcp_shift_bbr_sat_add_u64(
        whole, remainder_product / denominator);
    if (whole == UINT64_MAX) {
        return UINT64_MAX;
    }
    if ((remainder_product % denominator) != 0U) {
        whole = tcp_shift_bbr_sat_add_u64(whole, 1U);
    }
    return whole;
}

static int tcp_shift_bbr_elapsed_gt(uint64_t now_ns,
                                    uint64_t stamp_ns,
                                    uint64_t interval_ns)
{
    return now_ns > stamp_ns && now_ns - stamp_ns > interval_ns;
}

uint64_t tcp_shift_bbr_bdp_bytes(uint64_t bandwidth_bytes_per_sec,
                                 uint64_t rtt_ns)
{
    uint64_t bandwidth_whole;
    uint64_t bandwidth_remainder;
    uint64_t rtt_whole;
    uint64_t rtt_remainder;
    uint64_t cross;
    uint64_t result;

    if (bandwidth_bytes_per_sec == 0U || rtt_ns == 0U) {
        return 0U;
    }

    /* Exact ceil(bandwidth * rtt / 1e9) without a 128-bit intermediate:
     *
     *   bw = bw_q*D + bw_r
     *   rtt = rtt_q*D + rtt_r
     *   bw*rtt/D = bw_q*rtt + bw_r*rtt_q + bw_r*rtt_r/D
     *
     * Both remainders are < 1e9, so their product is < 1e18 and fits u64.
     * Any overflow in the quotient terms means the mathematical BDP itself is
     * above UINT64_MAX, so saturation is the correct result. */
    bandwidth_whole = bandwidth_bytes_per_sec / TCP_SHIFT_BBR_NSEC_PER_SEC;
    bandwidth_remainder =
        bandwidth_bytes_per_sec % TCP_SHIFT_BBR_NSEC_PER_SEC;
    rtt_whole = rtt_ns / TCP_SHIFT_BBR_NSEC_PER_SEC;
    rtt_remainder = rtt_ns % TCP_SHIFT_BBR_NSEC_PER_SEC;

    result = tcp_shift_bbr_sat_mul_u64(bandwidth_whole, rtt_ns);
    if (result == UINT64_MAX) {
        return UINT64_MAX;
    }
    result = tcp_shift_bbr_sat_add_u64(
        result,
        tcp_shift_bbr_sat_mul_u64(bandwidth_remainder, rtt_whole));
    if (result == UINT64_MAX) {
        return UINT64_MAX;
    }

    cross = bandwidth_remainder * rtt_remainder;
    result = tcp_shift_bbr_sat_add_u64(
        result, cross / TCP_SHIFT_BBR_NSEC_PER_SEC);
    if (result == UINT64_MAX) {
        return UINT64_MAX;
    }
    if ((cross % TCP_SHIFT_BBR_NSEC_PER_SEC) != 0U) {
        result = tcp_shift_bbr_sat_add_u64(result, 1U);
    }
    return result;
}

uint64_t tcp_shift_bbr_startup_pacing_rate_bytes_per_sec(
    uint64_t bandwidth_bytes_per_sec)
{
    /* Combine Startup gain and the 1% pacing margin into one rational so a
     * large rate cannot be prematurely saturated and then reduced again. */
    return tcp_shift_bbr_scale_floor_u64(
        bandwidth_bytes_per_sec,
        TCP_SHIFT_BBR_STARTUP_GAIN_NUM * TCP_SHIFT_BBR_PACING_MARGIN_NUM,
        TCP_SHIFT_BBR_GAIN_DEN * TCP_SHIFT_BBR_PACING_MARGIN_DEN);
}

uint32_t tcp_shift_bbr_startup_cwnd_target_bytes(
    uint64_t bandwidth_bytes_per_sec,
    uint64_t min_rtt_ns,
    uint32_t mss_bytes,
    uint32_t cwnd_limit_bytes,
    uint32_t initial_cwnd_bytes)
{
    uint64_t minimum_cwnd;
    uint64_t target;

    if (mss_bytes == 0U || cwnd_limit_bytes == 0U) {
        return 0U;
    }

    minimum_cwnd = (uint64_t)mss_bytes * TCP_SHIFT_BBR_MIN_CWND_PACKETS;
    if (minimum_cwnd > cwnd_limit_bytes) {
        minimum_cwnd = cwnd_limit_bytes;
    }

    if (bandwidth_bytes_per_sec == 0U || min_rtt_ns == 0U) {
        target = initial_cwnd_bytes;
    } else {
        /* tcp_shift_bbr_bdp_bytes() first rounds the base BDP upward, then the
         * fixed-point gain is also rounded upward. This is deliberately
         * conservative by at most a few bytes and, like Linux BBR's BDP
         * rounding, avoids a truncation-driven negative feedback loop. */
        target = tcp_shift_bbr_bdp_bytes(bandwidth_bytes_per_sec, min_rtt_ns);
        target = tcp_shift_bbr_scale_ceil_u64(
            target, TCP_SHIFT_BBR_STARTUP_GAIN_NUM,
            TCP_SHIFT_BBR_GAIN_DEN);
    }

    if (target < minimum_cwnd) {
        target = minimum_cwnd;
    }
    if (target > cwnd_limit_bytes) {
        target = cwnd_limit_bytes;
    }
    return (uint32_t)target;
}

int tcp_shift_bbr_startup_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint32_t initial_cwnd_bytes,
    uint32_t current_cwnd_bytes,
    uint64_t current_pacing_rate_bytes_per_sec,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t pacing_target;
    uint64_t grown_cwnd;
    uint32_t cwnd_target;
    uint32_t minimum_cwnd;
    uint32_t cwnd;

    if (model == NULL || transport == NULL || ack == NULL || policy == NULL ||
        model->mode != TCP_SHIFT_BBR_MODE_STARTUP || transport->mss_bytes == 0U ||
        transport->cwnd_limit_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        initial_cwnd_bytes == 0U ||
        initial_cwnd_bytes > transport->cwnd_limit_bytes ||
        current_cwnd_bytes == 0U ||
        current_cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    cwnd_target = tcp_shift_bbr_startup_cwnd_target_bytes(
        model->max_bw_bytes_per_sec,
        model->has_min_rtt != 0U ? model->min_rtt_ns : 0U,
        transport->mss_bytes,
        transport->cwnd_limit_bytes,
        initial_cwnd_bytes);
    if (cwnd_target == 0U) {
        return -1;
    }

    minimum_cwnd = transport->mss_bytes >
                           transport->cwnd_limit_bytes /
                               TCP_SHIFT_BBR_MIN_CWND_PACKETS
                       ? transport->cwnd_limit_bytes
                       : transport->mss_bytes * TCP_SHIFT_BBR_MIN_CWND_PACKETS;

    cwnd = current_cwnd_bytes;
    if (ack->acked_bytes != 0U) {
        grown_cwnd = (uint64_t)cwnd + ack->acked_bytes;
        if (grown_cwnd > transport->cwnd_limit_bytes) {
            grown_cwnd = transport->cwnd_limit_bytes;
        }

        if (model->full_bw_reached != 0U) {
            cwnd = (uint32_t)grown_cwnd < cwnd_target
                       ? (uint32_t)grown_cwnd
                       : cwnd_target;
        } else if (cwnd < cwnd_target ||
                   ack->rate.delivered_total_bytes < initial_cwnd_bytes) {
            cwnd = (uint32_t)grown_cwnd;
        }
    }

    if (cwnd < minimum_cwnd) {
        cwnd = minimum_cwnd;
    }
    if (cwnd > transport->cwnd_limit_bytes) {
        cwnd = transport->cwnd_limit_bytes;
    }

    pacing_target = tcp_shift_bbr_startup_pacing_rate_bytes_per_sec(
        model->max_bw_bytes_per_sec);
    if (pacing_target < current_pacing_rate_bytes_per_sec) {
        pacing_target = current_pacing_rate_bytes_per_sec;
    }

    policy->cwnd_bytes = cwnd;
    /* BBR does not use Reno slow-start threshold as a growth gate. Keep the
     * transport threshold at its representable ceiling until BBR loss policy
     * is independently qualified. */
    policy->ssthresh_bytes = transport->cwnd_limit_bytes;
    policy->pacing_rate_bytes_per_sec = pacing_target;
    return 0;
}

static void tcp_shift_bbr_recompute_max_bw(struct tcp_shift_bbr_model *model)
{
    uint64_t max_bw = 0U;
    uint32_t i;

    for (i = 0U; i < TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS; i++) {
        max_bw = tcp_shift_bbr_max_u64(max_bw, model->max_bw_filter[i]);
    }
    model->max_bw_bytes_per_sec = max_bw;
}

static void tcp_shift_bbr_update_round(struct tcp_shift_bbr_model *model,
                                       const struct tcp_shift_cc_rate_sample *sample)
{
    model->round_start = 0U;

    if (sample->delivered_total_bytes == 0U ||
        sample->delivered_total_bytes < sample->prior_delivered_bytes) {
        return;
    }

    if (sample->prior_delivered_bytes >= model->next_round_delivered) {
        model->next_round_delivered = sample->delivered_total_bytes;
        model->round_count++;
        model->round_start = 1U;
    }
}

static void tcp_shift_bbr_age_bw_filter(struct tcp_shift_bbr_model *model,
                                        uint32_t round)
{
    uint32_t delta;
    uint32_t step;

    if (model->has_bw_filter_round == 0U) {
        model->bw_filter_round = round;
        model->has_bw_filter_round = 1U;
        return;
    }

    delta = round - model->bw_filter_round;
    if (delta == 0U) {
        return;
    }

    if (delta >= TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS) {
        for (step = 0U; step < TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS; step++) {
            model->max_bw_filter[step] = 0U;
        }
    } else {
        for (step = 1U; step <= delta; step++) {
            uint32_t slot =
                (model->bw_filter_round + step) %
                TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS;
            model->max_bw_filter[slot] = 0U;
        }
    }

    model->bw_filter_round = round;
    tcp_shift_bbr_recompute_max_bw(model);
}

static void tcp_shift_bbr_update_max_bw(
    struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_rate_sample *sample)
{
    uint32_t slot;

    if ((sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) == 0U ||
        sample->delivery_rate_bytes_per_sec == 0U) {
        return;
    }

    model->valid_rate_samples++;

    if ((sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U &&
        sample->delivery_rate_bytes_per_sec < model->max_bw_bytes_per_sec) {
        model->ignored_app_limited_bw_samples++;
        return;
    }

    tcp_shift_bbr_age_bw_filter(model, model->round_count);
    slot = model->round_count % TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS;
    if (sample->delivery_rate_bytes_per_sec > model->max_bw_filter[slot]) {
        model->max_bw_filter[slot] = sample->delivery_rate_bytes_per_sec;
    }
    model->accepted_bw_samples++;
    tcp_shift_bbr_recompute_max_bw(model);
}

static uint64_t tcp_shift_bbr_full_bw_threshold(uint64_t full_bw)
{
    uint64_t quarter = full_bw / 4U;
    uint64_t increment;

    if ((full_bw % 4U) != 0U) {
        quarter++;
    }
    increment = quarter;
    if (UINT64_MAX - full_bw < increment) {
        return UINT64_MAX;
    }
    return full_bw + increment;
}

static void tcp_shift_bbr_check_startup_full_bw(
    struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_rate_sample *sample)
{
    uint64_t threshold;

    model->full_bw_now = 0U;

    if (model->mode != TCP_SHIFT_BBR_MODE_STARTUP ||
        model->full_bw_reached != 0U || model->round_start == 0U ||
        (sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) == 0U ||
        sample->delivery_rate_bytes_per_sec == 0U ||
        (sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U) {
        return;
    }

    threshold = tcp_shift_bbr_full_bw_threshold(model->full_bw_bytes_per_sec);
    if (model->full_bw_bytes_per_sec == 0U ||
        sample->delivery_rate_bytes_per_sec >= threshold) {
        model->full_bw_bytes_per_sec = sample->delivery_rate_bytes_per_sec;
        model->full_bw_count = 0U;
        return;
    }

    model->full_bw_count++;
    if (model->full_bw_count >= TCP_SHIFT_BBR_FULL_BW_ROUNDS) {
        model->full_bw_now = 1U;
        model->full_bw_reached = 1U;
    }
}

static void tcp_shift_bbr_update_min_rtt(
    struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_rate_sample *sample,
    uint64_t now_ns)
{
    int probe_expired;
    int min_expired;

    if ((sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) == 0U ||
        sample->rtt_ns == 0U) {
        model->probe_rtt_expired = 0U;
        model->min_rtt_expired = 0U;
        return;
    }

    model->valid_rtt_samples++;

    probe_expired = model->has_probe_rtt_min != 0U &&
                    tcp_shift_bbr_elapsed_gt(
                        now_ns, model->probe_rtt_min_stamp_ns,
                        TCP_SHIFT_BBR_PROBE_RTT_INTERVAL_NS);
    model->probe_rtt_expired = probe_expired != 0 ? 1U : 0U;

    if (model->has_probe_rtt_min == 0U ||
        sample->rtt_ns < model->probe_rtt_min_delay_ns ||
        probe_expired != 0) {
        model->probe_rtt_min_delay_ns = sample->rtt_ns;
        model->probe_rtt_min_stamp_ns = now_ns;
        model->has_probe_rtt_min = 1U;
    }

    min_expired = model->has_min_rtt != 0U &&
                  tcp_shift_bbr_elapsed_gt(now_ns, model->min_rtt_stamp_ns,
                                           TCP_SHIFT_BBR_MIN_RTT_FILTER_NS);
    model->min_rtt_expired = min_expired != 0 ? 1U : 0U;

    if (model->has_min_rtt == 0U ||
        model->probe_rtt_min_delay_ns < model->min_rtt_ns ||
        min_expired != 0) {
        model->min_rtt_ns = model->probe_rtt_min_delay_ns;
        model->min_rtt_stamp_ns = model->probe_rtt_min_stamp_ns;
        model->has_min_rtt = 1U;
    }
}

void tcp_shift_bbr_model_init(struct tcp_shift_bbr_model *model)
{
    size_t i;

    if (model == NULL) {
        return;
    }

    model->max_bw_bytes_per_sec = 0U;
    for (i = 0U; i < TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS; i++) {
        model->max_bw_filter[i] = 0U;
    }

    model->min_rtt_ns = 0U;
    model->min_rtt_stamp_ns = 0U;
    model->probe_rtt_min_delay_ns = 0U;
    model->probe_rtt_min_stamp_ns = 0U;
    model->last_update_ns = 0U;

    model->valid_rate_samples = 0U;
    model->accepted_bw_samples = 0U;
    model->ignored_app_limited_bw_samples = 0U;
    model->valid_rtt_samples = 0U;

    model->next_round_delivered = 0U;
    model->full_bw_bytes_per_sec = 0U;

    model->bw_filter_round = 0U;
    model->round_count = 0U;
    model->full_bw_count = 0U;
    model->mode = TCP_SHIFT_BBR_MODE_STARTUP;

    model->has_min_rtt = 0U;
    model->has_probe_rtt_min = 0U;
    model->has_update_time = 0U;
    model->has_bw_filter_round = 0U;
    model->probe_rtt_expired = 0U;
    model->min_rtt_expired = 0U;
    model->round_start = 0U;
    model->full_bw_now = 0U;
    model->full_bw_reached = 0U;
}

int tcp_shift_bbr_model_on_ack(struct tcp_shift_bbr_model *model,
                               const struct tcp_shift_cc_rate_sample *sample,
                               uint64_t now_ns)
{
    if (model == NULL || sample == NULL) {
        return -1;
    }

    if (model->has_update_time != 0U && now_ns < model->last_update_ns) {
        return -1;
    }

    model->last_update_ns = now_ns;
    model->has_update_time = 1U;

    tcp_shift_bbr_update_round(model, sample);
    tcp_shift_bbr_update_max_bw(model, sample);
    tcp_shift_bbr_check_startup_full_bw(model, sample);
    tcp_shift_bbr_update_min_rtt(model, sample, now_ns);
    return 0;
}
