#include "runtime/lwip_loop.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "lwip/init.h"

#define NS_PER_MS 1000000ULL

struct release_capture {
    unsigned count;
    struct tcp_shift_pacer_event event;
    uint64_t actual_release_ns;
};

static void fail(const char *message)
{
    fprintf(stderr, "pacer loop contract failed: %s (errno=%d %s)\n",
            message, errno, strerror(errno));
    exit(1);
}

static void require(int condition, const char *message)
{
    if (!condition) {
        fail(message);
    }
}

static uint64_t monotonic_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        fail("clock_gettime");
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int timer_is_armed(int fd)
{
    struct itimerspec current;

    if (timerfd_gettime(fd, &current) < 0) {
        fail("timerfd_gettime");
    }
    return current.it_value.tv_sec != 0 || current.it_value.tv_nsec != 0;
}

static int capture_release(void *arg,
                           const struct tcp_shift_pacer_event *event,
                           uint64_t actual_release_ns)
{
    struct release_capture *capture = arg;

    if (capture == NULL || event == NULL) {
        errno = EINVAL;
        return -1;
    }
    capture->count++;
    capture->event = *event;
    capture->actual_release_ns = actual_release_ns;
    return 0;
}

int main(void)
{
    struct tcp_shift_l3_tun l3;
    struct tcp_shift_lwip_loop loop;
    struct tcp_shift_pacer_event event;
    struct release_capture capture;
    const struct tcp_shift_pacer_stats *stats;
    uint64_t deadline_ns;
    uint64_t stop_ns;
    uint64_t idle_pacing_wakeups;
    uint64_t idle_timeout_wakeups;
    int fake_tun_fd;

    memset(&l3, 0, sizeof(l3));
    memset(&loop, 0, sizeof(loop));
    memset(&capture, 0, sizeof(capture));
    loop.epoll_fd = -1;
    loop.pacer.timer_fd = -1;

    lwip_init();

    fake_tun_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fake_tun_fd < 0) {
        fail("eventfd");
    }
    l3.tun_fd = fake_tun_fd;
    l3.attached = 1U;

    if (tcp_shift_lwip_loop_init(&loop, &l3) < 0) {
        close(fake_tun_fd);
        fail("loop init");
    }
    require(loop.pacer_watch.registered != 0U,
            "pacer timerfd is not registered with epoll owner");
    require(tcp_shift_pacer_fd(&loop.pacer) >= 0,
            "loop pacer timerfd missing");
    require(!timer_is_armed(tcp_shift_pacer_fd(&loop.pacer)),
            "loop pacer must start disarmed");

    if (tcp_shift_lwip_loop_set_pacer_release(&loop, capture_release,
                                               &capture) < 0) {
        tcp_shift_lwip_loop_close(&loop);
        close(fake_tun_fd);
        fail("set pacer release callback");
    }

    deadline_ns = monotonic_now_ns() + 20U * NS_PER_MS;
    event.deadline_ns = deadline_ns;
    event.flow_id = 42U;
    event.generation = 7U;
    event.bytes = 1460U;
    if (tcp_shift_lwip_loop_pacer_schedule(&loop, &event) < 0) {
        tcp_shift_lwip_loop_close(&loop);
        close(fake_tun_fd);
        fail("schedule pacing event");
    }
    require(timer_is_armed(tcp_shift_pacer_fd(&loop.pacer)),
            "scheduled event did not arm timerfd");

    stop_ns = deadline_ns + 500U * NS_PER_MS;
    while (capture.count == 0U && monotonic_now_ns() < stop_ns) {
        if (tcp_shift_lwip_loop_run_once(&loop) < 0) {
            tcp_shift_lwip_loop_close(&loop);
            close(fake_tun_fd);
            fail("run loop for pacing event");
        }
    }
    require(capture.count == 1U, "pacing callback was not released");
    require(capture.event.flow_id == 42U && capture.event.generation == 7U,
            "wrong pacing event released");
    require(capture.event.bytes == 1460U, "released byte count mismatch");
    require(capture.actual_release_ns >= deadline_ns,
            "pacing event released before deadline");
    require(loop.pacing_wakeups == 1U,
            "one pacing deadline must cause one pacing wakeup");
    require(loop.pacing_release_callbacks == 1U,
            "pacing release callback count mismatch");
    require(loop.pacing_callback_errors == 0U,
            "pacing callback error count nonzero");

    stats = tcp_shift_lwip_loop_pacer_stats(&loop);
    require(stats != NULL, "loop pacer stats unavailable");
    require(stats->timerfd_creates == 1U, "loop created more than one timerfd");
    require(stats->timer_expirations >= 1U,
            "loop did not consume timerfd expiration");
    require(stats->released_events == 1U && stats->released_bytes == 1460U,
            "loop release telemetry mismatch");
    require(stats->heap_current == 0U, "pacer heap did not drain");
    require(!timer_is_armed(tcp_shift_pacer_fd(&loop.pacer)),
            "drained loop pacer timerfd did not disarm");

    idle_pacing_wakeups = loop.pacing_wakeups;
    idle_timeout_wakeups = loop.timeout_wakeups;
    if (tcp_shift_lwip_loop_run_once(&loop) < 0) {
        tcp_shift_lwip_loop_close(&loop);
        close(fake_tun_fd);
        fail("idle loop cycle");
    }
    require(loop.pacing_wakeups == idle_pacing_wakeups,
            "disarmed pacer caused an idle pacing wakeup");
    require(!timer_is_armed(tcp_shift_pacer_fd(&loop.pacer)),
            "idle pacer rearmed without work");

    printf("pacer_loop_contract=ok timerfd_creates=%llu pacing_wakeups=%llu "
           "release_callbacks=%llu timer_expirations=%llu released_bytes=%llu "
           "heap_peak=%zu lateness_ns=%llu idle_pacing_wakeups_delta=%llu "
           "idle_timeout_wakeups_delta=%llu\n",
           (unsigned long long)stats->timerfd_creates,
           (unsigned long long)loop.pacing_wakeups,
           (unsigned long long)loop.pacing_release_callbacks,
           (unsigned long long)stats->timer_expirations,
           (unsigned long long)stats->released_bytes,
           stats->heap_peak,
           (unsigned long long)stats->last_lateness_ns,
           (unsigned long long)(loop.pacing_wakeups - idle_pacing_wakeups),
           (unsigned long long)(loop.timeout_wakeups - idle_timeout_wakeups));

    tcp_shift_lwip_loop_close(&loop);
    close(fake_tun_fd);
    return 0;
}
