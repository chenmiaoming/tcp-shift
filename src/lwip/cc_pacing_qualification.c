#include "lwip/cc_adapter.h"

#include <string.h>

#define TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS 2U

struct tcp_shift_pacing_qualification_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    unsigned used;
};

static struct tcp_shift_pacing_qualification_binding
    tcp_shift_pacing_qualification_bindings[
        TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS];

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
    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != newpcb ||
        adapter->controller.ops != &tcp_shift_reno_ops ||
        adapter->controller.state != &adapter->reno) {
        if (adapter != NULL && adapter->stats != NULL) {
            adapter->stats->controller_errors++;
        }
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    /* State shape and Reno transitions are identical. The next real ACK/loss/
     * RTO policy event publishes the deterministic nonzero pacing rate through
     * the generic controller output, after which the normal adapter scheduler
     * path owns all pacing mechanics. */
    adapter->controller.ops = &tcp_shift_fixed_pacing_reno_ops;
    return binding->accept(binding->callback_arg, newpcb, err);
}

void tcp_shift_lwip_cc_accept_fixed_pacing(struct tcp_pcb *pcb,
                                           tcp_accept_fn accept)
{
    struct tcp_shift_pacing_qualification_binding *binding;

    if (pcb == NULL || pcb->state != LISTEN) {
        tcp_shift_lwip_cc_accept(pcb, accept);
        return;
    }

    binding = tcp_shift_pacing_qualification_find(pcb);
    if (accept == NULL) {
        if (binding != NULL) {
            memset(binding, 0, sizeof(*binding));
        }
        /* This source is not compiled with the bridge's tcp_accept macro, so
         * this is the native lwIP unregister call. The original CC listener
         * ext-arg remains attached until pcb destruction and cleans itself. */
        tcp_accept(pcb, NULL);
        return;
    }

    if (binding == NULL) {
        binding = tcp_shift_pacing_qualification_alloc();
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

    /* Make our binding the callback arg captured by the normal CC wrapper. The
     * wrapper still owns controller allocation/ext-arg lifetime; our dispatch
     * only changes which already-compatible pure-C ops table the child uses. */
    tcp_arg(pcb, binding);
    tcp_shift_lwip_cc_accept(pcb, tcp_shift_pacing_qualification_accept);
}
