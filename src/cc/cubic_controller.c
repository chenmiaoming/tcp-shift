#include "cc/cubic.h"

uint32_t tcp_shift_cubic_hystartpp_slow_start_credit(
    uint32_t acked_bytes,
    uint32_t mss_bytes,
    unsigned pacing_active)
{
    uint32_t limit;

    if (acked_bytes == 0U || mss_bytes == 0U) {
        return 0U;
    }
    if (pacing_active != 0U) {
        /* RFC 9406 recommends L=infinity when the transport actually paces
         * this flow, so every newly ACKed byte may contribute to growth. */
        return acked_bytes;
    }

    /* RFC 9406 recommends L=8 for non-paced senders. Saturate the byte limit
     * so the helper remains valid for arbitrary transport MSS values. */
    if (mss_bytes > UINT32_MAX / TCP_SHIFT_CUBIC_HYSTARTPP_NON_PACED_L) {
        limit = UINT32_MAX;
    } else {
        limit = mss_bytes * TCP_SHIFT_CUBIC_HYSTARTPP_NON_PACED_L;
    }
    return acked_bytes < limit ? acked_bytes : limit;
}

static int tcp_shift_cubic_controller_init(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_cubic_model *model =
        (struct tcp_shift_cubic_model *)opaque_state;
    int result;

    result = tcp_shift_cubic_model_init(model, transport, init, policy);
    if (result == 0) {
        tcp_shift_cubic_hystart_reset(model);
    }
    return result;
}

static int tcp_shift_cubic_controller_on_ack(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_cubic_model *model =
        (struct tcp_shift_cubic_model *)opaque_state;
    struct tcp_shift_cc_ack growth_ack;
    const struct tcp_shift_cc_ack *model_ack = ack;
    int result;

    if (ack == NULL || ack->ack_time_ns == 0U || transport == NULL) {
        return -1;
    }

    result = tcp_shift_cubic_hystart_on_ack(
        model, transport, ack, ack->ack_time_ns);
    if (result < 0) {
        return -1;
    }

    if (model->cwnd_q16 < model->ssthresh_q16) {
        growth_ack = *ack;
        /* The ordinary tcp-shift CUBIC selector does not yet install the
         * generic Reno/CUBIC transport pacing fallback. Therefore this flow is
         * non-paced even though a process-wide pacer service may exist, and
         * RFC 9406 requires the conservative L=8 ACK-growth cap. When the
         * production transport explicitly reports active per-flow pacing, this
         * call site can switch to pacing_active=1 without changing HyStart++'s
         * RTT/CSS state machine. */
        growth_ack.acked_bytes =
            tcp_shift_cubic_hystartpp_slow_start_credit(
                ack->acked_bytes, transport->mss_bytes, 0U);
        model_ack = &growth_ack;
    }

    return tcp_shift_cubic_model_on_ack(
        model,
        transport,
        model_ack,
        ack->ack_time_ns,
        ack->smoothed_rtt_ns,
        policy);
}

static int tcp_shift_cubic_controller_on_loss(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_cubic_model *model =
        (struct tcp_shift_cubic_model *)opaque_state;
    int result;

    result = tcp_shift_cubic_model_on_loss(model, transport, loss, policy);
    if (result == 0) {
        /* RFC 9406 recommends HyStart++ only for the initial slow start.
         * Any congestion signal ends that initial attempt permanently. */
        tcp_shift_cubic_hystart_disable(model);
    }
    return result;
}

static int tcp_shift_cubic_controller_on_timeout(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_cubic_model *model =
        (struct tcp_shift_cubic_model *)opaque_state;
    int result;

    result = tcp_shift_cubic_model_on_timeout(model, transport, policy);
    if (result == 0) {
        /* Subsequent slow starts use the learned ssthresh but do not re-arm
         * HyStart++. The same transport-aware L policy still bounds ACK growth. */
        tcp_shift_cubic_hystart_disable(model);
    }
    return result;
}

const struct tcp_shift_cc_ops tcp_shift_cubic_ops = {
    .name = "cubic",
    .state_size = sizeof(struct tcp_shift_cubic_model),
    .init = tcp_shift_cubic_controller_init,
    .on_ack = tcp_shift_cubic_controller_on_ack,
    .on_loss = tcp_shift_cubic_controller_on_loss,
    .on_timeout = tcp_shift_cubic_controller_on_timeout,
};
