#include "lwip/cc_adapter.h"
#include "runtime/pacer.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TCP_SHIFT_LWIP_CC_MAX_LISTENERS 2U
#define TCP_SHIFT_DELIVERY_INITIAL_SLOTS 8U
#define TCP_SHIFT_PACING_INITIAL_REGISTRY 32U
#define TCP_SHIFT_NSEC_PER_SEC UINT64_C(1000000000)

struct tcp_shift_lwip_cc_listener_binding {
    struct tcp_pcb *listener;
    tcp_accept_fn accept;
    void *callback_arg;
    unsigned used;
};

/* P5 keeps delivery metadata outside upstream struct tcp_seg. The existing
 * segment pointer is stable while a sent segment moves between unacked/unsent
 * during fast/RTO retransmission. Sequence/payload progress is copied into the
 * sidecar on first transmission so partial ACK accounting never dereferences
 * the private segment layout. */
struct tcp_shift_delivery_slot {
    const void *segment;
    uint64_t first_tx_ns;
    uint64_t first_tx_mstamp_at_send_ns;
    uint64_t delivered_at_send;
    uint64_t delivered_mstamp_at_send_ns;
    uint32_t seq_start;
    uint32_t prior_inflight_bytes;
    uint16_t payload_bytes;
    uint16_t acked_payload_bytes;
    uint8_t app_limited;
    uint8_t retransmitted;
};

struct tcp_shift_pacing_registry_entry {
    struct tcp_shift_lwip_cc_adapter *adapter;
    uint32_t generation;
};

struct tcp_shift_pacing_service {
    const struct tcp_shift_lwip_cc_pacer_ops *ops;
    void *arg;
    struct tcp_shift_pacing_registry_entry *entries;
    size_t capacity;
    size_t active;
};

static struct tcp_shift_lwip_cc_listener_binding
    tcp_shift_lwip_cc_listeners[TCP_SHIFT_LWIP_CC_MAX_LISTENERS];
static struct tcp_shift_lwip_cc_stats tcp_shift_lwip_cc_stats;
static struct tcp_shift_pacing_service tcp_shift_pacing_service;

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

static uint64_t tcp_shift_delivery_now_ns(struct tcp_shift_lwip_cc_adapter *adapter)
{
    struct timespec now;
    uint64_t value;

    if (adapter != NULL) {
        adapter->delivery_last_clock_read_ns = 0U;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        if (adapter != NULL && adapter->stats != NULL) {
            adapter->stats->delivery_clock_errors++;
        }
        return 0U;
    }
    value = (uint64_t)now.tv_sec * TCP_SHIFT_NSEC_PER_SEC +
            (uint64_t)now.tv_nsec;
    if (adapter != NULL) {
        adapter->delivery_last_clock_read_ns = value;
        if (adapter->delivery_last_event_ns != 0U &&
            value < adapter->delivery_last_event_ns &&
            adapter->stats != NULL) {
            adapter->stats->delivery_timestamp_regressions++;
        }
        adapter->delivery_last_event_ns = value;
    }
    return value;
}

static void tcp_shift_pacing_cancel(struct tcp_shift_lwip_cc_adapter *adapter)
{
    size_t cancelled = 0U;

    if (adapter == NULL || adapter->pacing_scheduled == 0U) {
        return;
    }
    if (tcp_shift_pacing_service.ops == NULL ||
        tcp_shift_pacing_service.ops->cancel == NULL ||
        adapter->pacing_flow_id == 0U ||
        tcp_shift_pacing_service.ops->cancel(tcp_shift_pacing_service.arg,
                                             adapter->pacing_flow_id,
                                             adapter->pacing_generation,
                                             &cancelled) < 0) {
        if (adapter->stats != NULL) {
            adapter->stats->pacing_scheduler_errors++;
        }
    }
    adapter->pacing_scheduled = 0U;
}

static int tcp_shift_pacing_registry_grow(void)
{
    struct tcp_shift_pacing_registry_entry *entries;
    size_t next_capacity;
    size_t old_bytes;
    size_t new_bytes;

    if (tcp_shift_pacing_service.capacity == 0U) {
        next_capacity = TCP_SHIFT_PACING_INITIAL_REGISTRY;
    } else {
        if (tcp_shift_pacing_service.capacity > SIZE_MAX / 2U) {
            return -1;
        }
        next_capacity = tcp_shift_pacing_service.capacity * 2U;
    }
    if (next_capacity > SIZE_MAX / sizeof(*entries)) {
        return -1;
    }

    old_bytes = tcp_shift_pacing_service.capacity * sizeof(*entries);
    new_bytes = next_capacity * sizeof(*entries);
    entries = realloc(tcp_shift_pacing_service.entries, new_bytes);
    if (entries == NULL) {
        return -1;
    }
    memset((unsigned char *)entries + old_bytes, 0, new_bytes - old_bytes);
    tcp_shift_pacing_service.entries = entries;
    tcp_shift_pacing_service.capacity = next_capacity;
    return 0;
}

static int tcp_shift_pacing_register(struct tcp_shift_lwip_cc_adapter *adapter)
{
    size_t index;
    uint32_t generation;

    if (adapter == NULL) {
        return -1;
    }
    if (tcp_shift_pacing_service.ops == NULL) {
        return 0;
    }

    for (;;) {
        for (index = 0U; index < tcp_shift_pacing_service.capacity; index++) {
            if (tcp_shift_pacing_service.entries[index].adapter == NULL) {
                generation = tcp_shift_pacing_service.entries[index].generation + 1U;
                if (generation == 0U) {
                    generation = 1U;
                }
                tcp_shift_pacing_service.entries[index].generation = generation;
                tcp_shift_pacing_service.entries[index].adapter = adapter;
                adapter->pacing_flow_id = (uint64_t)index + 1U;
                adapter->pacing_generation = generation;
                tcp_shift_pacing_service.active++;
                return 0;
            }
        }
        if (tcp_shift_pacing_registry_grow() < 0) {
            return -1;
        }
    }
}

static void tcp_shift_pacing_unregister(struct tcp_shift_lwip_cc_adapter *adapter)
{
    size_t index;
    struct tcp_shift_pacing_registry_entry *entry;

    if (adapter == NULL || adapter->pacing_flow_id == 0U) {
        return;
    }

    tcp_shift_pacing_cancel(adapter);
    index = (size_t)(adapter->pacing_flow_id - 1U);
    if (index < tcp_shift_pacing_service.capacity) {
        entry = &tcp_shift_pacing_service.entries[index];
        if (entry->adapter == adapter &&
            entry->generation == adapter->pacing_generation) {
            entry->adapter = NULL;
            if (tcp_shift_pacing_service.active != 0U) {
                tcp_shift_pacing_service.active--;
            }
        }
    }
    adapter->pacing_flow_id = 0U;
    adapter->pacing_generation = 0U;
}

int tcp_shift_lwip_cc_configure_pacer(
    const struct tcp_shift_lwip_cc_pacer_ops *ops,
    void *arg)
{
    if (ops == NULL || ops->schedule == NULL || ops->cancel == NULL) {
        return -1;
    }
    if (tcp_shift_pacing_service.ops != NULL) {
        return tcp_shift_pacing_service.ops == ops &&
                       tcp_shift_pacing_service.arg == arg
                   ? 0
                   : -1;
    }
    if (tcp_shift_pacing_service.active != 0U) {
        return -1;
    }
    tcp_shift_pacing_service.ops = ops;
    tcp_shift_pacing_service.arg = arg;
    return 0;
}

int tcp_shift_lwip_cc_clear_pacer(void)
{
    if (tcp_shift_pacing_service.active != 0U) {
        return -1;
    }
    tcp_shift_pacing_service.ops = NULL;
    tcp_shift_pacing_service.arg = NULL;
    return 0;
}

static void tcp_shift_pacing_note_tx(struct tcp_shift_lwip_cc_adapter *adapter,
                                     uint16_t payload_bytes,
                                     uint64_t now_ns)
{
    struct tcp_shift_flow_pacer flow;

    if (adapter == NULL || payload_bytes == 0U ||
        adapter->pacing_rate_bytes_per_sec == 0U || now_ns == 0U) {
        return;
    }

    /* The runtime flow clock is the source of truth for byte/rate -> deadline
     * conversion. Keep zero catch-up credit for lwIP v1: if the event loop is
     * late, release the due segment and schedule the next one in the future
     * rather than trying to repay elapsed pacing credit as a burst. The scalar
     * fields remain in the adapter ABI for now; they mirror the reusable flow
     * clock until the next state-layout cleanup. */
    tcp_shift_flow_pacer_init(&flow, 0U);
    tcp_shift_flow_pacer_set_rate(&flow, adapter->pacing_rate_bytes_per_sec);
    flow.next_send_ns = adapter->pacing_next_send_ns;
    if (tcp_shift_flow_pacer_note_tx(&flow, now_ns, payload_bytes) < 0) {
        if (adapter->stats != NULL) {
            adapter->stats->pacing_scheduler_errors++;
        }
        adapter->pacing_rate_bytes_per_sec = 0U;
        adapter->pacing_next_send_ns = 0U;
        return;
    }
    adapter->pacing_next_send_ns = flow.next_send_ns;

    if (adapter->stats != NULL) {
        adapter->stats->pacing_tx_events++;
        adapter->stats->pacing_tx_bytes += payload_bytes;
        adapter->stats->pacing_last_rate_bytes_per_sec =
            adapter->pacing_rate_bytes_per_sec;
        adapter->stats->pacing_last_deadline_ns =
            adapter->pacing_next_send_ns;
    }
}

static int tcp_shift_lwip_cc_on_segment_send_eligible(void *arg,
                                                       struct tcp_pcb *pcb,
                                                       u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_flow_pacer flow;
    uint64_t deadline_ns;
    uint64_t now_ns;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        payload_bytes == 0U || adapter->pacing_rate_bytes_per_sec == 0U) {
        return 1;
    }

    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return 1;
    }

    tcp_shift_flow_pacer_init(&flow, 0U);
    tcp_shift_flow_pacer_set_rate(&flow, adapter->pacing_rate_bytes_per_sec);
    flow.next_send_ns = adapter->pacing_next_send_ns;
    deadline_ns = tcp_shift_flow_pacer_deadline(&flow, now_ns);
    if (deadline_ns == 0U) {
        return 1;
    }

    if (tcp_shift_pacing_service.ops == NULL ||
        tcp_shift_pacing_service.ops->schedule == NULL ||
        adapter->pacing_flow_id == 0U) {
        if (adapter->stats != NULL) {
            adapter->stats->pacing_scheduler_errors++;
        }
        adapter->pacing_rate_bytes_per_sec = 0U;
        adapter->pacing_next_send_ns = 0U;
        return 1;
    }

    if (adapter->pacing_scheduled == 0U) {
        if (tcp_shift_pacing_service.ops->schedule(
                tcp_shift_pacing_service.arg,
                adapter->pacing_flow_id,
                adapter->pacing_generation,
                deadline_ns,
                payload_bytes) < 0) {
            if (adapter->stats != NULL) {
                adapter->stats->pacing_scheduler_errors++;
            }
            adapter->pacing_rate_bytes_per_sec = 0U;
            adapter->pacing_next_send_ns = 0U;
            return 1;
        }
        adapter->pacing_scheduled = 1U;
    }
    if (adapter->stats != NULL) {
        adapter->stats->pacing_deferrals++;
        adapter->stats->pacing_last_deadline_ns = deadline_ns;
    }
    return 0;
}

int tcp_shift_lwip_cc_resume_paced(uint64_t flow_id,
                                   uint32_t generation,
                                   uint64_t actual_release_ns)
{
    struct tcp_shift_pacing_registry_entry *entry;
    struct tcp_shift_lwip_cc_adapter *adapter;
    size_t index;
    err_t err;

    if (flow_id == 0U || flow_id > tcp_shift_pacing_service.capacity) {
        tcp_shift_lwip_cc_stats.pacing_stale_releases++;
        return 0;
    }
    index = (size_t)(flow_id - 1U);
    entry = &tcp_shift_pacing_service.entries[index];
    if (entry->adapter == NULL || entry->generation != generation) {
        tcp_shift_lwip_cc_stats.pacing_stale_releases++;
        return 0;
    }

    adapter = entry->adapter;
    adapter->pacing_scheduled = 0U;
    if (adapter->bound == 0U || adapter->pcb == NULL) {
        if (adapter->stats != NULL) {
            adapter->stats->pacing_stale_releases++;
        }
        return 0;
    }
    if (adapter->stats != NULL) {
        adapter->stats->pacing_resume_events++;
        adapter->stats->pacing_last_actual_release_ns = actual_release_ns;
    }

    err = tcp_output(adapter->pcb);
    if (err != ERR_OK && adapter->stats != NULL) {
        adapter->stats->pacing_scheduler_errors++;
    }
    return 0;
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

    if (policy->pacing_rate_bytes_per_sec == 0U &&
        adapter->pacing_rate_bytes_per_sec != 0U) {
        tcp_shift_pacing_cancel(adapter);
        adapter->pacing_next_send_ns = 0U;
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

static uint32_t tcp_shift_delivery_outstanding_payload(
    const struct tcp_shift_lwip_cc_adapter *adapter)
{
    const struct tcp_shift_delivery_slot *slots =
        (const struct tcp_shift_delivery_slot *)adapter->delivery_slots;
    uint64_t total = 0U;
    uint16_t index;

    for (index = 0U; index < adapter->delivery_capacity; index++) {
        if (slots[index].segment != NULL &&
            slots[index].payload_bytes > slots[index].acked_payload_bytes) {
            total += (uint32_t)(slots[index].payload_bytes -
                                slots[index].acked_payload_bytes);
        }
    }
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

/* Outstanding windows are far below 2^31 bytes in the constrained profile,
 * so signed modular distance gives a wrap-safe position of ack_seq relative to
 * this slot's first payload byte. */
static uint16_t tcp_shift_delivery_acked_payload(
    const struct tcp_shift_delivery_slot *slot,
    uint32_t ack_seq)
{
    int32_t distance = (int32_t)(ack_seq - slot->seq_start);

    if (distance <= 0) {
        return 0U;
    }
    if ((uint32_t)distance >= slot->payload_bytes) {
        return slot->payload_bytes;
    }
    return (uint16_t)distance;
}

static void tcp_shift_lwip_cc_on_segment_tx(void *arg,
                                             struct tcp_pcb *pcb,
                                             const void *segment,
                                             u32_t seq_start,
                                             u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_delivery_slot *slot;
    uint32_t prior_inflight;
    uint64_t now_ns;
    unsigned start_of_flight;

    if (adapter == NULL || adapter->pcb != pcb || segment == NULL ||
        payload_bytes == 0U) {
        return;
    }

    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return;
    }
    tcp_shift_pacing_note_tx(adapter, payload_bytes, now_ns);
    if (adapter->stats != NULL) {
        adapter->stats->delivery_last_tx_ns = now_ns;
    }

    slot = tcp_shift_delivery_find(adapter, segment);
    if (slot != NULL) {
        slot->retransmitted = 1U;
        if (adapter->stats != NULL) {
            adapter->stats->delivery_retransmit_events++;
        }
        return;
    }

    start_of_flight = adapter->delivery_live == 0U;
    if (start_of_flight != 0U) {
        adapter->rate_first_tx_mstamp_ns = now_ns;
        adapter->delivered_mstamp_ns = now_ns;
    }

    slot = tcp_shift_delivery_create(adapter, segment);
    if (slot == NULL) {
        return;
    }

    prior_inflight = pcb->snd_nxt - pcb->lastack;
    if (UINT32_MAX - prior_inflight < payload_bytes) {
        prior_inflight = UINT32_MAX;
    } else {
        prior_inflight += payload_bytes;
    }

    slot->first_tx_ns = now_ns;
    slot->first_tx_mstamp_at_send_ns =
        adapter->rate_first_tx_mstamp_ns != 0U
            ? adapter->rate_first_tx_mstamp_ns
            : now_ns;
    slot->delivered_at_send = adapter->delivered_bytes;
    slot->delivered_mstamp_at_send_ns =
        adapter->delivered_mstamp_ns != 0U ? adapter->delivered_mstamp_ns
                                           : now_ns;
    slot->seq_start = seq_start;
    slot->prior_inflight_bytes = prior_inflight;
    slot->payload_bytes = payload_bytes;
    slot->app_limited = adapter->app_limited_until_bytes != 0U;
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
            adapter->stats->delivery_metadata_missing_slots++;
        }
        return;
    }

    /* The ACK policy hook runs before upstream frees acknowledged segments.
     * A segment reaching this callback must therefore already have had its
     * complete payload charged exactly once by the ACK-range accounting. */
    if (slot->payload_bytes != payload_bytes ||
        slot->acked_payload_bytes != slot->payload_bytes) {
        if (adapter->stats != NULL) {
            adapter->stats->delivery_metadata_misses++;
            adapter->stats->delivery_metadata_incomplete_ack_slots++;
        }
    }
    tcp_shift_delivery_release_slot(adapter, slot);
}

static uint64_t tcp_shift_rate_bytes_per_second(uint32_t delivered_bytes,
                                                 uint64_t interval_ns)
{
    if (delivered_bytes == 0U || interval_ns == 0U) {
        return 0U;
    }
    return ((uint64_t)delivered_bytes * TCP_SHIFT_NSEC_PER_SEC) / interval_ns;
}

static void tcp_shift_rate_record_stats(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_cc_rate_sample *rate)
{
    struct tcp_shift_lwip_cc_stats *stats = adapter->stats;

    if (stats == NULL) {
        return;
    }
    stats->rate_samples++;
    if ((rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) != 0U) {
        stats->rate_valid_samples++;
    } else {
        stats->rate_invalid_samples++;
    }
    if ((rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED) != 0U) {
        stats->rate_app_limited_samples++;
    }
    if ((rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED) != 0U) {
        stats->rate_retransmitted_samples++;
    }
    if (rate->delivered_total_bytes != 0U) {
        stats->rate_snapshot_samples++;
        if (rate->delivered_total_bytes <= rate->prior_delivered_bytes) {
            stats->rate_snapshot_errors++;
        }
    }
    stats->rate_last_bytes_per_sec = rate->delivery_rate_bytes_per_sec;
    if (rate->delivery_rate_bytes_per_sec > stats->rate_max_bytes_per_sec) {
        stats->rate_max_bytes_per_sec = rate->delivery_rate_bytes_per_sec;
    }
    stats->rate_last_interval_ns = rate->interval_ns;
    stats->rate_last_send_interval_ns = rate->send_interval_ns;
    stats->rate_last_ack_interval_ns = rate->ack_interval_ns;
    stats->rate_last_rtt_ns = rate->rtt_ns;
    stats->rate_last_prior_delivered_bytes = rate->prior_delivered_bytes;
    stats->rate_last_delivered_total_bytes = rate->delivered_total_bytes;
    stats->rate_last_delivered_bytes = rate->delivered_bytes;
    stats->rate_last_prior_inflight_bytes = rate->prior_inflight_bytes;
    stats->rate_last_flags = rate->flags;
}

static void tcp_shift_delivery_build_rate_sample(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    struct tcp_shift_cc_rate_sample *rate)
{
    struct tcp_shift_delivery_slot *slots = tcp_shift_delivery_slots(adapter);
    struct tcp_shift_delivery_slot *candidate = NULL;
    uint64_t delivered_delta64;
    uint64_t delivered_added = 0U;
    uint64_t now_ns;
    uint16_t touched = 0U;
    uint16_t index;
    unsigned partial = 0U;

    memset(rate, 0, sizeof(*rate));
    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return;
    }

    for (index = 0U; index < adapter->delivery_capacity; index++) {
        struct tcp_shift_delivery_slot *slot = &slots[index];
        uint16_t acked_payload;
        uint16_t newly_acked;

        if (slot->segment == NULL) {
            continue;
        }
        acked_payload = tcp_shift_delivery_acked_payload(slot, pcb->lastack);
        if (acked_payload <= slot->acked_payload_bytes) {
            continue;
        }

        newly_acked = (uint16_t)(acked_payload - slot->acked_payload_bytes);
        slot->acked_payload_bytes = acked_payload;
        delivered_added += newly_acked;
        touched++;
        if (acked_payload < slot->payload_bytes) {
            partial = 1U;
        }

        if (candidate == NULL ||
            slot->delivered_at_send > candidate->delivered_at_send ||
            (slot->delivered_at_send == candidate->delivered_at_send &&
             slot->delivered_mstamp_at_send_ns >
                 candidate->delivered_mstamp_at_send_ns)) {
            candidate = slot;
        }
    }

    if (delivered_added == 0U) {
        return;
    }
    if (adapter->stats != NULL) {
        if (partial != 0U) {
            adapter->stats->rate_partial_ack_events++;
        }
        if (touched > 1U) {
            adapter->stats->rate_multi_segment_ack_events++;
        }
    }

    adapter->delivered_bytes += delivered_added;
    adapter->delivered_mstamp_ns = now_ns;
    if (adapter->stats != NULL) {
        adapter->stats->delivery_payload_bytes += delivered_added;
        adapter->stats->delivery_last_ack_ns = now_ns;
    }

    if (adapter->app_limited_until_bytes != 0U &&
        adapter->delivered_bytes > adapter->app_limited_until_bytes) {
        adapter->app_limited_until_bytes = 0U;
        if (adapter->stats != NULL) {
            adapter->stats->app_limited_exits++;
        }
    }

    if (candidate == NULL) {
        tcp_shift_rate_record_stats(adapter, rate);
        return;
    }

    delivered_delta64 = adapter->delivered_bytes - candidate->delivered_at_send;
    rate->prior_delivered_bytes = candidate->delivered_at_send;
    rate->delivered_total_bytes = adapter->delivered_bytes;
    rate->delivered_bytes = delivered_delta64 > UINT32_MAX
                                ? UINT32_MAX
                                : (uint32_t)delivered_delta64;
    rate->prior_inflight_bytes = candidate->prior_inflight_bytes;
    if (adapter->rate_first_tx_mstamp_ns >=
        candidate->first_tx_mstamp_at_send_ns) {
        rate->send_interval_ns = adapter->rate_first_tx_mstamp_ns -
                                 candidate->first_tx_mstamp_at_send_ns;
    }
    if (now_ns >= candidate->delivered_mstamp_at_send_ns) {
        rate->ack_interval_ns =
            now_ns - candidate->delivered_mstamp_at_send_ns;
    }
    rate->interval_ns = rate->send_interval_ns > rate->ack_interval_ns
                            ? rate->send_interval_ns
                            : rate->ack_interval_ns;

    if (candidate->app_limited != 0U) {
        rate->flags |= TCP_SHIFT_CC_RATE_SAMPLE_APP_LIMITED;
    }
    if (candidate->retransmitted != 0U) {
        rate->flags |= TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED;
    } else if (candidate->first_tx_ns != 0U && now_ns >= candidate->first_tx_ns) {
        rate->rtt_ns = now_ns - candidate->first_tx_ns;
        rate->flags |= TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID;
    }

    rate->delivery_rate_bytes_per_sec =
        tcp_shift_rate_bytes_per_second(rate->delivered_bytes,
                                        rate->interval_ns);
    if (rate->delivered_bytes != 0U && rate->interval_ns != 0U &&
        rate->delivery_rate_bytes_per_sec != 0U) {
        rate->flags |= TCP_SHIFT_CC_RATE_SAMPLE_VALID;
    }
    tcp_shift_rate_record_stats(adapter, rate);
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
    tcp_shift_pacing_cancel(adapter);
    adapter->pacing_rate_bytes_per_sec = 0U;
    adapter->pacing_next_send_ns = 0U;
    /* Keep the ext-arg attached until PCB destruction so heap-owned adapter
     * storage and delivery metadata are still released. bound=0 makes later
     * policy calls fall back to native lwIP rather than stale controller state. */
    adapter->bound = 0U;
}

static int tcp_shift_lwip_cc_prepare_ack(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    tcpwnd_size_t acked_bytes,
    struct tcp_shift_cc_ack *ack)
{
    uint64_t ack_time_ns;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        acked_bytes == 0U || ack == NULL) {
        return 0;
    }

    memset(ack, 0, sizeof(*ack));
    tcp_shift_delivery_build_rate_sample(adapter, pcb, &ack->rate);
    ack_time_ns = adapter->delivery_last_clock_read_ns;
    ack->acked_bytes = acked_bytes;
    ack->ack_time_ns = ack_time_ns;

    if ((ack->rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) != 0U &&
        ack->rate.rtt_ns != 0U) {
        if (tcp_shift_cc_srtt_update(&adapter->srtt, ack->rate.rtt_ns) != 0) {
            tcp_shift_lwip_cc_disable_on_error(adapter);
            return 0;
        }
        if (adapter->stats != NULL) {
            adapter->stats->srtt_updates++;
        }
    }
    ack->smoothed_rtt_ns = adapter->srtt.smoothed_rtt_ns;
    if (adapter->stats != NULL) {
        if (ack_time_ns != 0U) {
            adapter->stats->ack_observation_events++;
        }
        adapter->stats->ack_last_time_ns = ack_time_ns;
        adapter->stats->ack_last_smoothed_rtt_ns = ack->smoothed_rtt_ns;
    }
    return 1;
}

static int tcp_shift_lwip_cc_on_ack_observe(void *arg,
                                            struct tcp_pcb *pcb,
                                            tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_ack ack;

    return tcp_shift_lwip_cc_prepare_ack(adapter, pcb, acked_bytes, &ack);
}

static int tcp_shift_lwip_cc_on_ack(void *arg,
                                    struct tcp_pcb *pcb,
                                    tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;

    if (!tcp_shift_lwip_cc_prepare_ack(adapter, pcb, acked_bytes, &ack)) {
        return 0;
    }

    tcp_shift_lwip_cc_transport_from_pcb(pcb, &transport);
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
    .on_ack_observe = tcp_shift_lwip_cc_on_ack_observe,
    .on_loss = tcp_shift_lwip_cc_on_loss,
    .on_timeout = tcp_shift_lwip_cc_on_timeout,
    .on_segment_send_eligible = tcp_shift_lwip_cc_on_segment_send_eligible,
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
    tcp_shift_pacing_unregister(adapter);
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
    tcp_shift_cc_srtt_init(&adapter->srtt);
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
        tcp_shift_lwip_cc_apply_policy(adapter, &policy) < 0 ||
        tcp_shift_pacing_register(adapter) < 0) {
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
        /* Pinned lwIP requires a non-NULL ext-arg callback table. Keep the
         * static callbacks installed and clear only the data pointer; a later
         * PCB destroy will invoke the callback with NULL data, which is a
         * deliberate no-op in tcp_shift_lwip_cc_pcb_destroyed(). */
        tcp_ext_arg_set(adapter->pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID,
                        NULL);
    }
    tcp_shift_pacing_unregister(adapter);
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

void tcp_shift_lwip_cc_mark_app_limited(struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_hook *hook;
    struct tcp_shift_lwip_cc_adapter *adapter;
    uint32_t inflight;
    uint32_t effective_window;
    uint32_t tracked_payload;
    uint64_t marker;

    if (pcb == NULL) {
        return;
    }
    hook = tcp_shift_lwip_cc_hook_get(pcb);
    if (hook == NULL || hook->arg == NULL) {
        return;
    }
    adapter = hook->arg;
    if (adapter->bound == 0U || adapter->pcb != pcb ||
        adapter->app_limited_until_bytes != 0U || pcb->unsent != NULL ||
        tcp_sndbuf(pcb) == 0U) {
        return;
    }

    inflight = pcb->snd_nxt - pcb->lastack;
    effective_window = pcb->cwnd < pcb->snd_wnd ? pcb->cwnd : pcb->snd_wnd;
    if (effective_window == 0U || inflight >= effective_window) {
        return;
    }

    tracked_payload = tcp_shift_delivery_outstanding_payload(adapter);
    marker = adapter->delivered_bytes + tracked_payload;
    if (marker == 0U) {
        marker = 1U;
    }
    adapter->app_limited_until_bytes = marker;
    if (adapter->stats != NULL) {
        adapter->stats->app_limited_enters++;
    }
}

const struct tcp_shift_lwip_cc_stats *tcp_shift_lwip_cc_get_stats(void)
{
    return &tcp_shift_lwip_cc_stats;
}