#include "runtime/pacer.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>

#define NS_PER_MS 1000000ULL

static void fail(const char *message)
{
    fprintf(stderr, "pacer contract failed: %s (errno=%d %s)\n", message,
            errno, strerror(errno));
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

static struct tcp_shift_pacer_event event_at(uint64_t deadline_ns,
                                             uint64_t flow_id,
                                             uint32_t generation,
                                             uint32_t bytes)
{
    struct tcp_shift_pacer_event event;

    event.deadline_ns = deadline_ns;
    event.flow_id = flow_id;
    event.generation = generation;
    event.bytes = bytes;
    return event;
}

int main(void)
{
    struct tcp_shift_pacer pacer;
    struct tcp_shift_pacer_event event;
    struct tcp_shift_pacer_event out;
    const struct tcp_shift_pacer_stats *stats;
    struct pollfd pfd;
    uint64_t base;
    uint64_t expirations;
    uint64_t rearms_before;
    size_t cancelled;
    int fd;
    int rc;

    if (tcp_shift_pacer_init(&pacer, 8U) < 0) {
        fail("init");
    }
    fd = tcp_shift_pacer_fd(&pacer);
    require(fd >= 0, "timerfd unavailable");
    require(!timer_is_armed(fd), "timerfd must start disarmed");

    base = monotonic_now_ns();
    event = event_at(base + 80U * NS_PER_MS, 80U, 1U, 800U);
    require(tcp_shift_pacer_schedule(&pacer, &event) == 0,
            "schedule first event");
    require(timer_is_armed(fd), "first event must arm timerfd");

    event = event_at(base + 20U * NS_PER_MS, 20U, 1U, 200U);
    require(tcp_shift_pacer_schedule(&pacer, &event) == 0,
            "schedule earlier event");
    event = event_at(base + 50U * NS_PER_MS, 50U, 1U, 500U);
    require(tcp_shift_pacer_schedule(&pacer, &event) == 0,
            "schedule middle event");

    require(tcp_shift_pacer_peek(&pacer, &out) == 1,
            "peek earliest event");
    require(out.flow_id == 20U, "min-heap deadline ordering");

    stats = tcp_shift_pacer_get_stats(&pacer);
    require(stats != NULL, "stats unavailable");
    require(stats->timerfd_creates == 1U, "exactly one timerfd created");
    require(stats->timer_arms == 1U, "first event must count one arm");
    require(stats->timer_rearms == 1U,
            "only the earlier deadline should rearm timer");
    rearms_before = stats->timer_rearms;

    event = event_at(base + 90U * NS_PER_MS, 90U, 1U, 900U);
    require(tcp_shift_pacer_schedule(&pacer, &event) == 0,
            "schedule later event");
    require(tcp_shift_pacer_get_stats(&pacer)->timer_rearms == rearms_before,
            "later event must not rearm earliest deadline");

    require(tcp_shift_pacer_cancel(&pacer, 20U, 1U, &cancelled) == 0 &&
                cancelled == 1U,
            "cancel earliest generation");
    require(tcp_shift_pacer_peek(&pacer, &out) == 1 && out.flow_id == 50U,
            "cancel must reveal next deadline");
    require(tcp_shift_pacer_get_stats(&pacer)->timer_rearms ==
                rearms_before + 1U,
            "cancelling earliest must rearm timer");

    rearms_before = tcp_shift_pacer_get_stats(&pacer)->timer_rearms;
    require(tcp_shift_pacer_cancel(&pacer, 90U, 1U, &cancelled) == 0 &&
                cancelled == 1U,
            "cancel non-head event");
    require(tcp_shift_pacer_get_stats(&pacer)->timer_rearms == rearms_before,
            "cancelling non-head must not rearm timer");
    require(tcp_shift_pacer_cancel(&pacer, 999U, 1U, &cancelled) == 0 &&
                cancelled == 0U,
            "cancel absent event");

    require(tcp_shift_pacer_pop_due(&pacer, base + 49U * NS_PER_MS, &out) == 0,
            "not-yet-due event released");
    require(tcp_shift_pacer_pop_due(&pacer, base + 50U * NS_PER_MS, &out) == 1,
            "due event not released");
    require(out.flow_id == 50U, "wrong event released at deadline");

    event = event_at(base + 120U * NS_PER_MS, 7U, 3U, 703U);
    require(tcp_shift_pacer_schedule(&pacer, &event) == 0,
            "schedule generation 3");
    event = event_at(base + 110U * NS_PER_MS, 7U, 4U, 704U);
    require(tcp_shift_pacer_schedule(&pacer, &event) == 0,
            "schedule generation 4");
    require(tcp_shift_pacer_cancel(&pacer, 7U, 3U, &cancelled) == 0 &&
                cancelled == 1U,
            "generation-specific cancel");
    require(tcp_shift_pacer_peek(&pacer, &out) == 1,
            "peek after generation cancel");
    require(!(out.flow_id == 7U && out.generation == 3U),
            "cancelled generation survived");

    while ((rc = tcp_shift_pacer_pop_due(&pacer,
                                          base + 500U * NS_PER_MS,
                                          &out)) == 1) {
        /* Drain using a synthetic monotonic observation. */
    }
    require(rc == 0, "drain failed");
    require(tcp_shift_pacer_peek(&pacer, &out) == 0,
            "heap must be empty after drain");
    require(!timer_is_armed(fd), "empty heap must disarm timerfd");

    /* Exercise a real one-shot expiration without polling or spinning. */
    base = monotonic_now_ns();
    event = event_at(base + 15U * NS_PER_MS, 1U, 9U, 1460U);
    require(tcp_shift_pacer_schedule(&pacer, &event) == 0,
            "schedule real timer event");
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1U, 250);
    require(rc == 1 && (pfd.revents & POLLIN) != 0,
            "one-shot timerfd did not become readable");
    require(tcp_shift_pacer_consume_timer(&pacer, &expirations) == 1,
            "consume timerfd expiration");
    require(expirations >= 1U, "timerfd expiration count is zero");
    require(tcp_shift_pacer_pop_due(&pacer, monotonic_now_ns(), &out) == 1,
            "expired event was not due");
    require(out.flow_id == 1U && out.generation == 9U,
            "wrong real-time event released");
    require(!timer_is_armed(fd), "timer must disarm after final release");

    stats = tcp_shift_pacer_get_stats(&pacer);
    require(stats->heap_current == 0U, "heap_current nonzero after drain");
    require(stats->heap_peak <= 8U, "heap exceeded configured bound");
    require(stats->timer_expirations >= 1U, "expiration telemetry missing");
    require(stats->cancelled_events == 3U, "cancel telemetry mismatch");
    require(stats->released_events == 4U, "release telemetry mismatch");
    require(stats->released_bytes == 3464U, "released byte telemetry mismatch");
    require(stats->last_actual_release_ns >= stats->last_requested_deadline_ns,
            "last release precedes requested deadline");

    printf("pacer_contract=ok timerfd_creates=%llu timer_arms=%llu "
           "timer_rearms=%llu timer_disarms=%llu timer_expirations=%llu "
           "scheduled_events=%llu released_events=%llu cancelled_events=%llu "
           "released_bytes=%llu last_lateness_ns=%llu max_lateness_ns=%llu "
           "heap_peak=%zu heap_capacity=%zu\n",
           (unsigned long long)stats->timerfd_creates,
           (unsigned long long)stats->timer_arms,
           (unsigned long long)stats->timer_rearms,
           (unsigned long long)stats->timer_disarms,
           (unsigned long long)stats->timer_expirations,
           (unsigned long long)stats->scheduled_events,
           (unsigned long long)stats->released_events,
           (unsigned long long)stats->cancelled_events,
           (unsigned long long)stats->released_bytes,
           (unsigned long long)stats->last_lateness_ns,
           (unsigned long long)stats->max_lateness_ns,
           stats->heap_peak, stats->heap_capacity);

    tcp_shift_pacer_close(&pacer);
    return 0;
}
