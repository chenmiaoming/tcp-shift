#ifndef TCP_SHIFT_LWIP_BBR3_RACK_EVIDENCE_H
#define TCP_SHIFT_LWIP_BBR3_RACK_EVIDENCE_H

#include <stdint.h>
#include "lwip/rack_tlp.h"

/*
 * One compact state record per existing P5 delivery slot; there is no
 * second dynamically allocated segment ledger. The actual PCB adapter must
 * supply its recorded send snapshot and monotonically increasing transmit
 * generation. This qualification-only API never invokes TCP retransmission,
 * updates cwnd or reports a loss on RTO/TLP alone.
 */
struct tcp_shift_bbr3_rack_attempt {
    uint64_t generation;
    uint64_t tx_ns;
    uint64_t delivered_at_tx;
    uint64_t qualified_generation;
    uint64_t revoked_generation;
    uint32_t seq_start;
    uint32_t prior_inflight_bytes;
    uint16_t payload_bytes;
    uint16_t delivered_bytes;
    uint8_t retransmitted;
    uint8_t app_limited;
    uint8_t probe_sample;
};

struct tcp_shift_bbr3_rack_tx {
    uint64_t generation;
    uint64_t tx_ns;
    uint64_t delivered_at_tx;
    uint32_t seq_start;
    uint32_t prior_inflight_bytes;
    uint16_t payload_bytes;
    uint16_t already_delivered_bytes;
    uint8_t retransmitted;
    uint8_t app_limited;
    uint8_t probe_sample;
};

struct tcp_shift_bbr3_rack_loss {
    uint64_t generation;
    uint64_t tx_ns;
    uint64_t delivered_at_tx;
    uint32_t seq_start;
    uint32_t tx_inflight_bytes;
    uint16_t newly_lost_bytes;
    uint8_t retransmitted;
    uint8_t app_limited;
    uint8_t probe_sample;
};

void tcp_shift_bbr3_rack_attempt_init(
    struct tcp_shift_bbr3_rack_attempt *attempt);

/* Record a real send or retransmission, not a retransmission request.
 * Transmission generations prevent repeated loss calls against the same
 * send attempt from being counted twice; sequence numbers can wrap. */
int tcp_shift_bbr3_rack_note_tx(
    struct tcp_shift_bbr3_rack_attempt *attempt,
    const struct tcp_shift_bbr3_rack_tx *tx);

/* ACK/SACK delivered progress is cumulative per payload range and monotonic.
 * Duplicate SACK and cumulative ACK must not resurrect lost payload. */
int tcp_shift_bbr3_rack_note_delivery(
    struct tcp_shift_bbr3_rack_attempt *attempt,
    uint16_t total_delivered_bytes);

/* Returns 1 only when RACK's RFC 8985 transmission-time test is mature,
 * and 0 for immature/duplicate/fully-delivered attempts. Other invalid
 * inputs return -1. An RTO's snd_una special case is NOT used here.
 * 'out' is written only on an actual, first qualified loss. */
int tcp_shift_bbr3_rack_qualify_loss(
    struct tcp_shift_bbr3_rack_attempt *attempt,
    struct tcp_shift_rack_tlp_state *rack,
    uint64_t now_ns,
    unsigned in_recovery,
    struct tcp_shift_bbr3_rack_loss *out);

/* Only the transport's separately validated DSACK/spurious-retransmission
 * association may call this function. A raw duplicate ACK is insufficient.
 * Returns 1 on a fresh undo, 0 if unmatched/repeated, -1 on bad arguments. */
int tcp_shift_bbr3_rack_undo_spurious(
    struct tcp_shift_bbr3_rack_attempt *attempt,
    uint64_t proved_spurious_generation);

#endif
