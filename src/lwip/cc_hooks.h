#ifndef TCP_SHIFT_LWIP_CC_HOOKS_H
#define TCP_SHIFT_LWIP_CC_HOOKS_H

#include "lwip/opt.h"

#if LWIP_TCP

#include "lwip/tcp.h"

#if LWIP_TCP_PCB_NUM_EXT_ARGS != 1
#error tcp-shift requires exactly one TCP PCB ext-arg slot for transport integration
#endif

#define TCP_SHIFT_LWIP_CC_EXT_ARG_ID 0U

/*
 * This is the only project ABI visible to patched lwIP TCP sites. It
 * deliberately does not include src/cc headers. The lwIP-specific adapter
 * translates congestion-policy events and P5 segment-lifecycle observations
 * into project-owned state. Unbound PCBs keep native lwIP behavior.
 *
 * Segment identity is opaque here so this public hook header does not expose
 * lwIP's private struct tcp_seg layout. The adapter may use the pointer as a
 * stable key while that segment moves between unsent/unacked queues during
 * retransmission. seq_start is the data sequence number in host byte order;
 * carrying it explicitly lets the adapter account partial ACKs without
 * dereferencing the private segment object.
 */
struct tcp_shift_lwip_cc_hook_ops {
    int (*on_ack)(void *arg, struct tcp_pcb *pcb, tcpwnd_size_t acked_bytes);
    int (*on_loss)(void *arg, struct tcp_pcb *pcb, tcpwnd_size_t lost_bytes);
    int (*on_timeout)(void *arg, struct tcp_pcb *pcb);
    void (*on_segment_tx)(void *arg,
                          struct tcp_pcb *pcb,
                          const void *segment,
                          u32_t seq_start,
                          u16_t payload_bytes);
    void (*on_segment_acked)(void *arg,
                             struct tcp_pcb *pcb,
                             const void *segment,
                             u16_t payload_bytes);
};

struct tcp_shift_lwip_cc_hook {
    const struct tcp_shift_lwip_cc_hook_ops *ops;
    void *arg;
};

static inline struct tcp_shift_lwip_cc_hook *
tcp_shift_lwip_cc_hook_get(const struct tcp_pcb *pcb)
{
    return (struct tcp_shift_lwip_cc_hook *)tcp_ext_arg_get(
        pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID);
}

static inline int
tcp_shift_lwip_cc_hook_ack(struct tcp_pcb *pcb, tcpwnd_size_t acked_bytes)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);

    if (hook == NULL || hook->ops == NULL || hook->ops->on_ack == NULL) {
        return 0;
    }
    return hook->ops->on_ack(hook->arg, pcb, acked_bytes) != 0;
}

static inline int
tcp_shift_lwip_cc_hook_loss(struct tcp_pcb *pcb, tcpwnd_size_t lost_bytes)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);

    if (hook == NULL || hook->ops == NULL || hook->ops->on_loss == NULL) {
        return 0;
    }
    return hook->ops->on_loss(hook->arg, pcb, lost_bytes) != 0;
}

static inline int
tcp_shift_lwip_cc_hook_timeout(struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);

    if (hook == NULL || hook->ops == NULL || hook->ops->on_timeout == NULL) {
        return 0;
    }
    return hook->ops->on_timeout(hook->arg, pcb) != 0;
}

static inline void
tcp_shift_lwip_cc_hook_segment_tx(struct tcp_pcb *pcb,
                                   const void *segment,
                                   u32_t seq_start,
                                   u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);

    if (hook == NULL || hook->ops == NULL ||
        hook->ops->on_segment_tx == NULL) {
        return;
    }
    hook->ops->on_segment_tx(hook->arg, pcb, segment, seq_start,
                             payload_bytes);
}

static inline void
tcp_shift_lwip_cc_hook_segment_acked(struct tcp_pcb *pcb,
                                      const void *segment,
                                      u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);

    if (hook == NULL || hook->ops == NULL ||
        hook->ops->on_segment_acked == NULL) {
        return;
    }
    hook->ops->on_segment_acked(hook->arg, pcb, segment, payload_bytes);
}

#endif /* LWIP_TCP */

#endif /* TCP_SHIFT_LWIP_CC_HOOKS_H */
