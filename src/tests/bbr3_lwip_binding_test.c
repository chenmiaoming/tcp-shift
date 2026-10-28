#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/bbr3_binding.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "bbr3-lwip-binding:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void prepare_ack(struct tcp_shift_cc_ack *a, uint64_t now_ns,
                        uint64_t prior, uint64_t total, uint32_t newly,
                        uint32_t sample_inflight)
{
    memset(a, 0, sizeof(*a));
    a->ack_time_ns = now_ns;
    a->acked_bytes = newly;
    a->rate.flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                    TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
    a->rate.interval_ns = UINT64_C(200000000);
    a->rate.rtt_ns = UINT64_C(200000000);
    a->rate.delivery_rate_bytes_per_sec = 14600U;
    a->rate.prior_delivered_bytes = prior;
    a->rate.delivered_total_bytes = total;
    a->rate.delivered_bytes = (uint32_t)(total - prior);
    a->rate.prior_inflight_bytes = sample_inflight;
}

static int test_binding_and_sack_sampling(void)
{
    struct tcp_pcb pcb;
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_bbr3_binding binding = {0};
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_bbr3_controller_policy policy;
    uint64_t expected_total = 0U;
    uint32_t initial_cwnd;
    uint32_t i;

    memset(&pcb, 0, sizeof(pcb));
    memset(&adapter, 0, sizeof(adapter));
    pcb.mss = 1460U;
    pcb.cwnd = 14600U;
    pcb.snd_nxt = 100000U;
    pcb.lastack = 85400U;
    adapter.pcb = &pcb;
    adapter.bound = 1U;

    /* Without the actual SACK-aware delivery policy, lwIP ACKed bytes are
     * sequence-space ACK progress, not necessarily newly delivered payload. */
    CHECK(tcp_shift_lwip_bbr3_binding_init(
              &binding, &adapter, &pcb, 7U, 1U) < 0);
    adapter.sack_delivery_policy = 1U;
    CHECK(tcp_shift_lwip_bbr3_binding_init(
              &binding, &adapter, &pcb, 7U, 1U) == 0);
    CHECK(tcp_shift_lwip_bbr3_binding_init(
              &binding, &adapter, &pcb, 7U, 1U) < 0);
    initial_cwnd = pcb.cwnd;

    for (i = 0U; i < 6U; i++) {
        uint64_t newly = 1460U;
        uint64_t prev = expected_total;

        expected_total += newly;
        prepare_ack(&ack, UINT64_C(200000001) * (i + 1U),
                    prev, expected_total, (uint32_t)newly, 14600U);
        adapter.delivered_bytes = expected_total;
        CHECK(tcp_shift_lwip_bbr3_binding_on_ack(
                  &binding, &ack, &policy) == 0);
        CHECK(policy.cwnd_bytes >= 5840U);
        CHECK(policy.pacing_rate_bytes_per_sec > 0U);
        CHECK(binding.accepted_samples == i + 1U);
        CHECK(binding.last_delivered_bytes == expected_total);
        CHECK(pcb.cwnd == initial_cwnd);
        CHECK(pcb.ssthresh == 0U);
        CHECK(adapter.pacing_rate_bytes_per_sec == 0U);
    }

    /* SACK-only ACK can newly deliver 1460 bytes without advancing SND.UNA.
     * The full rate sample can cover several packets; never alias it to the
     * bytes newly delivered by this ACK. */
    prepare_ack(&ack, ack.ack_time_ns + 1000U,
                1460U, expected_total + 1460U, 1460U, 14600U);
    adapter.delivered_bytes = ack.rate.delivered_total_bytes;
    CHECK(ack.rate.delivered_bytes > ack.acked_bytes);
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(
              &binding, &ack, &policy) == 0);
    CHECK(binding.last_delivered_bytes == expected_total + 1460U);

    /* Double reporting of the same packet via SACK then cumulative ACK
     * must never be fed as new ACK aggregation credit. */
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(
              &binding, &ack, &policy) < 0);
    CHECK(binding.accepted_samples == 7U);
    CHECK(binding.rejected_samples == 1U);

    /* Missing sampled in-flight provenance fails closed. */
    prepare_ack(&ack, ack.ack_time_ns + 1000U,
                expected_total, expected_total + 2920U, 1460U, 0U);
    adapter.delivered_bytes = ack.rate.delivered_total_bytes;
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(
              &binding, &ack, &policy) < 0);

    /* No loss/CE can be injected just because the old hooks received their
     * coarse-grained on_loss/on_ecn callbacks; binding has no such API. */
    CHECK(binding.controller.congestion.loss_in_round == 0U);
    CHECK(binding.controller.congestion.ecn_in_round == 0U);
    CHECK(binding.controller.congestion.inflight_longterm_bytes == UINT64_MAX);
    CHECK(pcb.cwnd == initial_cwnd);
    return 0;
}

static int test_rebinding_and_bad_data(void)
{
    struct tcp_pcb pcb;
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_bbr3_binding binding = {0};
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_bbr3_controller_policy p;

    memset(&pcb, 0, sizeof(pcb));
    memset(&adapter, 0, sizeof(adapter));
    pcb.mss = 1000U;
    pcb.cwnd = 10000U;
    adapter.pcb = &pcb;
    adapter.bound = 1U;
    adapter.sack_delivery_policy = 1U;
    CHECK(tcp_shift_lwip_bbr3_binding_init(
              &binding, &adapter, &pcb, 41U, 2U) == 0);

    prepare_ack(&ack, 100U, 0U, 1000U, 1000U, 10000U);
    adapter.delivered_bytes = 1000U;
    ack.rate.flags = 0U;
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(&binding, &ack, &p) < 0);
    CHECK(binding.last_delivered_bytes == 0U);
    ack.rate.flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    ack.acked_bytes = 999U;
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(&binding, &ack, &p) < 0);
    ack.acked_bytes = 1000U;
    pcb.mss = 1200U;
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(&binding, &ack, &p) < 0);
    pcb.mss = 1000U;
    adapter.pcb = NULL;
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(&binding, &ack, &p) < 0);
    adapter.pcb = &pcb;
    CHECK(tcp_shift_lwip_bbr3_binding_on_ack(&binding, &ack, &p) == 0);
    CHECK(binding.last_delivered_bytes == 1000U);
    CHECK(binding.accepted_samples == 1U);
    CHECK(binding.rejected_samples == 2U);
    return 0;
}

int main(void)
{
    CHECK(test_binding_and_sack_sampling() == 0);
    CHECK(test_rebinding_and_bad_data() == 0);
    puts("bbr3_lwip_binding_contract=ok sack_once=ok "
         "rs_delivered_separate=ok evidence_guard=ok "
         "pcb_unmodified=ok selector_unchanged=ok");
    return 0;
}
