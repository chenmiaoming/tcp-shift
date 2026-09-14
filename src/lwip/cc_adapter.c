#include "lwip/cc_adapter.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TCP_SHIFT_LWIP_CC_MAX_LISTENERS 2U
#define TCP_SHIFT_DELIVERY_INITIAL_SLOTS 8U

struct tcp_shift_lwip_cc_listener_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    unsigned used;
};

/* P5 keeps delivery metadata outside upstream struct tcp_seg. The existing
 * segment pointer is stable while a sent segment moves between unacked/unsent
 * during fast/RTO retransmission, so it is a sufficient sidecar key. */
struct tcp_shift_delivery_slot {
    const void *segment;
    uint64_t first_tx_ns;
    uint64_t delivered_at_send;
    uint64_t delivered_mstamp_at_send_ns;
};

static struct tcp_shift_lwip_cc_listener_binding
    tcp_shift_lwip_cc_listeners[TCP_SHIFT_LWIP_CC_MAX_LISTENERS];
static struct tcp_shift_lwip_cc_stats tcp_shift_lwip_cc_stats;

static uint32_t tcp_shift_lwip_cc_cwnd_limit(void)
{
#if LWIP_WND_SCALE
    return UINT32_MAX;
#else
    return UINT16_MAX;
#endif
}

/* Keep this calculation mechanically equivalent to pinned lwIP's
 * LWIP_TCP_CALC_INITIAL_CWND(). On passive open, lwIP invokes the raw accept
 * callback while pcb->cwnd is still its allocation sentinel (1 byte), then
 * assigns this initial cwnd immediately after the callback returns. Binding the
 * controller at accept therefore needs the value lwIP is about to publish. */
static uint32_t tcp_shift_lwip_cc_initial_cwnd(uint32_t mss_bytes)
{
    uint32_t twice_mss = 2U * mss_bytes;
    uint32_t four_mss = 4U * mss_bytes;
    uint32_t floor = twice_mss > 4380U ? twice_mss : 4380U;

    return four_mss < floor ? four_mss : floor;
}

static void tcp_shift_lwip_cc_transport_from_pcb(
    const struct tcp_pcb *pcb,
    struct tcp_shift_cc_transport *transport)
{
    transport->mss_bytes = pcb->mss;
    transport->inflight_bytes = pcb->snd_nxt - pcb->lastack;
    transport->send_window_bytes = pcb->snd_wnd;
    transport->cwnd_limit_bytes = tcp_shift_lwip_cc_cwnd_limit();
}

static int tcp_shift_lwip_cc_apply_policy(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_policy *policy)
{
    uint32_t limit = tcp_shift_lwip_cc_cwnd_limit();

    if (adapter == NULL || adapter->pcb == NULL || policy == NULL ||
        policy->cwnd_bytes == 0U || policy->ssthresh_bytes == 0U ||
        policy->cwnd_bytes > limit || policy->ssthresh_bytes > limit) {
        return -1;
    }

    adapter->pcb->cwnd = (tcpwnd_size_t)policy->cwnd_bytes;
    adapter->pcb->ssthresh = (tcpwnd_size_t)policy->ssthresh_bytes;
    adapter->pcb->bytes_acked = 0U;
    if (adapter->stats != NULL) {
        adapter->stats->last_cwnd_bytes = policy->cwnd_bytes;
        adapter->stats->last_ssthresh_bytes = policy->ssthresh_bytes;
    }
    return 0;
}

static uint64_t tcp_shift_delivery_now_ns(struct tcp_shift_lwip_cc_adapter *adapter)
{
    struct timespec now;
    uint64_t value;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        if (adapter != NULL && adapter->stats != NULL) {
            adapter->stats->delivery_clock_errors++;
        }
        return 0U;
    }
    value = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
            (uint64_t)now.tv_nsec;
    if (adapter != NULL) {
        if (adapter->delivery_last_event_ns != 0U &&
            value < adapter->delivery_last_event_ns &&
            adapter->stats != NULL) {
            adapter->stats->delivery_timestamp_regressions++;
        }
        adapter->delivery_last_event_ns = value;
    }
    return value;
}

static struct tcp_shift_delivery_slot *
tcp_shift_delivery_slots(struct tcp_shift_lwip_cc_adapter *adapter)
{
    return (struct tcp_shift_delivery_slot *)adapter->delivery_slots;
}

static struct tcp_shift_delivery_slot *
tcp_shift_delivery_find(struct tcp_shift_lwip_cc_adapter *adapter,
                        const void *segment)
{
    struct tcp_shift_delivery_slot *slots = tcp_shift_delivery_slots(adapter);
    uint16_t index;

    for (index = 0U; index < adapter->delivery_capacity; index++) {
        if (slots[index].segment == segment) {
            return &slots[index];
        }
    }
    return NULL;
}

static struct tcp_shift_delivery_slot *
tcp_shift_delivery_empty_slot(struct tcp_shift_lwip_cc_adapter *adapter)
{
    struct tcp_shift_delivery_slot *slots = tcp_shift_delivery_slots(adapter);
    uint16_t index;

    for (index = 0U; index < adapter->delivery_capacity; index++) {
        if (slots[index].segment == NULL) {
            return &slots[index];
        }
    }
    return NULL;
}

static int tcp_shift_delivery_grow(struct tcp_shift_lwip_cc_adapter *adapter)
{
    struct tcp_shift_delivery_slot *slots;
    uint32_t configured_max = (uint32_t)TCP_SND_QUEUELEN;
    uint32_t next_capacity;
    size_t old_bytes;
    size_t new_bytes;

    if (configured_max > UINT16_MAX) {
        configured_max = UINT16_MAX;
    }
    if ((uint32_t)adapter->delivery_capacity >= configured_max) {
        return -1;
    }

    if (adapter->delivery_capacity == 0U) {
        next_capacity = TCP_SHIFT_DELIVERY_INITIAL_SLOTS;
    } else {
        next_capacity = (uint32_t)adapter->delivery_capacity * 2U;
    }
    if (next_capacity > configured_max) {
        next_capacity = configured_max;
    }
    if (next_capacity == 0U) {
        return -1;
    }

    old_bytes = (size_t)adapter->delivery_capacity *
                sizeof(struct tcp_shift_delivery_slot);
    new_bytes = (size_t)next_capacity *
                sizeof(struct tcp_shift_delivery_slot);
    slots = realloc(adapter->delivery_slots, new_bytes);
    if (slots == NULL) {
        if (adapter->stats != NULL) {
            adapter->stats->delivery_metadata_alloc_failures++;
        }
        return -1;
    }
    memset((unsigned char *)slots + old_bytes, 0, new_bytes - old_bytes);
    adapter->delivery_slots = slots;
    adapter->delivery_capacity = (uint16_t)next_capacity;
    if (adapter->stats != NULL &&
        next_capacity > adapter->stats->delivery_peak_capacity_slots_per_flow) {
        adapter->stats->delivery_peak_capacity_slots_per_flow = next_capacity;
    }
    return 0;
}

static struct tcp_shift_delivery_slot *
tcp_shift_delivery_create(struct tcp_shift_lwip_cc_adapter *adapter,
                          const void *segment)
{
    struct tcp_shift_delivery_slot *slot =
        tcp_shift_delivery_empty_slot(adapter);

    if (slot == NULL) {
        if (tcp_shift_delivery_grow(adapter) < 0) {
            return NULL;
        }
        slot = tcp_shift_delivery_empty_slot(adapter);
    }
    if (slot == NULL) {
        if (adapter->stats != NULL) {
            adapter->stats->delivery_metadata_alloc_failures++;
        }
        return NULL;
    }

    memset(slot, 0, sizeof(*slot));
    slot->segment = segment;
    adapter->delivery_live++;
    if (adapter->stats != NULL) {
        adapter->stats->delivery_live_slots++;
        if (adapter->stats->delivery_live_slots >
            adapter->stats->delivery_peak_live_slots) {
            adapter->stats->delivery_peak_live_slots =
                adapter->stats->delivery_live_slots;
        }
        if ((uint32_t)adapter->delivery_live >
            adapter->stats->delivery_peak_slots_per_flow) {
            adapter->stats->delivery_peak_slots_per_flow =
                adapter->delivery_live;
        }
    }
    return slot;
}

static void tcp_shift_delivery_release_slot(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_shift_delivery_slot *slot)
{
    if (adapter == NULL || slot == NULL || slot->segment == NULL) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    if (adapter->delivery_live != 0U) {
        adapter->delivery_live--;
    }
    if (adapter->stats != NULL && adapter->stats->delivery_live_slots != 0U) {
        adapter->stats->delivery_live_slots--;
    }
}

static void tcp_shift_delivery_release_all(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    uint32_t live;

    if (adapter == NULL) {
        return;
    }
    live = adapter->delivery_live;
    if (adapter->stats != NULL && live != 0U) {
        adapter->stats->delivery_metadata_abandoned_slots += live;
        if (adapter->stats->delivery_live_slots >= live) {
            adapter->stats->delivery_live_slots -= live;
        } else {
            adapter->stats->delivery_live_slots = 0U;
        }
    }
    free(adapter->delivery_slots);
    adapter->delivery_slots = NULL;
    adapter->delivery_capacity = 0U;
    adapter->delivery_live = 0U;
}

static void tcp_shift_lwip_cc_on_segment_tx(void *arg,
                                             struct tcp_pcb *pcb,
                                             const void *segment,
                                             u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_delivery_slot *slot;
    uint64_t now_ns;

    if (adapter == NULL || adapter->pcb != pcb || segment == NULL ||
        payload_bytes == 0U) {
        return;
    }

    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return;
    }
    if (adapter->stats != NULL) {
        adapter->stats->delivery_last_tx_ns = now_ns;
    }

    slot = tcp_shift_delivery_find(adapter, segment);
    if (slot != NULL) {
        if (adapter->stats != NULL) {
            adapter->stats->delivery_retransmit_events++;
        }
        return;
    }

    slot = tcp_shift_delivery_create(adapter, segment);
    if (slot == NULL) {
        return;
    }
    slot->first_tx_ns = now_ns;
    slot->delivered_at_send = adapter->delivered_bytes;
    slot->delivered_mstamp_at_send_ns =
        adapter->delivered_mstamp_ns != 0U ? adapter->delivered_mstamp_ns
                                           : now_ns;
    if (adapter->stats != NULL) {
        adapter->stats->delivery_first_tx_events++;
    }
}

static void tcp_shift_lwip_cc_on_segment_acked(void *arg,
                                                struct tcp_pcb *pcb,
                                                const void *segment,
                                                u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_delivery_slot *slot;
    uint64_t now_ns;

    if (adapter == NULL || adapter->pcb != pcb || segment == NULL ||
        payload_bytes == 0U) {
        return;
    }

    if (adapter->stats != NULL) {
        adapter->stats->delivery_acked_segment_events++;
    }
    slot = tcp_shift_delivery_find(adapter, segment);
    if (slot == NULL) {
        if (adapter->stats != NULL) {
            adapter->stats->delivery_metadata_misses++;
        }
        return;
    }

    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return;
    }
    if (slot->first_tx_ns == 0U || now_ns < slot->first_tx_ns) {
        if (adapter->stats != NULL) {
            adapter->stats->delivery_timestamp_regressions++;
        }
    }

    adapter->delivered_bytes += payload_bytes;
    adapter->delivered_mstamp_ns = now_ns;
    if (adapter->stats != NULL) {
        adapter->stats->delivery_payload_bytes += payload_bytes;
        adapter->stats->delivery_last_ack_ns = now_ns;
    }
    tcp_shift_delivery_release_slot(adapter, slot);
}

static void tcp_shift_lwip_cc_disable_on_error(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL) {
        return;
    }
    if (adapter->stats != NULL) {
        adapter->stats->controller_errors++;
    }
    /* Keep the ext-arg attached until PCB destruction so heap-owned adapter
     * storage and delivery metadata are still released. bound=0 makes later
     * policy calls fall back to native lwIP rather than stale controller state. */
    adapter->bound = 0U;
}

static int tcp_shift_lwip_cc_on_ack(void *arg,
                                    struct tcp_pcb *pcb,
                                    tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        acked_bytes == 0U) {
        return 0;
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    ack.acked_bytes = acked_bytes;
    if (tcp_shift_cc_on_ack(&adapter->controller, &transport, &ack,
                            &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        tcp_shift_lwip_cc_disable_on_error(adapter);
        return 0;
    }

    if (adapter->stats != NULL) {
        adapter->stats->ack_events++;
        adapter->stats->policy_updates++;
    }
    return 1;
}

static int tcp_shift_lwip_cc_on_loss(void *arg,
                                     struct tcp_pcb *pcb,
                                     tcpwnd_size_t lost_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_loss loss;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        lost_bytes == 0U) {
        return 0;
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    loss.lost_bytes = lost_bytes;
    if (tcp_shift_cc_on_loss(&adapter->controller, &transport, &loss,
                             &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        tcp_shift_lwip_cc_disable_on_error(adapter);
        return 0;
    }

    if (adapter->stats != NULL) {
        adapter->stats->loss_events++;
        adapter->stats->policy_updates++;
    }
    return 1;
}

static int tcp_shift_lwip_cc_on_timeout(void *arg, struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb) {
        return 0;
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    if (tcp_shift_cc_on_timeout(&adapter->controller, &transport, &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        tcp_shift_lwip_cc_disable_on_error(adapter);
        return 0;
    }

    if (adapter->stats != NULL) {
        adapter->stats->timeout_events++;
        adapter->stats->policy_updates++;
    }
    return 1;
}

static const struct tcp_shift_lwip_cc_hook_ops tcp_shift_lwip_cc_hook_ops = {
    .on_ack = tcp_shift_lwip_cc_on_ack,
    .on_loss = tcp_shift_lwip_cc_on_loss,
    .on_timeout = tcp_shift_lwip_cc_on_timeout,
    .on_segment_tx = tcp_shift_lwip_cc_on_segment_tx,
    .on_segment_acked = tcp_shift_lwip_cc_on_segment_acked,
};

static void tcp_shift_lwip_cc_pcb_destroyed(u8_t id, void *data)
{
    struct tcp_shift_lwip_cc_hook *hook = data;
    struct tcp_shift_lwip_cc_adapter *adapter;
    unsigned heap_owned;

    (void)id;
    if (hook == NULL) {
        return;
    }
    adapter = hook->arg;
    if (adapter == NULL) {
        return;
    }

    heap_owned = adapter->heap_owned;
    tcp_shift_delivery_release_all(adapter);
    adapter->bound = 0U;
    adapter->pcb = NULL;
    if (heap_owned != 0U) {
        free(adapter);
    }
}

static const struct tcp_ext_arg_callbacks tcp_shift_lwip_cc_ext_callbacks = {
    .destroy = tcp_shift_lwip_cc_pcb_destroyed,
    .passive_open = NULL,
};

int tcp_shift_lwip_cc_adapter_bind(struct tcp_shift_lwip_cc_adapter *adapter,
                                   struct tcp_pcb *pcb,
                                   struct tcp_shift_lwip_cc_stats *stats)
{
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_init init;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || pcb == NULL || pcb->mss == 0U ||
        adapter->pcb != NULL ||
        tcp_ext_arg_get(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID) != NULL) {
        return -1;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->hook.ops = &tcp_shift_lwip_cc_hook_ops;
    adapter->hook.arg = adapter;
    adapter->stats = stats;
    adapter->pcb = pcb;
    adapter->delivered_mstamp_ns = tcp_shift_delivery_now_ns(adapter);
    if (stats != NULL) {
        stats->delivery_metadata_bytes_per_slot =
            (uint32_t)sizeof(struct tcp_shift_delivery_slot);
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
    init.initial_cwnd_bytes = pcb->cwnd;
    if (init.initial_cwnd_bytes < pcb->mss && pcb->state == ESTABLISHED) {
        init.initial_cwnd_bytes = tcp_shift_lwip_cc_initial_cwnd(pcb->mss);
    }
    init.initial_ssthresh_bytes = pcb->ssthresh;
    init.min_cwnd_bytes = pcb->mss;
    if (tcp_shift_cc_init(&adapter->controller, &tcp_shift_reno_ops,
                          &adapter->reno, sizeof(adapter->reno), &transport,
                          &init, &policy) != 0 ||
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0) {
        adapter->pcb = NULL;
        adapter->stats = NULL;
        return -1;
    }

    tcp_ext_arg_set_callbacks(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                              &tcp_shift_lwip_cc_ext_callbacks);
    tcp_ext_arg_set(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID, &adapter->hook);
    adapter->bound = 1U;
    if (stats != NULL) {
        stats->bindings++;
    }
    return 0;
}

void tcp_shift_lwip_cc_adapter_unbind(struct tcp_shift_lwip_cc_adapter *adapter)
{
    if (adapter == NULL || adapter->pcb == NULL) {
        return;
    }

    if (tcp_ext_arg_get(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID) ==
        &adapter->hook) {
        tcp_ext_arg_set_callbacks(adapter->pcb,
                                  (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID, NULL);
        tcp_ext_arg_set(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                        NULL);
    }
    tcp_shift_delivery_release_all(adapter);
    adapter->bound = 0U;
    adapter->pcb = NULL;
}

static void tcp_shift_lwip_cc_listener_destroyed(u8_t id, void *data)
{
    struct tcp_shift_lwip_cc_listener_binding *binding = data;

    (void)id;
    if (binding != NULL) {
        memset(binding, 0, sizeof(*binding));
    }
}

static const struct tcp_ext_arg_callbacks tcp_shift_lwip_cc_listener_callbacks = {
    .destroy = tcp_shift_lwip_cc_listener_destroyed,
    .passive_open = NULL,
};

static err_t tcp_shift_lwip_cc_accept_dispatch(void *arg,
                                                struct tcp_pcb *newpcb,
                                                err_t err)
{
    struct tcp_shift_lwip_cc_listener_binding *binding = arg;
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

    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        tcp_shift_lwip_cc_stats.bind_failures++;
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    if (tcp_shift_lwip_cc_adapter_bind(adapter, newpcb,
                                       &tcp_shift_lwip_cc_stats) < 0) {
        tcp_shift_lwip_cc_stats.bind_failures++;
        free(adapter);
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    adapter->heap_owned = 1U;

    return binding->accept(binding->callback_arg, newpcb, err);
}

void tcp_shift_lwip_cc_accept(struct tcp_pcb *pcb, tcp_accept_fn accept)
{
    struct tcp_shift_lwip_cc_listener_binding *binding = NULL;
    unsigned i;

    if (pcb == NULL || pcb->state != LISTEN) {
        tcp_accept(pcb, accept);
        return;
    }

    for (i = 0U; i < TCP_SHIFT_LWIP_CC_MAX_LISTENERS; i++) {
        if (tcp_shift_lwip_cc_listeners[i].used == 0U) {
            binding = &tcp_shift_lwip_cc_listeners[i];
            break;
        }
    }
    if (binding == NULL) {
        tcp_shift_lwip_cc_stats.bind_failures++;
        tcp_accept(pcb, NULL);
        return;
    }

    binding->listener = pcb;
    binding->accept = accept;
    binding->callback_arg = pcb->callback_arg;
    binding->used = 1U;

    tcp_arg(pcb, binding);
    tcp_ext_arg_set_callbacks(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                              &tcp_shift_lwip_cc_listener_callbacks);
    tcp_ext_arg_set(pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID, binding);
    tcp_accept(pcb, tcp_shift_lwip_cc_accept_dispatch);
}

const struct tcp_shift_lwip_cc_stats *tcp_shift_lwip_cc_get_stats(void)
{
    return &tcp_shift_lwip_cc_stats;
}
