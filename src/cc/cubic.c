#include "cc/cubic.h"

#include <limits.h>
#include <stddef.h>

#define TCP_SHIFT_NSEC_PER_SEC UINT64_C(1000000000)

static uint64_t tcp_shift_cubic_add_sat_u64(uint64_t left, uint64_t right)
{
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

static uint64_t tcp_shift_cubic_mul_sat_u64(uint64_t left, uint64_t right)
{
    if (left == 0U || right == 0U) {
        return 0U;
    }
    return left > UINT64_MAX / right ? UINT64_MAX : left * right;
}

static uint32_t tcp_shift_cubic_min_u32(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

static uint32_t tcp_shift_cubic_max_u32(uint32_t left, uint32_t right)
{
    return left > right ? left : right;
}

static uint64_t tcp_shift_cubic_min_u64(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

static uint64_t tcp_shift_cubic_max_u64(uint64_t left, uint64_t right)
{
    return left > right ? left : right;
}

static uint32_t tcp_shift_cubic_mul2_cap(uint32_t value, uint32_t limit)
{
    if (value > limit / 2U) {
        return limit;
    }
    return value * 2U;
}

static uint64_t tcp_shift_cubic_q16_from_bytes(uint32_t bytes,
                                                uint32_t mss_bytes)
{
    if (mss_bytes == 0U) {
        return 0U;
    }
    return ((uint64_t)bytes << TCP_SHIFT_CUBIC_Q_SHIFT) / mss_bytes;
}

static uint32_t tcp_shift_cubic_bytes_from_q16(uint64_t value_q16,
                                                uint32_t mss_bytes,
                                                uint32_t limit_bytes)
{
    uint64_t bytes;

    if (mss_bytes == 0U || value_q16 == 0U) {
        return 0U;
    }
    if (value_q16 > UINT64_MAX / mss_bytes) {
        return limit_bytes;
    }
    bytes = (value_q16 * mss_bytes) >> TCP_SHIFT_CUBIC_Q_SHIFT;
    return bytes > limit_bytes ? limit_bytes : (uint32_t)bytes;
}

static uint64_t tcp_shift_cubic_rescale_q16(uint64_t value_q16,
                                             uint32_t old_mss,
                                             uint32_t new_mss)
{
    if (value_q16 == 0U || old_mss == new_mss) {
        return value_q16;
    }
    if (new_mss == 0U || value_q16 > UINT64_MAX / old_mss) {
        return UINT64_MAX;
    }
    return (value_q16 * old_mss) / new_mss;
}

static int tcp_shift_cubic_sync_mss(struct tcp_shift_cubic_model *model,
                                     uint32_t mss_bytes)
{
    uint32_t old_mss;

    if (model == NULL || mss_bytes == 0U || model->mss_bytes == 0U) {
        return -1;
    }
    if (model->mss_bytes == mss_bytes) {
        return 0;
    }

    old_mss = model->mss_bytes;
    model->cwnd_q16 =
        tcp_shift_cubic_rescale_q16(model->cwnd_q16, old_mss, mss_bytes);
    model->ssthresh_q16 =
        tcp_shift_cubic_rescale_q16(model->ssthresh_q16, old_mss, mss_bytes);
    model->w_max_q16 =
        tcp_shift_cubic_rescale_q16(model->w_max_q16, old_mss, mss_bytes);
    model->w_est_q16 =
        tcp_shift_cubic_rescale_q16(model->w_est_q16, old_mss, mss_bytes);
    model->cwnd_prior_q16 = tcp_shift_cubic_rescale_q16(
        model->cwnd_prior_q16, old_mss, mss_bytes);
    model->cwnd_epoch_q16 = tcp_shift_cubic_rescale_q16(
        model->cwnd_epoch_q16, old_mss, mss_bytes);
    model->last_target_q16 = tcp_shift_cubic_rescale_q16(
        model->last_target_q16, old_mss, mss_bytes);
    model->mss_bytes = mss_bytes;
    return 0;
}

static uint32_t tcp_shift_cubic_effective_min_bytes(
    const struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport)
{
    uint32_t minimum = tcp_shift_cubic_max_u32(model->min_cwnd_bytes,
                                                transport->mss_bytes);

    return tcp_shift_cubic_min_u32(minimum, transport->cwnd_limit_bytes);
}

static uint64_t tcp_shift_cubic_limit_q16(
    const struct tcp_shift_cc_transport *transport)
{
    return tcp_shift_cubic_q16_from_bytes(transport->cwnd_limit_bytes,
                                          transport->mss_bytes);
}

static void tcp_shift_cubic_normalize(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport)
{
    uint64_t limit_q16 = tcp_shift_cubic_limit_q16(transport);
    uint64_t minimum_q16 = tcp_shift_cubic_q16_from_bytes(
        tcp_shift_cubic_effective_min_bytes(model, transport),
        transport->mss_bytes);

    model->cwnd_q16 = tcp_shift_cubic_max_u64(model->cwnd_q16, minimum_q16);
    model->cwnd_q16 = tcp_shift_cubic_min_u64(model->cwnd_q16, limit_q16);
    model->ssthresh_q16 = tcp_shift_cubic_min_u64(model->ssthresh_q16,
                                                   limit_q16);
}

static void tcp_shift_cubic_publish(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t minimum;
    uint32_t cwnd;
    uint32_t ssthresh;

    tcp_shift_cubic_normalize(model, transport);
    minimum = tcp_shift_cubic_effective_min_bytes(model, transport);
    cwnd = tcp_shift_cubic_bytes_from_q16(model->cwnd_q16,
                                           transport->mss_bytes,
                                           transport->cwnd_limit_bytes);
    ssthresh = tcp_shift_cubic_bytes_from_q16(model->ssthresh_q16,
                                               transport->mss_bytes,
                                               transport->cwnd_limit_bytes);
    if (cwnd < minimum) {
        cwnd = minimum;
        model->cwnd_q16 = tcp_shift_cubic_q16_from_bytes(
            cwnd, transport->mss_bytes);
    }
    if (ssthresh == 0U) {
        ssthresh = minimum;
        model->ssthresh_q16 = tcp_shift_cubic_q16_from_bytes(
            ssthresh, transport->mss_bytes);
    }

    policy->cwnd_bytes = cwnd;
    policy->ssthresh_bytes = ssthresh;
    policy->pacing_rate_bytes_per_sec = 0U;
}

static uint64_t tcp_shift_cubic_time_q10(uint64_t time_ns)
{
    uint64_t seconds = time_ns / TCP_SHIFT_NSEC_PER_SEC;
    uint64_t remainder = time_ns % TCP_SHIFT_NSEC_PER_SEC;
    uint64_t result;

    if (seconds > (UINT64_MAX >> TCP_SHIFT_CUBIC_TIME_Q_SHIFT)) {
        return UINT64_MAX;
    }
    result = seconds << TCP_SHIFT_CUBIC_TIME_Q_SHIFT;
    result += (remainder << TCP_SHIFT_CUBIC_TIME_Q_SHIFT) /
              TCP_SHIFT_NSEC_PER_SEC;
    return result;
}

static uint64_t tcp_shift_cubic_nonzero_time_q10(uint64_t time_ns)
{
    uint64_t value = tcp_shift_cubic_time_q10(time_ns);

    return time_ns != 0U && value == 0U ? 1U : value;
}

static int tcp_shift_cubic_cube_leq(uint64_t value, uint64_t limit)
{
    uint64_t square;

    if (value == 0U) {
        return 1;
    }
    if (value > limit / value) {
        return 0;
    }
    square = value * value;
    return value <= limit / square;
}

static uint64_t tcp_shift_cubic_cuberoot_floor(uint64_t value)
{
    uint64_t low = 0U;
    uint64_t high = UINT32_MAX;

    while (low < high) {
        uint64_t middle = low + (high - low + 1U) / 2U;

        if (tcp_shift_cubic_cube_leq(middle, value) != 0) {
            low = middle;
        } else {
            high = middle - 1U;
        }
    }
    return low;
}

static uint64_t tcp_shift_cubic_cube_sat(uint64_t value)
{
    uint64_t square;

    if (value == 0U) {
        return 0U;
    }
    if (value > UINT64_MAX / value) {
        return UINT64_MAX;
    }
    square = value * value;
    if (value > UINT64_MAX / square) {
        return UINT64_MAX;
    }
    return square * value;
}

static uint64_t tcp_shift_cubic_window_q16(
    const struct tcp_shift_cubic_model *model,
    uint64_t time_q10)
{
    uint64_t distance;
    uint64_t term;

    if (time_q10 >= model->k_q10) {
        distance = time_q10 - model->k_q10;
        term = tcp_shift_cubic_cube_sat(distance) / TCP_SHIFT_CUBIC_SCALE;
        return tcp_shift_cubic_add_sat_u64(model->w_max_q16, term);
    }

    distance = model->k_q10 - time_q10;
    term = tcp_shift_cubic_cube_sat(distance) / TCP_SHIFT_CUBIC_SCALE;
    return term >= model->w_max_q16 ? 0U : model->w_max_q16 - term;
}

static void tcp_shift_cubic_start_epoch(struct tcp_shift_cubic_model *model,
                                         uint64_t now_ns)
{
    uint64_t difference;
    uint64_t radicand;

    model->epoch_start_ns = now_ns;
    model->cwnd_epoch_q16 = model->cwnd_q16;
    model->w_est_q16 = model->cwnd_q16;
    model->last_target_q16 = model->cwnd_q16;
    model->epoch_active = 1U;

    if (model->cwnd_prior_q16 == 0U) {
        model->cwnd_prior_q16 = model->cwnd_q16;
    }

    if (model->after_timeout != 0U || model->has_w_max == 0U) {
        model->w_max_q16 = model->cwnd_q16;
        model->has_w_max = 1U;
        model->k_q10 = 0U;
        model->after_timeout = 0U;
        return;
    }

    if (model->w_max_q16 <= model->cwnd_q16) {
        model->k_q10 = 0U;
        return;
    }

    difference = model->w_max_q16 - model->cwnd_q16;
    radicand = tcp_shift_cubic_mul_sat_u64(difference,
                                            TCP_SHIFT_CUBIC_SCALE);
    model->k_q10 = tcp_shift_cubic_cuberoot_floor(radicand);
}

static uint64_t tcp_shift_cubic_ratio_q16(uint32_t numerator,
                                           uint32_t denominator)
{
    if (denominator == 0U) {
        return 0U;
    }
    return ((uint64_t)numerator << TCP_SHIFT_CUBIC_Q_SHIFT) / denominator;
}

static void tcp_shift_cubic_update_w_est(struct tcp_shift_cubic_model *model,
                                          uint32_t acked_bytes,
                                          uint32_t cwnd_bytes,
                                          uint64_t limit_q16)
{
    uint64_t increment = tcp_shift_cubic_ratio_q16(acked_bytes, cwnd_bytes);

    if (model->w_est_q16 < model->cwnd_prior_q16) {
        increment = (increment * TCP_SHIFT_CUBIC_RENO_ALPHA_NUM) /
                    TCP_SHIFT_CUBIC_RENO_ALPHA_DEN;
    }
    model->w_est_q16 = tcp_shift_cubic_add_sat_u64(model->w_est_q16,
                                                    increment);
    model->w_est_q16 = tcp_shift_cubic_min_u64(model->w_est_q16, limit_q16);
}

static uint32_t tcp_shift_cubic_beta_flight_bytes(
    const struct tcp_shift_cc_transport *transport)
{
    uint64_t reduced =
        ((uint64_t)transport->inflight_bytes * TCP_SHIFT_CUBIC_BETA_NUM) /
        TCP_SHIFT_CUBIC_BETA_DEN;
    uint32_t floor = tcp_shift_cubic_mul2_cap(transport->mss_bytes,
                                              transport->cwnd_limit_bytes);
    uint32_t value = reduced > UINT32_MAX ? UINT32_MAX : (uint32_t)reduced;

    value = tcp_shift_cubic_max_u32(value, floor);
    return tcp_shift_cubic_min_u32(value, transport->cwnd_limit_bytes);
}

int tcp_shift_cubic_model_init(struct tcp_shift_cubic_model *model,
                               const struct tcp_shift_cc_transport *transport,
                               const struct tcp_shift_cc_init *init,
                               struct tcp_shift_cc_policy *policy)
{
    uint32_t ssthresh_floor;

    if (model == NULL || transport == NULL || init == NULL || policy == NULL ||
        transport->mss_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes) {
        return -1;
    }

    ssthresh_floor = tcp_shift_cubic_mul2_cap(transport->mss_bytes,
                                              transport->cwnd_limit_bytes);
    if (init->initial_cwnd_bytes < transport->mss_bytes ||
        init->min_cwnd_bytes < transport->mss_bytes ||
        init->initial_cwnd_bytes < init->min_cwnd_bytes ||
        init->initial_ssthresh_bytes < ssthresh_floor ||
        init->initial_cwnd_bytes > transport->cwnd_limit_bytes ||
        init->initial_ssthresh_bytes > transport->cwnd_limit_bytes ||
        init->min_cwnd_bytes > transport->cwnd_limit_bytes) {
        return -1;
    }

    *model = (struct tcp_shift_cubic_model){0};
    model->mss_bytes = transport->mss_bytes;
    model->min_cwnd_bytes = init->min_cwnd_bytes;
    model->cwnd_q16 = tcp_shift_cubic_q16_from_bytes(
        init->initial_cwnd_bytes, transport->mss_bytes);
    model->ssthresh_q16 = tcp_shift_cubic_q16_from_bytes(
        init->initial_ssthresh_bytes, transport->mss_bytes);
    model->fast_convergence = 1U;
    tcp_shift_cubic_publish(model, transport, policy);
    return 0;
}

int tcp_shift_cubic_model_on_ack(struct tcp_shift_cubic_model *model,
                                 const struct tcp_shift_cc_transport *transport,
                                 const struct tcp_shift_cc_ack *ack,
                                 uint64_t now_ns,
                                 uint64_t smoothed_rtt_ns,
                                 struct tcp_shift_cc_policy *policy)
{
    uint64_t limit_q16;
    uint64_t current_cubic_q16;
    uint64_t target_q16;
    uint64_t elapsed_q10;
    uint64_t target_time_q10;
    uint64_t rtt_q10;
    uint64_t upper_q16;
    uint64_t difference;
    uint64_t increment_q16;
    uint32_t cwnd_bytes;
    uint32_t increase_bytes;
    uint32_t slow_start_limit;
    int in_congestion_avoidance;

    if (model == NULL || transport == NULL || ack == NULL || policy == NULL ||
        ack->acked_bytes == 0U || transport->mss_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        tcp_shift_cubic_sync_mss(model, transport->mss_bytes) != 0) {
        return -1;
    }
    if (model->ack_events != 0U && now_ns < model->last_ack_time_ns) {
        return -1;
    }

    tcp_shift_cubic_normalize(model, transport);
    in_congestion_avoidance = model->cwnd_q16 >= model->ssthresh_q16;
    if ((ack->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) == 0U &&
        in_congestion_avoidance != 0 && smoothed_rtt_ns == 0U) {
        return -1;
    }

    if ((ack->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U) {
        if (model->app_limited_paused == 0U) {
            model->app_limited_since_ns = model->ack_events != 0U
                                              ? model->last_ack_time_ns
                                              : now_ns;
            model->app_limited_paused = 1U;
        }
        model->last_ack_time_ns = now_ns;
        model->ack_events++;
        model->app_limited_acks++;
        tcp_shift_cubic_publish(model, transport, policy);
        return 0;
    }

    if (model->app_limited_paused != 0U) {
        if (model->epoch_active != 0U && now_ns >= model->app_limited_since_ns) {
            model->epoch_start_ns = tcp_shift_cubic_add_sat_u64(
                model->epoch_start_ns, now_ns - model->app_limited_since_ns);
        }
        model->app_limited_paused = 0U;
        model->app_limited_since_ns = 0U;
    }

    model->last_ack_time_ns = now_ns;
    model->ack_events++;

    if (model->cwnd_q16 < model->ssthresh_q16) {
        cwnd_bytes = tcp_shift_cubic_bytes_from_q16(
            model->cwnd_q16, transport->mss_bytes,
            transport->cwnd_limit_bytes);
        slow_start_limit = tcp_shift_cubic_mul2_cap(
            transport->mss_bytes, transport->cwnd_limit_bytes);
        increase_bytes = ack->acked_bytes < slow_start_limit
                             ? ack->acked_bytes
                             : slow_start_limit;
        if (increase_bytes > transport->cwnd_limit_bytes - cwnd_bytes) {
            cwnd_bytes = transport->cwnd_limit_bytes;
        } else {
            cwnd_bytes += increase_bytes;
        }
        model->cwnd_q16 = tcp_shift_cubic_q16_from_bytes(
            cwnd_bytes, transport->mss_bytes);
        tcp_shift_cubic_publish(model, transport, policy);
        return 0;
    }

    if (model->epoch_active == 0U) {
        tcp_shift_cubic_start_epoch(model, now_ns);
    }

    limit_q16 = tcp_shift_cubic_limit_q16(transport);
    cwnd_bytes = tcp_shift_cubic_bytes_from_q16(
        model->cwnd_q16, transport->mss_bytes, transport->cwnd_limit_bytes);
    tcp_shift_cubic_update_w_est(model, ack->acked_bytes, cwnd_bytes,
                                  limit_q16);

    elapsed_q10 = tcp_shift_cubic_time_q10(now_ns - model->epoch_start_ns);
    current_cubic_q16 = tcp_shift_cubic_window_q16(model, elapsed_q10);
    if (current_cubic_q16 < model->w_est_q16) {
        model->cwnd_q16 = tcp_shift_cubic_max_u64(model->cwnd_q16,
                                                   model->w_est_q16);
        model->last_target_q16 = model->w_est_q16;
        tcp_shift_cubic_publish(model, transport, policy);
        return 0;
    }

    rtt_q10 = tcp_shift_cubic_nonzero_time_q10(smoothed_rtt_ns);
    target_time_q10 = tcp_shift_cubic_add_sat_u64(elapsed_q10, rtt_q10);
    target_q16 = tcp_shift_cubic_window_q16(model, target_time_q10);
    upper_q16 = tcp_shift_cubic_add_sat_u64(model->cwnd_q16,
                                             model->cwnd_q16 / 2U);
    upper_q16 = tcp_shift_cubic_min_u64(upper_q16, limit_q16);
    target_q16 = tcp_shift_cubic_max_u64(target_q16, model->cwnd_q16);
    target_q16 = tcp_shift_cubic_min_u64(target_q16, upper_q16);
    model->last_target_q16 = target_q16;

    difference = target_q16 - model->cwnd_q16;
    if (difference != 0U && model->cwnd_q16 != 0U) {
        increment_q16 =
            (difference << TCP_SHIFT_CUBIC_Q_SHIFT) / model->cwnd_q16;
        model->cwnd_q16 = tcp_shift_cubic_add_sat_u64(model->cwnd_q16,
                                                       increment_q16);
    }
    tcp_shift_cubic_publish(model, transport, policy);
    return 0;
}

int tcp_shift_cubic_model_on_loss(struct tcp_shift_cubic_model *model,
                                  const struct tcp_shift_cc_transport *transport,
                                  const struct tcp_shift_cc_loss *loss,
                                  struct tcp_shift_cc_policy *policy)
{
    uint64_t prior_cwnd_q16;
    uint32_t reduced_bytes;
    uint32_t minimum_bytes;

    if (model == NULL || transport == NULL || loss == NULL || policy == NULL ||
        loss->lost_bytes == 0U || transport->mss_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        tcp_shift_cubic_sync_mss(model, transport->mss_bytes) != 0) {
        return -1;
    }

    tcp_shift_cubic_normalize(model, transport);
    prior_cwnd_q16 = model->cwnd_q16;
    if (model->fast_convergence != 0U && model->has_w_max != 0U &&
        prior_cwnd_q16 < model->w_max_q16) {
        model->w_max_q16 =
            (prior_cwnd_q16 * TCP_SHIFT_CUBIC_FAST_CONVERGENCE_NUM) /
            TCP_SHIFT_CUBIC_FAST_CONVERGENCE_DEN;
    } else {
        model->w_max_q16 = prior_cwnd_q16;
    }
    model->has_w_max = 1U;
    model->cwnd_prior_q16 = prior_cwnd_q16;

    reduced_bytes = tcp_shift_cubic_beta_flight_bytes(transport);
    minimum_bytes = tcp_shift_cubic_effective_min_bytes(model, transport);
    reduced_bytes = tcp_shift_cubic_max_u32(reduced_bytes, minimum_bytes);
    model->ssthresh_q16 = tcp_shift_cubic_q16_from_bytes(
        reduced_bytes, transport->mss_bytes);
    model->cwnd_q16 = model->ssthresh_q16;
    model->epoch_active = 0U;
    model->app_limited_paused = 0U;
    model->app_limited_since_ns = 0U;
    model->after_timeout = 0U;
    model->k_q10 = 0U;
    model->loss_events++;
    tcp_shift_cubic_publish(model, transport, policy);
    return 0;
}

int tcp_shift_cubic_model_on_timeout(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    uint32_t reduced_bytes;
    uint32_t minimum_bytes;

    if (model == NULL || transport == NULL || policy == NULL ||
        transport->mss_bytes == 0U ||
        transport->cwnd_limit_bytes < transport->mss_bytes ||
        tcp_shift_cubic_sync_mss(model, transport->mss_bytes) != 0) {
        return -1;
    }

    tcp_shift_cubic_normalize(model, transport);
    model->cwnd_prior_q16 = model->cwnd_q16;
    reduced_bytes = tcp_shift_cubic_beta_flight_bytes(transport);
    model->ssthresh_q16 = tcp_shift_cubic_q16_from_bytes(
        reduced_bytes, transport->mss_bytes);
    minimum_bytes = tcp_shift_cubic_effective_min_bytes(model, transport);
    model->cwnd_q16 = tcp_shift_cubic_q16_from_bytes(
        minimum_bytes, transport->mss_bytes);
    model->epoch_active = 0U;
    model->app_limited_paused = 0U;
    model->app_limited_since_ns = 0U;
    model->after_timeout = 1U;
    model->k_q10 = 0U;
    model->timeout_events++;
    tcp_shift_cubic_publish(model, transport, policy);
    return 0;
}

void tcp_shift_cubic_model_set_fast_convergence(
    struct tcp_shift_cubic_model *model,
    unsigned enabled)
{
    if (model != NULL) {
        model->fast_convergence = enabled != 0U;
    }
}
