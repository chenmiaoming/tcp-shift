#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cc/registry.h"
#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/tcp_memory.h"

#define NSEC_PER_SEC UINT64_C(1000000000)
#define TCP_SHIFT_LWIP_BBR_EXT_ARG_ID 2U

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "bbr-lwip-binding: check failed at %s:%d: %s\n",         \
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

    if (pacer == NULL || cancelled == NULL || flow_id != pacer->flow_id ||
        generation != pacer->generation) {
        return -1;
    }
    pacer->cancels++;
    *cancelled = 1U;
    return 0;
}

static const struct tcp_shift_lwip_cc_pacer_ops fake_pacer_ops = {
    .schedule = fake_schedule,
    .cancel = fake_cancel,
};

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct fake_pacer pacer;
    struct tcp_pcb *pcb;
    struct timespec delay;
    uint64_t deadline;
    uint64_t initial_rate;
    uint64_t recovery_rate;
    uint32_t prior_cwnd;
    tcpwnd_size_t handshake_sndbuf;
    uint32_t seq = UINT32_C(300000);
    uint16_t payload;
    unsigned char segment;

    CHECK(tcp_shift_cc_find_ops("bbr") == NULL);
    CHECK(tcp_shift_lwip_cc_configure_controller("bbr") < 0);

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    memset(&pacer, 0, sizeof(pacer));
    lwip_init();
    CHECK(LWIP_TCP_PCB_NUM_EXT_ARGS >= 3);
    CHECK(tcp_shift_lwip_cc_configure_pacer(&fake_pacer_ops, &pacer) == 0);

    pcb = tcp_new();
    CHECK(pcb != NULL);
    CHECK(pcb->mss != 0U);
    payload = pcb->mss;
    pcb->cwnd = (tcpwnd_size_t)(10U * pcb->mss);
    pcb->ssthresh = (tcpwnd_size_t)(1024U * 1024U);
    pcb->snd_wnd = (tcpwnd_size_t)(1024U * 1024U);
    pcb->lastack = seq;
    pcb->snd_nxt = seq;

    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    CHECK(adapter.controller.ops == &tcp_shift_reno_ops);
    CHECK(adapter.pacing_flow_id != 0U);
    CHECK(adapter.pacing_generation != 0U);
    CHECK(tcp_ext_arg_get(pcb, (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID) == NULL);

    /* Passive open can still carry SYN-ACK queue bookkeeping when the
     * controller is promoted. The 3xcwnd memory hint must be accepted without
     * weakening tcp_memory_flow_init()'s empty data-queue invariant. */
    handshake_sndbuf = pcb->snd_buf;
    pcb->snd_queuelen = 1U;
    pcb->unacked = (struct tcp_seg *)(void *)&segment;
    CHECK(tcp_shift_lwip_cc_apply_internal_bbr(&adapter, 3U) == 0);
    CHECK(pcb->snd_buf == handshake_sndbuf);
    pcb->unacked = NULL;
    pcb->snd_queuelen = 0U;
    CHECK(tcp_shift_lwip_cc_internal_bbr_active(&adapter) != 0);
    CHECK(strcmp(adapter.controller.ops->name, "bbr-internal-lwip") == 0);
    CHECK(adapter.controller.state != &adapter.controller_state);
    CHECK(tcp_ext_arg_get(pcb, (u8_t)TCP_SHIFT_LWIP_BBR_EXT_ARG_ID) ==
          adapter.controller.state);
    CHECK(adapter.pacing_rate_bytes_per_sec != 0U);
    CHECK(stats.pacing_last_rate_bytes_per_sec ==
          adapter.pacing_rate_bytes_per_sec);
    initial_rate = adapter.pacing_rate_bytes_per_sec;

    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment, seq, payload);
    CHECK(stats.delivery_first_tx_events == 1U);
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000L;
    CHECK(nanosleep(&delay, NULL) == 0);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(tcp_shift_lwip_cc_internal_bbr_active(&adapter) != 0);
    CHECK(stats.ack_events == 1U);
    CHECK(stats.rate_valid_samples == 1U);
    CHECK(stats.ack_observation_events == 1U);
    CHECK(stats.controller_errors == 0U);
    CHECK(adapter.pacing_rate_bytes_per_sec != 0U);
    CHECK(stats.last_cwnd_bytes == (uint32_t)pcb->cwnd);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment, payload);
    CHECK(stats.delivery_live_slots == 0U);

    /* Fast loss is owned by BBR packet conservation. The lwIP hook reports
     * raw outstanding sequence space, so the internal binding removes the one
     * MSS loss before entering recovery. */
    prior_cwnd = (uint32_t)pcb->cwnd;
    pcb->lastack = seq + payload;
    pcb->snd_nxt = pcb->lastack + (4U * payload);
    CHECK(tcp_shift_lwip_cc_hook_loss(pcb, payload) != 0);
    CHECK(tcp_shift_lwip_cc_hook_recovery_is_active(&adapter.hook) != 0U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_controller_owned(pcb) != 0U);
    CHECK((uint32_t)pcb->cwnd == 3U * payload);
    CHECK(stats.loss_events == 1U);
    CHECK(adapter.pacing_rate_bytes_per_sec != 0U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_exit(pcb) != 0);
    CHECK(tcp_shift_lwip_cc_hook_recovery_is_active(&adapter.hook) == 0U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_controller_owned(pcb) == 0U);
    CHECK((uint32_t)pcb->cwnd == prior_cwnd);
    CHECK(adapter.hook.recovery_enter_events == 1U);
    CHECK(adapter.hook.recovery_exit_events == 1U);

    /* A later RTO can supersede controller-owned fast recovery. The pinned
     * timeout hook runs after rto_prepare, so post-loss inflight is zero and
     * BBR publishes one MSS while preserving its pacing rate. */
    pcb->snd_nxt = pcb->lastack + (5U * payload);
    CHECK(tcp_shift_lwip_cc_hook_loss(pcb, payload) != 0);
    CHECK(tcp_shift_lwip_cc_hook_recovery_controller_owned(pcb) != 0U);
    recovery_rate = adapter.pacing_rate_bytes_per_sec;
    tcp_set_flags(pcb, TF_INFR);
    tcp_set_flags(pcb, TF_RTO);
    pcb->unacked = NULL;
    CHECK(tcp_shift_lwip_cc_hook_timeout(pcb) != 0);
    CHECK((pcb->flags & TF_INFR) == 0U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_is_active(&adapter.hook) == 0U);
    CHECK(tcp_shift_lwip_cc_hook_recovery_controller_owned(pcb) == 0U);
    CHECK((uint32_t)pcb->cwnd == payload);
    CHECK(adapter.pacing_rate_bytes_per_sec == recovery_rate);
    CHECK(stats.loss_events == 2U);
    CHECK(stats.timeout_events == 1U);
    CHECK(stats.controller_errors == 0U);

    deadline = now_ns();
    CHECK(deadline != 0U);
    deadline += NSEC_PER_SEC;
    adapter.pacing_next_send_ns = deadline;
    CHECK(tcp_shift_lwip_cc_hook_segment_send_eligible(pcb, payload) == 0);
    CHECK(adapter.pacing_scheduled != 0U);
    CHECK(pacer.schedules == 1U);
    CHECK(pacer.deadline_ns == deadline);

    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    CHECK(pacer.cancels == 1U);
    CHECK(tcp_shift_lwip_cc_clear_pacer() == 0);
    tcp_abort(pcb);

    printf("bbr_lwip_binding=ok public_registry=disabled sidecar=pcb-ext-2 "
           "ack_delivery_sample=ok pacing=nonzero scheduler_exec=ok "
           "sndbuf_hint=3xcwnd passive_open_hint=deferred recovery=controller-owned "
           "rto_post_loss_inflight=0 pacing_after_rto=preserved "
           "initial_rate=%llu\n",
           (unsigned long long)initial_rate);
    return 0;
}
