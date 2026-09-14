#include "runtime/lwip_loop.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include "lwip/timeouts.h"

#define TCP_SHIFT_TUN_RX_BUDGET 64U

static uint32_t tcp_shift_lwip_loop_tun_events(
    const struct tcp_shift_lwip_loop *loop)
{
    uint32_t events = EPOLLIN;

    if (tcp_shift_l3_tun_wants_write(loop->l3)) {
        events |= EPOLLOUT;
    }
    return events;
}

int tcp_shift_lwip_loop_watch_add(struct tcp_shift_lwip_loop *loop,
                                  struct tcp_shift_lwip_loop_watch *watch,
                                  int fd,
                                  uint32_t events,
                                  tcp_shift_lwip_loop_watch_fn callback,
                                  void *arg)
{
    struct epoll_event event;

    if (loop == NULL || loop->epoll_fd < 0 || watch == NULL || fd < 0 ||
        callback == NULL || watch->registered != 0U) {
        errno = EINVAL;
        return -1;
    }

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.ptr = watch;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        return -1;
    }

    watch->fd = fd;
    watch->events = events;
    watch->callback = callback;
    watch->arg = arg;
    watch->registered = 1U;
    return 0;
}

int tcp_shift_lwip_loop_watch_mod(struct tcp_shift_lwip_loop *loop,
                                  struct tcp_shift_lwip_loop_watch *watch,
                                  uint32_t events)
{
    struct epoll_event event;

    if (loop == NULL || loop->epoll_fd < 0 || watch == NULL ||
        watch->registered == 0U || watch->fd < 0 || watch->callback == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (watch->events == events) {
        return 0;
    }

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.ptr = watch;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, watch->fd, &event) < 0) {
        return -1;
    }
    watch->events = events;
    return 0;
}

int tcp_shift_lwip_loop_watch_remove(struct tcp_shift_lwip_loop *loop,
                                     struct tcp_shift_lwip_loop_watch *watch)
{
    if (loop == NULL || loop->epoll_fd < 0 || watch == NULL ||
        watch->registered == 0U) {
        errno = EINVAL;
        return -1;
    }

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, watch->fd, NULL) < 0) {
        return -1;
    }
    watch->fd = -1;
    watch->events = 0U;
    watch->callback = NULL;
    watch->arg = NULL;
    watch->registered = 0U;
    return 0;
}

static int tcp_shift_lwip_loop_tun_ready(void *arg, uint32_t events)
{
    struct tcp_shift_lwip_loop *loop = arg;

    if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
        errno = EIO;
        return -1;
    }

    if ((events & EPOLLIN) != 0U) {
        unsigned budget;

        loop->tun_readable_wakeups++;
        for (budget = 0; budget < TCP_SHIFT_TUN_RX_BUDGET; budget++) {
            int result = tcp_shift_l3_tun_rx_once(loop->l3);

            if (result < 0) {
                return -1;
            }
            if (result == 0) {
                break;
            }
        }
    }

    if ((events & EPOLLOUT) != 0U) {
        loop->tun_writable_wakeups++;
        if (tcp_shift_l3_tun_flush_tx(loop->l3) < 0) {
            return -1;
        }
    }
    return 0;
}

static int tcp_shift_lwip_loop_monotonic_ns(uint64_t *now_ns)
{
    struct timespec now;

    if (now_ns == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    *now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    return 0;
}

static int tcp_shift_lwip_loop_pacer_ready(void *arg, uint32_t events)
{
    struct tcp_shift_lwip_loop *loop = arg;
    struct tcp_shift_pacer_event event;
    uint64_t expirations;
    uint64_t now_ns;
    int result;

    if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
        errno = EIO;
        return -1;
    }
    if ((events & EPOLLIN) == 0U) {
        return 0;
    }

    loop->pacing_wakeups++;
    result = tcp_shift_pacer_consume_timer(&loop->pacer, &expirations);
    if (result < 0) {
        return -1;
    }
    if (result == 0 || expirations == 0U) {
        return 0;
    }
    if (tcp_shift_lwip_loop_monotonic_ns(&now_ns) < 0) {
        return -1;
    }

    for (;;) {
        result = tcp_shift_pacer_pop_due(&loop->pacer, now_ns, &event);
        if (result < 0) {
            return -1;
        }
        if (result == 0) {
            break;
        }
        if (loop->pacer_release == NULL) {
            errno = EIO;
            return -1;
        }
        loop->pacing_release_callbacks++;
        if (loop->pacer_release(loop->pacer_release_arg, &event, now_ns) < 0) {
            loop->pacing_callback_errors++;
            return -1;
        }
    }
    return 0;
}

static int tcp_shift_lwip_loop_sync_interest(struct tcp_shift_lwip_loop *loop)
{
    uint32_t wanted = tcp_shift_lwip_loop_tun_events(loop);

    if (wanted == loop->tun_events) {
        return 0;
    }
    if (tcp_shift_lwip_loop_watch_mod(loop, &loop->tun_watch, wanted) < 0) {
        return -1;
    }
    loop->tun_events = wanted;
    return 0;
}

static int tcp_shift_lwip_loop_timeout_ms(void)
{
    u32_t sleep_ms = sys_timeouts_sleeptime();

    if (sleep_ms == SYS_TIMEOUTS_SLEEPTIME_INFINITE) {
        return -1;
    }
    if (sleep_ms > (u32_t)INT_MAX) {
        return INT_MAX;
    }
    return (int)sleep_ms;
}

int tcp_shift_lwip_loop_init(struct tcp_shift_lwip_loop *loop,
                             struct tcp_shift_l3_tun *l3)
{
    int saved_errno;

    if (loop == NULL || l3 == NULL || l3->attached == 0U || l3->tun_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    memset(loop, 0, sizeof(*loop));
    loop->epoll_fd = -1;
    loop->tun_watch.fd = -1;
    loop->pacer.timer_fd = -1;
    loop->pacer_watch.fd = -1;

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        return -1;
    }
    loop->l3 = l3;
    loop->tun_events = tcp_shift_lwip_loop_tun_events(loop);

    if (tcp_shift_lwip_loop_watch_add(loop, &loop->tun_watch, l3->tun_fd,
                                      loop->tun_events,
                                      tcp_shift_lwip_loop_tun_ready, loop) < 0) {
        saved_errno = errno;
        goto fail;
    }
    if (tcp_shift_pacer_init(&loop->pacer,
                             TCP_SHIFT_LWIP_LOOP_PACER_MAX_EVENTS) < 0) {
        saved_errno = errno;
        goto fail;
    }
    if (tcp_shift_lwip_loop_watch_add(loop, &loop->pacer_watch,
                                      tcp_shift_pacer_fd(&loop->pacer),
                                      EPOLLIN,
                                      tcp_shift_lwip_loop_pacer_ready,
                                      loop) < 0) {
        saved_errno = errno;
        goto fail;
    }
    return 0;

fail:
    if (loop->pacer.timer_fd >= 0) {
        tcp_shift_pacer_close(&loop->pacer);
    }
    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
    }
    loop->epoll_fd = -1;
    loop->l3 = NULL;
    errno = saved_errno;
    return -1;
}

void tcp_shift_lwip_loop_close(struct tcp_shift_lwip_loop *loop)
{
    if (loop == NULL) {
        return;
    }
    if (loop->pacer.timer_fd >= 0) {
        tcp_shift_pacer_close(&loop->pacer);
    }
    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
    }
    loop->epoll_fd = -1;
    loop->l3 = NULL;
    loop->tun_events = 0U;
    loop->tun_watch.fd = -1;
    loop->tun_watch.events = 0U;
    loop->tun_watch.callback = NULL;
    loop->tun_watch.arg = NULL;
    loop->tun_watch.registered = 0U;
    loop->pacer_watch.fd = -1;
    loop->pacer_watch.events = 0U;
    loop->pacer_watch.callback = NULL;
    loop->pacer_watch.arg = NULL;
    loop->pacer_watch.registered = 0U;
    loop->pacer_release = NULL;
    loop->pacer_release_arg = NULL;
}

int tcp_shift_lwip_loop_set_pacer_release(
    struct tcp_shift_lwip_loop *loop,
    tcp_shift_lwip_loop_pacer_release_fn callback,
    void *arg)
{
    if (loop == NULL || loop->epoll_fd < 0 || loop->pacer.timer_fd < 0 ||
        callback == NULL || loop->pacer.stats.heap_current != 0U) {
        errno = EINVAL;
        return -1;
    }
    loop->pacer_release = callback;
    loop->pacer_release_arg = arg;
    return 0;
}

int tcp_shift_lwip_loop_pacer_schedule(
    struct tcp_shift_lwip_loop *loop,
    const struct tcp_shift_pacer_event *event)
{
    if (loop == NULL || loop->epoll_fd < 0 || loop->pacer.timer_fd < 0 ||
        loop->pacer_release == NULL) {
        errno = EINVAL;
        return -1;
    }
    return tcp_shift_pacer_schedule(&loop->pacer, event);
}

int tcp_shift_lwip_loop_pacer_cancel(struct tcp_shift_lwip_loop *loop,
                                     uint64_t flow_id,
                                     uint32_t generation,
                                     size_t *cancelled)
{
    if (loop == NULL || loop->epoll_fd < 0 || loop->pacer.timer_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return tcp_shift_pacer_cancel(&loop->pacer, flow_id, generation, cancelled);
}

const struct tcp_shift_pacer_stats *tcp_shift_lwip_loop_pacer_stats(
    const struct tcp_shift_lwip_loop *loop)
{
    if (loop == NULL || loop->epoll_fd < 0 || loop->pacer.timer_fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    return tcp_shift_pacer_get_stats(&loop->pacer);
}

int tcp_shift_lwip_loop_run_once(struct tcp_shift_lwip_loop *loop)
{
    struct epoll_event event;
    struct tcp_shift_lwip_loop_watch *watch;
    int ready;
    int callback_result = 0;

    if (loop == NULL || loop->epoll_fd < 0 || loop->l3 == NULL) {
        errno = EINVAL;
        return -1;
    }

    loop->wait_calls++;
    ready = epoll_wait(loop->epoll_fd, &event, 1,
                       tcp_shift_lwip_loop_timeout_ms());
    if (ready < 0) {
        if (errno == EINTR) {
            loop->eintr_wakeups++;
            return 0;
        }
        return -1;
    }

    if (ready == 0) {
        loop->timeout_wakeups++;
    } else {
        loop->ready_wakeups++;
        watch = event.data.ptr;
        if (watch == NULL || watch->registered == 0U ||
            watch->callback == NULL) {
            errno = EIO;
            return -1;
        }
        callback_result = watch->callback(watch->arg, event.events);
    }

    (void)sys_check_timeouts();
    if (callback_result < 0) {
        return -1;
    }
    return tcp_shift_lwip_loop_sync_interest(loop);
}
