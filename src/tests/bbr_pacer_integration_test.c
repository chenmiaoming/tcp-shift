#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr.h"
#include "cc/transport_pacing.h"
#include "runtime/pacer.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "bbr-pacer: check failed at %s:%d: %s\n",       \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static uint64_t spacing_ns(uint32_t bytes, uint64_t rate)
{
    uint64_t numerator = (uint64_t)bytes * UINT64_C(1000000000);
    uint64_t value = numerator / rate;

    if ((numerator % rate) != 0U) {
        value++;
    }
    return value;
}

static int check_initial_contract(void)
{
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1460U,
        .inflight_bytes = 4380U,
        .send_window_bytes = 1048576U,
        .cwnd_limit_bytes = 1048576U,
    };
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_flow_pacer flow;
    uint64_t expected;
    uint64_t rate_1ms;
    uint64_t rate_10ms;
    uint64_t now_ns = UINT64_C(1000000000);
    uint64_t deadline;
    uint64_t spacing;
    uint64_t late_ns;

    rate_1ms = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(4380U, 0U);
    expected = UINT64_C(4380000) *
               (TCP_SHIFT_BBR_STARTUP_GAIN_NUM *
                TCP_SHIFT_BBR_PACING_MARGIN_NUM) /
               (TCP_SHIFT_BBR_GAIN_DEN *
                TCP_SHIFT_BBR_PACING_MARGIN_DEN);
    CHECK(rate_1ms == expected);
    CHECK(rate_1ms == UINT64_C(12517389));

    rate_10ms = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(
        4380U, UINT64_C(10000000));
    CHECK(rate_10ms == UINT64_C(1251738));
    CHECK(rate_10ms < rate_1ms);
    CHECK(tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(0U, 0U) == 0U);

    memset(&policy, 0, sizeof(policy));
    policy.cwnd_bytes = 4380U;
    policy.ssthresh_bytes = 1048576U;
    policy.pacing_rate_bytes_per_sec = rate_1ms;

    /* A BBR-owned rate must pass through the generic transport fallback
     * unchanged, even when qualification supplies a much lower cap. */
    CHECK(tcp_shift_transport_pacing_apply_window_fallback(
              &transport, 0U, 1U, &policy) == 0);
    CHECK(policy.pacing_rate_bytes_per_sec == rate_1ms);

    /* The shared per-flow clock turns that controller-owned rate into an
     * absolute eligibility timeline. Zero catch-up means a late wakeup cannot
     * repay timing debt as a burst. */
    tcp_shift_flow_pacer_init(&flow, 0U);
    tcp_shift_flow_pacer_set_rate(&flow, policy.pacing_rate_bytes_per_sec);
    CHECK(tcp_shift_flow_pacer_deadline(&flow, now_ns) == 0U);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, now_ns, transport.mss_bytes) == 0);

    spacing = spacing_ns(transport.mss_bytes,
                         policy.pacing_rate_bytes_per_sec);
    deadline = tcp_shift_flow_pacer_deadline(&flow, now_ns);
    CHECK(deadline == now_ns + spacing);

    late_ns = deadline + UINT64_C(5000000);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, late_ns, transport.mss_bytes) == 0);
    CHECK(flow.next_send_ns == late_ns + spacing);
    CHECK(flow.tx_events == 2U);
    CHECK(flow.tx_bytes == 2920U);
    return 0;
}

static int check_rate_lifecycle(void)
{
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1460U,
        .inflight_bytes = 120000U,
        .send_window_bytes = 1048576U,
        .cwnd_limit_bytes = 1048576U,
    };
    struct tcp_shift_bbr_model model;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_rate_sample drain_sample;
    struct tcp_shift_cc_policy startup_policy;
    struct tcp_shift_cc_policy drain_policy;
    struct tcp_shift_flow_pacer flow;
    uint64_t initial_rate;
    uint64_t initial_deadline;
    uint64_t startup_deadline;
    uint64_t startup_spacing;
    uint64_t drain_deadline;
    uint64_t drain_spacing;
    uint64_t now_ns = UINT64_C(2000000000);
    uint32_t current_cwnd = 4380U;

    memset(&ack, 0, sizeof(ack));
    memset(&drain_sample, 0, sizeof(drain_sample));
    tcp_shift_bbr_model_init(&model);

    /* Use a 40 Mbit/s model estimate with 10 ms min RTT. Its STARTUP target is
     * above the nominal-1ms bootstrap rate, so the lifecycle covers both a
     * rate increase and the deliberate DRAIN decrease. */
    model.max_bw_bytes_per_sec = UINT64_C(5000000);
    model.min_rtt_ns = UINT64_C(10000000);
    model.has_min_rtt = 1U;

    initial_rate = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(
        current_cwnd, 0U);
    CHECK(initial_rate != 0U);

    tcp_shift_flow_pacer_init(&flow, 0U);
    tcp_shift_flow_pacer_set_rate(&flow, initial_rate);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, now_ns, transport.mss_bytes) == 0);
    initial_deadline = flow.next_send_ns;
    CHECK(initial_deadline > now_ns);

    ack.acked_bytes = transport.mss_bytes;
    ack.rate.delivered_total_bytes = current_cwnd;
    CHECK(tcp_shift_bbr_startup_policy(
              &model, &transport, &ack, current_cwnd, current_cwnd,
              initial_rate, &startup_policy) == 0);
    CHECK(startup_policy.pacing_rate_bytes_per_sec > initial_rate);

    /* A rate change must not erase or pull an already scheduled eligibility
     * timestamp forward. The new rate applies when the next transmission
     * advances the virtual clock. */
    tcp_shift_flow_pacer_set_rate(&flow,
                                  startup_policy.pacing_rate_bytes_per_sec);
    CHECK(flow.next_send_ns == initial_deadline);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, initial_deadline,
                                       transport.mss_bytes) == 0);
    startup_spacing = spacing_ns(
        transport.mss_bytes, startup_policy.pacing_rate_bytes_per_sec);
    startup_deadline = flow.next_send_ns;
    CHECK(startup_deadline == initial_deadline + startup_spacing);

    model.full_bw_reached = 1U;
    drain_sample.flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    drain_sample.delivery_rate_bytes_per_sec = model.max_bw_bytes_per_sec;
    drain_sample.prior_inflight_bytes = 120000U;
    CHECK(tcp_shift_bbr_model_check_drain(&model, &drain_sample) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_DRAIN);

    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, startup_policy.cwnd_bytes,
              &drain_policy) == 0);
    CHECK(drain_policy.pacing_rate_bytes_per_sec != 0U);
    CHECK(drain_policy.pacing_rate_bytes_per_sec <
          startup_policy.pacing_rate_bytes_per_sec);

    tcp_shift_flow_pacer_set_rate(&flow,
                                  drain_policy.pacing_rate_bytes_per_sec);
    CHECK(flow.next_send_ns == startup_deadline);
    CHECK(tcp_shift_flow_pacer_note_tx(&flow, startup_deadline,
                                       transport.mss_bytes) == 0);
    drain_spacing = spacing_ns(
        transport.mss_bytes, drain_policy.pacing_rate_bytes_per_sec);
    drain_deadline = flow.next_send_ns;
    CHECK(drain_deadline == startup_deadline + drain_spacing);
    CHECK(drain_spacing > startup_spacing);

    /* Once in-flight reaches 1*BDP, the model may leave DRAIN. ProbeBW policy
     * is intentionally outside this contract; this only verifies the mode
     * boundary that the eventual controller will consume. */
    drain_sample.prior_inflight_bytes = 40000U;
    CHECK(tcp_shift_bbr_model_check_drain(&model, &drain_sample) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);

    printf("bbr_pacer_lifecycle=ok initial_rate_Bps=%llu "
           "startup_rate_Bps=%llu drain_rate_Bps=%llu "
           "startup_spacing_ns=%llu drain_spacing_ns=%llu "
           "deadline_preserved=1 probe_bw_boundary=1\n",
           (unsigned long long)initial_rate,
           (unsigned long long)startup_policy.pacing_rate_bytes_per_sec,
           (unsigned long long)drain_policy.pacing_rate_bytes_per_sec,
           (unsigned long long)startup_spacing,
           (unsigned long long)drain_spacing);
    return 0;
}

int main(void)
{
    uint64_t rate_1ms;
    uint64_t rate_10ms;

    CHECK(check_initial_contract() == 0);
    CHECK(check_rate_lifecycle() == 0);

    rate_1ms = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(4380U, 0U);
    rate_10ms = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(
        4380U, UINT64_C(10000000));
    printf("bbr_pacer_integration=ok nominal_rtt_ms=1 "
           "initial_rate_Bps=%llu observed_rtt_rate_Bps=%llu "
           "controller_rate_precedence=1 zero_catch_up=1\n",
           (unsigned long long)rate_1ms,
           (unsigned long long)rate_10ms);
    return 0;
}
