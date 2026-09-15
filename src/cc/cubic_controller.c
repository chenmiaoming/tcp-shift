#include "cc/cubic.h"

static int tcp_shift_cubic_controller_init(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_cubic_model_init(
        (struct tcp_shift_cubic_model *)opaque_state, transport, init, policy);
}

static int tcp_shift_cubic_controller_on_ack(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    if (ack == NULL || ack->ack_time_ns == 0U) {
        return -1;
    }
    return tcp_shift_cubic_model_on_ack(
        (struct tcp_shift_cubic_model *)opaque_state,
        transport,
        ack,
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
    return tcp_shift_cubic_model_on_loss(
        (struct tcp_shift_cubic_model *)opaque_state, transport, loss, policy);
}

static int tcp_shift_cubic_controller_on_timeout(
    void *opaque_state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_cubic_model_on_timeout(
        (struct tcp_shift_cubic_model *)opaque_state, transport, policy);
}

const struct tcp_shift_cc_ops tcp_shift_cubic_ops = {
    .name = "cubic",
    .state_size = sizeof(struct tcp_shift_cubic_model),
    .init = tcp_shift_cubic_controller_init,
    .on_ack = tcp_shift_cubic_controller_on_ack,
    .on_loss = tcp_shift_cubic_controller_on_loss,
    .on_timeout = tcp_shift_cubic_controller_on_timeout,
};
