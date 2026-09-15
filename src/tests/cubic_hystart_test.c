#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/cc.h"
#include "cc/cubic.h"
#include "cc/registry.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "cubic-hystart: check failed at %s:%d: %s\n",           \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static void set_ack(struct tcp_shift_cc_ack *ack,
                    uint64_t now_ns,
                    uint64_t rtt_ns,
                    uint64_t prior_delivered,
                    uint64_t delivered_total)
{
    memset(ack, 0, sizeof(*ack));
    ack->acked_bytes = 1000U;
    ack->ack_time_ns = now_ns;
    ack->smoothed_rtt_ns = rtt_ns;
    ack->rate.rtt_ns = rtt_ns;
    ack->rate.prior_delivered_bytes = prior_delivered;
    ack->rate.delivered_total_bytes = delivered_total;
    ack->rate.flags = TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
}

static int run_delay_detector(void)
{
    union tcp_shift_cc_builtin_state state;
    struct tcp_shift_cc cc;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 16000U,
        .send_window_bytes = 65535U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 16000U,
        .initial_ssthresh_bytes = 64000U,
        .min_cwnd_bytes = 2000U,
    };
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;
    uint64_t delivered = 0U;
    uint32_t i;

    memset(&state, 0, sizeof(state));
    memset(&cc, 0, sizeof(cc));
    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_cubic_ops,
                            &state, sizeof(state),
                            &transport, &init, &policy) == 0);
    CHECK(state.cubic.hystart_enabled == 1U);

    /* Establish a 20 ms minimum RTT without creating an ACK train. All first
     * flight packets carry prior_delivered=0, so they remain in one round. */
    for (i = 0U; i < 8U; i++) {
        delivered += 1000U;
        set_ack(&ack,
                UINT64_C(100000000) + (uint64_t)i * UINT64_C(5000000),
                UINT64_C(20000000), 0U, delivered);
        CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
        CHECK(state.cubic.hystart_found == 0U);
    }
    CHECK(state.cubic.hystart_delay_min_ns == UINT64_C(20000000));

    /* Start a new packet-timed round whose RTT minimum is 30 ms. Linux's
     * threshold is minRTT + clamp(minRTT/8, 4 ms, 16 ms) = 24 ms here. */
    for (i = 0U; i < 9U; i++) {
        uint64_t prior = i == 0U ? UINT64_C(1000) : UINT64_C(1000);

        delivered += 1000U;
        set_ack(&ack,
                UINT64_C(200000000) + (uint64_t)i * UINT64_C(5000000),
                UINT64_C(30000000), prior, delivered);
        CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
        if (i < 8U) {
            CHECK(state.cubic.hystart_found == 0U);
        }
    }

    CHECK(state.cubic.hystart_found == 1U);
    CHECK(state.cubic.hystart_delay_found == 1U);
    CHECK(state.cubic.hystart_ack_train_found == 0U);
    CHECK(state.cubic.hystart_exit_events == 1U);
    CHECK(policy.ssthresh_bytes < init.initial_ssthresh_bytes);
    CHECK(state.cubic.has_w_max == 1U);
    CHECK(state.cubic.k_q10 == 0U);

    CHECK(tcp_shift_cc_on_timeout(&cc, &transport, &policy) == 0);
    CHECK(state.cubic.hystart_enabled == 1U);
    CHECK(state.cubic.hystart_found == 0U);
    CHECK(state.cubic.hystart_delay_found == 0U);
    CHECK(state.cubic.hystart_ack_train_found == 0U);
    return 0;
}

static int run_ack_train_detector(void)
{
    union tcp_shift_cc_builtin_state state;
    struct tcp_shift_cc cc;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 16000U,
        .send_window_bytes = 65535U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 16000U,
        .initial_ssthresh_bytes = 64000U,
        .min_cwnd_bytes = 2000U,
    };
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;
    uint64_t delivered = 0U;
    uint32_t i;

    memset(&state, 0, sizeof(state));
    memset(&cc, 0, sizeof(cc));
    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_cubic_ops,
                            &state, sizeof(state),
                            &transport, &init, &policy) == 0);

    /* With minRTT=40 ms and no CUBIC pacing request, the classic Linux
     * ACK-train threshold is 20 ms. Keep ACK gaps at 1 ms and RTT flat so the
     * train detector, not the delay detector, exits slow start. */
    for (i = 0U; i < 24U && state.cubic.hystart_found == 0U; i++) {
        delivered += 1000U;
        set_ack(&ack,
                UINT64_C(1000000000) + (uint64_t)i * UINT64_C(1000000),
                UINT64_C(40000000), 0U, delivered);
        CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    }

    CHECK(state.cubic.hystart_found == 1U);
    CHECK(state.cubic.hystart_ack_train_found == 1U);
    CHECK(state.cubic.hystart_delay_found == 0U);
    CHECK(state.cubic.hystart_exit_events == 1U);
    CHECK(policy.ssthresh_bytes < init.initial_ssthresh_bytes);
    return 0;
}

int main(void)
{
    CHECK(run_delay_detector() == 0);
    CHECK(run_ack_train_detector() == 0);

    printf("cubic_hystart=ok low_window=16 min_samples=8 "
           "ack_delta_ms=2 delay_thresh_ms=4..16 detectors=ack_train,delay\n");
    return 0;
}
