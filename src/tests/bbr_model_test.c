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

int main(void)
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

    /* Invalid delivery-rate samples are not admitted into max_bw, but a valid
     * RTT sample is independent model input. */
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

    /* A lower app-limited sample must not drag the bandwidth model down. */
    rate = sample(80000000U, 35000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 2000000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 100000000U);
    CHECK(model.ignored_app_limited_bw_samples == 1U);

    /* App-limited does not mean useless: a higher observed rate is admitted. */
    rate = sample(120000000U, 0U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 3000000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 120000000U);
    CHECK(model.accepted_bw_samples == 2U);

    /* The filter retains the current and previous ProbeBW-cycle maxima. */
    tcp_shift_bbr_model_advance_bw_cycle(&model);
    rate = sample(90000000U, 25000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 4000000000U) == 0);
    CHECK(model.max_bw_bytes_per_sec == 120000000U);
    CHECK(model.min_rtt_ns == 25000000U);

    tcp_shift_bbr_model_advance_bw_cycle(&model);
    CHECK(model.max_bw_bytes_per_sec == 90000000U);

    /* More than five seconds after the ProbeRTT candidate stamp, the current
     * valid RTT refreshes probe_rtt_min_delay even when it is higher. */
    rate = sample(70000000U, 40000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 10000000001ULL) == 0);
    CHECK(model.probe_rtt_expired == 1U);
    CHECK(model.probe_rtt_min_delay_ns == 40000000U);
    CHECK(model.min_rtt_ns == 25000000U);

    /* More than ten seconds after the min_rtt stamp, min_rtt may rise. At
     * exactly five seconds since the refreshed ProbeRTT candidate, the draft's
     * strict '>' expiration rule keeps the 40-ms candidate. */
    rate = sample(70000000U, 45000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 15000000001ULL) == 0);
    CHECK(model.min_rtt_expired == 1U);
    CHECK(model.probe_rtt_expired == 0U);
    CHECK(model.min_rtt_ns == 40000000U);

    /* Retransmission metadata is harmless by itself; P5 withholds RTT_VALID
     * for retransmitted RTT candidates, so the model trusts the validity bit. */
    previous_bw = model.max_bw_bytes_per_sec;
    previous_min_rtt = model.min_rtt_ns;
    rate = sample(0U, 10000000U, TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED);
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 16000000000ULL) == 0);
    CHECK(model.max_bw_bytes_per_sec == previous_bw);
    CHECK(model.min_rtt_ns == previous_min_rtt);

    /* The model requires monotonic caller time and must reject regressions
     * before mutating model estimates. */
    rate = sample(200000000U, 10000000U,
                  TCP_SHIFT_CC_RATE_SAMPLE_VALID |
                      TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID);
    previous_bw = model.max_bw_bytes_per_sec;
    previous_min_rtt = model.min_rtt_ns;
    CHECK(tcp_shift_bbr_model_on_ack(&model, &rate, 15000000000ULL) < 0);
    CHECK(model.max_bw_bytes_per_sec == previous_bw);
    CHECK(model.min_rtt_ns == previous_min_rtt);

    CHECK(sizeof(model) <= 128U);

    printf("bbr_model_contract=ok mode=startup state_bytes=%zu "
           "max_bw_bytes_per_sec=%llu min_rtt_ns=%llu "
           "valid_rate_samples=%llu accepted_bw_samples=%llu "
           "ignored_app_limited=%llu valid_rtt_samples=%llu\n",
           sizeof(model),
           (unsigned long long)model.max_bw_bytes_per_sec,
           (unsigned long long)model.min_rtt_ns,
           (unsigned long long)model.valid_rate_samples,
           (unsigned long long)model.accepted_bw_samples,
           (unsigned long long)model.ignored_app_limited_bw_samples,
           (unsigned long long)model.valid_rtt_samples);
    return 0;
}
