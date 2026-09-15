#include "cc/bbr.h"

#include <stddef.h>

static uint32_t tcp_shift_bbr_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t tcp_shift_bbr_drain_target_bytes(
    const struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_transport *transport)
{
    uint64_t target;
    uint64_t min_cwnd;

    target = tcp_shift_bbr_bdp_bytes(model->max_bw_bytes_per_sec,
                                     model->min_rtt_ns);
    min_cwnd = (uint64_t)transport->mss_bytes * TCP_SHIFT_BBR_MIN_CWND_PACKETS;
    if (target < min_cwnd) {
        target = min_cwnd;
    }
    if (target > transport->cwnd_limit_bytes) {
        target = transport->cwnd_limit_bytes;
    }
    return (uint32_t)target;
}

int tcp_shift_bbr_model_update_mode(
    struct tcp_shift_bbr_model *model,
    const struct tcp_shift_cc_transport *transport)
{
    uint32_t target;

    if (model == NULL || transport == NULL || transport->mss_bytes == 0U ||
        transport->cwnd_limit_bytes == 0U) {
        return -1;
    }

    if (model->mode == TCP_SHIFT_BBR_MODE_STARTUP &&
        model->full_bw_reached != 0U) {
        model->mode = TCP_SHIFT_BBR_MODE_DRAIN;
    }

    if (model->mode != TCP_SHIFT_BBR_MODE_DRAIN) {
        return 0;
    }

    /* Do not invent a drain target before both bandwidth and propagation RTT
     * have been observed. Startup may already have latched full_bw from rate
     * samples before a valid RTT sample exists; stay in DRAIN until the model
     * can express the 1*BDP target safely. */
    if (model->max_bw_bytes_per_sec == 0U || model->has_min_rtt == 0U ||
        model->min_rtt_ns == 0U) {
        return 0;
    }

    target = tcp_shift_bbr_drain_target_bytes(model, transport);
    if (transport->inflight_bytes <= target) {
        model->mode = TCP_SHIFT_BBR_MODE_PROBE_BW;
        model->probe_bw_cycle_index = 0U;
    }

    return 0;
}

int tcp_shift_bbr_model_gains(const struct tcp_shift_bbr_model *model,
                              struct tcp_shift_bbr_gains *gains)
{
    static const uint32_t probe_bw_pacing_gain[
        TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN] = {
        320U, 192U, 256U, 256U, 256U, 256U, 256U, 256U,
    };

    if (model == NULL || gains == NULL) {
        return -1;
    }

    switch (model->mode) {
    case TCP_SHIFT_BBR_MODE_STARTUP:
        gains->pacing_gain_num = TCP_SHIFT_BBR_STARTUP_GAIN_NUM;
        gains->cwnd_gain_num = TCP_SHIFT_BBR_STARTUP_GAIN_NUM;
        return 0;
    case TCP_SHIFT_BBR_MODE_DRAIN:
        gains->pacing_gain_num = TCP_SHIFT_BBR_DRAIN_GAIN_NUM;
        gains->cwnd_gain_num = TCP_SHIFT_BBR_STARTUP_GAIN_NUM;
        return 0;
    case TCP_SHIFT_BBR_MODE_PROBE_BW:
        if (model->probe_bw_cycle_index >= TCP_SHIFT_BBR_PROBE_BW_CYCLE_LEN) {
            return -1;
        }
        gains->pacing_gain_num =
            probe_bw_pacing_gain[model->probe_bw_cycle_index];
        gains->cwnd_gain_num = TCP_SHIFT_BBR_PROBE_BW_CWND_GAIN_NUM;
        return 0;
    case TCP_SHIFT_BBR_MODE_PROBE_RTT:
        gains->pacing_gain_num = TCP_SHIFT_BBR_UNIT_GAIN_NUM;
        gains->cwnd_gain_num = TCP_SHIFT_BBR_UNIT_GAIN_NUM;
        return 0;
    default:
        gains->pacing_gain_num = 0U;
        gains->cwnd_gain_num = 0U;
        return -1;
    }
}
