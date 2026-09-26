#include "cc/bbr.h"

#include <stddef.h>

static uint64_t tcp_shift_bbr_drain_sat_add_u64(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static uint64_t tcp_shift_bbr_drain_sat_mul_u64(uint64_t a, uint64_t b)
{
    if (a == 0U || b == 0U) {
        return 0U;
    }
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

static uint64_t tcp_shift_bbr_drain_scale_floor_u64(uint64_t value,
                                                     uint32_t numerator,
                                                     uint32_t denominator)
{
    uint64_t whole;
    uint64_t remainder_product;

    if (denominator == 0U) {
        return UINT64_MAX;
    }

    whole = tcp_shift_bbr_drain_sat_mul_u64(value / denominator, numerator);
    if (whole == UINT64_MAX) {
        return UINT64_MAX;
    }

    remainder_product = (value % denominator) * (uint64_t)numerator;
    return tcp_shift_bbr_drain_sat_add_u64(
        whole, remainder_product / denominator);
}

static uint32_t tcp_shift_bbr_drain_min_cwnd_bytes(
    const struct tcp_shift_cc_transport *transport)
{
    uint64_t minimum;

    minimum = (uint64_t)transport->mss_bytes * TCP_SHIFT_BBR_MIN_CWND_PACKETS;
    if (minimum > transport->cwnd_limit_bytes) {
        minimum = transport->cwnd_limit_bytes;
    }
    return (uint32_t)minimum;
}

uint64_t tcp_shift_bbr_drain_pacing_rate_bytes_per_sec(
    uint64_t bandwidth_bytes_per_sec)
{
    return tcp_shift_bbr_drain_scale_floor_u64(
        bandwidth_bytes_per_sec,
        TCP_SHIFT_BBR_DRAIN_GAIN_NUM * TCP_SHIFT_BBR_PACING_MARGIN_NUM,
        TCP_SHIFT_BBR_GAIN_DEN * TCP_SHIFT_BBR_PACING_MARGIN_DEN);
}

int tcp_shift_bbr_model_check_drain(
    struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_rate_sample *sample)
{
    uint64_t drain_target;

    if (model == NULL || sample == NULL) {
        return -1;
    }

    if (model->mode == TCP_SHIFT_BBR_MODE_STARTUP &&
        model->full_bw_reached != 0U) {
        model->mode = TCP_SHIFT_BBR_MODE_DRAIN;
    }

    if (model->mode != TCP_SHIFT_BBR_MODE_DRAIN ||
        (sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) == 0U ||
        model->max_bw_bytes_per_sec == 0U || model->has_min_rtt == 0U ||
        model->min_rtt_ns == 0U) {
        return 0;
    }

    drain_target = tcp_shift_bbr_bdp_bytes(model->max_bw_bytes_per_sec,
                                           model->min_rtt_ns);
    if (drain_target == 0U) {
        return 0;
    }

    if ((uint64_t)sample->prior_inflight_bytes <= drain_target) {
        model->mode = TCP_SHIFT_BBR_MODE_PROBE_BW;
    }
    return 0;
}

int tcp_shift_bbr_drain_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint32_t current_cwnd_bytes,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t grown_cwnd;
    uint64_t drain_target;
    uint32_t minimum_cwnd;
    uint32_t cwnd_target;
    uint32_t cwnd;
    uint32_t ssthresh;

    if (model == NULL || transport == NULL || ack == NULL || policy == NULL ||
        model->mode != TCP_SHIFT_BBR_MODE_DRAIN ||
        model->full_bw_reached == 0U || model->max_bw_bytes_per_sec == 0U ||
        model->has_min_rtt == 0U || model->min_rtt_ns == 0U ||
        transport->mss_bytes == 0U || transport->cwnd_limit_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        current_cwnd_bytes == 0U ||
        current_cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    minimum_cwnd = tcp_shift_bbr_drain_min_cwnd_bytes(transport);
    cwnd_target = tcp_shift_bbr_startup_cwnd_target_bytes(
        model->max_bw_bytes_per_sec,
        model->min_rtt_ns,
        transport->mss_bytes,
        transport->cwnd_limit_bytes,
        minimum_cwnd);
    if (cwnd_target == 0U) {
        return -1;
    }
    if (cwnd_target < transport->cwnd_limit_bytes) {
        uint32_t aggregation =
            tcp_shift_bbr_ack_aggregation_cwnd_bytes(model);

        if (aggregation > transport->cwnd_limit_bytes - cwnd_target) {
            cwnd_target = transport->cwnd_limit_bytes;
        } else {
            cwnd_target += aggregation;
        }
    }

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
    if (cwnd > transport->cwnd_limit_bytes) {
        cwnd = transport->cwnd_limit_bytes;
    }

    drain_target = tcp_shift_bbr_bdp_bytes(model->max_bw_bytes_per_sec,
                                           model->min_rtt_ns);
    if (drain_target < minimum_cwnd) {
        drain_target = minimum_cwnd;
    }
    if (drain_target > transport->cwnd_limit_bytes) {
        drain_target = transport->cwnd_limit_bytes;
    }
    ssthresh = (uint32_t)drain_target;

    policy->cwnd_bytes = cwnd;
    policy->ssthresh_bytes = ssthresh;
    policy->pacing_rate_bytes_per_sec =
        tcp_shift_bbr_drain_pacing_rate_bytes_per_sec(
            model->max_bw_bytes_per_sec);
    return 0;
}
