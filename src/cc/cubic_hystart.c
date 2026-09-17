#include "cc/cubic.h"

#include <limits.h>
#include <stddef.h>

static uint64_t tcp_shift_cubic_hystart_delay_threshold(
    uint64_t last_round_min_rtt_ns)
{
    uint64_t threshold =
        last_round_min_rtt_ns / TCP_SHIFT_CUBIC_HYSTARTPP_MIN_RTT_DIVISOR;

    if (threshold < TCP_SHIFT_CUBIC_HYSTARTPP_MIN_RTT_THRESH_NS) {
        threshold = TCP_SHIFT_CUBIC_HYSTARTPP_MIN_RTT_THRESH_NS;
    }
    if (threshold > TCP_SHIFT_CUBIC_HYSTARTPP_MAX_RTT_THRESH_NS) {
        threshold = TCP_SHIFT_CUBIC_HYSTARTPP_MAX_RTT_THRESH_NS;
    }
    return threshold;
}

static uint64_t tcp_shift_cubic_hystart_add_sat_u64(uint64_t left,
                                                     uint64_t right)
{
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

static int tcp_shift_cubic_hystart_is_slow_start(
    const struct tcp_shift_cubic_model *model)
{
    return model->cwnd_q16 < model->ssthresh_q16;
}

void tcp_shift_cubic_hystart_reset(struct tcp_shift_cubic_model *model)
{
    if (model == NULL) {
        return;
    }

    model->hystart_last_round_min_rtt_ns = UINT64_MAX;
    model->hystart_current_round_min_rtt_ns = UINT64_MAX;
    model->hystart_css_baseline_min_rtt_ns = UINT64_MAX;
    model->hystart_next_round_delivered = 0U;
    model->hystart_sample_count = 0U;
    model->hystart_css_rounds = 0U;
    model->hystart_exit_events = 0U;
    model->hystart_css_enter_events = 0U;
    model->hystart_css_revert_events = 0U;
    model->hystart_enabled = 1U;
    model->hystart_css = 0U;
    model->hystart_ack_css = 0U;
    model->hystart_exit_pending = 0U;
    model->hystart_initial_complete = 0U;
}

void tcp_shift_cubic_hystart_disable(struct tcp_shift_cubic_model *model)
{
    if (model == NULL) {
        return;
    }

    model->hystart_enabled = 0U;
    model->hystart_css = 0U;
    model->hystart_ack_css = 0U;
    model->hystart_exit_pending = 0U;
    model->hystart_initial_complete = 1U;
}

static void tcp_shift_cubic_hystart_start_round(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack)
{
    model->hystart_last_round_min_rtt_ns =
        model->hystart_current_round_min_rtt_ns;
    model->hystart_current_round_min_rtt_ns = UINT64_MAX;
    model->hystart_sample_count = 0U;
    model->hystart_next_round_delivered =
        tcp_shift_cubic_hystart_add_sat_u64(
            ack->rate.delivered_total_bytes,
            transport->inflight_bytes);
}

int tcp_shift_cubic_hystart_on_ack(
    struct tcp_shift_cubic_model *model,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    uint64_t now_ns)
{
    uint64_t threshold_ns;
    uint64_t trigger_rtt_ns;
    int new_round;

    if (model == NULL || transport == NULL || ack == NULL || now_ns == 0U ||
        transport->mss_bytes == 0U) {
        return -1;
    }

    /* Preserve the phase in which this ACK arrived. The RFC updates cwnd
     * before testing RTT conditions, so a transition caused by this ACK takes
     * effect on growth only from the following ACK. cubic.c consumes this bit. */
    model->hystart_ack_css = model->hystart_css;

    if (model->hystart_enabled == 0U ||
        model->hystart_initial_complete != 0U ||
        tcp_shift_cubic_hystart_is_slow_start(model) == 0) {
        return 0;
    }
    if (model->mss_bytes != transport->mss_bytes) {
        /* Let the main CUBIC model rescale segment-domain state first. */
        return 0;
    }

    new_round = model->hystart_next_round_delivered == 0U;
    if (new_round == 0 && ack->rate.delivered_total_bytes != 0U &&
        ack->rate.delivered_total_bytes >=
            model->hystart_next_round_delivered) {
        new_round = 1;
    }

    if (new_round != 0) {
        if (model->hystart_css != 0U &&
            model->hystart_next_round_delivered != 0U) {
            model->hystart_css_rounds++;
            if (model->hystart_css_rounds >=
                TCP_SHIFT_CUBIC_HYSTARTPP_CSS_ROUNDS) {
                /* The arriving ACK completes the fifth CSS round. Defer the
                 * actual ssthresh/CUBIC handoff until cubic.c has applied this
                 * ACK's CSS growth, matching RFC 9406's ACK ordering. */
                model->hystart_exit_pending = 1U;
                return 1;
            }
        }
        tcp_shift_cubic_hystart_start_round(model, transport, ack);
    }

    if ((ack->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) == 0U ||
        ack->rate.rtt_ns == 0U) {
        return 0;
    }

    if (ack->rate.rtt_ns < model->hystart_current_round_min_rtt_ns) {
        model->hystart_current_round_min_rtt_ns = ack->rate.rtt_ns;
    }
    if (model->hystart_sample_count != UINT32_MAX) {
        model->hystart_sample_count++;
    }

    if (model->hystart_css != 0U) {
        if (model->hystart_sample_count >=
                TCP_SHIFT_CUBIC_HYSTARTPP_MIN_SAMPLES &&
            model->hystart_current_round_min_rtt_ns <
                model->hystart_css_baseline_min_rtt_ns) {
            /* The delay spike was transient. RFC 9406 resumes ordinary slow
             * start and allows a later round to enter CSS again. */
            model->hystart_css = 0U;
            model->hystart_css_baseline_min_rtt_ns = UINT64_MAX;
            model->hystart_css_rounds = 0U;
            model->hystart_css_revert_events++;
            return 1;
        }
        return 0;
    }

    if (model->hystart_sample_count < TCP_SHIFT_CUBIC_HYSTARTPP_MIN_SAMPLES ||
        model->hystart_last_round_min_rtt_ns == UINT64_MAX ||
        model->hystart_current_round_min_rtt_ns == UINT64_MAX) {
        return 0;
    }

    threshold_ns = tcp_shift_cubic_hystart_delay_threshold(
        model->hystart_last_round_min_rtt_ns);
    trigger_rtt_ns = tcp_shift_cubic_hystart_add_sat_u64(
        model->hystart_last_round_min_rtt_ns, threshold_ns);
    if (model->hystart_current_round_min_rtt_ns >= trigger_rtt_ns) {
        model->hystart_css_baseline_min_rtt_ns =
            model->hystart_current_round_min_rtt_ns;
        model->hystart_css = 1U;
        model->hystart_css_rounds = 0U;
        model->hystart_css_enter_events++;
        return 1;
    }

    return 0;
}
