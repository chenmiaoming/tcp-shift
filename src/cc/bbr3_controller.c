#include "cc/bbr3_controller.h"

#include <stddef.h>

#define TCP_SHIFT_BBR3_NS_PER_SECOND UINT64_C(1000000000)

static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t max64(uint64_t a, uint64_t b) { return a > b ? a : b; }
static uint64_t add64(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}
static uint64_t mul64(uint64_t a, uint64_t b)
{
    return b != 0U && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}
static uint32_t cap32(uint64_t v)
{
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}
/* Exact, saturating floor(bw * RTT / 1e9): decomposing both operands
 * avoids the false BDP underestimation caused by overflowing bw*RTT before
 * dividing, even on 64-bit high-bandwidth, high-delay paths. */
static uint64_t bdp(uint64_t bw, uint64_t rtt)
{
    uint64_t seconds = rtt / TCP_SHIFT_BBR3_NS_PER_SECOND;
    uint64_t nanos = rtt % TCP_SHIFT_BBR3_NS_PER_SECOND;
    uint64_t whole = mul64(bw, seconds);
    uint64_t partial = mul64(bw / TCP_SHIFT_BBR3_NS_PER_SECOND, nanos);
    uint64_t cross = (bw % TCP_SHIFT_BBR3_NS_PER_SECOND) * nanos;
    return add64(add64(whole, partial),
                 cross / TCP_SHIFT_BBR3_NS_PER_SECOND);
}

int tcp_shift_bbr3_controller_init(struct tcp_shift_bbr3_controller *s,
                                    uint32_t initial_cwnd_bytes,
                                    uint32_t mss_bytes,
                                    uint32_t flow_seed,
                                    uint64_t now_ns)
{
    uint32_t i;
    uint64_t initial_rate;

    if (s == NULL || mss_bytes == 0U || flow_seed == 0U ||
        now_ns == 0U || mss_bytes > UINT32_MAX / 4U ||
        (uint64_t)initial_cwnd_bytes < (uint64_t)4U * mss_bytes) {
        return -1;
    }
    tcp_shift_bbr3_model_init(&s->model);
    tcp_shift_bbr3_congestion_init(&s->congestion);
    s->probe.initialized = 0U;
    for (i = 0U; i < TCP_SHIFT_BBR3_EXTRA_ACKED_ROUNDS; i++) {
        s->extra_acked_values[i] = 0U;
        s->extra_acked_rounds[i] = 0U;
    }
    s->extra_acked_interval_stamp_ns = now_ns;
    s->extra_acked_delivered_bytes = 0U;
    s->extra_acked_bytes = 0U;
    s->probe_rtt_done_stamp_ns = 0U;
    s->probe_rtt_round_delivered = 0U;
    s->probe_rtt_round_done = 0U;
    s->probe_rtt_started = 0U;
    s->initial_cwnd_bytes = initial_cwnd_bytes;
    s->cwnd_bytes = initial_cwnd_bytes;
    s->prior_cwnd_bytes = 0U;
    s->mss_bytes = mss_bytes;
    s->flow_seed = flow_seed;
    s->drain_start_round = 0U;
    /* BBRv3 initializes pace to 2.77 * initial_cwnd / 1ms. */
    initial_rate = mul64((uint64_t)initial_cwnd_bytes, 1000U);
    s->pacing_rate_bytes_per_sec = tcp_shift_bbr3_scale(
        initial_rate, (struct tcp_shift_bbr3_gain){277U, 100U});
    return 0;
}

static void update_ack_aggregation(struct tcp_shift_bbr3_controller *s,
                                    const struct tcp_shift_bbr3_controller_ack *a)
{
    uint64_t expected;
    uint64_t extra;
    uint32_t index;
    uint32_t len;
    uint32_t i;
    uint32_t round = s->model.round_count;
    uint64_t bw = tcp_shift_bbr3_congestion_rate_cap(
        &s->congestion, s->model.phase, s->model.max_bw_bytes_per_sec);

    expected = bdp(bw, a->now_ns - s->extra_acked_interval_stamp_ns);
    if (s->extra_acked_delivered_bytes <= expected) {
        s->extra_acked_delivered_bytes = 0U;
        s->extra_acked_interval_stamp_ns = a->now_ns;
        expected = 0U;
    }
    s->extra_acked_delivered_bytes =
        add64(s->extra_acked_delivered_bytes, a->acked_bytes);
    extra = s->extra_acked_delivered_bytes > expected
                ? s->extra_acked_delivered_bytes - expected : 0U;
    extra = min64(extra, s->cwnd_bytes);
    index = round % TCP_SHIFT_BBR3_EXTRA_ACKED_ROUNDS;
    if (s->extra_acked_rounds[index] != round) {
        s->extra_acked_rounds[index] = round;
        s->extra_acked_values[index] = 0U;
    }
    s->extra_acked_values[index] = max64(s->extra_acked_values[index], extra);
    len = s->model.full_bw_reached != 0U
              ? TCP_SHIFT_BBR3_EXTRA_ACKED_ROUNDS : 1U;
    s->extra_acked_bytes = 0U;
    for (i = 0U; i < TCP_SHIFT_BBR3_EXTRA_ACKED_ROUNDS; i++) {
        if (round >= s->extra_acked_rounds[i] &&
            round - s->extra_acked_rounds[i] < len) {
            s->extra_acked_bytes =
                max64(s->extra_acked_bytes, s->extra_acked_values[i]);
        }
    }
}

static uint64_t model_inflight(struct tcp_shift_bbr3_controller *s)
{
    uint64_t bw = tcp_shift_bbr3_congestion_rate_cap(
        &s->congestion, s->model.phase, s->model.max_bw_bytes_per_sec);
    uint64_t base;
    uint64_t inflight;

    if (s->model.has_min_rtt == 0U || bw == 0U) {
        return s->initial_cwnd_bytes;
    }
    base = bdp(bw, s->model.min_rtt_ns);
    inflight = tcp_shift_bbr3_scale(
        base, tcp_shift_bbr3_cwnd_gain(s->model.phase));
    inflight = add64(inflight, s->extra_acked_bytes);
    inflight = max64(inflight, (uint64_t)s->mss_bytes * 4U);
    if (s->model.phase == TCP_SHIFT_BBR3_PROBE_BW_UP) {
        inflight = add64(inflight, (uint64_t)s->mss_bytes * 2U);
    }
    return inflight;
}

static uint64_t probe_rtt_target(struct tcp_shift_bbr3_controller *s)
{
    uint64_t b = bdp(s->model.max_bw_bytes_per_sec,
                     s->model.min_rtt_ns);
    uint64_t half = b / 2U;
    return max64(half, (uint64_t)s->mss_bytes * 4U);
}

static void update_probe_rtt(struct tcp_shift_bbr3_controller *s,
                             const struct tcp_shift_bbr3_controller_ack *a)
{
    if (s->model.phase != TCP_SHIFT_BBR3_PROBE_RTT &&
        s->model.probe_rtt_expired != 0U && a->idle_restart == 0U &&
        s->model.has_min_rtt != 0U) {
        s->prior_cwnd_bytes = s->cwnd_bytes;
        s->model.phase = TCP_SHIFT_BBR3_PROBE_RTT;
        s->probe_rtt_started = 1U;
        s->probe_rtt_round_done = 0U;
        s->probe_rtt_done_stamp_ns = 0U;
        s->probe_rtt_round_delivered = a->rate.delivered_total_bytes;
    }
    if (s->model.phase != TCP_SHIFT_BBR3_PROBE_RTT) {
        return;
    }
    if (s->probe_rtt_done_stamp_ns == 0U) {
        if (a->inflight_bytes <= probe_rtt_target(s)) {
            s->probe_rtt_done_stamp_ns = add64(
                a->now_ns, TCP_SHIFT_BBR3_PROBE_RTT_DURATION_NS);
            s->probe_rtt_round_delivered = a->rate.delivered_total_bytes;
            s->probe_rtt_round_done = 0U;
        }
        return;
    }
    if (a->rate.prior_delivered_bytes >= s->probe_rtt_round_delivered &&
        a->rate.delivered_total_bytes > a->rate.prior_delivered_bytes) {
        s->probe_rtt_round_done = 1U;
    }
    if (s->probe_rtt_round_done == 0U ||
        a->now_ns <= s->probe_rtt_done_stamp_ns) {
        return;
    }
    s->model.probe_rtt_min_stamp_ns = a->now_ns;
    s->model.probe_rtt_expired = 0U;
    s->probe_rtt_started = 0U;
    tcp_shift_bbr3_congestion_reset_shortterm(&s->congestion);
    s->cwnd_bytes = max64(s->cwnd_bytes, s->prior_cwnd_bytes);
    if (s->model.full_bw_reached != 0U) {
        s->probe.initialized = 0U;
        if (tcp_shift_bbr3_probe_enter(
                &s->probe, &s->model, a->now_ns,
                a->rate.delivered_total_bytes, s->flow_seed) == 0) {
            s->model.phase = TCP_SHIFT_BBR3_PROBE_BW_CRUISE;
        }
    } else {
        s->model.phase = TCP_SHIFT_BBR3_STARTUP;
    }
}

static void advance_phase(struct tcp_shift_bbr3_controller *s,
                           const struct tcp_shift_bbr3_controller_ack *a,
                           int signal)
{
    uint64_t target;
    struct tcp_shift_bbr3_probe_ack probe_ack;
    enum tcp_shift_bbr3_phase was = s->model.phase;

    if (s->model.phase == TCP_SHIFT_BBR3_STARTUP &&
        s->model.full_bw_reached != 0U) {
        s->model.phase = TCP_SHIFT_BBR3_DRAIN;
        s->drain_start_round = s->model.round_count;
    }
    if (s->model.phase == TCP_SHIFT_BBR3_DRAIN) {
        target = bdp(s->model.max_bw_bytes_per_sec, s->model.min_rtt_ns);
        if (s->model.has_min_rtt != 0U &&
            (a->inflight_bytes <= target ||
             s->model.round_count > s->drain_start_round + 3U)) {
            (void)tcp_shift_bbr3_probe_enter(
                &s->probe, &s->model, a->now_ns,
                a->rate.delivered_total_bytes, s->flow_seed);
        }
    }
    if (was >= TCP_SHIFT_BBR3_PROBE_BW_DOWN &&
        was <= TCP_SHIFT_BBR3_PROBE_BW_UP) {
        probe_ack.rate = &a->rate;
        probe_ack.now_ns = a->now_ns;
        probe_ack.inflight_longterm_bytes =
            s->congestion.inflight_longterm_bytes;
        probe_ack.inflight_bytes = a->inflight_bytes;
        probe_ack.cwnd_bytes = s->cwnd_bytes;
        probe_ack.mss_bytes = s->mss_bytes;
        probe_ack.cwnd_limited = a->cwnd_limited;
        probe_ack.qualified_loss_too_high = signal != 0;
        (void)tcp_shift_bbr3_probe_on_ack(
            &s->probe, &s->model, &probe_ack);
        if (was != TCP_SHIFT_BBR3_PROBE_BW_REFILL &&
            s->model.phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL) {
            tcp_shift_bbr3_congestion_begin_probe(&s->congestion);
        }
    }
    update_probe_rtt(s, a);
}

static void update_policy(struct tcp_shift_bbr3_controller *s,
                           const struct tcp_shift_bbr3_controller_ack *a,
                           struct tcp_shift_bbr3_controller_policy *p)
{
    uint64_t bw = tcp_shift_bbr3_congestion_rate_cap(
        &s->congestion, s->model.phase, s->model.max_bw_bytes_per_sec);
    uint64_t desired;
    uint64_t inflight = model_inflight(s);
    uint64_t cwnd;
    uint64_t quantum;
    uint64_t bound;

    desired = tcp_shift_bbr3_scale(
        bw, tcp_shift_bbr3_pacing_gain(s->model.phase));
    desired = tcp_shift_bbr3_scale(
        desired, (struct tcp_shift_bbr3_gain){
            TCP_SHIFT_BBR3_PACING_MARGIN_NUM, 100U});
    if (desired != 0U &&
        (s->model.full_bw_reached != 0U ||
         desired > s->pacing_rate_bytes_per_sec)) {
        s->pacing_rate_bytes_per_sec = desired;
    }
    if (s->model.full_bw_reached != 0U) {
        cwnd = min64(add64(s->cwnd_bytes, a->acked_bytes), inflight);
    } else if (s->cwnd_bytes < inflight ||
               a->rate.delivered_total_bytes < s->initial_cwnd_bytes) {
        cwnd = add64(s->cwnd_bytes, a->acked_bytes);
    } else {
        cwnd = s->cwnd_bytes;
    }
    cwnd = max64(cwnd, (uint64_t)s->mss_bytes * 4U);
    if (s->model.phase == TCP_SHIFT_BBR3_PROBE_RTT) {
        cwnd = min64(cwnd, probe_rtt_target(s));
    }
    bound = tcp_shift_bbr3_congestion_cwnd_cap(
        &s->congestion, s->model.phase, s->mss_bytes);
    cwnd = min64(cwnd, bound);
    cwnd = max64(cwnd, (uint64_t)s->mss_bytes * 4U);
    s->cwnd_bytes = cap32(cwnd);

    quantum = s->pacing_rate_bytes_per_sec / 1000U;
    quantum = min64(quantum, 65536U);
    quantum = max64(quantum, (uint64_t)s->mss_bytes * 2U);
    p->pacing_rate_bytes_per_sec = s->pacing_rate_bytes_per_sec;
    p->max_inflight_bytes = inflight;
    p->extra_acked_bytes = s->extra_acked_bytes;
    p->cwnd_bytes = s->cwnd_bytes;
    p->send_quantum_bytes = cap32(quantum);
    p->mark_app_limited =
        s->model.phase == TCP_SHIFT_BBR3_PROBE_RTT ? 1U : 0U;
    p->phase = s->model.phase;
}

int tcp_shift_bbr3_controller_on_ack(
    struct tcp_shift_bbr3_controller *s,
    const struct tcp_shift_bbr3_controller_ack *a,
    struct tcp_shift_bbr3_controller_policy *p)
{
    struct tcp_shift_bbr3_congestion_ack c;
    int signal;
    enum tcp_shift_bbr3_phase phase;

    if (s == NULL || a == NULL || p == NULL || a->now_ns == 0U ||
        a->now_ns < s->extra_acked_interval_stamp_ns ||
        a->now_ns < s->model.last_update_ns ||
        (a->lost_bytes != 0U && a->validated_loss == 0U) ||
        (a->ce_bytes != 0U && a->ecn_eligible == 0U) ||
        a->ce_bytes > a->acked_bytes ||
        a->rate.prior_delivered_bytes > a->rate.delivered_total_bytes ||
        (a->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) == 0U) {
        return -1;
    }
    phase = s->model.phase;
    if (tcp_shift_bbr3_model_on_ack(&s->model, &a->rate,
                                    a->now_ns) != 0) {
        return -1;
    }
    c.delivery_rate_bytes_per_sec = a->rate.delivery_rate_bytes_per_sec;
    c.max_bw_bytes_per_sec = s->model.max_bw_bytes_per_sec;
    c.newly_delivered_bytes = a->acked_bytes;
    c.rate_sample_delivered_bytes = a->rate.delivered_bytes;
    c.newly_lost_bytes = a->lost_bytes;
    c.newly_ce_bytes = a->ce_bytes;
    c.tx_inflight_bytes = a->tx_inflight_bytes;
    c.cwnd_bytes = s->cwnd_bytes;
    c.target_inflight_bytes = model_inflight(s);
    c.newly_acked_bytes = a->acked_bytes;
    c.mss_bytes = s->mss_bytes;
    c.rate_valid = 1U;
    c.app_limited =
        (a->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U;
    c.ecn_eligible = a->ecn_eligible;
    c.probing_bandwidth =
        phase == TCP_SHIFT_BBR3_STARTUP ||
        phase == TCP_SHIFT_BBR3_PROBE_BW_REFILL ||
        phase == TCP_SHIFT_BBR3_PROBE_BW_UP;
    c.probe_sample = a->probe_sample;
    c.probe_up = phase == TCP_SHIFT_BBR3_PROBE_BW_UP;
    c.cwnd_limited = a->cwnd_limited;
    c.loss_round_end = a->loss_round_end;
    c.round_start = s->model.round_start;
    signal = tcp_shift_bbr3_congestion_on_ack(&s->congestion, &c);
    if (signal < 0) {
        return -1;
    }
    update_ack_aggregation(s, a);
    advance_phase(s, a, signal);
    update_policy(s, a, p);
    return 0;
}
