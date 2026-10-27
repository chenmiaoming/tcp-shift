#include <stdint.h>
#include <stdio.h>

#include "cc/bbr3_controller.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "bbr3-controller:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void sample(struct tcp_shift_bbr3_controller_ack *a,
                   uint64_t now, uint64_t prior, uint64_t delivered,
                   uint64_t rate, uint32_t acked, uint64_t rtt)
{
    a->now_ns = now;
    a->rate.prior_delivered_bytes = prior;
    a->rate.delivered_total_bytes = delivered;
    a->rate.delivery_rate_bytes_per_sec = rate;
    a->rate.delivered_bytes = acked;
    a->rate.rtt_ns = rtt;
    a->rate.flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                    TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
    a->acked_bytes = acked;
    a->lost_bytes = 0U;
    a->ce_bytes = 0U;
    a->validated_loss = 0U;
    a->ecn_eligible = 0U;
    a->loss_round_end = 1U;
    a->probe_sample = 0U;
    a->cwnd_limited = 1U;
    a->idle_restart = 0U;
    a->inflight_bytes = 1000U;
    a->tx_inflight_bytes = 20000U;
}

static int check_startup_aggregation_and_probe(void)
{
    struct tcp_shift_bbr3_controller c;
    struct tcp_shift_bbr3_controller_ack a = {0};
    struct tcp_shift_bbr3_controller_policy p;
    uint64_t last = 0U;
    uint32_t i;

    CHECK(tcp_shift_bbr3_controller_init(&c, 10000U, 1000U, 17U, 1U) == 0);
    CHECK(c.model.phase == TCP_SHIFT_BBR3_STARTUP);
    CHECK(c.pacing_rate_bytes_per_sec == 27700000U);
    for (i = 0U; i < 4U; i++) {
        sample(&a, 2U + i, last, last + 1000U,
               10000U, 1000U, 100000000U);
        CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
        last += 1000U;
        CHECK(p.cwnd_bytes >= 4000U);
        CHECK(p.send_quantum_bytes >= 2000U);
        CHECK(p.extra_acked_bytes >= 1000U);
    }
    CHECK(c.model.full_bw_reached == 1U);
    CHECK(c.probe.initialized == 1U);
    CHECK(p.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(p.pacing_rate_bytes_per_sec > 0U);

    /* Wall-clock ProbeBW wait triggers REFILL after the steady DOWN phase. */
    sample(&a, a.now_ns + c.probe.wait_ns + 10U, last,
           last + 1000U, 10000U, 1000U, 100000000U);
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
    CHECK(p.phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL);
    sample(&a, a.now_ns + 1U, last + 1000U,
           last + 2000U, 10000U, 1000U, 100000000U);
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
    CHECK(p.phase == TCP_SHIFT_BBR3_PROBE_BW_UP);
    CHECK(p.max_inflight_bytes >= 4000U);
    CHECK(p.extra_acked_bytes <= c.cwnd_bytes);
    return 0;
}

static int check_probe_rtt_lifecycle(void)
{
    struct tcp_shift_bbr3_controller c;
    struct tcp_shift_bbr3_controller_ack a = {0};
    struct tcp_shift_bbr3_controller_policy p;
    uint64_t last = 0U;
    uint64_t start;
    uint32_t i;

    CHECK(tcp_shift_bbr3_controller_init(&c, 10000U, 1000U, 31U, 1U) == 0);
    for (i = 0U; i < 4U; i++) {
        sample(&a, 100U + i, last, last + 1000U,
               10000U, 1000U, 100000000U);
        CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
        last += 1000U;
    }
    CHECK(c.model.full_bw_reached == 1U);
    start = UINT64_C(6000000000);
    sample(&a, start, last, last + 1000U,
           10000U, 1000U, 120000000U);
    a.inflight_bytes = 2000U;
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
    CHECK(p.phase == TCP_SHIFT_BBR3_PROBE_RTT);
    CHECK(p.mark_app_limited == 1U);
    CHECK(c.probe_rtt_done_stamp_ns ==
          start + TCP_SHIFT_BBR3_PROBE_RTT_DURATION_NS);
    CHECK(p.cwnd_bytes == 4000U);
    CHECK(c.prior_cwnd_bytes > 4000U);

    sample(&a, start + UINT64_C(100000000), last + 1000U,
           last + 2000U, 9000U, 1000U, 120000000U);
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
    CHECK(c.probe_rtt_round_done == 1U);
    CHECK(p.phase == TCP_SHIFT_BBR3_PROBE_RTT);
    sample(&a, start + UINT64_C(200000001), last + 2000U,
           last + 3000U, 9000U, 1000U, 120000000U);
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
    CHECK(p.phase == TCP_SHIFT_BBR3_PROBE_BW_CRUISE);
    CHECK(p.mark_app_limited == 0U);
    CHECK(c.model.probe_rtt_min_stamp_ns == a.now_ns);
    CHECK(c.cwnd_bytes >= 4000U);
    return 0;
}

static int check_qualified_loss_and_fail_closed(void)
{
    struct tcp_shift_bbr3_controller c;
    struct tcp_shift_bbr3_controller_ack a = {0};
    struct tcp_shift_bbr3_controller_policy p;
    uint64_t before;
    uint64_t last = 0U;
    uint32_t i;

    CHECK(tcp_shift_bbr3_controller_init(&c, 10000U, 1000U, 47U, 1U) == 0);
    for (i = 0U; i < 4U; i++) {
        sample(&a, 100U + i, last, last + 1000U,
               10000U, 1000U, 100000000U);
        CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
        last += 1000U;
    }
    CHECK(c.model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    c.model.phase = TCP_SHIFT_BBR3_PROBE_BW_UP; /* synthesized test phase */
    before = c.model.last_update_ns;
    sample(&a, before + 1U, last, last + 1000U,
           10000U, 1000U, 100000000U);
    a.lost_bytes = 400U;
    a.probe_sample = 1U;
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) < 0);
    CHECK(c.model.last_update_ns == before);
    a.validated_loss = 1U;
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) == 0);
    CHECK(p.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(c.congestion.probe_feedback_consumed == 1U);

    /* Raw CE without negotiated ECN is never admitted as feedback. */
    sample(&a, a.now_ns + 1U, last + 1000U,
           last + 2000U, 10000U, 1000U, 100000000U);
    a.ce_bytes = 200U;
    before = c.model.last_update_ns;
    CHECK(tcp_shift_bbr3_controller_on_ack(&c, &a, &p) < 0);
    CHECK(c.model.last_update_ns == before);
    return 0;
}

int main(void)
{
    CHECK(check_startup_aggregation_and_probe() == 0);
    CHECK(check_probe_rtt_lifecycle() == 0);
    CHECK(check_qualified_loss_and_fail_closed() == 0);
    puts("bbr3_controller_contract=ok startup_drain=ok "
         "probe_bw=ok ack_aggregation=ok probe_rtt_200ms_round=ok "
         "loss_evidence_gate=ok separate_v1=ok");
    return 0;
}
