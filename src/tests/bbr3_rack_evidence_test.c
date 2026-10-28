#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/bbr3_rack_evidence.h"

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "bbr3-rack-evidence:%d: %s\n", __LINE__, #test); \
        return 1; \
    } \
} while (0)

static struct tcp_shift_bbr3_rack_tx tx(uint64_t generation,
                                        uint64_t when, uint32_t seq,
                                        uint16_t bytes)
{
    struct tcp_shift_bbr3_rack_tx t;
    memset(&t, 0, sizeof(t));
    t.generation = generation;
    t.tx_ns = when;
    t.seq_start = seq;
    t.payload_bytes = bytes;
    t.prior_inflight_bytes = 10000U;
    t.delivered_at_tx = 3000U;
    t.probe_sample = 1U;
    return t;
}

static void rack_newer(struct tcp_shift_rack_tlp_state *r,
                       uint64_t when, uint32_t end_seq)
{
    tcp_shift_rack_tlp_init(r);
    r->rack_xmit_ts_ns = when;
    r->rack_end_seq = end_seq;
    r->rack_rtt_ns = 1000U;
    r->min_rtt_ns = 1000U;
}

static int test_mature_rack_once_and_spurious(void)
{
    struct tcp_shift_bbr3_rack_attempt a;
    struct tcp_shift_rack_tlp_state rack;
    struct tcp_shift_bbr3_rack_tx t = tx(1U, 1000U, 10000U, 1000U);
    struct tcp_shift_bbr3_rack_loss loss = {0};

    tcp_shift_bbr3_rack_attempt_init(&a);
    rack_newer(&rack, 2500U, 12000U);
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) == 0);
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) < 0);
    CHECK(tcp_shift_bbr3_rack_note_delivery(&a, 200U) == 0);
    CHECK(tcp_shift_bbr3_rack_note_delivery(&a, 200U) == 0);
    CHECK(tcp_shift_bbr3_rack_note_delivery(&a, 199U) < 0);

    loss.newly_lost_bytes = 999U; /* must not modify on an immature verdict */
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 1001U, 0U, &loss) == 0);
    CHECK(loss.newly_lost_bytes == 999U);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 100000U, 0U, &loss) == 1);
    CHECK(loss.newly_lost_bytes == 800U);
    CHECK(loss.seq_start == 10000U);
    CHECK(loss.tx_inflight_bytes == 10000U);
    CHECK(loss.delivered_at_tx == 3000U);
    CHECK(loss.tx_ns == 1000U);
    CHECK(loss.generation == 1U);
    CHECK(loss.probe_sample == 1U);
    CHECK(loss.retransmitted == 0U);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 200000U, 1U, &loss) == 0);
    CHECK(tcp_shift_bbr3_rack_undo_spurious(&a, 0U) < 0);
    CHECK(tcp_shift_bbr3_rack_undo_spurious(&a, 2U) == 0);
    CHECK(tcp_shift_bbr3_rack_undo_spurious(&a, 1U) == 1);
    CHECK(tcp_shift_bbr3_rack_undo_spurious(&a, 1U) == 0);

    /* The same sequence range can be genuinely lost on its retransmission,
     * but only after a *different, newer* transmit generation qualifies. */
    t = tx(2U, 200000U, 10000U, 1000U);
    t.retransmitted = 1U;
    t.app_limited = 1U;
    t.probe_sample = 0U;
    t.already_delivered_bytes = 200U;
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) == 0);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 200001U, 0U, &loss) == 0);
    rack_newer(&rack, 220000U, 12000U);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 300000U, 0U, &loss) == 1);
    CHECK(loss.generation == 2U);
    CHECK(loss.newly_lost_bytes == 800U);
    CHECK(loss.retransmitted == 1U);
    CHECK(loss.app_limited == 1U);
    CHECK(loss.probe_sample == 0U);
    CHECK(tcp_shift_bbr3_rack_undo_spurious(&a, 1U) == 0);
    CHECK(tcp_shift_bbr3_rack_undo_spurious(&a, 2U) == 1);
    return 0;
}

static int test_sack_delivery_prevents_loss(void)
{
    struct tcp_shift_bbr3_rack_attempt a;
    struct tcp_shift_rack_tlp_state rack;
    struct tcp_shift_bbr3_rack_tx t = tx(7U, 1000U, 20000U, 1000U);
    struct tcp_shift_bbr3_rack_loss loss = {0};

    tcp_shift_bbr3_rack_attempt_init(&a);
    rack_newer(&rack, 2500U, 23000U);
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) == 0);
    /* SACK is delivered before later cumulative ACK; both can report the
     * same segment, but monotonic delivered progress makes that harmless. */
    CHECK(tcp_shift_bbr3_rack_note_delivery(&a, 1000U) == 0);
    CHECK(tcp_shift_bbr3_rack_note_delivery(&a, 1000U) == 0);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 100000U, 0U, &loss) == 0);
    CHECK(a.qualified_generation == 0U);
    CHECK(tcp_shift_bbr3_rack_note_delivery(&a, 1001U) < 0);

    /* RTO alone may request a retransmission even when RACK has not
     * received any later delivery evidence; it must not invent a RACK loss. */
    tcp_shift_bbr3_rack_attempt_init(&a);
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) == 0);
    tcp_shift_rack_tlp_init(&rack);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 100000U, 0U, &loss) == 0);
    CHECK(a.qualified_generation == 0U);
    return 0;
}

static int test_sequence_wrap_and_input_guards(void)
{
    struct tcp_shift_bbr3_rack_attempt a;
    struct tcp_shift_rack_tlp_state rack;
    struct tcp_shift_bbr3_rack_tx t = tx(1U, 1000U,
                                        UINT32_MAX - 31U, 64U);
    struct tcp_shift_bbr3_rack_loss loss = {0};

    tcp_shift_bbr3_rack_attempt_init(&a);
    rack_newer(&rack, 3000U, 100U);
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) == 0);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 100000U, 0U, &loss) == 1);
    CHECK(loss.seq_start == UINT32_MAX - 31U);
    CHECK(loss.newly_lost_bytes == 64U);

    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 100000U, 2U, &loss) < 0);
    CHECK(tcp_shift_bbr3_rack_qualify_loss(
              &a, &rack, 999U, 0U, &loss) < 0);
    t = tx(2U, 2000U, 0U, 1000U);
    t.prior_inflight_bytes = 10U;
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) < 0);
    CHECK(a.generation == 1U);
    t.prior_inflight_bytes = 10000U;
    t.already_delivered_bytes = 1000U;
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) < 0);
    CHECK(a.generation == 1U);
    t.already_delivered_bytes = 0U;
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) == 0);
    CHECK(tcp_shift_bbr3_rack_note_tx(&a, &t) < 0);
    return 0;
}

int main(void)
{
    CHECK(test_mature_rack_once_and_spurious() == 0);
    CHECK(test_sack_delivery_prevents_loss() == 0);
    CHECK(test_sequence_wrap_and_input_guards() == 0);
    puts("bbr3_rack_loss_contract=ok mature_rack_only=ok "
         "per_transmission_once=ok sack_dedup=ok "
         "rto_not_inferred=ok dsack_undo_guard=ok seq_wrap=ok");
    return 0;
}
