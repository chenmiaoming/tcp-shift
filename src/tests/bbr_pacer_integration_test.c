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

int main(void)
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

    printf("bbr_pacer_integration=ok nominal_rtt_ms=1 "
           "initial_rate_Bps=%llu observed_rtt_rate_Bps=%llu "
           "controller_rate_precedence=1 zero_catch_up=1 spacing_ns=%llu\n",
           (unsigned long long)rate_1ms,
           (unsigned long long)rate_10ms,
           (unsigned long long)spacing);
    return 0;
}
