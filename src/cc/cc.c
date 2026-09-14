#include "cc/cc.h"

static int tcp_shift_cc_valid_transport(
    const struct tcp_shift_cc_transport *transport)
{
    return transport != NULL && transport->mss_bytes != 0U &&
           transport->cwnd_limit_bytes >= transport->mss_bytes;
}

static int tcp_shift_cc_valid(struct tcp_shift_cc *cc,
                              struct tcp_shift_cc_policy *policy)
{
    return cc != NULL && cc->ops != NULL && cc->state != NULL &&
           policy != NULL;
}

int tcp_shift_cc_init(struct tcp_shift_cc *cc,
                      const struct tcp_shift_cc_ops *ops,
                      void *state,
                      size_t state_size,
                      const struct tcp_shift_cc_transport *transport,
                      const struct tcp_shift_cc_init *init,
                      struct tcp_shift_cc_policy *policy)
{
    int result;

    if (cc == NULL) {
        return -1;
    }

    /* A failed initialization must never leave a callable controller handle.
     * Commit ops/state only after the controller accepts its configuration. */
    cc->ops = NULL;
    cc->state = NULL;

    if (ops == NULL || state == NULL || init == NULL || policy == NULL ||
        !tcp_shift_cc_valid_transport(transport) || ops->name == NULL ||
        ops->state_size == 0U || state_size < ops->state_size ||
        ops->init == NULL || ops->on_ack == NULL || ops->on_loss == NULL ||
        ops->on_timeout == NULL) {
        return -1;
    }

    policy->cwnd_bytes = 0U;
    policy->ssthresh_bytes = 0U;
    policy->pacing_rate_bytes_per_sec = 0U;
    result = ops->init(state, transport, init, policy);
    if (result != 0) {
        return result;
    }

    cc->ops = ops;
    cc->state = state;
    return 0;
}

int tcp_shift_cc_on_ack(struct tcp_shift_cc *cc,
                        const struct tcp_shift_cc_transport *transport,
                        const struct tcp_shift_cc_ack *ack,
                        struct tcp_shift_cc_policy *policy)
{
    if (!tcp_shift_cc_valid(cc, policy) ||
        !tcp_shift_cc_valid_transport(transport) || ack == NULL ||
        ack->acked_bytes == 0U) {
        return -1;
    }
    return cc->ops->on_ack(cc->state, transport, ack, policy);
}

int tcp_shift_cc_on_loss(struct tcp_shift_cc *cc,
                         const struct tcp_shift_cc_transport *transport,
                         const struct tcp_shift_cc_loss *loss,
                         struct tcp_shift_cc_policy *policy)
{
    if (!tcp_shift_cc_valid(cc, policy) ||
        !tcp_shift_cc_valid_transport(transport) || loss == NULL ||
        loss->lost_bytes == 0U) {
        return -1;
    }
    return cc->ops->on_loss(cc->state, transport, loss, policy);
}

int tcp_shift_cc_on_timeout(struct tcp_shift_cc *cc,
                            const struct tcp_shift_cc_transport *transport,
                            struct tcp_shift_cc_policy *policy)
{
    if (!tcp_shift_cc_valid(cc, policy) ||
        !tcp_shift_cc_valid_transport(transport)) {
        return -1;
    }
    return cc->ops->on_timeout(cc->state, transport, policy);
}
