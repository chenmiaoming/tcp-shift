#ifndef TCP_SHIFT_LWIP_CC_HOOKS_H
#define TCP_SHIFT_LWIP_CC_HOOKS_H

#include "lwip/opt.h"

#if LWIP_TCP

#include "lwip/tcp.h"

#if LWIP_TCP_PCB_NUM_EXT_ARGS != 1
#error tcp-shift requires exactly one TCP PCB ext-arg slot for CC integration
#endif

#define TCP_SHIFT_LWIP_CC_EXT_ARG_ID 0U

/*
 * This is the only ABI visible to the patched lwIP congestion-policy sites.
 * It deliberately does not include src/cc headers: lwIP sees opaque callbacks,
 * while src/lwip/cc_adapter.c translates these transport events into the pure-C
 * controller interface. Unbound PCBs keep native lwIP congestion control.
 */
struct tcp_shift_lwip_cc_hook_ops {
    int (*on_ack)(void *arg, struct tcp_pcb *pcb, tcpwnd_size_t acked_bytes);
    int (*on_loss)(void *arg, struct tcp_pcb *pcb, tcpwnd_size_t lost_bytes);
    int (*on_timeout)(void *arg, struct tcp_pcb *pcb);
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

#endif /* LWIP_TCP */

#endif /* TCP_SHIFT_LWIP_CC_HOOKS_H */
