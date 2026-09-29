#ifndef TCP_SHIFT_LWIP_PRR_H
#define TCP_SHIFT_LWIP_PRR_H

#include <stdint.h>

/*
 * RFC 9937 Proportional Rate Reduction (PRR) transport state.
 *
 * Congestion control remains responsible for choosing ssthresh. Loss
 * detection remains the responsibility of RACK/NewReno/SACK. PRR only
 * regulates how many bytes may be sent in response to ACK-delivered data
 * while fast recovery is active.
 */
struct tcp_shift_prr {
    uint64_t prr_delivered;
    uint64_t prr_out;
    uint32_t recover_fs;
    uint32_t ssthresh;
    uint8_t active;
};

struct tcp_shift_prr_ack {
    uint32_t delivered_data;
    uint32_t inflight;
    uint32_t smss;
    uint8_t safe_ack;
};

struct tcp_shift_prr_result {
    uint32_t sndcnt;
    uint32_t cwnd;
};

void tcp_shift_prr_reset(struct tcp_shift_prr *prr);

int tcp_shift_prr_init(struct tcp_shift_prr *prr,
                       uint32_t recover_fs,
                       uint32_t ssthresh);

int tcp_shift_prr_on_ack(struct tcp_shift_prr *prr,
                         const struct tcp_shift_prr_ack *ack,
                         struct tcp_shift_prr_result *result);

int tcp_shift_prr_on_send(struct tcp_shift_prr *prr, uint32_t sent_bytes);

int tcp_shift_prr_complete(struct tcp_shift_prr *prr, uint32_t *cwnd);

#endif /* TCP_SHIFT_LWIP_PRR_H */
