#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr_controller.h"

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "bbr-timeout: check failed at %s:%d: %s\n",    \
                    __FILE__, __LINE__, #expr);                             \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static int check_timeout_loss_state(void)
{
    struct tcp_shift_bbr_controller_state state;
    struct tcp_shift_bbr_timeout_observation timeout;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;
    uint64_t pacing_before;

    memset(&state, 0, sizeof(state));
    memset(&timeout, 0, sizeof(timeout));
    memset(&transport, 0, sizeof(transport));
    memset(&init, 0, sizeof(init));

    transport.mss_bytes = 1460U;
    transport.inflight_bytes = 12000U;
    transport.send_window_bytes = 1000000U;
    transport.cwnd_limit_bytes = 1000000U;
    init.initial_cwnd_bytes = 14600U;
    init.initial_ssthresh_bytes = transport.cwnd_limit_bytes;
    init.min_cwnd_bytes = 2920U;

    CHECK(tcp_shift_bbr_controller_init(
              &state, &transport, &init, 5U, &policy) == 0);

    /* Seed fields that Linux bbr_set_state(TCP_CA_Loss) deliberately does not
     * discard. Only full_bw's growth baseline is reset by that callback. */
    state.model.mode = TCP_SHIFT_BBR_MODE_PROBE_BW;
    state.model.max_bw_bytes_per_sec = UINT64_C(123000000);
    state.model.min_rtt_ns = UINT64_C(20000000);
    state.model.has_min_rtt = 1U;
    state.model.full_bw_bytes_per_sec = UINT64_C(120000000);
    state.model.full_bw_count = 3U;
    state.model.full_bw_reached = 1U;
    state.cwnd_bytes = 20000U;

    /* Make fast-recovery state active first: an RTO must supersede stale packet
     * conservation ownership before publishing its Loss-state cwnd. */
    CHECK(tcp_shift_bbr_controller_recovery_enter(
              &state, &transport, 0U, transport.mss_bytes, &policy) == 0);
    CHECK(state.recovery.in_recovery == 1U);
    pacing_before = state.pacing_rate_bytes_per_sec;

    /* 4380 bytes is three MSS of post-loss in-flight data. Linux
     * tcp_enter_loss() shape is post-loss inflight + one packet => four MSS. */
    timeout.post_loss_inflight_bytes = 4380U;
    CHECK(tcp_shift_bbr_controller_on_timeout(
              &state, &transport, &timeout, &policy) == 0);
    CHECK(state.recovery.in_recovery == 0U);
    CHECK(state.recovery.packet_conservation == 0U);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);
    CHECK(state.model.max_bw_bytes_per_sec == UINT64_C(123000000));
    CHECK(state.model.min_rtt_ns == UINT64_C(20000000));
    CHECK(state.model.full_bw_bytes_per_sec == 0U);
    CHECK(state.model.full_bw_count == 3U);
    CHECK(state.model.full_bw_reached == 1U);
    CHECK(state.model.round_start == 1U);
    CHECK(policy.cwnd_bytes == 5840U);
    CHECK(policy.ssthresh_bytes == transport.cwnd_limit_bytes);
    CHECK(policy.pacing_rate_bytes_per_sec == pacing_before);

    /* All post-loss flight may have been marked lost; one-MSS cwnd is valid at
     * timeout and BBR's normal minimum-cwnd policy is applied on later ACKs. */
    timeout.post_loss_inflight_bytes = 0U;
    CHECK(tcp_shift_bbr_controller_on_timeout(
              &state, &transport, &timeout, &policy) == 0);
    CHECK(policy.cwnd_bytes == transport.mss_bytes);

    /* Observation arithmetic is capped by the transport's representable cwnd. */
    timeout.post_loss_inflight_bytes = transport.cwnd_limit_bytes;
    CHECK(tcp_shift_bbr_controller_on_timeout(
              &state, &transport, &timeout, &policy) == 0);
    CHECK(policy.cwnd_bytes == transport.cwnd_limit_bytes);
    return 0;
}

static int check_invalid_inputs(void)
{
    struct tcp_shift_bbr_controller_state state;
    struct tcp_shift_bbr_timeout_observation timeout;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;

    memset(&state, 0, sizeof(state));
    memset(&timeout, 0, sizeof(timeout));
    memset(&transport, 0, sizeof(transport));
    memset(&init, 0, sizeof(init));

    transport.mss_bytes = 1460U;
    transport.cwnd_limit_bytes = 1000000U;
    init.initial_cwnd_bytes = 14600U;
    init.min_cwnd_bytes = 2920U;

    CHECK(tcp_shift_bbr_controller_init(
              &state, &transport, &init, 0U, &policy) == 0);
    CHECK(tcp_shift_bbr_controller_on_timeout(
              &state, &transport, NULL, &policy) < 0);
    CHECK(tcp_shift_bbr_controller_on_timeout(
              NULL, &transport, &timeout, &policy) < 0);
    transport.mss_bytes = 0U;
    CHECK(tcp_shift_bbr_controller_on_timeout(
              &state, &transport, &timeout, &policy) < 0);
    return 0;
}

int main(void)
{
    CHECK(check_timeout_loss_state() == 0);
    CHECK(check_invalid_inputs() == 0);

    printf("bbr_controller_timeout=ok loss_state=preserve-mode "
           "full_bw_baseline=reset full_bw_reached=preserved "
           "cwnd=post-loss-inflight-plus-one-mss pacing=preserved\n");
    return 0;
}
