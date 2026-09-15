#include "cc/bbr.h"

#include <stddef.h>

static uint64_t tcp_shift_bbr_max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

static int tcp_shift_bbr_elapsed_gt(uint64_t now_ns,
                                    uint64_t stamp_ns,
                                    uint64_t interval_ns)
{
    return now_ns > stamp_ns && now_ns - stamp_ns > interval_ns;
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

/* Keep the BBRv1-style recent-bandwidth horizon in packet-timed rounds. The
 * window advances only when a sample is admissible for the bandwidth model.
 * This deliberately preserves the last trustworthy path rate across an
 * arbitrarily long application-limited period, matching BBR's rule that a low
 * app-limited sample must not make the sender slow itself down. */
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
