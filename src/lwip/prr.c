#include "lwip/prr.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static uint64_t tcp_shift_prr_add_sat_u64(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static uint32_t tcp_shift_prr_add_sat_u32(uint32_t a, uint64_t b)
{
    return b > UINT32_MAX - a ? UINT32_MAX : a + (uint32_t)b;
}

static uint64_t tcp_shift_prr_div_round_up_product(uint64_t value,
                                                    uint32_t multiplier,
                                                    uint32_t divisor)
{
    uint64_t quotient;
    uint64_t remainder;
    uint64_t scaled_quotient;
    uint64_t scaled_remainder;

    if (divisor == 0U) {
        return UINT64_MAX;
    }

    /*
     * value is a cumulative byte counter while multiplier/divisor are bounded
     * by TCP's 32-bit sequence/window domain. Split value before multiplying
     * so the RFC 9937 ceil(prr_delivered * ssthresh / RecoverFS)
     * calculation does not require a non-standard 128-bit integer type.
     */
    quotient = value / divisor;
    remainder = value % divisor;
    if (quotient > UINT64_MAX / multiplier) {
        return UINT64_MAX;
    }
    scaled_quotient = quotient * multiplier;
    scaled_remainder = remainder * (uint64_t)multiplier;
    scaled_remainder =
        scaled_remainder / divisor +
        (scaled_remainder % divisor != 0U ? 1U : 0U);
    return tcp_shift_prr_add_sat_u64(scaled_quotient, scaled_remainder);
}

void tcp_shift_prr_reset(struct tcp_shift_prr *prr)
{
    if (prr != NULL) {
        memset(prr, 0, sizeof(*prr));
    }
}

int tcp_shift_prr_init(struct tcp_shift_prr *prr,
                       uint32_t recover_fs,
                       uint32_t ssthresh)
{
    if (prr == NULL || recover_fs == 0U || ssthresh == 0U) {
        return -1;
    }

    *prr = (struct tcp_shift_prr){
        .recover_fs = recover_fs,
        .ssthresh = ssthresh,
        .active = 1U,
    };
    return 0;
}

int tcp_shift_prr_on_ack(struct tcp_shift_prr *prr,
                         const struct tcp_shift_prr_ack *ack,
                         struct tcp_shift_prr_result *result)
{
    uint64_t sndcnt;
    uint64_t allowed;
    uint64_t delivered_minus_out;
    uint32_t room;

    if (prr == NULL || ack == NULL || result == NULL ||
        prr->active == 0U || prr->recover_fs == 0U ||
        prr->ssthresh == 0U || ack->smss == 0U) {
        return -1;
    }

    result->sndcnt = 0U;
    result->cwnd = ack->inflight;

    /* RFC 9937 section 6.2: an ACK delivering no new data grants no credit. */
    if (ack->delivered_data == 0U) {
        return 0;
    }

    prr->prr_delivered = tcp_shift_prr_add_sat_u64(
        prr->prr_delivered, ack->delivered_data);

    if (ack->inflight > prr->ssthresh) {
        /* Proportional Rate Reduction; integer division rounds upward. */
        allowed = tcp_shift_prr_div_round_up_product(
            prr->prr_delivered, prr->ssthresh, prr->recover_fs);
        sndcnt = allowed > prr->prr_out ? allowed - prr->prr_out : 0U;
    } else {
        /* PRR-CRB by default, switching to PRR-SSRB on SafeACK. */
        delivered_minus_out =
            prr->prr_delivered > prr->prr_out
                ? prr->prr_delivered - prr->prr_out
                : 0U;
        sndcnt = delivered_minus_out > ack->delivered_data
                     ? delivered_minus_out
                     : ack->delivered_data;
        if (ack->safe_ack != 0U) {
            sndcnt = tcp_shift_prr_add_sat_u64(sndcnt, ack->smss);
        }
        room = prr->ssthresh > ack->inflight
                   ? prr->ssthresh - ack->inflight
                   : 0U;
        if (sndcnt > room) {
            sndcnt = room;
        }
    }

    /* Force exactly one initial fast retransmit if no credit exists yet. */
    if (prr->prr_out == 0U && sndcnt == 0U) {
        sndcnt = ack->smss;
    }

    result->sndcnt =
        sndcnt > UINT32_MAX ? UINT32_MAX : (uint32_t)sndcnt;
    result->cwnd = tcp_shift_prr_add_sat_u32(
        ack->inflight, result->sndcnt);
    return 0;
}

int tcp_shift_prr_on_send(struct tcp_shift_prr *prr, uint32_t sent_bytes)
{
    if (prr == NULL || prr->active == 0U || sent_bytes == 0U) {
        return -1;
    }

    prr->prr_out = tcp_shift_prr_add_sat_u64(prr->prr_out, sent_bytes);
    return 0;
}

int tcp_shift_prr_complete(struct tcp_shift_prr *prr, uint32_t *cwnd)
{
    uint32_t target;

    if (prr == NULL || cwnd == NULL || prr->active == 0U ||
        prr->ssthresh == 0U) {
        return -1;
    }

    target = prr->ssthresh;
    tcp_shift_prr_reset(prr);
    *cwnd = target;
    return 0;
}
