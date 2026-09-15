#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "bbr-drain-policy: check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static void seed_full_pipe(struct tcp_shift_bbr_model *model)
{
    tcp_shift_bbr_model_init(model);
    model->max_bw_bytes_per_sec = UINT64_C(100000000);
    model->min_rtt_ns = UINT64_C(20000000);
    model->has_min_rtt = 1U;
    model->full_bw_reached = 1U;
}

static struct tcp_shift_cc_rate_sample observed_round(
    uint64_t rate,
    uint64_t prior_delivered,
    uint64_t delivered_total,
    uint32_t prior_inflight)
{
    struct tcp_shift_cc_rate_sample sample;

    memset(&sample, 0, sizeof(sample));
    sample.delivery_rate_bytes_per_sec = rate;
    sample.rtt_ns = UINT64_C(20000000);
    sample.prior_delivered_bytes = prior_delivered;
    sample.delivered_total_bytes = delivered_total;
    sample.prior_inflight_bytes = prior_inflight;
    sample.flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                   TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
    return sample;
}

static int check_drain_arithmetic(void)
{
    CHECK(TCP_SHIFT_BBR_DRAIN_GAIN_NUM == 88U);
    CHECK(tcp_shift_bbr_drain_pacing_rate_bytes_per_sec(
              UINT64_C(100000000)) == UINT64_C(34031250));
    CHECK(tcp_shift_bbr_drain_pacing_rate_bytes_per_sec(0U) == 0U);
    /* Drain gain is below one, so even UINT64_MAX scales down rather than
     * saturating. Keep the exact floor result as an overflow regression. */
    CHECK(tcp_shift_bbr_drain_pacing_rate_bytes_per_sec(UINT64_MAX) ==
          UINT64_C(6277657592584281783));
    return 0;
}

static int check_mode_transition(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_cc_rate_sample rate;

    seed_full_pipe(&model);
    memset(&rate, 0, sizeof(rate));
    rate.flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    rate.prior_inflight_bytes = 3000000U;

    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_DRAIN);

    rate.prior_inflight_bytes = 2000001U;
    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_DRAIN);

    rate.prior_inflight_bytes = 2000000U;
    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);

    /* Linux BBR's drain check deliberately falls through: if the pipe is
     * already at or below one BDP on the ACK that declares full pipe, compact
     * BBR may transition STARTUP -> DRAIN -> PROBE_BW in one call too. */
    seed_full_pipe(&model);
    rate.prior_inflight_bytes = 1500000U;
    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);

    /* Missing min RTT or an invalid rate sample may enter DRAIN once full pipe
     * is known, but must not fabricate enough information to exit it. */
    seed_full_pipe(&model);
    model.has_min_rtt = 0U;
    model.min_rtt_ns = 0U;
    rate.prior_inflight_bytes = 0U;
    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_DRAIN);

    seed_full_pipe(&model);
    rate.flags = 0U;
    rate.prior_inflight_bytes = 0U;
    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_DRAIN);

    CHECK(tcp_shift_bbr_model_check_drain(NULL, &rate) < 0);
    CHECK(tcp_shift_bbr_model_check_drain(&model, NULL) < 0);
    return 0;
}

static int check_full_pipe_to_drain_integration(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_cc_rate_sample rate;

    tcp_shift_bbr_model_init(&model);

    rate = observed_round(UINT64_C(100000000), 0U, 1000U, 4000000U);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 1U) == 0);
    CHECK(model.full_bw_bytes_per_sec == UINT64_C(100000000));
    CHECK(model.full_bw_count == 0U);

    rate = observed_round(UINT64_C(125000000), 1000U, 2000U, 4000000U);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 2U) == 0);
    CHECK(model.full_bw_bytes_per_sec == UINT64_C(125000000));
    CHECK(model.full_bw_count == 0U);

    rate = observed_round(UINT64_C(150000000), 2000U, 3000U, 4000000U);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 3U) == 0);
    CHECK(model.full_bw_count == 1U);

    rate = observed_round(UINT64_C(155000000), 3000U, 4000U, 4000000U);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 4U) == 0);
    CHECK(model.full_bw_count == 2U);

    /* The third below-threshold non-app-limited round latches full pipe. With
     * 156 MB/s and 20 ms min RTT the compact drain target is 3.12 MB, so the
     * 4 MB prior inflight observation must enter and remain in DRAIN. */
    rate = observed_round(UINT64_C(156000000), 4000U, 5000U, 4000000U);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 5U) == 0);
    CHECK(model.full_bw_count == TCP_SHIFT_BBR_FULL_BW_ROUNDS);
    CHECK(model.full_bw_reached == 1U);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_STARTUP);
    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_DRAIN);
    CHECK(tcp_shift_bbr_bdp_bytes(model.max_bw_bytes_per_sec,
                                  model.min_rtt_ns) == UINT64_C(3120000));

    /* A later valid ACK reports 3 MB prior inflight, below one BDP, so DRAIN
     * completes and ProbeBW becomes the next mode. ProbeBW cycling itself is
     * intentionally a later checkpoint. */
    rate = observed_round(UINT64_C(156000000), 5000U, 6000U, 3000000U);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 6U) == 0);
    CHECK(tcp_shift_bbr_model_check_drain(&model, &rate) == 0);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);
    return 0;
}

static int check_drain_policy(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;

    seed_full_pipe(&model);
    model.mode = TCP_SHIFT_BBR_MODE_DRAIN;

    memset(&transport, 0, sizeof(transport));
    transport.mss_bytes = 1460U;
    transport.cwnd_limit_bytes = 10000000U;

    memset(&ack, 0, sizeof(ack));
    ack.acked_bytes = 1460U;

    /* DRAIN keeps the high-gain cwnd budget but intentionally drops pacing
     * from Startup gain to 88/256. Since full pipe is latched, a cwnd above
     * the high-gain target snaps down on an ACK. */
    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, 6000000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 5773438U);
    CHECK(policy.ssthresh_bytes == 2000000U);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(34031250));

    /* Below target, ACKs still release cwnd toward the retained high-gain
     * budget; DRAIN is a pacing reduction, not an arbitrary cwnd collapse. */
    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, 100000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 101460U);

    /* No fully ACKed bytes means do not snap cwnd down merely because policy
     * is being republished. */
    ack.acked_bytes = 0U;
    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, 6000000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 6000000U);

    /* The four-packet minimum and representable transport cap remain hard
     * bounds even with a tiny modeled BDP. */
    model.max_bw_bytes_per_sec = 1U;
    model.min_rtt_ns = 1U;
    ack.acked_bytes = 1U;
    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, 1460U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 5840U);
    CHECK(policy.ssthresh_bytes == 5840U);

    transport.cwnd_limit_bytes = 5000U;
    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, 5000U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 5000U);
    CHECK(policy.ssthresh_bytes == 5000U);

    model.mode = TCP_SHIFT_BBR_MODE_PROBE_BW;
    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, 5000U, &policy) < 0);
    model.mode = TCP_SHIFT_BBR_MODE_DRAIN;
    model.has_min_rtt = 0U;
    CHECK(tcp_shift_bbr_drain_policy(
              &model, &transport, &ack, 5000U, &policy) < 0);
    return 0;
}

int main(void)
{
    CHECK(check_drain_arithmetic() == 0);
    CHECK(check_mode_transition() == 0);
    CHECK(check_full_pipe_to_drain_integration() == 0);
    CHECK(check_drain_policy() == 0);

    printf("bbr_drain_policy=ok pacing_gain=88/256 pacing_margin=99/100 "
           "cwnd_gain=739/256 drain_target=1bdp transition=startup-drain-probebw "
           "full_pipe_detector=integrated\n");
    return 0;
}
