#ifndef TCP_SHIFT_LWIP_CC_ADAPTER_H
#define TCP_SHIFT_LWIP_CC_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "cc/cc.h"
#include "cc/reno.h"
#include "lwip/cc_hooks.h"
#include "lwip/tcp.h"

struct tcp_shift_lwip_cc_pacer_ops {
    int (*schedule)(void *arg,
                    uint64_t flow_id,
                    uint32_t generation,
                    uint64_t deadline_ns,
                    uint32_t bytes);
    int (*cancel)(void *arg,
                  uint64_t flow_id,
                  uint32_t generation,
                  size_t *cancelled);
};

struct tcp_shift_lwip_cc_stats {
    uint64_t bindings;
    uint64_t bind_failures;
    uint64_t ack_events;
    uint64_t loss_events;
    uint64_t timeout_events;
    uint64_t policy_updates;
    uint64_t controller_errors;
    uint64_t delivery_first_tx_events;
    uint64_t delivery_retransmit_events;
    uint64_t delivery_acked_segment_events;
    uint64_t delivery_payload_bytes;
    uint64_t delivery_metadata_alloc_failures;
    uint64_t delivery_metadata_misses;
    uint64_t delivery_metadata_abandoned_slots;
    uint64_t delivery_clock_errors;
    uint64_t delivery_timestamp_regressions;
    uint64_t delivery_last_tx_ns;
    uint64_t delivery_last_ack_ns;
    uint64_t rate_samples;
    uint64_t rate_valid_samples;
    uint64_t rate_invalid_samples;
    uint64_t rate_app_limited_samples;
    uint64_t rate_retransmitted_samples;
    uint64_t rate_partial_ack_events;
    uint64_t rate_multi_segment_ack_events;
    uint64_t app_limited_enters;
    uint64_t app_limited_exits;
    uint64_t rate_last_bytes_per_sec;
    uint64_t rate_max_bytes_per_sec;
    uint64_t rate_last_interval_ns;
    uint64_t rate_last_send_interval_ns;
    uint64_t rate_last_ack_interval_ns;
    uint64_t rate_last_rtt_ns;
    uint64_t pacing_deferrals;
    uint64_t pacing_resume_events;
    uint64_t pacing_stale_releases;
    uint64_t pacing_scheduler_errors;
    uint64_t pacing_tx_events;
    uint64_t pacing_tx_bytes;
    uint64_t pacing_last_rate_bytes_per_sec;
    uint64_t pacing_last_deadline_ns;
    uint64_t pacing_last_actual_release_ns;
    uint32_t rate_last_delivered_bytes;
    uint32_t rate_last_prior_inflight_bytes;
    uint32_t rate_last_flags;
    uint32_t delivery_metadata_bytes_per_slot;
    uint32_t delivery_live_slots;
    uint32_t delivery_peak_live_slots;
    uint32_t delivery_peak_slots_per_flow;
    uint32_t delivery_peak_capacity_slots_per_flow;
    uint32_t last_cwnd_bytes;
    uint32_t last_ssthresh_bytes;
};

struct tcp_shift_lwip_cc_adapter {
    struct tcp_shift_lwip_cc_hook hook;
    struct tcp_shift_cc controller;
    struct tcp_shift_reno_state reno;
    struct tcp_shift_lwip_cc_stats *stats;
    struct tcp_pcb *pcb;
    void *delivery_slots;
    uint64_t delivered_bytes;
    uint64_t delivered_mstamp_ns;
    uint64_t delivery_last_event_ns;
    uint64_t rate_first_tx_mstamp_ns;
    uint64_t app_limited_until_bytes;
    uint64_t pacing_rate_bytes_per_sec;
    uint64_t pacing_next_send_ns;
    uint64_t pacing_flow_id;
    uint32_t pacing_generation;
    uint16_t delivery_capacity;
    uint16_t delivery_live;
    unsigned pacing_scheduled;
    unsigned bound;
    unsigned heap_owned;
};

int tcp_shift_lwip_cc_adapter_bind(struct tcp_shift_lwip_cc_adapter *adapter,
                                   struct tcp_pcb *pcb,
                                   struct tcp_shift_lwip_cc_stats *stats);
void tcp_shift_lwip_cc_adapter_unbind(struct tcp_shift_lwip_cc_adapter *adapter);

/* Bridge/runtime integration. The adapter sees only an opaque pacing service;
 * timerfd/epoll remain runtime-owned. Configuration must happen before paced
 * child PCBs are bound and must be cleared after those PCBs are destroyed. */
int tcp_shift_lwip_cc_configure_pacer(
    const struct tcp_shift_lwip_cc_pacer_ops *ops,
    void *arg);
int tcp_shift_lwip_cc_clear_pacer(void);

/* Generation-safe timer release entrypoint. Stale releases are ignored. */
int tcp_shift_lwip_cc_resume_paced(uint64_t flow_id,
                                   uint32_t generation,
                                   uint64_t actual_release_ns);

/* Bridge integration point. The tcp_shift_bridge target compiles its raw-API
 * tcp_accept() call to this wrapper. The wrapper keeps the original accept
 * callback but binds a controller to the established child PCB first. */
void tcp_shift_lwip_cc_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);

/* Called by the bridge only when the backend application has no bytes ready
 * while the public TCP has transport capacity. The adapter applies the final
 * transport checks before setting a Linux-style delivered+inflight marker. */
void tcp_shift_lwip_cc_mark_app_limited(struct tcp_pcb *pcb);

const struct tcp_shift_lwip_cc_stats *tcp_shift_lwip_cc_get_stats(void);

#endif /* TCP_SHIFT_LWIP_CC_ADAPTER_H */
