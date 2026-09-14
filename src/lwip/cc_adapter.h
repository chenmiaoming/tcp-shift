#ifndef TCP_SHIFT_LWIP_CC_ADAPTER_H
#define TCP_SHIFT_LWIP_CC_ADAPTER_H

#include <stdint.h>

#include "cc/cc.h"
#include "cc/reno.h"
#include "lwip/cc_hooks.h"
#include "lwip/tcp.h"

struct tcp_shift_lwip_cc_stats {
    uint64_t bindings;
    uint64_t bind_failures;
    uint64_t ack_events;
    uint64_t loss_events;
    uint64_t timeout_events;
    uint64_t policy_updates;
    uint64_t controller_errors;
    uint32_t last_cwnd_bytes;
    uint32_t last_ssthresh_bytes;
};

struct tcp_shift_lwip_cc_adapter {
    struct tcp_shift_lwip_cc_hook hook;
    struct tcp_shift_cc controller;
    struct tcp_shift_reno_state reno;
    struct tcp_shift_lwip_cc_stats *stats;
    struct tcp_pcb *pcb;
    unsigned bound;
    unsigned heap_owned;
};

int tcp_shift_lwip_cc_adapter_bind(struct tcp_shift_lwip_cc_adapter *adapter,
                                   struct tcp_pcb *pcb,
                                   struct tcp_shift_lwip_cc_stats *stats);
void tcp_shift_lwip_cc_adapter_unbind(struct tcp_shift_lwip_cc_adapter *adapter);

/* Bridge integration point. The tcp_shift_bridge target compiles its raw-API
 * tcp_accept() call to this wrapper. The wrapper keeps the original accept
 * callback but binds a controller to the established child PCB first. */
void tcp_shift_lwip_cc_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);

const struct tcp_shift_lwip_cc_stats *tcp_shift_lwip_cc_get_stats(void);

#endif /* TCP_SHIFT_LWIP_CC_ADAPTER_H */
