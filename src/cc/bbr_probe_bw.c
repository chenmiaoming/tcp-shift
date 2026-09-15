#include "cc/bbr_probe_bw.h"

#include <stddef.h>

static uint64_t tcp_shift_bbr_probe_sat_add_u64(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static uint64_t tcp_shift_bbr_probe_sat_mul_u64(uint64_t a, uint64_t b)
{
    if (a == 0U || b == 0U) {
        return 0U;
    }
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

static uint64_t tcp_shift_bbr_probe_scale_floor_u64(uint64_t value,
                                                     uint32_t numerator,
                                                     uint32_t denominator)
{
    uint64_t whole;
    uint64_t remainder_product;

    if (denominator == 0U) {
        return UINT64_MAX;
    }

    whole = tcp_shift_bbr_probe_sat_mul_u64(value / denominator, numerator);
    if (whole == UINT64_MAX) {
        return UINT64_MAX;
    }
    remainder_product = (value % denominator) * (uint64_t)numerator;
    return tcp_shift_bbr_probe_sat_add_u64(
        whole, remainder_product / denominator);
}

static uint64_t tcp_shift_bbr_probe_scale_ceil_u64(uint64_t value,
                                                    uint32_t numerator,
                                                    uint32_t denominator)
{
    uint64_t whole;
    uint64_t remainder_product;

    if (denominator == 0U) {
        return UINT64_MAX;
    }

    whole = tcp_shift_bbr_probe_sat_mul_u64(value / denominator, numerator);
    if (whole == UINT64_MAX) {
        return UINT64_MAX;
    }
    remainder_product = (value % denominator) * (uint64_t)numerator;
    whole = tcp_shift_bbr_probe_sat_add_u64(
        whole, remainder_product / denominator);
    if (whole == UINT64_MAX) {
        return UINT64_MAX;
    }
    if ((remainder_product % denominator) != 0U) {
        whole = tcp_shift_bbr_probe_sat_add_u64(whole, 1U);
    }
    return whole;
}

static uint32_t tcp_shift_bbr_probe_min_cwnd_bytes(
    const struct tcp_shift_cc_transport *transport)
{
    uint64_t minimum;

    minimum = (uint64_t)transport->mss_bytes * TCP_SHIFT_BBR_MIN_CWND_PACKETS;
    if (minimum > transport->cwnd_limit_bytes) {
        minimum = transport->cwnd_limit_bytes;
    }
    return (uint32_t)minimum;
}

static uint64_t tcp_shift_bbr_probe_inflight_target(
    const struct tcp_shift_bbr_model *model,
    uint32_t gain_num)
{
    uint64_t bdp;

    bdp = tcp_shift_bbr_bdp_bytes(model->max_bw_bytes_per_sec,
                                  model->min_rtt_ns);
    return tcp_shift_bbr_probe_scale_ceil_u64(
        bdp, gain_num, TCP_SHIFT_BBR_GAIN_DEN);
}

uint32_t tcp_shift_bbr_probe_bw_pacing_gain_num(uint32_t cycle_index)
{
    if (cycle_index >= TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN) {
        return 0U;
    }
    if (cycle_index == 0U) {
        return TCP_SHIFT_BBR_PROBE_BW_UP_GAIN_NUM;
    }
    if (cycle_index == 1U) {
        return TCP_SHIFT_BBR_PROBE_BW_DOWN_GAIN_NUM;
    }
    return TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM;
}

int tcp_shift_bbr_probe_bw_init(
    const struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_bw_state *probe,
    uint32_t initial_cycle_index,
    uint64_t now_ns)
{
    if (model == NULL || probe == NULL ||
        model->mode != TCP_SHIFT_BBR_MODE_PROBE_BW ||
        model->full_bw_reached == 0U || model->max_bw_bytes_per_sec == 0U ||
        model->has_min_rtt == 0U || model->min_rtt_ns == 0U ||
        initial_cycle_index >= TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN ||
        initial_cycle_index == 1U ||
        (model->has_update_time != 0U && now_ns < model->last_update_ns)) {
        return -1;
    }

    probe->cycle_start_ns = now_ns;
    probe->cycle_index = initial_cycle_index;
    probe->initialized = 1U;
    return 0;
}

int tcp_shift_bbr_probe_bw_update_cycle(
    const struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_bw_state *probe,
    const struct tcp_shift_cc_rate_sample *sample,
    uint64_t now_ns,
    uint8_t loss_signal)
{
    uint64_t inflight_target;
    uint32_t gain_num;
    int full_length;
    int advance = 0;
    int sample_valid;

    if (model == NULL || probe == NULL || sample == NULL ||
        model->mode != TCP_SHIFT_BBR_MODE_PROBE_BW ||
        model->full_bw_reached == 0U || model->max_bw_bytes_per_sec == 0U ||
        model->has_min_rtt == 0U || model->min_rtt_ns == 0U ||
        probe->initialized == 0U ||
        probe->cycle_index >= TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN ||
        now_ns < probe->cycle_start_ns ||
        (model->has_update_time != 0U && now_ns < model->last_update_ns)) {
        return -1;
    }

    gain_num = tcp_shift_bbr_probe_bw_pacing_gain_num(probe->cycle_index);
    if (gain_num == 0U) {
        return -1;
    }

    full_length = now_ns > probe->cycle_start_ns &&
                  now_ns - probe->cycle_start_ns > model->min_rtt_ns;
    sample_valid =
        (sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) != 0U;

    if (gain_num == TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM) {
        advance = full_length;
    } else if (gain_num > TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM) {
        inflight_target = tcp_shift_bbr_probe_inflight_target(model, gain_num);
        advance = full_length != 0 &&
                  (loss_signal != 0U ||
                   (sample_valid != 0 &&
                    (uint64_t)sample->prior_inflight_bytes >= inflight_target));
    } else {
        inflight_target = tcp_shift_bbr_probe_inflight_target(
            model, TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM);
        advance = full_length != 0 ||
                  (sample_valid != 0 &&
                   (uint64_t)sample->prior_inflight_bytes <= inflight_target);
    }

    if (advance != 0) {
        probe->cycle_index =
            (probe->cycle_index + 1U) &
            (TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN - 1U);
        probe->cycle_start_ns = now_ns;
    }
    return 0;
}

int tcp_shift_bbr_probe_bw_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_bw_state *probe,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint32_t current_cwnd_bytes,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t base_bdp;
    uint64_t cwnd_target_u64;
    uint64_t grown_cwnd;
    uint64_t pacing_target;
    uint64_t ssthresh_u64;
    uint32_t gain_num;
    uint32_t minimum_cwnd;
    uint32_t cwnd_target;
    uint32_t cwnd;

    if (model == NULL || probe == NULL || transport == NULL || ack == NULL ||
        policy == NULL || model->mode != TCP_SHIFT_BBR_MODE_PROBE_BW ||
        model->full_bw_reached == 0U || model->max_bw_bytes_per_sec == 0U ||
        model->has_min_rtt == 0U || model->min_rtt_ns == 0U ||
        probe->initialized == 0U ||
        probe->cycle_index >= TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN ||
        transport->mss_bytes == 0U || transport->cwnd_limit_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        current_cwnd_bytes == 0U ||
        current_cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    gain_num = tcp_shift_bbr_probe_bw_pacing_gain_num(probe->cycle_index);
    if (gain_num == 0U) {
        return -1;
    }

    minimum_cwnd = tcp_shift_bbr_probe_min_cwnd_bytes(transport);
    base_bdp = tcp_shift_bbr_bdp_bytes(model->max_bw_bytes_per_sec,
                                       model->min_rtt_ns);
    cwnd_target_u64 = tcp_shift_bbr_probe_scale_ceil_u64(
        base_bdp, TCP_SHIFT_BBR_PROBE_BW_CWND_GAIN_NUM,
        TCP_SHIFT_BBR_GAIN_DEN);
    if (cwnd_target_u64 < minimum_cwnd) {
        cwnd_target_u64 = minimum_cwnd;
    }
    if (cwnd_target_u64 > transport->cwnd_limit_bytes) {
        cwnd_target_u64 = transport->cwnd_limit_bytes;
    }
    cwnd_target = (uint32_t)cwnd_target_u64;

    cwnd = current_cwnd_bytes;
    if (ack->acked_bytes != 0U) {
        grown_cwnd = (uint64_t)cwnd + ack->acked_bytes;
        if (grown_cwnd > transport->cwnd_limit_bytes) {
            grown_cwnd = transport->cwnd_limit_bytes;
        }
        cwnd = (uint32_t)grown_cwnd < cwnd_target
                   ? (uint32_t)grown_cwnd
                   : cwnd_target;
    }
    if (cwnd < minimum_cwnd) {
        cwnd = minimum_cwnd;
    }

    ssthresh_u64 = base_bdp;
    if (ssthresh_u64 < minimum_cwnd) {
        ssthresh_u64 = minimum_cwnd;
    }
    if (ssthresh_u64 > transport->cwnd_limit_bytes) {
        ssthresh_u64 = transport->cwnd_limit_bytes;
    }

    pacing_target = tcp_shift_bbr_probe_scale_floor_u64(
        model->max_bw_bytes_per_sec,
        gain_num * TCP_SHIFT_BBR_PACING_MARGIN_NUM,
        TCP_SHIFT_BBR_GAIN_DEN * TCP_SHIFT_BBR_PACING_MARGIN_DEN);

    policy->cwnd_bytes = cwnd;
    policy->ssthresh_bytes = (uint32_t)ssthresh_u64;
    policy->pacing_rate_bytes_per_sec = pacing_target;
    return 0;
}
