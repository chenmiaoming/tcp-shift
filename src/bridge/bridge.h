#ifndef TCP_SHIFT_BRIDGE_BRIDGE_H
#define TCP_SHIFT_BRIDGE_BRIDGE_H

#include <stdint.h>

#include "lwip/tcp.h"
#include "runtime/lwip_loop.h"

#define TCP_SHIFT_BRIDGE_BACKEND_SOCKET_BUFFER 16384U

struct tcp_shift_bridge_flow;

struct tcp_shift_bridge {
    struct tcp_shift_lwip_loop *loop;
    struct tcp_pcb *listener;
    struct tcp_shift_bridge_flow *flows;
    uint16_t public_port;
    uint16_t backend_port;
    uint64_t accepts;
    uint64_t backend_connects;
    uint64_t public_to_backend_bytes;
    uint64_t backend_to_public_bytes;
    uint64_t active_flows;
    uint64_t peak_active_flows;
    uint64_t pending_public_bytes;
    uint64_t peak_pending_public_bytes;
    uint64_t backend_write_blocked_events;
    uint64_t backend_read_blocked_events;
    uint64_t backend_read_blocked_sndbuf_zero_events;
    uint64_t backend_read_blocked_tcp_write_mem_events;
    uint64_t backend_failures;
    uint64_t public_errors;
    uint32_t backend_socket_sndbuf_bytes;
    uint32_t backend_socket_rcvbuf_bytes;
};

int tcp_shift_bridge_start_ipv4(struct tcp_shift_bridge *bridge,
                                struct tcp_shift_lwip_loop *loop,
                                uint16_t public_port,
                                uint16_t backend_port);
int tcp_shift_bridge_start_ipv6(struct tcp_shift_bridge *bridge,
                                struct tcp_shift_lwip_loop *loop,
                                uint16_t public_port,
                                uint16_t backend_port);
void tcp_shift_bridge_stop(struct tcp_shift_bridge *bridge);

#endif /* TCP_SHIFT_BRIDGE_BRIDGE_H */
