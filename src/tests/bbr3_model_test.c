#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr3_model.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "bbr3-model: %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static struct tcp_shift_cc_rate_sample rate_sample(
    uint64_t bw, uint64_t rtt, uint64_t prior, uint64_t total,
    uint32_t flags)
{
    struct tcp_shift_cc_rate_sample result;
    memset(&result, 0, sizeof(result));
    result.delivery_rate_bytes_per_sec = bw;
    result.rtt_ns = rtt;
    result.prior_delivered_bytes = prior;
    result.delivered_total_bytes = total;
    result.flags = flags;
    return result;
}

static int check_cycle_filter(void)
{
    struct tcp_shift_bbr3_model model;
    struct tcp_shift_cc_rate_sample sample;
    uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;

    tcp_shift_bbr3_model_init(&model);
    CHECK(model.phase == TCP_SHIFT_BBR3_STARTUP);
    CHECK(model.max_bw_bytes_per_sec == 0U);
    sample = rate_sample(1000U, 0U, 0U, 1000U, valid);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, 1U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 1000U);
    CHECK(model.round_count == 1U);
    CHECK(model.cycle_count == 0U);

    /* Packet-round updates must NOT age BBRv3 max_bw; cycles do that. */
    sample = rate_sample(100U, 0U, 1000U, 2000U, valid);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, 2U) == 0);
    CHECK(model.round_count == 2U);
    CHECK(model.max_bw_bytes_per_sec == 1000U);
    CHECK(model.cycle_count == 0U);

    CHECK(tcp_shift_bbr3_model_advance_bw_cycle(&model) == 0);
    CHECK(model.cycle_count == 1U);
    CHECK(model.max_bw_bytes_per_sec == 1000U);
    sample = rate_sample(900U, 0U, 2000U, 3000U, valid);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, 3U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 1000U);

    /* Third slot reuses cycle zero, expiring its old peak. */
    CHECK(tcp_shift_bbr3_model_advance_bw_cycle(&model) == 0);
    CHECK(model.cycle_count == 2U);
    CHECK(model.max_bw_bytes_per_sec == 900U);
    sample = rate_sample(100U, 0U, 3000U, 4000U,
                         valid | TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, 4U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 900U);
    sample = rate_sample(1100U, 0U, 4000U, 5000U,
                         valid | TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, 5U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 1100U);

    /* Invalid monotonicity must not modify estimator state. */
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, 4U) < 0);
    CHECK(model.max_bw_bytes_per_sec == 1100U);
    CHECK(model.cycle_count == 2U);
    return 0;
}

static int check_rtt_and_full_bw(void)
{
    struct tcp_shift_bbr3_model model;
    struct tcp_shift_cc_rate_sample sample;
    uint32_t flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                     TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
    uint32_t round;

    tcp_shift_bbr3_model_init(&model);
    sample = rate_sample(1000U, 200000000U, 0U, 1000U, flags);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, 1U) == 0);
    CHECK(model.min_rtt_ns == 200000000U);
    CHECK(model.probe_rtt_min_delay_ns == 200000000U);
    CHECK(model.full_bw_bytes_per_sec == 1000U);

    for (round = 2U; round <= 4U; round++) {
        sample = rate_sample(1100U, 220000000U,
                             (uint64_t)(round - 1U) * 1000U,
                             (uint64_t)round * 1000U, flags);
        CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample, round) == 0);
    }
    CHECK(model.full_bw_count == TCP_SHIFT_BBR3_FULL_BW_ROUNDS);
    CHECK(model.full_bw_reached == 1U);

    sample = rate_sample(900U, 240000000U, 4000U, 5000U, flags);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample,
                                      UINT64_C(6000000000)) == 0);
    CHECK(model.probe_rtt_expired == 1U);
    CHECK(model.probe_rtt_min_delay_ns == 240000000U);
    CHECK(model.min_rtt_ns == 200000000U);
    sample = rate_sample(900U, 250000000U, 5000U, 6000U, flags);
    CHECK(tcp_shift_bbr3_model_on_ack(&model, &sample,
                                      UINT64_C(12000000000)) == 0);
    CHECK(model.min_rtt_ns == 250000000U);
    return 0;
}

static int check_gain_and_headroom(void)
{
    struct tcp_shift_bbr3_gain gain;

    gain = tcp_shift_bbr3_pacing_gain(TCP_SHIFT_BBR3_STARTUP);
    CHECK(tcp_shift_bbr3_scale(1000U, gain) == 2770U);
    gain = tcp_shift_bbr3_pacing_gain(TCP_SHIFT_BBR3_DRAIN);
    CHECK(tcp_shift_bbr3_scale(1000U, gain) == 500U);
    gain = tcp_shift_bbr3_pacing_gain(TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(tcp_shift_bbr3_scale(1000U, gain) == 900U);
    gain = tcp_shift_bbr3_pacing_gain(TCP_SHIFT_BBR3_PROBE_BW_CRUISE);
    CHECK(tcp_shift_bbr3_scale(1000U, gain) == 1000U);
    gain = tcp_shift_bbr3_pacing_gain(TCP_SHIFT_BBR3_PROBE_BW_REFILL);
    CHECK(tcp_shift_bbr3_scale(1000U, gain) == 1000U);
    gain = tcp_shift_bbr3_pacing_gain(TCP_SHIFT_BBR3_PROBE_BW_UP);
    CHECK(tcp_shift_bbr3_scale(1000U, gain) == 1250U);
    gain = tcp_shift_bbr3_cwnd_gain(TCP_SHIFT_BBR3_PROBE_RTT);
    CHECK(tcp_shift_bbr3_scale(1000U, gain) == 500U);
    CHECK(tcp_shift_bbr3_inflight_with_headroom(UINT64_MAX, 1460U) ==
          UINT64_MAX);
    CHECK(tcp_shift_bbr3_inflight_with_headroom(100000U, 1460U) == 85000U);
    CHECK(tcp_shift_bbr3_inflight_with_headroom(3000U, 1460U) == 5840U);
    CHECK(tcp_shift_bbr3_scale(UINT64_MAX,
                               (struct tcp_shift_bbr3_gain){277U, 100U}) ==
          UINT64_MAX);
    return 0;
}

int main(void)
{
    CHECK(check_cycle_filter() == 0);
    CHECK(check_rtt_and_full_bw() == 0);
    CHECK(check_gain_and_headroom() == 0);
    puts("bbr3_model_contract=ok separate_v1=ok cycle_filter=2 "
         "full_bw_rounds=3 rtt_windows=5s,10s gains=ok headroom=15pct");
    return 0;
}
