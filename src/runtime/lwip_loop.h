#ifndef TCP_SHIFT_RUNTIME_LWIP_LOOP_H
#define TCP_SHIFT_RUNTIME_LWIP_LOOP_H

#include <stdint.h>

#include "lwip/l3_tun.h"

struct tcp_shift_lwip_loop {
    int epoll_fd;
    struct tcp_shift_l3_tun *l3;
    uint32_t tun_events;
};

int tcp_shift_lwip_loop_init(struct tcp_shift_lwip_loop *loop,
                             struct tcp_shift_l3_tun *l3);
void tcp_shift_lwip_loop_close(struct tcp_shift_lwip_loop *loop);

/* Wait for one readiness/timer cycle. Returns 0 on success and -1 on error. */
int tcp_shift_lwip_loop_run_once(struct tcp_shift_lwip_loop *loop);

#endif /* TCP_SHIFT_RUNTIME_LWIP_LOOP_H */
