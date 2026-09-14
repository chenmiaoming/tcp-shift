#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/tcp.h"

#define NSEC_PER_SEC UINT64_C(1000000000)

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "pacer-adapter-lifecycle: check failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

struct fake_pacer {
    uint64_t flow_id;
    uint64_t deadline_ns;
    uint32_t generation;
    uint32_t bytes;
    unsigned schedules;
    unsigned cancels;
};

static uint64_t now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static int fake_schedule(void *arg,
                         uint64_t flow_id,
                         uint32_t generation,
                         uint64_t deadline_ns,
                         uint32_t bytes)
{
    struct fake_pacer *pacer = arg;

    if (pacer == NULL || flow_id == 0U || generation == 0U ||
        deadline_ns == 0U || bytes == 0U) {
        return -1;
    }
    pacer->flow_id = flow_id;
    pacer->generation = generation;
    pacer->deadline_ns = deadline_ns;
    pacer->bytes = bytes;
    pacer->schedules++;
    return 0;
}

static int fake_cancel(void *arg,
                       uint64_t flow_id,
                       uint32_t generation,
                       size_t *cancelled)
{
    struct fake_pacer *pacer = arg;

    if (pacer == NULL || cancelled == NULL ||
        flow_id != pacer->flow_id || generation != pacer->generation) {
        return -1;
    }
    pacer->cancels++;
    *cancelled = 1U;
    return 0;
}

static const struct tcp_shift_lwip_cc_pacer_ops fake_ops = {
    .schedule = fake_schedule,
    .cancel = fake_cancel,
};

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct fake_pacer pacer;
    struct tcp_pcb *pcb;
    const struct tcp_shift_lwip_cc_stats *global_stats;
    uint64_t deadline;
    uint64_t flow_id;
    uint64_t stale_before;
    uint32_t generation;

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    memset(&pacer, 0, sizeof(pacer));
    lwip_init();

    CHECK(tcp_shift_lwip_cc_configure_pacer(&fake_ops, &pacer) == 0);

    pcb = tcp_new();
    CHECK(pcb != NULL);
    CHECK(pcb->mss != 0U);
    pcb->cwnd = (tcpwnd_size_t)(pcb->mss * 3U);
    pcb->ssthresh = (tcpwnd_size_t)(16U * 1024U);
    pcb->snd_wnd = (tcpwnd_size_t)(16U * 1024U);

    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    CHECK(adapter.pacing_flow_id != 0U);
    CHECK(adapter.pacing_generation != 0U);

    adapter.pacing_rate_bytes_per_sec =
        TCP_SHIFT_FIXED_PACING_RENO_RATE_BYTES_PER_SEC;
    deadline = now_ns();
    CHECK(deadline != 0U);
    deadline += NSEC_PER_SEC;
    adapter.pacing_next_send_ns = deadline;

    CHECK(tcp_shift_lwip_cc_hook_segment_send_eligible(pcb, pcb->mss) == 0);
    CHECK(adapter.pacing_scheduled != 0U);
    CHECK(pacer.schedules == 1U);
    CHECK(pacer.deadline_ns == deadline);
    CHECK(pacer.bytes == pcb->mss);

    flow_id = adapter.pacing_flow_id;
    generation = adapter.pacing_generation;
    CHECK(pacer.flow_id == flow_id);
    CHECK(pacer.generation == generation);

    /* Unbind models teardown while an eligibility deadline is outstanding. It
     * must cancel by scalar identity before clearing the registry entry. */
    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    CHECK(pacer.cancels == 1U);
    CHECK(adapter.pacing_scheduled == 0U);
    CHECK(adapter.pacing_flow_id == 0U);
    CHECK(adapter.pacing_generation == 0U);
    CHECK(tcp_shift_lwip_cc_clear_pacer() == 0);

    /* A late timer delivery carrying the old generation must be harmless and
     * observable as stale, never a dereference of the unbound adapter/PCB. */
    global_stats = tcp_shift_lwip_cc_get_stats();
    stale_before = global_stats->pacing_stale_releases;
    CHECK(tcp_shift_lwip_cc_resume_paced(flow_id, generation, now_ns()) == 0);
    CHECK(tcp_shift_lwip_cc_get_stats()->pacing_stale_releases ==
          stale_before + 1U);

    tcp_abort(pcb);
    printf("pacer_adapter_lifecycle=ok schedules=%u cancels=%u stale_release_safe=1 flow_id=%llu generation=%u\n",
           pacer.schedules,
           pacer.cancels,
           (unsigned long long)flow_id,
           generation);
    return 0;
}
