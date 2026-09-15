#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/transport_pacing.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "transport-pacing: check failed at %s:%d: %s\n",        \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 10000U,
        .send_window_bytes = 1000000U,
        .cwnd_limit_bytes = 1000000U,
    };
    struct tcp_shift_cc_policy policy;
    uint64_t rate;

    memset(&policy, 0, sizeof(policy));
    policy.cwnd_bytes = 20000U;
    policy.ssthresh_bytes = 100000U;

    /* 20 KB / 10 ms = 2 MB/s, with the Linux slow-start 200% transport gain. */
    rate = tcp_shift_transport_pacing_window_rate(
        &transport, &policy, UINT64_C(10000000));
    CHECK(rate == UINT64_C(4000000));
    CHECK(tcp_shift_transport_pacing_apply_window_fallback(
              &transport, UINT64_C(10000000), 0U, &policy) == 0);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(4000000));

    /* Once cwnd reaches at least half of ssthresh, use the 120% CA gain. */
    policy.cwnd_bytes = 60000U;
    policy.ssthresh_bytes = 100000U;
    policy.pacing_rate_bytes_per_sec = 0U;
    rate = tcp_shift_transport_pacing_window_rate(
        &transport, &policy, UINT64_C(10000000));
    CHECK(rate == UINT64_C(7200000));

    /* In-flight data can be the larger transport window just like packets_out
     * in Linux generic TCP pacing. */
    transport.inflight_bytes = 80000U;
    policy.pacing_rate_bytes_per_sec = 0U;
    rate = tcp_shift_transport_pacing_window_rate(
        &transport, &policy, UINT64_C(10000000));
    CHECK(rate == UINT64_C(9600000));

    /* Qualification can cap only the fallback rate. */
    CHECK(tcp_shift_transport_pacing_apply_window_fallback(
              &transport, UINT64_C(10000000), UINT64_C(9000000),
              &policy) == 0);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(9000000));

    /* A controller-owned rate (BBR) has strict precedence and is not clipped
     * by a fallback diagnostic cap. */
    policy.pacing_rate_bytes_per_sec = UINT64_C(1234567);
    CHECK(tcp_shift_transport_pacing_apply_window_fallback(
              &transport, UINT64_C(10000000), UINT64_C(1000),
              &policy) == 0);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(1234567));

    /* Loss-based fallback remains disabled until a usable SRTT exists. Startup
     * pacing is intentionally a separate policy decision rather than an
     * arbitrary RTT constant hidden in this helper. */
    policy.pacing_rate_bytes_per_sec = 0U;
    CHECK(tcp_shift_transport_pacing_apply_window_fallback(
              &transport, 0U, 0U, &policy) == 0);
    CHECK(policy.pacing_rate_bytes_per_sec == 0U);

    CHECK(tcp_shift_transport_pacing_apply_window_fallback(
              NULL, UINT64_C(10000000), 0U, &policy) < 0);
    CHECK(tcp_shift_transport_pacing_apply_window_fallback(
              &transport, UINT64_C(10000000), 0U, NULL) < 0);

    printf("transport_pacing=ok fallback=window_over_srtt "
           "ss_gain_percent=200 ca_gain_percent=120 "
           "controller_rate_precedence=1 startup_rtt=explicit\n");
    return 0;
}
