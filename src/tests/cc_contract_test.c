#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/cc.h"
#include "cc/reno.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "cc-contract: check failed at %s:%d: %s\n",      \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int test_invalid_inputs(void)
{
    struct tcp_shift_cc cc;
    struct tcp_shift_reno_state state;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 0U,
        .send_window_bytes = 64000U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 4000U,
        .initial_ssthresh_bytes = 8000U,
        .min_cwnd_bytes = 1000U,
    };
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_cc_ack ack = {.acked_bytes = 1000U};
    struct tcp_shift_cc_loss loss = {.lost_bytes = 1000U};

    memset(&cc, 0, sizeof(cc));
    memset(&state, 0xa5, sizeof(state));
    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_reno_ops, &state,
                            sizeof(state) - 1U, &transport, &init,
                            &policy) == -1);
    CHECK(cc.ops == NULL);
    CHECK(cc.state == NULL);

    transport.mss_bytes = 0U;
    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_reno_ops, &state,
                            sizeof(state), &transport, &init,
                            &policy) == -1);
    CHECK(cc.ops == NULL);
    CHECK(cc.state == NULL);
    transport.mss_bytes = 1000U;

    init.min_cwnd_bytes = 999U;
    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_reno_ops, &state,
                            sizeof(state), &transport, &init,
                            &policy) == -1);
    CHECK(cc.ops == NULL);
    CHECK(cc.state == NULL);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == -1);
    init.min_cwnd_bytes = 1000U;

    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_reno_ops, &state,
                            sizeof(state), &transport, &init,
                            &policy) == 0);
    CHECK(cc.ops == &tcp_shift_reno_ops);
    CHECK(cc.state == &state);
    ack.acked_bytes = 0U;
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == -1);
    loss.lost_bytes = 0U;
    CHECK(tcp_shift_cc_on_loss(&cc, &transport, &loss, &policy) == -1);
    CHECK(tcp_shift_cc_on_timeout(NULL, &transport, &policy) == -1);
    return 0;
}

static int test_reno_transitions(void)
{
    struct tcp_shift_cc cc;
    struct tcp_shift_reno_state state;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = 0U,
        .send_window_bytes = 64000U,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = 4000U,
        .initial_ssthresh_bytes = 8000U,
        .min_cwnd_bytes = 1000U,
    };
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_loss loss = {.lost_bytes = 1000U};

    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_reno_ops, &state,
                            sizeof(state), &transport, &init,
                            &policy) == 0);
    CHECK(strcmp(tcp_shift_reno_ops.name, "reno") == 0);
    CHECK(tcp_shift_reno_ops.state_size == sizeof(state));
    CHECK(policy.cwnd_bytes == 4000U);
    CHECK(policy.ssthresh_bytes == 8000U);
    CHECK(policy.pacing_rate_bytes_per_sec == 0U);

    /* Byte-counted slow start is capped at two MSS per ACK. */
    ack.acked_bytes = 4000U;
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 6000U);
    CHECK(policy.ssthresh_bytes == 8000U);
    CHECK(state.ca_acked_bytes == 0U);

    ack.acked_bytes = 2000U;
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 8000U);
    CHECK(policy.ssthresh_bytes == 8000U);

    /* Congestion avoidance accumulates one cwnd of ACKed bytes before
     * increasing by one MSS. */
    ack.acked_bytes = 4000U;
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 8000U);
    CHECK(state.ca_acked_bytes == 4000U);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 9000U);
    CHECK(policy.ssthresh_bytes == 8000U);
    CHECK(state.ca_acked_bytes == 0U);

    /* Congestion loss reduces the effective min(cwnd, peer window) by half,
     * with the conventional two-MSS ssthresh floor. */
    transport.send_window_bytes = 6000U;
    CHECK(tcp_shift_cc_on_loss(&cc, &transport, &loss, &policy) == 0);
    CHECK(state.ssthresh_bytes == 3000U);
    CHECK(policy.ssthresh_bytes == 3000U);
    CHECK(policy.cwnd_bytes == 3000U);
    CHECK(state.ca_acked_bytes == 0U);

    /* Timeout keeps the reduced ssthresh but collapses cwnd to the configured
     * minimum. */
    CHECK(tcp_shift_cc_on_timeout(&cc, &transport, &policy) == 0);
    CHECK(state.ssthresh_bytes == 2000U);
    CHECK(policy.ssthresh_bytes == 2000U);
    CHECK(policy.cwnd_bytes == 1000U);
    CHECK(policy.pacing_rate_bytes_per_sec == 0U);

    /* A later path-MSS increase raises the effective minimum without needing
     * a platform-specific callback. */
    transport.mss_bytes = 1400U;
    transport.send_window_bytes = 64000U;
    ack.acked_bytes = 1400U;
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == 2800U);
    CHECK(policy.ssthresh_bytes == 2000U);
    return 0;
}

static int test_saturating_accounting(void)
{
    struct tcp_shift_cc cc;
    struct tcp_shift_reno_state state;
    struct tcp_shift_cc_transport transport = {
        .mss_bytes = 1000U,
        .inflight_bytes = UINT32_MAX,
        .send_window_bytes = UINT32_MAX,
    };
    struct tcp_shift_cc_init init = {
        .initial_cwnd_bytes = UINT32_MAX - 1000U,
        .initial_ssthresh_bytes = UINT32_MAX - 1000U,
        .min_cwnd_bytes = 1000U,
    };
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_cc_ack ack = {.acked_bytes = UINT32_MAX};

    CHECK(tcp_shift_cc_init(&cc, &tcp_shift_reno_ops, &state,
                            sizeof(state), &transport, &init,
                            &policy) == 0);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == UINT32_MAX);
    CHECK(policy.ssthresh_bytes == UINT32_MAX - 1000U);
    CHECK(tcp_shift_cc_on_ack(&cc, &transport, &ack, &policy) == 0);
    CHECK(policy.cwnd_bytes == UINT32_MAX);
    CHECK(policy.ssthresh_bytes == UINT32_MAX - 1000U);
    CHECK(policy.pacing_rate_bytes_per_sec == 0U);
    return 0;
}

int main(void)
{
    CHECK(test_invalid_inputs() == 0);
    CHECK(test_reno_transitions() == 0);
    CHECK(test_saturating_accounting() == 0);

    printf("cc_contract=ok controller=%s state_bytes=%zu pacing=none\n",
           tcp_shift_reno_ops.name, sizeof(struct tcp_shift_reno_state));
    return 0;
}
