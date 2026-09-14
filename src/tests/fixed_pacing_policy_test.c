#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/cc.h"
#include "cc/reno.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "fixed-pacing-policy: check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    struct tcp_shift_cc cc;
    struct tcp_shift_reno_state state;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 0U,
        .send_window_bytes = 64000U,
        .cwnd_limit_bytes = 65535U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 4000U,
        .initial_ssthresh_bytes = 8000U,
        .min_cwnd_bytes = 1000U,
    };
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_loss loss = {.lost_bytes = 1000U};

    memset(&cc, 0, sizeof(cc));
    memset(&state, 0, sizeof(state));
    memset(&ack, 0, sizeof(ack));
    ack.acked_bytes = 2000U;

    CHECK(tcp_shift_fixed_pacing_reno_ops.state_size == sizeof(state));
    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_fixed_pacing_reno_ops,
                            &state, sizeof(state), &transport, &init,
                            &policy) == 0);
    CHECK(policy.cwnd_bytes == 4000U);
    CHECK(policy.ssthresh_bytes == 8000U);
    CHECK(policy.pacing_rate_bytes_per_sec ==
          TCP_SHIFT_FIXED_PACING_RENO_RATE_BYTES_PER_SEC);

    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 6000U);
    CHECK(policy.pacing_rate_bytes_per_sec ==
          TCP_SHIFT_FIXED_PACING_RENO_RATE_BYTES_PER_SEC);

    transport.send_window_bytes = 6000U;
    CHECK(tcp_shift_cc_on_loss(&cc, &transport, &loss, &policy) == 0);
    CHECK(policy.ssthresh_bytes == 3000U);
    CHECK(policy.cwnd_bytes == 3000U);
    CHECK(policy.pacing_rate_bytes_per_sec ==
          TCP_SHIFT_FIXED_PACING_RENO_RATE_BYTES_PER_SEC);

    CHECK(tcp_shift_cc_on_timeout(&cc, &transport, &policy) == 0);
    CHECK(policy.cwnd_bytes == 1000U);
    CHECK(policy.ssthresh_bytes == 2000U);
    CHECK(policy.pacing_rate_bytes_per_sec ==
          TCP_SHIFT_FIXED_PACING_RENO_RATE_BYTES_PER_SEC);

    printf("fixed_pacing_policy=ok controller=%s state_bytes=%zu rate_bytes_per_sec=%llu\n",
           tcp_shift_fixed_pacing_reno_ops.name,
           sizeof(state),
           (unsigned long long)policy.pacing_rate_bytes_per_sec);
    return 0;
}
