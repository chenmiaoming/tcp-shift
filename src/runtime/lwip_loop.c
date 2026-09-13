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

static int tcp_shift_lwip_loop_sync_interest(struct tcp_shift_lwip_loop *loop)
{
    struct epoll_event event;
    uint32_t wanted = tcp_shift_lwip_loop_tun_events(loop);

    if (wanted == loop->tun_events) {
        return 0;
    }

    memset(&event, 0, sizeof(event));
    event.events = wanted;
    event.data.fd = loop->l3->tun_fd;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, loop->l3->tun_fd,
                  &event) < 0) {
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
    struct epoll_event event;

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
    loop->tun_events = tcp_shift_lwip_loop_tun_events(loop);

    memset(&event, 0, sizeof(event));
    event.events = loop->tun_events;
    event.data.fd = l3->tun_fd;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, l3->tun_fd, &event) < 0) {
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
}

int tcp_shift_lwip_loop_run_once(struct tcp_shift_lwip_loop *loop)
{
    struct epoll_event event;
    int ready;

    if (loop == NULL || loop->epoll_fd < 0 || loop->l3 == NULL) {
        errno = EINVAL;
        return -1;
    }

    ready = epoll_wait(loop->epoll_fd, &event, 1,
                       tcp_shift_lwip_loop_timeout_ms());
    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }

    if (ready > 0) {
        if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
            errno = EIO;
            return -1;
        }

        if ((event.events & EPOLLIN) != 0U) {
            unsigned budget;

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

        if ((event.events & EPOLLOUT) != 0U &&
            tcp_shift_l3_tun_flush_tx(loop->l3) < 0) {
            return -1;
        }
    }

    (void)sys_check_timeouts();
    return tcp_shift_lwip_loop_sync_interest(loop);
}
