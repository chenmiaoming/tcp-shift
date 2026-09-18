#include "lwip/cc_adapter.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cc/bbr_controller.h"
#include "cc/bbr_recovery.h"
#include "lwip/tcp_memory.h"

#define TCP_SHIFT_LWIP_BBR_EXT_ARG_ID 2U

struct tcp_shift_lwip_bbr_binding {
    struct tcp_shift_bbr_controller_state controller;
    struct tcp_shift_lwip_cc_adapter *adapter;
    const struct tcp_shift_lwip_cc_hook_ops *base_hook_ops;
    uint32_t cycle_seed;
};

static uint32_t tcp_shift_lwip_bbr_cwnd_limit(void)
{
#if LWIP_WND_SCALE
    return UINT32_MAX;
#else
    return UINT16_MAX;
#endif
}

static void tcp_shift_lwip_bbr_transport_from_pcb(
    const struct tcp_pcb *pcb,
    struct tcp_shift_cc_transport *transport)
{
    transport->mss_bytes = pcb->mss;
    transport->inflight_bytes = pcb->snd_nxt - pcb->lastack;
    transport->send_window_bytes = pcb->snd_wnd;
    transport->cwnd_limit_bytes = tcp_shift_lwip_bbr_cwnd_limit();
}

static int tcp_shift_lwip_bbr_apply_policy(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_policy *policy)
{
    uint32_t limit = tcp_shift_lwip_bbr_cwnd_limit();

    if (adapter == NULL || adapter->pcb == NULL || policy == NULL ||
        policy->cwnd_bytes == 0U || policy->ssthresh_bytes == 0U ||
        policy->cwnd_bytes > limit || policy->ssthresh_bytes > limit ||
        policy->pacing_rate_bytes_per_sec == 0U) {
        return -1;
    }

    adapter->pacing_rate_bytes_per_sec = policy->pacing_rate_bytes_per_sec;
    adapter->pcb->cwnd = (tcpwnd_size_t)policy->cwnd_bytes;
    adapter->pcb->ssthresh = (tcpwnd_size_t)policy->ssthresh_bytes;
    adapter->pcb->bytes_acked = 0U;
    if (adapter->stats != NULL) {
        adapter->stats->last_cwnd_bytes = policy->cwnd_bytes;
        adapter->stats->last_ssthresh_bytes = policy->ssthresh_bytes;
        adapter->stats->pacing_last_rate_bytes_per_sec =
            policy->pacing_rate_bytes_per_sec;
    }
    return 0;
}

static int tcp_shift_lwip_bbr_init(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_init *init,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_bbr_binding *binding = state;

    if (binding == NULL) {
        return -1;
    }
    return tcp_shift_bbr_controller_init(&binding->controller, transport, init,
                                         binding->cycle_seed, policy);
}

static int tcp_shift_lwip_bbr_on_ack(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_bbr_binding *binding = state;

    return binding == NULL
               ? -1
               : tcp_shift_bbr_controller_on_ack(&binding->controller,
                                                  transport, ack, policy);
}

static int tcp_shift_lwip_bbr_on_loss(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_bbr_binding *binding = state;
    struct tcp_shift_cc_transport post_loss;

    if (binding == NULL || transport == NULL || loss == NULL ||
        loss->lost_bytes == 0U) {
        return -1;
    }

    /* Pinned lwIP reports one fast-retransmit loss after moving that segment
     * from unacked to unsent, but snd_nxt-lastack still counts its sequence
     * space. BBR packet conservation needs the post-loss in-flight view. */
    post_loss = *transport;
    post_loss.inflight_bytes =
        loss->lost_bytes >= transport->inflight_bytes
            ? 0U
            : transport->inflight_bytes - loss->lost_bytes;
    return tcp_shift_bbr_controller_recovery_enter(
        &binding->controller, &post_loss, loss->lost_bytes, policy);
}

static int tcp_shift_lwip_bbr_on_timeout(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_lwip_bbr_binding *binding = state;
    struct tcp_shift_bbr_timeout_observation timeout;

    if (binding == NULL || binding->adapter == NULL ||
        binding->adapter->pcb == NULL || transport == NULL ||
        (binding->adapter->pcb->flags & TF_RTO) == 0U ||
        binding->adapter->pcb->unacked != NULL) {
        return -1;
    }

    /* tcp_slowtmr() invokes the hook after tcp_rexmit_rto_prepare(). At this
     * pinned boundary all previously unacked data has been marked for
     * retransmission and moved to unsent, while commit/output has not run yet.
     * The transport-equivalent post-loss in-flight observation is therefore 0. */
    timeout.post_loss_inflight_bytes = 0U;
    return tcp_shift_bbr_controller_on_timeout(
        &binding->controller, transport, &timeout, policy);
}

static const struct tcp_shift_cc_ops tcp_shift_lwip_internal_bbr_ops = {
    .name = "bbr-internal-lwip",
    .state_size = sizeof(struct tcp_shift_lwip_bbr_binding),
    .init = tcp_shift_lwip_bbr_init,
    .on_ack = tcp_shift_lwip_bbr_on_ack,
    .on_loss = tcp_shift_lwip_bbr_on_loss,
    .on_timeout = tcp_shift_lwip_bbr_on_timeout,
};

static struct tcp_shift_lwip_bbr_binding *
tcp_shift_lwip_bbr_binding_from_adapter(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_bbr_binding *binding;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        adapter->controller.ops != &tcp_shift_lwip_internal_bbr_ops ||
        adapter->controller.state == NULL) {
        return NULL;
    }
    binding = (struct tcp_shift_lwip_bbr_binding *)tcp_ext_arg_get(
        pcb, (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID);
    return binding != NULL && adapter->controller.state == binding
               ? binding
               : NULL;
}

static int tcp_shift_lwip_bbr_hook_ack(void *arg,
                                        struct tcp_pcb *pcb,
                                        tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->on_ack == NULL) {
        return 0;
    }
    return binding->base_hook_ops->on_ack(arg, pcb, acked_bytes);
}

static int tcp_shift_lwip_bbr_hook_loss(void *arg,
                                         struct tcp_pcb *pcb,
                                         tcpwnd_size_t lost_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);
    int handled;

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->on_loss == NULL) {
        return 0;
    }
    handled = binding->base_hook_ops->on_loss(arg, pcb, lost_bytes);
    if (handled != 0) {
        adapter->hook.recovery_controller_owned = 1U;
    }
    return handled;
}

static int tcp_shift_lwip_bbr_hook_timeout(void *arg, struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->on_timeout == NULL) {
        return 0;
    }
    return binding->base_hook_ops->on_timeout(arg, pcb);
}

static int tcp_shift_lwip_bbr_hook_recovery_exit(void *arg,
                                                  struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_policy policy;

    if (binding == NULL ||
        adapter->hook.recovery_controller_owned == 0U) {
        return 0;
    }

    tcp_shift_lwip_bbr_transport_from_pcb(pcb, &transport);
    if (tcp_shift_bbr_controller_recovery_exit(
            &binding->controller, &transport, &policy) != 0 ||
        tcp_shift_lwip_bbr_apply_policy(adapter, &policy) != 0) {
        return 0;
    }
    if (adapter->stats != NULL) {
        adapter->stats->policy_updates++;
    }
    return 1;
}

static int tcp_shift_lwip_bbr_hook_send_eligible(void *arg,
                                                  struct tcp_pcb *pcb,
                                                  u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->on_segment_send_eligible == NULL) {
        return 1;
    }
    return binding->base_hook_ops->on_segment_send_eligible(
        arg, pcb, payload_bytes);
}

static void tcp_shift_lwip_bbr_hook_segment_tx(void *arg,
                                                struct tcp_pcb *pcb,
                                                const void *segment,
                                                u32_t seq_start,
                                                u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding != NULL && binding->base_hook_ops != NULL &&
        binding->base_hook_ops->on_segment_tx != NULL) {
        binding->base_hook_ops->on_segment_tx(
            arg, pcb, segment, seq_start, payload_bytes);
    }
}

static void tcp_shift_lwip_bbr_hook_segment_acked(void *arg,
                                                   struct tcp_pcb *pcb,
                                                   const void *segment,
                                                   u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding != NULL && binding->base_hook_ops != NULL &&
        binding->base_hook_ops->on_segment_acked != NULL) {
        binding->base_hook_ops->on_segment_acked(
            arg, pcb, segment, payload_bytes);
    }
}

static const struct tcp_shift_lwip_cc_hook_ops tcp_shift_lwip_bbr_hook_ops = {
    .on_ack = tcp_shift_lwip_bbr_hook_ack,
    .on_loss = tcp_shift_lwip_bbr_hook_loss,
    .on_timeout = tcp_shift_lwip_bbr_hook_timeout,
    .on_recovery_exit = tcp_shift_lwip_bbr_hook_recovery_exit,
    .on_segment_send_eligible = tcp_shift_lwip_bbr_hook_send_eligible,
    .on_segment_tx = tcp_shift_lwip_bbr_hook_segment_tx,
    .on_segment_acked = tcp_shift_lwip_bbr_hook_segment_acked,
};

static void tcp_shift_lwip_bbr_destroyed(u8_t id, void *data)
{
    (void)id;
    free(data);
}

static const struct tcp_ext_arg_callbacks tcp_shift_lwip_bbr_callbacks = {
    .destroy = tcp_shift_lwip_bbr_destroyed,
    .passive_open = NULL,
};

int tcp_shift_lwip_cc_apply_internal_bbr(
    struct tcp_shift_lwip_cc_adapter *adapter,
    uint32_t cycle_seed)
{
    struct tcp_shift_lwip_bbr_binding *binding;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;
    struct tcp_shift_cc next;
    uint32_t limit;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb == NULL ||
        adapter->controller.ops != &tcp_shift_reno_ops ||
        adapter->controller.state != &adapter->reno ||
        adapter->pacing_flow_id == 0U || adapter->pacing_generation == 0U ||
        adapter->pacing_scheduled != 0U ||
        tcp_ext_arg_get(adapter->pcb,
                        (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID) != NULL) {
        return -1;
    }

    binding = calloc(1U, sizeof(*binding));
    if (binding == NULL) {
        return -1;
    }
    binding->cycle_seed = cycle_seed;
    binding->adapter = adapter;
    binding->base_hook_ops = adapter->hook.ops;
    if (binding->base_hook_ops == NULL) {
        free(binding);
        return -1;
    }

    tcp_shift_lwip_bbr_transport_from_pcb(adapter->pcb, &transport);
    init.initial_cwnd_bytes = adapter->pcb->cwnd;
    init.initial_ssthresh_bytes = adapter->pcb->ssthresh;
    init.min_cwnd_bytes = adapter->pcb->mss;
    if (tcp_shift_cc_init(&next, &tcp_shift_lwip_internal_bbr_ops,
                          binding, sizeof(*binding), &transport, &init,
                          &policy) != 0) {
        free(binding);
        return -1;
    }

    limit = tcp_shift_lwip_bbr_cwnd_limit();
    if (policy.cwnd_bytes == 0U || policy.ssthresh_bytes == 0U ||
        policy.cwnd_bytes > limit || policy.ssthresh_bytes > limit ||
        policy.pacing_rate_bytes_per_sec == 0U ||
        tcp_shift_lwip_tcp_memory_set_sndbuf_expand(
            adapter->pcb, TCP_SHIFT_BBR_SNDBUF_EXPAND_NUM,
            TCP_SHIFT_BBR_SNDBUF_EXPAND_DEN) != 0) {
        free(binding);
        return -1;
    }

    tcp_ext_arg_set_callbacks(adapter->pcb,
                              (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID,
                              &tcp_shift_lwip_bbr_callbacks);
    tcp_ext_arg_set(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID,
                    binding);

    adapter->controller = next;
    adapter->hook.ops = &tcp_shift_lwip_bbr_hook_ops;
    adapter->pacing_rate_bytes_per_sec = policy.pacing_rate_bytes_per_sec;
    adapter->pacing_next_send_ns = 0U;
    adapter->pcb->cwnd = (tcpwnd_size_t)policy.cwnd_bytes;
    adapter->pcb->ssthresh = (tcpwnd_size_t)policy.ssthresh_bytes;
    adapter->pcb->bytes_acked = 0U;
    if (adapter->stats != NULL) {
        adapter->stats->last_cwnd_bytes = policy.cwnd_bytes;
        adapter->stats->last_ssthresh_bytes = policy.ssthresh_bytes;
        adapter->stats->pacing_last_rate_bytes_per_sec =
            policy.pacing_rate_bytes_per_sec;
    }
    return 0;
}

int tcp_shift_lwip_cc_internal_bbr_active(
    const struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL || adapter->bound == 0U || adapter->pcb == NULL ||
        adapter->controller.ops != &tcp_shift_lwip_internal_bbr_ops ||
        adapter->controller.state == NULL ||
        adapter->hook.ops != &tcp_shift_lwip_bbr_hook_ops) {
        return 0;
    }
    return tcp_ext_arg_get(adapter->pcb,
                           (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID) ==
                   adapter->controller.state
               ? 1
               : 0;
}
