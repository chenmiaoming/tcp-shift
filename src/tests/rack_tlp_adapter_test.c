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

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct tcp_shift_lwip_sack_range sack;
    struct tcp_pcb *pcb;
    unsigned char outstanding_sentinel;
    unsigned char segment1;
    unsigned char segment2;
    u64_t remaining_ns = 0U;
    uint32_t seq;
    uint16_t payload;
    int status;

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
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
    CHECK(tcp_shift_lwip_cc_hook_sack(pcb, &sack, 1U) != 0);
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

    pcb->unacked = NULL;
    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    tcp_abort(pcb);

    printf("rack_tlp_adapter_contract=ok rack_end=%u rack_rtt_ns=%llu "
           "min_rtt_ns=%llu sacked=%u\n",
           (unsigned)sack.right,
           (unsigned long long)adapter.rack_tlp.rack_rtt_ns,
           (unsigned long long)adapter.rack_tlp.min_rtt_ns,
           (unsigned)adapter.rack_tlp.segs_sacked);
    return 0;
}
