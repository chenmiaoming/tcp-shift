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
                    "cubic-hystartpp: check failed at %s:%d: %s\n",         \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static void set_ack(struct tcp_shift_cc_ack *ack,
                    uint32_t acked_bytes,
                    uint64_t now_ns,
                    uint64_t rtt_ns,
                    uint64_t delivered_total)
{
    memset(ack, 0, sizeof(*ack));
    ack->acked_bytes = acked_bytes;
    ack->ack_time_ns = now_ns;
    ack->smoothed_rtt_ns = rtt_ns;
    ack->rate.rtt_ns = rtt_ns;
    ack->rate.delivered_total_bytes = delivered_total;
    ack->rate.flags = TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
}

static int init_cubic(union tcp_shift_cc_builtin_state *state,
                      struct tcp_shift_cc *cc,
                      const struct tcp_shift_cc_transport *transport,
                      struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 16000U,
        .initial_ssthresh_bytes = 64000U,
        .min_cwnd_bytes = 2000U,
    };

    memset(state, 0, sizeof(*state));
    memset(cc, 0, sizeof(*cc));
    return tcp_shift_cc_init(cc, &tcp_shift_cubic_ops,
                             state, sizeof(*state),
                             transport, &init, policy);
}

static int drive_to_css(union tcp_shift_cc_builtin_state *state,
                        struct tcp_shift_cc *cc,
                        const struct tcp_shift_cc_transport *transport,
                        struct tcp_shift_cc_policy *policy,
                        uint64_t *delivered)
{
    struct tcp_shift_cc_ack ack;
    uint32_t i;

    /* Round one: eight valid samples at 20 ms. The delivery-domain windowEnd
     * is delivered+inflight, so delivered=9k starts the next round. */
    for (i = 0U; i < 8U; i++) {
        *delivered += 1000U;
        set_ack(&ack, 1000U,
                UINT64_C(100000000) + (uint64_t)i * UINT64_C(1000000),
                UINT64_C(20000000), *delivered);
        CHECK(tcp_shift_cc_on_ack(cc, transport, &ack, policy) == 0);
    }
    CHECK(state->cubic.hystart_current_round_min_rtt_ns ==
          UINT64_C(20000000));
    CHECK(state->cubic.hystart_sample_count == 8U);

    /* Round two: 30 ms minimum. With lastRoundMinRTT=20 ms, RFC 9406 uses
     * max(4 ms, min(20/8 ms, 16 ms)) = 4 ms, so the eighth sample enters CSS. */
    for (i = 0U; i < 8U; i++) {
        *delivered += 1000U;
        set_ack(&ack, 1000U,
                UINT64_C(200000000) + (uint64_t)i * UINT64_C(1000000),
                UINT64_C(30000000), *delivered);
        CHECK(tcp_shift_cc_on_ack(cc, transport, &ack, policy) == 0);
    }

    CHECK(state->cubic.hystart_last_round_min_rtt_ns ==
          UINT64_C(20000000));
    CHECK(state->cubic.hystart_current_round_min_rtt_ns ==
          UINT64_C(30000000));
    CHECK(state->cubic.hystart_css == 1U);
    CHECK(state->cubic.hystart_css_enter_events == 1U);
    CHECK(state->cubic.hystart_css_baseline_min_rtt_ns ==
          UINT64_C(30000000));
    CHECK(state->cubic.hystart_exit_events == 0U);
    return 0;
}

static int run_paced_l_infinity(void)
{
    union tcp_shift_cc_builtin_state state;
    struct tcp_shift_cc cc;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 8000U,
        .send_window_bytes = 65535U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;

    CHECK(init_cubic(&state, &cc, &transport, &policy) == 0);
    CHECK(state.cubic.hystart_enabled == 1U);

    /* RFC 9406 recommends L=infinity for paced TCP. A 6-SMSS ACK therefore
     * contributes all 6000 bytes instead of the old Linux-shaped 2-SMSS cap. */
    set_ack(&ack, 6000U, UINT64_C(100000000), UINT64_C(20000000),
            UINT64_C(6000));
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 22000U);
    return 0;
}

static int run_css_growth_and_revert(void)
{
    union tcp_shift_cc_builtin_state state;
    struct tcp_shift_cc cc;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 8000U,
        .send_window_bytes = 65535U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;
    uint64_t delivered = 0U;
    uint32_t i;
    uint32_t cwnd_before;

    CHECK(init_cubic(&state, &cc, &transport, &policy) == 0);
    CHECK(drive_to_css(&state, &cc, &transport, &policy, &delivered) == 0);
    CHECK(policy.cwnd_bytes == 32000U);

    /* The first ACK after CSS entry starts in CSS. With N=4000, growth is
     * N/4=1000. The new round's 25 ms RTT then remains below the 30 ms CSS
     * baseline; after eight samples RFC 9406 declares the exit spurious. */
    cwnd_before = policy.cwnd_bytes;
    delivered += 1000U;
    set_ack(&ack, 4000U, UINT64_C(300000000), UINT64_C(25000000), delivered);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == cwnd_before + 1000U);
    CHECK(state.cubic.hystart_css_rounds == 1U);

    for (i = 1U; i < 8U; i++) {
        delivered += 1000U;
        set_ack(&ack, 1000U,
                UINT64_C(300000000) + (uint64_t)i * UINT64_C(1000000),
                UINT64_C(25000000), delivered);
        CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    }
    CHECK(state.cubic.hystart_css == 0U);
    CHECK(state.cubic.hystart_css_revert_events == 1U);
    CHECK(state.cubic.hystart_enabled == 1U);
    CHECK(state.cubic.hystart_exit_events == 0U);

    /* The ACK that proves the spike spurious still arrived in CSS. The next
     * ACK is ordinary slow start again and gets the full paced L=infinity credit. */
    cwnd_before = policy.cwnd_bytes;
    delivered += 1000U;
    set_ack(&ack, 1000U, UINT64_C(400000000), UINT64_C(25000000), delivered);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == cwnd_before + 1000U);
    return 0;
}

static int run_css_confirmation(void)
{
    union tcp_shift_cc_builtin_state state;
    struct tcp_shift_cc cc;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 8000U,
        .send_window_bytes = 65535U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;
    uint64_t delivered = 0U;
    uint32_t i;

    CHECK(init_cubic(&state, &cc, &transport, &policy) == 0);
    CHECK(drive_to_css(&state, &cc, &transport, &policy, &delivered) == 0);

    /* CSS began in the round ending at delivered=17k. Keeping RTT at the
     * 30 ms baseline prevents a spurious reversion. Boundaries at 17,25,33,
     * 41,49 kB complete the five RFC 9406 CSS rounds. */
    for (i = 0U; delivered < UINT64_C(49000); i++) {
        delivered += 1000U;
        set_ack(&ack, 1000U,
                UINT64_C(500000000) + (uint64_t)i * UINT64_C(1000000),
                UINT64_C(30000000), delivered);
        CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    }

    CHECK(state.cubic.hystart_css_rounds ==
          TCP_SHIFT_CUBIC_HYSTARTPP_CSS_ROUNDS);
    CHECK(state.cubic.hystart_exit_events == 1U);
    CHECK(state.cubic.hystart_enabled == 0U);
    CHECK(state.cubic.hystart_initial_complete == 1U);
    CHECK(state.cubic.hystart_css == 0U);
    CHECK(state.cubic.hystart_exit_pending == 0U);
    CHECK(policy.cwnd_bytes == policy.ssthresh_bytes);
    CHECK(state.cubic.has_w_max == 1U);
    CHECK(state.cubic.k_q10 == 0U);
    return 0;
}

static int run_initial_only(void)
{
    union tcp_shift_cc_builtin_state state;
    struct tcp_shift_cc cc;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 8000U,
        .send_window_bytes = 65535U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;

    CHECK(init_cubic(&state, &cc, &transport, &policy) == 0);
    CHECK(state.cubic.hystart_enabled == 1U);
    CHECK(tcp_shift_cc_on_timeout(&cc, &transport, &policy) == 0);
    CHECK(state.cubic.hystart_enabled == 0U);
    CHECK(state.cubic.hystart_initial_complete == 1U);

    set_ack(&ack, 1000U, UINT64_C(1000000000), UINT64_C(20000000),
            UINT64_C(1000));
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(state.cubic.hystart_enabled == 0U);
    CHECK(state.cubic.hystart_css_enter_events == 0U);
    return 0;
}

int main(void)
{
    CHECK(run_paced_l_infinity() == 0);
    CHECK(run_css_growth_and_revert() == 0);
    CHECK(run_css_confirmation() == 0);
    CHECK(run_initial_only() == 0);

    printf("cubic_hystartpp=ok rfc=9406 min_samples=8 "
           "delay_thresh_ms=4..16 css_divisor=4 css_rounds=5 "
           "paced_L=infinity detectors=delay_only initial_only=1\n");
    return 0;
}
