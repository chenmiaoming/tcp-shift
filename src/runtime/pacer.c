#include "runtime/pacer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define TCP_SHIFT_PACER_INITIAL_CAPACITY 8U
#define TCP_SHIFT_PACER_NSEC_PER_SEC UINT64_C(1000000000)

static uint64_t tcp_shift_flow_pacer_spacing_ns(uint32_t bytes,
                                                 uint64_t rate_bytes_per_sec)
{
    uint64_t numerator;
    uint64_t spacing;

    if (bytes == 0U || rate_bytes_per_sec == 0U) {
        return 0U;
    }

    /* bytes is u32, so bytes * 1e9 cannot overflow u64. Divide with an
     * explicit remainder instead of numerator + rate - 1, which could. */
    numerator = (uint64_t)bytes * TCP_SHIFT_PACER_NSEC_PER_SEC;
    spacing = numerator / rate_bytes_per_sec;
    if (numerator % rate_bytes_per_sec != 0U) {
        spacing++;
    }
    return spacing == 0U ? 1U : spacing;
}

static uint64_t tcp_shift_flow_pacer_add_sat(uint64_t left, uint64_t right)
{
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

void tcp_shift_flow_pacer_init(struct tcp_shift_flow_pacer *flow,
                               uint32_t max_catch_up_bytes)
{
    if (flow == NULL) {
        return;
    }
    memset(flow, 0, sizeof(*flow));
    flow->max_catch_up_bytes = max_catch_up_bytes;
}

void tcp_shift_flow_pacer_reset(struct tcp_shift_flow_pacer *flow)
{
    uint32_t max_catch_up_bytes;

    if (flow == NULL) {
        return;
    }
    max_catch_up_bytes = flow->max_catch_up_bytes;
    memset(flow, 0, sizeof(*flow));
    flow->max_catch_up_bytes = max_catch_up_bytes;
}

void tcp_shift_flow_pacer_set_rate(struct tcp_shift_flow_pacer *flow,
                                   uint64_t rate_bytes_per_sec)
{
    if (flow == NULL) {
        return;
    }
    flow->rate_bytes_per_sec = rate_bytes_per_sec;
    if (rate_bytes_per_sec == 0U) {
        flow->next_send_ns = 0U;
    }
}

uint64_t tcp_shift_flow_pacer_deadline(const struct tcp_shift_flow_pacer *flow,
                                       uint64_t now_ns)
{
    if (flow == NULL || flow->rate_bytes_per_sec == 0U ||
        flow->next_send_ns == 0U || flow->next_send_ns <= now_ns) {
        return 0U;
    }
    return flow->next_send_ns;
}

int tcp_shift_flow_pacer_note_tx(struct tcp_shift_flow_pacer *flow,
                                 uint64_t now_ns,
                                 uint32_t bytes)
{
    uint64_t base_ns;
    uint64_t spacing_ns;
    uint64_t catch_up_ns;
    uint64_t floor_ns;

    if (flow == NULL || now_ns == 0U || bytes == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (flow->rate_bytes_per_sec == 0U) {
        flow->next_send_ns = 0U;
        return 0;
    }

    spacing_ns = tcp_shift_flow_pacer_spacing_ns(
        bytes, flow->rate_bytes_per_sec);
    if (spacing_ns == 0U) {
        errno = EINVAL;
        return -1;
    }

    base_ns = flow->next_send_ns;
    if (base_ns == 0U) {
        /* A newly paced or long-idle flow gets one immediate transmission,
         * then enters the virtual timeline. Startup never inherits credit
         * from time before pacing was enabled. */
        base_ns = now_ns;
    } else if (base_ns < now_ns) {
        catch_up_ns = tcp_shift_flow_pacer_spacing_ns(
            flow->max_catch_up_bytes, flow->rate_bytes_per_sec);
        floor_ns = catch_up_ns < now_ns ? now_ns - catch_up_ns : 0U;
        if (base_ns < floor_ns) {
            base_ns = floor_ns;
            flow->catch_up_clamps++;
        }
    }

    flow->next_send_ns = tcp_shift_flow_pacer_add_sat(base_ns, spacing_ns);
    flow->tx_events++;
    flow->tx_bytes = tcp_shift_flow_pacer_add_sat(flow->tx_bytes, bytes);
    return 0;
}

static int tcp_shift_pacer_event_less(const struct tcp_shift_pacer_event *a,
                                      const struct tcp_shift_pacer_event *b)
{
    if (a->deadline_ns != b->deadline_ns) {
        return a->deadline_ns < b->deadline_ns;
    }
    if (a->flow_id != b->flow_id) {
        return a->flow_id < b->flow_id;
    }
    if (a->generation != b->generation) {
        return a->generation < b->generation;
    }
    return a->bytes < b->bytes;
}

static void tcp_shift_pacer_swap(struct tcp_shift_pacer_event *a,
                                 struct tcp_shift_pacer_event *b)
{
    struct tcp_shift_pacer_event tmp = *a;

    *a = *b;
    *b = tmp;
}

static void tcp_shift_pacer_sift_up(struct tcp_shift_pacer *pacer,
                                    size_t index)
{
    while (index != 0U) {
        size_t parent = (index - 1U) / 2U;

        if (!tcp_shift_pacer_event_less(&pacer->heap[index],
                                        &pacer->heap[parent])) {
            break;
        }
        tcp_shift_pacer_swap(&pacer->heap[index], &pacer->heap[parent]);
        index = parent;
    }
}

static void tcp_shift_pacer_sift_down(struct tcp_shift_pacer *pacer,
                                      size_t index)
{
    for (;;) {
        size_t left = index * 2U + 1U;
        size_t right = left + 1U;
        size_t smallest = index;

        if (left < pacer->heap_len &&
            tcp_shift_pacer_event_less(&pacer->heap[left],
                                       &pacer->heap[smallest])) {
            smallest = left;
        }
        if (right < pacer->heap_len &&
            tcp_shift_pacer_event_less(&pacer->heap[right],
                                       &pacer->heap[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        tcp_shift_pacer_swap(&pacer->heap[index], &pacer->heap[smallest]);
        index = smallest;
    }
}

static void tcp_shift_pacer_heapify(struct tcp_shift_pacer *pacer)
{
    size_t i;

    if (pacer->heap_len < 2U) {
        return;
    }
    i = pacer->heap_len / 2U;
    while (i != 0U) {
        i--;
        tcp_shift_pacer_sift_down(pacer, i);
    }
}

static int tcp_shift_pacer_reserve(struct tcp_shift_pacer *pacer)
{
    struct tcp_shift_pacer_event *heap;
    size_t next_cap;

    if (pacer->heap_len < pacer->heap_cap) {
        return 0;
    }
    if (pacer->heap_len >= pacer->heap_max) {
        errno = ENOSPC;
        return -1;
    }

    next_cap = pacer->heap_cap == 0U ? TCP_SHIFT_PACER_INITIAL_CAPACITY
                                     : pacer->heap_cap * 2U;
    if (next_cap < pacer->heap_cap || next_cap > pacer->heap_max) {
        next_cap = pacer->heap_max;
    }
    if (next_cap > SIZE_MAX / sizeof(*heap)) {
        errno = ENOMEM;
        return -1;
    }

    heap = realloc(pacer->heap, next_cap * sizeof(*heap));
    if (heap == NULL) {
        return -1;
    }
    pacer->heap = heap;
    pacer->heap_cap = next_cap;
    pacer->stats.heap_capacity = next_cap;
    return 0;
}

static int tcp_shift_pacer_sync_timer(struct tcp_shift_pacer *pacer)
{
    struct itimerspec spec;
    uint64_t deadline_ns;

    if (pacer->heap_len == 0U) {
        if (pacer->armed_deadline_ns == 0U) {
            return 0;
        }
        memset(&spec, 0, sizeof(spec));
        if (timerfd_settime(pacer->timer_fd, TFD_TIMER_ABSTIME, &spec,
                            NULL) < 0) {
            return -1;
        }
        pacer->armed_deadline_ns = 0U;
        pacer->stats.timer_disarms++;
        return 0;
    }

    deadline_ns = pacer->heap[0].deadline_ns;
    if (deadline_ns == pacer->armed_deadline_ns) {
        return 0;
    }

    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = (time_t)(deadline_ns / 1000000000ULL);
    spec.it_value.tv_nsec = (long)(deadline_ns % 1000000000ULL);
    if (timerfd_settime(pacer->timer_fd, TFD_TIMER_ABSTIME, &spec, NULL) < 0) {
        return -1;
    }
    if (pacer->armed_deadline_ns == 0U) {
        pacer->stats.timer_arms++;
    } else {
        pacer->stats.timer_rearms++;
    }
    pacer->armed_deadline_ns = deadline_ns;
    return 0;
}

int tcp_shift_pacer_init(struct tcp_shift_pacer *pacer, size_t max_events)
{
    if (pacer == NULL || max_events == 0U ||
        max_events > SIZE_MAX / sizeof(struct tcp_shift_pacer_event)) {
        errno = EINVAL;
        return -1;
    }

    memset(pacer, 0, sizeof(*pacer));
    pacer->timer_fd = -1;
    pacer->timer_fd = timerfd_create(CLOCK_MONOTONIC,
                                     TFD_NONBLOCK | TFD_CLOEXEC);
    if (pacer->timer_fd < 0) {
        return -1;
    }
    pacer->heap_max = max_events;
    pacer->stats.timerfd_creates = 1U;
    return 0;
}

void tcp_shift_pacer_close(struct tcp_shift_pacer *pacer)
{
    if (pacer == NULL) {
        return;
    }
    if (pacer->timer_fd >= 0) {
        close(pacer->timer_fd);
    }
    free(pacer->heap);
    memset(pacer, 0, sizeof(*pacer));
    pacer->timer_fd = -1;
}

int tcp_shift_pacer_fd(const struct tcp_shift_pacer *pacer)
{
    if (pacer == NULL || pacer->timer_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return pacer->timer_fd;
}

int tcp_shift_pacer_schedule(struct tcp_shift_pacer *pacer,
                             const struct tcp_shift_pacer_event *event)
{
    size_t index;

    if (pacer == NULL || pacer->timer_fd < 0 || event == NULL ||
        event->deadline_ns == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (tcp_shift_pacer_reserve(pacer) < 0) {
        return -1;
    }

    index = pacer->heap_len++;
    pacer->heap[index] = *event;
    tcp_shift_pacer_sift_up(pacer, index);
    pacer->stats.scheduled_events++;
    pacer->stats.heap_current = pacer->heap_len;
    if (pacer->heap_len > pacer->stats.heap_peak) {
        pacer->stats.heap_peak = pacer->heap_len;
    }

    if (tcp_shift_pacer_sync_timer(pacer) < 0) {
        size_t rollback;

        for (rollback = 0U; rollback < pacer->heap_len; rollback++) {
            if (pacer->heap[rollback].deadline_ns == event->deadline_ns &&
                pacer->heap[rollback].flow_id == event->flow_id &&
                pacer->heap[rollback].generation == event->generation &&
                pacer->heap[rollback].bytes == event->bytes) {
                pacer->heap[rollback] = pacer->heap[pacer->heap_len - 1U];
                pacer->heap_len--;
                tcp_shift_pacer_heapify(pacer);
                pacer->stats.heap_current = pacer->heap_len;
                pacer->stats.scheduled_events--;
                break;
            }
        }
        return -1;
    }
    return 0;
}

int tcp_shift_pacer_cancel(struct tcp_shift_pacer *pacer,
                           uint64_t flow_id,
                           uint32_t generation,
                           size_t *cancelled)
{
    size_t read_index;
    size_t write_index = 0U;
    size_t removed = 0U;

    if (pacer == NULL || pacer->timer_fd < 0 || cancelled == NULL) {
        errno = EINVAL;
        return -1;
    }
    *cancelled = 0U;

    for (read_index = 0U; read_index < pacer->heap_len; read_index++) {
        const struct tcp_shift_pacer_event *event = &pacer->heap[read_index];

        if (event->flow_id == flow_id && event->generation == generation) {
            removed++;
            continue;
        }
        if (write_index != read_index) {
            pacer->heap[write_index] = *event;
        }
        write_index++;
    }
    if (removed == 0U) {
        return 0;
    }

    pacer->heap_len = write_index;
    tcp_shift_pacer_heapify(pacer);
    pacer->stats.cancelled_events += removed;
    pacer->stats.heap_current = pacer->heap_len;
    if (tcp_shift_pacer_sync_timer(pacer) < 0) {
        return -1;
    }
    *cancelled = removed;
    return 0;
}

int tcp_shift_pacer_peek(const struct tcp_shift_pacer *pacer,
                         struct tcp_shift_pacer_event *event)
{
    if (pacer == NULL || pacer->timer_fd < 0 || event == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (pacer->heap_len == 0U) {
        return 0;
    }
    *event = pacer->heap[0];
    return 1;
}

int tcp_shift_pacer_pop_due(struct tcp_shift_pacer *pacer,
                            uint64_t now_ns,
                            struct tcp_shift_pacer_event *event)
{
    if (pacer == NULL || pacer->timer_fd < 0 || event == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (pacer->heap_len == 0U || pacer->heap[0].deadline_ns > now_ns) {
        return 0;
    }

    *event = pacer->heap[0];
    pacer->heap_len--;
    if (pacer->heap_len != 0U) {
        pacer->heap[0] = pacer->heap[pacer->heap_len];
        tcp_shift_pacer_sift_down(pacer, 0U);
    }
    pacer->stats.released_events++;
    pacer->stats.released_bytes += event->bytes;
    pacer->stats.last_requested_deadline_ns = event->deadline_ns;
    pacer->stats.last_actual_release_ns = now_ns;
    pacer->stats.last_lateness_ns =
        now_ns > event->deadline_ns ? now_ns - event->deadline_ns : 0U;
    if (pacer->stats.last_lateness_ns > pacer->stats.max_lateness_ns) {
        pacer->stats.max_lateness_ns = pacer->stats.last_lateness_ns;
    }
    pacer->stats.heap_current = pacer->heap_len;
    if (tcp_shift_pacer_sync_timer(pacer) < 0) {
        return -1;
    }
    return 1;
}

int tcp_shift_pacer_consume_timer(struct tcp_shift_pacer *pacer,
                                  uint64_t *expirations)
{
    uint64_t value;
    ssize_t count;

    if (pacer == NULL || pacer->timer_fd < 0 || expirations == NULL) {
        errno = EINVAL;
        return -1;
    }

    count = read(pacer->timer_fd, &value, sizeof(value));
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *expirations = 0U;
            return 0;
        }
        return -1;
    }
    if ((size_t)count != sizeof(value)) {
        errno = EIO;
        return -1;
    }
    *expirations = value;
    pacer->stats.timer_expirations += value;
    return 1;
}

const struct tcp_shift_pacer_stats *tcp_shift_pacer_get_stats(
    const struct tcp_shift_pacer *pacer)
{
    if (pacer == NULL) {
        errno = EINVAL;
        return NULL;
    }
    return &pacer->stats;
}
