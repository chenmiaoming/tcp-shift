#include "lwip/cc_adapter.h"
#include "cc/transport_pacing.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS 2U

struct tcp_shift_pacing_qualification_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    unsigned linux_mode;
    unsigned used;
};

static struct tcp_shift_pacing_qualification_binding
    tcp_shift_pacing_qualification_bindings[
        TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS];

static uint32_t tcp_shift_pacing_qualification_cwnd_limit(void)
{
#if LWIP_WND_SCALE
    return UINT32_MAX;
#else
    return UINT16_MAX;
#endif
}

static int tcp_shift_pacing_qualification_linux_mode(void)
{
    const char *mode = getenv("TCP_SHIFT_PACING_QUALIFICATION");

    return mode != NULL &&
           (strcmp(mode, "linux") == 0 || strcmp(mode, "linux-cap") == 0);
}

static int tcp_shift_pacing_qualification_linux_rate_cap(uint64_t *rate_cap)
{
    const char *mode = getenv("TCP_SHIFT_PACING_QUALIFICATION");
    const char *value;
    const char *cursor;
    uint64_t parsed = 0U;

    if (rate_cap == NULL) {
        return -1;
    }
    *rate_cap = 0U;

    if (mode == NULL || strcmp(mode, "linux-cap") != 0) {
        return 0;
    }

    value = getenv("TCP_SHIFT_PACING_QUALIFICATION_MAX_BYTES_PER_SEC");
    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    for (cursor = value; *cursor != '\0'; cursor++) {
        uint64_t digit;

        if (*cursor < '0' || *cursor > '9') {
            return -1;
        }
        digit = (uint64_t)(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10U) {
            return -1;
        }
        parsed = parsed * 10U + digit;
    }

    if (parsed == 0U) {
        return -1;
    }
    *rate_cap = parsed;
    return 0;
}

static int tcp_shift_pacing_qualification_publish_linux_rate(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t rate_cap;

    if (adapter == NULL || transport == NULL || policy == NULL ||
        tcp_shift_pacing_qualification_linux_rate_cap(&rate_cap) < 0) {
        return -1;
    }

    /* Keep benchmark qualification on the same transport policy primitive
     * that production Reno/CUBIC will eventually use. A controller-owned
     * nonzero rate (BBR) has strict precedence. linux-cap remains a benchmark-
     * only ceiling on the fallback so known path knowledge never leaks into a
     * controller-owned rate or ordinary transport operation. */
    return tcp_shift_transport_pacing_apply_window_fallback(
        transport, adapter->srtt.smoothed_rtt_ns, rate_cap, policy);
}

static int tcp_shift_pacing_qualification_inner_init(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_ops *inner,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    struct tcp_shift_cc_policy *policy)
{
    int result;

    if (adapter == NULL || inner == NULL || transport == NULL || init == NULL ||
        policy == NULL || inner->state_size > sizeof(adapter->controller_state)) {
        return -1;
    }

    memset(&adapter->controller_state, 0, sizeof(adapter->controller_state));
    result = inner->init(&adapter->controller_state, transport, init, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_pacing_qualification_publish_linux_rate(adapter, transport,
                                                              policy);
}

static int tcp_shift_pacing_qualification_inner_ack(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_ops *inner,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    int result;

    if (adapter == NULL || inner == NULL || transport == NULL || ack == NULL ||
        policy == NULL) {
        return -1;
    }
    result = inner->on_ack(&adapter->controller_state, transport, ack, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_pacing_qualification_publish_linux_rate(adapter, transport,
                                                              policy);
}

static int tcp_shift_pacing_qualification_inner_loss(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_ops *inner,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    int result;

    if (adapter == NULL || inner == NULL || transport == NULL || loss == NULL ||
        policy == NULL) {
        return -1;
    }
    result = inner->on_loss(&adapter->controller_state, transport, loss, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_pacing_qualification_publish_linux_rate(adapter, transport,
                                                              policy);
}

static int tcp_shift_pacing_qualification_inner_timeout(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_ops *inner,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    int result;

    if (adapter == NULL || inner == NULL || transport == NULL || policy == NULL) {
        return -1;
    }
    result = inner->on_timeout(&adapter->controller_state, transport, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_pacing_qualification_publish_linux_rate(adapter, transport,
                                                              policy);
}

static int tcp_shift_pacing_qualification_reno_init(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_init(state, &tcp_shift_reno_ops,
                                                      transport, init, policy);
}

static int tcp_shift_pacing_qualification_reno_ack(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_ack(state, &tcp_shift_reno_ops,
                                                     transport, ack, policy);
}

static int tcp_shift_pacing_qualification_reno_loss(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_loss(state, &tcp_shift_reno_ops,
                                                      transport, loss, policy);
}

static int tcp_shift_pacing_qualification_reno_timeout(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_timeout(
        state, &tcp_shift_reno_ops, transport, policy);
}

static int tcp_shift_pacing_qualification_cubic_init(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_init(state, &tcp_shift_cubic_ops,
                                                      transport, init, policy);
}

static int tcp_shift_pacing_qualification_cubic_ack(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_ack(state, &tcp_shift_cubic_ops,
                                                     transport, ack, policy);
}

static int tcp_shift_pacing_qualification_cubic_loss(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_loss(state, &tcp_shift_cubic_ops,
                                                      transport, loss, policy);
}

static int tcp_shift_pacing_qualification_cubic_timeout(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    return tcp_shift_pacing_qualification_inner_timeout(
        state, &tcp_shift_cubic_ops, transport, policy);
}

static const struct tcp_shift_cc_ops tcp_shift_linux_paced_reno_qualification_ops = {
    .name = "reno-linux-pacing-qualification",
    .state_size = sizeof(struct tcp_shift_lwip_cc_adapter),
    .init = tcp_shift_pacing_qualification_reno_init,
    .on_ack = tcp_shift_pacing_qualification_reno_ack,
    .on_loss = tcp_shift_pacing_qualification_reno_loss,
    .on_timeout = tcp_shift_pacing_qualification_reno_timeout,
};

static const struct tcp_shift_cc_ops tcp_shift_linux_paced_cubic_qualification_ops = {
    .name = "cubic-linux-pacing-qualification",
    .state_size = sizeof(struct tcp_shift_lwip_cc_adapter),
    .init = tcp_shift_pacing_qualification_cubic_init,
    .on_ack = tcp_shift_pacing_qualification_cubic_ack,
    .on_loss = tcp_shift_pacing_qualification_cubic_loss,
    .on_timeout = tcp_shift_pacing_qualification_cubic_timeout,
};

static struct tcp_shift_pacing_qualification_binding *
tcp_shift_pacing_qualification_find(struct tcp_pcb *listener)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS; i++) {
        if (tcp_shift_pacing_qualification_bindings[i].used != 0U &&
            tcp_shift_pacing_qualification_bindings[i].listener == listener) {
            return &tcp_shift_pacing_qualification_bindings[i];
        }
    }
    return NULL;
}

static struct tcp_shift_pacing_qualification_binding *
tcp_shift_pacing_qualification_alloc(void)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS; i++) {
        if (tcp_shift_pacing_qualification_bindings[i].used == 0U) {
            return &tcp_shift_pacing_qualification_bindings[i];
        }
    }
    return NULL;
}

static int tcp_shift_pacing_qualification_reinit_linux(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb)
{
    const struct tcp_shift_cc_ops *wrapper;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;
    uint32_t limit = tcp_shift_pacing_qualification_cwnd_limit();

    if (adapter == NULL || pcb == NULL || adapter->bound == 0U ||
        adapter->pcb != pcb || adapter->controller.ops == NULL) {
        return -1;
    }

    if (adapter->controller.ops == &tcp_shift_reno_ops) {
        wrapper = &tcp_shift_linux_paced_reno_qualification_ops;
    } else if (adapter->controller.ops == &tcp_shift_cubic_ops) {
        wrapper = &tcp_shift_linux_paced_cubic_qualification_ops;
    } else {
        return -1;
    }

    transport.mss_bytes = pcb->mss;
    transport.inflight_bytes = pcb->snd_nxt - pcb->lastack;
    transport.send_window_bytes = pcb->snd_wnd;
    transport.cwnd_limit_bytes = limit;

    init.initial_cwnd_bytes = pcb->cwnd;
    init.initial_ssthresh_bytes = pcb->ssthresh;
    init.min_cwnd_bytes = pcb->mss;

    if (tcp_shift_cc_init(&adapter->controller, wrapper, adapter,
                          sizeof(*adapter), &transport, &init, &policy) != 0 ||
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

static err_t tcp_shift_pacing_qualification_accept(void *arg,
                                                    struct tcp_pcb *newpcb,
                                                    err_t err)
{
    struct tcp_shift_pacing_qualification_binding *binding = arg;
    struct tcp_shift_lwip_cc_hook *hook;
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

    hook = tcp_shift_lwip_cc_hook_get(newpcb);
    adapter = hook != NULL ? hook->arg : NULL;

    if (binding->linux_mode != 0U) {
        if (tcp_shift_pacing_qualification_reinit_linux(adapter, newpcb) < 0) {
            if (adapter != NULL && adapter->stats != NULL) {
                adapter->stats->controller_errors++;
            }
            tcp_abort(newpcb);
            return ERR_ABRT;
        }
        return binding->accept(binding->callback_arg, newpcb, err);
    }

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != newpcb ||
        adapter->controller.ops != &tcp_shift_reno_ops ||
        adapter->controller.state != &adapter->reno) {
        if (adapter != NULL && adapter->stats != NULL) {
            adapter->stats->controller_errors++;
        }
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    /* Legacy P5c mode remains deterministic and Reno-only. State shape and
     * Reno transitions are identical; only the ops table publishes a fixed
     * nonzero pacing rate for scheduler qualification. */
    adapter->controller.ops = &tcp_shift_fixed_pacing_reno_ops;
    return binding->accept(binding->callback_arg, newpcb, err);
}

void tcp_shift_lwip_cc_accept_fixed_pacing(struct tcp_pcb *pcb,
                                           tcp_accept_fn accept)
{
    struct tcp_shift_pacing_qualification_binding *binding;
    unsigned linux_mode;

    if (pcb == NULL || pcb->state != LISTEN) {
        if (tcp_shift_pacing_qualification_linux_mode() != 0) {
            tcp_shift_lwip_cc_accept_selected(pcb, accept);
        } else {
            tcp_shift_lwip_cc_accept(pcb, accept);
        }
        return;
    }

    binding = tcp_shift_pacing_qualification_find(pcb);
    if (accept == NULL) {
        linux_mode = binding != NULL ? binding->linux_mode : 0U;
        if (binding != NULL) {
            memset(binding, 0, sizeof(*binding));
        }
        if (linux_mode != 0U) {
            tcp_shift_lwip_cc_accept_selected(pcb, NULL);
        } else {
            /* This source is not compiled with the bridge's tcp_accept macro,
             * so this is the native lwIP unregister call. */
            tcp_accept(pcb, NULL);
        }
        return;
    }

    if (binding == NULL) {
        binding = tcp_shift_pacing_qualification_alloc();
    }
    if (binding == NULL) {
        if (tcp_shift_pacing_qualification_linux_mode() != 0) {
            tcp_shift_lwip_cc_accept_selected(pcb, NULL);
        } else {
            tcp_shift_lwip_cc_accept(pcb, NULL);
        }
        return;
    }

    linux_mode = (unsigned)tcp_shift_pacing_qualification_linux_mode();
    memset(binding, 0, sizeof(*binding));
    binding->listener = pcb;
    binding->accept = accept;
    binding->callback_arg = pcb->callback_arg;
    binding->linux_mode = linux_mode;
    binding->used = 1U;

    tcp_arg(pcb, binding);
    if (linux_mode != 0U) {
        /* Let the normal selector bind Reno/CUBIC first. The child callback
         * then replaces only the controller policy surface with qualification
         * transport pacing; allocation/lifetime remains owned by the adapter. */
        tcp_shift_lwip_cc_accept_selected(pcb,
                                          tcp_shift_pacing_qualification_accept);
    } else {
        tcp_shift_lwip_cc_accept(pcb, tcp_shift_pacing_qualification_accept);
    }
}
