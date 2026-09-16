#include "cc/bbr_probe.h"

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
    uint64_t scaled;
    uint64_t remainder_product;

    if (denominator == 0U) {
        return UINT64_MAX;
    }

    scaled = tcp_shift_bbr_probe_sat_mul_u64(value / denominator, numerator);
    if (scaled == UINT64_MAX) {
        return UINT64_MAX;
    }

    remainder_product = (value % denominator) * (uint64_t)numerator;
    scaled = tcp_shift_bbr_probe_sat_add_u64(
        scaled, remainder_product / denominator);
    if (scaled == UINT64_MAX) {
        return UINT64_MAX;
    }
    if ((remainder_product % denominator) != 0U) {
        scaled = tcp_shift_bbr_probe_sat_add_u64(scaled, 1U);
    }
    return scaled;
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

static void tcp_shift_bbr_probe_bw_reset_cycle(
    struct tcp_shift_bbr_probe_state *state,
    uint64_t now_ns)
{
    uint32_t offset;

    offset = state->cycle_seed % TCP_SHIFT_BBR_PROBE_BW_SEED_SPAN;
    state->cycle_index =
        (uint8_t)((TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN - offset) &
                  (TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN - 1U));
    state->cycle_start_ns = now_ns;
    state->cycle_started = 1U;
}

void tcp_shift_bbr_probe_state_init(struct tcp_shift_bbr_probe_state *state,
                                    uint32_t cycle_seed)
{
    if (state == NULL) {
        return;
    }

    state->cycle_start_ns = 0U;
    state->probe_rtt_done_stamp_ns = 0U;
    state->probe_rtt_prior_cwnd_bytes = 0U;
    state->cycle_index = 0U;
    state->cycle_seed =
        (uint8_t)(cycle_seed % TCP_SHIFT_BBR_PROBE_BW_SEED_SPAN);
    state->cycle_started = 0U;
    state->probe_rtt_round_done = 0U;
}

uint32_t tcp_shift_bbr_probe_bw_pacing_gain_num(
    const struct tcp_shift_bbr_probe_state *state)
{
    if (state == NULL) {
        return 0U;
    }
    if (state->cycle_index == 0U) {
        return TCP_SHIFT_BBR_PROBE_BW_UP_GAIN_NUM;
    }
    if (state->cycle_index == 1U) {
        return TCP_SHIFT_BBR_PROBE_BW_DOWN_GAIN_NUM;
    }
    return TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM;
}

uint64_t tcp_shift_bbr_probe_bw_pacing_rate_bytes_per_sec(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_state *state)
{
    uint32_t gain;

    if (model == NULL || state == NULL) {
        return 0U;
    }

    gain = tcp_shift_bbr_probe_bw_pacing_gain_num(state);
    return tcp_shift_bbr_probe_scale_floor_u64(
        model->max_bw_bytes_per_sec,
        gain * TCP_SHIFT_BBR_PACING_MARGIN_NUM,
        TCP_SHIFT_BBR_GAIN_DEN * TCP_SHIFT_BBR_PACING_MARGIN_DEN);
}

int tcp_shift_bbr_probe_bw_update(
    struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_rate_sample *sample,
    uint64_t now_ns)
{
    uint64_t bdp;
    uint64_t phase_target;
    uint32_t gain;
    int full_length;
    int advance = 0;

    if (model == NULL || state == NULL || sample == NULL) {
        return -1;
    }
    if (model->mode != TCP_SHIFT_BBR_MODE_PROBE_BW) {
        return 0;
    }
    if (now_ns == 0U) {
        return 0;
    }
    if (state->cycle_started == 0U) {
        tcp_shift_bbr_probe_bw_reset_cycle(state, now_ns);
        return 0;
    }
    if (now_ns < state->cycle_start_ns) {
        return -1;
    }
    if (model->max_bw_bytes_per_sec == 0U || model->has_min_rtt == 0U ||
        model->min_rtt_ns == 0U) {
        return 0;
    }

    full_length = now_ns - state->cycle_start_ns >= model->min_rtt_ns;
    bdp = tcp_shift_bbr_bdp_bytes(model->max_bw_bytes_per_sec,
                                  model->min_rtt_ns);
    if (bdp == 0U) {
        return 0;
    }

    gain = tcp_shift_bbr_probe_bw_pacing_gain_num(state);
    if (gain > TCP_SHIFT_BBR_GAIN_DEN) {
        phase_target = tcp_shift_bbr_probe_scale_ceil_u64(
            bdp, gain, TCP_SHIFT_BBR_GAIN_DEN);
        advance = full_length != 0 &&
                  (((sample->flags & TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED) !=
                    0U) ||
                   (uint64_t)sample->prior_inflight_bytes >= phase_target);
    } else if (gain < TCP_SHIFT_BBR_GAIN_DEN) {
        advance = full_length != 0 ||
                  (uint64_t)sample->prior_inflight_bytes <= bdp;
    } else {
        advance = full_length;
    }

    if (advance != 0) {
        state->cycle_index =
            (uint8_t)((state->cycle_index + 1U) &
                      (TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN - 1U));
        state->cycle_start_ns = now_ns;
    }
    return 0;
}

int tcp_shift_bbr_probe_bw_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint32_t current_cwnd_bytes,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t bdp;
    uint64_t target;
    uint64_t grown;
    uint32_t minimum;
    uint32_t cwnd;
    uint32_t ssthresh;

    if (model == NULL || state == NULL || transport == NULL || ack == NULL ||
        policy == NULL || model->mode != TCP_SHIFT_BBR_MODE_PROBE_BW ||
        model->full_bw_reached == 0U || model->max_bw_bytes_per_sec == 0U ||
        model->has_min_rtt == 0U || model->min_rtt_ns == 0U ||
        transport->mss_bytes == 0U || transport->cwnd_limit_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        current_cwnd_bytes == 0U ||
        current_cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    minimum = tcp_shift_bbr_probe_min_cwnd_bytes(transport);
    bdp = tcp_shift_bbr_bdp_bytes(model->max_bw_bytes_per_sec,
                                  model->min_rtt_ns);
    target = tcp_shift_bbr_probe_scale_ceil_u64(
        bdp, TCP_SHIFT_BBR_CWND_GAIN_NUM, TCP_SHIFT_BBR_GAIN_DEN);
    if (target < minimum) {
        target = minimum;
    }
    if (target > transport->cwnd_limit_bytes) {
        target = transport->cwnd_limit_bytes;
    }

    cwnd = current_cwnd_bytes;
    if (ack->acked_bytes != 0U) {
        grown = (uint64_t)cwnd + ack->acked_bytes;
        if (grown > transport->cwnd_limit_bytes) {
            grown = transport->cwnd_limit_bytes;
        }
        cwnd = grown < target ? (uint32_t)grown : (uint32_t)target;
    }
    if (cwnd < minimum) {
        cwnd = minimum;
    }

    if (bdp < minimum) {
        bdp = minimum;
    }
    if (bdp > transport->cwnd_limit_bytes) {
        bdp = transport->cwnd_limit_bytes;
    }
    ssthresh = (uint32_t)bdp;

    policy->cwnd_bytes = cwnd;
    policy->ssthresh_bytes = ssthresh;
    policy->pacing_rate_bytes_per_sec =
        tcp_shift_bbr_probe_bw_pacing_rate_bytes_per_sec(model, state);
    return 0;
}

int tcp_shift_bbr_probe_rtt_update(
    struct tcp_shift_bbr_model *model,
    struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_rate_sample *sample,
    uint32_t current_cwnd_bytes,
    uint64_t now_ns)
{
    uint32_t minimum;

    if (model == NULL || state == NULL || transport == NULL || sample == NULL ||
        transport->mss_bytes == 0U || transport->cwnd_limit_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        current_cwnd_bytes == 0U ||
        current_cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    if (model->mode != TCP_SHIFT_BBR_MODE_PROBE_RTT &&
        model->min_rtt_expired != 0U) {
        if (current_cwnd_bytes > state->probe_rtt_prior_cwnd_bytes) {
            state->probe_rtt_prior_cwnd_bytes = current_cwnd_bytes;
        }
        state->probe_rtt_done_stamp_ns = 0U;
        state->probe_rtt_round_done = 0U;
        state->cycle_started = 0U;
        model->mode = TCP_SHIFT_BBR_MODE_PROBE_RTT;
    }

    if (model->mode != TCP_SHIFT_BBR_MODE_PROBE_RTT) {
        return 0;
    }

    minimum = tcp_shift_bbr_probe_min_cwnd_bytes(transport);
    if (state->probe_rtt_done_stamp_ns == 0U) {
        if (sample->prior_inflight_bytes <= minimum) {
            state->probe_rtt_done_stamp_ns = tcp_shift_bbr_probe_sat_add_u64(
                now_ns, TCP_SHIFT_BBR_PROBE_RTT_DURATION_NS);
            state->probe_rtt_round_done = 0U;
            model->next_round_delivered = sample->delivered_total_bytes;
        }
        return 0;
    }

    if (model->round_start != 0U) {
        state->probe_rtt_round_done = 1U;
    }
    if (state->probe_rtt_round_done == 0U ||
        now_ns < state->probe_rtt_done_stamp_ns) {
        return 0;
    }

    model->min_rtt_stamp_ns = now_ns;
    model->min_rtt_expired = 0U;
    state->probe_rtt_done_stamp_ns = 0U;
    state->probe_rtt_round_done = 0U;

    if (model->full_bw_reached != 0U) {
        model->mode = TCP_SHIFT_BBR_MODE_PROBE_BW;
        tcp_shift_bbr_probe_bw_reset_cycle(state, now_ns);
    } else {
        model->mode = TCP_SHIFT_BBR_MODE_STARTUP;
        state->cycle_started = 0U;
    }
    return 1;
}

int tcp_shift_bbr_probe_rtt_policy(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_bbr_probe_state *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t minimum;

    if (model == NULL || state == NULL || transport == NULL || policy == NULL ||
        model->mode != TCP_SHIFT_BBR_MODE_PROBE_RTT ||
        transport->mss_bytes == 0U || transport->cwnd_limit_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes) {
        return -1;
    }

    minimum = tcp_shift_bbr_probe_min_cwnd_bytes(transport);
    policy->cwnd_bytes = minimum;
    policy->ssthresh_bytes = minimum;
    policy->pacing_rate_bytes_per_sec = tcp_shift_bbr_probe_scale_floor_u64(
        model->max_bw_bytes_per_sec,
        TCP_SHIFT_BBR_PROBE_BW_CRUISE_GAIN_NUM *
            TCP_SHIFT_BBR_PACING_MARGIN_NUM,
        TCP_SHIFT_BBR_GAIN_DEN * TCP_SHIFT_BBR_PACING_MARGIN_DEN);
    return 0;
}

uint32_t tcp_shift_bbr_probe_rtt_restore_cwnd_bytes(
    const struct tcp_shift_bbr_probe_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t cwnd_limit_bytes)
{
    uint32_t restored;

    if (state == NULL || cwnd_limit_bytes == 0U) {
        return 0U;
    }

    restored = current_cwnd_bytes;
    if (state->probe_rtt_prior_cwnd_bytes > restored) {
        restored = state->probe_rtt_prior_cwnd_bytes;
    }
    if (restored > cwnd_limit_bytes) {
        restored = cwnd_limit_bytes;
    }
    return restored;
}
