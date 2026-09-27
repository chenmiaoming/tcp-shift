#include "lwip/cc_adapter.h"
#include "lwip/priv/tcp_priv.h"
#include "runtime/pacer.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
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
    uint64_t tx_ns;
    uint64_t first_tx_mstamp_at_send_ns;
    uint64_t delivered_at_send;
    uint64_t delivered_mstamp_at_send_ns;
    uint32_t seq_start;
    uint32_t prior_inflight_bytes;
    uint16_t payload_bytes;
    uint16_t acked_payload_bytes;
    uint8_t app_limited;
    uint8_t retransmitted;
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    uint8_t rack_delivered;
    uint8_t rack_lost;
#endif
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

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
struct tcp_shift_recovery_timer_service {
    const struct tcp_shift_lwip_recovery_timer_ops *ops;
    void *arg;
};
#endif

static struct tcp_shift_lwip_cc_listener_binding
    tcp_shift_lwip_cc_listeners[TCP_SHIFT_LWIP_CC_MAX_LISTENERS];
static struct tcp_shift_lwip_cc_stats tcp_shift_lwip_cc_stats;
static struct tcp_shift_pacing_service tcp_shift_pacing_service;
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
static struct tcp_shift_recovery_timer_service
    tcp_shift_recovery_timer_service;
#endif

static uint32_t tcp_shift_delivery_outstanding_payload(
    const struct tcp_shift_lwip_cc_adapter *adapter);

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
static uint64_t tcp_shift_rack_detection_deadline(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    uint64_t now_ns);
static void tcp_shift_rack_arm_detection_timer(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    uint64_t now_ns);
static void tcp_shift_tlp_arm_pto(struct tcp_shift_lwip_cc_adapter *adapter,
                                  struct tcp_pcb *pcb,
                                  uint64_t now_ns);
#endif

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
    return tcp_shift_initial_cwnd_bytes(mss_bytes);
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

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
static void tcp_shift_recovery_timer_cancel(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    size_t cancelled = 0U;

    if (adapter == NULL || adapter->recovery_timer_scheduled == 0U) {
        return;
    }
    if (tcp_shift_recovery_timer_service.ops != NULL &&
        tcp_shift_recovery_timer_service.ops->cancel != NULL &&
        adapter->pacing_flow_id != 0U) {
        (void)tcp_shift_recovery_timer_service.ops->cancel(
            tcp_shift_recovery_timer_service.arg,
            adapter->pacing_flow_id,
            adapter->pacing_generation,
            &cancelled);
    }
    adapter->recovery_timer_scheduled = 0U;
    adapter->recovery_timer_deadline_ns = 0U;
    adapter->recovery_timer_kind = 0U;
}
#endif

static void tcp_shift_pacing_unregister(struct tcp_shift_lwip_cc_adapter *adapter)
{
    size_t index;
    struct tcp_shift_pacing_registry_entry *entry;

    if (adapter == NULL || adapter->pacing_flow_id == 0U) {
        return;
    }

    tcp_shift_pacing_cancel(adapter);
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_recovery_timer_cancel(adapter);
#endif
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

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
int tcp_shift_lwip_cc_configure_recovery_timer(
    const struct tcp_shift_lwip_recovery_timer_ops *ops,
    void *arg)
{
    if (ops == NULL || ops->schedule == NULL || ops->cancel == NULL) {
        return -1;
    }
    if (tcp_shift_recovery_timer_service.ops != NULL) {
        return tcp_shift_recovery_timer_service.ops == ops &&
                       tcp_shift_recovery_timer_service.arg == arg
                   ? 0
                   : -1;
    }
    if (tcp_shift_pacing_service.active != 0U) {
        return -1;
    }
    tcp_shift_recovery_timer_service.ops = ops;
    tcp_shift_recovery_timer_service.arg = arg;
    return 0;
}

int tcp_shift_lwip_cc_clear_recovery_timer(void)
{
    if (tcp_shift_pacing_service.active != 0U) {
        return -1;
    }
    tcp_shift_recovery_timer_service.ops = NULL;
    tcp_shift_recovery_timer_service.arg = NULL;
    return 0;
}
#endif

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

static u32_t tcp_shift_lwip_cc_effective_cwnd(void *arg,
                                                    struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    uint32_t raw_inflight;
    uint32_t actual_inflight;
    uint32_t credit;
    uint32_t cwnd;

    if (pcb == NULL) {
        return 0U;
    }
    cwnd = (uint32_t)pcb->cwnd;
    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        adapter->sack_delivery_policy == 0U) {
        return cwnd;
    }

    raw_inflight = pcb->snd_nxt - pcb->lastack;
    actual_inflight = tcp_shift_delivery_outstanding_payload(adapter);
    if (actual_inflight >= raw_inflight) {
        return cwnd;
    }

    /* lwIP gates new sends with seq-lastack against cwnd. Linux SACK-aware
     * in-flight accounting removes already-SACKed data from packets_in_flight.
     * Add exactly that released sequence-space as temporary cwnd credit so the
     * existing tcp_output() inequality becomes equivalent to
     * actual_unsacked_inflight + new_bytes <= controller_cwnd. */
    credit = raw_inflight - actual_inflight;
    if (cwnd > UINT32_MAX - credit) {
        return UINT32_MAX;
    }
    return cwnd + credit;
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

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
int tcp_shift_lwip_cc_resume_recovery_timer(uint64_t flow_id,
                                            uint32_t generation,
                                            uint32_t kind,
                                            uint64_t actual_release_ns)
{
    struct tcp_shift_pacing_registry_entry *entry;
    struct tcp_shift_lwip_cc_adapter *adapter;
    uint64_t deadline_ns;
    uint64_t now_ns;
    uint32_t probe_start_seq = 0U;
    uint32_t probe_end_seq = 0U;
    u8_t probe_is_retrans = 0U;
    size_t index;
    err_t err;

    if (flow_id == 0U || flow_id > tcp_shift_pacing_service.capacity) {
        return 0;
    }
    index = (size_t)(flow_id - 1U);
    entry = &tcp_shift_pacing_service.entries[index];
    if (entry->adapter == NULL || entry->generation != generation) {
        return 0;
    }

    adapter = entry->adapter;
    adapter->recovery_timer_scheduled = 0U;
    adapter->recovery_timer_deadline_ns = 0U;
    adapter->recovery_timer_kind = 0U;
    if (adapter->bound == 0U || adapter->pcb == NULL ||
        actual_release_ns == 0U) {
        return 0;
    }

    if (kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP) {
        if (!tcp_shift_tlp_probe_allowed(&adapter->rack_tlp) ||
            tcp_shift_lwip_cc_hook_recovery_is_active(&adapter->hook) ||
            adapter->rack_tlp.segs_sacked != 0U) {
            return 0;
        }

        err = tcp_shift_tcp_tlp_probe(
            adapter->pcb, &probe_start_seq, &probe_end_seq,
            &probe_is_retrans);
        if (err != ERR_OK) {
            /* PTO is only a probe opportunity. If it cannot transmit, leave
             * the ordinary RTO as the conservative final fallback. */
            return err == ERR_VAL ? 0 : -1;
        }
        if (probe_end_seq != 0U) {
            tcp_shift_tlp_note_probe_sent(
                &adapter->rack_tlp, probe_start_seq, probe_end_seq,
                probe_is_retrans != 0U);
        }
        return 0;
    }

    if (kind != TCP_SHIFT_LWIP_RECOVERY_TIMER_RACK) {
        return 0;
    }

    deadline_ns = tcp_shift_rack_detection_deadline(
        adapter, adapter->pcb, actual_release_ns);
    if (deadline_ns == 0U) {
        return 0;
    }
    if (deadline_ns > actual_release_ns) {
        tcp_shift_rack_arm_detection_timer(
            adapter, adapter->pcb, actual_release_ns);
        return 0;
    }

    err = tcp_shift_tcp_rack_rexmit_due(adapter->pcb);
    if (err != ERR_OK) {
        /* A transiently busy segment remains protected by the ordinary RTO
         * fallback. Avoid an immediate timer spin; a later ACK can re-arm
         * RACK with fresh evidence. */
        return err == ERR_VAL ? 0 : -1;
    }

    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns != 0U) {
        tcp_shift_rack_arm_detection_timer(adapter, adapter->pcb, now_ns);
    }
    return 0;
}

#endif

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
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
static int tcp_shift_rack_loss_trace_enabled(void)
{
    const char *value = getenv("TCP_SHIFT_RACK_LOSS_TRACE");
    return value != NULL && value[0] != '\0' &&
                   !(value[0] == '0' && value[1] == '\0');
}

static unsigned tcp_shift_rack_loss_trace_events;

static int tcp_shift_rack_seq_before_u32(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

static int tcp_shift_rack_seq_after_eq_u32(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) >= 0;
}

static int tcp_shift_rack_sack_is_dsack(
    uint32_t ack_seq,
    const struct tcp_shift_lwip_sack_range *ranges,
    uint8_t range_count)
{
    if (ranges == NULL || range_count == 0U ||
        !tcp_shift_rack_seq_before_u32(ranges[0].left, ranges[0].right)) {
        return 0;
    }

    /* RFC 2883 section 4: classify from the cumulative ACK carried in the
     * same packet, never from SND.UNA/pcb->lastack. The first block is D-SACK
     * when it lies at/below that ACK, or when it is contained by the second
     * SACK block above the ACK. */
    if (tcp_shift_rack_seq_after_eq_u32(ack_seq, ranges[0].right)) {
        return 1;
    }
    if (range_count >= 2U &&
        tcp_shift_rack_seq_after_eq_u32(ranges[0].left, ranges[1].left) &&
        tcp_shift_rack_seq_after_eq_u32(ranges[1].right, ranges[0].right)) {
        return 1;
    }
    return 0;
}

static void tcp_shift_rack_record_stats(
    struct tcp_shift_lwip_cc_adapter *adapter)
{
    struct tcp_shift_lwip_cc_stats *stats;

    if (adapter == NULL || adapter->stats == NULL) {
        return;
    }
    stats = adapter->stats;
    stats->rack_reo_wnd_mult = adapter->rack_tlp.reo_wnd_mult;
    stats->rack_reo_wnd_persist = adapter->rack_tlp.reo_wnd_persist;
    if (adapter->rack_tlp.reo_wnd_mult > stats->rack_reo_wnd_mult_max) {
        stats->rack_reo_wnd_mult_max = adapter->rack_tlp.reo_wnd_mult;
    }
}

static void tcp_shift_rack_slot_view(
    const struct tcp_shift_delivery_slot *slot,
    struct tcp_shift_rack_segment *segment)
{
    segment->xmit_ts_ns = slot->tx_ns;
    segment->seq_start = slot->seq_start;
    segment->end_seq = slot->seq_start + slot->payload_bytes;
    segment->retransmitted = slot->retransmitted;
    segment->lost = 0U;
}

static void tcp_shift_rack_process_delivered_slots(
    struct tcp_shift_lwip_cc_adapter *adapter,
    uint64_t ack_time_ns,
    const char *delivery_source)
{
    struct tcp_shift_delivery_slot *slots;
    uint16_t processed = 0U;

    if (adapter == NULL || ack_time_ns == 0U) {
        return;
    }
    slots = tcp_shift_delivery_slots(adapter);

    /* RFC 8985 section 6.2 step 2: update RACK.segment using newly delivered
     * segments in ascending Segment.xmit_ts order, breaking timestamp ties
     * with end_seq. rack_delivered=1 means the timing pass is complete. */
    for (;;) {
        struct tcp_shift_delivery_slot *next = NULL;
        uint16_t index;

        for (index = 0U; index < adapter->delivery_capacity; index++) {
            struct tcp_shift_delivery_slot *slot = &slots[index];
            uint32_t slot_end;
            uint32_t next_end;

            if (slot->segment == NULL || slot->rack_delivered != 0U ||
                slot->payload_bytes == 0U ||
                slot->acked_payload_bytes < slot->payload_bytes) {
                continue;
            }
            if (next == NULL) {
                next = slot;
                continue;
            }
            slot_end = slot->seq_start + slot->payload_bytes;
            next_end = next->seq_start + next->payload_bytes;
            if (tcp_shift_rack_sent_after(
                    next->tx_ns, next_end, slot->tx_ns, slot_end)) {
                next = slot;
            }
        }

        if (next == NULL) {
            break;
        }

        {
            struct tcp_shift_rack_segment segment;

            tcp_shift_rack_slot_view(next, &segment);
            if (next->retransmitted == 0U &&
                ack_time_ns >= next->tx_ns) {
                uint64_t sample = ack_time_ns - next->tx_ns;

                if (sample != 0U &&
                    (adapter->rack_tlp.min_rtt_ns == 0U ||
                     sample < adapter->rack_tlp.min_rtt_ns)) {
                    adapter->rack_tlp.min_rtt_ns = sample;
                }
            }
            tcp_shift_rack_set_rtt_estimates(
                &adapter->rack_tlp,
                adapter->rack_tlp.min_rtt_ns,
                adapter->srtt.smoothed_rtt_ns);
            (void)tcp_shift_rack_note_delivered(
                &adapter->rack_tlp, &segment, ack_time_ns);
        }
        next->rack_delivered = 1U;
        processed++;
        if (processed >= adapter->delivery_capacity) {
            break;
        }
    }

    /* RFC 8985 section 6.2 step 3 is a second pass in ascending end_seq.
     * This ordering prevents multiple ranges newly delivered by one ACK from
     * being mistaken for network reordering. rack_delivered=2 is complete. */
    processed = 0U;
    for (;;) {
        struct tcp_shift_delivery_slot *next = NULL;
        uint16_t index;

        for (index = 0U; index < adapter->delivery_capacity; index++) {
            struct tcp_shift_delivery_slot *slot = &slots[index];
            uint32_t slot_end;
            uint32_t next_end;

            if (slot->segment == NULL || slot->rack_delivered != 1U ||
                slot->payload_bytes == 0U ||
                slot->acked_payload_bytes < slot->payload_bytes) {
                continue;
            }
            if (next == NULL) {
                next = slot;
                continue;
            }
            slot_end = slot->seq_start + slot->payload_bytes;
            next_end = next->seq_start + next->payload_bytes;
            if (tcp_shift_rack_seq_before_u32(slot_end, next_end)) {
                next = slot;
            }
        }

        if (next == NULL) {
            break;
        }

        {
            struct tcp_shift_rack_segment segment;
            uint8_t reordering_before = adapter->rack_tlp.reordering_seen;

            uint32_t fack_before = adapter->rack_tlp.fack;
            uint32_t rack_end_before = adapter->rack_tlp.rack_end_seq;
            uint64_t rack_xmit_before = adapter->rack_tlp.rack_xmit_ts_ns;

            tcp_shift_rack_slot_view(next, &segment);
            tcp_shift_rack_detect_reordering(&adapter->rack_tlp, &segment);
            if (reordering_before == 0U &&
                adapter->rack_tlp.reordering_seen != 0U) {
                if (tcp_shift_rack_loss_trace_enabled() &&
                    tcp_shift_rack_loss_trace_events < 128U) {
                    fprintf(stderr,
                            "tcp-shift-rack-loss-trace: event=reordering-first "
                            "source=%s seq=%u end=%u retrans=%u tx_ns=%llu "
                            "ack_ns=%llu lastack=%u snd_nxt=%u "
                            "fack_before=%u fack_after=%u "
                            "rack_end_before=%u rack_xmit_before=%llu "
                            "rack_end_after=%u rack_xmit_after=%llu "
                            "acked_payload=%u payload=%u\n",
                            delivery_source != NULL ? delivery_source : "unknown",
                            segment.seq_start, segment.end_seq,
                            segment.retransmitted,
                            (unsigned long long)segment.xmit_ts_ns,
                            (unsigned long long)ack_time_ns,
                            adapter->pcb != NULL ? adapter->pcb->lastack : 0U,
                            adapter->pcb != NULL ? adapter->pcb->snd_nxt : 0U,
                            fack_before, adapter->rack_tlp.fack,
                            rack_end_before,
                            (unsigned long long)rack_xmit_before,
                            adapter->rack_tlp.rack_end_seq,
                            (unsigned long long)adapter->rack_tlp.rack_xmit_ts_ns,
                            (unsigned)next->acked_payload_bytes,
                            (unsigned)next->payload_bytes);
                    tcp_shift_rack_loss_trace_events++;
                }
                if (adapter->stats != NULL) {
                    adapter->stats->rack_reordering_events++;
                }
            }
            tcp_shift_rack_record_stats(adapter);
        }
        next->rack_delivered = 2U;
        processed++;
        if (processed >= adapter->delivery_capacity) {
            break;
        }
    }
}

static uint32_t tcp_shift_rack_count_sacked_slots(
    const struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_pcb *pcb)
{
    const struct tcp_shift_delivery_slot *slots;
    uint32_t count = 0U;
    uint16_t index;

    if (adapter == NULL || pcb == NULL) {
        return 0U;
    }
    slots = (const struct tcp_shift_delivery_slot *)adapter->delivery_slots;
    for (index = 0U; index < adapter->delivery_capacity; index++) {
        const struct tcp_shift_delivery_slot *slot = &slots[index];
        uint32_t end_seq;

        if (slot->segment == NULL || slot->payload_bytes == 0U ||
            slot->acked_payload_bytes < slot->payload_bytes) {
            continue;
        }
        end_seq = slot->seq_start + slot->payload_bytes;
        if ((int32_t)(end_seq - pcb->lastack) > 0) {
            count++;
        }
    }
    return count;
}

static uint64_t tcp_shift_rack_add_sat_ns(uint64_t left, uint64_t right)
{
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

/* RFC 8985 RACK_detect_loss(): return an immediate deadline when any
 * transmission is already mature for loss, otherwise return the deadline
 * after the maximum positive remaining interval. The RFC deliberately uses
 * max(remaining): one reordering timer wakes when all currently eligible
 * older transmissions have aged through the reordering window. */
static uint64_t tcp_shift_rack_detection_deadline(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    uint64_t now_ns)
{
    struct tcp_shift_delivery_slot *slots;
    uint64_t max_remaining_ns = 0U;
    uint16_t index;
    unsigned in_recovery;
    unsigned due = 0U;

    if (adapter == NULL || pcb == NULL || now_ns == 0U ||
        adapter->rack_tlp.rack_xmit_ts_ns == 0U ||
        adapter->rack_tlp.rack_rtt_ns == 0U) {
        return 0U;
    }

    slots = tcp_shift_delivery_slots(adapter);
    in_recovery = tcp_shift_lwip_cc_hook_recovery_is_active(&adapter->hook);
    for (index = 0U; index < adapter->delivery_capacity; index++) {
        struct tcp_shift_delivery_slot *slot = &slots[index];
        struct tcp_shift_rack_segment segment;
        uint64_t remaining_ns = 0U;

        if (slot->segment == NULL || slot->payload_bytes == 0U ||
            slot->acked_payload_bytes >= slot->payload_bytes ||
            slot->tx_ns == 0U) {
            continue;
        }

        tcp_shift_rack_slot_view(slot, &segment);
        if (tcp_shift_rack_loss_remaining(
                &adapter->rack_tlp, &segment, now_ns, in_recovery,
                &remaining_ns)) {
            due = 1U;
        } else if (remaining_ns > max_remaining_ns) {
            max_remaining_ns = remaining_ns;
        }
    }

    if (due != 0U) {
        return now_ns;
    }
    return max_remaining_ns != 0U
               ? tcp_shift_rack_add_sat_ns(now_ns, max_remaining_ns)
               : 0U;
}

static void tcp_shift_rack_arm_detection_timer(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    uint64_t now_ns)
{
    uint64_t deadline_ns;
    size_t cancelled = 0U;

    if (adapter == NULL || pcb == NULL) {
        return;
    }

    deadline_ns = tcp_shift_rack_detection_deadline(adapter, pcb, now_ns);
    if (deadline_ns == 0U) {
        tcp_shift_recovery_timer_cancel(adapter);
        return;
    }
    if (adapter->recovery_timer_scheduled != 0U &&
        adapter->recovery_timer_deadline_ns == deadline_ns) {
        return;
    }

    if (adapter->recovery_timer_scheduled != 0U) {
        if (tcp_shift_recovery_timer_service.ops == NULL ||
            tcp_shift_recovery_timer_service.ops->cancel == NULL ||
            tcp_shift_recovery_timer_service.ops->cancel(
                tcp_shift_recovery_timer_service.arg,
                adapter->pacing_flow_id,
                adapter->pacing_generation,
                &cancelled) < 0) {
            tcp_shift_recovery_timer_cancel(adapter);
            return;
        }
        adapter->recovery_timer_scheduled = 0U;
        adapter->recovery_timer_deadline_ns = 0U;
    }

    if (tcp_shift_recovery_timer_service.ops == NULL ||
        tcp_shift_recovery_timer_service.ops->schedule == NULL ||
        adapter->pacing_flow_id == 0U) {
        return;
    }
    if (tcp_shift_recovery_timer_service.ops->schedule(
            tcp_shift_recovery_timer_service.arg,
            adapter->pacing_flow_id,
            adapter->pacing_generation,
            deadline_ns,
            TCP_SHIFT_LWIP_RECOVERY_TIMER_RACK) < 0) {
        return;
    }
    adapter->recovery_timer_scheduled = 1U;
    adapter->recovery_timer_deadline_ns = deadline_ns;
    adapter->recovery_timer_kind = TCP_SHIFT_LWIP_RECOVERY_TIMER_RACK;
}

static uint32_t tcp_shift_tlp_flight_segments(
    const struct tcp_shift_lwip_cc_adapter *adapter)
{
    const struct tcp_shift_delivery_slot *slots;
    uint32_t count = 0U;
    uint16_t index;

    if (adapter == NULL || adapter->delivery_slots == NULL) {
        return 0U;
    }
    slots = (const struct tcp_shift_delivery_slot *)adapter->delivery_slots;
    for (index = 0U; index < adapter->delivery_capacity; index++) {
        const struct tcp_shift_delivery_slot *slot = &slots[index];

        if (slot->segment != NULL && slot->payload_bytes != 0U &&
            slot->acked_payload_bytes < slot->payload_bytes) {
            count++;
        }
    }
    return count;
}

static uint64_t tcp_shift_tlp_rto_expiration_ns(
    const struct tcp_pcb *pcb,
    uint64_t now_ns)
{
    uint64_t remaining_ticks;
    uint64_t remaining_ns;

    if (pcb == NULL || now_ns == 0U || pcb->rto <= 0) {
        return 0U;
    }
    if (pcb->rtime >= pcb->rto) {
        return now_ns;
    }

    remaining_ticks = (uint64_t)(pcb->rto - pcb->rtime);
    remaining_ns =
        remaining_ticks * (uint64_t)TCP_SLOW_INTERVAL * UINT64_C(1000000);
    return remaining_ns > UINT64_MAX - now_ns ? UINT64_MAX
                                               : now_ns + remaining_ns;
}

static void tcp_shift_tlp_arm_pto(struct tcp_shift_lwip_cc_adapter *adapter,
                                  struct tcp_pcb *pcb,
                                  uint64_t now_ns)
{
    uint64_t pto_ns;
    uint64_t deadline_ns;
    uint64_t rto_expiration_ns;
    uint32_t flight_segments;
    size_t cancelled = 0U;

    if (adapter == NULL || pcb == NULL || now_ns == 0U) {
        return;
    }
    if (tcp_shift_lwip_cc_hook_recovery_is_active(&adapter->hook) ||
        adapter->rack_tlp.segs_sacked != 0U ||
        !tcp_shift_tlp_probe_allowed(&adapter->rack_tlp)) {
        if (adapter->recovery_timer_scheduled != 0U &&
            adapter->recovery_timer_kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP) {
            tcp_shift_recovery_timer_cancel(adapter);
        }
        return;
    }

    flight_segments = tcp_shift_tlp_flight_segments(adapter);
    if (flight_segments == 0U) {
        if (adapter->recovery_timer_scheduled != 0U &&
            adapter->recovery_timer_kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP) {
            tcp_shift_recovery_timer_cancel(adapter);
        }
        return;
    }

    /* RACK reordering evidence takes priority over TLP. */
    if (adapter->recovery_timer_scheduled != 0U &&
        adapter->recovery_timer_kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_RACK) {
        return;
    }

    rto_expiration_ns = tcp_shift_tlp_rto_expiration_ns(pcb, now_ns);
    pto_ns = tcp_shift_tlp_calc_pto_ns(
        &adapter->rack_tlp, now_ns, rto_expiration_ns,
        flight_segments, 0U);
    if (pto_ns == 0U) {
        return;
    }
    deadline_ns = pto_ns > UINT64_MAX - now_ns ? UINT64_MAX
                                                : now_ns + pto_ns;

    if (adapter->recovery_timer_scheduled != 0U) {
        if (adapter->recovery_timer_deadline_ns == deadline_ns &&
            adapter->recovery_timer_kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP) {
            return;
        }
        if (tcp_shift_recovery_timer_service.ops == NULL ||
            tcp_shift_recovery_timer_service.ops->cancel == NULL ||
            tcp_shift_recovery_timer_service.ops->cancel(
                tcp_shift_recovery_timer_service.arg,
                adapter->pacing_flow_id,
                adapter->pacing_generation,
                &cancelled) < 0) {
            tcp_shift_recovery_timer_cancel(adapter);
            return;
        }
        adapter->recovery_timer_scheduled = 0U;
        adapter->recovery_timer_deadline_ns = 0U;
        adapter->recovery_timer_kind = 0U;
    }

    if (tcp_shift_recovery_timer_service.ops == NULL ||
        tcp_shift_recovery_timer_service.ops->schedule == NULL ||
        adapter->pacing_flow_id == 0U) {
        return;
    }
    if (tcp_shift_recovery_timer_service.ops->schedule(
            tcp_shift_recovery_timer_service.arg,
            adapter->pacing_flow_id,
            adapter->pacing_generation,
            deadline_ns,
            TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP) < 0) {
        return;
    }

    adapter->recovery_timer_scheduled = 1U;
    adapter->recovery_timer_deadline_ns = deadline_ns;
    adapter->recovery_timer_kind = TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP;
}

#endif

static void tcp_shift_lwip_cc_transport_from_adapter(
    const struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_pcb *pcb,
    struct tcp_shift_cc_transport *transport)
{
    tcp_shift_lwip_cc_transport_from_pcb(pcb, transport);
    if (adapter != NULL && adapter->sack_delivery_policy != 0U) {
        transport->inflight_bytes =
            tcp_shift_delivery_outstanding_payload(adapter);
    }
}

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
        if (adapter->stats->delivery_last_tx_ns != 0U &&
            now_ns >= adapter->stats->delivery_last_tx_ns) {
            uint64_t tx_gap_ns =
                now_ns - adapter->stats->delivery_last_tx_ns;

            if (tx_gap_ns > adapter->stats->pacing_max_tx_gap_ns) {
                adapter->stats->pacing_max_tx_gap_ns = tx_gap_ns;
            }
        }
        adapter->stats->delivery_last_tx_ns = now_ns;
    }

    /* Match Linux tcp_rate_skb_sent(): start a new send phase when there
     * were no packets outstanding before this successful transmission. The
     * delivery metadata can deliberately outlive the transport's unacked list
     * across RTO requeueing, so delivery_live is not an equivalent test. At
     * this hook point lwIP has transmitted seg but has not yet moved it from
     * unsent back to unacked, making pcb->unacked the pre-send outstanding
     * state we need. */
    start_of_flight = pcb->unacked == NULL;
    if (start_of_flight != 0U) {
        adapter->rate_first_tx_mstamp_ns = now_ns;
        adapter->delivered_mstamp_ns = now_ns;
    }

    slot = tcp_shift_delivery_find(adapter, segment);
    if (slot != NULL) {
        /* Delivery-rate sampling is keyed to the packet's last transmission,
         * not only its original transmission. A retransmission therefore
         * refreshes the delivery/send snapshot and last-tx timestamp. Karn
         * filtering is carried by the retransmitted flag, so RTT remains
         * invalid for this slot. */
        slot->tx_ns = now_ns;
        slot->first_tx_mstamp_at_send_ns =
            adapter->rate_first_tx_mstamp_ns != 0U
                ? adapter->rate_first_tx_mstamp_ns
                : now_ns;
        slot->delivered_at_send = adapter->delivered_bytes;
        slot->delivered_mstamp_at_send_ns =
            adapter->delivered_mstamp_ns != 0U ? adapter->delivered_mstamp_ns
                                               : now_ns;
        slot->app_limited = adapter->app_limited_until_bytes != 0U;
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
        slot->rack_delivered = 0U;
        slot->rack_lost = 0U;
#endif
        if (adapter->stats != NULL) {
            adapter->stats->delivery_retransmit_events++;
            if (slot->retransmitted != 0U) {
                adapter->stats->delivery_repeat_retransmit_events++;
            } else {
                adapter->stats->delivery_unique_retransmit_events++;
            }
        }
        slot->retransmitted = 1U;
        return;
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

    slot->tx_ns = now_ns;
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

static int tcp_shift_delivery_candidate_newer(
    const struct tcp_shift_delivery_slot *slot,
    const struct tcp_shift_delivery_slot *candidate)
{
    return candidate == NULL || slot->tx_ns > candidate->tx_ns ||
           (slot->tx_ns == candidate->tx_ns &&
            (int32_t)((slot->seq_start + slot->payload_bytes) -
                      (candidate->seq_start + candidate->payload_bytes)) > 0);
}

static void tcp_shift_delivery_finalize_rate_sample(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_shift_delivery_slot *candidate,
    uint64_t delivered_added,
    uint64_t now_ns,
    uint16_t touched,
    unsigned partial,
    struct tcp_shift_cc_rate_sample *rate)
{
    uint64_t delivered_delta64;

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

    if (candidate->tx_ns >= candidate->first_tx_mstamp_at_send_ns) {
        rate->send_interval_ns =
            candidate->tx_ns - candidate->first_tx_mstamp_at_send_ns;
    }
    adapter->rate_first_tx_mstamp_ns = candidate->tx_ns;
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
    } else if (candidate->tx_ns != 0U && now_ns >= candidate->tx_ns) {
        rate->rtt_ns = now_ns - candidate->tx_ns;
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

static uint32_t tcp_shift_delivery_build_rate_sample(
    struct tcp_shift_lwip_cc_adapter *adapter,
    struct tcp_pcb *pcb,
    struct tcp_shift_cc_rate_sample *rate)
{
    struct tcp_shift_delivery_slot *slots = tcp_shift_delivery_slots(adapter);
    struct tcp_shift_delivery_slot *candidate = NULL;
    uint64_t delivered_added = 0U;
    uint64_t now_ns;
    uint16_t touched = 0U;
    uint16_t index;
    unsigned partial = 0U;

    memset(rate, 0, sizeof(*rate));
    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return 0U;
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

        if (tcp_shift_delivery_candidate_newer(slot, candidate)) {
            candidate = slot;
        }
    }

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_rack_process_delivered_slots(
        adapter, now_ns, "cumulative");
#endif
    tcp_shift_delivery_finalize_rate_sample(
        adapter, candidate, delivered_added, now_ns, touched, partial, rate);
    return delivered_added > UINT32_MAX ? UINT32_MAX
                                        : (uint32_t)delivered_added;
}

static int tcp_shift_delivery_slot_in_sack_range(
    const struct tcp_shift_delivery_slot *slot,
    const struct tcp_shift_lwip_sack_range *range)
{
    uint32_t seq_end;

    if (slot == NULL || range == NULL || slot->segment == NULL ||
        slot->payload_bytes == 0U) {
        return 0;
    }
    seq_end = slot->seq_start + slot->payload_bytes;
    return (int32_t)(slot->seq_start - range->left) >= 0 &&
                   (int32_t)(range->right - seq_end) >= 0
               ? 1
               : 0;
}

static uint32_t tcp_shift_delivery_build_sack_rate_sample(
    struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_shift_lwip_sack_range *ranges,
    uint8_t range_count,
    struct tcp_shift_cc_rate_sample *rate)
{
    struct tcp_shift_delivery_slot *slots = tcp_shift_delivery_slots(adapter);
    struct tcp_shift_delivery_slot *candidate = NULL;
    uint64_t delivered_added = 0U;
    uint64_t now_ns;
    uint16_t touched = 0U;
    uint16_t index;
    uint8_t range_index;

    memset(rate, 0, sizeof(*rate));
    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return 0U;
    }

    for (index = 0U; index < adapter->delivery_capacity; index++) {
        struct tcp_shift_delivery_slot *slot = &slots[index];
        int covered = 0;

        if (slot->segment == NULL ||
            slot->acked_payload_bytes >= slot->payload_bytes) {
            continue;
        }
        for (range_index = 0U; range_index < range_count; range_index++) {
            if (tcp_shift_delivery_slot_in_sack_range(
                    slot, &ranges[range_index])) {
                covered = 1;
                break;
            }
        }
        if (!covered) {
            continue;
        }

        delivered_added +=
            (uint16_t)(slot->payload_bytes - slot->acked_payload_bytes);
        slot->acked_payload_bytes = slot->payload_bytes;
        touched++;
        if (tcp_shift_delivery_candidate_newer(slot, candidate)) {
            candidate = slot;
        }
    }

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_rack_process_delivered_slots(
        adapter, now_ns, "sack");
#endif
    tcp_shift_delivery_finalize_rate_sample(
        adapter, candidate, delivered_added, now_ns, touched, 0U, rate);
    return delivered_added > UINT32_MAX ? UINT32_MAX
                                        : (uint32_t)delivered_added;
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
    uint32_t newly_delivered;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        acked_bytes == 0U || ack == NULL) {
        return 0;
    }

    memset(ack, 0, sizeof(*ack));
    newly_delivered =
        tcp_shift_delivery_build_rate_sample(adapter, pcb, &ack->rate);
    ack_time_ns = adapter->delivery_last_clock_read_ns;
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_rack_note_sacked_segments(
        &adapter->rack_tlp, tcp_shift_rack_count_sacked_slots(adapter, pcb));
#endif
    ack->acked_bytes = adapter->sack_delivery_policy != 0U
                           ? newly_delivered
                           : acked_bytes;
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
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
        tcp_shift_tlp_note_rtt_sample(&adapter->rack_tlp);
#endif
    }
    ack->smoothed_rtt_ns = adapter->srtt.smoothed_rtt_ns;
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    {
        enum tcp_shift_tlp_ack_result tlp_result =
            tcp_shift_tlp_process_ack(
                &adapter->rack_tlp, pcb->lastack, 0U, 0U);

        if (tlp_result == TCP_SHIFT_TLP_ACK_LOSS_REPAIRED &&
            !tcp_shift_lwip_cc_hook_tlp_loss(pcb, pcb->mss)) {
            tcp_shift_lwip_cc_disable_on_error(adapter);
            return 0;
        }
    }
    tcp_shift_rack_set_rtt_estimates(
        &adapter->rack_tlp,
        adapter->rack_tlp.min_rtt_ns,
        adapter->srtt.smoothed_rtt_ns);
    tcp_shift_rack_arm_detection_timer(adapter, pcb, ack_time_ns);
    tcp_shift_tlp_arm_pto(adapter, pcb, ack_time_ns);
#endif
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
    if (adapter->sack_delivery_policy != 0U && ack.acked_bytes == 0U) {
        return 1;
    }

    tcp_shift_lwip_cc_transport_from_adapter(adapter, pcb, &transport);
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

static int tcp_shift_lwip_cc_on_sack(
    void *arg,
    struct tcp_pcb *pcb,
    u32_t ack_seq,
    const struct tcp_shift_lwip_sack_range *ranges,
    u8_t range_count)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_ack ack;
    struct tcp_shift_cc_policy policy;
    uint32_t newly_delivered;
    uint64_t ack_time_ns;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        adapter->sack_delivery_policy == 0U || ranges == NULL ||
        range_count == 0U) {
        return 0;
    }

#if !defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) || !TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    (void)ack_seq;
#endif
    memset(&ack, 0, sizeof(ack));
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    if (tcp_shift_rack_sack_is_dsack(ack_seq, ranges, range_count)) {
        tcp_shift_rack_note_dsack(&adapter->rack_tlp, pcb->snd_nxt);
        if (adapter->stats != NULL) {
            adapter->stats->rack_dsack_events++;
        }
        tcp_shift_rack_record_stats(adapter);
    }
#endif
    newly_delivered = tcp_shift_delivery_build_sack_rate_sample(
        adapter, ranges, range_count, &ack.rate);
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    if (adapter->rack_tlp.tlp_is_retrans != 0U &&
        adapter->rack_tlp.tlp_end_seq != 0U) {
        u8_t index;

        for (index = 0U; index < range_count; index++) {
            if (ranges[index].left == adapter->rack_tlp.tlp_start_seq &&
                ranges[index].right == adapter->rack_tlp.tlp_end_seq &&
                (int32_t)(ack_seq - ranges[index].right) >= 0) {
                (void)tcp_shift_tlp_process_ack(
                    &adapter->rack_tlp, ack_seq, 1U, 0U);
                break;
            }
        }
    }
    tcp_shift_rack_note_sacked_segments(
        &adapter->rack_tlp, tcp_shift_rack_count_sacked_slots(adapter, pcb));
#endif
    if (adapter->stats != NULL) {
        adapter->stats->delivery_sack_events++;
        adapter->stats->delivery_sack_payload_bytes += newly_delivered;
    }
    if (newly_delivered == 0U) {
        return 1;
    }

    ack_time_ns = adapter->delivery_last_clock_read_ns;
    ack.acked_bytes = newly_delivered;
    ack.ack_time_ns = ack_time_ns;
    if ((ack.rate.flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) != 0U &&
        ack.rate.rtt_ns != 0U) {
        if (tcp_shift_cc_srtt_update(&adapter->srtt, ack.rate.rtt_ns) != 0) {
            tcp_shift_lwip_cc_disable_on_error(adapter);
            return 0;
        }
        if (adapter->stats != NULL) {
            adapter->stats->srtt_updates++;
        }
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
        tcp_shift_tlp_note_rtt_sample(&adapter->rack_tlp);
#endif
    }
    ack.smoothed_rtt_ns = adapter->srtt.smoothed_rtt_ns;
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_rack_set_rtt_estimates(
        &adapter->rack_tlp,
        adapter->rack_tlp.min_rtt_ns,
        adapter->srtt.smoothed_rtt_ns);
    tcp_shift_rack_arm_detection_timer(adapter, pcb, ack_time_ns);
    tcp_shift_tlp_arm_pto(adapter, pcb, ack_time_ns);
#endif
    if (adapter->stats != NULL) {
        adapter->stats->ack_observation_events++;
        adapter->stats->ack_last_time_ns = ack_time_ns;
        adapter->stats->ack_last_smoothed_rtt_ns = ack.smoothed_rtt_ns;
    }

    tcp_shift_lwip_cc_transport_from_adapter(adapter, pcb, &transport);
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

static void tcp_shift_lwip_cc_on_tlp_dupack(
    void *arg,
    struct tcp_pcb *pcb,
    unsigned sack_seen)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        sack_seen != 0U) {
        return;
    }
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    (void)tcp_shift_tlp_process_ack(
        &adapter->rack_tlp, pcb->lastack, 0U, 1U);
#else
    (void)sack_seen;
#endif
}

static int tcp_shift_lwip_cc_on_tlp_loss(void *arg,
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

    tcp_shift_lwip_cc_transport_from_adapter(adapter, pcb, &transport);
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

static int tcp_shift_lwip_cc_on_rack_retrans_loss(
    void *arg,
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

    /* RFC 8985 treats a proven lost retransmission as an additional
     * congestion indication. Apply the controller's loss response while the
     * transport keeps the existing fast-recovery episode/end marker intact. */
    tcp_shift_lwip_cc_transport_from_adapter(adapter, pcb, &transport);
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

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_tlp_reset(&adapter->rack_tlp);
    if (adapter->recovery_timer_scheduled != 0U &&
        adapter->recovery_timer_kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP) {
        tcp_shift_recovery_timer_cancel(adapter);
    }
#endif
    tcp_shift_lwip_cc_transport_from_adapter(adapter, pcb, &transport);
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

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
static int tcp_shift_lwip_cc_rack_loss_status(void *arg,
                                               struct tcp_pcb *pcb,
                                               const void *segment,
                                               uint64_t *remaining_ns)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_delivery_slot *slot;
    struct tcp_shift_rack_segment rack_segment;
    uint64_t now_ns;
    uint64_t remaining64 = 0U;
    unsigned in_recovery;

    if (remaining_ns != NULL) {
        *remaining_ns = 0U;
    }
    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb ||
        segment == NULL) {
        return -1;
    }

    slot = tcp_shift_delivery_find(adapter, segment);
    if (slot == NULL || slot->payload_bytes == 0U ||
        slot->acked_payload_bytes >= slot->payload_bytes ||
        slot->tx_ns == 0U) {
        return -1;
    }

    now_ns = tcp_shift_delivery_now_ns(adapter);
    if (now_ns == 0U) {
        return -1;
    }

    rack_segment.xmit_ts_ns = slot->tx_ns;
    rack_segment.seq_start = slot->seq_start;
    rack_segment.end_seq = slot->seq_start + slot->payload_bytes;
    rack_segment.retransmitted = slot->retransmitted;
    rack_segment.lost = 0U;
    in_recovery = tcp_shift_lwip_cc_hook_recovery_is_active(&adapter->hook);

    if (tcp_shift_rack_loss_remaining(
            &adapter->rack_tlp, &rack_segment, now_ns, in_recovery,
            &remaining64)) {
        if (tcp_shift_rack_loss_trace_enabled() &&
            tcp_shift_rack_loss_trace_events < 128U) {
            fprintf(stderr,
                    "tcp-shift-rack-loss-trace: seq=%u end=%u xmit_ns=%llu "
                    "now_ns=%llu rack_xmit_ns=%llu rack_end=%u rack_rtt_ns=%llu "
                    "min_rtt_ns=%llu srtt_ns=%llu reo_wnd_ns=%llu "
                    "segs_sacked=%u reordering=%u in_recovery=%u retrans=%u\n",
                    rack_segment.seq_start, rack_segment.end_seq,
                    (unsigned long long)rack_segment.xmit_ts_ns,
                    (unsigned long long)now_ns,
                    (unsigned long long)adapter->rack_tlp.rack_xmit_ts_ns,
                    adapter->rack_tlp.rack_end_seq,
                    (unsigned long long)adapter->rack_tlp.rack_rtt_ns,
                    (unsigned long long)adapter->rack_tlp.min_rtt_ns,
                    (unsigned long long)adapter->rack_tlp.srtt_ns,
                    (unsigned long long)adapter->rack_tlp.reo_wnd_ns,
                    adapter->rack_tlp.segs_sacked,
                    adapter->rack_tlp.reordering_seen,
                    in_recovery, rack_segment.retransmitted);
            tcp_shift_rack_loss_trace_events++;
        }
        return 1;
    }
    if (remaining_ns != NULL) {
        *remaining_ns = remaining64;
    }
    return 0;
}
#endif

static int tcp_shift_lwip_cc_on_recovery_exit(
    void *arg,
    struct tcp_pcb *pcb,
    u32_t ack_seq)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb) {
        return 0;
    }

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_rack_note_recovery_exit(&adapter->rack_tlp, ack_seq);
    tcp_shift_rack_record_stats(adapter);
#else
    (void)ack_seq;
#endif
    /* Reno/CUBIC keep native recovery-window ownership. This callback only
     * advances RACK's DSACK persistence lifecycle for those controllers. */
    return 0;
}

static int tcp_shift_lwip_cc_on_timeout(void *arg, struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_adapter *adapter = arg;
    struct tcp_shift_cc_transport transport;
    struct tcp_shift_cc_policy policy;

    if (adapter == NULL || adapter->bound == 0U || adapter->pcb != pcb) {
        return 0;
    }

#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_tlp_reset(&adapter->rack_tlp);
    tcp_shift_recovery_timer_cancel(adapter);
#endif
    tcp_shift_lwip_cc_transport_from_adapter(adapter, pcb, &transport);
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
    .on_sack = tcp_shift_lwip_cc_on_sack,
    .on_loss = tcp_shift_lwip_cc_on_loss,
    .on_rack_retrans_loss = tcp_shift_lwip_cc_on_rack_retrans_loss,
    .on_tlp_loss = tcp_shift_lwip_cc_on_tlp_loss,
    .on_tlp_dupack = tcp_shift_lwip_cc_on_tlp_dupack,
    .on_timeout = tcp_shift_lwip_cc_on_timeout,
    .on_recovery_exit = tcp_shift_lwip_cc_on_recovery_exit,
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    .rack_loss_status = tcp_shift_lwip_cc_rack_loss_status,
#endif
    .effective_cwnd = tcp_shift_lwip_cc_effective_cwnd,
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
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    tcp_shift_rack_tlp_init(&adapter->rack_tlp);
#endif
    adapter->hook.ops = &tcp_shift_lwip_cc_hook_ops;
    adapter->hook.arg = adapter;
    adapter->stats = stats;
    adapter->pcb = pcb;
    adapter->delivered_mstamp_ns = tcp_shift_delivery_now_ns(adapter);
    if (stats != NULL) {
        stats->delivery_metadata_bytes_per_slot =
            (uint32_t)sizeof(struct tcp_shift_delivery_slot);
    }

    tcp_shift_lwip_cc_transport_from_adapter(adapter, pcb, &transport);
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
#if defined(TCP_SHIFT_EXPERIMENTAL_RACK_TLP) && TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    {
        uint64_t now_ns = tcp_shift_delivery_now_ns(adapter);

        if (now_ns != 0U) {
            tcp_shift_tlp_arm_pto(adapter, pcb, now_ns);
        }
    }
#endif
}

const struct tcp_shift_lwip_cc_stats *tcp_shift_lwip_cc_get_stats(void)
{
    return &tcp_shift_lwip_cc_stats;
}