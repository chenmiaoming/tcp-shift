#ifndef TCP_SHIFT_RUNTIME_LWIP_LOOP_H
#define TCP_SHIFT_RUNTIME_LWIP_LOOP_H

#include <stdint.h>

#include "lwip/l3_tun.h"

struct tcp_shift_lwip_loop;
struct tcp_shift_lwip_loop_watch;

typedef int (*tcp_shift_lwip_loop_watch_fn)(void *arg, uint32_t events);

/*
 * A watch is caller-owned and must remain alive while registered. run_once()
 * asks epoll for one ready fd at a time, so a callback may unregister and free
 * its own enclosing object before returning without leaving another event in
 * the current batch pointing at freed memory. This preserves the single-owner
 * lifetime model while P2 introduces per-flow backend sockets.
 */
struct tcp_shift_lwip_loop_watch {
    int fd;
    uint32_t events;
    tcp_shift_lwip_loop_watch_fn callback;
    void *arg;
    unsigned registered;
};

struct tcp_shift_lwip_loop {
    int epoll_fd;
    struct tcp_shift_l3_tun *l3;
    struct tcp_shift_lwip_loop_watch tun_watch;
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

int tcp_shift_lwip_loop_watch_add(struct tcp_shift_lwip_loop *loop,
                                  struct tcp_shift_lwip_loop_watch *watch,
                                  int fd,
                                  uint32_t events,
                                  tcp_shift_lwip_loop_watch_fn callback,
                                  void *arg);
int tcp_shift_lwip_loop_watch_mod(struct tcp_shift_lwip_loop *loop,
                                  struct tcp_shift_lwip_loop_watch *watch,
                                  uint32_t events);
int tcp_shift_lwip_loop_watch_remove(struct tcp_shift_lwip_loop *loop,
                                     struct tcp_shift_lwip_loop_watch *watch);

/* Wait for one readiness/timer cycle. Returns 0 on success and -1 on error. */
int tcp_shift_lwip_loop_run_once(struct tcp_shift_lwip_loop *loop);

#endif /* TCP_SHIFT_RUNTIME_LWIP_LOOP_H */
