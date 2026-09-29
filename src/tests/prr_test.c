#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/prr.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "prr: check failed at %s:%d: %s\n",             \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int test_proportional_reduction(void)
{
    struct tcp_shift_prr prr;
    struct tcp_shift_prr_result result;
    const struct tcp_shift_prr_ack first = {
        .delivered_data = 1000U,
        .inflight = 9000U,
        .smss = 1000U,
        .safe_ack = 0U,
    };
    const struct tcp_shift_prr_ack second = {
        .delivered_data = 1000U,
        .inflight = 8000U,
        .smss = 1000U,
        .safe_ack = 0U,
    };

    CHECK(tcp_shift_prr_init(&prr, 10000U, 7000U) == 0);
    CHECK(tcp_shift_prr_on_ack(&prr, &first, &result) == 0);
    CHECK(result.sndcnt == 700U);
    CHECK(result.cwnd == 9700U);
    CHECK(tcp_shift_prr_on_send(&prr, result.sndcnt) == 0);

    CHECK(tcp_shift_prr_on_ack(&prr, &second, &result) == 0);
    CHECK(result.sndcnt == 700U);
    CHECK(result.cwnd == 8700U);
    CHECK(prr.prr_delivered == 2000U);
    CHECK(prr.prr_out == 700U);
    return 0;
}

static int test_reduction_bounds(void)
{
    struct tcp_shift_prr crb;
    struct tcp_shift_prr ssrb;
    struct tcp_shift_prr_result result;
    struct tcp_shift_prr_ack ack = {
        .delivered_data = 1000U,
        .inflight = 5000U,
        .smss = 1000U,
        .safe_ack = 0U,
    };

    CHECK(tcp_shift_prr_init(&crb, 10000U, 7000U) == 0);
    crb.prr_delivered = 2000U;
    crb.prr_out = 2000U;
    CHECK(tcp_shift_prr_on_ack(&crb, &ack, &result) == 0);
    CHECK(result.sndcnt == 1000U);
    CHECK(result.cwnd == 6000U);

    CHECK(tcp_shift_prr_init(&ssrb, 10000U, 7000U) == 0);
    ssrb.prr_delivered = 2000U;
    ssrb.prr_out = 2000U;
    ack.safe_ack = 1U;
    CHECK(tcp_shift_prr_on_ack(&ssrb, &ack, &result) == 0);
    CHECK(result.sndcnt == 2000U);
    CHECK(result.cwnd == 7000U);
    return 0;
}

static int test_forced_first_retransmit(void)
{
    struct tcp_shift_prr prr;
    struct tcp_shift_prr_result result;
    const struct tcp_shift_prr_ack ack = {
        .delivered_data = 1000U,
        .inflight = 7000U,
        .smss = 1000U,
        .safe_ack = 0U,
    };

    CHECK(tcp_shift_prr_init(&prr, 10000U, 7000U) == 0);
    CHECK(tcp_shift_prr_on_ack(&prr, &ack, &result) == 0);
    CHECK(result.sndcnt == 1000U);
    CHECK(result.cwnd == 8000U);
    return 0;
}

static int test_zero_delivery_and_completion(void)
{
    struct tcp_shift_prr prr;
    struct tcp_shift_prr_result result;
    struct tcp_shift_prr_ack ack = {
        .delivered_data = 0U,
        .inflight = 6000U,
        .smss = 1000U,
        .safe_ack = 1U,
    };
    uint32_t cwnd = 0U;

    CHECK(tcp_shift_prr_init(&prr, 10000U, 7000U) == 0);
    CHECK(tcp_shift_prr_on_ack(&prr, &ack, &result) == 0);
    CHECK(result.sndcnt == 0U);
    CHECK(result.cwnd == 6000U);
    CHECK(prr.prr_delivered == 0U);
    CHECK(prr.prr_out == 0U);

    CHECK(tcp_shift_prr_on_send(&prr, 1000U) == 0);
    CHECK(prr.prr_out == 1000U);
    CHECK(tcp_shift_prr_complete(&prr, &cwnd) == 0);
    CHECK(cwnd == 7000U);
    CHECK(prr.active == 0U);
    CHECK(prr.prr_delivered == 0U);
    CHECK(prr.prr_out == 0U);
    return 0;
}

static int test_fail_closed(void)
{
    struct tcp_shift_prr prr;
    struct tcp_shift_prr_ack ack;
    struct tcp_shift_prr_result result;
    uint32_t cwnd;

    memset(&prr, 0, sizeof(prr));
    memset(&ack, 0, sizeof(ack));
    memset(&result, 0, sizeof(result));

    CHECK(tcp_shift_prr_init(NULL, 1U, 1U) < 0);
    CHECK(tcp_shift_prr_init(&prr, 0U, 1U) < 0);
    CHECK(tcp_shift_prr_init(&prr, 1U, 0U) < 0);
    CHECK(tcp_shift_prr_on_ack(&prr, &ack, &result) < 0);
    CHECK(tcp_shift_prr_on_send(&prr, 1U) < 0);
    CHECK(tcp_shift_prr_complete(&prr, &cwnd) < 0);
    return 0;
}

int main(void)
{
    CHECK(test_proportional_reduction() == 0);
    CHECK(test_reduction_bounds() == 0);
    CHECK(test_forced_first_retransmit() == 0);
    CHECK(test_zero_delivery_and_completion() == 0);
    CHECK(test_fail_closed() == 0);

    printf("rfc9937_prr=ok proportional=1 crb=1 safeack_ssrb=1 "
           "forced_fast_retransmit=1 completion_cwnd_equals_ssthresh=1\n");
    return 0;
}
