#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr_controller.h"

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "bbr-controller: check failed at %s:%d: %s\n",  \
                    __FILE__, __LINE__, #expr);                              \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static struct tcp_shift_cc_rate_sample rate_sample(
    uint64_t rate,
    uint64_t prior_delivered,
    uint64_t delivered_total,
    uint32_t prior_inflight,
    uint32_t flags)
{
    struct tcp_shift_cc_rate_sample value;

    memset(&value, 0, sizeof(value));
    value.delivery_rate_bytes_per_sec = rate;
    value.rtt_ns = UINT64_C(20000000);
    value.prior_delivered_bytes = prior_delivered;
    value.delivered_total_bytes = delivered_total;
    value.prior_inflight_bytes = prior_inflight;
    value.flags = flags | TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
    return value;
}

static int drive_ack(struct tcp_shift_bbr_controller_state *state,
                     const struct tcp_shift_cc_transport *transport,
                     uint64_t now_ns,
                     uint64_t rate,
                     uint64_t prior_delivered,
                     uint64_t delivered_total,
                     uint32_t prior_inflight,
                     uint32_t flags,
                     struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_cc_ack ack;

    memset(&ack, 0, sizeof(ack));
    ack.acked_bytes = transport->mss_bytes;
    ack.ack_time_ns = now_ns;
    ack.smoothed_rtt_ns = UINT64_C(20000000);
    ack.rate = rate_sample(rate, prior_delivered, delivered_total,
                           prior_inflight, flags);
    return tcp_shift_bbr_controller_on_ack(state, transport, &ack, policy);
}

static int check_lifecycle(void)
{
    struct tcp_shift_bbr_controller_state state;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;
    uint64_t initial_rate;
    uint64_t probe_up_rate;
    uint64_t probe_rtt_done;
    uint32_t prior_cwnd;
    const uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;

    memset(&state, 0, sizeof(state));
    memset(&transport, 0, sizeof(transport));
    memset(&init, 0, sizeof(init));

    transport.mss_bytes = 1460U;
    transport.send_window_bytes = 10000000U;
    transport.cwnd_limit_bytes = 10000000U;
    init.initial_cwnd_bytes = 14600U;
    init.initial_ssthresh_bytes = transport.cwnd_limit_bytes;
    init.min_cwnd_bytes = 2920U;

    CHECK(tcp_shift_bbr_controller_init(
              &state, &transport, &init, 0U, &policy) == 0);
    CHECK(state.initialized == 1U);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_STARTUP);
    CHECK(policy.cwnd_bytes == init.initial_cwnd_bytes);
    CHECK(policy.ssthresh_bytes == transport.cwnd_limit_bytes);
    initial_rate = tcp_shift_bbr_initial_pacing_rate_bytes_per_sec(
        init.initial_cwnd_bytes, 0U);
    CHECK(policy.pacing_rate_bytes_per_sec == initial_rate);

    /* Build full-pipe evidence over packet-timed rounds. The fifth round is the
     * third below the 25% growth threshold and has >1BDP inflight, so the ACK
     * must finish in DRAIN rather than remaining in STARTUP. */
    CHECK(drive_ack(&state, &transport, UINT64_C(1000000000),
                    UINT64_C(100000000), 0U, 10000U, 4000000U,
                    valid, &policy) == 0);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_STARTUP);

    CHECK(drive_ack(&state, &transport, UINT64_C(1020000000),
                    UINT64_C(125000000), 10000U, 20000U, 4000000U,
                    valid, &policy) == 0);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_STARTUP);

    CHECK(drive_ack(&state, &transport, UINT64_C(1040000000),
                    UINT64_C(150000000), 20000U, 30000U, 4000000U,
                    valid, &policy) == 0);
    CHECK(state.model.full_bw_count == 1U);

    CHECK(drive_ack(&state, &transport, UINT64_C(1060000000),
                    UINT64_C(155000000), 30000U, 40000U, 4000000U,
                    valid, &policy) == 0);
    CHECK(state.model.full_bw_count == 2U);

    CHECK(drive_ack(&state, &transport, UINT64_C(1080000000),
                    UINT64_C(156000000), 40000U, 50000U, 4000000U,
                    valid, &policy) == 0);
    CHECK(state.model.full_bw_reached == 1U);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_DRAIN);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(53088750));

    /* 156 MB/s * 20 ms = 3.12 MB. Once prior inflight falls below that base
     * BDP, DRAIN exits and the controller starts the seeded ProbeBW phase. */
    CHECK(drive_ack(&state, &transport, UINT64_C(1100000000),
                    UINT64_C(156000000), 50000U, 60000U, 3000000U,
                    valid, &policy) == 0);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);
    CHECK(state.probe.cycle_started == 1U);
    CHECK(state.probe.cycle_index == 0U);
    probe_up_rate = UINT64_C(193050000);
    CHECK(policy.pacing_rate_bytes_per_sec == probe_up_rate);

    /* Probe-up lasts at least one min RTT and reaches the 1.25*BDP target
     * (3.9 MB here), then advances to probe-down. */
    CHECK(drive_ack(&state, &transport, UINT64_C(1121000000),
                    UINT64_C(156000000), 60000U, 70000U, 3900000U,
                    valid, &policy) == 0);
    CHECK(state.probe.cycle_index == 1U);
    CHECK(policy.pacing_rate_bytes_per_sec == UINT64_C(115830000));

    /* Force the 10-second min-RTT horizon to expire on a later valid RTT ACK.
     * Controller policy must switch to four packets while saving the prior
     * cwnd for restoration. */
    state.cwnd_bytes = 4000000U;
    state.model.min_rtt_stamp_ns = UINT64_C(1000000000);
    state.model.probe_rtt_min_stamp_ns = UINT64_C(7000000000);
    state.model.has_probe_rtt_min = 1U;
    CHECK(drive_ack(&state, &transport, UINT64_C(12000000001),
                    UINT64_C(156000000), 70000U, 80000U, 4000000U,
                    valid, &policy) == 0);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_PROBE_RTT);
    CHECK(state.probe.probe_rtt_prior_cwnd_bytes == 4000000U);
    CHECK(policy.cwnd_bytes == 5840U);

    /* Reaching four packets starts the 200 ms dwell and a fresh packet-timed
     * round. Both must complete before ProbeRTT can exit. */
    CHECK(drive_ack(&state, &transport, UINT64_C(12010000000),
                    UINT64_C(156000000), 80000U, 90000U, 5840U,
                    valid, &policy) == 0);
    CHECK(state.probe.probe_rtt_done_stamp_ns == UINT64_C(12210000000));
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_PROBE_RTT);

    CHECK(drive_ack(&state, &transport, UINT64_C(12050000000),
                    UINT64_C(156000000), 90000U, 100000U, 5840U,
                    valid, &policy) == 0);
    CHECK(state.probe.probe_rtt_round_done == 1U);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_PROBE_RTT);

    prior_cwnd = state.probe.probe_rtt_prior_cwnd_bytes;
    probe_rtt_done = state.probe.probe_rtt_done_stamp_ns;
    CHECK(drive_ack(&state, &transport, probe_rtt_done,
                    UINT64_C(156000000), 100000U, 110000U, 5840U,
                    valid, &policy) == 0);
    CHECK(state.model.mode == TCP_SHIFT_BBR_MODE_PROBE_BW);
    CHECK(state.probe.cycle_started == 1U);
    CHECK(state.probe.cycle_index == 0U);
    CHECK(state.cwnd_bytes > prior_cwnd);
    CHECK(policy.pacing_rate_bytes_per_sec == probe_up_rate);
    CHECK(state.model.min_rtt_expired == 0U);
    CHECK(state.model.min_rtt_stamp_ns == probe_rtt_done);
    return 0;
}

static int check_recovery_composition(void)
{
    struct tcp_shift_bbr_controller_state state;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;
    uint64_t pacing_before_recovery;
    const uint32_t valid = TCP_SHIFT_CC_RATE_SAMPLE_VALID;

    memset(&state, 0, sizeof(state));
    memset(&transport, 0, sizeof(transport));
    memset(&init, 0, sizeof(init));

    transport.mss_bytes = 1460U;
    transport.send_window_bytes = 1000000U;
    transport.cwnd_limit_bytes = 1000000U;
    transport.inflight_bytes = 12000U;
    init.initial_cwnd_bytes = 14600U;
    init.initial_ssthresh_bytes = transport.cwnd_limit_bytes;
    init.min_cwnd_bytes = 2920U;

    CHECK(tcp_shift_bbr_controller_init(
              &state, &transport, &init, 0U, &policy) == 0);

    /* Establish a cumulative-delivered marker. Recovery entry must reset the
     * next packet-timed round boundary to exactly this snapshot and preserve
     * the current ACK/SACK credit in the first conservation window. */
    CHECK(drive_ack(&state, &transport, UINT64_C(1000000000),
                    UINT64_C(10000000), 0U, 10000U, 12000U,
                    valid, &policy) == 0);
    CHECK(state.delivered_bytes == 10000U);
    state.cwnd_bytes = 20000U;
    pacing_before_recovery = state.pacing_rate_bytes_per_sec;

    CHECK(tcp_shift_bbr_controller_recovery_enter(
              &state, &transport, transport.mss_bytes,
              transport.mss_bytes, &policy) == 0);
    CHECK(state.recovery.in_recovery == 1U);
    CHECK(state.recovery.packet_conservation == 1U);
    CHECK(state.recovery.prior_cwnd_bytes == 20000U);
    CHECK(state.model.next_round_delivered == 10000U);
    CHECK(state.model.round_start == 0U);
    CHECK(policy.cwnd_bytes == 13460U);
    CHECK(policy.pacing_rate_bytes_per_sec == pacing_before_recovery);
    CHECK(state.pending_probe_loss_bytes == transport.mss_bytes);

    /* A recovery ACK whose prior-delivered snapshot predates the entry marker
     * is still in the first packet-timed recovery round. Normal STARTUP policy
     * would grow cwnd, but packet conservation must retain ownership. */
    transport.inflight_bytes = 10000U;
    CHECK(drive_ack(&state, &transport, UINT64_C(1020000000),
                    UINT64_C(10000000), 9000U, 11460U, 10000U,
                    valid, &policy) == 0);
    CHECK(state.model.round_start == 0U);
    CHECK(state.recovery.packet_conservation == 1U);
    CHECK(policy.cwnd_bytes == 13460U);

    /* Once prior_delivered reaches the entry marker, model.round_start opens a
     * new packet-timed round. That ACK releases packet conservation and normal
     * BBR cwnd growth resumes from the recovery-adjusted cwnd. */
    transport.inflight_bytes = 8500U;
    CHECK(drive_ack(&state, &transport, UINT64_C(1040000000),
                    UINT64_C(10000000), 10000U, 12920U, 8500U,
                    valid, &policy) == 0);
    CHECK(state.pending_probe_loss_bytes == 0U);
    CHECK(state.model.round_start == 1U);
    CHECK(state.recovery.packet_conservation == 0U);
    CHECK(policy.cwnd_bytes > 12000U);

    /* Transport-observed exit restores the last known-good pre-recovery cwnd.
     * The real adapter will then feed the same/new ACK through normal policy,
     * which can cap it to the current BDP target. */
    CHECK(tcp_shift_bbr_controller_recovery_exit(
              &state, &transport, &policy) == 0);
    CHECK(state.recovery.in_recovery == 0U);
    CHECK(policy.cwnd_bytes >= 20000U);
    CHECK(state.cwnd_bytes == policy.cwnd_bytes);
    CHECK(policy.pacing_rate_bytes_per_sec != 0U);
    return 0;
}

static int check_invalid_inputs(void)
{
    struct tcp_shift_bbr_controller_state state;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_cc_ack ack;

    memset(&state, 0, sizeof(state));
    memset(&transport, 0, sizeof(transport));
    memset(&init, 0, sizeof(init));
    memset(&ack, 0, sizeof(ack));

    transport.mss_bytes = 1460U;
    transport.cwnd_limit_bytes = 1000000U;
    init.initial_cwnd_bytes = 14600U;
    init.min_cwnd_bytes = 2920U;

    CHECK(tcp_shift_bbr_controller_init(
              &state, &transport, &init, 3U, &policy) == 0);
    ack.acked_bytes = 1460U;
    CHECK(tcp_shift_bbr_controller_on_ack(
              &state, &transport, &ack, &policy) < 0);
    CHECK(tcp_shift_bbr_controller_recovery_enter(
              &state, &transport, 0U, 0U, &policy) < 0);
    CHECK(tcp_shift_bbr_controller_recovery_exit(
              &state, &transport, &policy) < 0);
    CHECK(tcp_shift_bbr_controller_init(
              NULL, &transport, &init, 0U, &policy) < 0);
    transport.mss_bytes = 0U;
    CHECK(tcp_shift_bbr_controller_init(
              &state, &transport, &init, 0U, &policy) < 0);
    return 0;
}

int main(void)
{
    CHECK(check_lifecycle() == 0);
    CHECK(check_recovery_composition() == 0);
    CHECK(check_invalid_inputs() == 0);

    printf("bbr_controller_lifecycle=ok modes=startup-drain-probebw-probertt-probebw "
           "public_ops=disabled loss_timeout=pending cycle_seed=external\n");
    printf("bbr_controller_recovery=ok conservation=one-packet-round "
           "round_marker=delivered restore=prior_cwnd public_ops=disabled\n");
    return 0;
}
