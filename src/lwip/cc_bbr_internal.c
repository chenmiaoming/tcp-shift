#include "lwip/cc_adapter.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cc/bbr_controller.h"
#include "cc/bbr_recovery.h"
#include "lwip/tcp_memory.h"
#include "lwip/priv/tcp_priv.h"

#define TCP_SHIFT_LWIP_BBR_EXT_ARG_ID 2U

struct tcp_shift_lwip_bbr_binding {
    struct tcp_shift_bbr_controller_state controller;
    struct tcp_shift_lwip_cc_adapter *adapter;
    const struct tcp_shift_lwip_cc_hook_ops *base_hook_ops;
    uint64_t recovery_enter_ns;
    uint32_t cycle_seed;
};

static uint64_t tcp_shift_lwip_bbr_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void tcp_shift_lwip_bbr_note_recovery_cwnd(
    struct tcp_shift_lwip_bbr_binding *binding)
{
    struct tcp_shift_lwip_cc_stats *stats;
    uint32_t cwnd;

    if (binding == NULL || binding->adapter == NULL ||
        binding->adapter->stats == NULL) {
        return;
    }
    stats = binding->adapter->stats;
    cwnd = binding->controller.cwnd_bytes;
    if (cwnd != 0U &&
        (stats->bbr_recovery_min_cwnd_bytes == 0U ||
         cwnd < stats->bbr_recovery_min_cwnd_bytes)) {
        stats->bbr_recovery_min_cwnd_bytes = cwnd;
    }
}

static void tcp_shift_lwip_bbr_record_stats(
    struct tcp_shift_lwip_bbr_binding *binding)
{
    struct tcp_shift_lwip_cc_stats *stats;
    const struct tcp_shift_bbr_model *model;

    if (binding == NULL || binding->adapter == NULL ||
        binding->adapter->stats == NULL) {
        return;
    }

    stats = binding->adapter->stats;
    model = &binding->controller.model;
    stats->bbr_model_observations++;
    stats->bbr_max_bw_bytes_per_sec = model->max_bw_bytes_per_sec;
    stats->bbr_min_rtt_ns = model->has_min_rtt != 0U ? model->min_rtt_ns : 0U;
    stats->bbr_full_bw_bytes_per_sec = model->full_bw_bytes_per_sec;
    stats->bbr_accepted_bw_samples = model->accepted_bw_samples;
    stats->bbr_ignored_app_limited_bw_samples =
        model->ignored_app_limited_bw_samples;
    stats->bbr_round_count = model->round_count;
    stats->bbr_full_bw_count = model->full_bw_count;
    stats->bbr_mode = (uint32_t)model->mode;
    stats->bbr_cycle_index = binding->controller.probe.cycle_index;
    stats->bbr_full_bw_reached = model->full_bw_reached;
    stats->bbr_recovery_in_progress =
        binding->controller.recovery.in_recovery;
}

static uint32_t tcp_shift_lwip_bbr_cwnd_limit(void)
{
#if LWIP_WND_SCALE
    return UINT32_MAX;
#else
    return UINT16_MAX;
#endif
}

static void tcp_shift_lwip_bbr_segment_list_stats(
    const struct tcp_seg *seg,
    uint32_t *segments,
    uint32_t *bytes)
{
    uint32_t count = 0U;
    uint32_t total = 0U;

    while (seg != NULL) {
        count++;
        if (UINT32_MAX - total < seg->len) {
            total = UINT32_MAX;
        } else {
            total += seg->len;
        }
        seg = seg->next;
    }

    if (segments != NULL) {
        *segments = count;
    }
    if (bytes != NULL) {
        *bytes = total;
    }
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
    int result;

    if (binding == NULL) {
        return -1;
    }
    result = tcp_shift_bbr_controller_on_ack(
        &binding->controller, transport, ack, policy);
    if (result == 0) {
        if (binding->controller.recovery.in_recovery != 0U &&
            binding->adapter != NULL && binding->adapter->stats != NULL) {
            if (binding->controller.recovery.packet_conservation != 0U) {
                binding->adapter->stats
                    ->bbr_recovery_packet_conservation_acks++;
            }
            tcp_shift_lwip_bbr_note_recovery_cwnd(binding);
        }
        tcp_shift_lwip_bbr_record_stats(binding);
    }
    return result;
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
    {
        int result = tcp_shift_bbr_controller_recovery_enter(
            &binding->controller, &post_loss, loss->lost_bytes, policy);
        if (result == 0) {
            uint64_t now_ns = tcp_shift_lwip_bbr_now_ns();

            binding->recovery_enter_ns = now_ns;
            if (binding->adapter != NULL && binding->adapter->stats != NULL) {
                struct tcp_shift_lwip_cc_stats *stats =
                    binding->adapter->stats;

                stats->bbr_recovery_enter_events++;
                stats->bbr_recovery_last_enter_ns = now_ns;
                stats->bbr_recovery_last_enter_cwnd_bytes =
                    binding->controller.cwnd_bytes;
                stats->bbr_recovery_last_enter_inflight_bytes =
                    post_loss.inflight_bytes;
            }
            tcp_shift_lwip_bbr_note_recovery_cwnd(binding);
            tcp_shift_lwip_bbr_record_stats(binding);
        }
        return result;
    }
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
    if (binding->adapter->stats != NULL) {
        struct tcp_shift_lwip_cc_stats *stats = binding->adapter->stats;

        stats->bbr_timeout_observations++;
        stats->bbr_timeout_last_max_bw_bytes_per_sec =
            binding->controller.model.max_bw_bytes_per_sec;
        stats->bbr_timeout_last_pacing_rate_bytes_per_sec =
            binding->controller.pacing_rate_bytes_per_sec;
        stats->bbr_timeout_last_cwnd_bytes = binding->controller.cwnd_bytes;
        stats->bbr_timeout_last_transport_inflight_bytes =
            transport->inflight_bytes;
        stats->bbr_timeout_last_round_count =
            binding->controller.model.round_count;
        stats->bbr_timeout_last_mode =
            (uint32_t)binding->controller.model.mode;
        stats->bbr_timeout_last_cycle_index =
            binding->controller.probe.cycle_index;
        stats->bbr_timeout_last_recovery_in_progress =
            binding->controller.recovery.in_recovery;
        stats->bbr_timeout_last_packet_conservation =
            binding->controller.recovery.packet_conservation;
        stats->bbr_timeout_last_lastack = binding->adapter->pcb->lastack;
        stats->bbr_timeout_last_snd_nxt = binding->adapter->pcb->snd_nxt;
        stats->bbr_timeout_last_recovery_end_seq =
            binding->adapter->hook.recovery_end_seq;
        stats->bbr_timeout_last_dupacks = binding->adapter->pcb->dupacks;
        stats->bbr_timeout_last_nrtx = binding->adapter->pcb->nrtx;
        stats->bbr_timeout_last_rtime = binding->adapter->pcb->rtime;
        stats->bbr_timeout_last_rto = binding->adapter->pcb->rto;
        tcp_shift_lwip_bbr_segment_list_stats(
            binding->adapter->pcb->unacked,
            &stats->bbr_timeout_last_unacked_segments,
            &stats->bbr_timeout_last_unacked_bytes);
        tcp_shift_lwip_bbr_segment_list_stats(
            binding->adapter->pcb->unsent,
            &stats->bbr_timeout_last_unsent_segments,
            &stats->bbr_timeout_last_unsent_bytes);
    }
    {
        int result = tcp_shift_bbr_controller_on_timeout(
            &binding->controller, transport, &timeout, policy);
        if (result == 0) {
            tcp_shift_lwip_bbr_record_stats(binding);
        }
        return result;
    }
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

static int tcp_shift_lwip_bbr_hook_ack_observe(
    void *arg,
    struct tcp_pcb *pcb,
    tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->on_ack_observe == NULL) {
        return 0;
    }
    return binding->base_hook_ops->on_ack_observe(arg, pcb, acked_bytes);
}

static int tcp_shift_lwip_bbr_hook_sack(
    void *arg,
    struct tcp_pcb *pcb,
    const struct tcp_shift_lwip_sack_range *ranges,
    u8_t range_count)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->on_sack == NULL) {
        return 0;
    }
    return binding->base_hook_ops->on_sack(
        arg, pcb, ranges, range_count);
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
        uint64_t now_ns = tcp_shift_lwip_bbr_now_ns();
        struct tcp_shift_lwip_cc_stats *stats = adapter->stats;

        stats->policy_updates++;
        stats->bbr_recovery_exit_events++;
        stats->bbr_recovery_last_exit_ns = now_ns;
        if (binding->recovery_enter_ns != 0U &&
            now_ns >= binding->recovery_enter_ns) {
            uint64_t recovery_ns = now_ns - binding->recovery_enter_ns;

            if (UINT64_MAX - stats->bbr_recovery_total_ns < recovery_ns) {
                stats->bbr_recovery_total_ns = UINT64_MAX;
            } else {
                stats->bbr_recovery_total_ns += recovery_ns;
            }
            if (recovery_ns > stats->bbr_recovery_max_ns) {
                stats->bbr_recovery_max_ns = recovery_ns;
            }
        }
    }
    binding->recovery_enter_ns = 0U;
    tcp_shift_lwip_bbr_note_recovery_cwnd(binding);
    tcp_shift_lwip_bbr_record_stats(binding);
    return 1;
}

static int tcp_shift_lwip_bbr_hook_separate_cwnd_window(
    void *arg,
    struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    return binding != NULL && adapter->sack_delivery_policy != 0U &&
                   adapter->hook.recovery_controller_owned != 0U
               ? 1
               : 0;
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
    .on_ack_observe = tcp_shift_lwip_bbr_hook_ack_observe,
    .on_sack = tcp_shift_lwip_bbr_hook_sack,
    .on_loss = tcp_shift_lwip_bbr_hook_loss,
    .on_timeout = tcp_shift_lwip_bbr_hook_timeout,
    .on_recovery_exit = tcp_shift_lwip_bbr_hook_recovery_exit,
    .on_separate_cwnd_window =
        tcp_shift_lwip_bbr_hook_separate_cwnd_window,
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
#if LWIP_TCP_SACK_OUT
    adapter->sack_delivery_policy = 1U;
#endif
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
    tcp_shift_lwip_bbr_record_stats(binding);
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
