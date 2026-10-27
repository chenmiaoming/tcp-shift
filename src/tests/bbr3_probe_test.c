#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr3_probe.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "bbr3-probe:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void setup(struct tcp_shift_bbr3_model *model,
                  struct tcp_shift_bbr3_probe *probe)
{
    tcp_shift_bbr3_model_init(model);
    memset(probe, 0, sizeof(*probe));
    model->max_bw_bytes_per_sec = 10000U;
    model->max_bw_filter[0] = 10000U;
    model->min_rtt_ns = 100000000U;
    model->has_min_rtt = 1U;
    model->full_bw_reached = 1U;
}

static struct tcp_shift_cc_rate_sample rate(
    uint64_t before, uint64_t after, uint64_t bw, uint32_t flags)
{
    struct tcp_shift_cc_rate_sample result;

    memset(&result, 0, sizeof(result));
    result.prior_delivered_bytes = before;
    result.delivered_total_bytes = after;
    result.delivery_rate_bytes_per_sec = bw;
    result.flags = flags;
    return result;
}

static struct tcp_shift_bbr3_probe_ack feedback(
    const struct tcp_shift_cc_rate_sample *sample, uint64_t time,
    uint32_t inflight, uint64_t inflight_hi)
{
    struct tcp_shift_bbr3_probe_ack ack;

    memset(&ack, 0, sizeof(ack));
    ack.rate = sample;
    ack.now_ns = time;
    ack.inflight_bytes = inflight;
    ack.inflight_longterm_bytes = inflight_hi;
    ack.cwnd_bytes = 4000U;
    ack.mss_bytes = 100U;
    return ack;
}

static int test_full_cycle_and_feedback_aging(void)
{
    struct tcp_shift_bbr3_model model;
    struct tcp_shift_bbr3_probe probe;
    struct tcp_shift_cc_rate_sample sample;
    struct tcp_shift_bbr3_probe_ack ack;
    const uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    uint64_t now = UINT64_C(1000);
    uint64_t delivered = 1000U;
    unsigned i;

    setup(&model, &probe);
    CHECK(tcp_shift_bbr3_probe_enter(&probe, &model,
                                     now, delivered, 0U) < 0);
    CHECK(tcp_shift_bbr3_probe_enter(&probe, &model,
                                     now, delivered, 77U) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(probe.wait_ns >= UINT64_C(2000000000));
    CHECK(probe.wait_ns < UINT64_C(3000000000));
    CHECK(probe.rounds_since_probe <= 1U);
    CHECK(tcp_shift_bbr3_probe_enter(&probe, &model,
                                     now, delivered, 1U) < 0);

    sample = rate(900U, 1100U, 10000U, valid);
    ack = feedback(&sample, now + 1U, 1200U, 1300U);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(model.cycle_count == 0U);

    sample = rate(1000U, 1200U, 10000U, valid);
    ack = feedback(&sample, now + 2U, 1000U, 1300U);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_CRUISE);
    /* Entering DOWN from Startup does not finish a ProbeBW filter cycle. */
    CHECK(model.cycle_count == 0U);

    sample = rate(1100U, 1300U, 10000U, valid);
    ack = feedback(&sample, now + probe.wait_ns + 1U,
                   1000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL);
    CHECK(probe.ack_phase == TCP_SHIFT_BBR3_ACKS_REFILLING);

    sample = rate(1200U, 1400U, 10000U, valid);
    ack = feedback(&sample, ack.now_ns + 1U, 1000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL);

    sample = rate(1300U, 1500U, 10000U, valid);
    ack = feedback(&sample, ack.now_ns + 1U, 1000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_UP);
    CHECK(probe.ack_phase == TCP_SHIFT_BBR3_ACKS_PROBE_STARTING);

    delivered = 1500U;
    for (i = 0U; i < 3U; i++) {
        sample = rate(delivered, delivered + 100U, 10000U, valid);
        ack = feedback(&sample, ack.now_ns + 1U, 1000U, UINT64_MAX);
        CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
        delivered += 100U;
        if (i < 2U) {
            CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_UP);
        }
    }
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(probe.pending_probe_feedback == 1U);
    CHECK(model.cycle_count == 0U);

    sample = rate(delivered, delivered + 100U, 10000U, valid);
    ack = feedback(&sample, ack.now_ns + 1U, 2000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.cycle_count == 1U);
    CHECK(probe.pending_probe_feedback == 0U);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    return 0;
}

static int test_reno_round_wait(void)
{
    struct tcp_shift_bbr3_model model;
    struct tcp_shift_bbr3_probe probe;
    struct tcp_shift_cc_rate_sample sample;
    struct tcp_shift_bbr3_probe_ack ack;
    const uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    uint64_t delivered = 1000U;
    unsigned i;
    unsigned target;

    setup(&model, &probe);
    CHECK(tcp_shift_bbr3_probe_enter(
              &probe, &model, 1U, delivered, 23U) == 0);
    target = 10U; /* BDP=1000 bytes and MSS=100 bytes. */
    CHECK(probe.rounds_since_probe <= 1U);

    for (i = probe.rounds_since_probe; i < target; i++) {
        sample = rate(delivered, delivered + 100U, 10000U, valid);
        ack = feedback(&sample, (uint64_t)i + 2U, 2000U, UINT64_MAX);
        CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
        delivered += 100U;
        if (i + 1U < target) {
            CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
        }
    }
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL);
    CHECK(ack.now_ns < TCP_SHIFT_BBR3_PROBE_WAIT_BASE_NS);
    return 0;
}

static int test_app_limited_loss_and_invalid(void)
{
    struct tcp_shift_bbr3_model model;
    struct tcp_shift_bbr3_probe probe;
    struct tcp_shift_cc_rate_sample sample;
    struct tcp_shift_bbr3_probe_ack ack;
    const uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    uint64_t now = 100U;

    setup(&model, &probe);
    model.full_bw_reached = 0U;
    CHECK(tcp_shift_bbr3_probe_enter(
              &probe, &model, now, 1000U, 19U) < 0);
    model.full_bw_reached = 1U;
    CHECK(tcp_shift_bbr3_probe_enter(
              &probe, &model, now, 1000U, 19U) == 0);

    sample = rate(1000U, 1000U, 10000U, valid);
    ack = feedback(&sample, now + probe.wait_ns + 10U,
                   1000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);

    sample = rate(1000U, 1100U, 10000U, valid);
    ack = feedback(&sample, now + probe.wait_ns + 11U,
                   1000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL);
    sample = rate(1100U, 1200U, 10000U, valid);
    ack = feedback(&sample, ack.now_ns + 1U, 1000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_UP);

    sample = rate(1200U, 1300U, 10000U,
                  valid | TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    ack = feedback(&sample, ack.now_ns + 1U, 2000U, UINT64_MAX);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_UP);
    CHECK(probe.up_stagnant_rounds == 0U);

    sample = rate(1300U, 1400U, 10000U, valid);
    ack = feedback(&sample, ack.now_ns + 1U, 2000U, 2000U);
    ack.qualified_loss_too_high = 1U;
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(probe.previous_probe_too_high == 1U);
    CHECK(probe.pending_probe_feedback == 1U);

    /* Non-app-limited feedback is required to age the bandwidth window. */
    sample = rate(1400U, 1500U, 1000U,
                  valid | TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    ack = feedback(&sample, ack.now_ns + 1U, 2000U, 2000U);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) == 0);
    CHECK(model.cycle_count == 0U);
    CHECK(probe.pending_probe_feedback == 0U);

    sample = rate(1600U, 1500U, 1000U, valid);
    ack = feedback(&sample, ack.now_ns + 1U, 2000U, 2000U);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) < 0);
    sample = rate(1500U, 1600U, 1000U, valid);
    ack = feedback(&sample, 0U, 2000U, 2000U);
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model, &ack) < 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    return 0;
}

int main(void)
{
    CHECK(test_full_cycle_and_feedback_aging() == 0);
    CHECK(test_reno_round_wait() == 0);
    CHECK(test_app_limited_loss_and_invalid() == 0);
    puts("bbr3_probe_contract=ok cycle=down,cruise,refill,up "
         "wait=clock,reno refill_round=1 "
         "feedback_aging=guarded loss=qualified-only");
    return 0;
}
