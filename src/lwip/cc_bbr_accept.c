#include "lwip/cc_adapter.h"

#include <string.h>

#define TCP_SHIFT_LWIP_BBR_MAX_LISTENERS 2U

struct tcp_shift_lwip_bbr_listener_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    unsigned used;
};

static struct tcp_shift_lwip_bbr_listener_binding
    tcp_shift_lwip_bbr_listeners[TCP_SHIFT_LWIP_BBR_MAX_LISTENERS];

static struct tcp_shift_lwip_bbr_listener_binding *
tcp_shift_lwip_bbr_find_listener(struct tcp_pcb *listener)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_LWIP_BBR_MAX_LISTENERS; i++) {
        if (tcp_shift_lwip_bbr_listeners[i].used != 0U &&
            tcp_shift_lwip_bbr_listeners[i].listener == listener) {
            return &tcp_shift_lwip_bbr_listeners[i];
        }
    }
    return NULL;
}

static struct tcp_shift_lwip_bbr_listener_binding *
tcp_shift_lwip_bbr_alloc_listener(void)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_LWIP_BBR_MAX_LISTENERS; i++) {
        if (tcp_shift_lwip_bbr_listeners[i].used == 0U) {
            return &tcp_shift_lwip_bbr_listeners[i];
        }
    }
    return NULL;
}

static err_t tcp_shift_lwip_bbr_accept_dispatch(void *arg,
                                                 struct tcp_pcb *newpcb,
                                                 err_t err)
{
    struct tcp_shift_lwip_bbr_listener_binding *binding = arg;
    struct tcp_shift_lwip_cc_hook *hook;
    struct tcp_shift_lwip_cc_adapter *adapter;
    uint32_t cycle_seed;

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

    /* The ordinary listener wrapper has already installed Reno delivery
     * sampling and registered this flow with the shared pacer. */
    hook = tcp_shift_lwip_cc_hook_get(newpcb);
    adapter = hook != NULL ? hook->arg : NULL;
    if (adapter == NULL || adapter->pacing_flow_id == 0U) {
        if (adapter != NULL && adapter->stats != NULL) {
            adapter->stats->controller_errors++;
        }
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    cycle_seed = (uint32_t)adapter->pacing_flow_id ^
                 (uint32_t)(adapter->pacing_flow_id >> 32);
    if (tcp_shift_lwip_cc_apply_internal_bbr(adapter, cycle_seed) < 0) {
        if (adapter->stats != NULL) {
            adapter->stats->controller_errors++;
        }
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    return binding->accept(binding->callback_arg, newpcb, err);
}

void tcp_shift_lwip_cc_accept_internal_bbr(struct tcp_pcb *pcb,
                                           tcp_accept_fn accept)
{
    struct tcp_shift_lwip_bbr_listener_binding *binding;

    if (pcb == NULL || pcb->state != LISTEN) {
        tcp_shift_lwip_cc_accept(pcb, accept);
        return;
    }

    binding = tcp_shift_lwip_bbr_find_listener(pcb);
    if (accept == NULL) {
        if (binding != NULL) {
            memset(binding, 0, sizeof(*binding));
        }
        tcp_shift_lwip_cc_accept(pcb, NULL);
        return;
    }

    if (binding == NULL) {
        binding = tcp_shift_lwip_bbr_alloc_listener();
    }
    if (binding == NULL) {
        tcp_shift_lwip_cc_accept(pcb, NULL);
        return;
    }

    memset(binding, 0, sizeof(*binding));
    binding->listener = pcb;
    binding->accept = accept;
    binding->callback_arg = pcb->callback_arg;
    binding->used = 1U;

    /* tcp_shift_lwip_cc_accept() captures this wrapper binding as its user
     * callback argument, then performs the normal adapter bind before dispatch. */
    tcp_arg(pcb, binding);
    tcp_shift_lwip_cc_accept(pcb, tcp_shift_lwip_bbr_accept_dispatch);
}
