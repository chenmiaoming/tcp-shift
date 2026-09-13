#ifndef TCP_SHIFT_RUNTIME_LWIP_LOOP_H
#define TCP_SHIFT_RUNTIME_LWIP_LOOP_H

#include <stdint.h>

#include "lwip/l3_tun.h"

struct tcp_shift_lwip_loop {
    int epoll_fd;
    struct tcp_shift_l3_tun *l3;
    uint32_t tun_events;
    uint64_t wait_calls;
    uint64_t ready_wakeups;
    uint64_t timeout_wakeups;
    uint64_t eintr_wakeups;
    uint64_t tun_readable_wakeups;
    uint64_t tun_writable_wakeups;
};

int tcp_shift_lwip_loop_init(struct tcp_shift_lwip_loop *loop,
                             struct tcp_shift_l3_tun *l3);
void tcp_shift_lwip_loop_close(struct tcp_shift_lwip_loop *loop);

/* Wait for one readiness/timer cycle. Returns 0 on success and -1 on error. */
int tcp_shift_lwip_loop_run_once(struct tcp_shift_lwip_loop *loop);

#endif /* TCP_SHIFT_RUNTIME_LWIP_LOOP_H */
