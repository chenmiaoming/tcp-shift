#ifndef TCP_SHIFT_CC_TRANSPORT_PACING_H
#define TCP_SHIFT_CC_TRANSPORT_PACING_H

#include <stdint.h>

#include "cc/cc.h"

#define TCP_SHIFT_TRANSPORT_PACING_SS_PERCENT 200U
#define TCP_SHIFT_TRANSPORT_PACING_CA_PERCENT 120U

/*
 * Generic loss-based TCP pacing fallback.
 *
 * Reno and CUBIC currently publish pacing_rate=0, so the transport may derive
 * a Linux-shaped rate from max(cwnd, in-flight)/SRTT. Before an RTT sample is
 * available this helper mirrors Linux generic TCP pacing: it keeps a finite,
 * deliberately very high startup rate rather than disabling pacing state.
 * A congestion controller such as BBR that publishes a nonzero pacing rate
 * owns that rate and bypasses this fallback entirely.
 */
uint64_t tcp_shift_transport_pacing_window_rate(
    const struct tcp_shift_cc_transport *transport,
    const struct tcp_shift_cc_policy *policy,
    uint64_t smoothed_rtt_ns);

/*
 * Populate policy->pacing_rate_bytes_per_sec only when the controller left it
 * at zero. rate_cap_bytes_per_sec is an optional qualification-only ceiling;
 * zero means uncapped. Controller-owned nonzero rates are never clipped.
 */
int tcp_shift_transport_pacing_apply_window_fallback(
    const struct tcp_shift_cc_transport *transport,
    uint64_t smoothed_rtt_ns,
    uint64_t rate_cap_bytes_per_sec,
    struct tcp_shift_cc_policy *policy);

#endif /* TCP_SHIFT_CC_TRANSPORT_PACING_H */
