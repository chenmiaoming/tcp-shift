#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct tcp_pcb *pcb;
    uint32_t seq;
    uint16_t payload;
    unsigned char segment1;
    unsigned char segment2;

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

    /* First transmitted payload starts with delivered=0. ACKing it must
     * publish the exact send snapshot and the new cumulative delivered total. */
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment1, seq, payload);
    CHECK(stats.delivery_first_tx_events == 1U);
    pcb->snd_nxt = seq + payload;
    pcb->lastack = seq + payload;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, payload) != 0);
    CHECK(adapter.delivered_bytes == payload);
    CHECK(stats.rate_snapshot_samples == 1U);
    CHECK(stats.rate_snapshot_errors == 0U);
    CHECK(stats.rate_last_prior_delivered_bytes == 0U);
    CHECK(stats.rate_last_delivered_total_bytes == payload);
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment1, payload);
    CHECK(stats.delivery_live_slots == 0U);

    /* The second payload must carry the first payload's delivered total as its
     * prior-delivered snapshot, proving cumulative round inputs rather than a
     * per-ACK byte delta are exported to the generic CC observation. */
    seq += payload;
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &segment2, seq, payload);
    CHECK(stats.delivery_first_tx_events == 2U);
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
    tcp_shift_lwip_cc_hook_segment_acked(pcb, &segment2, payload);
    CHECK(stats.delivery_live_slots == 0U);
    CHECK(stats.delivery_metadata_misses == 0U);

    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    tcp_abort(pcb);

    printf("bbr_delivery_snapshot=ok samples=%llu errors=%llu "
           "prior_delivered=%llu delivered_total=%llu payload=%u\n",
           (unsigned long long)stats.rate_snapshot_samples,
           (unsigned long long)stats.rate_snapshot_errors,
           (unsigned long long)stats.rate_last_prior_delivered_bytes,
           (unsigned long long)stats.rate_last_delivered_total_bytes,
           (unsigned)payload);
    return 0;
}
