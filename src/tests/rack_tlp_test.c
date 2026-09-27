#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lwip/rack_tlp.h"

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "rack-tlp: check failed at %s:%d: %s\n",        \
                    __FILE__, __LINE__, #expr);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int test_sent_after_and_rack_update(void)
{
    struct tcp_shift_rack_tlp_state state;
    struct tcp_shift_rack_segment older = {
        .xmit_ts_ns = UINT64_C(100000000),
        .seq_start = 1000U,
        .end_seq = 2000U,
    };
    struct tcp_shift_rack_segment newer = {
        .xmit_ts_ns = UINT64_C(110000000),
        .seq_start = 2000U,
        .end_seq = 3000U,
    };

    tcp_shift_rack_tlp_init(&state);
    tcp_shift_rack_set_rtt_estimates(
        &state, UINT64_C(80000000), UINT64_C(100000000));

    CHECK(tcp_shift_rack_sent_after(
              newer.xmit_ts_ns, newer.end_seq,
              older.xmit_ts_ns, older.end_seq) == 1);
    CHECK(tcp_shift_rack_sent_after(
              older.xmit_ts_ns, older.end_seq,
              newer.xmit_ts_ns, newer.end_seq) == 0);
    CHECK(tcp_shift_rack_sent_after(
              UINT64_C(100), 3000U, UINT64_C(100), 2000U) == 1);

    CHECK(tcp_shift_rack_note_delivered(
              &state, &newer, UINT64_C(210000000)) == 1);
    tcp_shift_rack_detect_reordering(&state, &newer);
    CHECK(state.rack_xmit_ts_ns == newer.xmit_ts_ns);
    CHECK(state.rack_end_seq == newer.end_seq);
    CHECK(state.rack_rtt_ns == UINT64_C(100000000));
    CHECK(state.rack_ack_ts_ns == UINT64_C(210000000));
    CHECK(state.fack == newer.end_seq);

    /* An original older segment delivered below FACK proves reordering. */
    CHECK(tcp_shift_rack_note_delivered(
              &state, &older, UINT64_C(220000000)) == 1);
    tcp_shift_rack_detect_reordering(&state, &older);
    CHECK(state.reordering_seen == 1U);
    return 0;
}

static int test_reordering_window_and_loss_deadline(void)
{
    struct tcp_shift_rack_tlp_state state;
    struct tcp_shift_rack_segment delivered = {
        .xmit_ts_ns = UINT64_C(200000000),
        .seq_start = 3000U,
        .end_seq = 4000U,
    };
    struct tcp_shift_rack_segment missing = {
        .xmit_ts_ns = UINT64_C(100000000),
        .seq_start = 2000U,
        .end_seq = 3000U,
    };
    uint64_t remaining = 0U;

    tcp_shift_rack_tlp_init(&state);
    tcp_shift_rack_set_rtt_estimates(
        &state, UINT64_C(100000000), UINT64_C(120000000));
    CHECK(tcp_shift_rack_note_delivered(
              &state, &delivered, UINT64_C(300000000)) == 1);

    CHECK(tcp_shift_rack_reo_wnd(&state, 0U) == UINT64_C(25000000));
    CHECK(tcp_shift_rack_loss_remaining(
              &state, &missing, UINT64_C(224999999), 0U,
              &remaining) == 0);
    CHECK(remaining == 1U);
    CHECK(tcp_shift_rack_loss_remaining(
              &state, &missing, UINT64_C(225000000), 0U,
              &remaining) == 1);

    /* RTO always marks the SND.UNA segment, but later segments still use
     * their RACK deadline. */
    CHECK(tcp_shift_rack_lost_on_rto(
              &state, &missing, 2000U, UINT64_C(150000000), 0U) == 1);
    CHECK(tcp_shift_rack_lost_on_rto(
              &state, &delivered, 2000U, UINT64_C(150000000), 0U) == 0);

    /* With no observed reordering, 3 SACKed segments collapse reo_wnd to 0. */
    state.reordering_seen = 0U;
    tcp_shift_rack_note_sacked_segments(&state, 3U);
    CHECK(tcp_shift_rack_reo_wnd(&state, 0U) == 0U);

    /* Once reordering is observed, start at min_RTT/4 and cap at SRTT. */
    state.reordering_seen = 1U;
    state.reo_wnd_mult = 8U;
    CHECK(tcp_shift_rack_reo_wnd(&state, 0U) == UINT64_C(120000000));
    return 0;
}

static int test_dsack_adaptation(void)
{
    struct tcp_shift_rack_tlp_state state;
    uint32_t i;

    tcp_shift_rack_tlp_init(&state);
    tcp_shift_rack_set_rtt_estimates(
        &state, UINT64_C(80000000), UINT64_C(100000000));
    state.reordering_seen = 1U;

    tcp_shift_rack_note_dsack(&state, 10000U);
    CHECK(state.reo_wnd_mult == 2U);
    CHECK(state.reo_wnd_persist ==
          TCP_SHIFT_RACK_REO_WND_PERSIST_RECOVERIES);
    CHECK(tcp_shift_rack_reo_wnd(&state, 0U) == UINT64_C(40000000));

    /* Same DSACK round must not increment twice. */
    tcp_shift_rack_note_dsack(&state, 12000U);
    CHECK(state.reo_wnd_mult == 2U);

    tcp_shift_rack_note_recovery_exit(&state, 10000U);
    CHECK(state.dsack_round_active == 0U);
    for (i = 1U; i < TCP_SHIFT_RACK_REO_WND_PERSIST_RECOVERIES; i++) {
        tcp_shift_rack_note_recovery_exit(&state, 10000U);
    }
    CHECK(state.reo_wnd_mult == 1U);
    CHECK(state.reo_wnd_persist == 0U);
    return 0;
}

static int test_pto_and_probe_state(void)
{
    struct tcp_shift_rack_tlp_state state;
    enum tcp_shift_tlp_ack_result result;

    tcp_shift_rack_tlp_init(&state);
    tcp_shift_rack_set_rtt_estimates(
        &state, UINT64_C(80000000), UINT64_C(100000000));

    CHECK(tcp_shift_tlp_calc_pto_ns(
              &state, 0U, UINT64_C(1000000000), 2U,
              UINT64_C(40000000)) == UINT64_C(200000000));
    CHECK(tcp_shift_tlp_calc_pto_ns(
              &state, 0U, UINT64_C(1000000000), 1U,
              UINT64_C(40000000)) == UINT64_C(240000000));
    CHECK(tcp_shift_tlp_calc_pto_ns(
              &state, 0U, UINT64_C(150000000), 2U,
              0U) == UINT64_C(150000000));

    CHECK(tcp_shift_tlp_probe_allowed(&state) == 1);
    tcp_shift_tlp_note_probe_sent(&state, 5000U, 1U);
    CHECK(tcp_shift_tlp_probe_allowed(&state) == 0);

    /* ACK exactly covering a retransmitted probe is ambiguous. */
    result = tcp_shift_tlp_process_ack(&state, 5000U, 0U, 0U);
    CHECK(result == TCP_SHIFT_TLP_ACK_NONE);

    /* An ACK advancing beyond the probe proves the repaired-loss case. */
    result = tcp_shift_tlp_process_ack(&state, 6000U, 0U, 0U);
    CHECK(result == TCP_SHIFT_TLP_ACK_LOSS_REPAIRED);
    CHECK(state.tlp_end_seq == 0U);

    tcp_shift_tlp_note_rtt_sample(&state);
    CHECK(tcp_shift_tlp_probe_allowed(&state) == 1);

    tcp_shift_tlp_note_probe_sent(&state, 7000U, 1U);
    result = tcp_shift_tlp_process_ack(&state, 7000U, 1U, 0U);
    CHECK(result == TCP_SHIFT_TLP_ACK_CLEARED);
    return 0;
}

static int test_retransmission_ambiguity(void)
{
    struct tcp_shift_rack_tlp_state state;
    struct tcp_shift_rack_segment retrans = {
        .xmit_ts_ns = UINT64_C(200000000),
        .seq_start = 3000U,
        .end_seq = 4000U,
        .retransmitted = 1U,
    };

    tcp_shift_rack_tlp_init(&state);
    tcp_shift_rack_set_rtt_estimates(
        &state, UINT64_C(100000000), UINT64_C(120000000));

    /* Apparent RTT below min_RTT cannot safely advance RACK.segment. */
    CHECK(tcp_shift_rack_note_delivered(
              &state, &retrans, UINT64_C(250000000)) == 0);
    CHECK(state.rack_xmit_ts_ns == 0U);

    CHECK(tcp_shift_rack_note_delivered(
              &state, &retrans, UINT64_C(320000000)) == 1);
    CHECK(state.rack_xmit_ts_ns == retrans.xmit_ts_ns);
    return 0;
}

int main(void)
{
    CHECK(test_sent_after_and_rack_update() == 0);
    CHECK(test_reordering_window_and_loss_deadline() == 0);
    CHECK(test_dsack_adaptation() == 0);
    CHECK(test_pto_and_probe_state() == 0);
    CHECK(test_retransmission_ambiguity() == 0);

    printf("rack_tlp_contract=ok rfc=8985 "
           "reo=min_rtt/4 dsack_persist=16 pto=2*srtt "
           "single_probe=1 rto_fallback=preserved\n");
    return 0;
}
