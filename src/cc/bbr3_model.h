#ifndef TCP_SHIFT_CC_BBR3_MODEL_H
#define TCP_SHIFT_CC_BBR3_MODEL_H

#include <stdint.h>
#include "cc/cc.h"

/*
 * Independent BBRv3 model foundation, based on
 * draft-ietf-ccwg-bbr-06, sections 2.11, 5.3 and 5.5.
 *
 * This is not a complete, selectable congestion controller. In particular
 * inflight_longterm, loss/ECN response, ACK aggregation, ProbeRTT scheduling,
 * and the ProbeBW phase transition machine are not yet implemented.
 * Do not route production traffic through this model alone.
 */
#define TCP_SHIFT_BBR3_MAX_BW_CYCLES 2U
#define TCP_SHIFT_BBR3_MIN_RTT_WINDOW_NS UINT64_C(10000000000)
#define TCP_SHIFT_BBR3_PROBE_RTT_INTERVAL_NS UINT64_C(5000000000)
#define TCP_SHIFT_BBR3_FULL_BW_ROUNDS 3U

enum tcp_shift_bbr3_phase {
    TCP_SHIFT_BBR3_STARTUP = 0,
    TCP_SHIFT_BBR3_DRAIN,
    TCP_SHIFT_BBR3_PROBE_BW_DOWN,
    TCP_SHIFT_BBR3_PROBE_BW_CRUISE,
    TCP_SHIFT_BBR3_PROBE_BW_REFILL,
    TCP_SHIFT_BBR3_PROBE_BW_UP,
    TCP_SHIFT_BBR3_PROBE_RTT
};

struct tcp_shift_bbr3_gain {
    uint32_t numerator;
    uint32_t denominator;
};

struct tcp_shift_bbr3_model {
    uint64_t max_bw_bytes_per_sec;
    uint64_t max_bw_filter[TCP_SHIFT_BBR3_MAX_BW_CYCLES];
    uint64_t min_rtt_ns;
    uint64_t min_rtt_stamp_ns;
    uint64_t probe_rtt_min_delay_ns;
    uint64_t probe_rtt_min_stamp_ns;
    uint64_t last_update_ns;
    uint64_t next_round_delivered;
    uint64_t full_bw_bytes_per_sec;
    uint32_t round_count;
    uint32_t cycle_count;
    uint32_t full_bw_count;
    uint8_t has_bw;
    uint8_t has_min_rtt;
    uint8_t has_probe_rtt_min;
    uint8_t has_update_time;
    uint8_t round_start;
    uint8_t full_bw_reached;
    uint8_t probe_rtt_expired;
    enum tcp_shift_bbr3_phase phase;
};

void tcp_shift_bbr3_model_init(struct tcp_shift_bbr3_model *model);

/* This performs only independent rate/RTT/full-pipe estimation; it does not
 * change phase or drive cwnd/recovery. Time must be monotonic and nonzero. */
int tcp_shift_bbr3_model_on_ack(
    struct tcp_shift_bbr3_model *model,
    const struct tcp_shift_cc_rate_sample *sample,
    uint64_t now_ns);

/* The future ProbeBW machine calls this at the end of each complete probe
 * cycle, not on every packet-timed round as BBRv1 does. */
int tcp_shift_bbr3_model_advance_bw_cycle(
    struct tcp_shift_bbr3_model *model);

struct tcp_shift_bbr3_gain tcp_shift_bbr3_pacing_gain(
    enum tcp_shift_bbr3_phase phase);
struct tcp_shift_bbr3_gain tcp_shift_bbr3_cwnd_gain(
    enum tcp_shift_bbr3_phase phase);

/* Overflow-safe, saturating fixed rational scaling, floor-rounded. */
uint64_t tcp_shift_bbr3_scale(uint64_t value,
                              struct tcp_shift_bbr3_gain gain);

/* Long-term limit is UINT64_MAX when unset. Leave at least 15% headroom,
 * minimum one MSS; never return less than the 4-MSS pipe floor. */
uint64_t tcp_shift_bbr3_inflight_with_headroom(
    uint64_t inflight_longterm_bytes,
    uint32_t mss_bytes);

#endif
