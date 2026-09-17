#include "lwip/cc_adapter.h"
#include "cc/transport_pacing.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS 2U
#define TCP_SHIFT_PACING_QUALIFICATION_MAX_QUANTUM_FLOWS 64U

struct tcp_shift_pacing_qualification_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    unsigned linux_mode;
    unsigned quantum_mode;
    unsigned used;
};

/* Qualification-only sidecar used to A/B Linux-shaped batching without
 * changing the production adapter state layout yet. The base adapter remains
 * the owner of rate, virtual deadline, scheduler lifetime and TX accounting;
 * this sidecar owns only the bounded allowance between two deadline checks. */
struct tcp_shift_pacing_qualification_quantum_flow {
    struct tcp_shift_lwip_cc_adapter *adapter;
    uint64_t rate_bytes_per_sec;
    uint64_t grants;
    uint32_t quantum_bytes;
    uint32_t remaining_bytes;
};

static struct tcp_shift_pacing_qualification_binding
    tcp_shift_pacing_qualification_bindings[
        TCP_SHIFT_PACING_QUALIFICATION_MAX_LISTENERS];
static struct tcp_shift_pacing_qualification_quantum_flow
    tcp_shift_pacing_qualification_quantum_flows[
        TCP_SHIFT_PACING_QUALIFICATION_MAX_QUANTUM_FLOWS];
static const struct tcp_shift_lwip_cc_hook_ops *
    tcp_shift_pacing_qualification_base_hook_ops;

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
           (strcmp(mode, "linux") == 0 || strcmp(mode, "linux-cap") == 0 ||
            strcmp(mode, "linux-quantum") == 0 ||
            strcmp(mode, "linux-cap-quantum") == 0);
}

static int tcp_shift_pacing_qualification_quantum_mode(void)
{
    const char *mode = getenv("TCP_SHIFT_PACING_QUALIFICATION");

    return mode != NULL &&
           (strcmp(mode, "linux-quantum") == 0 ||
            strcmp(mode, "linux-cap-quantum") == 0);
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

    if (mode == NULL ||
        (strcmp(mode, "linux-cap") != 0 &&
         strcmp(mode, "linux-cap-quantum") != 0)) {
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

static struct tcp_shift_pacing_qualification_quantum_flow *
tcp_shift_pacing_qualification_quantum_find(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    unsigned i;

    for (i = 0U; i < TCP_SHIFT_PACING_QUALIFICATION_MAX_QUANTUM_FLOWS; i++) {
        if (tcp_shift_pacing_qualification_quantum_flows[i].adapter == adapter) {
            return &tcp_shift_pacing_qualification_quantum_flows[i];
        }
    }
    return NULL;
}

static struct tcp_shift_pacing_qualification_quantum_flow *
tcp_shift_pacing_qualification_quantum_alloc(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    struct tcp_shift_pacing_qualification_quantum_flow *flow;
    unsigned i;

    flow = tcp_shift_pacing_qualification_quantum_find(adapter);
    if (flow != NULL) {
        memset(flow, 0, sizeof(*flow));
        flow->adapter = adapter;
        return flow;
    }

    for (i = 0U; i < TCP_SHIFT_PACING_QUALIFICATION_MAX_QUANTUM_FLOWS; i++) {
        if (tcp_shift_pacing_qualification_quantum_flows[i].adapter == NULL) {
            flow = &tcp_shift_pacing_qualification_quantum_flows[i];
            memset(flow, 0, sizeof(*flow));
            flow->adapter = adapter;
            return flow;
        }
    }
    return NULL;
}

static void tcp_shift_pacing_qualification_quantum_refresh(
    struct tcp_shift_pacing_qualification_quantum_flow *flow)
{
    uint64_t rate;
    uint32_t quantum;
    uint32_t mss;

    if (flow == NULL || flow->adapter == NULL) {
        return;
    }

    rate = flow->adapter->pacing_rate_bytes_per_sec;
    mss = flow->adapter->pcb != NULL ? flow->adapter->pcb->mss : 0U;
    quantum = tcp_shift_transport_pacing_quantum_bytes(rate, mss);
    if (flow->rate_bytes_per_sec != rate || flow->quantum_bytes != quantum) {
        /* Rate/mode transitions must not inherit batch credit from a previous
         * pacing policy. The adapter's absolute next_send deadline is not
         * touched here, matching the shared flow-pacer lifecycle contract. */
        flow->remaining_bytes = 0U;
    }
    flow->rate_bytes_per_sec = rate;
    flow->quantum_bytes = quantum;
}

static int tcp_shift_pacing_qualification_quantum_on_ack(
    void *arg, struct tcp_pcb *pcb, tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_pacing_qualification_quantum_flow *flow;
    int result = 0;

    if (tcp_shift_pacing_qualification_base_hook_ops != NULL &&
        tcp_shift_pacing_qualification_base_hook_ops->on_ack != NULL) {
        result = tcp_shift_pacing_qualification_base_hook_ops->on_ack(
            arg, pcb, acked_bytes);
    }
    flow = tcp_shift_pacing_qualification_quantum_find(adapter);
    tcp_shift_pacing_qualification_quantum_refresh(flow);
    return result;
}

static int tcp_shift_pacing_qualification_quantum_on_loss(
    void *arg, struct tcp_pcb *pcb, tcpwnd_size_t lost_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_pacing_qualification_quantum_flow *flow;
    int result = 0;

    if (tcp_shift_pacing_qualification_base_hook_ops != NULL &&
        tcp_shift_pacing_qualification_base_hook_ops->on_loss != NULL) {
        result = tcp_shift_pacing_qualification_base_hook_ops->on_loss(
            arg, pcb, lost_bytes);
    }
    flow = tcp_shift_pacing_qualification_quantum_find(adapter);
    tcp_shift_pacing_qualification_quantum_refresh(flow);
    return result;
}

static int tcp_shift_pacing_qualification_quantum_on_timeout(
    void *arg, struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_pacing_qualification_quantum_flow *flow;
    int result = 0;

    if (tcp_shift_pacing_qualification_base_hook_ops != NULL &&
        tcp_shift_pacing_qualification_base_hook_ops->on_timeout != NULL) {
        result = tcp_shift_pacing_qualification_base_hook_ops->on_timeout(arg,
                                                                           pcb);
    }
    flow = tcp_shift_pacing_qualification_quantum_find(adapter);
    tcp_shift_pacing_qualification_quantum_refresh(flow);
    return result;
}

static int tcp_shift_pacing_qualification_quantum_send_eligible(
    void *arg, struct tcp_pcb *pcb, u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_pacing_qualification_quantum_flow *flow;
    uint32_t allowance;
    int eligible;

    flow = tcp_shift_pacing_qualification_quantum_find(adapter);
    if (flow == NULL || payload_bytes == 0U ||
        tcp_shift_pacing_qualification_base_hook_ops == NULL ||
        tcp_shift_pacing_qualification_base_hook_ops
                ->on_segment_send_eligible == NULL) {
        return tcp_shift_pacing_qualification_base_hook_ops != NULL &&
                       tcp_shift_pacing_qualification_base_hook_ops
                               ->on_segment_send_eligible != NULL
                   ? tcp_shift_pacing_qualification_base_hook_ops
                         ->on_segment_send_eligible(arg, pcb, payload_bytes)
                   : 1;
    }

    tcp_shift_pacing_qualification_quantum_refresh(flow);
    if (flow->remaining_bytes >= payload_bytes) {
        flow->remaining_bytes -= payload_bytes;
        return 1;
    }
    /* Do not let a sub-MSS tail become free credit for the next full segment. */
    flow->remaining_bytes = 0U;

    eligible = tcp_shift_pacing_qualification_base_hook_ops
                   ->on_segment_send_eligible(arg, pcb, payload_bytes);
    if (eligible == 0) {
        return 0;
    }

    allowance = flow->quantum_bytes;
    if (allowance < payload_bytes) {
        allowance = payload_bytes;
    }
    flow->remaining_bytes = allowance - payload_bytes;
    flow->grants++;
    return 1;
}

static void tcp_shift_pacing_qualification_quantum_on_segment_tx(
    void *arg,
    struct tcp_pcb *pcb,
    const void *segment,
    u32_t seq_start,
    u16_t payload_bytes)
{
    if (tcp_shift_pacing_qualification_base_hook_ops != NULL &&
        tcp_shift_pacing_qualification_base_hook_ops->on_segment_tx != NULL) {
        tcp_shift_pacing_qualification_base_hook_ops->on_segment_tx(
            arg, pcb, segment, seq_start, payload_bytes);
    }
}

static void tcp_shift_pacing_qualification_quantum_on_segment_acked(
    void *arg,
    struct tcp_pcb *pcb,
    const void *segment,
    u16_t payload_bytes)
{
    if (tcp_shift_pacing_qualification_base_hook_ops != NULL &&
        tcp_shift_pacing_qualification_base_hook_ops->on_segment_acked != NULL) {
        tcp_shift_pacing_qualification_base_hook_ops->on_segment_acked(
            arg, pcb, segment, payload_bytes);
    }
}

static const struct tcp_shift_lwip_cc_hook_ops
    tcp_shift_pacing_qualification_quantum_hook_ops = {
        .on_ack = tcp_shift_pacing_qualification_quantum_on_ack,
        .on_loss = tcp_shift_pacing_qualification_quantum_on_loss,
        .on_timeout = tcp_shift_pacing_qualification_quantum_on_timeout,
        .on_segment_send_eligible =
            tcp_shift_pacing_qualification_quantum_send_eligible,
        .on_segment_tx = tcp_shift_pacing_qualification_quantum_on_segment_tx,
        .on_segment_acked =
            tcp_shift_pacing_qualification_quantum_on_segment_acked,
};

static int tcp_shift_pacing_qualification_quantum_attach(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    struct tcp_shift_pacing_qualification_quantum_flow *flow;

    if (adapter == NULL || adapter->hook.ops == NULL || adapter->pcb == NULL) {
        return -1;
    }
    if (tcp_shift_pacing_qualification_base_hook_ops == NULL) {
        tcp_shift_pacing_qualification_base_hook_ops = adapter->hook.ops;
    } else if (tcp_shift_pacing_qualification_base_hook_ops !=
               adapter->hook.ops) {
        return -1;
    }

    flow = tcp_shift_pacing_qualification_quantum_alloc(adapter);
    if (flow == NULL) {
        return -1;
    }
    tcp_shift_pacing_qualification_quantum_refresh(flow);
    adapter->hook.ops = &tcp_shift_pacing_qualification_quantum_hook_ops;
    return 0;
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
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_pacing_qualification_inner_init(
        adapter, &tcp_shift_cubic_ops, transport, init, policy);
    if (result == 0) {
        /* This wrapper has just installed a nonzero transport pacing fallback,
         * so RFC 9406's paced L=infinity rule now describes the actual flow. */
        tcp_shift_cubic_model_set_hystart_pacing(
            &adapter->controller_state.cubic, 1U);
    }
    return result;
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
        if (tcp_shift_pacing_qualification_reinit_linux(adapter, newpcb) < 0 ||
            (binding->quantum_mode != 0U &&
             tcp_shift_pacing_qualification_quantum_attach(adapter) < 0)) {
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
    unsigned quantum_mode;

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
    quantum_mode =
        (unsigned)tcp_shift_pacing_qualification_quantum_mode();
    memset(binding, 0, sizeof(*binding));
    binding->listener = pcb;
    binding->accept = accept;
    binding->callback_arg = pcb->callback_arg;
    binding->linux_mode = linux_mode;
    binding->quantum_mode = quantum_mode;
    binding->used = 1U;

    tcp_arg(pcb, binding);
    if (linux_mode != 0U) {
        /* Let the normal selector bind Reno/CUBIC first. The child callback
         * then replaces only the controller policy surface with qualification
         * transport pacing; allocation/lifetime remains owned by the adapter.
         * linux-quantum additionally wraps only the segment eligibility hook;
         * the base adapter remains authoritative for all other callbacks. */
        tcp_shift_lwip_cc_accept_selected(pcb,
                                          tcp_shift_pacing_qualification_accept);
    } else {
        tcp_shift_lwip_cc_accept(pcb, tcp_shift_pacing_qualification_accept);
    }
}
