#ifndef TCP_SHIFT_LWIP_PROBE_LISTENER_H
#define TCP_SHIFT_LWIP_PROBE_LISTENER_H

#include <stdint.h>

#include "lwip/tcp.h"

struct tcp_shift_probe_listener {
    struct tcp_pcb *pcb;
    uint16_t port;
    uint64_t accepts;
    uint64_t rx_bytes;
    uint64_t errors;
};

int tcp_shift_probe_listener_start(struct tcp_shift_probe_listener *listener,
                                   uint16_t port);
void tcp_shift_probe_listener_stop(struct tcp_shift_probe_listener *listener);

#endif /* TCP_SHIFT_LWIP_PROBE_LISTENER_H */
