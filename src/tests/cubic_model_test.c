#include <stdint.h>
#include <stdio.h>

#include "cc/cubic.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "cubic-model: check failed at %s:%d: %s\n",      \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int test_slow_start_app_limited_loss_and_timeout(void)
{
    struct tcp_shift_cubic_model model;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 0U,
        .send_window_bytes = 64000U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 4000U,
        .initial_ssthresh_bytes = 8000U,
        .min_cwnd_bytes = 1000U,
    };
    struct tcp_shift_cc_ack ack = {.acked_bytes = 2000U};
    struct tcp_shift_cc_loss loss = {.lost_bytes = 1000U};
    struct tcp_shift_cc_policy policy;
    uint64_t w_max_before;

    CHECK(tcp_shift_cubic_model_init(&model, &transport, &init, &policy) == 0);
    CHECK(policy.cwnd_bytes == 4000U);
    CHECK(policy.ssthresh_bytes == 8000U);
    CHECK(policy.pacing_rate_bytes_per_sec == 0U);
    CHECK(model.fast_convergence == 1U);

    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(100000000), 0U,
                                       &policy) == 0);
    CHECK(policy.cwnd_bytes == 6000U);
    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(200000000), 0U,
                                       &policy) == 0);
    CHECK(policy.cwnd_bytes == 8000U);

    ack.acked_bytes = 1000U;
    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(1000000000),
                                       UINT64_C(100000000), &policy) == 0);
    CHECK(model.epoch_active == 1U);
    CHECK(model.k_q10 == 0U);
    CHECK(policy.cwnd_bytes > 8000U);
    CHECK(policy.cwnd_bytes < 8300U);

    ack.rate.flags = TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED;
    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(2000000000), 0U,
                                       &policy) == 0);
    CHECK(model.app_limited_paused == 1U);
    CHECK(model.app_limited_acks == 1U);
    {
        uint32_t paused_cwnd = policy.cwnd_bytes;

        ack.rate.flags = 0U;
        CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                           UINT64_C(12000000000),
                                           UINT64_C(100000000),
                                           &policy) == 0);
        CHECK(model.app_limited_paused == 0U);
        CHECK(model.epoch_start_ns == UINT64_C(12000000000));
        CHECK(policy.cwnd_bytes >= paused_cwnd);
        CHECK(policy.cwnd_bytes < 9000U);
    }

    transport.inflight_bytes = 10000U;
    CHECK(tcp_shift_cubic_model_on_loss(&model, &transport, &loss,
                                        &policy) == 0);
    CHECK(policy.ssthresh_bytes == 7000U);
    CHECK(policy.cwnd_bytes == 7000U);
    CHECK(model.loss_events == 1U);
    w_max_before = model.w_max_q16;

    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(13000000000),
                                       UINT64_C(100000000), &policy) == 0);
    CHECK(model.k_q10 > 0U);
    CHECK(model.cwnd_q16 < w_max_before);

    transport.inflight_bytes = 7000U;
    CHECK(tcp_shift_cubic_model_on_loss(&model, &transport, &loss,
                                        &policy) == 0);
    CHECK(model.loss_events == 2U);
    CHECK(model.w_max_q16 < w_max_before);
    CHECK(policy.ssthresh_bytes == 4900U);
    CHECK(policy.cwnd_bytes == 4900U);

    transport.inflight_bytes = 9000U;
    CHECK(tcp_shift_cubic_model_on_timeout(&model, &transport, &policy) == 0);
    CHECK(model.timeout_events == 1U);
    CHECK(model.after_timeout == 1U);
    CHECK(policy.ssthresh_bytes == 6300U);
    CHECK(policy.cwnd_bytes == 1000U);

    transport.mss_bytes = 1250U;
    ack.acked_bytes = 2500U;
    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(14000000000), 0U,
                                       &policy) == 0);
    CHECK(model.mss_bytes == 1250U);
    CHECK(policy.cwnd_bytes == 3500U);

    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(13000000000), 0U,
                                       &policy) == -1);
    return 0;
}

static int test_cubic_k_and_fast_convergence_toggle(void)
{
    struct tcp_shift_cubic_model model;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 100000U,
        .send_window_bytes = 200000U,
        .cwnd_limit_bytes = 250000U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 100000U,
        .initial_ssthresh_bytes = 50000U,
        .min_cwnd_bytes = 1000U,
    };
    struct tcp_shift_cc_ack ack = {.acked_bytes = 1000U};
    struct tcp_shift_cc_loss loss = {.lost_bytes = 1000U};
    struct tcp_shift_cc_policy policy;
    uint64_t remembered_w_max;

    CHECK(tcp_shift_cubic_model_init(&model, &transport, &init, &policy) == 0);
    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(1000000000),
                                       UINT64_C(100000000), &policy) == 0);
    CHECK(model.k_q10 == 0U);

    CHECK(tcp_shift_cubic_model_on_loss(&model, &transport, &loss,
                                        &policy) == 0);
    CHECK(policy.cwnd_bytes == 70000U);
    CHECK(model.w_max_q16 == UINT64_C(100) * TCP_SHIFT_CUBIC_Q_ONE);
    remembered_w_max = model.w_max_q16;

    transport.inflight_bytes = 70000U;
    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(2000000000),
                                       UINT64_C(100000000), &policy) == 0);
    CHECK(model.k_q10 >= 4317U && model.k_q10 <= 4319U);
    CHECK(model.w_max_q16 == remembered_w_max);

    tcp_shift_cubic_model_set_fast_convergence(&model, 0U);
    CHECK(model.fast_convergence == 0U);
    CHECK(tcp_shift_cubic_model_on_loss(&model, &transport, &loss,
                                        &policy) == 0);
    CHECK(model.w_max_q16 == model.cwnd_prior_q16);
    return 0;
}

static int test_invalid_inputs(void)
{
    struct tcp_shift_cubic_model model;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 0U,
        .send_window_bytes = 64000U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 4000U,
        .initial_ssthresh_bytes = 8000U,
        .min_cwnd_bytes = 1000U,
    };
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_cc_ack ack = {.acked_bytes = 1000U};

    CHECK(tcp_shift_cubic_model_init(NULL, &transport, &init, &policy) == -1);
    transport.mss_bytes = 0U;
    CHECK(tcp_shift_cubic_model_init(&model, &transport, &init, &policy) == -1);
    transport.mss_bytes = 1000U;
    CHECK(tcp_shift_cubic_model_init(&model, &transport, &init, &policy) == 0);

    model.cwnd_q16 = model.ssthresh_q16;
    CHECK(tcp_shift_cubic_model_on_ack(&model, &transport, &ack,
                                       UINT64_C(1000000000), 0U,
                                       &policy) == -1);
    return 0;
}

int main(void)
{
    CHECK(test_slow_start_app_limited_loss_and_timeout() == 0);
    CHECK(test_cubic_k_and_fast_convergence_toggle() == 0);
    CHECK(test_invalid_inputs() == 0);

    printf("cubic_model_contract=ok beta=7/10 C=2/5 reno_alpha=9/17 "
           "fast_convergence=17/20 time_q=10 window_q=16\n");
    return 0;
}
