#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "bbr-startup-policy: check failed at %s:%d: %s\n",\
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int check_arithmetic(void)
{
    const uint64_t bw = UINT64_C(100000000);
    const uint64_t rtt_ns = UINT64_C(20000000);

    CHECK(tcp_shift_bbr_bdp_bytes(bw, rtt_ns) == UINT64_C(2000000));
    CHECK(tcp_shift_bbr_startup_pacing_rate_bytes_per_sec(bw) ==
          UINT64_C(285785156));
    CHECK(tcp_shift_bbr_startup_cwnd_target_bytes(
              bw, rtt_ns, 1460U, 10000000U, 14600U) == 5773438U);

    /* Exact ceil semantics: a nonzero sub-byte mathematical BDP must still
     * produce one byte before gain/min-cwnd policy is applied. */
    CHECK(tcp_shift_bbr_bdp_bytes(1U, 1U) == 1U);
    CHECK(tcp_shift_bbr_startup_cwnd_target_bytes(
              1U, 1U, 1460U, 10000000U, 14600U) == 5840U);

    /* Saturate rather than wrap when the mathematical product/rate cannot fit
     * the fixed-width model representation. */
    CHECK(tcp_shift_bbr_bdp_bytes(UINT64_MAX, UINT64_MAX) == UINT64_MAX);
    CHECK(tcp_shift_bbr_startup_pacing_rate_bytes_per_sec(UINT64_MAX) ==
          UINT64_MAX);

    CHECK(tcp_shift_bbr_startup_cwnd_target_bytes(
              UINT64_MAX, UINT64_MAX, 1460U, 4000000U, 14600U) == 4000000U);

    /* Until both bw and RTT exist, stay at a safe initial/minimum target. */
    CHECK(tcp_shift_bbr_startup_cwnd_target_bytes(
              0U, 0U, 1460U, 10000000U, 14600U) == 14600U);
    CHECK(tcp_shift_bbr_startup_cwnd_target_bytes(
              0U, 0U, 1460U, 10000000U, 2920U) == 5840U);
    return 0;
}

static int check_startup_policy(void)
{
    struct tcp_shift_bbr_model model;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;
    uint32_t target;

    tcp_shift_bbr_model_init(&model);
    model.max_bw_bytes_per_sec = UINT64_C(100000000);
    model.min_rtt_ns = UINT64_C(20000000);
    model.has_min_rtt = 1U;

    memset(&transport, 0, sizeof(transport));
    transport.mss_bytes = 1460U;
    transport.cwnd_limit_bytes = 10000000U;

    memset(&ack, 0, sizeof(ack));
    ack.acked_bytes = 10000U;
    ack.rate.delivered_total_bytes = 100000U;

    target = tcp_shift_bbr_startup_cwnd_target_bytes(
        model.max_bw_bytes_per_sec, model.min_rtt_ns,
        transport.mss_bytes, transport.cwnd_limit_bytes, 14600U);
    CHECK(target == 5773438U);

    /* Before full pipe, Startup grows toward the target and must not reduce an
     * already-higher pacing rate when a noisy sample lowers max_bw. */
    CHECK(tcp_shift_bbr_startup_policy(
              &model, &transport, &ack, 14600U, 100000U,
              UINT64_C(300000000), &policy) == 0);
    CHECK(policy.cwnd_bytes == 110000U);
    CHECK(policy.ssthresh_bytes == transport.cwnd_limit_bytes);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(300000000));

    /* Linux BBR's initial-window exception allows ACK-clocked growth even when
     * the current modeled target is below cwnd, until the initial window has
     * actually been delivered. */
    model.max_bw_bytes_per_sec = 1000U;
    model.min_rtt_ns = 1000U;
    ack.acked_bytes = 1460U;
    ack.rate.delivered_total_bytes = 1000U;
    CHECK(tcp_shift_bbr_startup_policy(
              &model, &transport, &ack, 14600U, 14600U, 0U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 16060U);

    /* Once full pipe is latched, Linux BBR snaps cwnd down to
     * min(cwnd+acked, target) on a normal ACK. */
    model.max_bw_bytes_per_sec = UINT64_C(100000000);
    model.min_rtt_ns = UINT64_C(20000000);
    model.full_bw_reached = 1U;
    ack.acked_bytes = 1460U;
    ack.rate.delivered_total_bytes = 100000U;
    CHECK(tcp_shift_bbr_startup_policy(
              &model, &transport, &ack, 14600U, 6000000U, 0U, &policy) == 0);
    CHECK(policy.cwnd_bytes == target);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(285785156));

    /* Controller policy must respect the transport's representable cwnd cap. */
    transport.cwnd_limit_bytes = 65535U;
    ack.acked_bytes = 65535U;
    CHECK(tcp_shift_bbr_startup_policy(
              &model, &transport, &ack, 14600U, 65000U, 0U, &policy) == 0);
    CHECK(policy.cwnd_bytes == 65535U);
    CHECK(policy.ssthresh_bytes == 65535U);

    /* Wrong mode and invalid transport input fail closed. */
    model.mode = TCP_SHIFT_BBR_MODE_DRAIN;
    CHECK(tcp_shift_bbr_startup_policy(
              &model, &transport, &ack, 14600U, 65000U, 0U, &policy) < 0);
    model.mode = TCP_SHIFT_BBR_MODE_STARTUP;
    transport.mss_bytes = 0U;
    CHECK(tcp_shift_bbr_startup_policy(
              &model, &transport, &ack, 14600U, 65000U, 0U, &policy) < 0);
    return 0;
}

int main(void)
{
    CHECK(check_arithmetic() == 0);
    CHECK(check_startup_policy() == 0);

    printf("bbr_startup_policy=ok gain=739/256 pacing_margin=99/100 "
           "bdp_rounding=ceil min_cwnd_packets=4 saturation=ok\n");
    return 0;
}
