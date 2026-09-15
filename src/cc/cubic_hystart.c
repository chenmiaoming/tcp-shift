#include "cc/cubic.h"

#include <limits.h>
#include <stddef.h>

static uint64_t tcp_shift_cubic_hystart_delay_threshold(uint64_t delay_min_ns)
{
    uint64_t threshold = delay_min_ns / 8U;

    if (threshold < TCP_SHIFT_CUBIC_HYSTART_DELAY_MIN_NS) {
        threshold = TCP_SHIFT_CUBIC_HYSTART_DELAY_MIN_NS;
    }
    if (threshold > TCP_SHIFT_CUBIC_HYSTART_DELAY_MAX_NS) {
        threshold = TCP_SHIFT_CUBIC_HYSTART_DELAY_MAX_NS;
    }
    return threshold;
}

static uint64_t tcp_shift_cubic_hystart_add_sat_u64(uint64_t left,
                                                     uint64_t right)
{
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

void tcp_shift_cubic_hystart_reset(struct tcp_shift_cubic_model *model)
{
    if (model == NULL) {
        return;
    }

    model->hystart_delay_min_ns = 0U;
    model->hystart_curr_rtt_ns = UINT64_MAX;
    model->hystart_round_start_ns = 0U;
    model->hystart_last_ack_ns = 0U;
    model->hystart_next_round_delivered = 0U;
    model->hystart_sample_count = 0U;
    model->hystart_found = 0U;
    model->hystart_ack_train_found = 0U;
    model->hystart_delay_found = 0U;
    model->hystart_enabled = 1U;
}

static void tcp_shift_cubic_hystart_start_round(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint64_t now_ns)
{
    model->hystart_round_start_ns = now_ns;
    model->hystart_last_ack_ns = now_ns;
    model->hystart_curr_rtt_ns = UINT64_MAX;
    model->hystart_sample_count = 0U;

    /* Linux records end_seq=snd_nxt when a HyStart round starts. In the
     * transport-neutral delivery domain, cumulative delivered bytes plus the
     * bytes still in flight is the matching end-of-flight boundary. Reset the
     * round only after cumulative delivery crosses that boundary; using the
     * current delivered total alone makes delayed/multi-segment ACK streams
     * start a new round too early and starves both HyStart detectors. */
    if (ack->rate.delivered_total_bytes != 0U) {
        model->hystart_next_round_delivered =
            tcp_shift_cubic_hystart_add_sat_u64(
                ack->rate.delivered_total_bytes,
                transport->inflight_bytes);
    }
}

static int tcp_shift_cubic_hystart_is_slow_start(
    const struct tcp_shift_cubic_model *model)
{
    return model->cwnd_q16 < model->ssthresh_q16;
}

static int tcp_shift_cubic_hystart_low_window_reached(
    const struct tcp_shift_cubic_model *model)
{
    uint64_t low_window_q16 =
        (uint64_t)TCP_SHIFT_CUBIC_HYSTART_LOW_WINDOW << TCP_SHIFT_CUBIC_Q_SHIFT;

    return model->cwnd_q16 >= low_window_q16;
}

static void tcp_shift_cubic_hystart_exit_slow_start(
    struct tcp_shift_cubic_model *model)
{
    model->ssthresh_q16 = model->cwnd_q16;
    model->cwnd_prior_q16 = model->cwnd_q16;
    model->w_max_q16 = model->cwnd_q16;
    model->has_w_max = 1U;
    model->k_q10 = 0U;
    model->epoch_active = 0U;
    model->after_timeout = 0U;
    model->hystart_found = 1U;
    model->hystart_exit_events++;
}

int tcp_shift_cubic_hystart_on_ack(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint64_t now_ns)
{
    uint64_t rtt_ns;
    uint64_t ack_gap_ns;
    uint64_t train_threshold_ns;
    uint64_t delay_threshold_ns;
    int new_round;

    if (model == NULL || transport == NULL || ack == NULL || now_ns == 0U ||
        transport->mss_bytes == 0U) {
        return -1;
    }
    if (model->hystart_enabled == 0U || model->hystart_found != 0U ||
        tcp_shift_cubic_hystart_is_slow_start(model) == 0) {
        return 0;
    }
    if (model->mss_bytes != transport->mss_bytes) {
        /* Let the main CUBIC model rescale segment-domain state first. */
        return 0;
    }
    if ((ack->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U ||
        (ack->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) == 0U ||
        ack->rate.rtt_ns == 0U) {
        return 0;
    }

    rtt_ns = ack->rate.rtt_ns;
    if (model->hystart_delay_min_ns == 0U ||
        rtt_ns < model->hystart_delay_min_ns) {
        model->hystart_delay_min_ns = rtt_ns;
    }

    new_round = model->hystart_round_start_ns == 0U;
    if (new_round == 0 && model->hystart_next_round_delivered != 0U &&
        ack->rate.delivered_total_bytes >
            model->hystart_next_round_delivered) {
        new_round = 1;
    }
    if (new_round != 0) {
        tcp_shift_cubic_hystart_start_round(model, transport, ack, now_ns);
    }

    if (tcp_shift_cubic_hystart_low_window_reached(model) == 0) {
        return 0;
    }

    /* Linux tcp_cubic's classic HyStart ACK-train detector uses a 2 ms ACK
     * spacing threshold. tcp-shift CUBIC currently requests no pacing, so use
     * the unpaced Linux threshold of half the minimum observed RTT. */
    if (model->hystart_last_ack_ns != 0U &&
        now_ns >= model->hystart_last_ack_ns) {
        ack_gap_ns = now_ns - model->hystart_last_ack_ns;
        if (ack_gap_ns <= TCP_SHIFT_CUBIC_HYSTART_ACK_DELTA_NS) {
            model->hystart_last_ack_ns = now_ns;
            train_threshold_ns = model->hystart_delay_min_ns / 2U;
            if (now_ns >= model->hystart_round_start_ns &&
                now_ns - model->hystart_round_start_ns > train_threshold_ns) {
                model->hystart_ack_train_found = 1U;
            }
        }
    }

    if (rtt_ns < model->hystart_curr_rtt_ns) {
        model->hystart_curr_rtt_ns = rtt_ns;
    }
    if (model->hystart_sample_count < TCP_SHIFT_CUBIC_HYSTART_MIN_SAMPLES) {
        model->hystart_sample_count++;
    } else {
        delay_threshold_ns = tcp_shift_cubic_hystart_delay_threshold(
            model->hystart_delay_min_ns);
        if (model->hystart_curr_rtt_ns >
            model->hystart_delay_min_ns + delay_threshold_ns) {
            model->hystart_delay_found = 1U;
        }
    }

    if (model->hystart_ack_train_found != 0U ||
        model->hystart_delay_found != 0U) {
        tcp_shift_cubic_hystart_exit_slow_start(model);
        return 1;
    }
    return 0;
}
