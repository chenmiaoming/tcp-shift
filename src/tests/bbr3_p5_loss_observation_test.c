#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/cc_adapter.h"
#include "lwip/bbr3_rack_evidence.h"
#include "lwip/bbr3_loss_round.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "bbr3-p5-rack:%d: %s\n", __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static int delay_sample(void)
{
    struct timespec ts = {0, 1000000L};

    return nanosleep(&ts, NULL);
}

int main(void)
{
    struct tcp_shift_lwip_cc_adapter adapter;
    struct tcp_shift_lwip_cc_stats stats;
    struct tcp_shift_lwip_sack_range sack;
    struct tcp_shift_bbr3_rack_loss evidence;
    struct tcp_shift_bbr3_loss_round round = {0};
    struct tcp_shift_bbr3_loss_round_result result = {0};
    struct tcp_pcb *pcb;
    unsigned char first_seg;
    unsigned char second_seg;
    unsigned char third_seg;
    uint64_t witness_ns;
    uint64_t generation;
    uint64_t delivered_before;
    uint32_t seq = UINT32_C(100000);
    uint32_t cwnd_before;
    uint16_t mss;

    memset(&adapter, 0, sizeof(adapter));
    memset(&stats, 0, sizeof(stats));
    memset(&evidence, 0, sizeof(evidence));
    lwip_init();

    pcb = tcp_new();
    CHECK(pcb != NULL);
    mss = pcb->mss;
    CHECK(mss != 0U);
    pcb->cwnd = (tcpwnd_size_t)(10U * mss);
    pcb->ssthresh = (tcpwnd_size_t)(20U * mss);
    pcb->snd_wnd = (tcpwnd_size_t)(20U * mss);
    pcb->lastack = seq;
    pcb->snd_nxt = seq;
    CHECK(tcp_shift_lwip_cc_adapter_bind(&adapter, pcb, &stats) == 0);
    adapter.sack_delivery_policy = 1U;

    /* A real negotiated SACK is required, not just local bookkeeping. */
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &first_seg, seq, mss);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &first_seg, monotonic_ns(),
              &evidence) < 0);
    tcp_set_flags(pcb, TF_SACK);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &first_seg, monotonic_ns(),
              &evidence) == 0);

    /* SACK B after sending it later: actual hook path supplies RACK's
     * younger-delivery witness, while the hole A stays undelivered. */
    pcb->snd_nxt = seq + mss;
    CHECK(delay_sample() == 0);
    tcp_shift_lwip_cc_hook_segment_tx(
        pcb, &second_seg, seq + mss, mss);
    pcb->snd_nxt = seq + 2U * mss;
    sack.left = seq + mss;
    sack.right = seq + 2U * mss;
    CHECK(tcp_shift_lwip_cc_hook_sack(
              pcb, pcb->lastack, &sack, 1U) != 0);
    CHECK(adapter.rack_tlp.rack_xmit_ts_ns != 0U);
    CHECK(adapter.delivered_bytes == mss);
    delivered_before = adapter.delivered_bytes;
    CHECK(tcp_shift_bbr3_loss_round_start(
              &round, delivered_before) == 0);
    cwnd_before = pcb->cwnd;
    witness_ns = adapter.rack_tlp.rack_xmit_ts_ns +
                 UINT64_C(2000000000);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &first_seg, witness_ns, &evidence) == 1);
    CHECK(evidence.seq_start == seq);
    CHECK(evidence.newly_lost_bytes == mss);
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &evidence) == 0);
    CHECK(evidence.tx_inflight_bytes == mss);
    CHECK(evidence.probe_sample == 0U); /* unknown provenance */
    CHECK(evidence.retransmitted == 0U);
    generation = evidence.generation;
    CHECK(generation != 0U);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &first_seg, witness_ns, &evidence) == 0);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &second_seg, witness_ns, &evidence) == 0);
    CHECK(adapter.delivered_bytes == delivered_before);
    CHECK(pcb->cwnd == cwnd_before);

    /* Retx A after B was SACKed; the new send-time inflight is only A,
     * not the stale two-MSS raw sequence-space distance. */
    CHECK(delay_sample() == 0);
    tcp_shift_lwip_cc_hook_segment_tx(pcb, &first_seg, seq, mss);
    CHECK(stats.delivery_retransmit_events == 1U);

    /* C supplies a second, genuinely later SACK/RACK delivery witness. */
    CHECK(delay_sample() == 0);
    tcp_shift_lwip_cc_hook_segment_tx(
        pcb, &third_seg, seq + 2U * mss, mss);
    pcb->snd_nxt = seq + 3U * mss;
    sack.left = seq + 2U * mss;
    sack.right = seq + 3U * mss;
    CHECK(tcp_shift_lwip_cc_hook_sack(
              pcb, pcb->lastack, &sack, 1U) != 0);
    CHECK(adapter.delivered_bytes == (uint64_t)mss * 2U);

    /* A partial cumulative ACK delivers half of A. RACK may only mark
     * the remaining half as lost; already delivered bytes never repeat. */
    pcb->lastack = seq + mss / 2U;
    CHECK(tcp_shift_lwip_cc_hook_ack(pcb, mss / 2U) != 0);
    CHECK(adapter.delivered_bytes ==
          (uint64_t)mss * 2U + mss / 2U);
    cwnd_before = pcb->cwnd;
    witness_ns = adapter.rack_tlp.rack_xmit_ts_ns +
                 UINT64_C(2000000000);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &first_seg, witness_ns, &evidence) == 1);
    CHECK(evidence.generation > generation);
    CHECK(evidence.newly_lost_bytes == (uint16_t)(mss - mss / 2U));
    CHECK(tcp_shift_bbr3_loss_round_add(&round, &evidence) == 0);
    CHECK(evidence.tx_inflight_bytes == mss);
    CHECK(evidence.retransmitted == 1U);
    CHECK(evidence.probe_sample == 0U);
    CHECK(pcb->cwnd == cwnd_before);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &first_seg, witness_ns, &evidence) == 0);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &third_seg, witness_ns, &evidence) == 0);

    /* The test marks the packet-timed boundary explicitly; the live lwIP
     * callback does NOT yet supply this boundary to the controller. These
     * losses came from different send snapshots and cannot share a ratio. */
    CHECK(tcp_shift_bbr3_loss_round_finish(
              &round, adapter.delivered_bytes, 0U, &result) < 0);
    CHECK(tcp_shift_bbr3_loss_round_finish(
              &round, adapter.delivered_bytes, 1U, &result) == 0);
    CHECK(result.events == 2U);
    CHECK(result.same_send_snapshot == 0U);
    CHECK(result.loss_fraction_valid == 0U);
    CHECK(result.qualified_probe_excess_loss == 0U);

    /* No observer can be invoked on stale segment state after unbind. */
    tcp_shift_lwip_cc_adapter_unbind(&adapter);
    CHECK(tcp_shift_lwip_cc_bbr3_observe_rack_loss(
              &adapter, &first_seg, witness_ns, &evidence) < 0);
    tcp_abort(pcb);
    puts("bbr3_p5_loss_observation=ok sack_verified=ok "
         "partial_loss=ok retransmit_generation=ok "
         "inflight_refreshed=ok no_cwnd_mutation=ok");
    return 0;
}
