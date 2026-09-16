#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr_probe.h"

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "bbr-probe-policy: check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                              \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static void seed_probe_bw(struct tcp_shift_bbr_model *model)
{
    tcp_shift_bbr_model_init(model);
    model->max_bw_bytes_per_sec = UINT64_C(100000000);
    model->min_rtt_ns = UINT64_C(20000000);
    model->min_rtt_stamp_ns = UINT64_C(1);
    model->has_min_rtt = 1U;
    model->full_bw_reached = 1U;
    model->mode = TCP_SHIFT_BBR_MODE_PROBE_BW;
}

static struct tcp_shift_cc_transport transport(void)
{
    struct tcp_shift_cc_transport value;

    memset(&value, 0, sizeof(value));
    value.mss_bytes = 1460U;
    value.cwnd_limit_bytes = 10000000U;
    return value;
}

static struct tcp_shift_cc_rate_sample rate_sample(uint32_t inflight,
                                                    uint32_t flags,
                                                    uint64_t prior_delivered,
                                                    uint64_t delivered_total)
{
    struct tcp_shift_cc_rate_sample value;

    memset(&value, 0, sizeof(value));
    value.delivery_rate_bytes_per_sec = UINT64_C(100000000);
    value.rtt_ns = UINT64_C(20000000);
    value.prior_inflight_bytes = inflight;
    value.prior_delivered_bytes = prior_delivered;
    value.delivered_total_bytes = delivered_total;
    value.flags = flags;
    return value;
}

static int check_cycle_seed_and_gains(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_state probe;
    struct tcp_shift_cc_rate_sample sample;

    seed_probe_bw(&model);
    sample = rate_sample(2500000U, TCP_SHIFT_CC_RATE_SAMPLE_VALID,
                         0U, 1000U);

    tcp_shift_bbr_probe_state_init(&probe, 0U);
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(1000000000)) == 0);
    CHECK(probe.cycle_started == 1U);
    CHECK(probe.cycle_index == 0U);
    CHECK(tcp_shift_bbr_probe_bw_pacing_gain_num(&probe) == 320U);
    CHECK(tcp_shift_bbr_probe_bw_pacing_rate_bytes_per_sec(
              &model, &probe) == UINT64_C(123750000));

    /* Probe-up must persist for one min RTT and until its 1.25*BDP inflight
     * target is reached (or a retransmission signal says the path cannot hold
     * the target). Base BDP here is exactly 2 MB, so the target is 2.5 MB. */
    sample.prior_inflight_bytes = 2499999U;
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(1020000000)) == 0);
    CHECK(probe.cycle_index == 0U);

    sample.prior_inflight_bytes = 2500000U;
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(1021000000)) == 0);
    CHECK(probe.cycle_index == 1U);
    CHECK(tcp_shift_bbr_probe_bw_pacing_gain_num(&probe) == 192U);
    CHECK(tcp_shift_bbr_probe_bw_pacing_rate_bytes_per_sec(
              &model, &probe) == UINT64_C(74250000));

    /* Probe-down may end before a full min RTT once inflight reaches one BDP. */
    sample.prior_inflight_bytes = 2000000U;
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(1022000000)) == 0);
    CHECK(probe.cycle_index == 2U);
    CHECK(tcp_shift_bbr_probe_bw_pacing_gain_num(&probe) == 256U);
    CHECK(tcp_shift_bbr_probe_bw_pacing_rate_bytes_per_sec(
              &model, &probe) == UINT64_C(99000000));

    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(1041000000)) == 0);
    CHECK(probe.cycle_index == 2U);
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(1042000000)) == 0);
    CHECK(probe.cycle_index == 3U);

    /* Linux randomizes the initial cycle over probe-up or cruise phases and
     * never starts directly in probe-down. The pure-C state consumes an
     * external deterministic seed rather than owning an RNG. */
    tcp_shift_bbr_probe_state_init(&probe, 1U);
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(2000000000)) == 0);
    CHECK(probe.cycle_index == 7U);

    tcp_shift_bbr_probe_state_init(&probe, 6U);
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(3000000000)) == 0);
    CHECK(probe.cycle_index == 2U);

    /* RETRANSMITTED is the current transport-neutral loss proxy for ending a
     * full-length probe-up that cannot reach its 1.25*BDP target. */
    tcp_shift_bbr_probe_state_init(&probe, 0U);
    sample = rate_sample(2000000U,
                         TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                             TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED,
                         0U, 1000U);
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(4000000000)) == 0);
    CHECK(tcp_shift_bbr_probe_bw_update(
              &model, &probe, &sample, UINT64_C(4020000000)) == 0);
    CHECK(probe.cycle_index == 1U);
    return 0;
}

static int check_probe_bw_policy(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_state probe;
    struct tcp_shift_cc_transport tx = transport();
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;

    seed_probe_bw(&model);
    tcp_shift_bbr_probe_state_init(&probe, 0U);
    probe.cycle_started = 1U;
    probe.cycle_index = 2U;

    memset(&ack, 0, sizeof(ack));
    ack.acked_bytes = 1460U;

    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &tx, &ack, 5000000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 4000000U);
    CHECK(policy.ssthresh_bytes == 2000000U);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(99000000));

    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &tx, &ack, 100000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 101460U);

    model.max_bw_bytes_per_sec = 1U;
    model.min_rtt_ns = 1U;
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &tx, &ack, 1460U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 5840U);
    CHECK(policy.ssthresh_bytes == 5840U);

    model.mode = TCP_SHIFT_BBR_MODE_DRAIN;
    CHECK(tcp_shift_bbr_probe_bw_policy(
              &model, &probe, &tx, &ack, 5840U, &policy) < 0);
    return 0;
}

static int check_probe_rtt_lifecycle(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_bbr_probe_state probe;
    struct tcp_shift_cc_transport tx = transport();
    struct tcp_shift_cc_rate_sample sample;
    struct tcp_shift_cc_policy policy;
    const uint64_t enter_ns = UINT64_C(10000000000);
    const uint64_t arm_ns = UINT64_C(10010000000);
    const uint64_t done_ns = arm_ns + TCP_SHIFT_BBR_PROBE_RTT_DURATION_NS;

    seed_probe_bw(&model);
    tcp_shift_bbr_probe_state_init(&probe, 3U);
    model.min_rtt_expired = 1U;

    sample = rate_sample(10000U, TCP_SHIFT_CC_RATE_SAMPLE_VALID,
                         0U, 1000U);
    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 4000000U, enter_ns) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_RTT);
    CHECK(probe.probe_rtt_prior_cwnd_bytes == 4000000U);
    CHECK(probe.probe_rtt_done_stamp_ns == 0U);

    CHECK(tcp_shift_bbr_probe_rtt_policy(
              &model, &probe, &tx, &policy) == 0);
    CHECK(policy.cwnd_bytes == 5840U);
    CHECK(policy.ssthresh_bytes == 5840U);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(99000000));

    sample = rate_sample(5840U, TCP_SHIFT_CC_RATE_SAMPLE_VALID,
                         1000U, 2000U);
    model.round_start = 0U;
    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 5840U, arm_ns) == 0);
    CHECK(probe.probe_rtt_done_stamp_ns == done_ns);
    CHECK(model.next_round_delivered == 2000U);
    CHECK(probe.probe_rtt_round_done == 0U);

    /* A packet-timed round may finish before 200 ms, but both conditions are
     * required before ProbeRTT can exit. */
    model.round_start = 1U;
    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 5840U,
              UINT64_C(10050000000)) == 0);
    CHECK(probe.probe_rtt_round_done == 1U);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_RTT);

    model.round_start = 0U;
    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 5840U, done_ns - 1U) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_RTT);

    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 5840U, done_ns) == 1);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);
    CHECK(model.min_rtt_expired == 0U);
    CHECK(model.min_rtt_stamp_ns == done_ns);
    CHECK(probe.cycle_started == 1U);
    CHECK(probe.cycle_index == 5U);
    CHECK(tcp_shift_bbr_probe_rtt_restore_cwnd_bytes(
              &probe, 5840U, tx.cwnd_limit_bytes) == 4000000U);

    /* If full pipe was not established before ProbeRTT, Linux BBRv1 returns to
     * STARTUP rather than pretending ProbeBW is already justified. */
    tcp_shift_bbr_model_init(&model);
    model.max_bw_bytes_per_sec = UINT64_C(100000000);
    model.min_rtt_ns = UINT64_C(20000000);
    model.has_min_rtt = 1U;
    model.min_rtt_expired = 1U;
    tcp_shift_bbr_probe_state_init(&probe, 0U);
    sample = rate_sample(5840U, TCP_SHIFT_CC_RATE_SAMPLE_VALID,
                         0U, 1000U);
    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 100000U, enter_ns) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_RTT);
    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 5840U, arm_ns) == 0);
    model.round_start = 1U;
    CHECK(tcp_shift_bbr_probe_rtt_update(
              &model, &probe, &tx, &sample, 5840U, done_ns) == 1);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_STARTUP);
    return 0;
}

int main(void)
{
    CHECK(check_cycle_seed_and_gains() == 0);
    CHECK(check_probe_bw_policy() == 0);
    CHECK(check_probe_rtt_lifecycle() == 0);

    printf("bbr_probe_policy=ok cycle=320,192,256 cwnd_gain=512/256 "
           "probe_rtt=200ms transition=probebw-probertt-probebw "
           "cycle_seed=external\n");
    return 0;
}
