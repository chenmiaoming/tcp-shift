#ifndef TCP_SHIFT_LWIP_BBR3_BINDING_H
#define TCP_SHIFT_LWIP_BBR3_BINDING_H

#include <stdint.h>

#include "cc/bbr3_controller.h"
#include "lwip/cc_adapter.h"

/*
 * Qualification-only, per-flow BBRv3 observation binding.
 *
 * Unlike the live compact-BBR sidecar, this never installs a lwIP hook,
 * claims recovery, modifies pcb->cwnd/ssthresh, or schedules a packet.
 * It translates actual SACK-aware lwIP delivery snapshots into independent
 * BBRv3 policy *candidates*. The caller must not apply a candidate to TCP
 * until validated RACK/SACK loss, ECN CE byte accounting and recovery
 * conservation are integrated and separately qualified.
 *
 * The binding deliberately has no fallback from coarse on_loss/on_ecn
 * notifications to invented BBRv3 loss/CE byte samples.
 */
struct tcp_shift_lwip_bbr3_binding {
    struct tcp_shift_bbr3_controller controller;
    const struct tcp_shift_lwip_cc_adapter *adapter;
    const struct tcp_pcb *pcb;
    uint64_t last_delivered_bytes;
    uint64_t last_ack_ns;
    uint64_t accepted_samples;
    uint64_t rejected_samples;
    uint8_t initialized;
};

/* Requires fully initialized lwIP cwnd and SACK-aware delivery accounting.
 * Do not bind in the passive-open callback while cwnd is the lwIP sentinel.
 * The binding owns no transport pointer or heap allocation. */
int tcp_shift_lwip_bbr3_binding_init(
    struct tcp_shift_lwip_bbr3_binding *binding,
    const struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_pcb *pcb,
    uint32_t flow_seed,
    uint64_t now_ns);

/* Consume exactly one newly delivered, valid public-leg ACK or SACK snapshot.
 * No assumed loss, no CE attribution, no guessed cwnd-limited status.
 * No PCB mutation, even on success. */
int tcp_shift_lwip_bbr3_binding_on_ack(
    struct tcp_shift_lwip_bbr3_binding *binding,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_bbr3_controller_policy *candidate);

#endif
