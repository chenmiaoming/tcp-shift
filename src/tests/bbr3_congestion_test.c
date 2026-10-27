#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr3_congestion.h"
#include "cc/bbr3_probe.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "bbr3-congestion:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static struct tcp_shift_bbr3_congestion_ack ack_defaults(void)
{
    struct tcp_shift_bbr3_congestion_ack ack;

    memset(&ack, 0, sizeof(ack));
    ack.delivery_rate_bytes_per_sec = 1000U;
    ack.max_bw_bytes_per_sec = 10000U;
    ack.newly_delivered_bytes = 1000U;
    ack.tx_inflight_bytes = 10000U;
    ack.cwnd_bytes = 10000U;
    ack.target_inflight_bytes = 12000U;
    ack.newly_acked_bytes = 1000U;
    ack.mss_bytes = 1000U;
    ack.rate_valid = 1U;
    return ack;
}

static int check_loss_round_and_undo(void)
{
    struct tcp_shift_bbr3_congestion s;
    struct tcp_shift_bbr3_congestion_ack ack = ack_defaults();

    tcp_shift_bbr3_congestion_init(&s);
    CHECK(s.bw_shortterm_bytes_per_sec == UINT64_MAX);
    CHECK(s.inflight_shortterm_bytes == UINT64_MAX);
    CHECK(s.inflight_longterm_bytes == UINT64_MAX);
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.bw_shortterm_bytes_per_sec == UINT64_MAX);

    CHECK(tcp_shift_bbr3_congestion_save_recovery(&s) == 0);
    ack.newly_lost_bytes = 100U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.bw_shortterm_bytes_per_sec == UINT64_MAX);
    CHECK(s.loss_in_round == 1U);

    /* The loss response is executed at the *round* boundary, not per ACK. */
    ack.newly_lost_bytes = 0U;
    ack.loss_round_end = 1U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.bw_shortterm_bytes_per_sec == 7000U);
    CHECK(s.inflight_shortterm_bytes == 7000U);
    CHECK(s.loss_in_round == 0U);
    CHECK(tcp_shift_bbr3_congestion_rate_cap(
        &s, TCP_SHIFT_BBR3_PROBE_BW_DOWN, 10000U) == 7000U);
    CHECK(tcp_shift_bbr3_congestion_rate_cap(
        &s, TCP_SHIFT_BBR3_PROBE_BW_UP, 10000U) == 10000U);
    CHECK(tcp_shift_bbr3_congestion_cwnd_cap(
        &s, TCP_SHIFT_BBR3_PROBE_BW_DOWN, 1000U) == 7000U);
    CHECK(tcp_shift_bbr3_congestion_cwnd_cap(
        &s, TCP_SHIFT_BBR3_PROBE_BW_CRUISE, 1000U) == 7000U);
    CHECK(tcp_shift_bbr3_congestion_cwnd_cap(
        &s, TCP_SHIFT_BBR3_PROBE_BW_UP, 1000U) == UINT64_MAX);

    /* No new loss: an additional loss-round-end cannot cut a second time. */
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.bw_shortterm_bytes_per_sec == 7000U);
    CHECK(s.inflight_shortterm_bytes == 7000U);

    /* Subsequent round's delivered rate/bytes are the short-term floor. */
    ack.loss_round_end = 0U;
    ack.delivery_rate_bytes_per_sec = 6700U;
    ack.newly_delivered_bytes = 5800U;
    ack.newly_lost_bytes = 100U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    ack.newly_lost_bytes = 0U;
    ack.loss_round_end = 1U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.bw_shortterm_bytes_per_sec == 6700U);
    CHECK(s.inflight_shortterm_bytes == 5800U);
    CHECK(tcp_shift_bbr3_congestion_undo_spurious(&s) == 0);
    CHECK(s.bw_shortterm_bytes_per_sec == UINT64_MAX);
    CHECK(s.inflight_shortterm_bytes == UINT64_MAX);
    CHECK(s.inflight_longterm_bytes == UINT64_MAX);
    CHECK(s.loss_in_round == 0U);
    CHECK(s.ecn_round_delivered_bytes == 0U);
    CHECK(s.ecn_round_ce_bytes == 0U);
    CHECK(s.bw_latest_bytes_per_sec == 0U);
    CHECK(s.inflight_latest_bytes == 0U);
    CHECK(tcp_shift_bbr3_congestion_undo_spurious(&s) < 0);

    tcp_shift_bbr3_congestion_begin_probe(&s);
    CHECK(s.bw_shortterm_bytes_per_sec == UINT64_MAX);
    return 0;
}

static int check_loss_threshold_and_probe_bound(void)
{
    struct tcp_shift_bbr3_congestion s;
    struct tcp_shift_bbr3_congestion_ack ack = ack_defaults();
    int signals;

    tcp_shift_bbr3_congestion_init(&s);
    ack.probing_bandwidth = 1U;
    ack.probe_up = 1U;
    ack.probe_sample = 1U;
    ack.newly_lost_bytes = 200U; /* exactly 2% is not above threshold */
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_longterm_bytes == UINT64_MAX);

    ack.newly_lost_bytes = 201U;
    ack.app_limited = 1U;
    signals = tcp_shift_bbr3_congestion_on_ack(&s, &ack);
    CHECK(signals == TCP_SHIFT_BBR3_SIGNAL_EXCESS_LOSS);
    CHECK(s.inflight_longterm_bytes == UINT64_MAX);
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_longterm_bytes == UINT64_MAX);

    tcp_shift_bbr3_congestion_begin_probe(&s);
    ack.app_limited = 0U;
    signals = tcp_shift_bbr3_congestion_on_ack(&s, &ack);
    CHECK(signals == TCP_SHIFT_BBR3_SIGNAL_EXCESS_LOSS);
    CHECK(s.inflight_longterm_bytes == 10000U);
    CHECK(s.probe_feedback_consumed == 1U);

    /* Qualified safe probe can grow known upper bound to tx_inflight. */
    tcp_shift_bbr3_congestion_begin_probe(&s);
    ack.newly_lost_bytes = 0U;
    ack.tx_inflight_bytes = 13000U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_longterm_bytes == 13000U);

    /* Probe_UP growth requires cwnd-limited and cwnd >= the upper bound. */
    ack.cwnd_limited = 0U;
    ack.cwnd_bytes = 14000U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_longterm_bytes == 13000U);
    ack.cwnd_limited = 1U;
    ack.newly_acked_bytes = 14000U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_longterm_bytes == 14000U);
    ack.round_start = 1U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.probe_up_rounds == 1U);

    CHECK(tcp_shift_bbr3_congestion_cwnd_cap(
        &s, TCP_SHIFT_BBR3_PROBE_BW_UP, 1000U) >= 14000U);
    CHECK(tcp_shift_bbr3_congestion_cwnd_cap(
        &s, TCP_SHIFT_BBR3_PROBE_BW_CRUISE, 1000U) <
          s.inflight_longterm_bytes);
    CHECK(tcp_shift_bbr3_congestion_cwnd_cap(
        &s, TCP_SHIFT_BBR3_STARTUP, 1000U) == UINT64_MAX);
    return 0;
}

static int check_ecn_gating_and_lower_bound(void)
{
    struct tcp_shift_bbr3_congestion s;
    struct tcp_shift_bbr3_congestion_ack ack = ack_defaults();
    uint64_t before;

    tcp_shift_bbr3_congestion_init(&s);
    ack.newly_ce_bytes = 100U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) < 0);
    CHECK(s.ecn_in_round == 0U);

    ack.ecn_eligible = 1U;
    ack.newly_ce_bytes = 600U;
    ack.probing_bandwidth = 1U;
    ack.probe_sample = 1U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) ==
          TCP_SHIFT_BBR3_SIGNAL_EXCESS_ECN);
    CHECK(s.inflight_longterm_bytes == 10000U);
    CHECK(s.bw_shortterm_bytes_per_sec == UINT64_MAX);

    ack.probing_bandwidth = 0U;
    ack.probe_sample = 0U;
    ack.newly_ce_bytes = 0U;
    ack.loss_round_end = 1U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_shortterm_bytes < 10000U);
    CHECK(s.bw_shortterm_bytes_per_sec == UINT64_MAX);
    CHECK(s.ecn_alpha_q8 < TCP_SHIFT_BBR3_ECN_ALPHA_SCALE);

    before = s.inflight_shortterm_bytes;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_shortterm_bytes == before);
    tcp_shift_bbr3_congestion_begin_probe(&s);
    CHECK(s.inflight_shortterm_bytes == UINT64_MAX);
    return 0;
}

static int check_delivered_floor_and_input_validation(void)
{
    struct tcp_shift_bbr3_congestion s;
    struct tcp_shift_bbr3_congestion_ack ack = ack_defaults();

    tcp_shift_bbr3_congestion_init(&s);
    ack.newly_lost_bytes = 1U;
    ack.delivery_rate_bytes_per_sec = 9000U;
    ack.newly_delivered_bytes = 9000U;
    ack.loss_round_end = 1U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.bw_shortterm_bytes_per_sec == 9000U);
    CHECK(s.inflight_shortterm_bytes == 9000U);

    ack.newly_ce_bytes = ack.newly_delivered_bytes + 1U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) < 0);
    ack.newly_ce_bytes = 0U;
    ack.mss_bytes = 0U;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) < 0);
    CHECK(s.bw_shortterm_bytes_per_sec == 9000U);
    CHECK(s.inflight_shortterm_bytes == 9000U);

    /* No undefined shift/division at the 64-bit arithmetic boundary. */
    ack = ack_defaults();
    tcp_shift_bbr3_congestion_init(&s);
    ack.probing_bandwidth = 1U;
    ack.probe_sample = 1U;
    ack.probe_up = 1U;
    ack.newly_lost_bytes = UINT64_MAX / 100U;
    ack.tx_inflight_bytes = UINT64_MAX;
    CHECK(tcp_shift_bbr3_congestion_on_ack(&s, &ack) == 0);
    CHECK(s.inflight_longterm_bytes == UINT64_MAX);
    return 0;
}

/* Integrate the independent signal classifier with the phase-2 scheduler.
 * This test explicitly wires qualified congestion to the scheduler; no
 * production lwIP event has been connected yet. */
static int check_qualified_signal_to_probe_scheduler(void)
{
    struct tcp_shift_bbr3_model model;
    struct tcp_shift_bbr3_probe probe;
    struct tcp_shift_bbr3_congestion congestion;
    struct tcp_shift_bbr3_congestion_ack signal_ack = ack_defaults();
    struct tcp_shift_cc_rate_sample rate_sample;
    struct tcp_shift_bbr3_probe_ack phase_ack;
    uint64_t time_ns;

    tcp_shift_bbr3_model_init(&model);
    model.full_bw_reached = 1U;
    model.has_min_rtt = 1U;
    model.min_rtt_ns = 100000000U;
    model.max_bw_bytes_per_sec = 10000U;
    memset(&probe, 0, sizeof(probe));
    tcp_shift_bbr3_congestion_init(&congestion);
    CHECK(tcp_shift_bbr3_probe_enter(&probe, &model, 100U,
                                     1000U, 37U) == 0);

    memset(&rate_sample, 0, sizeof(rate_sample));
    rate_sample.flags = TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    rate_sample.delivery_rate_bytes_per_sec = 10000U;
    rate_sample.prior_delivered_bytes = 1000U;
    rate_sample.delivered_total_bytes = 1100U;

    memset(&phase_ack, 0, sizeof(phase_ack));
    phase_ack.rate = &rate_sample;
    phase_ack.mss_bytes = 1000U;
    phase_ack.cwnd_bytes = 10000U;
    phase_ack.inflight_bytes = 3000U;
    phase_ack.inflight_longterm_bytes = UINT64_MAX;
    phase_ack.now_ns = 100U + probe.wait_ns + 1U;
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model,
                                      &phase_ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL);

    rate_sample.prior_delivered_bytes = 1100U;
    rate_sample.delivered_total_bytes = 1200U;
    phase_ack.now_ns++;
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model,
                                      &phase_ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_UP);

    tcp_shift_bbr3_congestion_begin_probe(&congestion);
    signal_ack.probing_bandwidth = 1U;
    signal_ack.probe_sample = 1U;
    signal_ack.probe_up = 1U;
    signal_ack.newly_lost_bytes = 300U; /* 3% > 2% threshold */
    CHECK(tcp_shift_bbr3_congestion_on_ack(&congestion, &signal_ack) ==
          TCP_SHIFT_BBR3_SIGNAL_EXCESS_LOSS);
    CHECK(congestion.inflight_longterm_bytes == 10000U);

    rate_sample.prior_delivered_bytes = 1200U;
    rate_sample.delivered_total_bytes = 1300U;
    time_ns = phase_ack.now_ns + 1U;
    phase_ack.now_ns = time_ns;
    phase_ack.qualified_loss_too_high = 1U;
    phase_ack.inflight_longterm_bytes =
        congestion.inflight_longterm_bytes;
    CHECK(tcp_shift_bbr3_probe_on_ack(&probe, &model,
                                      &phase_ack) == 0);
    CHECK(model.phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN);
    CHECK(probe.previous_probe_too_high == 1U);
    CHECK(congestion.probe_feedback_consumed == 1U);
    return 0;
}

int main(void)
{
    CHECK(check_loss_round_and_undo() == 0);
    CHECK(check_loss_threshold_and_probe_bound() == 0);
    CHECK(check_ecn_gating_and_lower_bound() == 0);
    CHECK(check_delivered_floor_and_input_validation() == 0);
    CHECK(check_qualified_signal_to_probe_scheduler() == 0);
    puts("bbr3_congestion_contract=ok loss_round_once=ok "
         "loss_thresh=2pct ecn_eligible_only=ok "
         "app_limited_upper_guard=ok undo=ok cwnd_caps=ok");
    return 0;
}
