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
            fprintf(stderr,                                                   \
                    "bbr-delivery-snapshot: check failed at %s:%d: %s\n",   \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int pause_for_rtt_sample(void)
{
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};

    return nanosleep(&pause, NULL);
}

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct tcp_pcb *pcb;
    unsigned char outstanding_sentinel;
    uint32_t seq;
    uint16_t payload;
    unsigned char segment1;
    unsigned char segment2;
    unsigned char segment3;
    unsigned char segment4;
    unsigned char segment5;
    unsigned char segment6;
    unsigned char segment7;
    unsigned char segment8;
    uint32_t seq4;
    uint32_t seq5;
    uint32_t seq6;
    uint32_t seq7;
    uint32_t seq8;
    struct tcp_shift_lwip_sack_range sack_range;
    uint64_t srtt_after_clean_samples;

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    outstanding_sentinel = 0U;
    lwip_init();

    pcb = tcp_new();
    CHECK(pcb != NULL);
    CHECK(pcb->mss != 0U);

    payload = pcb->mss;
    seq = UINT32_C(100000);
    pcb->cwnd = (tcpwnd_size_t)(payload * 4U);
    pcb->ssthresh = (tcpwnd_size_t)(16U * 1024U);
    pcb->snd_wnd = (tcpwnd_size_t)(16U * 1024U);
    pcb->lastack = seq;
    pcb->snd_nxt = seq;

    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    CHECK(adapter.srtt.samples == 0U);
    CHECK(adapter.srtt.smoothed_rtt_ns == 0U);

    /* First transmitted payload starts with delivered=0. ACKing it must
     * publish the exact send snapshot and the new cumulative delivered total.
     * The same monotonic ACK read also seeds the high-resolution SRTT state. */
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment1, seq, payload);
    CHECK(stats.delivery_first_tx_events == 1U);
    CHECK(pause_for_rtt_sample() == 0);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(adapter.delivered_bytes == payload);
    CHECK(stats.rate_snapshot_samples == 1U);
    CHECK(stats.rate_snapshot_errors == 0U);
    CHECK(stats.rate_last_prior_delivered_bytes == 0U);
    CHECK(stats.rate_last_delivered_total_bytes == payload);
    CHECK(stats.ack_observation_events == 1U);
    CHECK(stats.ack_last_time_ns != 0U);
    CHECK(stats.ack_last_time_ns == stats.delivery_last_ack_ns);
    CHECK(stats.srtt_updates == 1U);
    CHECK(stats.ack_last_smoothed_rtt_ns != 0U);
    CHECK(adapter.srtt.samples == 1U);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment1, payload);
    CHECK(stats.delivery_live_slots == 0U);

    /* The second payload must carry the first payload's delivered total as its
     * prior-delivered snapshot, proving cumulative round inputs rather than a
     * per-ACK byte delta are exported to the generic CC observation. */
    seq += payload;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment2, seq, payload);
    CHECK(stats.delivery_first_tx_events == 2U);
    CHECK(pause_for_rtt_sample() == 0);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(adapter.delivered_bytes == (uint64_t)payload * 2U);
    CHECK(stats.rate_snapshot_samples == 2U);
    CHECK(stats.rate_snapshot_errors == 0U);
    CHECK(stats.rate_last_prior_delivered_bytes == payload);
    CHECK(stats.rate_last_delivered_total_bytes == (uint64_t)payload * 2U);
    CHECK(stats.rate_last_delivered_total_bytes >
          stats.rate_last_prior_delivered_bytes);
    CHECK(stats.ack_observation_events == 2U);
    CHECK(stats.srtt_updates == 2U);
    CHECK(adapter.srtt.samples == 2U);
    srtt_after_clean_samples = adapter.srtt.smoothed_rtt_ns;
    CHECK(srtt_after_clean_samples != 0U);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment2, payload);
    CHECK(stats.delivery_live_slots == 0U);

    /* Model an RTO-style retransmission after the transport has moved the
     * outstanding segment back to unsent: metadata is still live, while the
     * pre-send unacked list is empty. Linux starts a fresh rate-sampling send
     * phase in this case. Karn filtering must still keep RTT ambiguous. */
    seq += payload;
    pcb->unacked = NULL;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment3, seq, payload);
    CHECK(pause_for_rtt_sample() == 0);
    pcb->unacked = NULL;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment3, seq, payload);
    CHECK(stats.delivery_retransmit_events == 1U);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(stats.rate_snapshot_samples == 3U);
    CHECK(stats.rate_retransmitted_samples == 1U);
    CHECK(stats.rate_last_send_interval_ns == 0U);
    CHECK((stats.rate_last_flags & TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED) != 0U);
    CHECK((stats.rate_last_flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) == 0U);
    CHECK(stats.ack_observation_events == 3U);
    CHECK(stats.srtt_updates == 2U);
    CHECK(adapter.srtt.samples == 2U);
    CHECK(adapter.srtt.smoothed_rtt_ns == srtt_after_clean_samples);
    CHECK(stats.ack_last_smoothed_rtt_ns == srtt_after_clean_samples);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment3, payload);

    CHECK(stats.delivery_live_slots == 0U);

    /* Reproduce a NewReno-style two-hole flight. Segments 4-6 are first sent
     * with delivered=3*MSS. Repairing the first hole lets a partial ACK charge
     * segments 4 and 5, advancing delivered to 5*MSS. The retransmission of
     * segment 6 must therefore refresh its delivery snapshot to that newer
     * delivered point. Linux rate sampling keys the ACK sample to the packet's
     * last transmission, not its original transmission. */
    seq += payload;
    seq4 = seq;
    seq5 = seq4 + payload;
    seq6 = seq5 + payload;

    pcb->unacked = NULL;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment4, seq4, payload);
    pcb->unacked = (struct tcp_seg *)(void *)&outstanding_sentinel;
    pcb->snd_nxt = seq5;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment5, seq5, payload);
    pcb->snd_nxt = seq6;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment6, seq6, payload);
    pcb->snd_nxt = seq6 + payload;
    CHECK(stats.delivery_first_tx_events == 6U);
    CHECK(stats.delivery_live_slots == 3U);

    CHECK(pause_for_rtt_sample() == 0);
    pcb->unacked = (struct tcp_seg *)(void *)&outstanding_sentinel;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment4, seq4, payload);
    CHECK(stats.delivery_retransmit_events == 2U);

    /* Partial ACK through segment 5: the second hole starts at segment 6. */
    pcb->lastack = seq6;
    CHECK(tcp_shift_lwip_cc_hook_ack(
              pcb, (tcpwnd_size_t)((uint32_t)payload * 2U)) != 0);
    CHECK(adapter.delivered_bytes == (uint64_t)payload * 5U);
    CHECK(stats.rate_snapshot_samples == 4U);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment4, payload);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment5, payload);
    CHECK(stats.delivery_live_slots == 1U);

    /* Retransmit the second hole only after the partial ACK advanced delivery.
     * Its final ACK must use this retransmission snapshot (prior_delivered=5P),
     * while Karn filtering still marks RTT as ambiguous. */
    CHECK(pause_for_rtt_sample() == 0);
    pcb->unacked = (struct tcp_seg *)(void *)&outstanding_sentinel;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment6, seq6, payload);
    CHECK(stats.delivery_retransmit_events == 3U);
    pcb->lastack = seq6 + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(adapter.delivered_bytes == (uint64_t)payload * 6U);
    CHECK(stats.rate_snapshot_samples == 5U);
    CHECK(stats.rate_snapshot_errors == 0U);
    CHECK(stats.rate_last_prior_delivered_bytes == (uint64_t)payload * 5U);
    CHECK(stats.rate_last_delivered_total_bytes == (uint64_t)payload * 6U);
    CHECK(stats.rate_last_delivered_bytes == payload);
    CHECK(stats.rate_last_send_interval_ns > 0U);
    CHECK(stats.rate_last_send_interval_ns <= stats.rate_last_interval_ns);
    CHECK((stats.rate_last_flags & TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED) != 0U);
    CHECK((stats.rate_last_flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) == 0U);
    CHECK(stats.ack_observation_events == 5U);
    CHECK(stats.srtt_updates == 2U);
    CHECK(adapter.srtt.samples == 2U);
    CHECK(adapter.srtt.smoothed_rtt_ns == srtt_after_clean_samples);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment6, payload);

    CHECK(stats.delivery_live_slots == 0U);

    /* SACK-aware delivery accounting must credit out-of-order delivery at the
     * SACK observation, then charge only the remaining hole when cumulative
     * ACK catches up. This models Linux acked_sacked accounting without
     * double-growing BBR cwnd on the later cumulative ACK. */
    adapter.sack_delivery_policy = 1U;
    seq7 = seq6 + payload;
    seq8 = seq7 + payload;
    pcb->lastack = seq7;
    pcb->unacked = NULL;
    pcb->snd_nxt = seq7;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment7, seq7, payload);
    pcb->unacked = (struct tcp_seg *)(void *)&outstanding_sentinel;
    pcb->snd_nxt = seq8;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment8, seq8, payload);
    pcb->snd_nxt = seq8 + payload;
    CHECK(stats.delivery_live_slots == 2U);

    CHECK(pause_for_rtt_sample() == 0);
    sack_range.left = seq8;
    sack_range.right = seq8 + payload;
    CHECK(tcp_shift_lwip_cc_hook_sack(pcb, &sack_range, 1U) != 0);
    CHECK(adapter.delivered_bytes == (uint64_t)payload * 7U);
    CHECK(stats.delivery_sack_events == 1U);
    CHECK(stats.delivery_sack_payload_bytes == payload);
    CHECK(stats.rate_snapshot_samples == 6U);
    CHECK(tcp_shift_lwip_cc_hook_effective_cwnd(pcb) ==
          (uint32_t)pcb->cwnd + payload);

    /* Cumulative ACK spans both the hole and the already-SACKed segment. Only
     * segment7 is newly delivered here; segment8 must not be credited twice. */
    pcb->lastack = seq8 + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(
              pcb, (tcpwnd_size_t)((uint32_t)payload * 2U)) != 0);
    CHECK(adapter.delivered_bytes == (uint64_t)payload * 8U);
    CHECK(stats.delivery_sack_payload_bytes == payload);
    CHECK(stats.rate_snapshot_samples == 7U);
    CHECK(stats.rate_last_delivered_total_bytes == (uint64_t)payload * 8U);
    CHECK(tcp_shift_lwip_cc_hook_effective_cwnd(pcb) == (uint32_t)pcb->cwnd);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment7, payload);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment8, payload);

    CHECK(stats.delivery_live_slots == 0U);
    CHECK(stats.delivery_metadata_misses == 0U);
    CHECK(stats.delivery_clock_errors == 0U);
    CHECK(stats.delivery_timestamp_regressions == 0U);

    pcb->unacked = NULL;
    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    tcp_abort(pcb);

    printf("bbr_delivery_snapshot=ok samples=%llu errors=%llu "
           "ack_observations=%llu srtt_updates=%llu srtt_ns=%llu "
           "retransmitted_samples=%llu retransmit_events=%llu "
           "sack_events=%llu sack_payload_bytes=%llu "
           "prior_delivered=%llu delivered_total=%llu payload=%u\n",
           (unsigned long long)stats.rate_snapshot_samples,
           (unsigned long long)stats.rate_snapshot_errors,
           (unsigned long long)stats.ack_observation_events,
           (unsigned long long)stats.srtt_updates,
           (unsigned long long)stats.ack_last_smoothed_rtt_ns,
           (unsigned long long)stats.rate_retransmitted_samples,
           (unsigned long long)stats.delivery_retransmit_events,
           (unsigned long long)stats.delivery_sack_events,
           (unsigned long long)stats.delivery_sack_payload_bytes,
           (unsigned long long)stats.rate_last_prior_delivered_bytes,
           (unsigned long long)stats.rate_last_delivered_total_bytes,
           (unsigned)payload);
    return 0;
}
