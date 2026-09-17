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

static int tcp_shift_lwip_bbr_loss_pending(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_loss *loss,
    struct tcp_shift_cc_policy *policy)
{
    (void)state;
    (void)transport;
    (void)loss;
    (void)policy;

    /* Recovery ownership is deliberately not claimed by this checkpoint.
     * Returning an error makes the base adapter disable the internal binding
     * and lets pinned lwIP execute its established native recovery path. */
    return -1;
}

static int tcp_shift_lwip_bbr_timeout_pending(
    void *state,
    const struct tcp_shift_cc_transport *transport,
    struct tcp_shift_cc_policy *policy)
{
    (void)state;
    (void)transport;
    (void)policy;

    /* The next checkpoint maps the post-rto_prepare inflight observation into
     * tcp_shift_bbr_controller_on_timeout(). Until then, fail back to native
     * lwIP instead of publishing an unqualified timeout policy. */
    return -1;
}

static const struct tcp_shift_cc_ops tcp_shift_lwip_internal_bbr_ops = {
    .name = "bbr-internal-lwip",
    .state_size = sizeof(struct tcp_shift_lwip_bbr_binding),
    .init = tcp_shift_lwip_bbr_init,
    .on_ack = tcp_shift_lwip_bbr_on_ack,
    .on_loss = tcp_shift_lwip_bbr_loss_pending,
    .on_timeout = tcp_shift_lwip_bbr_timeout_pending,
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
        adapter->controller.state == NULL) {
        return 0;
    }
    return tcp_ext_arg_get(adapter->pcb,
                           (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID) ==
                   adapter->controller.state
               ? 1
               : 0;
}
