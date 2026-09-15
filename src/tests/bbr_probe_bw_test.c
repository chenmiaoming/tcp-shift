#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr_probe_bw.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "bbr-probe-bw: check failed at %s:%d: %s\n",    \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static void seed_probe_bw(struct tcp_shift_bbr_model *model)
{
    tcp_shift_bbr_model_init(model);
    model->mode = TCP_SHIFT_BBR_MODE_PROBE_BW;
    model->max_bw_bytes_per_sec = UINT64_C(100000000);
    model->min_rtt_ns = UINT64_C(20000000);
    model->has_min_rtt = 1U;
    model->full_bw_reached = 1U;
}

static struct tcp_shift_cc_rate_sample inflight_sample(uint32_t inflight,
                                                        uint32_t flags)
{
    struct tcp_shift_cc_rate_sample sample;

    memset(&sample, 0, sizeof(sample));
    sample.prior_inflight_bytes = inflight;
    sample.flags = flags;
    return sample;
}

static int check_gain_contract(void)
{
    uint32_t index;

    CHECK(tcp_shift_bbr_probe_bw_pacing_gain_num(0U) == 320U);
    CHECK(tcp_shift_bbr_probe_bw_pacing_gain_num(1U) == 192U);
    for (index = 2U; index < TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN; index++) {
        CHECK(tcp_shift_bbr_probe_bw_pacing_gain_num(index) == 256U);
    }
    CHECK(tcp_shift_bbr_probe_bw_pacing_gain_num(8U) == 0U);
    return 0;
}

static int check_cycle_contract(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_bw_state probe;
    struct tcp_shift_cc_rate_sample sample;
    uint64_t now;
    uint32_t index;

    seed_probe_bw(&model);
    memset(&probe, 0, sizeof(probe));

    /* Linux reset randomization never starts directly in the 3/4 drain phase;
     * entropy remains caller-owned, but the pure helper preserves that rule. */
    CHECK(tcp_shift_bbr_probe_bw_init(&model, &probe, 1U, 0U) < 0);
    CHECK(tcp_shift_bbr_probe_bw_init(&model, &probe, 0U, 0U) == 0);
    CHECK(probe.cycle_index == 0U);

    sample = inflight_sample(2500000U, TCP_SHIFT_CC_RATE_SAMPLE_VALID);

    /* Strictly more than one minRTT is required. At exactly one minRTT the
     * 5/4 probe phase remains active even after reaching its 2.5 MB target. */
    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &sample, UINT64_C(20000000), 0U) == 0);
    CHECK(probe.cycle_index == 0U);

    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &sample, UINT64_C(20000001), 0U) == 0);
    CHECK(probe.cycle_index == 1U);
    CHECK(probe.cycle_start_ns == UINT64_C(20000001));

    /* The 3/4 phase may finish before one minRTT once inflight has drained to
     * the base 1*BDP target (2 MB here). */
    sample.prior_inflight_bytes = 2000000U;
    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &sample, UINT64_C(20000002), 0U) == 0);
    CHECK(probe.cycle_index == 2U);

    /* Cruise phases advance on wall-clock minRTT only, with the same strict
     * greater-than boundary used by Linux BBR. */
    now = probe.cycle_start_ns + UINT64_C(20000000);
    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &sample, now, 0U) == 0);
    CHECK(probe.cycle_index == 2U);
    now++;
    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &sample, now, 0U) == 0);
    CHECK(probe.cycle_index == 3U);

    /* Walk the remaining five 1x phases and prove the eight-phase ring wraps
     * from phase 7 back to phase 0. */
    for (index = 3U; index <= 7U; index++) {
        now = probe.cycle_start_ns + UINT64_C(20000001);
        CHECK(tcp_shift_bbr_probe_bw_update_cycle(
                  &model, &probe, &sample, now, 0U) == 0);
        CHECK(probe.cycle_index == ((index + 1U) & 7U));
    }
    CHECK(probe.cycle_index == 0U);

    /* A current-sample loss signal can end a full-length 5/4 probe even when
     * the compact prior-inflight observation did not reach 1.25*BDP. */
    CHECK(tcp_shift_bbr_probe_bw_init(&model, &probe, 0U, now) == 0);
    sample.prior_inflight_bytes = 1000000U;
    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &sample, now + UINT64_C(20000001), 1U) == 0);
    CHECK(probe.cycle_index == 1U);

    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &sample, now, 0U) < 0);
    return 0;
}

static int check_policy_contract(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_bw_state probe;
    struct tcp_shift_cc_rate_sample rate;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;
    uint64_t now;

    seed_probe_bw(&model);
    memset(&probe, 0, sizeof(probe));
    CHECK(tcp_shift_bbr_probe_bw_init(&model, &probe, 0U, 0U) == 0);

    memset(&transport, 0, sizeof(transport));
    transport.mss_bytes = 1460U;
    transport.cwnd_limit_bytes = 10000000U;

    memset(&ack, 0, sizeof(ack));
    ack.acked_bytes = 1460U;

    /* 100 MB/s * 5/4 * 99/100 = 123.75 MB/s. Steady-state cwnd gain is 2,
     * so 2 MB base BDP yields a 4 MB cwnd target. */
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &transport, &ack, 5000000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 4000000U);
    CHECK(policy.ssthresh_bytes == 2000000U);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(123750000));

    rate = inflight_sample(2500000U, TCP_SHIFT_CC_RATE_SAMPLE_VALID);
    now = UINT64_C(20000001);
    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &rate, now, 0U) == 0);
    CHECK(probe.cycle_index == 1U);
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &transport, &ack, 3000000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 3001460U);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(74250000));

    rate.prior_inflight_bytes = 2000000U;
    now++;
    CHECK(tcp_shift_bbr_probe_bw_update_cycle(
              &model, &probe, &rate, now, 0U) == 0);
    CHECK(probe.cycle_index == 2U);
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &transport, &ack, 3000000U, &policy) == 0);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(99000000));

    /* Republish without newly ACKed bytes: do not snap an oversized cwnd down
     * merely because a policy snapshot was requested. */
    ack.acked_bytes = 0U;
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &transport, &ack, 5000000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 5000000U);

    /* Tiny BDP is still bounded by four MSS. */
    model.max_bw_bytes_per_sec = 1U;
    model.min_rtt_ns = 1U;
    ack.acked_bytes = 1U;
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &transport, &ack, 1460U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 5840U);
    CHECK(policy.ssthresh_bytes == 5840U);

    model.mode = TCP_SHIFT_BBR_MODE_DRAIN;
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &transport, &ack, 5840U, &policy) < 0);
    return 0;
}

int main(void)
{
    CHECK(check_gain_contract() == 0);
    CHECK(check_cycle_contract() == 0);
    CHECK(check_policy_contract() == 0);

    printf("bbr_probe_bw=ok cycle=5/4,3/4,1,1,1,1,1,1 "
           "cwnd_gain=2 pacing_margin=99/100 timer=min_rtt inflight=prior_bytes\n");
    return 0;
}
