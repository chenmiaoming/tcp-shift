#ifndef TCP_SHIFT_RUNTIME_PACER_H
#define TCP_SHIFT_RUNTIME_PACER_H

#include <stddef.h>
#include <stdint.h>

struct tcp_shift_pacer_event {
    uint64_t deadline_ns;
    uint64_t flow_id;
    uint32_t generation;
    uint32_t bytes;
};

struct tcp_shift_pacer_stats {
    uint64_t timerfd_creates;
    uint64_t timer_arms;
    uint64_t timer_rearms;
    uint64_t timer_disarms;
    uint64_t timer_expirations;
    uint64_t scheduled_events;
    uint64_t released_events;
    uint64_t cancelled_events;
    uint64_t released_bytes;
    uint64_t last_requested_deadline_ns;
    uint64_t last_actual_release_ns;
    uint64_t last_lateness_ns;
    uint64_t max_lateness_ns;
    size_t heap_current;
    size_t heap_peak;
    size_t heap_capacity;
};

struct tcp_shift_pacer {
    int timer_fd;
    struct tcp_shift_pacer_event *heap;
    size_t heap_len;
    size_t heap_cap;
    size_t heap_max;
    uint64_t armed_deadline_ns;
    struct tcp_shift_pacer_stats stats;
};

/*
 * Initialize one process-wide pacing scheduler. max_events is a hard memory
 * bound for queued pacing eligibility events. The timerfd is nonblocking,
 * close-on-exec, CLOCK_MONOTONIC, and initially disarmed.
 */
int tcp_shift_pacer_init(struct tcp_shift_pacer *pacer, size_t max_events);
void tcp_shift_pacer_close(struct tcp_shift_pacer *pacer);

int tcp_shift_pacer_fd(const struct tcp_shift_pacer *pacer);

/* Queue one generation-safe event. deadline_ns is absolute CLOCK_MONOTONIC. */
int tcp_shift_pacer_schedule(struct tcp_shift_pacer *pacer,
                             const struct tcp_shift_pacer_event *event);

/* Cancel every queued event for exactly this flow generation. */
int tcp_shift_pacer_cancel(struct tcp_shift_pacer *pacer,
                           uint64_t flow_id,
                           uint32_t generation,
                           size_t *cancelled);

/* Inspect the earliest event without removing it. Returns 1/0/-1. */
int tcp_shift_pacer_peek(const struct tcp_shift_pacer *pacer,
                         struct tcp_shift_pacer_event *event);

/*
 * Remove one event only if its deadline is <= now_ns. Returns 1 when an event
 * was released, 0 when none is due, and -1 on invalid input or timer error.
 */
int tcp_shift_pacer_pop_due(struct tcp_shift_pacer *pacer,
                            uint64_t now_ns,
                            struct tcp_shift_pacer_event *event);

/* Drain the timerfd expiration counter. Returns 1/0/-1. */
int tcp_shift_pacer_consume_timer(struct tcp_shift_pacer *pacer,
                                  uint64_t *expirations);

const struct tcp_shift_pacer_stats *tcp_shift_pacer_get_stats(
    const struct tcp_shift_pacer *pacer);

#endif /* TCP_SHIFT_RUNTIME_PACER_H */
