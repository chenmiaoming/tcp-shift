#ifndef TCP_SHIFT_LWIP_CC_HOOKS_H
#define TCP_SHIFT_LWIP_CC_HOOKS_H

#include "lwip/opt.h"

#if LWIP_TCP

#include "lwip/tcp.h"

#if LWIP_TCP_PCB_NUM_EXT_ARGS < 2
#error tcp-shift requires PCB ext-arg slots for CC and transport memory integration
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
 *
 * on_segment_send_eligible is the P5c pacing gate. It runs only after native
 * window and Nagle checks but before tcp_output_segment() mutates/sends the
 * current data segment. Returning zero leaves the segment on pcb->unsent; a
 * later runtime deadline simply calls native tcp_output() again.
 *
 * Recovery ownership is explicit at this boundary. Native lwIP continues to
 * own TF_INFR inflation/deflation for Reno/CUBIC. An internal controller may
 * claim recovery cwnd ownership after a handled fast loss; while that claim is
 * active, patched lwIP suppresses only its native recovery cwnd inflation and
 * delegates the exit transition before clearing TF_INFR.
 */
struct tcp_shift_lwip_cc_hook_ops {
    int (*on_ack)(void *arg, struct tcp_pcb *pcb, tcpwnd_size_t acked_bytes);
    int (*on_loss)(void *arg, struct tcp_pcb *pcb, tcpwnd_size_t lost_bytes);
    int (*on_timeout)(void *arg, struct tcp_pcb *pcb);
    int (*on_recovery_exit)(void *arg, struct tcp_pcb *pcb);
    int (*on_segment_send_eligible)(void *arg,
                                    struct tcp_pcb *pcb,
                                    u16_t payload_bytes);
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
    u32_t recovery_enter_events;
    u32_t recovery_exit_events;
    u8_t recovery_active;
    u8_t recovery_exit_pending;
    u8_t recovery_controller_owned;
};

static inline struct tcp_shift_lwip_cc_hook *
tcp_shift_lwip_cc_hook_get(const struct tcp_pcb *pcb)
{
    return (struct tcp_shift_lwip_cc_hook *)tcp_ext_arg_get(
        pcb, (u8_t)TCP_SHIFT_LWIP_CC_EXT_ARG_ID);
}

static inline void
tcp_shift_lwip_cc_hook_recovery_mark_enter(struct tcp_shift_lwip_cc_hook *hook)
{
    if (hook == NULL || hook->recovery_active != 0U) {
        return;
    }
    hook->recovery_active = 1U;
    hook->recovery_exit_pending = 0U;
    hook->recovery_enter_events++;
}

static inline void
tcp_shift_lwip_cc_hook_recovery_mark_exit(struct tcp_shift_lwip_cc_hook *hook)
{
    if (hook == NULL || hook->recovery_active == 0U) {
        return;
    }
    hook->recovery_active = 0U;
    hook->recovery_exit_pending = 1U;
    hook->recovery_controller_owned = 0U;
    hook->recovery_exit_events++;
}

static inline void
tcp_shift_lwip_cc_hook_recovery_reset(struct tcp_shift_lwip_cc_hook *hook)
{
    if (hook == NULL) {
        return;
    }
    hook->recovery_active = 0U;
    hook->recovery_exit_pending = 0U;
    hook->recovery_controller_owned = 0U;
}

static inline unsigned
tcp_shift_lwip_cc_hook_recovery_is_active(
    const struct tcp_shift_lwip_cc_hook *hook)
{
    return hook != NULL && hook->recovery_active != 0U ? 1U : 0U;
}

static inline unsigned
tcp_shift_lwip_cc_hook_recovery_controller_owned(const struct tcp_pcb *pcb)
{
    const struct tcp_shift_lwip_cc_hook *hook =
        tcp_shift_lwip_cc_hook_get(pcb);

    return hook != NULL && hook->recovery_active != 0U &&
                   hook->recovery_controller_owned != 0U
               ? 1U
               : 0U;
}

static inline unsigned
tcp_shift_lwip_cc_hook_take_recovery_exit(struct tcp_shift_lwip_cc_hook *hook)
{
    unsigned pending;

    if (hook == NULL) {
        return 0U;
    }
    pending = hook->recovery_exit_pending != 0U ? 1U : 0U;
    hook->recovery_exit_pending = 0U;
    return pending;
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
    int handled;

    if (hook == NULL || hook->ops == NULL || hook->ops->on_loss == NULL) {
        return 0;
    }
    handled = hook->ops->on_loss(hook->arg, pcb, lost_bytes) != 0;
    if (handled != 0) {
        tcp_shift_lwip_cc_hook_recovery_mark_enter(hook);
    }
    return handled;
}

static inline int
tcp_shift_lwip_cc_hook_recovery_exit(struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);
    int handled = 0;

    if (hook == NULL) {
        return 0;
    }
    if (hook->recovery_active != 0U &&
        hook->recovery_controller_owned != 0U &&
        hook->ops != NULL && hook->ops->on_recovery_exit != NULL) {
        handled = hook->ops->on_recovery_exit(hook->arg, pcb) != 0;
    }
    tcp_shift_lwip_cc_hook_recovery_mark_exit(hook);
    return handled;
}

static inline int
tcp_shift_lwip_cc_hook_timeout(struct tcp_pcb *pcb)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);
    unsigned controller_owned;
    int handled;

    if (hook == NULL || hook->ops == NULL || hook->ops->on_timeout == NULL) {
        return 0;
    }
    controller_owned = hook->recovery_controller_owned != 0U ? 1U : 0U;
    handled = hook->ops->on_timeout(hook->arg, pcb) != 0;

    /* RTO supersedes an observed fast-recovery episode even if the controller
     * callback fails and the transport falls back to native loss handling. */
    if (handled != 0 && controller_owned != 0U) {
        tcp_clear_flags(pcb, TF_INFR);
    }
    if (handled != 0 || hook->recovery_active != 0U ||
        hook->recovery_controller_owned != 0U) {
        tcp_shift_lwip_cc_hook_recovery_reset(hook);
    }
    return handled;
}

static inline int
tcp_shift_lwip_cc_hook_segment_send_eligible(struct tcp_pcb *pcb,
                                               u16_t payload_bytes)
{
    struct tcp_shift_lwip_cc_hook *hook = tcp_shift_lwip_cc_hook_get(pcb);

    if (hook == NULL || hook->ops == NULL ||
        hook->ops->on_segment_send_eligible == NULL) {
        return 1;
    }
    return hook->ops->on_segment_send_eligible(hook->arg, pcb,
                                                payload_bytes) != 0;
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
