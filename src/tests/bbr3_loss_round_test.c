#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/bbr3_loss_round.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "bbr3-loss-round:%d: %s\n", __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static struct tcp_shift_bbr3_rack_loss sample(
    uint64_t generation, uint64_t delivered_at_tx,
    uint32_t inflight, uint16_t lost, uint8_t probe)
{
    struct tcp_shift_bbr3_rack_loss event;

    memset(&event, 0, sizeof(event));
    event.generation = generation;
    event.tx_ns = generation * 1000U;
    event.delivered_at_tx = delivered_at_tx;
    event.tx_inflight_bytes = inflight;
    event.newly_lost_bytes = lost;
    event.probe_sample = probe;
    return event;
}

static int same_context_probe_threshold(void)
{
    struct tcp_shift_bbr3_loss_round round = {0};
    struct tcp_shift_bbr3_loss_round_result result = {0};
    struct tcp_shift_bbr3_rack_loss a = sample(1U, 1000U, 10000U, 101U, 1U);
    struct tcp_shift_bbr3_rack_loss b = sample(2U, 1000U, 10000U, 102U, 1U);

    CHECK(tcp_shift_bbr3_loss_round_start(&round, 5000U) == 0);
    CHECK(tcp_shift_bbr3_loss_round_start(&round, 6000U) < 0);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &a) == 0);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &a) < 0);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &b) == 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 6000U, 0U, &result) < 0);
    CHECK(round.active == 1U);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 6000U, 1U, &result) == 0);
    CHECK(result.events == 2U);
    CHECK(result.newly_lost_bytes == 203U);
    CHECK(result.tx_inflight_bytes == 10000U);
    CHECK(result.same_send_snapshot == 1U);
    CHECK(result.loss_fraction_valid == 1U);
    CHECK(result.qualified_probe_excess_loss == 1U);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 7000U, 1U, &result) < 0);

    CHECK(tcp_shift_bbr3_loss_round_start(&round, 6000U) == 0);
    a = sample(3U, 2000U, 10000U, 200U, 1U);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &a) == 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 7000U, 1U, &result) == 0);
    CHECK(result.qualified_probe_excess_loss == 0U);
    return 0;
}

static int context_mismatch_and_unqualified_provenance(void)
{
    struct tcp_shift_bbr3_loss_round round = {0};
    struct tcp_shift_bbr3_loss_round_result result = {0};
    struct tcp_shift_bbr3_rack_loss a = sample(5U, 100U, 10000U, 300U, 1U);
    struct tcp_shift_bbr3_rack_loss b = sample(6U, 200U, 10000U, 300U, 1U);

    CHECK(tcp_shift_bbr3_loss_round_start(&round, 10U) == 0);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &a) == 0);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &b) == 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 20U, 1U, &result) == 0);
    CHECK(result.newly_lost_bytes == 600U);
    CHECK(result.same_send_snapshot == 0U);
    CHECK(result.tx_inflight_bytes == 0U);
    CHECK(result.loss_fraction_valid == 0U);
    CHECK(result.qualified_probe_excess_loss == 0U);

    CHECK(tcp_shift_bbr3_loss_round_start(&round, 20U) == 0);
    a = sample(7U, 500U, 10000U, 300U, 0U);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &a) == 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 30U, 1U, &result) == 0);
    CHECK(result.loss_fraction_valid == 1U);
    CHECK(result.qualified_probe_excess_loss == 0U);
    return 0;
}

static int validation_and_empty_round(void)
{
    struct tcp_shift_bbr3_loss_round round = {0};
    struct tcp_shift_bbr3_loss_round_result result = {0};
    struct tcp_shift_bbr3_rack_loss a = sample(1U, 0U, 500U, 600U, 1U);

    CHECK(tcp_shift_bbr3_loss_round_start(&round, 0U) == 0);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &a) < 0);
    a.tx_inflight_bytes = 1000U;
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &a) == 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 0U, 1U, &result) < 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 1U, 1U, &result) == 0);
    CHECK(result.qualified_probe_excess_loss == 1U);

    CHECK(tcp_shift_bbr3_loss_round_start(&round, 1U) == 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
        &round, 2U, 1U, &result) == 0);
    CHECK(result.events == 0U);
    CHECK(result.newly_lost_bytes == 0U);
    CHECK(result.loss_fraction_valid == 0U);
    CHECK(result.qualified_probe_excess_loss == 0U);
    return 0;
}

int main(void)
{
    CHECK(same_context_probe_threshold() == 0);
    CHECK(context_mismatch_and_unqualified_provenance() == 0);
    CHECK(validation_and_empty_round() == 0);
    puts("bbr3_loss_round_contract=ok compatible_flight=ok "
         "mixed_context_fail_closed=ok threshold_gt_2pct=ok "
         "probe_provenance_gate=ok packet_round_gate=ok");
    return 0;
}
