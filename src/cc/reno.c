#include "cc/reno.h"
#include "cc/registry.h"

#include <limits.h>

static uint32_t tcp_shift_reno_add_sat(uint32_t value, uint32_t increment)
{
    if (increment > UINT32_MAX - value) {
        return UINT32_MAX;
    }
    return value + increment;
}

static uint32_t tcp_shift_reno_min(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

static uint32_t tcp_shift_reno_max(uint32_t left, uint32_t right)
{
    return left > right ? left : right;
}

static uint32_t tcp_shift_reno_add_cap(uint32_t value,
                                       uint32_t increment,
                                       uint32_t limit)
{
    uint32_t result = tcp_shift_reno_add_sat(value, increment);

    return tcp_shift_reno_min(result, limit);
}

static uint32_t tcp_shift_reno_mul2_cap(uint32_t value, uint32_t limit)
{
    if (value > limit / 2U) {
        return limit;
    }
    return value * 2U;
}

static uint32_t tcp_shift_reno_effective_min(
    const struct tcp_shift_reno_state *state,
    const struct tcp_shift_cc_transport *transport)
{
    uint32_t effective = tcp_shift_reno_max(state->min_cwnd_bytes,
                                             transport->mss_bytes);

    return tcp_shift_reno_min(effective, transport->cwnd_limit_bytes);
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
    uint32_t floor = tcp_shift_reno_mul2_cap(transport->mss_bytes,
                                             transport->cwnd_limit_bytes);
    uint32_t reduced;

    if (transport->send_window_bytes < effective_window) {
        effective_window = transport->send_window_bytes;
    }
    reduced = effective_window / 2U;
    state->ssthresh_bytes = tcp_shift_reno_min(
        tcp_shift_reno_max(reduced, floor), transport->cwnd_limit_bytes);
}

static int tcp_shift_reno_init(void *opaque_state,
                               const struct tcp_shift_cc_transport *transport,
                               const struct tcp_shift_cc_init *init,
                               struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_reno_state *state = opaque_state;
    uint32_t ssthresh_floor = tcp_shift_reno_mul2_cap(
        transport->mss_bytes, transport->cwnd_limit_bytes);

    if (init->initial_cwnd_bytes < transport->mss_bytes ||
        init->min_cwnd_bytes < transport->mss_bytes ||
        init->initial_cwnd_bytes < init->min_cwnd_bytes ||
        init->initial_ssthresh_bytes < ssthresh_floor ||
        init->initial_cwnd_bytes > transport->cwnd_limit_bytes ||
        init->initial_ssthresh_bytes > transport->cwnd_limit_bytes ||
        init->min_cwnd_bytes > transport->cwnd_limit_bytes) {
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
        uint32_t slow_start_limit = tcp_shift_reno_mul2_cap(
            transport->mss_bytes, transport->cwnd_limit_bytes);
        uint32_t increase = ack->acked_bytes;

        if (increase > slow_start_limit) {
            increase = slow_start_limit;
        }
        state->cwnd_bytes = tcp_shift_reno_add_cap(
            state->cwnd_bytes, increase, transport->cwnd_limit_bytes);
    } else {
        state->ca_acked_bytes =
            tcp_shift_reno_add_sat(state->ca_acked_bytes, ack->acked_bytes);
        if (state->ca_acked_bytes >= state->cwnd_bytes) {
            state->ca_acked_bytes -= state->cwnd_bytes;
            state->cwnd_bytes = tcp_shift_reno_add_cap(
                state->cwnd_bytes, transport->mss_bytes,
                transport->cwnd_limit_bytes);
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
    state->cwnd_bytes = tcp_shift_reno_min(
        tcp_shift_reno_max(state->ssthresh_bytes, effective_min),
        transport->cwnd_limit_bytes);
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

static int tcp_shift_fixed_pacing_publish(int result,
                                          struct tcp_shift_cc_policy *policy)
{
    if (result != 0) {
        return result;
    }
    policy->pacing_rate_bytes_per_sec =
        TCP_SHIFT_FIXED_PACING_RENO_RATE_BYTES_PER_SEC;
    return 0;
}

static int tcp_shift_fixed_pacing_reno_init(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_fixed_pacing_publish(
        tcp_shift_reno_init(opaque_state, transport, init, policy), policy);
}

static int tcp_shift_fixed_pacing_reno_on_ack(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_fixed_pacing_publish(
        tcp_shift_reno_on_ack(opaque_state, transport, ack, policy), policy);
}

static int tcp_shift_fixed_pacing_reno_on_loss(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_fixed_pacing_publish(
        tcp_shift_reno_on_loss(opaque_state, transport, loss, policy), policy);
}

static int tcp_shift_fixed_pacing_reno_on_timeout(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_fixed_pacing_publish(
        tcp_shift_reno_on_timeout(opaque_state, transport, policy), policy);
}

const struct tcp_shift_cc_ops tcp_shift_reno_ops = {
    .name = "reno",
    .state_size = sizeof(struct tcp_shift_reno_state),
    .init = tcp_shift_reno_init,
    .on_ack = tcp_shift_reno_on_ack,
    .on_loss = tcp_shift_reno_on_loss,
    .on_timeout = tcp_shift_reno_on_timeout,
};

const struct tcp_shift_cc_ops tcp_shift_fixed_pacing_reno_ops = {
    .name = "fixed-pacing-reno",
    .state_size = sizeof(struct tcp_shift_reno_state),
    .init = tcp_shift_fixed_pacing_reno_init,
    .on_ack = tcp_shift_fixed_pacing_reno_on_ack,
    .on_loss = tcp_shift_fixed_pacing_reno_on_loss,
    .on_timeout = tcp_shift_fixed_pacing_reno_on_timeout,
};

static int tcp_shift_cc_name_equal(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return 0;
    }
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == *right;
}

const struct tcp_shift_cc_ops *tcp_shift_cc_find_ops(const char *name)
{
    if (tcp_shift_cc_name_equal(name, tcp_shift_reno_ops.name)) {
        return &tcp_shift_reno_ops;
    }
    if (tcp_shift_cc_name_equal(name, tcp_shift_cubic_ops.name)) {
        return &tcp_shift_cubic_ops;
    }
    return NULL;
}

const struct tcp_shift_cc_ops *tcp_shift_cc_default_ops(void)
{
    return &tcp_shift_reno_ops;
}
