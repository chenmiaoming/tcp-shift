#include "cc/bbr_controller.h"

#include <stddef.h>

static int tcp_shift_bbr_controller_valid_transport(
    const struct tcp_shift_cc_transport *transport)
{
    return transport != NULL && transport->mss_bytes != 0U &&
           transport->cwnd_limit_bytes >= transport->mss_bytes;
}

int tcp_shift_bbr_controller_init(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    uint32_t cycle_seed,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t initial_rate;

    if (state == NULL || init == NULL || policy == NULL ||
        !tcp_shift_bbr_controller_valid_transport(transport) ||
        init->initial_cwnd_bytes == 0U ||
        init->initial_cwnd_bytes > transport->cwnd_limit_bytes ||
        init->min_cwnd_bytes == 0U ||
        init->min_cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    tcp_shift_bbr_model_init(&state->model);
    tcp_shift_bbr_probe_state_init(&state->probe, cycle_seed);

    state->initial_cwnd_bytes = init->initial_cwnd_bytes;
    state->cwnd_bytes = init->initial_cwnd_bytes;
    initial_rate = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(
        init->initial_cwnd_bytes, 0U);
    if (initial_rate == 0U) {
        return -1;
    }
    state->pacing_rate_bytes_per_sec = initial_rate;
    state->initialized = 1U;

    policy->cwnd_bytes = state->cwnd_bytes;
    policy->ssthresh_bytes = transport->cwnd_limit_bytes;
    policy->pacing_rate_bytes_per_sec = state->pacing_rate_bytes_per_sec;
    return 0;
}

int tcp_shift_bbr_controller_on_ack(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t current_cwnd;
    int probe_rtt_result;
    int result;

    if (state == NULL || ack == NULL || policy == NULL ||
        state->initialized == 0U || ack->acked_bytes == 0U ||
        ack->ack_time_ns == 0U ||
        !tcp_shift_bbr_controller_valid_transport(transport) ||
        state->cwnd_bytes == 0U ||
        state->cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    result = tcp_shift_bbr_model_on_ack(
        &state->model, &ack->rate, ack->ack_time_ns);
    if (result < 0) {
        return -1;
    }

    result = tcp_shift_bbr_model_check_drain(&state->model, &ack->rate);
    if (result < 0) {
        return -1;
    }

    current_cwnd = state->cwnd_bytes;
    probe_rtt_result = tcp_shift_bbr_probe_rtt_update(
        &state->model, &state->probe, transport, &ack->rate,
        current_cwnd, ack->ack_time_ns);
    if (probe_rtt_result < 0) {
        return -1;
    }
    if (probe_rtt_result > 0) {
        current_cwnd = tcp_shift_bbr_probe_rtt_restore_cwnd_bytes(
            &state->probe, current_cwnd, transport->cwnd_limit_bytes);
        if (current_cwnd == 0U) {
            return -1;
        }
    }

    if (state->model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW) {
        result = tcp_shift_bbr_probe_bw_update(
            &state->model, &state->probe, &ack->rate, ack->ack_time_ns);
        if (result < 0) {
            return -1;
        }
    }

    switch (state->model.mode) {
    case TCP_SHIFT_BBR_MODE_STARTUP:
        result = tcp_shift_bbr_startup_policy(
            &state->model, transport, ack,
            state->initial_cwnd_bytes, current_cwnd,
            state->pacing_rate_bytes_per_sec, policy);
        break;
    case TCP_SHIFT_BBR_MODE_DRAIN:
        result = tcp_shift_bbr_drain_policy(
            &state->model, transport, ack, current_cwnd, policy);
        break;
    case TCP_SHIFT_BBR_MODE_PROBE_BW:
        result = tcp_shift_bbr_probe_bw_policy(
            &state->model, &state->probe, transport, ack,
            current_cwnd, policy);
        break;
    case TCP_SHIFT_BBR_MODE_PROBE_RTT:
        result = tcp_shift_bbr_probe_rtt_policy(
            &state->model, &state->probe, transport, policy);
        break;
    default:
        return -1;
    }
    if (result < 0 || policy->cwnd_bytes == 0U ||
        policy->cwnd_bytes > transport->cwnd_limit_bytes ||
        policy->pacing_rate_bytes_per_sec == 0U) {
        return -1;
    }

    state->cwnd_bytes = policy->cwnd_bytes;
    state->pacing_rate_bytes_per_sec = policy->pacing_rate_bytes_per_sec;
    return 0;
}
