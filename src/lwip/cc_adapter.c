#include "lwip/cc_adapter.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TCP_SHIFT_LWIP_CC_MAX_LISTENERS 2U

struct tcp_shift_lwip_cc_listener_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    unsigned used;
};

static struct tcp_shift_lwip_cc_listener_binding
    tcp_shift_lwip_cc_listeners[TCP_SHIFT_LWIP_CC_MAX_LISTENERS];
static struct tcp_shift_lwip_cc_stats tcp_shift_lwip_cc_stats;

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

static void tcp_shift_lwip_cc_disable_on_error(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL) {
        return;
    }
    if (adapter->stats != NULL) {
        adapter->stats->controller_errors++;
    }
    /* Keep the ext-arg attached until PCB destruction so heap-owned adapter
     * storage is still released. bound=0 makes later hook calls fall back to
     * native lwIP congestion control instead of using stale controller state. */
    adapter->bound = 0U;
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
        tcp_shift_lwip_cc_disable_on_error(adapter);
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
        tcp_shift_lwip_cc_disable_on_error(adapter);
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
        tcp_shift_lwip_cc_disable_on_error(adapter);
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
    unsigned heap_owned;

    (void)id;
    if (hook == NULL) {
        return;
    }
    adapter = hook->arg;
    if (adapter == NULL) {
        return;
    }

    heap_owned = adapter->heap_owned;
    adapter->bound = 0U;
    adapter->pcb = NULL;
    if (heap_owned != 0U) {
        free(adapter);
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
        adapter->pcb != NULL ||
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
    if (adapter == NULL || adapter->pcb == NULL) {
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

static void tcp_shift_lwip_cc_listener_destroyed(u8_t id, void *data)
{
    struct tcp_shift_lwip_cc_listener_binding *binding = data;

    (void)id;
    if (binding != NULL) {
        memset(binding, 0, sizeof(*binding));
    }
}

static const struct tcp_ext_arg_callbacks tcp_shift_lwip_cc_listener_callbacks = {
    .destroy = tcp_shift_lwip_cc_listener_destroyed,
    .passive_open = NULL,
};

static err_t tcp_shift_lwip_cc_accept_dispatch(void *arg,
                                                struct tcp_pcb *newpcb,
                                                err_t err)
{
    struct tcp_shift_lwip_cc_listener_binding *binding = arg;
    struct tcp_shift_lwip_cc_adapter *adapter;

    if (binding == NULL || binding->used == 0U || binding->accept == NULL) {
        if (newpcb != NULL) {
            tcp_abort(newpcb);
            return ERR_ABRT;
        }
        return ERR_VAL;
    }

    if (newpcb == NULL || err != ERR_OK) {
        return binding->accept(binding->callback_arg, newpcb, err);
    }

    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        tcp_shift_lwip_cc_stats.bind_failures++;
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    if (tcp_shift_lwip_cc_adapter_bind(adapter, newpcb,
                                       &tcp_shift_lwip_cc_stats) < 0) {
        tcp_shift_lwip_cc_stats.bind_failures++;
        free(adapter);
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    adapter->heap_owned = 1U;

    return binding->accept(binding->callback_arg, newpcb, err);
}

void tcp_shift_lwip_cc_accept(struct tcp_pcb *pcb, tcp_accept_fn accept)
{
    struct tcp_shift_lwip_cc_listener_binding *binding = NULL;
    unsigned i;

    if (pcb == NULL || pcb->state != LISTEN) {
        tcp_accept(pcb, accept);
        return;
    }

    for (i = 0U; i < TCP_SHIFT_LWIP_CC_MAX_LISTENERS; i++) {
        if (tcp_shift_lwip_cc_listeners[i].used == 0U) {
            binding = &tcp_shift_lwip_cc_listeners[i];
            break;
        }
    }
    if (binding == NULL) {
        tcp_shift_lwip_cc_stats.bind_failures++;
        tcp_accept(pcb, NULL);
        return;
    }

    binding->listener = pcb;
    binding->accept = accept;
    binding->callback_arg = pcb->callback_arg;
    binding->used = 1U;

    tcp_arg(pcb, binding);
    tcp_ext_arg_set_callbacks(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                              &tcp_shift_lwip_cc_listener_callbacks);
    tcp_ext_arg_set(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID, binding);
    tcp_accept(pcb, tcp_shift_lwip_cc_accept_dispatch);
}

const struct tcp_shift_lwip_cc_stats *tcp_shift_lwip_cc_get_stats(void)
{
    return &tcp_shift_lwip_cc_stats;
}
