#include "lwip/cc_adapter.h"

#include <limits.h>
#include <string.h>

#define TCP_SHIFT_CC_SELECTOR_MAX_LISTENERS 2U

struct tcp_shift_cc_selector_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    const struct tcp_shift_cc_ops *ops;
    unsigned used;
};

static struct tcp_shift_cc_selector_binding
    tcp_shift_cc_selector_bindings[TCP_SHIFT_CC_SELECTOR_MAX_LISTENERS];
static const struct tcp_shift_cc_ops *tcp_shift_cc_selector_ops;

static uint32_t tcp_shift_cc_selector_cwnd_limit(void)
{
#if LWIP_WND_SCALE
    return UINT32_MAX;
#else
    return UINT16_MAX;
#endif
}

static const struct tcp_shift_cc_ops *tcp_shift_cc_selector_current(void)
{
    return tcp_shift_cc_selector_ops != NULL ? tcp_shift_cc_selector_ops
                                             : tcp_shift_cc_default_ops();
}

static struct tcp_shift_cc_selector_binding *
tcp_shift_cc_selector_find(struct tcp_pcb *listener)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_CC_SELECTOR_MAX_LISTENERS; i++) {
        if (tcp_shift_cc_selector_bindings[i].used != 0U &&
            tcp_shift_cc_selector_bindings[i].listener == listener) {
            return &tcp_shift_cc_selector_bindings[i];
        }
    }
    return NULL;
}

static struct tcp_shift_cc_selector_binding *tcp_shift_cc_selector_alloc(void)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_CC_SELECTOR_MAX_LISTENERS; i++) {
        if (tcp_shift_cc_selector_bindings[i].used == 0U) {
            return &tcp_shift_cc_selector_bindings[i];
        }
    }
    return NULL;
}

static int tcp_shift_cc_selector_has_listener(void)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_CC_SELECTOR_MAX_LISTENERS; i++) {
        if (tcp_shift_cc_selector_bindings[i].used != 0U) {
            return 1;
        }
    }
    return 0;
}

int tcp_shift_lwip_cc_configure_controller(const char *name)
{
    const struct tcp_shift_cc_ops *ops;

    if (tcp_shift_cc_selector_has_listener() != 0) {
        return -1;
    }
    if (name == NULL || name[0] == '\0') {
        ops = tcp_shift_cc_default_ops();
    } else {
        ops = tcp_shift_cc_find_ops(name);
    }
    if (ops == NULL) {
        return -1;
    }
    tcp_shift_cc_selector_ops = ops;
    return 0;
}

const char *tcp_shift_lwip_cc_configured_controller_name(void)
{
    const struct tcp_shift_cc_ops *ops = tcp_shift_cc_selector_current();

    return ops != NULL ? ops->name : NULL;
}

static int tcp_shift_cc_selector_reinit(struct tcp_shift_lwip_cc_adapter *adapter,
                                        struct tcp_pcb *pcb,
                                        const struct tcp_shift_cc_ops *ops)
{
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;
    uint32_t limit = tcp_shift_cc_selector_cwnd_limit();

    if (adapter == NULL || pcb == NULL || ops == NULL || adapter->bound == 0U ||
        adapter->pcb != pcb || adapter->controller.ops != &tcp_shift_reno_ops ||
        adapter->controller.state != &adapter->reno) {
        return -1;
    }
    if (ops == &tcp_shift_reno_ops) {
        return 0;
    }

    transport.mss_bytes = pcb->mss;
    transport.inflight_bytes = pcb->snd_nxt - pcb->lastack;
    transport.send_window_bytes = pcb->snd_wnd;
    transport.cwnd_limit_bytes = limit;

    init.initial_cwnd_bytes = pcb->cwnd;
    init.initial_ssthresh_bytes = pcb->ssthresh;
    init.min_cwnd_bytes = pcb->mss;

    memset(&adapter->controller_state, 0, sizeof(adapter->controller_state));
    if (tcp_shift_cc_init(&adapter->controller, ops,
                          &adapter->controller_state,
                          sizeof(adapter->controller_state), &transport,
                          &init, &policy) != 0 ||
        policy.cwnd_bytes == 0U || policy.ssthresh_bytes == 0U ||
        policy.cwnd_bytes > limit || policy.ssthresh_bytes > limit) {
        return -1;
    }

    adapter->pacing_rate_bytes_per_sec = policy.pacing_rate_bytes_per_sec;
    adapter->pacing_next_send_ns = 0U;
    pcb->cwnd = (tcpwnd_size_t)policy.cwnd_bytes;
    pcb->ssthresh = (tcpwnd_size_t)policy.ssthresh_bytes;
    pcb->bytes_acked = 0U;
    if (adapter->stats != NULL) {
        adapter->stats->last_cwnd_bytes = policy.cwnd_bytes;
        adapter->stats->last_ssthresh_bytes = policy.ssthresh_bytes;
        adapter->stats->pacing_last_rate_bytes_per_sec =
            policy.pacing_rate_bytes_per_sec;
    }
    return 0;
}

static err_t tcp_shift_cc_selector_accept(void *arg,
                                          struct tcp_pcb *newpcb,
                                          err_t err)
{
    struct tcp_shift_cc_selector_binding *binding = arg;
    struct tcp_shift_lwip_cc_hook *hook;
    struct tcp_shift_lwip_cc_adapter *adapter;

    if (binding == NULL || binding->used == 0U || binding->accept == NULL ||
        binding->ops == NULL) {
        if (newpcb != NULL) {
            tcp_abort(newpcb);
            return ERR_ABRT;
        }
        return ERR_VAL;
    }
    if (newpcb == NULL || err != ERR_OK) {
        return binding->accept(binding->callback_arg, newpcb, err);
    }

    hook = tcp_shift_lwip_cc_hook_get(newpcb);
    adapter = hook != NULL ? hook->arg : NULL;
    if (tcp_shift_cc_selector_reinit(adapter, newpcb, binding->ops) < 0) {
        if (adapter != NULL && adapter->stats != NULL) {
            adapter->stats->controller_errors++;
        }
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    return binding->accept(binding->callback_arg, newpcb, err);
}

void tcp_shift_lwip_cc_accept_selected(struct tcp_pcb *pcb,
                                       tcp_accept_fn accept)
{
    struct tcp_shift_cc_selector_binding *binding;

    if (pcb == NULL || pcb->state != LISTEN) {
        tcp_shift_lwip_cc_accept(pcb, accept);
        return;
    }

    binding = tcp_shift_cc_selector_find(pcb);
    if (accept == NULL) {
        if (binding != NULL) {
            memset(binding, 0, sizeof(*binding));
        }
        tcp_accept(pcb, NULL);
        return;
    }

    if (binding == NULL) {
        binding = tcp_shift_cc_selector_alloc();
    }
    if (binding == NULL) {
        tcp_shift_lwip_cc_accept(pcb, NULL);
        return;
    }

    memset(binding, 0, sizeof(*binding));
    binding->listener = pcb;
    binding->accept = accept;
    binding->callback_arg = pcb->callback_arg;
    binding->ops = tcp_shift_cc_selector_current();
    if (binding->ops == NULL) {
        memset(binding, 0, sizeof(*binding));
        tcp_shift_lwip_cc_accept(pcb, NULL);
        return;
    }
    binding->used = 1U;

    tcp_arg(pcb, binding);
    tcp_shift_lwip_cc_accept(pcb, tcp_shift_cc_selector_accept);
}
