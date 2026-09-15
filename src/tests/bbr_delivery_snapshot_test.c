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
    uint32_t seq;
    uint16_t payload;
    unsigned char segment1;
    unsigned char segment2;
    unsigned char segment3;
    uint64_t srtt_after_clean_samples;

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
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

    /* Retransmitting the third segment marks its RTT ambiguous. The ACK still
     * carries a monotonic observation and delivery snapshot, but Karn filtering
     * must prevent it from updating the RFC 6298 SRTT state. */
    seq += payload;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment3, seq, payload);
    CHECK(pause_for_rtt_sample() == 0);
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment3, seq, payload);
    CHECK(stats.delivery_retransmit_events == 1U);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(stats.rate_snapshot_samples == 3U);
    CHECK(stats.rate_retransmitted_samples == 1U);
    CHECK((stats.rate_last_flags & TCP_SHIFT_CC_RATE_SAMPLE_RETRANSMITTED) != 0U);
    CHECK((stats.rate_last_flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) == 0U);
    CHECK(stats.ack_observation_events == 3U);
    CHECK(stats.srtt_updates == 2U);
    CHECK(adapter.srtt.samples == 2U);
    CHECK(adapter.srtt.smoothed_rtt_ns == srtt_after_clean_samples);
    CHECK(stats.ack_last_smoothed_rtt_ns == srtt_after_clean_samples);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment3, payload);

    CHECK(stats.delivery_live_slots == 0U);
    CHECK(stats.delivery_metadata_misses == 0U);
    CHECK(stats.delivery_clock_errors == 0U);
    CHECK(stats.delivery_timestamp_regressions == 0U);

    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    tcp_abort(pcb);

    printf("bbr_delivery_snapshot=ok samples=%llu errors=%llu "
           "ack_observations=%llu srtt_updates=%llu srtt_ns=%llu "
           "retransmitted_samples=%llu prior_delivered=%llu "
           "delivered_total=%llu payload=%u\n",
           (unsigned long long)stats.rate_snapshot_samples,
           (unsigned long long)stats.rate_snapshot_errors,
           (unsigned long long)stats.ack_observation_events,
           (unsigned long long)stats.srtt_updates,
           (unsigned long long)stats.ack_last_smoothed_rtt_ns,
           (unsigned long long)stats.rate_retransmitted_samples,
           (unsigned long long)stats.rate_last_prior_delivered_bytes,
           (unsigned long long)stats.rate_last_delivered_total_bytes,
           (unsigned)payload);
    return 0;
}
