#include "lwip/cc_adapter.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cc/bbr_controller.h"
#include "cc/bbr_recovery.h"
#include "lwip/tcp_memory.h"
#include "lwip/priv/tcp_priv.h"

#define TCP_SHIFT_LWIP_BBR_EXT_ARG_ID 2U

#define TCP_SHIFT_BBR_STARTUP_TRACE_EVENTS 16U
#define TCP_SHIFT_BBR_STARTUP_TRACE_ROUNDS 12U
#define TCP_SHIFT_BBR_STARTUP_TRACE_FLOWS 16U

enum tcp_shift_bbr_startup_trace_kind {
    TCP_SHIFT_BBR_STARTUP_TRACE_INIT = 1U,
    TCP_SHIFT_BBR_STARTUP_TRACE_ROUND = 2U,
    TCP_SHIFT_BBR_STARTUP_TRACE_TRANSITION = 3U,
};

struct tcp_shift_bbr_startup_trace_event {
    uint64_t time_ns;
    uint64_t flow_id;
    uint64_t pacing_rate_bytes_per_sec;
    uint64_t max_bw_bytes_per_sec;
    uint64_t min_rtt_ns;
    uint64_t sample_rate_bytes_per_sec;
    uint64_t sample_rtt_ns;
    uint64_t delivered_total_bytes;
    uint32_t cwnd_bytes;
    uint32_t inflight_bytes;
    uint32_t acked_bytes;
    uint32_t flags;
    uint32_t round;
    uint32_t mode_before;
    uint32_t mode_after;
    uint32_t kind;
};

struct tcp_shift_bbr_startup_trace_retained {
    struct tcp_shift_bbr_startup_trace_event
        events[TCP_SHIFT_BBR_STARTUP_TRACE_EVENTS];
    uint32_t count;
};

struct tcp_shift_lwip_bbr_binding {
    struct tcp_shift_bbr_controller_state controller;
    struct tcp_shift_lwip_cc_adapter *adapter;
    const struct tcp_shift_lwip_cc_hook_ops *base_hook_ops;
    struct tcp_shift_bbr_startup_trace_retained *startup_trace;
    uint64_t recovery_enter_ns;
    uint64_t startup_trace_flow_id;
    uint32_t recovery_entry_acked_bytes;
    uint32_t startup_trace_rounds;
    uint32_t cycle_seed;
    unsigned startup_trace_enabled;
};

static struct tcp_shift_bbr_startup_trace_retained
    tcp_shift_bbr_startup_traces[TCP_SHIFT_BBR_STARTUP_TRACE_FLOWS];
static uint32_t tcp_shift_bbr_startup_trace_count;
static unsigned tcp_shift_bbr_startup_dumped;

static uint64_t tcp_shift_lwip_bbr_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

void tcp_shift_lwip_cc_dump_internal_bbr_startup_trace(void)
{
    uint32_t flow_index;

    if (tcp_shift_bbr_startup_dumped != 0U) {
        return;
    }

    for (flow_index = 0U;
         flow_index < tcp_shift_bbr_startup_trace_count;
         flow_index++) {
        const struct tcp_shift_bbr_startup_trace_retained *flow =
            &tcp_shift_bbr_startup_traces[flow_index];
        uint32_t event_index;

        for (event_index = 0U; event_index < flow->count; event_index++) {
            const struct tcp_shift_bbr_startup_trace_event *event =
                &flow->events[event_index];
            const char *kind =
                event->kind == TCP_SHIFT_BBR_STARTUP_TRACE_INIT
                    ? "init"
                    : event->kind == TCP_SHIFT_BBR_STARTUP_TRACE_ROUND
                          ? "round"
                          : "transition";

            fprintf(
                stderr,
                "tcp-shift-bbr-startup: flow_id=%" PRIu64
                " event=%s time_ns=%" PRIu64
                " round=%" PRIu32
                " mode_before=%" PRIu32
                " mode=%" PRIu32
                " cwnd_bytes=%" PRIu32
                " pacing_rate_Bps=%" PRIu64
                " max_bw_Bps=%" PRIu64
                " min_rtt_ns=%" PRIu64
                " inflight_bytes=%" PRIu32
                " acked_bytes=%" PRIu32
                " sample_rate_Bps=%" PRIu64
                " sample_rtt_ns=%" PRIu64
                " delivered_total_bytes=%" PRIu64
                " flags=%" PRIu32 "\n",
                event->flow_id, kind, event->time_ns, event->round,
                event->mode_before, event->mode_after, event->cwnd_bytes,
                event->pacing_rate_bytes_per_sec,
                event->max_bw_bytes_per_sec, event->min_rtt_ns,
                event->inflight_bytes, event->acked_bytes,
                event->sample_rate_bytes_per_sec, event->sample_rtt_ns,
                event->delivered_total_bytes, event->flags);
        }
    }
    tcp_shift_bbr_startup_dumped = 1U;
}

static unsigned tcp_shift_lwip_bbr_startup_trace_requested(void)
{
    const char *value = getenv("TCP_SHIFT_BBR_STARTUP_TRACE");

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0
               ? 1U
               : 0U;
}

static int tcp_shift_lwip_bbr_startup_trace_enable(
    struct tcp_shift_lwip_bbr_binding *binding)
{
    if (binding == NULL || !tcp_shift_lwip_bbr_startup_trace_requested()) {
        return 0;
    }
    if (tcp_shift_bbr_startup_trace_count >= TCP_SHIFT_BBR_STARTUP_TRACE_FLOWS) {
        return -1;
    }

    binding->startup_trace =
        &tcp_shift_bbr_startup_traces[tcp_shift_bbr_startup_trace_count++];
    binding->startup_trace_enabled = 1U;
    return 0;
}

static void tcp_shift_lwip_bbr_trace_append(
    struct tcp_shift_lwip_bbr_binding *binding,
    const struct tcp_shift_bbr_startup_trace_event *event)
{
    if (binding == NULL || event == NULL ||
        binding->startup_trace_enabled == 0U ||
        binding->startup_trace == NULL ||
        binding->startup_trace->count >= TCP_SHIFT_BBR_STARTUP_TRACE_EVENTS) {
        return;
    }
    binding->startup_trace->events[binding->startup_trace->count++] = *event;
}

static void tcp_shift_lwip_bbr_trace_init(
    struct tcp_shift_lwip_bbr_binding *binding,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_policy *policy)
{
    struct tcp_shift_bbr_startup_trace_event event;

    if (binding == NULL || binding->startup_trace_enabled == 0U ||
        transport == NULL || policy == NULL) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.time_ns = tcp_shift_lwip_bbr_now_ns();
    event.flow_id = binding->startup_trace_flow_id;
    event.pacing_rate_bytes_per_sec = policy->pacing_rate_bytes_per_sec;
    event.cwnd_bytes = policy->cwnd_bytes;
    event.inflight_bytes = transport->inflight_bytes;
    event.mode_before = TCP_SHIFT_BBR_MODE_STARTUP;
    event.mode_after = TCP_SHIFT_BBR_MODE_STARTUP;
    event.kind = TCP_SHIFT_BBR_STARTUP_TRACE_INIT;
    tcp_shift_lwip_bbr_trace_append(binding, &event);
}

static void tcp_shift_lwip_bbr_trace_ack(
    struct tcp_shift_lwip_bbr_binding *binding,
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_ack *ack,
    enum tcp_shift_bbr_mode mode_before)
{
    const struct tcp_shift_bbr_model *model;
    struct tcp_shift_bbr_startup_trace_event event;
    uint32_t kind = 0U;

    if (binding == NULL || binding->startup_trace_enabled == 0U ||
        transport == NULL || ack == NULL) {
        return;
    }

    model = &binding->controller.model;
    if (model->round_start != 0U &&
        binding->startup_trace_rounds < TCP_SHIFT_BBR_STARTUP_TRACE_ROUNDS) {
        kind = TCP_SHIFT_BBR_STARTUP_TRACE_ROUND;
        binding->startup_trace_rounds++;
    } else if (mode_before != model->mode) {
        kind = TCP_SHIFT_BBR_STARTUP_TRACE_TRANSITION;
    }
    if (kind == 0U) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.time_ns = ack->ack_time_ns;
    event.flow_id = binding->startup_trace_flow_id;
    event.pacing_rate_bytes_per_sec =
        binding->controller.pacing_rate_bytes_per_sec;
    event.max_bw_bytes_per_sec = model->max_bw_bytes_per_sec;
    event.min_rtt_ns =
        model->has_min_rtt != 0U ? model->min_rtt_ns : 0U;
    event.sample_rate_bytes_per_sec = ack->rate.delivery_rate_bytes_per_sec;
    event.sample_rtt_ns = ack->rate.rtt_ns;
    event.delivered_total_bytes = ack->rate.delivered_total_bytes;
    event.cwnd_bytes = binding->controller.cwnd_bytes;
    event.inflight_bytes = transport->inflight_bytes;
    event.acked_bytes = ack->acked_bytes;
    event.flags = ack->rate.flags;
    event.round = model->round_count;
    event.mode_before = (uint32_t)mode_before;
    event.mode_after = (uint32_t)model->mode;
    event.kind = kind;
    tcp_shift_lwip_bbr_trace_append(binding, &event);
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
    enum tcp_shift_bbr_mode mode_before;
    int result;

    if (binding == NULL) {
        return -1;
    }
    mode_before = binding->controller.model.mode;
    result = tcp_shift_bbr_controller_on_ack(
        &binding->controller, transport, ack, policy);
    if (result == 0) {
        tcp_shift_lwip_bbr_trace_ack(binding, transport, ack, mode_before);
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
        uint32_t entry_acked_bytes = binding->recovery_entry_acked_bytes;
        int result;

        binding->recovery_entry_acked_bytes = 0U;
        result = tcp_shift_bbr_controller_recovery_enter(
            &binding->controller, &post_loss, entry_acked_bytes,
            loss->lost_bytes, policy);
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

static void tcp_shift_lwip_bbr_hook_ack_begin(void *arg,
                                                struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL) {
        return;
    }
    binding->recovery_entry_acked_bytes = 0U;
    if (binding->base_hook_ops != NULL &&
        binding->base_hook_ops->on_ack_begin != NULL) {
        binding->base_hook_ops->on_ack_begin(arg, pcb);
    }
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
    uint64_t delivered_before;
    uint64_t delivered_after;
    uint64_t newly_delivered;
    int handled;

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->on_sack == NULL) {
        return 0;
    }

    delivered_before = adapter->delivered_bytes;
    handled = binding->base_hook_ops->on_sack(
        arg, pcb, ranges, range_count);
    delivered_after = adapter->delivered_bytes;
    if (handled != 0 && delivered_after >= delivered_before) {
        newly_delivered = delivered_after - delivered_before;
        if (newly_delivered > UINT32_MAX -
                                  binding->recovery_entry_acked_bytes) {
            binding->recovery_entry_acked_bytes = UINT32_MAX;
        } else {
            binding->recovery_entry_acked_bytes +=
                (uint32_t)newly_delivered;
        }
    }
    return handled;
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

static void tcp_shift_lwip_bbr_hook_tlp_dupack(
    void *arg,
    struct tcp_pcb *pcb,
    unsigned sack_seen)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding != NULL && binding->base_hook_ops != NULL &&
        binding->base_hook_ops->on_tlp_dupack != NULL) {
        binding->base_hook_ops->on_tlp_dupack(
            arg, pcb, sack_seen);
    }
}

static int tcp_shift_lwip_bbr_hook_tlp_loss(
    void *arg,
    struct tcp_pcb *pcb,
    tcpwnd_size_t lost_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || lost_bytes == 0U ||
        tcp_shift_bbr_controller_note_loss(
            &binding->controller, (uint32_t)lost_bytes) != 0) {
        return 0;
    }

    if (adapter->stats != NULL) {
        adapter->stats->loss_events++;
    }
    tcp_shift_lwip_bbr_record_stats(binding);
    return 1;
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

static int tcp_shift_lwip_bbr_hook_rack_loss_status(
    void *arg,
    struct tcp_pcb *pcb,
    const void *segment,
    uint64_t *remaining_ns)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->rack_loss_status == NULL) {
        if (remaining_ns != NULL) {
            *remaining_ns = 0U;
        }
        return -1;
    }
    return binding->base_hook_ops->rack_loss_status(
        arg, pcb, segment, remaining_ns);
}

static u32_t tcp_shift_lwip_bbr_hook_effective_cwnd(void *arg,
                                                       struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_lwip_bbr_binding *binding =
        tcp_shift_lwip_bbr_binding_from_adapter(adapter, pcb);

    if (binding == NULL || binding->base_hook_ops == NULL ||
        binding->base_hook_ops->effective_cwnd == NULL) {
        return pcb != NULL ? (u32_t)pcb->cwnd : 0U;
    }
    return binding->base_hook_ops->effective_cwnd(arg, pcb);
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
    .on_ack_begin = tcp_shift_lwip_bbr_hook_ack_begin,
    .on_ack = tcp_shift_lwip_bbr_hook_ack,
    .on_ack_observe = tcp_shift_lwip_bbr_hook_ack_observe,
    .on_sack = tcp_shift_lwip_bbr_hook_sack,
    .on_loss = tcp_shift_lwip_bbr_hook_loss,
    .on_tlp_loss = tcp_shift_lwip_bbr_hook_tlp_loss,
    .on_tlp_dupack = tcp_shift_lwip_bbr_hook_tlp_dupack,
    .on_timeout = tcp_shift_lwip_bbr_hook_timeout,
    .on_recovery_exit = tcp_shift_lwip_bbr_hook_recovery_exit,
    .rack_loss_status = tcp_shift_lwip_bbr_hook_rack_loss_status,
    .effective_cwnd = tcp_shift_lwip_bbr_hook_effective_cwnd,
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
    binding->startup_trace_flow_id = adapter->pacing_flow_id;
    if (tcp_shift_lwip_bbr_startup_trace_enable(binding) < 0) {
        free(binding);
        return -1;
    }
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
    tcp_shift_lwip_bbr_trace_init(binding, &transport, &policy);
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
