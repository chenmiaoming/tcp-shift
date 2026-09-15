#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/cc.h"
#include "cc/cubic.h"
#include "cc/registry.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "cubic-controller: check failed at %s:%d: %s\n",        \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    union tcp_shift_cc_builtin_state state;
    struct tcp_shift_cc cc;
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
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_loss loss = {.lost_bytes = 1000U};
    struct tcp_shift_cc_policy policy;

    memset(&state, 0, sizeof(state));
    memset(&cc, 0, sizeof(cc));
    memset(&ack, 0, sizeof(ack));

    CHECK(tcp_shift_cc_find_ops("cubic") == &tcp_shift_cubic_ops);
    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_cubic_ops,
                            &state, sizeof(state),
                            &transport, &init, &policy) == 0);
    CHECK(cc.ops == &tcp_shift_cubic_ops);
    CHECK(cc.state == &state);
    CHECK(policy.cwnd_bytes == 4000U);
    CHECK(policy.ssthresh_bytes == 8000U);
    CHECK(policy.pacing_rate_bytes_per_sec == 0U);

    ack.acked_bytes = 2000U;
    ack.ack_time_ns = UINT64_C(100000000);
    ack.smoothed_rtt_ns = UINT64_C(80000000);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 6000U);

    ack.ack_time_ns = UINT64_C(200000000);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 8000U);

    ack.acked_bytes = 1000U;
    ack.ack_time_ns = UINT64_C(1000000000);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes > 8000U);

    transport.inflight_bytes = 10000U;
    CHECK(tcp_shift_cc_on_loss(&cc, &transport, &loss, &policy) == 0);
    CHECK(policy.cwnd_bytes == 7000U);
    CHECK(policy.ssthresh_bytes == 7000U);

    CHECK(tcp_shift_cc_on_timeout(&cc, &transport, &policy) == 0);
    CHECK(policy.cwnd_bytes == 1000U);
    CHECK(policy.ssthresh_bytes == 7000U);

    ack.acked_bytes = 1000U;
    ack.ack_time_ns = 0U;
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == -1);

    printf("cubic_controller=ok name=%s state_bytes=%zu default=%s "
           "ack_time_required=1\n",
           tcp_shift_cubic_ops.name,
           tcp_shift_cubic_ops.state_size,
           tcp_shift_cc_default_ops()->name);
    return 0;
}
