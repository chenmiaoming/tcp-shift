#include "lwip/cc_adapter.h"
#include "cc/transport_pacing.h"

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
static unsigned tcp_shift_cc_selector_internal_bbr;
static const struct tcp_shift_lwip_cc_hook_ops *
    tcp_shift_cc_selector_base_hook_ops;

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

#if defined(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE) && \
    TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE
    if (name != NULL && strcmp(name, "bbr") == 0) {
        /* Compact BBR is adapter-bound and deliberately absent from the
         * pure-C registry. The ordinary selector chains adapter/pacer binding
         * into the existing BBR sidecar. RFC 3168 ECN is still unsupported by
         * compact BBR, so an ECN-capable build keeps Reno/CUBIC available but
         * fails closed if BBR itself is requested. */
#if defined(TCP_SHIFT_EXPERIMENTAL_ECN) && TCP_SHIFT_EXPERIMENTAL_ECN
        tcp_shift_cc_selector_internal_bbr = 0U;
        return -1;
#else
        tcp_shift_cc_selector_ops = tcp_shift_cc_default_ops();
        tcp_shift_cc_selector_internal_bbr = 1U;
        return tcp_shift_cc_selector_ops != NULL ? 0 : -1;
#endif
    }
#endif

    if (name == NULL || name[0] == '\0') {
        ops = tcp_shift_cc_default_ops();
    } else {
        ops = tcp_shift_cc_find_ops(name);
    }
    if (ops == NULL) {
        return -1;
    }
    tcp_shift_cc_selector_ops = ops;
    tcp_shift_cc_selector_internal_bbr = 0U;
    return 0;
}

const char *tcp_shift_lwip_cc_configured_controller_name(void)
{
#if defined(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE) && \
    TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE
    if (tcp_shift_cc_selector_internal_bbr != 0U) {
        return "bbr";
    }
#endif
    {
        const struct tcp_shift_cc_ops *ops = tcp_shift_cc_selector_current();

        return ops != NULL ? ops->name : NULL;
    }
}

static int tcp_shift_cc_selector_is_loss_based_ops(
    const struct tcp_shift_cc_ops *ops)
{
    return ops == &tcp_shift_reno_ops || ops == &tcp_shift_cubic_ops;
}

static void tcp_shift_cc_selector_set_cubic_pacing(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_ops *inner,
    unsigned active)
{
    if (adapter != NULL && inner == &tcp_shift_cubic_ops) {
        tcp_shift_cubic_model_set_hystart_pacing(
            &adapter->controller_state.cubic, active);
    }
}

static int tcp_shift_cc_selector_apply_transport_pacing(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_ops *inner,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    int result;

    if (adapter == NULL || inner == NULL || transport == NULL || policy == NULL ||
        tcp_shift_cc_selector_is_loss_based_ops(inner) == 0) {
        return -1;
    }

    /* The flow id/generation pair is allocated only when this PCB has actually
     * registered with the process-wide shared pacer. Merely having pacing code
     * in the binary is not enough to select RFC 9406's paced behavior. */
    if (adapter->pacing_flow_id == 0U || adapter->pacing_generation == 0U) {
        tcp_shift_cc_selector_set_cubic_pacing(adapter, inner, 0U);
        return 0;
    }

    /* Production uses the uncapped generic fallback. Controller-owned nonzero
     * rates keep strict precedence inside the shared helper; no bottleneck/path
     * knowledge is injected here. */
    result = tcp_shift_transport_pacing_apply_window_fallback(
        transport, adapter->srtt.smoothed_rtt_ns, 0U, policy);
    if (result != 0 || policy->pacing_rate_bytes_per_sec == 0U) {
        tcp_shift_cc_selector_set_cubic_pacing(adapter, inner, 0U);
        return -1;
    }

    /* The policy now carries a finite nonzero rate and the flow is registered
     * with the real scheduler. Only at this point is CUBIC allowed to use the
     * RFC 9406 paced L=infinity ACK-growth rule. */
    tcp_shift_cc_selector_set_cubic_pacing(adapter, inner, 1U);
    return 0;
}

static int tcp_shift_cc_selector_paced_reno_ack(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_reno_ops.on_ack(&adapter->controller_state, transport,
                                       ack, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_reno_ops, transport, policy);
}

static int tcp_shift_cc_selector_paced_reno_loss(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_reno_ops.on_loss(&adapter->controller_state, transport,
                                        loss, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_reno_ops, transport, policy);
}

static int tcp_shift_cc_selector_paced_reno_ecn(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_reno_ops.on_ecn(
        &adapter->controller_state, transport, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_reno_ops, transport, policy);
}

static int tcp_shift_cc_selector_paced_reno_timeout(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_reno_ops.on_timeout(&adapter->controller_state, transport,
                                           policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_reno_ops, transport, policy);
}

static int tcp_shift_cc_selector_paced_cubic_ack(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_cubic_ops.on_ack(&adapter->controller_state, transport,
                                        ack, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_cubic_ops, transport, policy);
}

static int tcp_shift_cc_selector_paced_cubic_loss(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_cubic_ops.on_loss(&adapter->controller_state, transport,
                                         loss, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_cubic_ops, transport, policy);
}

static int tcp_shift_cc_selector_paced_cubic_ecn(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_cubic_ops.on_ecn(
        &adapter->controller_state, transport, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_cubic_ops, transport, policy);
}

static int tcp_shift_cc_selector_paced_cubic_timeout(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_cc_adapter *adapter = state;
    int result;

    result = tcp_shift_cubic_ops.on_timeout(&adapter->controller_state,
                                            transport, policy);
    if (result != 0) {
        return result;
    }
    return tcp_shift_cc_selector_apply_transport_pacing(
        adapter, &tcp_shift_cubic_ops, transport, policy);
}

/* These ops are callback-only transport wrappers. They are installed on the
 * controller object only while the base adapter is executing an ACK/loss/RTO
 * callback, then the selector restores the original raw Reno/CUBIC ops/state.
 * This lets the base adapter see the final nonzero pacing policy before its
 * apply_policy() step, so it never cancels/reset the virtual pacing clock on
 * every ACK merely because the underlying loss-based controller publishes 0. */
static const struct tcp_shift_cc_ops tcp_shift_cc_selector_paced_reno_ops = {
    .name = "reno-production-transport-pacing",
    .state_size = sizeof(struct tcp_shift_lwip_cc_adapter),
    .init = NULL,
    .on_ack = tcp_shift_cc_selector_paced_reno_ack,
    .on_loss = tcp_shift_cc_selector_paced_reno_loss,
    .on_ecn = tcp_shift_cc_selector_paced_reno_ecn,
    .on_timeout = tcp_shift_cc_selector_paced_reno_timeout,
};

static const struct tcp_shift_cc_ops tcp_shift_cc_selector_paced_cubic_ops = {
    .name = "cubic-production-transport-pacing",
    .state_size = sizeof(struct tcp_shift_lwip_cc_adapter),
    .init = NULL,
    .on_ack = tcp_shift_cc_selector_paced_cubic_ack,
    .on_loss = tcp_shift_cc_selector_paced_cubic_loss,
    .on_ecn = tcp_shift_cc_selector_paced_cubic_ecn,
    .on_timeout = tcp_shift_cc_selector_paced_cubic_timeout,
};

static const struct tcp_shift_cc_ops *tcp_shift_cc_selector_wrapper_for(
    const struct tcp_shift_cc_ops *inner)
{
    if (inner == &tcp_shift_reno_ops) {
        return &tcp_shift_cc_selector_paced_reno_ops;
    }
    if (inner == &tcp_shift_cubic_ops) {
        return &tcp_shift_cc_selector_paced_cubic_ops;
    }
    return NULL;
}

static int tcp_shift_cc_selector_call_paced_ack(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    tcpwnd_size_t acked_bytes)
{
    const struct tcp_shift_cc_ops *inner;
    const struct tcp_shift_cc_ops *wrapper;
    void *inner_state;
    int result;

    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_ack == NULL) {
        return 0;
    }
    if (adapter == NULL || adapter->controller.ops == NULL ||
        tcp_shift_cc_selector_is_loss_based_ops(adapter->controller.ops) == 0) {
        return tcp_shift_cc_selector_base_hook_ops->on_ack(adapter, pcb,
                                                           acked_bytes);
    }

    inner = adapter->controller.ops;
    wrapper = tcp_shift_cc_selector_wrapper_for(inner);
    if (wrapper == NULL) {
        return tcp_shift_cc_selector_base_hook_ops->on_ack(adapter, pcb,
                                                           acked_bytes);
    }
    inner_state = adapter->controller.state;
    adapter->controller.ops = wrapper;
    adapter->controller.state = adapter;
    result = tcp_shift_cc_selector_base_hook_ops->on_ack(adapter, pcb,
                                                         acked_bytes);
    adapter->controller.ops = inner;
    adapter->controller.state = inner_state;
    return result;
}

static int tcp_shift_cc_selector_call_paced_loss(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    tcpwnd_size_t lost_bytes)
{
    const struct tcp_shift_cc_ops *inner;
    const struct tcp_shift_cc_ops *wrapper;
    void *inner_state;
    int result;

    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_loss == NULL) {
        return 0;
    }
    if (adapter == NULL || adapter->controller.ops == NULL ||
        tcp_shift_cc_selector_is_loss_based_ops(adapter->controller.ops) == 0) {
        return tcp_shift_cc_selector_base_hook_ops->on_loss(adapter, pcb,
                                                            lost_bytes);
    }

    inner = adapter->controller.ops;
    wrapper = tcp_shift_cc_selector_wrapper_for(inner);
    if (wrapper == NULL) {
        return tcp_shift_cc_selector_base_hook_ops->on_loss(adapter, pcb,
                                                            lost_bytes);
    }
    inner_state = adapter->controller.state;
    adapter->controller.ops = wrapper;
    adapter->controller.state = adapter;
    result = tcp_shift_cc_selector_base_hook_ops->on_loss(adapter, pcb,
                                                          lost_bytes);
    adapter->controller.ops = inner;
    adapter->controller.state = inner_state;
    return result;
}

static int tcp_shift_cc_selector_call_paced_rack_retrans_loss(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    tcpwnd_size_t lost_bytes)
{
    const struct tcp_shift_cc_ops *inner;
    const struct tcp_shift_cc_ops *wrapper;
    void *inner_state;
    int result;

    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_rack_retrans_loss == NULL) {
        return 0;
    }
    if (adapter == NULL || adapter->controller.ops == NULL ||
        tcp_shift_cc_selector_is_loss_based_ops(adapter->controller.ops) == 0) {
        return tcp_shift_cc_selector_base_hook_ops->on_rack_retrans_loss(
            adapter, pcb, lost_bytes);
    }

    inner = adapter->controller.ops;
    wrapper = tcp_shift_cc_selector_wrapper_for(inner);
    if (wrapper == NULL) {
        return tcp_shift_cc_selector_base_hook_ops->on_rack_retrans_loss(
            adapter, pcb, lost_bytes);
    }
    inner_state = adapter->controller.state;
    adapter->controller.ops = wrapper;
    adapter->controller.state = adapter;
    result = tcp_shift_cc_selector_base_hook_ops->on_rack_retrans_loss(
        adapter, pcb, lost_bytes);
    adapter->controller.ops = inner;
    adapter->controller.state = inner_state;
    return result;
}

static int tcp_shift_cc_selector_call_paced_ecn(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb)
{
    const struct tcp_shift_cc_ops *inner;
    const struct tcp_shift_cc_ops *wrapper;
    void *inner_state;
    int result;

    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_ecn == NULL) {
        return 0;
    }
    if (adapter == NULL || adapter->controller.ops == NULL ||
        tcp_shift_cc_selector_is_loss_based_ops(adapter->controller.ops) == 0) {
        return tcp_shift_cc_selector_base_hook_ops->on_ecn(adapter, pcb);
    }

    inner = adapter->controller.ops;
    wrapper = tcp_shift_cc_selector_wrapper_for(inner);
    if (wrapper == NULL) {
        return tcp_shift_cc_selector_base_hook_ops->on_ecn(adapter, pcb);
    }
    inner_state = adapter->controller.state;
    adapter->controller.ops = wrapper;
    adapter->controller.state = adapter;
    result = tcp_shift_cc_selector_base_hook_ops->on_ecn(adapter, pcb);
    adapter->controller.ops = inner;
    adapter->controller.state = inner_state;
    return result;
}

static int tcp_shift_cc_selector_call_paced_timeout(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb)
{
    const struct tcp_shift_cc_ops *inner;
    const struct tcp_shift_cc_ops *wrapper;
    void *inner_state;
    int result;

    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_timeout == NULL) {
        return 0;
    }
    if (adapter == NULL || adapter->controller.ops == NULL ||
        tcp_shift_cc_selector_is_loss_based_ops(adapter->controller.ops) == 0) {
        return tcp_shift_cc_selector_base_hook_ops->on_timeout(adapter, pcb);
    }

    inner = adapter->controller.ops;
    wrapper = tcp_shift_cc_selector_wrapper_for(inner);
    if (wrapper == NULL) {
        return tcp_shift_cc_selector_base_hook_ops->on_timeout(adapter, pcb);
    }
    inner_state = adapter->controller.state;
    adapter->controller.ops = wrapper;
    adapter->controller.state = adapter;
    result = tcp_shift_cc_selector_base_hook_ops->on_timeout(adapter, pcb);
    adapter->controller.ops = inner;
    adapter->controller.state = inner_state;
    return result;
}

static int tcp_shift_cc_selector_pacing_on_ack(void *arg,
                                                struct tcp_pcb *pcb,
                                                tcpwnd_size_t acked_bytes)
{
    return tcp_shift_cc_selector_call_paced_ack(arg, pcb, acked_bytes);
}

static void tcp_shift_cc_selector_pacing_on_ack_begin(
    void *arg,
    struct tcp_pcb *pcb)
{
    if (tcp_shift_cc_selector_base_hook_ops != NULL &&
        tcp_shift_cc_selector_base_hook_ops->on_ack_begin != NULL) {
        tcp_shift_cc_selector_base_hook_ops->on_ack_begin(arg, pcb);
    }
}

static int tcp_shift_cc_selector_pacing_on_ack_observe(
    void *arg,
    struct tcp_pcb *pcb,
    tcpwnd_size_t acked_bytes)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_ack_observe == NULL) {
        return 0;
    }
    return tcp_shift_cc_selector_base_hook_ops->on_ack_observe(
        arg, pcb, acked_bytes);
}

static int tcp_shift_cc_selector_pacing_on_sack(
    void *arg,
    struct tcp_pcb *pcb,
    u32_t ack_seq,
    const struct tcp_shift_lwip_sack_range *ranges,
    u8_t range_count)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_sack == NULL) {
        return 0;
    }
    return tcp_shift_cc_selector_base_hook_ops->on_sack(
        arg, pcb, ack_seq, ranges, range_count);
}

static int tcp_shift_cc_selector_pacing_on_loss(void *arg,
                                                 struct tcp_pcb *pcb,
                                                 tcpwnd_size_t lost_bytes)
{
    return tcp_shift_cc_selector_call_paced_loss(arg, pcb, lost_bytes);
}

static int tcp_shift_cc_selector_pacing_on_ecn(void *arg,
                                                struct tcp_pcb *pcb)
{
    return tcp_shift_cc_selector_call_paced_ecn(arg, pcb);
}

#if defined(TCP_SHIFT_EXPERIMENTAL_ECN) && TCP_SHIFT_EXPERIMENTAL_ECN
static void tcp_shift_cc_selector_pacing_on_ecn_rto_gate(
    void *arg,
    struct tcp_pcb *pcb,
    unsigned entering)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_ecn_rto_gate == NULL) {
        return;
    }
    tcp_shift_cc_selector_base_hook_ops->on_ecn_rto_gate(
        arg, pcb, entering);
}
#endif

static int tcp_shift_cc_selector_pacing_on_rack_retrans_loss(
    void *arg,
    struct tcp_pcb *pcb,
    tcpwnd_size_t lost_bytes)
{
    return tcp_shift_cc_selector_call_paced_rack_retrans_loss(
        arg, pcb, lost_bytes);
}

static void tcp_shift_cc_selector_pacing_on_tlp_dupack(
    void *arg,
    struct tcp_pcb *pcb,
    unsigned sack_seen)
{
    if (tcp_shift_cc_selector_base_hook_ops != NULL &&
        tcp_shift_cc_selector_base_hook_ops->on_tlp_dupack != NULL) {
        tcp_shift_cc_selector_base_hook_ops->on_tlp_dupack(
            arg, pcb, sack_seen);
    }
}

static int tcp_shift_cc_selector_pacing_on_tlp_loss(
    void *arg,
    struct tcp_pcb *pcb,
    tcpwnd_size_t lost_bytes)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_tlp_loss == NULL) {
        return 0;
    }
    return tcp_shift_cc_selector_base_hook_ops->on_tlp_loss(
        arg, pcb, lost_bytes);
}

static int tcp_shift_cc_selector_pacing_on_timeout(void *arg,
                                                    struct tcp_pcb *pcb)
{
    return tcp_shift_cc_selector_call_paced_timeout(arg, pcb);
}

static int tcp_shift_cc_selector_pacing_on_recovery_exit(
    void *arg,
    struct tcp_pcb *pcb,
    u32_t ack_seq)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->on_recovery_exit == NULL) {
        return 0;
    }
    return tcp_shift_cc_selector_base_hook_ops->on_recovery_exit(
        arg, pcb, ack_seq);
}

static unsigned tcp_shift_cc_selector_pacing_prr_active(
    void *arg,
    struct tcp_pcb *pcb)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->prr_active == NULL) {
        return 0U;
    }
    return tcp_shift_cc_selector_base_hook_ops->prr_active(arg, pcb);
}

static int tcp_shift_cc_selector_pacing_rack_loss_status(
    void *arg,
    struct tcp_pcb *pcb,
    const void *segment,
    uint64_t *remaining_ns)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->rack_loss_status == NULL) {
        if (remaining_ns != NULL) {
            *remaining_ns = 0U;
        }
        return -1;
    }
    return tcp_shift_cc_selector_base_hook_ops->rack_loss_status(
        arg, pcb, segment, remaining_ns);
}

static u32_t tcp_shift_cc_selector_pacing_effective_cwnd(
    void *arg,
    struct tcp_pcb *pcb)
{
    if (tcp_shift_cc_selector_base_hook_ops == NULL ||
        tcp_shift_cc_selector_base_hook_ops->effective_cwnd == NULL) {
        return pcb != NULL ? (u32_t)pcb->cwnd : 0U;
    }
    return tcp_shift_cc_selector_base_hook_ops->effective_cwnd(arg, pcb);
}

static int tcp_shift_cc_selector_pacing_send_eligible(void *arg,
                                                       struct tcp_pcb *pcb,
                                                       u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    const struct tcp_shift_cc_ops *inner =
        adapter != NULL ? adapter->controller.ops : NULL;
    int result = 1;

    if (tcp_shift_cc_selector_base_hook_ops != NULL &&
        tcp_shift_cc_selector_base_hook_ops->on_segment_send_eligible != NULL) {
        result = tcp_shift_cc_selector_base_hook_ops->on_segment_send_eligible(
            arg, pcb, payload_bytes);
    }

    /* The base adapter drops the rate to zero if the scheduler cannot execute
     * a deadline. Reflect that runtime failure back into raw production CUBIC
     * immediately; the next policy callback may retry pacing, but the ACK that
     * arrives while pacing is down must use the RFC's non-paced L=8 cap. */
    if (adapter != NULL && adapter->pacing_rate_bytes_per_sec == 0U &&
        inner == &tcp_shift_cubic_ops) {
        tcp_shift_cc_selector_set_cubic_pacing(adapter, inner, 0U);
    }
    return result;
}

static void tcp_shift_cc_selector_pacing_on_segment_tx(
    void *arg,
    struct tcp_pcb *pcb,
    const void *segment,
    u32_t seq_start,
    u16_t payload_bytes)
{
    if (tcp_shift_cc_selector_base_hook_ops != NULL &&
        tcp_shift_cc_selector_base_hook_ops->on_segment_tx != NULL) {
        tcp_shift_cc_selector_base_hook_ops->on_segment_tx(
            arg, pcb, segment, seq_start, payload_bytes);
    }
}

static void tcp_shift_cc_selector_pacing_on_segment_acked(
    void *arg,
    struct tcp_pcb *pcb,
    const void *segment,
    u16_t payload_bytes)
{
    if (tcp_shift_cc_selector_base_hook_ops != NULL &&
        tcp_shift_cc_selector_base_hook_ops->on_segment_acked != NULL) {
        tcp_shift_cc_selector_base_hook_ops->on_segment_acked(
            arg, pcb, segment, payload_bytes);
    }
}

static const struct tcp_shift_lwip_cc_hook_ops
    tcp_shift_cc_selector_pacing_hook_ops = {
        .on_ack_begin = tcp_shift_cc_selector_pacing_on_ack_begin,
        .on_ack = tcp_shift_cc_selector_pacing_on_ack,
        .on_ack_observe = tcp_shift_cc_selector_pacing_on_ack_observe,
        .on_sack = tcp_shift_cc_selector_pacing_on_sack,
        .on_loss = tcp_shift_cc_selector_pacing_on_loss,
        .on_ecn = tcp_shift_cc_selector_pacing_on_ecn,
#if defined(TCP_SHIFT_EXPERIMENTAL_ECN) && TCP_SHIFT_EXPERIMENTAL_ECN
        .on_ecn_rto_gate =
            tcp_shift_cc_selector_pacing_on_ecn_rto_gate,
#endif
        .on_rack_retrans_loss =
            tcp_shift_cc_selector_pacing_on_rack_retrans_loss,
        .on_tlp_loss = tcp_shift_cc_selector_pacing_on_tlp_loss,
        .on_tlp_dupack = tcp_shift_cc_selector_pacing_on_tlp_dupack,
        .on_timeout = tcp_shift_cc_selector_pacing_on_timeout,
        .on_recovery_exit =
            tcp_shift_cc_selector_pacing_on_recovery_exit,
        .rack_loss_status =
            tcp_shift_cc_selector_pacing_rack_loss_status,
        .prr_active =
            tcp_shift_cc_selector_pacing_prr_active,
        .effective_cwnd =
            tcp_shift_cc_selector_pacing_effective_cwnd,
        .on_segment_send_eligible =
            tcp_shift_cc_selector_pacing_send_eligible,
        .on_segment_tx = tcp_shift_cc_selector_pacing_on_segment_tx,
        .on_segment_acked = tcp_shift_cc_selector_pacing_on_segment_acked,
};

static int tcp_shift_cc_selector_attach_transport_pacing(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL || adapter->hook.ops == NULL || adapter->pcb == NULL) {
        return -1;
    }

    /* Without a registered per-flow pacer, leave the original hook table and
     * loss-based controller untouched. */
    if (adapter->pacing_flow_id == 0U || adapter->pacing_generation == 0U) {
        tcp_shift_cc_selector_set_cubic_pacing(
            adapter, adapter->controller.ops, 0U);
        return 0;
    }

    if (adapter->hook.ops == &tcp_shift_cc_selector_pacing_hook_ops) {
        return 0;
    }
    if (tcp_shift_cc_selector_base_hook_ops == NULL) {
        tcp_shift_cc_selector_base_hook_ops = adapter->hook.ops;
    } else if (tcp_shift_cc_selector_base_hook_ops != adapter->hook.ops) {
        return -1;
    }
    adapter->hook.ops = &tcp_shift_cc_selector_pacing_hook_ops;
    return 0;
}

static int tcp_shift_cc_selector_publish_initial_pacing(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_ops *inner,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    uint64_t previous_rate;

    if (adapter->pacing_flow_id == 0U || adapter->pacing_generation == 0U) {
        tcp_shift_cc_selector_set_cubic_pacing(adapter, inner, 0U);
        return 0;
    }
    if (tcp_shift_cc_selector_apply_transport_pacing(adapter, inner, transport,
                                                       policy) != 0) {
        return -1;
    }

    previous_rate = adapter->pacing_rate_bytes_per_sec;
    adapter->pacing_rate_bytes_per_sec = policy->pacing_rate_bytes_per_sec;
    if (previous_rate == 0U) {
        adapter->pacing_next_send_ns = 0U;
    }
    if (adapter->stats != NULL) {
        adapter->stats->pacing_last_rate_bytes_per_sec =
            policy->pacing_rate_bytes_per_sec;
    }
    return 0;
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

    transport.mss_bytes = pcb->mss;
    transport.inflight_bytes = pcb->snd_nxt - pcb->lastack;
    transport.send_window_bytes = pcb->snd_wnd;
    transport.cwnd_limit_bytes = limit;

    if (ops == &tcp_shift_reno_ops) {
        policy.cwnd_bytes = pcb->cwnd;
        policy.ssthresh_bytes = pcb->ssthresh;
        policy.pacing_rate_bytes_per_sec = 0U;
        if (tcp_shift_cc_selector_publish_initial_pacing(
                adapter, &tcp_shift_reno_ops, &transport, &policy) != 0 ||
            tcp_shift_cc_selector_attach_transport_pacing(adapter) != 0) {
            return -1;
        }
        return 0;
    }

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

#if defined(TCP_SHIFT_P6_CUBIC_SINGLE_FLOW_NO_FAST_CONVERGENCE_QUALIFICATION)
    /* RFC 9438 section 4.7 says fast convergence SHOULD be disabled when
     * there is only one CUBIC flow and no other traffic. Keep production
     * defaults untouched; this qualification-only build isolates that policy
     * on the deterministic single-flow reference path. */
    if (ops == &tcp_shift_cubic_ops) {
        tcp_shift_cubic_model_set_fast_convergence(
            &adapter->controller_state.cubic, 0U);
    }
#endif

    if (tcp_shift_cc_selector_is_loss_based_ops(ops) != 0 &&
        tcp_shift_cc_selector_publish_initial_pacing(
            adapter, ops, &transport, &policy) != 0) {
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

    if (tcp_shift_cc_selector_is_loss_based_ops(ops) != 0 &&
        tcp_shift_cc_selector_attach_transport_pacing(adapter) != 0) {
        return -1;
    }
    return 0;
}

int tcp_shift_lwip_cc_apply_configured_controller(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL || adapter->pcb == NULL) {
        return -1;
    }
#if defined(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE) && \
    TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE
    if (tcp_shift_cc_selector_internal_bbr != 0U) {
        uint32_t cycle_seed =
            (uint32_t)adapter->pacing_flow_id ^
            (uint32_t)(adapter->pacing_flow_id >> 32);

        return tcp_shift_lwip_cc_apply_internal_bbr(adapter, cycle_seed);
    }
#endif
    return tcp_shift_cc_selector_reinit(adapter, adapter->pcb,
                                        tcp_shift_cc_selector_current());
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

#if defined(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE) && \
    TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE
    if (tcp_shift_cc_selector_internal_bbr != 0U) {
        tcp_shift_lwip_cc_accept_internal_bbr(pcb, accept);
        return;
    }
#endif

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
