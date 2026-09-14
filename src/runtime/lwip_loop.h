#ifndef TCP_SHIFT_RUNTIME_LWIP_LOOP_H
#define TCP_SHIFT_RUNTIME_LWIP_LOOP_H

#include <stddef.h>
#include <stdint.h>

#include "lwip/l3_tun.h"
#include "runtime/pacer.h"

#define TCP_SHIFT_LWIP_LOOP_PACER_MAX_EVENTS 4096U

struct tcp_shift_lwip_loop;
struct tcp_shift_lwip_loop_watch;

typedef int (*tcp_shift_lwip_loop_watch_fn)(void *arg, uint32_t events);
typedef int (*tcp_shift_lwip_loop_pacer_release_fn)(
    void *arg,
    const struct tcp_shift_pacer_event *event,
    uint64_t actual_release_ns);

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

    struct tcp_shift_pacer pacer;
    struct tcp_shift_lwip_loop_watch pacer_watch;
    tcp_shift_lwip_loop_pacer_release_fn pacer_release;
    void *pacer_release_arg;

    uint64_t wait_calls;
    uint64_t ready_wakeups;
    uint64_t timeout_wakeups;
    uint64_t eintr_wakeups;
    uint64_t tun_readable_wakeups;
    uint64_t tun_writable_wakeups;
    uint64_t pacing_wakeups;
    uint64_t pacing_release_callbacks;
    uint64_t pacing_callback_errors;
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

int tcp_shift_lwip_loop_set_pacer_release(
    struct tcp_shift_lwip_loop *loop,
    tcp_shift_lwip_loop_pacer_release_fn callback,
    void *arg);
int tcp_shift_lwip_loop_pacer_schedule(
    struct tcp_shift_lwip_loop *loop,
    const struct tcp_shift_pacer_event *event);
int tcp_shift_lwip_loop_pacer_cancel(struct tcp_shift_lwip_loop *loop,
                                     uint64_t flow_id,
                                     uint32_t generation,
                                     size_t *cancelled);
const struct tcp_shift_pacer_stats *tcp_shift_lwip_loop_pacer_stats(
    const struct tcp_shift_lwip_loop *loop);

/* Wait for one readiness/timer cycle. Returns 0 on success and -1 on error. */
int tcp_shift_lwip_loop_run_once(struct tcp_shift_lwip_loop *loop);

#endif /* TCP_SHIFT_RUNTIME_LWIP_LOOP_H */
