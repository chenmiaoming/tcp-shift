#include "lwip/cc_adapter.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static uint32_t tcp_shift_lwip_cc_cwnd_limit(void)
{
#if LWIP_WND_SCALE
    return UINT32_MAX;
#else
    return UINT16_MAX;
#endif
}

static void tcp_shift_lwip_cc_transport_from_pcb(
    const struct tcp_pcb *pcb,
    struct tcp_shift_cc_transport *transport)
{
    transport->mss_bytes = pcb->mss;
    transport->inflight_bytes = pcb->snd_nxt - pcb->lastack;
    transport->send_window_bytes = pcb->snd_wnd;
    transport->cwnd_limit_bytes = tcp_shift_lwip_cc_cwnd_limit();
}

static int tcp_shift_lwip_cc_apply_policy(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_policy *policy)
{
    uint32_t limit = tcp_shift_lwip_cc_cwnd_limit();

    if (adapter == NULL || adapter->pcb == NULL || policy == NULL ||
        policy->cwnd_bytes == 0U || policy->ssthresh_bytes == 0U ||
        policy->cwnd_bytes > limit || policy->ssthresh_bytes > limit) {
        return -1;
    }

    adapter->pcb->cwnd = (tcpwnd_size_t)policy->cwnd_bytes;
    adapter->pcb->ssthresh = (tcpwnd_size_t)policy->ssthresh_bytes;
    adapter->pcb->bytes_acked = 0U;
    if (adapter->stats != NULL) {
        adapter->stats->last_cwnd_bytes = policy->cwnd_bytes;
        adapter->stats->last_ssthresh_bytes = policy->ssthresh_bytes;
    }
    return 0;
}

static void tcp_shift_lwip_cc_detach_on_error(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL || adapter->bound == 0U || adapter->pcb == NULL) {
        return;
    }
    if (tcp_ext_arg_get(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID) ==
        &adapter->hook) {
        tcp_ext_arg_set_callbacks(adapter->pcb,
                                  (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID, NULL);
        tcp_ext_arg_set(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                        NULL);
    }
    adapter->bound = 0U;
    adapter->pcb = NULL;
}

static int tcp_shift_lwip_cc_on_ack(void *arg,
                                    struct tcp_pcb *pcb,
                                    tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        acked_bytes == 0U) {
        return 0;
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    ack.acked_bytes = acked_bytes;
    if (tcp_shift_cc_on_ack(&adapter->controller, &transport, &ack,
                            &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        tcp_shift_lwip_cc_detach_on_error(adapter);
        return 0;
    }

    if (adapter->stats != NULL) {
        adapter->stats->ack_events++;
        adapter->stats->policy_updates++;
    }
    return 1;
}

static int tcp_shift_lwip_cc_on_loss(void *arg,
                                     struct tcp_pcb *pcb,
                                     tcpwnd_size_t lost_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_loss loss;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        lost_bytes == 0U) {
        return 0;
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    loss.lost_bytes = lost_bytes;
    if (tcp_shift_cc_on_loss(&adapter->controller, &transport, &loss,
                             &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        tcp_shift_lwip_cc_detach_on_error(adapter);
        return 0;
    }

    if (adapter->stats != NULL) {
        adapter->stats->loss_events++;
        adapter->stats->policy_updates++;
    }
    return 1;
}

static int tcp_shift_lwip_cc_on_timeout(void *arg, struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb) {
        return 0;
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    if (tcp_shift_cc_on_timeout(&adapter->controller, &transport, &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        tcp_shift_lwip_cc_detach_on_error(adapter);
        return 0;
    }

    if (adapter->stats != NULL) {
        adapter->stats->timeout_events++;
        adapter->stats->policy_updates++;
    }
    return 1;
}

static const struct tcp_shift_lwip_cc_hook_ops tcp_shift_lwip_cc_hook_ops = {
    .on_ack = tcp_shift_lwip_cc_on_ack,
    .on_loss = tcp_shift_lwip_cc_on_loss,
    .on_timeout = tcp_shift_lwip_cc_on_timeout,
};

static void tcp_shift_lwip_cc_pcb_destroyed(u8_t id, void *data)
{
    struct tcp_shift_lwip_cc_hook *hook = data;
    struct tcp_shift_lwip_cc_adapter *adapter;

    (void)id;
    if (hook == NULL) {
        return;
    }
    adapter = hook->arg;
    if (adapter != NULL) {
        adapter->bound = 0U;
        adapter->pcb = NULL;
    }
}

static const struct tcp_ext_arg_callbacks tcp_shift_lwip_cc_ext_callbacks = {
    .destroy = tcp_shift_lwip_cc_pcb_destroyed,
    .passive_open = NULL,
};

int tcp_shift_lwip_cc_adapter_bind(struct tcp_shift_lwip_cc_adapter *adapter,
                                   struct tcp_pcb *pcb,
                                   struct tcp_shift_lwip_cc_stats *stats)
{
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || pcb == NULL || pcb->mss == 0U ||
        adapter->bound != 0U ||
        tcp_ext_arg_get(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID) != NULL) {
        return -1;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->hook.ops = &tcp_shift_lwip_cc_hook_ops;
    adapter->hook.arg = adapter;
    adapter->stats = stats;
    adapter->pcb = pcb;

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    init.initial_cwnd_bytes = pcb->cwnd;
    init.initial_ssthresh_bytes = pcb->ssthresh;
    init.min_cwnd_bytes = pcb->mss;
    if (tcp_shift_cc_init(&adapter->controller, &tcp_shift_reno_ops,
                          &adapter->reno, sizeof(adapter->reno), &transport,
                          &init, &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        adapter->pcb = NULL;
        adapter->stats = NULL;
        return -1;
    }

    tcp_ext_arg_set_callbacks(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                              &tcp_shift_lwip_cc_ext_callbacks);
    tcp_ext_arg_set(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID, &adapter->hook);
    adapter->bound = 1U;
    if (stats != NULL) {
        stats->bindings++;
    }
    return 0;
}

void tcp_shift_lwip_cc_adapter_unbind(struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL || adapter->bound == 0U || adapter->pcb == NULL) {
        return;
    }

    if (tcp_ext_arg_get(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID) ==
        &adapter->hook) {
        tcp_ext_arg_set_callbacks(adapter->pcb,
                                  (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID, NULL);
        tcp_ext_arg_set(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                        NULL);
    }
    adapter->bound = 0U;
    adapter->pcb = NULL;
}
