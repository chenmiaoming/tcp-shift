#include "runtime/lwip_loop.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/epoll.h>
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
    if (loop == NULL || l3 == NULL || l3->attached == 0U || l3->tun_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    memset(loop, 0, sizeof(*loop));
    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        return -1;
    }
    loop->l3 = l3;
    loop->tun_watch.fd = -1;
    loop->tun_events = tcp_shift_lwip_loop_tun_events(loop);

    if (tcp_shift_lwip_loop_watch_add(loop, &loop->tun_watch, l3->tun_fd,
                                      loop->tun_events,
                                      tcp_shift_lwip_loop_tun_ready, loop) < 0) {
        int saved_errno = errno;
        close(loop->epoll_fd);
        loop->epoll_fd = -1;
        loop->l3 = NULL;
        errno = saved_errno;
        return -1;
    }
    return 0;
}

void tcp_shift_lwip_loop_close(struct tcp_shift_lwip_loop *loop)
{
    if (loop == NULL) {
        return;
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
