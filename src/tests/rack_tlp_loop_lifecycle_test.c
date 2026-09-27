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

#define NS_PER_MS UINT64_C(1000000)
#define TEST_RECOVERY_KIND UINT32_C(1)

struct recovery_capture {
    unsigned count;
    struct tcp_shift_lwip_loop_recovery_event event;
    uint64_t actual_release_ns;
};

static void fail(const char *message)
{
    fprintf(stderr, "RACK recovery loop lifecycle failed: %s (errno=%d %s)\n",
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
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int timer_is_armed(int fd)
{
    struct itimerspec current;

    if (timerfd_gettime(fd, &current) < 0) {
        fail("timerfd_gettime");
    }
    return current.it_value.tv_sec != 0 || current.it_value.tv_nsec != 0;
}

static int capture_recovery(
    void *arg,
    const struct tcp_shift_lwip_loop_recovery_event *event,
    uint64_t actual_release_ns)
{
    struct recovery_capture *capture = arg;

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
    struct tcp_shift_lwip_loop_recovery_event event;
    struct recovery_capture capture;
    const struct tcp_shift_pacer_stats *stats;
    uint64_t deadline_ns;
    uint64_t stop_ns;
    size_t cancelled = 0U;
    int fake_tun_fd;

    memset(&l3, 0, sizeof(l3));
    memset(&loop, 0, sizeof(loop));
    memset(&capture, 0, sizeof(capture));
    loop.epoll_fd = -1;
    loop.pacer.timer_fd = -1;
    loop.recovery_timer.timer_fd = -1;

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
    require(loop.recovery_timer_watch.registered != 0U,
            "recovery timerfd is not registered with epoll owner");
    require(tcp_shift_pacer_fd(&loop.recovery_timer) >= 0,
            "recovery timerfd missing");
    require(!timer_is_armed(tcp_shift_pacer_fd(&loop.recovery_timer)),
            "recovery timer must start disarmed");

    if (tcp_shift_lwip_loop_set_recovery_release(
            &loop, capture_recovery, &capture) < 0) {
        tcp_shift_lwip_loop_close(&loop);
        close(fake_tun_fd);
        fail("set recovery release callback");
    }

    /* Teardown path: queue one future recovery deadline, cancel the exact
     * flow generation, and require the process-wide timer to disarm with no
     * callback left behind. */
    deadline_ns = monotonic_now_ns() + UINT64_C(100) * NS_PER_MS;
    event.deadline_ns = deadline_ns;
    event.flow_id = UINT64_C(42);
    event.generation = UINT32_C(7);
    event.kind = TEST_RECOVERY_KIND;
    if (tcp_shift_lwip_loop_recovery_schedule(&loop, &event) < 0) {
        fail("schedule cancellable recovery event");
    }
    require(timer_is_armed(tcp_shift_pacer_fd(&loop.recovery_timer)),
            "recovery schedule did not arm timerfd");
    stats = tcp_shift_lwip_loop_recovery_stats(&loop);
    require(stats != NULL && stats->heap_current == 1U,
            "recovery event did not enter heap");

    if (tcp_shift_lwip_loop_recovery_cancel(
            &loop, event.flow_id, event.generation, &cancelled) < 0) {
        fail("cancel recovery event");
    }
    require(cancelled == 1U, "recovery cancel count mismatch");
    stats = tcp_shift_lwip_loop_recovery_stats(&loop);
    require(stats != NULL, "recovery stats unavailable after cancel");
    require(stats->cancelled_events == 1U,
            "recovery cancel telemetry mismatch");
    require(stats->heap_current == 0U,
            "cancelled recovery event remained in heap");
    require(!timer_is_armed(tcp_shift_pacer_fd(&loop.recovery_timer)),
            "empty recovery heap left timerfd armed");
    require(capture.count == 0U,
            "cancelled recovery deadline invoked callback");

    /* The same process-wide timer must remain reusable after teardown. */
    deadline_ns = monotonic_now_ns() + UINT64_C(20) * NS_PER_MS;
    event.deadline_ns = deadline_ns;
    event.flow_id = UINT64_C(42);
    event.generation = UINT32_C(8);
    event.kind = TEST_RECOVERY_KIND;
    if (tcp_shift_lwip_loop_recovery_schedule(&loop, &event) < 0) {
        fail("schedule post-cancel recovery event");
    }

    stop_ns = deadline_ns + UINT64_C(500) * NS_PER_MS;
    while (capture.count == 0U && monotonic_now_ns() < stop_ns) {
        if (tcp_shift_lwip_loop_run_once(&loop) < 0) {
            fail("run loop for recovery event");
        }
    }
    require(capture.count == 1U,
            "post-cancel recovery callback was not released");
    require(capture.event.flow_id == event.flow_id &&
                capture.event.generation == event.generation &&
                capture.event.kind == event.kind,
            "wrong recovery event released");
    require(capture.actual_release_ns >= deadline_ns,
            "recovery event released before deadline");
    require(loop.recovery_timer_wakeups == 1U,
            "one live recovery deadline must cause one timer wakeup");
    require(loop.recovery_release_callbacks == 1U,
            "recovery release callback count mismatch");
    require(loop.recovery_callback_errors == 0U,
            "recovery callback error count nonzero");

    stats = tcp_shift_lwip_loop_recovery_stats(&loop);
    require(stats != NULL, "final recovery stats unavailable");
    require(stats->timerfd_creates == 1U,
            "recovery scheduler created more than one timerfd");
    require(stats->released_events == 1U,
            "recovery release telemetry mismatch");
    require(stats->heap_current == 0U,
            "recovery heap did not drain");
    require(!timer_is_armed(tcp_shift_pacer_fd(&loop.recovery_timer)),
            "drained recovery timerfd did not disarm");

    printf("rack_recovery_loop_lifecycle=ok timerfd_creates=%llu "
           "cancelled=%llu releases=%llu wakeups=%llu callbacks=%llu "
           "heap_peak=%zu heap_final=%zu\n",
           (unsigned long long)stats->timerfd_creates,
           (unsigned long long)stats->cancelled_events,
           (unsigned long long)stats->released_events,
           (unsigned long long)loop.recovery_timer_wakeups,
           (unsigned long long)loop.recovery_release_callbacks,
           stats->heap_peak,
           stats->heap_current);

    tcp_shift_lwip_loop_close(&loop);
    close(fake_tun_fd);
    return 0;
}
