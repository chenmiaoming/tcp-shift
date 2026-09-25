#include "cc/bbr_controller.h"

#include <stddef.h>

static int tcp_shift_bbr_controller_valid_transport(
    const struct tcp_shift_cc_transport *transport)
{
    return transport != NULL && transport->mss_bytes != 0U &&
           transport->cwnd_limit_bytes >= transport->mss_bytes;
}

static uint32_t tcp_shift_bbr_controller_add_limit(uint32_t a,
                                                    uint32_t b,
                                                    uint32_t limit)
{
    if (a >= limit || b > limit - a) {
        return limit;
    }
    return a + b;
}

static void tcp_shift_bbr_controller_publish(
    const struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    policy->cwnd_bytes = state->cwnd_bytes;
    policy->ssthresh_bytes = transport->cwnd_limit_bytes;
    policy->pacing_rate_bytes_per_sec = state->pacing_rate_bytes_per_sec;
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
    tcp_shift_bbr_recovery_init(&state->recovery);

    state->initial_cwnd_bytes = init->initial_cwnd_bytes;
    state->cwnd_bytes = init->initial_cwnd_bytes;
    state->pending_probe_loss_bytes = 0U;
    state->delivered_bytes = 0U;
    initial_rate = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(
        init->initial_cwnd_bytes, 0U);
    if (initial_rate == 0U) {
        return -1;
    }
    state->pacing_rate_bytes_per_sec = initial_rate;
    state->initialized = 1U;

    tcp_shift_bbr_controller_publish(state, transport, policy);
    return 0;
}

int tcp_shift_bbr_controller_on_ack(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t current_cwnd;
    uint32_t probe_loss_bytes;
    uint32_t recovery_cwnd = 0U;
    unsigned recovery_owns_cwnd = 0U;
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

    probe_loss_bytes = state->pending_probe_loss_bytes;

    result = tcp_shift_bbr_model_on_ack(
        &state->model, &ack->rate, ack->ack_time_ns);
    if (result < 0) {
        return -1;
    }
    if (ack->rate.delivered_total_bytes > state->delivered_bytes) {
        state->delivered_bytes = ack->rate.delivered_total_bytes;
    }

    current_cwnd = state->cwnd_bytes;
    if (state->recovery.in_recovery != 0U) {
        result = tcp_shift_bbr_recovery_on_ack(
            &state->recovery, current_cwnd, transport->inflight_bytes,
            ack->acked_bytes, 0U, transport->mss_bytes,
            transport->cwnd_limit_bytes, state->model.round_start,
            &recovery_cwnd, &recovery_owns_cwnd);
        if (result < 0) {
            return -1;
        }
        current_cwnd = recovery_cwnd;
    }

    result = tcp_shift_bbr_model_check_drain(&state->model, &ack->rate);
    if (result < 0) {
        return -1;
    }

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
            &state->model, &state->probe, &ack->rate, probe_loss_bytes,
            ack->ack_time_ns);
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

    /* Linux BBR still updates model/gains/pacing while packet conservation
     * owns cwnd, then skips normal cwnd growth for that ACK. Preserve that
     * ordering by overriding only the final cwnd publication. */
    if (recovery_owns_cwnd != 0U) {
        policy->cwnd_bytes = recovery_cwnd;
    }

    state->cwnd_bytes = policy->cwnd_bytes;
    state->pacing_rate_bytes_per_sec = policy->pacing_rate_bytes_per_sec;
    state->pending_probe_loss_bytes = 0U;
    return 0;
}

int tcp_shift_bbr_controller_recovery_enter(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    uint32_t lost_bytes,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t cwnd;

    if (state == NULL || policy == NULL || state->initialized == 0U ||
        lost_bytes == 0U ||
        !tcp_shift_bbr_controller_valid_transport(transport) ||
        state->cwnd_bytes == 0U ||
        state->cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    /* Linux BBR starts a fresh packet-timed Recovery round at the current
     * cumulative delivered marker. Without this reset, a stale round boundary
     * could release packet conservation on the first recovery ACK. */
    state->model.next_round_delivered = state->delivered_bytes;
    state->model.round_start = 0U;
    state->pending_probe_loss_bytes = lost_bytes;

    if (tcp_shift_bbr_recovery_enter(
            &state->recovery, state->cwnd_bytes,
            transport->inflight_bytes, 0U, lost_bytes,
            transport->mss_bytes, transport->cwnd_limit_bytes, &cwnd) < 0) {
        return -1;
    }

    state->cwnd_bytes = cwnd;
    tcp_shift_bbr_controller_publish(state, transport, policy);
    return 0;
}

int tcp_shift_bbr_controller_recovery_exit(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t cwnd;

    if (state == NULL || policy == NULL || state->initialized == 0U ||
        !tcp_shift_bbr_controller_valid_transport(transport) ||
        state->cwnd_bytes == 0U ||
        state->cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    if (tcp_shift_bbr_recovery_exit(
            &state->recovery, state->cwnd_bytes,
            transport->cwnd_limit_bytes, &cwnd) < 0) {
        return -1;
    }

    state->cwnd_bytes = cwnd;
    state->pending_probe_loss_bytes = 0U;
    tcp_shift_bbr_controller_publish(state, transport, policy);
    return 0;
}

int tcp_shift_bbr_controller_on_timeout(
    struct tcp_shift_bbr_controller_state *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_bbr_timeout_observation *timeout,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t cwnd;

    if (state == NULL || timeout == NULL || policy == NULL ||
        state->initialized == 0U ||
        !tcp_shift_bbr_controller_valid_transport(transport) ||
        state->cwnd_bytes == 0U ||
        state->cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    /* Linux BBRv1 bbr_set_state(TCP_CA_Loss) resets only the full-bandwidth
     * baseline and marks the event as a round boundary. It does not reset mode,
     * max_bw/min_rtt filters, full_bw_cnt/full_bw_reached, or pacing. */
    state->model.full_bw_bytes_per_sec = 0U;
    state->model.full_bw_now = 0U;
    state->model.round_start = 1U;
    state->pending_probe_loss_bytes = 0U;

    /* An RTO supersedes fast-recovery packet conservation. A future spurious
     * timeout undo contract can preserve/restore separate prior-cwnd state;
     * do not leave stale Recovery ownership active meanwhile. */
    tcp_shift_bbr_recovery_init(&state->recovery);

    /* Linux tcp_enter_loss() sets cwnd to tcp_packets_in_flight()+1 packet after
     * timeout loss marking. The caller therefore supplies that post-marking
     * in-flight value explicitly instead of reusing raw outstanding bytes. */
    cwnd = tcp_shift_bbr_controller_add_limit(
        timeout->post_loss_inflight_bytes, transport->mss_bytes,
        transport->cwnd_limit_bytes);
    if (cwnd < transport->mss_bytes) {
        cwnd = transport->mss_bytes;
    }
    state->cwnd_bytes = cwnd;

    tcp_shift_bbr_controller_publish(state, transport, policy);
    return 0;
}
