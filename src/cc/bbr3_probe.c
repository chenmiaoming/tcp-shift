#include "cc/bbr3_probe.h"

#include <stddef.h>

#define NS_PER_SECOND UINT64_C(1000000000)

static uint64_t sat_add(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static uint64_t sat_mul(uint64_t a, uint64_t b)
{
    if (a == 0U || b == 0U) {
        return 0U;
    }
    return a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* Exact, saturating ceil(bw_bytes_per_second * rtt_ns / 1e9).
 * Decomposition avoids non-ISO __int128 and the accidental truncation of
 * low-BDP links with fractional-MSS BDP. */
static uint64_t bdp_bytes(uint64_t bw, uint64_t rtt_ns)
{
    uint64_t result;
    uint64_t cross;

    if (bw == 0U || rtt_ns == 0U) {
        return 0U;
    }
    result = sat_add(sat_mul(bw, rtt_ns / NS_PER_SECOND),
                     sat_mul(bw / NS_PER_SECOND,
                             rtt_ns % NS_PER_SECOND));
    cross = (bw % NS_PER_SECOND) * (rtt_ns % NS_PER_SECOND);
    result = sat_add(result, cross / NS_PER_SECOND);
    return sat_add(result, (cross % NS_PER_SECOND) != 0U ? 1U : 0U);
}

static uint32_t random_next(struct tcp_shift_bbr3_probe *probe)
{
    uint32_t value = probe->random_state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    probe->random_state = value;
    return value;
}

static void begin_round(struct tcp_shift_bbr3_probe *probe,
                        uint64_t delivered_total_bytes)
{
    probe->next_round_delivered = delivered_total_bytes;
}

static void pick_wait(struct tcp_shift_bbr3_probe *probe)
{
    probe->rounds_since_probe = random_next(probe) & 1U;
    probe->wait_ns = TCP_SHIFT_BBR3_PROBE_WAIT_BASE_NS +
        (random_next(probe) % TCP_SHIFT_BBR3_PROBE_WAIT_JITTER_NS);
}

static void enter_down(struct tcp_shift_bbr3_probe *probe,
                       struct tcp_shift_bbr3_model *model,
                       uint64_t now_ns,
                       uint64_t delivered_total_bytes)
{
    pick_wait(probe);
    probe->cycle_stamp_ns = now_ns;
    probe->ack_phase = TCP_SHIFT_BBR3_ACKS_PROBE_STOPPING;
    probe->probe_bw_sample = 0U;
    begin_round(probe, delivered_total_bytes);
    model->phase = TCP_SHIFT_BBR3_PROBE_BW_DOWN;
}

static void enter_refill(struct tcp_shift_bbr3_probe *probe,
                         struct tcp_shift_bbr3_model *model,
                         uint64_t delivered_total_bytes)
{
    probe->probe_up_rounds = 0U;
    probe->up_stagnant_rounds = 0U;
    probe->previous_probe_precautionary = 0U;
    probe->ack_phase = TCP_SHIFT_BBR3_ACKS_REFILLING;
    begin_round(probe, delivered_total_bytes);
    model->phase = TCP_SHIFT_BBR3_PROBE_BW_REFILL;
}

static void enter_up(struct tcp_shift_bbr3_probe *probe,
                     struct tcp_shift_bbr3_model *model,
                     const struct tcp_shift_bbr3_probe_ack *ack)
{
    probe->ack_phase = TCP_SHIFT_BBR3_ACKS_PROBE_STARTING;
    probe->up_stagnant_rounds = 0U;
    probe->up_full_bw_bytes_per_sec = ack->rate->delivery_rate_bytes_per_sec;
    probe->probe_up_rounds = 1U;
    probe->probe_bw_sample = 1U;
    begin_round(probe, ack->rate->delivered_total_bytes);
    model->phase = TCP_SHIFT_BBR3_PROBE_BW_UP;
}

int tcp_shift_bbr3_probe_enter(struct tcp_shift_bbr3_probe *probe,
                               struct tcp_shift_bbr3_model *model,
                               uint64_t now_ns,
                               uint64_t delivered_total_bytes,
                               uint32_t flow_seed)
{
    if (probe == NULL || model == NULL || now_ns == 0U ||
        flow_seed == 0U || model->full_bw_reached == 0U ||
        model->max_bw_bytes_per_sec == 0U || model->has_min_rtt == 0U ||
        model->min_rtt_ns == 0U || probe->initialized != 0U) {
        return -1;
    }

    probe->cycle_stamp_ns = 0U;
    probe->next_round_delivered = 0U;
    probe->up_full_bw_bytes_per_sec = 0U;
    probe->wait_ns = 0U;
    probe->random_state = flow_seed;
    probe->rounds_since_probe = 0U;
    probe->up_stagnant_rounds = 0U;
    probe->probe_up_rounds = 0U;
    probe->ack_phase = TCP_SHIFT_BBR3_ACKS_INIT;
    probe->initialized = 1U;
    probe->previous_probe_too_high = 0U;
    probe->previous_probe_precautionary = 0U;
    probe->probe_bw_sample = 0U;
    enter_down(probe, model, now_ns, delivered_total_bytes);
    return 0;
}

static int round_start(struct tcp_shift_bbr3_probe *probe,
                       const struct tcp_shift_cc_rate_sample *rate)
{
    if (rate->delivered_total_bytes == 0U ||
        rate->prior_delivered_bytes < probe->next_round_delivered) {
        return 0;
    }
    begin_round(probe, rate->delivered_total_bytes);
    if (probe->rounds_since_probe < UINT32_MAX) {
        probe->rounds_since_probe++;
    }
    return 1;
}

static uint32_t reno_wait_rounds(struct tcp_shift_bbr3_model *model,
                                 const struct tcp_shift_bbr3_probe_ack *ack)
{
    uint64_t bdp = bdp_bytes(model->max_bw_bytes_per_sec,
                             model->min_rtt_ns);
    uint64_t target = bdp < ack->cwnd_bytes ? bdp : ack->cwnd_bytes;
    uint64_t packets = target / ack->mss_bytes;

    if ((target % ack->mss_bytes) != 0U) {
        packets++;
    }
    if (packets > TCP_SHIFT_BBR3_RENO_MAX_ROUNDS) {
        packets = TCP_SHIFT_BBR3_RENO_MAX_ROUNDS;
    }
    return (uint32_t)packets;
}

static int time_to_probe(struct tcp_shift_bbr3_probe *probe,
                         struct tcp_shift_bbr3_model *model,
                         const struct tcp_shift_bbr3_probe_ack *ack)
{
    if ((ack->now_ns > probe->cycle_stamp_ns &&
         ack->now_ns - probe->cycle_stamp_ns > probe->wait_ns) ||
        probe->rounds_since_probe >= reno_wait_rounds(model, ack)) {
        enter_refill(probe, model, ack->rate->delivered_total_bytes);
        return 1;
    }
    return 0;
}

static int time_to_cruise(const struct tcp_shift_bbr3_model *model,
                          const struct tcp_shift_bbr3_probe_ack *ack)
{
    uint64_t bdp = bdp_bytes(model->max_bw_bytes_per_sec,
                             model->min_rtt_ns);
    uint64_t headroom = tcp_shift_bbr3_inflight_with_headroom(
        ack->inflight_longterm_bytes, ack->mss_bytes);

    return ack->inflight_bytes <= bdp &&
           ack->inflight_bytes <= headroom;
}

static int up_plateau(struct tcp_shift_bbr3_probe *probe,
                      const struct tcp_shift_bbr3_probe_ack *ack,
                      int new_round)
{
    uint64_t threshold;
    uint64_t increment;
    uint64_t rate = ack->rate->delivery_rate_bytes_per_sec;

    if ((ack->rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) == 0U ||
        (ack->rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U ||
        rate == 0U) {
        return 0;
    }
    increment = probe->up_full_bw_bytes_per_sec / 4U;
    if ((probe->up_full_bw_bytes_per_sec % 4U) != 0U) {
        increment++;
    }
    threshold = sat_add(probe->up_full_bw_bytes_per_sec, increment);
    if (probe->up_full_bw_bytes_per_sec == 0U || rate >= threshold) {
        probe->up_full_bw_bytes_per_sec = rate;
        probe->up_stagnant_rounds = 0U;
    } else if (new_round && probe->up_stagnant_rounds < UINT32_MAX) {
        probe->up_stagnant_rounds++;
    }
    return probe->up_stagnant_rounds >= TCP_SHIFT_BBR3_FULL_BW_ROUNDS;
}

int tcp_shift_bbr3_probe_on_ack(struct tcp_shift_bbr3_probe *probe,
                                struct tcp_shift_bbr3_model *model,
                                const struct tcp_shift_bbr3_probe_ack *ack)
{
    int new_round;

    if (probe == NULL || model == NULL || ack == NULL ||
        ack->rate == NULL || probe->initialized == 0U ||
        model->full_bw_reached == 0U ||
        ack->now_ns == 0U || ack->now_ns < probe->cycle_stamp_ns ||
        ack->mss_bytes == 0U || ack->cwnd_bytes == 0U ||
        ack->rate->prior_delivered_bytes >
            ack->rate->delivered_total_bytes ||
        (model->phase < TCP_SHIFT_BBR3_PROBE_BW_DOWN ||
         model->phase > TCP_SHIFT_BBR3_PROBE_BW_UP)) {
        return -1;
    }
    /* Pure ACKs with no newly delivered data do not run the phase machine. */
    if (ack->rate->delivered_total_bytes ==
        ack->rate->prior_delivered_bytes) {
        return 0;
    }

    new_round = round_start(probe, ack->rate);
    if (model->phase == TCP_SHIFT_BBR3_PROBE_BW_DOWN &&
        new_round && probe->ack_phase ==
                         TCP_SHIFT_BBR3_ACKS_PROBE_STOPPING) {
        probe->ack_phase = TCP_SHIFT_BBR3_ACKS_INIT;
        if ((ack->rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) == 0U &&
            (ack->rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) != 0U &&
            tcp_shift_bbr3_model_advance_bw_cycle(model) != 0) {
            return -1;
        }
        if (probe->previous_probe_precautionary != 0U &&
            probe->previous_probe_too_high == 0U) {
            enter_refill(probe, model,
                         ack->rate->delivered_total_bytes);
            return 0;
        }
    }

    switch (model->phase) {
    case TCP_SHIFT_BBR3_PROBE_BW_DOWN:
        if (!time_to_probe(probe, model, ack) &&
            time_to_cruise(model, ack)) {
            model->phase = TCP_SHIFT_BBR3_PROBE_BW_CRUISE;
        }
        break;
    case TCP_SHIFT_BBR3_PROBE_BW_CRUISE:
        (void)time_to_probe(probe, model, ack);
        break;
    case TCP_SHIFT_BBR3_PROBE_BW_REFILL:
        if (new_round) {
            enter_up(probe, model, ack);
        }
        break;
    case TCP_SHIFT_BBR3_PROBE_BW_UP:
        if (new_round && probe->ack_phase ==
                             TCP_SHIFT_BBR3_ACKS_PROBE_STARTING) {
            probe->ack_phase = TCP_SHIFT_BBR3_ACKS_PROBE_FEEDBACK;
        }
        if (ack->qualified_loss_too_high != 0U) {
            probe->previous_probe_too_high = 1U;
            enter_down(probe, model, ack->now_ns,
                       ack->rate->delivered_total_bytes);
        } else if (probe->previous_probe_too_high != 0U &&
                   ack->inflight_longterm_bytes != UINT64_MAX &&
                   ack->inflight_bytes >= ack->inflight_longterm_bytes) {
            probe->previous_probe_precautionary = 1U;
            probe->previous_probe_too_high = 0U;
            enter_down(probe, model, ack->now_ns,
                       ack->rate->delivered_total_bytes);
        } else if (ack->cwnd_limited != 0U &&
                   ack->inflight_longterm_bytes != UINT64_MAX &&
                   ack->cwnd_bytes >= ack->inflight_longterm_bytes) {
            probe->up_full_bw_bytes_per_sec =
                ack->rate->delivery_rate_bytes_per_sec;
            probe->up_stagnant_rounds = 0U;
        } else if (up_plateau(probe, ack, new_round)) {
            probe->previous_probe_too_high = 0U;
            enter_down(probe, model, ack->now_ns,
                       ack->rate->delivered_total_bytes);
        }
        break;
    default:
        return -1;
    }
    return 0;
}
