#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "bbr-model: check failed at %s:%d: %s\n",       \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static struct tcp_shift_cc_rate_sample sample(uint64_t rate,
                                               uint64_t rtt_ns,
                                               uint32_t flags)
{
    struct tcp_shift_cc_rate_sample value;

    memset(&value, 0, sizeof(value));
    value.delivery_rate_bytes_per_sec = rate;
    value.rtt_ns = rtt_ns;
    value.flags = flags;
    return value;
}

static struct tcp_shift_cc_rate_sample round_sample(uint64_t rate,
                                                     uint64_t prior_delivered,
                                                     uint64_t delivered_total,
                                                     uint32_t flags)
{
    struct tcp_shift_cc_rate_sample value = sample(rate, 0U, flags);

    value.prior_delivered_bytes = prior_delivered;
    value.delivered_total_bytes = delivered_total;
    return value;
}

static int check_estimator_contract(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_cc_rate_sample rate;
    uint64_t previous_bw;
    uint64_t previous_min_rtt;

    tcp_shift_bbr_model_init(&model);
    CHECK(model.mode == TCP_SHIFT_BBR_MODE_STARTUP);
    CHECK(model.max_bw_bytes_per_sec == 0U);
    CHECK(model.has_min_rtt == 0U);
    CHECK(model.has_probe_rtt_min == 0U);

    rate = sample(90000000U, 32000000U, TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 500000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 0U);
    CHECK(model.valid_rate_samples == 0U);
    CHECK(model.valid_rtt_samples == 1U);
    CHECK(model.min_rtt_ns == 32000000U);

    rate = sample(100000000U, 30000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 1000000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 100000000U);
    CHECK(model.min_rtt_ns == 30000000U);
    CHECK(model.valid_rate_samples == 1U);
    CHECK(model.accepted_bw_samples == 1U);

    rate = sample(80000000U, 35000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 2000000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 100000000U);
    CHECK(model.ignored_app_limited_bw_samples == 1U);

    rate = sample(120000000U, 0U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 3000000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 120000000U);
    CHECK(model.accepted_bw_samples == 2U);

    /* Without cumulative delivery snapshots the filter stays in virtual round
     * zero. This preserves backward-compatible estimator use while packet-timed
     * adapters provide the round horizon exercised separately below. */
    rate = sample(90000000U, 25000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 4000000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 120000000U);
    CHECK(model.min_rtt_ns == 25000000U);

    rate = sample(70000000U, 40000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 10000000001ULL) == 0);
    CHECK(model.probe_rtt_expired == 1U);
    CHECK(model.probe_rtt_min_delay_ns == 40000000U);
    CHECK(model.min_rtt_ns == 25000000U);

    rate = sample(70000000U, 45000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 15000000001ULL) == 0);
    CHECK(model.min_rtt_expired == 1U);
    CHECK(model.probe_rtt_expired == 0U);
    CHECK(model.min_rtt_ns == 40000000U);

    previous_bw = model.max_bw_bytes_per_sec;
    previous_min_rtt = model.min_rtt_ns;
    rate = sample(0U, 10000000U, TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 16000000000ULL) == 0);
    CHECK(model.max_bw_bytes_per_sec == previous_bw);
    CHECK(model.min_rtt_ns == previous_min_rtt);

    rate = sample(200000000U, 10000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    previous_bw = model.max_bw_bytes_per_sec;
    previous_min_rtt = model.min_rtt_ns;
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 15000000000ULL) < 0);
    CHECK(model.max_bw_bytes_per_sec == previous_bw);
    CHECK(model.min_rtt_ns == previous_min_rtt);
    return 0;
}

static int check_round_filter_contract(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_cc_rate_sample rate;
    const uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    uint32_t round;

    tcp_shift_bbr_model_init(&model);

    /* Round 1 carries the peak. It remains visible through the complete
     * ten-round horizon even though later accepted samples are lower. */
    for (round = 1U; round <= TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS; round++) {
        uint64_t prior = (uint64_t)(round - 1U) * 1000U;
        uint64_t total = (uint64_t)round * 1000U;
        uint64_t bw = round == 1U ? 1000U : 900U;

        rate = round_sample(bw, prior, total, valid);
        CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, round) == 0);
        CHECK(model.round_count == round);
        CHECK(model.max_bw_bytes_per_sec == 1000U);
    }

    /* Round 11 replaces round 1, so the old peak expires and the best of
     * rounds 2..11 becomes the model bandwidth. */
    rate = round_sample(800U, 10000U, 11000U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 11U) == 0);
    CHECK(model.round_count == 11U);
    CHECK(model.max_bw_bytes_per_sec == 900U);
    CHECK(model.bw_filter_round == 11U);

    /* Lower app-limited samples must not age away the last trustworthy path
     * rate, even across more than a full ten-round nominal window. */
    for (round = 12U; round <= 25U; round++) {
        uint64_t prior = (uint64_t)(round - 1U) * 1000U;
        uint64_t total = (uint64_t)round * 1000U;

        rate = round_sample(100U, prior, total,
                            valid | TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
        CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, round) == 0);
        CHECK(model.round_count == round);
        CHECK(model.max_bw_bytes_per_sec == 900U);
        CHECK(model.bw_filter_round == 11U);
    }

    /* The next trustworthy sample fast-forwards the filter. Since more than
     * ten packet rounds elapsed since the last admitted sample, all old slots
     * expire before the new rate is installed. */
    rate = round_sample(700U, 25000U, 26000U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 26U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 700U);
    CHECK(model.bw_filter_round == 26U);

    /* An app-limited sample at or above the current model is still admissible. */
    rate = round_sample(1000U, 26000U, 27000U,
                        valid | TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 27U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 1000U);
    CHECK(model.bw_filter_round == 27U);
    CHECK(model.accepted_bw_samples == 13U);
    CHECK(model.ignored_app_limited_bw_samples == 14U);
    return 0;
}

static int check_round_and_startup_contract(struct tcp_shift_bbr_model *model)
{
    struct tcp_shift_cc_rate_sample rate;
    const uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;

    tcp_shift_bbr_model_init(model);

    rate = round_sample(100U, 0U, 1000U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 1U) == 0);
    CHECK(model->round_start == 1U);
    CHECK(model->round_count == 1U);
    CHECK(model->next_round_delivered == 1000U);
    CHECK(model->full_bw_bytes_per_sec == 100U);
    CHECK(model->full_bw_count == 0U);

    rate = round_sample(110U, 500U, 1500U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 2U) == 0);
    CHECK(model->round_start == 0U);
    CHECK(model->round_count == 1U);
    CHECK(model->full_bw_count == 0U);

    rate = round_sample(125U, 1000U, 2000U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 3U) == 0);
    CHECK(model->round_start == 1U);
    CHECK(model->round_count == 2U);
    CHECK(model->full_bw_bytes_per_sec == 125U);
    CHECK(model->full_bw_count == 0U);

    rate = round_sample(150U, 2000U, 3000U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 4U) == 0);
    CHECK(model->round_count == 3U);
    CHECK(model->full_bw_count == 1U);
    CHECK(model->full_bw_reached == 0U);

    rate = round_sample(140U, 3000U, 4000U,
                        valid | TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 5U) == 0);
    CHECK(model->round_count == 4U);
    CHECK(model->full_bw_count == 1U);
    CHECK(model->full_bw_reached == 0U);

    rate = round_sample(155U, 4000U, 5000U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 6U) == 0);
    CHECK(model->full_bw_count == 2U);
    CHECK(model->full_bw_reached == 0U);

    rate = round_sample(156U, 5000U, 6000U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 7U) == 0);
    CHECK(model->round_count == 6U);
    CHECK(model->full_bw_count == TCP_SHIFT_BBR_FULL_BW_ROUNDS);
    CHECK(model->full_bw_now == 1U);
    CHECK(model->full_bw_reached == 1U);
    CHECK(model->ignored_app_limited_bw_samples == 1U);

    rate = round_sample(200U, 5500U, 6500U, valid);
    CHECK(tcp_shift_bbr_model_on_ack(model, &rate, 8U) == 0);
    CHECK(model->round_start == 0U);
    CHECK(model->full_bw_now == 0U);
    CHECK(model->full_bw_reached == 1U);
    return 0;
}

int main(void)
{
    struct tcp_shift_bbr_model round_model;

    CHECK(check_estimator_contract() == 0);
    CHECK(check_round_filter_contract() == 0);
    CHECK(check_round_and_startup_contract(&round_model) == 0);
    CHECK(sizeof(round_model) <= 224U);

    printf("bbr_model_contract=ok mode=startup state_bytes=%zu "
           "filter_rounds=%u round_count=%u full_bw_bytes_per_sec=%llu "
           "full_bw_count=%u full_bw_reached=%u ignored_app_limited=%llu "
           "next_round_delivered=%llu\n",
           sizeof(round_model),
           TCP_SHIFT_BBR_MAX_BW_FILTER_ROUNDS,
           round_model.round_count,
           (unsigned long long)round_model.full_bw_bytes_per_sec,
           round_model.full_bw_count,
           (unsigned)round_model.full_bw_reached,
           (unsigned long long)round_model.ignored_app_limited_bw_samples,
           (unsigned long long)round_model.next_round_delivered);
    return 0;
}
