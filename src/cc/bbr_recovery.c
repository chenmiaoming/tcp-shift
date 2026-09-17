#include "cc/bbr_recovery.h"

#include <stddef.h>

static int tcp_shift_bbr_recovery_valid_window(uint32_t current_cwnd_bytes,
                                                uint32_t mss_bytes,
                                                uint32_t cwnd_limit_bytes)
{
    return mss_bytes != 0U && cwnd_limit_bytes >= mss_bytes &&
           current_cwnd_bytes >= mss_bytes &&
           current_cwnd_bytes <= cwnd_limit_bytes;
}

static uint32_t tcp_shift_bbr_recovery_floor_sub(uint32_t value,
                                                  uint32_t subtract,
                                                  uint32_t floor)
{
    if (value <= subtract || value - subtract < floor) {
        return floor;
    }
    return value - subtract;
}

static uint32_t tcp_shift_bbr_recovery_sat_add_limit(uint32_t a,
                                                      uint32_t b,
                                                      uint32_t limit)
{
    if (a >= limit || b > limit - a) {
        return limit;
    }
    return a + b;
}

void tcp_shift_bbr_recovery_init(
    struct tcp_shift_bbr_recovery_state *state)
{
    if (state == NULL) {
        return;
    }
    state->prior_cwnd_bytes = 0U;
    state->in_recovery = 0U;
    state->packet_conservation = 0U;
}

int tcp_shift_bbr_recovery_enter(
    struct tcp_shift_bbr_recovery_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t inflight_bytes,
    uint32_t acked_bytes,
    uint32_t lost_bytes,
    uint32_t mss_bytes,
    uint32_t cwnd_limit_bytes,
    uint32_t *cwnd_bytes)
{
    uint32_t conservation_cwnd;

    if (state == NULL || cwnd_bytes == NULL || state->in_recovery != 0U ||
        !tcp_shift_bbr_recovery_valid_window(current_cwnd_bytes, mss_bytes,
                                              cwnd_limit_bytes)) {
        return -1;
    }

    state->prior_cwnd_bytes = current_cwnd_bytes;
    state->in_recovery = 1U;
    state->packet_conservation = 1U;

    /* Linux first deducts newly lost packets and then, on the Recovery state
     * transition, deliberately replaces cwnd with inflight + acked. Preserve
     * the loss subtraction here as an explicit validity/ordering operation even
     * though the transition assignment supersedes its numeric result. */
    (void)tcp_shift_bbr_recovery_floor_sub(current_cwnd_bytes, lost_bytes,
                                            mss_bytes);
    conservation_cwnd = tcp_shift_bbr_recovery_sat_add_limit(
        inflight_bytes, acked_bytes, cwnd_limit_bytes);
    if (conservation_cwnd < mss_bytes) {
        conservation_cwnd = mss_bytes;
    }

    *cwnd_bytes = conservation_cwnd;
    return 0;
}

int tcp_shift_bbr_recovery_on_ack(
    struct tcp_shift_bbr_recovery_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t inflight_bytes,
    uint32_t acked_bytes,
    uint32_t lost_bytes,
    uint32_t mss_bytes,
    uint32_t cwnd_limit_bytes,
    unsigned round_start,
    uint32_t *cwnd_bytes,
    unsigned *owns_cwnd)
{
    uint32_t cwnd;
    uint32_t conservation_cwnd;

    if (state == NULL || cwnd_bytes == NULL || owns_cwnd == NULL ||
        state->in_recovery == 0U ||
        !tcp_shift_bbr_recovery_valid_window(current_cwnd_bytes, mss_bytes,
                                              cwnd_limit_bytes)) {
        return -1;
    }

    cwnd = tcp_shift_bbr_recovery_floor_sub(current_cwnd_bytes, lost_bytes,
                                             mss_bytes);

    /* Linux clears packet_conservation when its packet-timed round detector
     * starts the next RTT. From that ACK onward normal BBR target-cwnd policy
     * resumes, using the loss-adjusted cwnd as its starting point. */
    if (round_start != 0U) {
        state->packet_conservation = 0U;
    }

    if (state->packet_conservation != 0U) {
        conservation_cwnd = tcp_shift_bbr_recovery_sat_add_limit(
            inflight_bytes, acked_bytes, cwnd_limit_bytes);
        if (conservation_cwnd < mss_bytes) {
            conservation_cwnd = mss_bytes;
        }
        if (cwnd < conservation_cwnd) {
            cwnd = conservation_cwnd;
        }
        *owns_cwnd = 1U;
    } else {
        *owns_cwnd = 0U;
    }

    if (cwnd > cwnd_limit_bytes) {
        cwnd = cwnd_limit_bytes;
    }
    *cwnd_bytes = cwnd;
    return 0;
}

int tcp_shift_bbr_recovery_exit(
    struct tcp_shift_bbr_recovery_state *state,
    uint32_t current_cwnd_bytes,
    uint32_t cwnd_limit_bytes,
    uint32_t *cwnd_bytes)
{
    uint32_t restored;

    if (state == NULL || cwnd_bytes == NULL || state->in_recovery == 0U ||
        current_cwnd_bytes == 0U || cwnd_limit_bytes == 0U ||
        current_cwnd_bytes > cwnd_limit_bytes ||
        state->prior_cwnd_bytes == 0U) {
        return -1;
    }

    restored = current_cwnd_bytes > state->prior_cwnd_bytes
                   ? current_cwnd_bytes
                   : state->prior_cwnd_bytes;
    if (restored > cwnd_limit_bytes) {
        restored = cwnd_limit_bytes;
    }

    state->in_recovery = 0U;
    state->packet_conservation = 0U;
    *cwnd_bytes = restored;
    return 0;
}
