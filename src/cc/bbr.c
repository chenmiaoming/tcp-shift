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
    model->max_bw_bytes_per_sec =
        tcp_shift_bbr_max_u64(model->max_bw_filter[0],
                              model->max_bw_filter[1]);
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

    /* draft-ietf-ccwg-bbr-06 UpdateMaxBw(): app-limited samples do not
     * decrease the model, but a sample at/above the current max remains useful
     * evidence that max_bw was not too high. */
    if ((sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U &&
        sample->delivery_rate_bytes_per_sec < model->max_bw_bytes_per_sec) {
        model->ignored_app_limited_bw_samples++;
        return;
    }

    slot = model->cycle_count % TCP_SHIFT_BBR_MAX_BW_FILTER_CYCLES;
    if (sample->delivery_rate_bytes_per_sec > model->max_bw_filter[slot]) {
        model->max_bw_filter[slot] = sample->delivery_rate_bytes_per_sec;
    }
    model->accepted_bw_samples++;
    tcp_shift_bbr_recompute_max_bw(model);
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
    for (i = 0U; i < TCP_SHIFT_BBR_MAX_BW_FILTER_CYCLES; i++) {
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

    model->cycle_count = 0U;
    model->mode = TCP_SHIFT_BBR_MODE_STARTUP;

    model->has_min_rtt = 0U;
    model->has_probe_rtt_min = 0U;
    model->has_update_time = 0U;
    model->probe_rtt_expired = 0U;
    model->min_rtt_expired = 0U;
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

    tcp_shift_bbr_update_max_bw(model, sample);
    tcp_shift_bbr_update_min_rtt(model, sample, now_ns);
    return 0;
}

void tcp_shift_bbr_model_advance_bw_cycle(struct tcp_shift_bbr_model *model)
{
    uint32_t slot;

    if (model == NULL) {
        return;
    }

    model->cycle_count++;
    slot = model->cycle_count % TCP_SHIFT_BBR_MAX_BW_FILTER_CYCLES;
    model->max_bw_filter[slot] = 0U;
    tcp_shift_bbr_recompute_max_bw(model);
}
