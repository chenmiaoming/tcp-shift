#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/cc_adapter.h"
#include "lwip/init.h"
#include "lwip/tcp.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "rack-adapter: check failed at %s:%d: %s\n",   \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int sleep_ns(long ns)
{
    struct timespec pause = {.tv_sec = 0, .tv_nsec = ns};

    return nanosleep(&pause, NULL);
}

struct fake_timer_state {
    unsigned schedules;
    unsigned cancels;
    uint64_t deadline_ns;
    uint32_t kind;
};

static int fake_pacer_schedule(void *arg,
                               uint64_t flow_id,
                               uint32_t generation,
                               uint64_t deadline_ns,
                               uint32_t bytes)
{
    (void)arg;
    (void)flow_id;
    (void)generation;
    (void)deadline_ns;
    (void)bytes;
    return 0;
}

static int fake_pacer_cancel(void *arg,
                             uint64_t flow_id,
                             uint32_t generation,
                             size_t *cancelled)
{
    (void)arg;
    (void)flow_id;
    (void)generation;
    if (cancelled != NULL) {
        *cancelled = 0U;
    }
    return 0;
}

static int fake_recovery_schedule(void *arg,
                                  uint64_t flow_id,
                                  uint32_t generation,
                                  uint64_t deadline_ns,
                                  uint32_t kind)
{
    struct fake_timer_state *state = arg;

    (void)flow_id;
    (void)generation;
    state->schedules++;
    state->deadline_ns = deadline_ns;
    state->kind = kind;
    return 0;
}

static int fake_recovery_cancel(void *arg,
                                uint64_t flow_id,
                                uint32_t generation,
                                size_t *cancelled)
{
    struct fake_timer_state *state = arg;

    (void)flow_id;
    (void)generation;
    state->cancels++;
    if (cancelled != NULL) {
        *cancelled = 1U;
    }
    return 0;
}

static const struct tcp_shift_lwip_cc_pacer_ops fake_pacer_ops = {
    .schedule = fake_pacer_schedule,
    .cancel = fake_pacer_cancel,
};

static const struct tcp_shift_lwip_recovery_timer_ops fake_recovery_ops = {
    .schedule = fake_recovery_schedule,
    .cancel = fake_recovery_cancel,
};

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct tcp_shift_lwip_sack_range sack;
    struct tcp_pcb *pcb;
    unsigned char outstanding_sentinel;
    unsigned char segment1;
    unsigned char segment2;
    unsigned char segment3;
    struct fake_timer_state timer_state;
    uint64_t remaining_ns = 0U;
    uint32_t seq;
    uint16_t payload;
    int status;

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    memset(&timer_state, 0, sizeof(timer_state));
    outstanding_sentinel = 0U;
    lwip_init();

    pcb = tcp_new();
    CHECK(pcb != NULL);
    CHECK(pcb->mss != 0U);

    payload = pcb->mss;
    seq = UINT32_C(500000);
    pcb->cwnd = (tcpwnd_size_t)(payload * 8U);
    pcb->ssthresh = (tcpwnd_size_t)(payload * 16U);
    pcb->snd_wnd = (tcpwnd_size_t)(payload * 16U);
    pcb->lastack = seq;
    pcb->snd_nxt = seq;

    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    adapter.sack_delivery_policy = 1U;

    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment1, seq, payload);
    pcb->unacked = (struct tcp_seg *)(void *)&outstanding_sentinel;
    pcb->snd_nxt = seq + payload;
    tcp_shift_lwip_cc_hook_segment_tx(
        pcb, &segment2, seq + payload, payload);
    pcb->snd_nxt = seq + (2U * payload);

    /* Give the later segment a measurable RTT, then selectively acknowledge
     * it. RACK.segment must advance to segment2 while segment1 stays
     * outstanding and becomes the candidate older transmission. */
    CHECK(sleep_ns(4000000L) == 0);
    sack.left = seq + payload;
    sack.right = seq + (2U * payload);
    CHECK(tcp_shift_lwip_cc_hook_sack(pcb, seq, &sack, 1U) != 0);
    CHECK(adapter.rack_tlp.rack_xmit_ts_ns != 0U);
    CHECK(adapter.rack_tlp.rack_end_seq == sack.right);
    CHECK(adapter.rack_tlp.rack_rtt_ns != 0U);
    CHECK(adapter.rack_tlp.min_rtt_ns != 0U);
    CHECK(adapter.rack_tlp.segs_sacked == 1U);

    status = tcp_shift_lwip_cc_hook_rack_loss_status(
        pcb, &segment1, &remaining_ns);
    CHECK(status == 0);
    CHECK(remaining_ns > 0U);

    /* Wait beyond RTT + min_RTT/4. The same segment must then become lost by
     * elapsed transmit time, without needing a third later SACK. */
    CHECK(sleep_ns(3000000L) == 0);
    remaining_ns = UINT64_MAX;
    status = tcp_shift_lwip_cc_hook_rack_loss_status(
        pcb, &segment1, &remaining_ns);
    CHECK(status == 1);
    CHECK(remaining_ns == 0U);

    /* A segment already delivered by SACK is not a loss candidate. */
    CHECK(tcp_shift_lwip_cc_hook_rack_loss_status(
              pcb, &segment2, &remaining_ns) == -1);

    /* RFC 2883 requires D-SACK classification against the cumulative ACK in
     * this same packet, not pcb->lastack. Keep lastack deliberately behind
     * ack_seq: the first block is still a D-SACK and must grow reo_wnd. */
    sack.left = seq;
    sack.right = seq + payload;
    CHECK(pcb->lastack == seq);
    CHECK(tcp_shift_lwip_cc_hook_sack(
              pcb, seq + payload, &sack, 1U) != 0);
    CHECK(stats.rack_dsack_events == 1U);
    /* The hook runs during tcp_parseopt(), before the same packet's cumulative
     * ACK is applied. RACK delivery ordering is intentionally deferred here;
     * the live tcp_receive() path completes the combined RFC 8985 pass. */
    CHECK(stats.rack_reordering_events == 0U);
    CHECK(adapter.rack_tlp.reordering_seen == 0U);
    CHECK(adapter.rack_tlp.reo_wnd_mult == 2U);
    CHECK(adapter.rack_tlp.reo_wnd_persist ==
          TCP_SHIFT_RACK_REO_WND_PERSIST_RECOVERIES);
    CHECK(stats.rack_reo_wnd_mult == 2U);
    CHECK(stats.rack_reo_wnd_mult_max == 2U);

    /* Recovery exit advances the RFC 8985 D-SACK persistence lifecycle even
     * for a transport/native recovery owner. */
    adapter.hook.recovery_active = 1U;
    adapter.hook.recovery_end_seq = pcb->snd_nxt;
    CHECK(tcp_shift_lwip_cc_hook_recovery_exit(
              pcb, pcb->snd_nxt) == 0);
    CHECK(adapter.hook.recovery_active == 0U);
    CHECK(adapter.rack_tlp.dsack_round_active == 0U);
    CHECK(adapter.rack_tlp.reo_wnd_mult == 2U);
    CHECK(adapter.rack_tlp.reo_wnd_persist ==
          TCP_SHIFT_RACK_REO_WND_PERSIST_RECOVERIES - 1U);
    CHECK(stats.rack_reo_wnd_persist ==
          TCP_SHIFT_RACK_REO_WND_PERSIST_RECOVERIES - 1U);

    pcb->unacked = NULL;
    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    tcp_abort(pcb);

    /* tcp_parseopt() exposes SACK blocks before tcp_receive() advances
     * SND.UNA. RFC 8985 section 6.2 requires both kinds of newly delivered
     * segment from one ACK to participate in the same ordered Step 2/Step 3
     * pass. Reproduce the loss-only shape that previously produced a false
     * reordering event: this ACK cumulatively covers segment1 while SACKing
     * segment3 around an outstanding segment2 hole. */
    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    pcb = tcp_new();
    CHECK(pcb != NULL);
    payload = pcb->mss;
    seq = UINT32_C(600000);
    pcb->cwnd = (tcpwnd_size_t)(payload * 8U);
    pcb->ssthresh = (tcpwnd_size_t)(payload * 16U);
    pcb->snd_wnd = (tcpwnd_size_t)(payload * 16U);
    pcb->lastack = seq;
    pcb->snd_nxt = seq;
    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    adapter.sack_delivery_policy = 1U;

    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment1, seq, payload);
    pcb->snd_nxt = seq + payload;
    tcp_shift_lwip_cc_hook_segment_tx(
        pcb, &segment2, seq + payload, payload);
    pcb->snd_nxt = seq + (2U * payload);
    tcp_shift_lwip_cc_hook_segment_tx(
        pcb, &segment3, seq + (2U * payload), payload);
    pcb->snd_nxt = seq + (3U * payload);
    pcb->unacked = (struct tcp_seg *)(void *)&outstanding_sentinel;

    CHECK(sleep_ns(1000000L) == 0);
    sack.left = seq + (2U * payload);
    sack.right = seq + (3U * payload);
    CHECK(tcp_shift_lwip_cc_hook_sack(
              pcb, seq + payload, &sack, 1U) != 0);
    /* RACK processing is deferred because this same ACK also advances
     * cumulative delivery. */
    CHECK(adapter.rack_tlp.rack_end_seq == 0U);
    CHECK(adapter.rack_tlp.fack == 0U);
    CHECK(adapter.rack_tlp.reordering_seen == 0U);

    /* Match tcp_receive(): publish the cumulative ACK before invoking the CC
     * ACK hook. The combined RACK pass must visit segment1 before segment3 by
     * end_seq, so the ordinary hole does not masquerade as packet reordering. */
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(adapter.rack_tlp.rack_end_seq == sack.right);
    CHECK(adapter.rack_tlp.fack == sack.right);
    CHECK(adapter.rack_tlp.reordering_seen == 0U);
    CHECK(stats.rack_reordering_events == 0U);

    pcb->unacked = NULL;
    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    tcp_abort(pcb);

    /* A tail can be first transmitted only after the prior flight has fully
     * drained. In that case there is no later ACK to arm PTO. The bridge's
     * application-limited transition must therefore arm the RFC 8985 TLP
     * timer while that tail remains outstanding. */
    CHECK(tcp_shift_lwip_cc_configure_pacer(&fake_pacer_ops, NULL) == 0);
    CHECK(tcp_shift_lwip_cc_configure_recovery_timer(
              &fake_recovery_ops, &timer_state) == 0);

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    pcb = tcp_new();
    CHECK(pcb != NULL);
    payload = pcb->mss;
    seq = UINT32_C(700000);
    pcb->cwnd = (tcpwnd_size_t)(payload * 8U);
    pcb->ssthresh = (tcpwnd_size_t)(payload * 16U);
    pcb->snd_wnd = (tcpwnd_size_t)(payload * 16U);
    pcb->lastack = seq;
    pcb->snd_nxt = seq;
    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    CHECK(adapter.pacing_flow_id != 0U);

    adapter.rack_tlp.rtt_sample_since_probe = 1U;
    adapter.rack_tlp.srtt_ns = UINT64_C(40000000);
    adapter.rack_tlp.min_rtt_ns = UINT64_C(40000000);
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment3, seq, payload);
    pcb->snd_nxt = seq + payload;
    pcb->unacked = (struct tcp_seg *)(void *)&outstanding_sentinel;
    CHECK(timer_state.schedules == 0U);

    tcp_shift_lwip_cc_mark_app_limited(pcb);
    CHECK(stats.app_limited_enters == 1U);
    CHECK(adapter.app_limited_until_bytes == payload);
    CHECK(timer_state.schedules == 1U);
    CHECK(timer_state.kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP);
    CHECK(timer_state.deadline_ns != 0U);
    CHECK(adapter.recovery_timer_scheduled == 1U);
    CHECK(adapter.recovery_timer_kind == TCP_SHIFT_LWIP_RECOVERY_TIMER_TLP);

    pcb->unacked = NULL;
    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    CHECK(timer_state.cancels == 1U);
    tcp_abort(pcb);
    CHECK(tcp_shift_lwip_cc_clear_recovery_timer() == 0);
    CHECK(tcp_shift_lwip_cc_clear_pacer() == 0);

    printf("rack_tlp_adapter_contract=ok rack_end=%u rack_rtt_ns=%llu "
           "min_rtt_ns=%llu sacked=%u dsack=%llu reo_mult=%u persist=%u\n",
           (unsigned)sack.right,
           (unsigned long long)adapter.rack_tlp.rack_rtt_ns,
           (unsigned long long)adapter.rack_tlp.min_rtt_ns,
           (unsigned)adapter.rack_tlp.segs_sacked,
           (unsigned long long)stats.rack_dsack_events,
           (unsigned)adapter.rack_tlp.reo_wnd_mult,
           (unsigned)adapter.rack_tlp.reo_wnd_persist);
    return 0;
}
