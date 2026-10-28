#include "lwip/bbr3_binding.h"

#include <limits.h>
#include <stddef.h>

static int tcp_shift_lwip_bbr3_live(const struct tcp_shift_lwip_bbr3_binding *b)
{
    return b != NULL && b->initialized != 0U &&
           b->adapter != NULL && b->pcb != NULL &&
           b->adapter->bound != 0U &&
           b->adapter->pcb == b->pcb &&
           b->adapter->sack_delivery_policy != 0U &&
           b->pcb->mss == b->controller.mss_bytes;
}

int tcp_shift_lwip_bbr3_binding_init(
    struct tcp_shift_lwip_bbr3_binding *b,
    const struct tcp_shift_lwip_cc_adapter *adapter,
    const struct tcp_pcb *pcb,
    uint32_t flow_seed,
    uint64_t now_ns)
{
    if (b == NULL || adapter == NULL || pcb == NULL ||
        b->initialized != 0U || adapter->bound == 0U ||
        adapter->pcb != pcb || adapter->sack_delivery_policy == 0U ||
        pcb->mss == 0U ||
        pcb->cwnd < (uint32_t)pcb->mss * 4U ||
        now_ns == 0U || flow_seed == 0U) {
        return -1;
    }
    if (tcp_shift_bbr3_controller_init(
            &b->controller, (uint32_t)pcb->cwnd,
            (uint32_t)pcb->mss, flow_seed, now_ns) < 0) {
        return -1;
    }
    b->adapter = adapter;
    b->pcb = pcb;
    b->last_delivered_bytes = adapter->delivered_bytes;
    b->last_ack_ns = now_ns;
    b->accepted_samples = 0U;
    b->rejected_samples = 0U;
    b->initialized = 1U;
    return 0;
}

int tcp_shift_lwip_bbr3_binding_on_ack(
    struct tcp_shift_lwip_bbr3_binding *b,
    const struct tcp_shift_cc_ack *ack,
    struct tcp_shift_bbr3_controller_policy *candidate)
{
    struct tcp_shift_bbr3_controller_ack input = {0};
    const struct tcp_shift_cc_rate_sample *rate;
    uint64_t delivered_since_last;
    uint64_t sampled;
    uint32_t flight;

    if (!tcp_shift_lwip_bbr3_live(b) || ack == NULL ||
        candidate == NULL) {
        return -1;
    }
    rate = &ack->rate;
    if (ack->ack_time_ns == 0U ||
        ack->ack_time_ns < b->last_ack_ns ||
        (rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_VALID) == 0U ||
        rate->delivery_rate_bytes_per_sec == 0U ||
        rate->interval_ns == 0U ||
        rate->prior_delivered_bytes > rate->delivered_total_bytes ||
        rate->delivered_total_bytes <= b->last_delivered_bytes ||
        rate->delivered_total_bytes != b->adapter->delivered_bytes ||
        rate->prior_inflight_bytes == 0U ||
        ((rate->flags & TCP_SHIFT_CC_RATE_SAMPLE_RTT_VALID) != 0U &&
         rate->rtt_ns == 0U)) {
        b->rejected_samples++;
        return -1;
    }
    delivered_since_last =
        rate->delivered_total_bytes - b->last_delivered_bytes;
    sampled = rate->delivered_total_bytes - rate->prior_delivered_bytes;
    /* The existing adapter's SACK-aware policy credits newly delivered
     * payload once, regardless of cumulative ACK vs a SACK-only ACK.
     * The full sampled flight (RS.delivered) is intentionally separate. */
    if (delivered_since_last > UINT32_MAX ||
        ack->acked_bytes != (uint32_t)delivered_since_last ||
        sampled == 0U ||
        rate->delivered_bytes !=
            (sampled > UINT32_MAX ? UINT32_MAX : (uint32_t)sampled)) {
        b->rejected_samples++;
        return -1;
    }

    flight = b->pcb->snd_nxt - b->pcb->lastack;
    input.rate = *rate;
    input.now_ns = ack->ack_time_ns;
    input.acked_bytes = ack->acked_bytes;
    input.inflight_bytes = flight;
    /* Metadata captured when the reference packet was transmitted. */
    input.tx_inflight_bytes = rate->prior_inflight_bytes;

    /* Not yet represented in the generic lwIP hook ABI:
     *   - newly lost bytes tied to the relevant send-time packet snapshot;
     *   - CE-marked bytes from an eligible, negotiated ECN ACK;
     *   - packet-timed loss-round completion and ProbeBW send provenance;
     *   - cwnd-limited status from the sender.
     * Supplying zero is deliberate fail-closed behavior. A synthetic
     * aggregated on_loss count would be unsafe in the BBRv3 model. */
    input.lost_bytes = 0U;
    input.ce_bytes = 0U;
    input.validated_loss = 0U;
    input.ecn_eligible = 0U;
    input.loss_round_end = 0U;
    input.probe_sample = 0U;
    input.cwnd_limited = 0U;
    input.idle_restart = 0U;
    if (tcp_shift_bbr3_controller_on_ack(
            &b->controller, &input, candidate) < 0) {
        b->rejected_samples++;
        return -1;
    }
    b->last_delivered_bytes = rate->delivered_total_bytes;
    b->last_ack_ns = ack->ack_time_ns;
    b->accepted_samples++;
    return 0;
}
