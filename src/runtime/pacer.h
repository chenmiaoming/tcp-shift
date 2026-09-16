#ifndef TCP_SHIFT_RUNTIME_PACER_H
#define TCP_SHIFT_RUNTIME_PACER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Per-flow transport pacing clock.
 *
 * Congestion control owns the target rate; this state owns only the conversion
 * from bytes/rate into send eligibility timestamps. max_catch_up_bytes bounds
 * credit accumulated while the event loop is late so a delayed timer cannot
 * turn into an unbounded burst. A value of zero disables catch-up entirely.
 *
 * quantum_bytes is a separate bounded batching allowance. Once a pacing
 * deadline becomes eligible, at most one quantum may pass without consulting
 * another deadline. The virtual clock is still advanced by each transmitted
 * byte, so batching changes packet grouping rather than the long-term rate.
 */
struct tcp_shift_flow_pacer {
    uint64_t rate_bytes_per_sec;
    uint64_t next_send_ns;
    uint32_t max_catch_up_bytes;
    uint32_t quantum_bytes;
    uint32_t quantum_remaining_bytes;
    uint64_t tx_events;
    uint64_t tx_bytes;
    uint64_t catch_up_clamps;
    uint64_t quantum_grants;
};

void tcp_shift_flow_pacer_init(struct tcp_shift_flow_pacer *flow,
                               uint32_t max_catch_up_bytes);
void tcp_shift_flow_pacer_reset(struct tcp_shift_flow_pacer *flow);
void tcp_shift_flow_pacer_set_rate(struct tcp_shift_flow_pacer *flow,
                                   uint64_t rate_bytes_per_sec);

/* Update the bounded batch allowance. Changing its size invalidates unused
 * credit from the previous quantum; publishing the same value is idempotent. */
static inline void tcp_shift_flow_pacer_set_quantum(
    struct tcp_shift_flow_pacer *flow,
    uint32_t quantum_bytes)
{
    if (flow == NULL || flow->quantum_bytes == quantum_bytes) {
        return;
    }
    flow->quantum_bytes = quantum_bytes;
    flow->quantum_remaining_bytes = 0U;
}

/*
 * Decide whether one segment may be sent now. Return 1 when eligible, 0 when
 * the caller must defer until *deadline_ns, and -1 for invalid input.
 *
 * Eligibility credit is consumed before the transport send. If the subsequent
 * send fails, the only consequence is conservative under-use of that quantum;
 * credit is never created by a failed send. A zero quantum preserves strict
 * one-segment pacing by granting only the requested segment at each deadline.
 */
static inline int tcp_shift_flow_pacer_segment_eligible(
    struct tcp_shift_flow_pacer *flow,
    uint64_t now_ns,
    uint32_t bytes,
    uint64_t *deadline_ns)
{
    uint32_t allowance;

    if (flow == NULL || now_ns == 0U || bytes == 0U || deadline_ns == NULL) {
        return -1;
    }
    *deadline_ns = 0U;

    if (flow->rate_bytes_per_sec == 0U) {
        return 1;
    }
    if (flow->quantum_remaining_bytes >= bytes) {
        flow->quantum_remaining_bytes -= bytes;
        return 1;
    }

    /* Discard a sub-segment tail. Quantum sizing is normally MSS-aligned, and
     * this keeps a short tail from bypassing a future pacing deadline. */
    flow->quantum_remaining_bytes = 0U;
    if (flow->next_send_ns != 0U && flow->next_send_ns > now_ns) {
        *deadline_ns = flow->next_send_ns;
        return 0;
    }

    allowance = flow->quantum_bytes;
    if (allowance < bytes) {
        allowance = bytes;
    }
    flow->quantum_remaining_bytes = allowance - bytes;
    flow->quantum_grants++;
    return 1;
}

/*
 * Return the absolute CLOCK_MONOTONIC deadline for the next send. Zero means
 * pacing is disabled or the raw virtual clock is currently eligible. Callers
 * using bounded batching should prefer tcp_shift_flow_pacer_segment_eligible().
 */
uint64_t tcp_shift_flow_pacer_deadline(const struct tcp_shift_flow_pacer *flow,
                                       uint64_t now_ns);

/*
 * Advance the virtual send clock after bytes were handed to the transport.
 * Returns 0 on success and -1 for invalid input. The caller must still enforce
 * cwnd/rwnd; this primitive only enforces the requested pacing rate.
 */
int tcp_shift_flow_pacer_note_tx(struct tcp_shift_flow_pacer *flow,
                                 uint64_t now_ns,
                                 uint32_t bytes);

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
