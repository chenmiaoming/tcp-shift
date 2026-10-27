#include "cc/bbr3_model.h"

#include <stddef.h>
#include <string.h>

static uint64_t max_u64(uint64_t left, uint64_t right)
{
    return left > right ? left : right;
}

uint64_t tcp_shift_bbr3_scale(uint64_t value,
                              struct tcp_shift_bbr3_gain gain)
{
    uint64_t whole;
    uint64_t fraction;

    if (gain.denominator == 0U) {
        return UINT64_MAX;
    }
    whole = value / gain.denominator;
    if (gain.numerator != 0U &&
        whole > UINT64_MAX / gain.numerator) {
        return UINT64_MAX;
    }
    whole *= gain.numerator;
    fraction = (value % gain.denominator) * (uint64_t)gain.numerator;
    fraction /= gain.denominator;
    return UINT64_MAX - whole < fraction ? UINT64_MAX
                                          : whole + fraction;
}

void tcp_shift_bbr3_model_init(struct tcp_shift_bbr3_model *model)
{
    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->phase = TCP_SHIFT_BBR3_STARTUP;
}

static int expired(uint64_t now_ns, uint64_t timestamp, uint64_t age_ns)
{
    return now_ns > timestamp && now_ns - timestamp > age_ns;
}

static void update_rtt(struct tcp_shift_bbr3_model *model,
                       const struct tcp_shift_cc_rate_sample *sample,
                       uint64_t now_ns)
{
    int probe_expired = model->has_probe_rtt_min != 0U &&
        expired(now_ns, model->probe_rtt_min_stamp_ns,
                TCP_SHIFT_BBR3_PROBE_RTT_INTERVAL_NS);
    int min_expired = model->has_min_rtt != 0U &&
        expired(now_ns, model->min_rtt_stamp_ns,
                TCP_SHIFT_BBR3_MIN_RTT_WINDOW_NS);

    model->probe_rtt_expired = (uint8_t)probe_expired;
    if ((sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) != 0U &&
        sample->rtt_ns != 0U &&
        (model->has_probe_rtt_min == 0U ||
         sample->rtt_ns < model->probe_rtt_min_delay_ns ||
         probe_expired)) {
        model->probe_rtt_min_delay_ns = sample->rtt_ns;
        model->probe_rtt_min_stamp_ns = now_ns;
        model->has_probe_rtt_min = 1U;
    }
    if (model->has_probe_rtt_min != 0U &&
        (model->has_min_rtt == 0U ||
         model->probe_rtt_min_delay_ns < model->min_rtt_ns ||
         min_expired)) {
        model->min_rtt_ns = model->probe_rtt_min_delay_ns;
        model->min_rtt_stamp_ns = model->probe_rtt_min_stamp_ns;
        model->has_min_rtt = 1U;
    }
}

static void update_round(struct tcp_shift_bbr3_model *model,
                         const struct tcp_shift_cc_rate_sample *sample)
{
    model->round_start = 0U;
    if (sample->delivered_total_bytes == 0U ||
        sample->prior_delivered_bytes < model->next_round_delivered) {
        return;
    }
    model->next_round_delivered = sample->delivered_total_bytes;
    model->round_count++;
    model->round_start = 1U;
}

static void update_full_bw(struct tcp_shift_bbr3_model *model,
                           const struct tcp_shift_cc_rate_sample *sample)
{
    uint64_t threshold;
    uint64_t quarter;

    if (model->full_bw_reached != 0U ||
        model->round_start == 0U ||
        (sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) == 0U ||
        (sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U ||
        model->max_bw_bytes_per_sec == 0U) {
        return;
    }

    /* Require 25% rate growth to reset the three-round full-pipe counter. */
    quarter = model->full_bw_bytes_per_sec / 4U;
    if ((model->full_bw_bytes_per_sec % 4U) != 0U) {
        quarter++;
    }
    threshold = UINT64_MAX - model->full_bw_bytes_per_sec < quarter
                    ? UINT64_MAX
                    : model->full_bw_bytes_per_sec + quarter;
    if (model->full_bw_bytes_per_sec == 0U ||
        model->max_bw_bytes_per_sec >= threshold) {
        model->full_bw_bytes_per_sec = model->max_bw_bytes_per_sec;
        model->full_bw_count = 0U;
    } else {
        model->full_bw_count++;
        if (model->full_bw_count >= TCP_SHIFT_BBR3_FULL_BW_ROUNDS) {
            model->full_bw_reached = 1U;
        }
    }
}

int tcp_shift_bbr3_model_on_ack(
    struct tcp_shift_bbr3_model *model,
    const struct tcp_shift_cc_rate_sample *sample,
    uint64_t now_ns)
{
    uint32_t slot;
    uint64_t rate;

    if (model == NULL || sample == NULL || now_ns == 0U ||
        (model->has_update_time != 0U &&
         now_ns < model->last_update_ns) ||
        sample->prior_delivered_bytes > sample->delivered_total_bytes) {
        return -1;
    }

    model->last_update_ns = now_ns;
    model->has_update_time = 1U;
    update_round(model, sample);
    update_rtt(model, sample, now_ns);

    rate = sample->delivery_rate_bytes_per_sec;
    if ((sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) != 0U &&
        rate != 0U &&
        (rate >= model->max_bw_bytes_per_sec ||
         (sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) == 0U)) {
        slot = model->cycle_count % TCP_SHIFT_BBR3_MAX_BW_CYCLES;
        model->max_bw_filter[slot] =
            max_u64(rate, model->max_bw_filter[slot]);
        model->max_bw_bytes_per_sec = max_u64(
            model->max_bw_filter[0], model->max_bw_filter[1]);
        model->has_bw = 1U;
    }
    update_full_bw(model, sample);
    return 0;
}

int tcp_shift_bbr3_model_advance_bw_cycle(
    struct tcp_shift_bbr3_model *model)
{
    uint32_t slot;

    if (model == NULL || model->cycle_count == UINT32_MAX) {
        return -1;
    }
    model->cycle_count++;
    slot = model->cycle_count % TCP_SHIFT_BBR3_MAX_BW_CYCLES;
    model->max_bw_filter[slot] = 0U;
    model->max_bw_bytes_per_sec = max_u64(
        model->max_bw_filter[0], model->max_bw_filter[1]);
    model->has_bw = model->max_bw_bytes_per_sec != 0U;
    return 0;
}

struct tcp_shift_bbr3_gain tcp_shift_bbr3_pacing_gain(
    enum tcp_shift_bbr3_phase phase)
{
    struct tcp_shift_bbr3_gain gain = {0U, 1U};

    switch (phase) {
    case TCP_SHIFT_BBR3_STARTUP:
        gain.numerator = 277U;
        gain.denominator = 100U;
        break;
    case TCP_SHIFT_BBR3_DRAIN:
        gain.numerator = 1U;
        gain.denominator = 2U;
        break;
    case TCP_SHIFT_BBR3_PROBE_BW_DOWN:
        gain.numerator = 9U;
        gain.denominator = 10U;
        break;
    case TCP_SHIFT_BBR3_PROBE_BW_UP:
        gain.numerator = 5U;
        gain.denominator = 4U;
        break;
    case TCP_SHIFT_BBR3_PROBE_BW_CRUISE:
    case TCP_SHIFT_BBR3_PROBE_BW_REFILL:
    case TCP_SHIFT_BBR3_PROBE_RTT:
        gain.numerator = 1U;
        gain.denominator = 1U;
        break;
    default:
        break;
    }
    return gain;
}

struct tcp_shift_bbr3_gain tcp_shift_bbr3_cwnd_gain(
    enum tcp_shift_bbr3_phase phase)
{
    struct tcp_shift_bbr3_gain gain = {2U, 1U};

    if (phase == TCP_SHIFT_BBR3_PROBE_RTT) {
        gain.numerator = 1U;
        gain.denominator = 2U;
    } else if (phase > TCP_SHIFT_BBR3_PROBE_RTT) {
        gain.numerator = 0U;
    }
    return gain;
}

uint64_t tcp_shift_bbr3_inflight_with_headroom(
    uint64_t inflight_longterm_bytes,
    uint32_t mss_bytes)
{
    uint64_t min_pipe = (uint64_t)mss_bytes * 4U;
    uint64_t headroom;
    uint64_t result;

    if (mss_bytes == 0U) {
        return 0U;
    }
    if (inflight_longterm_bytes == UINT64_MAX) {
        return UINT64_MAX;
    }

    headroom = tcp_shift_bbr3_scale(
        inflight_longterm_bytes,
        (struct tcp_shift_bbr3_gain){15U, 100U});
    if (headroom < mss_bytes) {
        headroom = mss_bytes;
    }
    result = inflight_longterm_bytes > headroom
                 ? inflight_longterm_bytes - headroom
                 : 0U;
    return result < min_pipe ? min_pipe : result;
}
