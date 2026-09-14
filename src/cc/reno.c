#include "cc/reno.h"

#include <limits.h>

static uint32_t tcp_shift_reno_add_sat(uint32_t value, uint32_t increment)
{
    if (increment > UINT32_MAX - value) {
        return UINT32_MAX;
    }
    return value + increment;
}

static uint32_t tcp_shift_reno_mul2_sat(uint32_t value)
{
    if (value > UINT32_MAX / 2U) {
        return UINT32_MAX;
    }
    return value * 2U;
}

static uint32_t tcp_shift_reno_max(uint32_t left, uint32_t right)
{
    return left > right ? left : right;
}

static uint32_t tcp_shift_reno_effective_min(
    const struct tcp_shift_reno_state *state,
    const struct tcp_shift_cc_transport *transport)
{
    return tcp_shift_reno_max(state->min_cwnd_bytes, transport->mss_bytes);
}

static void tcp_shift_reno_publish(const struct tcp_shift_reno_state *state,
                                   struct tcp_shift_cc_policy *policy)
{
    policy->cwnd_bytes = state->cwnd_bytes;
    policy->ssthresh_bytes = state->ssthresh_bytes;
    policy->pacing_rate_bytes_per_sec = 0U;
}

static void tcp_shift_reno_reduce_ssthresh(
    struct tcp_shift_reno_state *state,
    const struct tcp_shift_cc_transport *transport)
{
    uint32_t effective_window = state->cwnd_bytes;
    uint32_t floor = tcp_shift_reno_mul2_sat(transport->mss_bytes);
    uint32_t reduced;

    if (transport->send_window_bytes < effective_window) {
        effective_window = transport->send_window_bytes;
    }
    reduced = effective_window / 2U;
    state->ssthresh_bytes = tcp_shift_reno_max(reduced, floor);
}

static int tcp_shift_reno_init(void *opaque_state,
                               const struct tcp_shift_cc_transport *transport,
                               const struct tcp_shift_cc_init *init,
                               struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_reno_state *state = opaque_state;
    uint32_t ssthresh_floor = tcp_shift_reno_mul2_sat(transport->mss_bytes);

    if (init->initial_cwnd_bytes < transport->mss_bytes ||
        init->min_cwnd_bytes < transport->mss_bytes ||
        init->initial_cwnd_bytes < init->min_cwnd_bytes ||
        init->initial_ssthresh_bytes < ssthresh_floor) {
        return -1;
    }

    state->cwnd_bytes = init->initial_cwnd_bytes;
    state->ssthresh_bytes = init->initial_ssthresh_bytes;
    state->ca_acked_bytes = 0U;
    state->min_cwnd_bytes = init->min_cwnd_bytes;
    tcp_shift_reno_publish(state, policy);
    return 0;
}

static int tcp_shift_reno_on_ack(void *opaque_state,
                                 const struct tcp_shift_cc_transport *transport,
                                 const struct tcp_shift_cc_ack *ack,
                                 struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_reno_state *state = opaque_state;
    uint32_t effective_min = tcp_shift_reno_effective_min(state, transport);

    if (state->cwnd_bytes < effective_min) {
        state->cwnd_bytes = effective_min;
    }

    if (state->cwnd_bytes < state->ssthresh_bytes) {
        uint32_t slow_start_limit = tcp_shift_reno_mul2_sat(transport->mss_bytes);
        uint32_t increase = ack->acked_bytes;

        if (increase > slow_start_limit) {
            increase = slow_start_limit;
        }
        state->cwnd_bytes = tcp_shift_reno_add_sat(state->cwnd_bytes, increase);
    } else {
        state->ca_acked_bytes =
            tcp_shift_reno_add_sat(state->ca_acked_bytes, ack->acked_bytes);
        if (state->ca_acked_bytes >= state->cwnd_bytes) {
            state->ca_acked_bytes -= state->cwnd_bytes;
            state->cwnd_bytes =
                tcp_shift_reno_add_sat(state->cwnd_bytes, transport->mss_bytes);
        }
    }

    tcp_shift_reno_publish(state, policy);
    return 0;
}

static int tcp_shift_reno_on_loss(void *opaque_state,
                                  const struct tcp_shift_cc_transport *transport,
                                  const struct tcp_shift_cc_loss *loss,
                                  struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_reno_state *state = opaque_state;
    uint32_t effective_min;

    (void)loss;
    tcp_shift_reno_reduce_ssthresh(state, transport);
    effective_min = tcp_shift_reno_effective_min(state, transport);
    state->cwnd_bytes = tcp_shift_reno_max(state->ssthresh_bytes, effective_min);
    state->ca_acked_bytes = 0U;
    tcp_shift_reno_publish(state, policy);
    return 0;
}

static int tcp_shift_reno_on_timeout(void *opaque_state,
                                     const struct tcp_shift_cc_transport *transport,
                                     struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_reno_state *state = opaque_state;

    tcp_shift_reno_reduce_ssthresh(state, transport);
    state->cwnd_bytes = tcp_shift_reno_effective_min(state, transport);
    state->ca_acked_bytes = 0U;
    tcp_shift_reno_publish(state, policy);
    return 0;
}

const struct tcp_shift_cc_ops tcp_shift_reno_ops = {
    .name = "reno",
    .state_size = sizeof(struct tcp_shift_reno_state),
    .init = tcp_shift_reno_init,
    .on_ack = tcp_shift_reno_on_ack,
    .on_loss = tcp_shift_reno_on_loss,
    .on_timeout = tcp_shift_reno_on_timeout,
};
