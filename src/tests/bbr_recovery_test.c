#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc/bbr_recovery.h"

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "bbr-recovery: check failed at %s:%d: %s\n",  \
                    __FILE__, __LINE__, #expr);                             \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static int check_packet_conservation(void)
{
    struct tcp_shift_bbr_recovery_state state;
    uint32_t cwnd;
    unsigned owns_cwnd;

    memset(&state, 0, sizeof(state));
    tcp_shift_bbr_recovery_init(&state);
    CHECK(state.in_recovery == 0U);
    CHECK(state.packet_conservation == 0U);

    CHECK(tcp_shift_bbr_recovery_enter(
              &state,
              100000U,
              60000U,
              1460U,
              1460U,
              1460U,
              1000000U,
              &cwnd) == 0);
    CHECK(state.in_recovery == 1U);
    CHECK(state.packet_conservation == 1U);
    CHECK(state.prior_cwnd_bytes == 100000U);
    CHECK(cwnd == 61460U);

    /* During the first packet-timed recovery round, one ACK may release at
     * most the corresponding inflight+ACK credit. Loss accounting happens
     * first; conservation then raises cwnd only as needed to preserve flight. */
    CHECK(tcp_shift_bbr_recovery_on_ack(
              &state,
              cwnd,
              59000U,
              2920U,
              1460U,
              1460U,
              1000000U,
              0U,
              &cwnd,
              &owns_cwnd) == 0);
    CHECK(owns_cwnd == 1U);
    CHECK(state.packet_conservation == 1U);
    CHECK(cwnd == 61920U);

    /* Large loss still floors at one MSS before conservation replenishes only
     * the bytes justified by current inflight plus this ACK. */
    CHECK(tcp_shift_bbr_recovery_on_ack(
              &state,
              cwnd,
              55000U,
              1460U,
              70000U,
              1460U,
              1000000U,
              0U,
              &cwnd,
              &owns_cwnd) == 0);
    CHECK(owns_cwnd == 1U);
    CHECK(cwnd == 56460U);

    /* A packet-timed round boundary releases packet conservation. The helper
     * still applies loss accounting, but normal BBR target-cwnd policy owns
     * growth from this ACK onward. */
    CHECK(tcp_shift_bbr_recovery_on_ack(
              &state,
              56460U,
              54000U,
              1460U,
              1460U,
              1460U,
              1000000U,
              1U,
              &cwnd,
              &owns_cwnd) == 0);
    CHECK(owns_cwnd == 0U);
    CHECK(state.packet_conservation == 0U);
    CHECK(cwnd == 55000U);

    /* Simulate normal BBR policy growing cwnd after the first recovery round.
     * Exit restores the last known good pre-recovery cwnd before the caller's
     * current BDP target applies its final cap. */
    CHECK(tcp_shift_bbr_recovery_exit(
              &state, 80000U, 1000000U, &cwnd) == 0);
    CHECK(cwnd == 100000U);
    CHECK(state.in_recovery == 0U);
    CHECK(state.packet_conservation == 0U);
    return 0;
}

static int check_limits_and_invalid_inputs(void)
{
    struct tcp_shift_bbr_recovery_state state;
    uint32_t cwnd;
    unsigned owns_cwnd;

    tcp_shift_bbr_recovery_init(&state);
    CHECK(tcp_shift_bbr_recovery_enter(
              &state, 14600U, 999000U, 10000U, 0U,
              1460U, 1000000U, &cwnd) == 0);
    CHECK(cwnd == 1000000U);
    CHECK(tcp_shift_bbr_recovery_enter(
              &state, 14600U, 1000U, 1460U, 0U,
              1460U, 1000000U, &cwnd) < 0);

    CHECK(tcp_shift_bbr_recovery_on_ack(
              &state, 1000000U, 999000U, 10000U, 0U,
              1460U, 1000000U, 0U, &cwnd, &owns_cwnd) == 0);
    CHECK(cwnd == 1000000U);
    CHECK(owns_cwnd == 1U);

    CHECK(tcp_shift_bbr_recovery_exit(
              &state, 1000000U, 1000000U, &cwnd) == 0);
    CHECK(cwnd == 1000000U);
    CHECK(tcp_shift_bbr_recovery_exit(
              &state, 1000000U, 1000000U, &cwnd) < 0);

    tcp_shift_bbr_recovery_init(&state);
    CHECK(tcp_shift_bbr_recovery_enter(
              &state, 1000U, 0U, 0U, 0U,
              1460U, 1000000U, &cwnd) < 0);
    CHECK(tcp_shift_bbr_recovery_on_ack(
              &state, 14600U, 1000U, 1460U, 0U,
              1460U, 1000000U, 0U, &cwnd, &owns_cwnd) < 0);
    return 0;
}

int main(void)
{
    CHECK(TCP_SHIFT_BBR_SNDBUF_EXPAND_NUM == 3U);
    CHECK(TCP_SHIFT_BBR_SNDBUF_EXPAND_DEN == 1U);
    CHECK(check_packet_conservation() == 0);
    CHECK(check_limits_and_invalid_inputs() == 0);

    printf("bbr_recovery=ok packet_conservation=first_round "
           "restore=prior_cwnd sndbuf_expand=3x units=bytes\n");
    return 0;
}
